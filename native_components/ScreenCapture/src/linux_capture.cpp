#include "linux_capture.h"
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/res.h>
#include <xcb/composite.h>
#include <xcb/render.h>
#include <xcb/shape.h>
#include <poll.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace screen_capture::linux_capture {
namespace {
using Clock = std::chrono::steady_clock;
struct Free { void operator()(void* p) const { std::free(p); } };
template<class T> using Reply = std::unique_ptr<T, Free>;
[[noreturn]] void Fail(const std::string& reason) { throw std::runtime_error(reason); }
struct Disconnect { void operator()(xcb_connection_t* c) const { if (c) xcb_disconnect(c); } };

class Session {
public:
    std::unique_ptr<xcb_connection_t, Disconnect> connection;
    xcb_screen_t* screen = nullptr;
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(2);
    Session() {
        int index = 0;
        connection.reset(xcb_connect(nullptr, &index));
        if (!connection || xcb_connection_has_error(c())) Fail("ERROR:x11:display_unavailable");
        auto screens = xcb_setup_roots_iterator(xcb_get_setup(c()));
        while (index-- > 0 && screens.rem) xcb_screen_next(&screens);
        if (!screens.rem) Fail("ERROR:x11:screen_unavailable");
        screen = screens.data;
    }
    xcb_connection_t* c() const { return connection.get(); }
    void Budget() const {
        if (Clock::now() >= deadline) Fail("RETRY:x11:timeout");
    }
    template<class T, class Cookie> Reply<T> Get(Cookie cookie) {
        xcb_flush(c());
        for (;;) {
            Budget();
            void* reply = nullptr;
            xcb_generic_error_t* error = nullptr;
            if (xcb_poll_for_reply(c(), cookie.sequence, &reply, &error)) {
                Reply<T> result(static_cast<T*>(reply));
                Reply<xcb_generic_error_t> ownedError(error);
                if (error) Fail("RETRY:x11:protocol_" + std::to_string(error->error_code));
                if (!result) Fail("RETRY:x11:empty_reply");
                return result;
            }
            if (xcb_connection_has_error(c())) Fail("ERROR:x11:connection_lost");
            pollfd fd{xcb_get_file_descriptor(c()), POLLIN, 0};
            // Short waits let Budget bound a server grab even if the server stops replying.
            if (poll(&fd, 1, 10) < 0) Fail("RETRY:x11:poll_failed");
        }
    }
    uint8_t ErrorCode(xcb_void_cookie_t cookie) {
        // Complete a later reply first, so request_check does not wait indefinitely.
        Get<xcb_get_input_focus_reply_t>(xcb_get_input_focus(c()));
        Reply<xcb_generic_error_t> error(xcb_request_check(c(), cookie));
        return error ? error->error_code : 0;
    }
    void Check(xcb_void_cookie_t cookie) {
        const auto error = ErrorCode(cookie);
        if (error) Fail("RETRY:x11:protocol_" + std::to_string(error));
    }
    xcb_atom_t Atom(const char* name) {
        return Get<xcb_intern_atom_reply_t>(xcb_intern_atom(c(), false, uint16_t(std::strlen(name)), name))->atom;
    }
    uint32_t Property(xcb_window_t window, xcb_atom_t atom, xcb_atom_t type) {
        auto reply = Get<xcb_get_property_reply_t>(xcb_get_property(c(), false, window, atom, type, 0, 1));
        uint32_t value = 0;
        if (reply->type == type && reply->format == 32 && xcb_get_property_value_length(reply.get()) >= 4)
            std::memcpy(&value, xcb_get_property_value(reply.get()), 4);
        return value;
    }
    std::vector<xcb_window_t> Children(xcb_window_t parent) {
        auto reply = Get<xcb_query_tree_reply_t>(xcb_query_tree(c(), parent));
        auto children = xcb_query_tree_children(reply.get());
        return {children, children + xcb_query_tree_children_length(reply.get())};
    }
};

// All requests go through a private connection. Disconnect releases a grab on every
// exception path. The grab ends before CPU image conversion, scaling or PNG encoding.
struct ServerGrab {
    Session& session;
    explicit ServerGrab(Session& s) : session(s) {
        session.Check(xcb_grab_server_checked(session.c()));
        session.deadline = Clock::now() + std::chrono::milliseconds(250);
    }
    ~ServerGrab() {
        xcb_ungrab_server(session.c());
        xcb_flush(session.c());
    }
};

struct Client { uint32_t base, mask, pid; };
std::vector<Client> Clients(Session& s) {
    const auto* extension = xcb_get_extension_data(s.c(), &xcb_res_id);
    if (!extension || !extension->present) Fail("ERROR:x11:xres_required");
    auto version = s.Get<xcb_res_query_version_reply_t>(xcb_res_query_version(s.c(), 1, 2));
    if (version->server_major < 1 || (version->server_major == 1 && version->server_minor < 2))
        Fail("ERROR:x11:xres_required");
    xcb_res_client_id_spec_t spec{0, XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
    auto ids = s.Get<xcb_res_query_client_ids_reply_t>(xcb_res_query_client_ids(s.c(), 1, &spec));
    std::unordered_map<uint32_t, uint32_t> pids;
    for (auto it = xcb_res_query_client_ids_ids_iterator(ids.get()); it.rem; xcb_res_client_id_value_next(&it)) {
        if ((it.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) &&
            xcb_res_client_id_value_value_length(it.data) == 1)
            pids[it.data->spec.client] = *xcb_res_client_id_value_value(it.data);
    }
    auto clients = s.Get<xcb_res_query_clients_reply_t>(xcb_res_query_clients(s.c()));
    std::vector<Client> result;
    for (auto it = xcb_res_query_clients_clients_iterator(clients.get()); it.rem; xcb_res_client_next(&it))
        result.push_back({it.data->resource_base, it.data->resource_mask, pids[it.data->resource_base]});
    return result;
}
uint32_t Owner(const std::vector<Client>& clients, uint32_t id) {
    for (const auto& c : clients) if ((id & ~c.mask) == c.base) return c.pid;
    return 0;
}
struct Window {
    uint32_t id;
    raster::Rect rect;
    int group;
    bool main;
    uint32_t opacity;
};
struct Group {
    raster::Rect rect;
    uint32_t id = 0, visual = 0, opacity = UINT32_MAX;
    int border = 0;
    int ownClients = 0;
    bool foreign = false;
};
struct Tree {
    Session& s;
    const std::vector<Client>& clients;
    uint32_t pid;
    xcb_atom_t wmState, transient, type, normal, opacity;
    std::vector<Window> windows;
    std::vector<Group> groups;
    size_t visited = 0;

    uint32_t Opacity(uint32_t id) {
        auto reply = s.Get<xcb_get_property_reply_t>(xcb_get_property(s.c(), false, id, opacity, XCB_ATOM_CARDINAL, 0, 1));
        uint32_t alpha = UINT32_MAX;
        if (reply->format == 32 && reply->value_len == 1)
            std::memcpy(&alpha, xcb_get_property_value(reply.get()), sizeof(alpha));
        return alpha;
    }

    void Visit(uint32_t id, int group, int level) {
        if (++visited > 2048 || level > 16) Fail("RETRY:x11:tree_limit");
        auto attributes = s.Get<xcb_get_window_attributes_reply_t>(xcb_get_window_attributes(s.c(), id));
        if (attributes->map_state != XCB_MAP_STATE_VIEWABLE || attributes->_class == XCB_WINDOW_CLASS_INPUT_ONLY) return;
        const bool own = Owner(clients, id) == pid;
        if (own || level == 0) {
            auto geometry = s.Get<xcb_get_geometry_reply_t>(xcb_get_geometry(s.c(), id));
            auto position = s.Get<xcb_translate_coordinates_reply_t>(xcb_translate_coordinates(s.c(), id, s.screen->root, 0, 0));
            if (!position->same_screen) Fail("RETRY:x11:screen_changed");
            const raster::Rect rect{position->dst_x, position->dst_y, geometry->width, geometry->height};
            if (level == 0) {
                const int border = geometry->border_width;
                groups[group].rect = {rect.x - border, rect.y - border, rect.width + 2 * border, rect.height + 2 * border};
                groups[group].id = id;
                groups[group].visual = attributes->visual;
                groups[group].border = border;
                groups[group].opacity = Opacity(id);
            }
            if (own) {
                const auto windowType = s.Property(id, type, XCB_ATOM_ATOM);
                const bool main = !attributes->override_redirect && !s.Property(id, transient, XCB_ATOM_WINDOW) &&
                    (windowType == 0 || windowType == normal);
                windows.push_back({id, rect, group, main, Opacity(id)});
                ++groups[group].ownClients;
                return; // Its children are already part of the client surface.
            }
        }
        // A managed foreign client is a leaf. A WM frame is not: descend to its client.
        if (s.Property(id, wmState, wmState) != 0) { groups[group].foreign = true; return; }
        const auto children = s.Children(id);
        for (auto child : children) Visit(child, group, level + 1);
    }
};

struct PixelFormat {
    int depth = 0, bits = 0, pad = 0;
    uint32_t red = 0, green = 0, blue = 0, alpha = 0;
};
std::unordered_map<uint32_t, PixelFormat> Formats(Session& s) {
    const auto* composite = xcb_get_extension_data(s.c(), &xcb_composite_id);
    if (!composite || !composite->present) Fail("ERROR:x11:composite_required");
    auto version = s.Get<xcb_composite_query_version_reply_t>(xcb_composite_query_version(s.c(), 0, 4));
    if (version->major_version == 0 && version->minor_version < 2) Fail("ERROR:x11:composite_required");
    const auto* render = xcb_get_extension_data(s.c(), &xcb_render_id);
    if (!render || !render->present) Fail("ERROR:x11:render_required");
    auto reply = s.Get<xcb_render_query_pict_formats_reply_t>(xcb_render_query_pict_formats(s.c()));
    std::unordered_map<uint32_t, PixelFormat> byFormat, byVisual;
    for (auto it = xcb_render_query_pict_formats_formats_iterator(reply.get()); it.rem; xcb_render_pictforminfo_next(&it)) {
        const auto& f = *it.data;
        if (f.type != XCB_RENDER_PICT_TYPE_DIRECT || !f.direct.red_mask || !f.direct.green_mask || !f.direct.blue_mask) continue;
        PixelFormat format;
        format.depth = f.depth;
        format.red = uint32_t(f.direct.red_mask) << f.direct.red_shift;
        format.green = uint32_t(f.direct.green_mask) << f.direct.green_shift;
        format.blue = uint32_t(f.direct.blue_mask) << f.direct.blue_shift;
        format.alpha = uint32_t(f.direct.alpha_mask) << f.direct.alpha_shift;
        for (auto wire = xcb_setup_pixmap_formats_iterator(xcb_get_setup(s.c())); wire.rem; xcb_format_next(&wire)) {
            if (wire.data->depth == f.depth) { format.bits = wire.data->bits_per_pixel; format.pad = wire.data->scanline_pad; break; }
        }
        byFormat[f.id] = format;
    }
    for (auto screen = xcb_render_query_pict_formats_screens_iterator(reply.get()); screen.rem; xcb_render_pictscreen_next(&screen))
        for (auto depth = xcb_render_pictscreen_depths_iterator(screen.data); depth.rem; xcb_render_pictdepth_next(&depth))
            for (auto visual = xcb_render_pictdepth_visuals_iterator(depth.data); visual.rem; xcb_render_pictvisual_next(&visual)) {
                auto format = byFormat.find(visual.data->format);
                if (format != byFormat.end()) byVisual[visual.data->visual] = format->second;
            }
    return byVisual;
}

struct RawLayer {
    Reply<xcb_get_image_reply_t> reply;
    PixelFormat format;
    raster::Rect screen;
    uint32_t opacity = UINT32_MAX;
    std::vector<raster::Rect> shape;
    bool shaped = false;
};

RawLayer ReadPixmap(Session& s, const Group& group, const raster::Rect& screen,
                    const std::unordered_map<uint32_t, PixelFormat>& formats,
                    bool shapeAvailable, uint64_t& pixels) {
    if (!raster::Inside(screen, group.rect)) Fail("ERROR:x11:unsupported_hierarchy");
    pixels += uint64_t(screen.width) * screen.height;
    if (pixels > 2 * raster::kMaxPixels) Fail("ERROR:x11:image_too_large");
    const int x = screen.x - group.rect.x, y = screen.y - group.rect.y;
    if (x > INT16_MAX || y > INT16_MAX) Fail("ERROR:x11:image_too_large");
    auto format = formats.find(group.visual);
    if (format == formats.end()) Fail("ERROR:x11:unsupported_visual");
    const uint32_t pixmap = xcb_generate_id(s.c());
    const auto error = s.ErrorCode(xcb_composite_name_window_pixmap_checked(s.c(), group.id, pixmap));
    if (error == XCB_MATCH) Fail("ERROR:x11:window_not_redirected");
    if (error) Fail("RETRY:x11:protocol_" + std::to_string(error));
    // Naming only references an existing buffer. We never redirect or repaint another
    // client's window here: late redirection can expose an uninitialised hidden area.
    struct Pixmap {
        Session& s; uint32_t id;
        ~Pixmap() { xcb_free_pixmap(s.c(), id); }
    } resource{s, pixmap};
    auto geometry = s.Get<xcb_get_geometry_reply_t>(xcb_get_geometry(s.c(), pixmap));
    if (geometry->width != group.rect.width || geometry->height != group.rect.height || geometry->depth != format->second.depth)
        Fail("RETRY:x11:pixmap_changed");
    RawLayer result;
    result.screen = screen;
    result.format = format->second;
    result.opacity = group.opacity;
    if (shapeAvailable) {
        auto shape = s.Get<xcb_shape_get_rectangles_reply_t>(xcb_shape_get_rectangles(s.c(), group.id, XCB_SHAPE_SK_BOUNDING));
        result.shaped = true;
        const auto* rectangles = xcb_shape_get_rectangles_rectangles(shape.get());
        const int count = xcb_shape_get_rectangles_rectangles_length(shape.get());
        if (count > 8192) Fail("ERROR:x11:shape_too_complex");
        for (int i = 0; i < count; ++i)
            result.shape.push_back({rectangles[i].x + group.border - x, rectangles[i].y + group.border - y,
                rectangles[i].width, rectangles[i].height});
    }
    result.reply = s.Get<xcb_get_image_reply_t>(xcb_get_image(s.c(), XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap,
        int16_t(x), int16_t(y), uint16_t(screen.width), uint16_t(screen.height), UINT32_MAX));
    return result;
}

raster::Image DecodeLayer(const RawLayer& layer, bool littleEndian) {
    const auto& f = layer.format;
    auto image = raster::Decode(xcb_get_image_data(layer.reply.get()), size_t(xcb_get_image_data_length(layer.reply.get())),
        layer.screen.width, layer.screen.height, f.bits, f.pad, littleEndian, f.red, f.green, f.blue, f.alpha);
    if (layer.shaped) {
        // Mask in local capture coordinates, not screen coordinates; shapes can extend
        // into the border or outside the requested crop.
        std::vector<uint8_t> mask(size_t(image.width) * image.height, 0);
        for (const auto& rect : layer.shape) {
            for (int y = std::max(0, rect.y); y < std::min(image.height, rect.y + rect.height); ++y) {
                const int left = std::max(0, rect.x), right = std::min(image.width, rect.x + rect.width);
                if (left < right) std::fill(mask.begin() + size_t(y) * image.width + left, mask.begin() + size_t(y) * image.width + right, 1);
            }
        }
        for (size_t pixel = 0; pixel < mask.size(); ++pixel)
            if (!mask[pixel]) std::fill_n(image.rgba.begin() + pixel * 4, 4, 0);
    }
    return image;
}
}

Capture Read(uint32_t pid, const raster::Rect* requested) {
    Session s;
    // Load the extension before grabbing (xcb_get_extension_data may do a round trip).
    const auto* extension = xcb_get_extension_data(s.c(), &xcb_res_id);
    if (!extension || !extension->present) Fail("ERROR:x11:xres_required");
    const auto formats = Formats(s);
    const auto* shape = xcb_get_extension_data(s.c(), &xcb_shape_id);
    const bool shapeAvailable = shape && shape->present;
    std::vector<Client> clients;
    Tree tree{s, clients, pid, s.Atom("WM_STATE"), XCB_ATOM_WM_TRANSIENT_FOR,
        s.Atom("_NET_WM_WINDOW_TYPE"), s.Atom("_NET_WM_WINDOW_TYPE_NORMAL"), s.Atom("_NET_WM_WINDOW_OPACITY"), {}, {}};
    const auto activeAtom = s.Atom("_NET_ACTIVE_WINDOW");
    Capture result;
    std::vector<RawLayer> images;
    {
        ServerGrab grab(s);
        clients = Clients(s); // Ownership and window IDs must belong to the same snapshot.
        for (auto child : s.Children(s.screen->root)) {
            tree.groups.push_back({});
            tree.Visit(child, int(tree.groups.size()) - 1, 0);
        }
        const Window* anchor = nullptr;
        for (const auto& window : tree.windows) {
            if (!window.main) continue;
            const auto area = int64_t(window.rect.width) * window.rect.height;
            if (!anchor || area > int64_t(anchor->rect.width) * anchor->rect.height ||
                (area == int64_t(anchor->rect.width) * anchor->rect.height && window.id < anchor->id)) anchor = &window;
        }
        if (!anchor) Fail("RETRY:x11:main_window_unavailable");
        result.anchor = anchor->id;
        result.region = requested ? *requested : raster::Rect{0, 0, anchor->rect.width, anchor->rect.height};
        if (!raster::Inside(result.region, {0, 0, anchor->rect.width, anchor->rect.height})) Fail("ERROR:region_oob");
        result.screen = {anchor->rect.x + result.region.x, anchor->rect.y + result.region.y, result.region.width, result.region.height};
        if (uint64_t(result.screen.width) * result.screen.height > raster::kMaxPixels) Fail("ERROR:x11:image_too_large");
        if (anchor->opacity != UINT32_MAX || tree.groups[anchor->group].opacity != UINT32_MAX)
            Fail("ERROR:x11:unsupported_transparency");
        if (tree.groups[anchor->group].foreign || tree.groups[anchor->group].ownClients != 1)
            Fail("ERROR:x11:unsupported_hierarchy");
        uint64_t pixels = 0;
        images.push_back(ReadPixmap(s, tree.groups[anchor->group], result.screen, formats, shapeAvailable, pixels));
        result.foreground = s.Property(s.screen->root, activeAtom, XCB_ATOM_WINDOW);
        if (Owner(clients, result.foreground) != pid) result.foreground = 0;
        result.foregroundInFrame = result.foreground == anchor->id;
        for (const auto& window : tree.windows) {
            const auto& bounds = tree.groups[window.group].rect;
            if (window.id == anchor->id || window.group < anchor->group || !raster::Intersects(bounds, result.screen)) continue;
            // A normal WM frame contains one application client. Ambiguous wrappers
            // are rejected instead of claiming a complete layer list.
            if (window.group == anchor->group) Fail("ERROR:x11:unsupported_hierarchy");
            if (tree.groups[window.group].foreign || tree.groups[window.group].ownClients != 1)
                Fail("ERROR:x11:unsupported_hierarchy");
            if (images.size() >= 128) Fail("ERROR:x11:too_many_layers");
            const int left = std::max(bounds.x, result.screen.x), top = std::max(bounds.y, result.screen.y);
            const raster::Rect intersection{left, top,
                std::min(bounds.x + bounds.width, result.screen.x + result.screen.width) - left,
                std::min(bounds.y + bounds.height, result.screen.y + result.screen.height) - top};
            auto raw = ReadPixmap(s, tree.groups[window.group], intersection, formats, shapeAvailable, pixels);
            if (raw.opacity == UINT32_MAX) raw.opacity = window.opacity;
            images.push_back(std::move(raw));
            result.layers.push_back({window.id,
                {bounds.x - anchor->rect.x, bounds.y - anchor->rect.y, bounds.width, bounds.height},
                !raster::Inside(bounds, result.screen)});
            if (window.id == result.foreground) result.foregroundInFrame = true;
        }
    }
    const bool littleEndian = xcb_get_setup(s.c())->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;
    result.image = DecodeLayer(images.front(), littleEndian);
    for (size_t at = 3; at < result.image.rgba.size(); at += 4)
        if (result.image.rgba[at] != 255) Fail("ERROR:x11:unsupported_transparency");
    for (size_t index = 1; index < images.size(); ++index) {
        const auto& layer = images[index];
        raster::Composite(result.image, DecodeLayer(layer, littleEndian), layer.screen.x - result.screen.x,
            layer.screen.y - result.screen.y, layer.opacity);
    }
    return result;
}

std::string Metadata(const Capture& c, bool grid, const std::string& gridX, const std::string& gridY) {
    std::string json = "{\"window_rect\":{\"left\":" + std::to_string(c.screen.x) + ",\"top\":" + std::to_string(c.screen.y) +
        ",\"width\":" + std::to_string(c.screen.width) + ",\"height\":" + std::to_string(c.screen.height) +
        "},\"capture_complete\":true,\"overlay_failures\":[],\"overlays\":[";
    for (size_t index = 0; index < c.layers.size(); ++index) {
        const auto& layer = c.layers[index];
        if (index) json += ',';
        json += "{\"hwnd\":" + std::to_string(layer.id) + ",\"x\":" + std::to_string(layer.rect.x) +
            ",\"y\":" + std::to_string(layer.rect.y) + ",\"width\":" + std::to_string(layer.rect.width) +
            ",\"height\":" + std::to_string(layer.rect.height) + ",\"clipped\":" + (layer.clipped ? "true" : "false") +
            ",\"source\":\"xcomposite\",\"class_name\":\"\"}";
    }
    json += ']';
    if (c.foreground) json += ",\"foreground\":{\"hwnd\":" + std::to_string(c.foreground) +
        ",\"in_frame\":" + (c.foregroundInFrame ? "true" : "false") + "}";
    if (grid) json += ",\"grid_coords\":{\"grid_x\":[" + gridX + "],\"grid_y\":[" + gridY + "]}";
    return json + '}';
}
}
