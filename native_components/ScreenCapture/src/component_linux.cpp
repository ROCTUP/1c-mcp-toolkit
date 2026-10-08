#include "component.h"
#include "linux_capture.h"
#include "miniz.h"
#include <unistd.h>
#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>

namespace screen_capture {
bool ScreenCaptureComponent::CaptureMainWindow(int scale, bool showGrid, std::string& outB64,
    int regionX, int regionY, int regionW, int regionH, const std::string& highlightRects) {
    outB64.clear();
    try {
        // Match the BSL range, including calls made directly through the Native API.
        if (scale < 10 || scale > 200) { outB64 = "ERROR:x11:invalid_scale"; return true; }
        const bool noRegion = regionX == -1 && regionY == -1 && regionW == -1 && regionH == -1;
        if (!noRegion && (regionX < 0 || regionY < 0 || regionW <= 0 || regionH <= 0)) {
            outB64 = "ERROR:region_oob"; return true;
        }
        std::vector<raster::Rect> rectangles;
        if (!raster::ParseRects(highlightRects, rectangles)) { outB64 = "ERROR:highlight_fmt"; return true; }
        const raster::Rect region{regionX, regionY, regionW, regionH};
        auto captured = linux_capture::Read(uint32_t(getpid()), noRegion ? nullptr : &region);
        for (size_t index = 0; index < rectangles.size(); ++index) {
            if (!raster::Inside(rectangles[index], captured.region)) {
                outB64 = "ERROR:highlight_oob:" + std::to_string(index + 1); return true;
            }
        }
        if (scale != 100) {
            captured.image = raster::Resize(captured.image,
                std::max(1, captured.region.width * scale / 100), std::max(1, captured.region.height * scale / 100));
        }
        std::string gridX, gridY;
        if (showGrid) raster::Grid(captured.image, captured.region, gridX, gridY);
        for (size_t index = 0; index < rectangles.size(); ++index)
            raster::Highlight(captured.image, captured.region, rectangles[index], int(index + 1));
        size_t length = 0;
        std::unique_ptr<void, decltype(&mz_free)> png(tdefl_write_image_to_png_file_in_memory(
            captured.image.rgba.data(), captured.image.width, captured.image.height, 4, &length), mz_free);
        if (!png) { outB64 = "ERROR:x11:png_failed"; return true; }
        const auto* bytes = static_cast<const uint8_t*>(png.get());
        const std::string encoded = Base64Encode(std::vector<uint8_t>(bytes, bytes + length));
        const std::string metadata = linux_capture::Metadata(captured, showGrid, gridX, gridY);
        outB64 = "V2|" + std::to_string(metadata.size()) + '|' + metadata + '|' + encoded;
    } catch (const std::bad_alloc&) {
        outB64 = "ERROR:x11:out_of_memory";
    } catch (const std::runtime_error& error) {
        outB64 = error.what();
        if (outB64.rfind("ERROR:", 0) != 0 && outB64.rfind("RETRY:", 0) != 0) outB64 = "ERROR:x11:internal";
    } catch (...) {
        outB64 = "ERROR:x11:internal";
    }
    return true;
}
}
