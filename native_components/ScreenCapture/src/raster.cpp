#include "raster.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace screen_capture::raster {
bool Inside(const Rect& a, const Rect& b) {
    return a.width > 0 && a.height > 0 && a.x >= b.x && a.y >= b.y &&
        int64_t(a.x) + a.width <= int64_t(b.x) + b.width &&
        int64_t(a.y) + a.height <= int64_t(b.y) + b.height;
}
bool Intersects(const Rect& a, const Rect& b) {
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 &&
        int64_t(a.x) < int64_t(b.x) + b.width && int64_t(b.x) < int64_t(a.x) + a.width &&
        int64_t(a.y) < int64_t(b.y) + b.height && int64_t(b.y) < int64_t(a.y) + a.height;
}
static Image Allocate(int width, int height) {
    if (width <= 0 || height <= 0 || uint64_t(width) * height > kMaxPixels)
        throw std::runtime_error("ERROR:x11:image_too_large");
    return {width, height, std::vector<uint8_t>(size_t(width) * height * 4, 255)};
}
static uint8_t Channel(uint32_t pixel, uint32_t mask) {
    if (!mask) throw std::runtime_error("ERROR:x11:unsupported_visual");
    unsigned shift = 0;
    while (!(mask & 1)) { mask >>= 1; ++shift; }
    if ((mask & (mask + 1)) != 0) throw std::runtime_error("ERROR:x11:unsupported_visual");
    return uint8_t((uint64_t((pixel >> shift) & mask) * 255 + mask / 2) / mask);
}
Image Decode(const uint8_t* data, size_t length, int width, int height,
             int bitsPerPixel, int scanlinePad, bool littleEndian,
             uint32_t redMask, uint32_t greenMask, uint32_t blueMask, uint32_t alphaMask) {
    if ((bitsPerPixel != 16 && bitsPerPixel != 24 && bitsPerPixel != 32) ||
        (scanlinePad != 8 && scanlinePad != 16 && scanlinePad != 32) ||
        width <= 0 || height <= 0)
        throw std::runtime_error("ERROR:x11:unsupported_visual");
    const size_t stride = ((size_t(width) * bitsPerPixel + scanlinePad - 1) / scanlinePad) * (scanlinePad / 8);
    if (!data || stride * size_t(height) > length)
        throw std::runtime_error("RETRY:x11:short_image");
    Image out = Allocate(width, height);
    const int bytes = bitsPerPixel / 8;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const uint8_t* p = data + size_t(y) * stride + size_t(x) * bytes;
        uint32_t pixel = 0;
        for (int byte = 0; byte < bytes; ++byte)
            pixel |= uint32_t(p[byte]) << (8 * (littleEndian ? byte : bytes - byte - 1));
        const size_t at = (size_t(y) * width + x) * 4;
        out.rgba[at] = Channel(pixel, redMask);
        out.rgba[at + 1] = Channel(pixel, greenMask);
        out.rgba[at + 2] = Channel(pixel, blueMask);
        if (alphaMask) out.rgba[at + 3] = Channel(pixel, alphaMask);
    }
    return out;
}
void Composite(Image& destination, const Image& source, int x, int y, uint32_t opacity) {
    if (source.width <= 0 || source.height <= 0 || destination.width <= 0 || destination.height <= 0 ||
        source.rgba.size() != size_t(source.width) * source.height * 4 ||
        destination.rgba.size() != size_t(destination.width) * destination.height * 4)
        throw std::runtime_error("ERROR:x11:invalid_image");
    for (int sy = std::max<int64_t>(0, -int64_t(y)); sy < source.height && int64_t(sy) + y < destination.height; ++sy) {
        for (int sx = std::max<int64_t>(0, -int64_t(x)); sx < source.width && int64_t(sx) + x < destination.width; ++sx) {
            const size_t from = (size_t(sy) * source.width + sx) * 4;
            const size_t to = (size_t(sy + y) * destination.width + sx + x) * 4;
            const unsigned alpha = unsigned((uint64_t(source.rgba[from + 3]) * opacity + UINT32_MAX / 2) / UINT32_MAX);
            for (int channel = 0; channel < 3; ++channel) {
                const unsigned colour = unsigned((uint64_t(source.rgba[from + channel]) * opacity + UINT32_MAX / 2) / UINT32_MAX);
                destination.rgba[to + channel] = uint8_t(std::min(255u, colour + (destination.rgba[to + channel] * (255 - alpha) + 127) / 255));
            }
            destination.rgba[to + 3] = uint8_t(alpha + (destination.rgba[to + 3] * (255 - alpha) + 127) / 255);
        }
    }
}
Image Resize(const Image& source, int width, int height) {
    Image out = Allocate(width, height);
    if (source.width <= 0 || source.height <= 0 ||
        source.rgba.size() != size_t(source.width) * source.height * 4)
        throw std::runtime_error("ERROR:x11:invalid_image");
    // Area average when shrinking; bilinear when enlarging. Text strokes survive downsampling.
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const size_t at = (size_t(y) * width + x) * 4;
        if (width <= source.width && height <= source.height) {
            const double x0 = double(x) * source.width / width;
            const double x1 = double(x + 1) * source.width / width;
            const double y0 = double(y) * source.height / height;
            const double y1 = double(y + 1) * source.height / height;
            double sums[3] = {};
            for (int sy = int(y0); sy < int(std::ceil(y1)) && sy < source.height; ++sy)
                for (int sx = int(x0); sx < int(std::ceil(x1)) && sx < source.width; ++sx) {
                    const double weight = (std::min(x1, double(sx + 1)) - std::max(x0, double(sx))) *
                                          (std::min(y1, double(sy + 1)) - std::max(y0, double(sy)));
                    for (int c = 0; c < 3; ++c) sums[c] += source.rgba[(size_t(sy) * source.width + sx) * 4 + c] * weight;
                }
            for (int c = 0; c < 3; ++c) out.rgba[at + c] = uint8_t(std::clamp(std::lround(sums[c] / ((x1 - x0) * (y1 - y0))), 0L, 255L));
        } else {
            const double sx = std::clamp((x + .5) * source.width / width - .5, 0.0, double(source.width - 1));
            const double sy = std::clamp((y + .5) * source.height / height - .5, 0.0, double(source.height - 1));
            const int ix = int(sx), iy = int(sy);
            for (int c = 0; c < 3; ++c) {
                double sum = 0;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                    const double weight = (dx ? sx - ix : 1 - (sx - ix)) * (dy ? sy - iy : 1 - (sy - iy));
                    sum += source.rgba[(size_t(std::min(iy + dy, source.height - 1)) * source.width + std::min(ix + dx, source.width - 1)) * 4 + c] * weight;
                }
                out.rgba[at + c] = uint8_t(std::clamp(std::lround(sum), 0L, 255L));
            }
        }
    }
    return out;
}
bool ParseRects(const std::string& input, std::vector<Rect>& output) {
    output.clear();
    if (input.empty()) return true;
    size_t at = 0;
    while (at < input.size()) {
        int values[4] = {};
        for (int field = 0; field < 4; ++field) {
            const size_t start = at;
            while (at < input.size() && input[at] >= '0' && input[at] <= '9') {
                values[field] = values[field] * 10 + input[at++] - '0';
                if (values[field] > 32767) return false;
            }
            if (at == start) return false;
            if (field < 3 && (at == input.size() || input[at++] != ',')) return false;
        }
        Rect r{values[0], values[1], values[2], values[3]};
        if (r.width == 0 || r.height == 0 || output.size() == 20) return false;
        for (const auto& previous : output) if (Intersects(r, previous)) return false;
        output.push_back(r);
        if (at == input.size()) break;
        if (input[at++] != ';' || at == input.size()) return false;
    }
    return true;
}
static void Pixel(Image& image, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) return;
    const size_t at = (size_t(y) * image.width + x) * 4;
    image.rgba[at] = r; image.rgba[at + 1] = g; image.rgba[at + 2] = b;
}
static void Text(Image& image, int x, int y, const std::string& text) {
    static const char* digits[] = {"111101101101111", "010110010010111", "111001111100111",
        "111001111001111", "101101111001001", "111100111001111", "111100111101111",
        "111001001001001", "111101111101111", "111101111001111"};
    const int scale = std::max(2, std::max(image.width, image.height) / 1200);
    for (int yy = -1; yy <= 5 * scale; ++yy)
        for (int xx = -1; xx <= int(text.size()) * 4 * scale; ++xx) Pixel(image, x + xx, y + yy, 0, 0, 0);
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9') continue;
        for (int row = 0; row < 5 * scale; ++row) for (int col = 0; col < 3 * scale; ++col)
            if (digits[text[index] - '0'][(row / scale) * 3 + col / scale] == '1')
                Pixel(image, x + int(index) * 4 * scale + col, y + row, 255, 255, 100);
    }
}
void Grid(Image& image, const Rect& region, std::string& gridX, std::string& gridY) {
    gridX.clear(); gridY.clear();
    const int columns = std::min(10, image.width / 50), rows = std::min(10, image.height / 50);
    for (int index = 1; index < columns; ++index) {
        const int x = image.width * index / columns;
        const std::string label = std::to_string(region.x + region.width * index / columns);
        if (!gridX.empty()) gridX += ',';
        gridX += label;
        for (int y = 0; y < image.height; ++y) Pixel(image, x, y, 255, 80, 80);
        Text(image, x + 2, 2, label);
    }
    for (int index = 1; index < rows; ++index) {
        const int y = image.height * index / rows;
        const std::string label = std::to_string(region.y + region.height * index / rows);
        if (!gridY.empty()) gridY += ',';
        gridY += label;
        for (int x = 0; x < image.width; ++x) Pixel(image, x, y, 255, 80, 80);
        Text(image, 2, y + 2, label);
    }
}
void Highlight(Image& image, const Rect& region, const Rect& rect, int label) {
    const int left = int(int64_t(rect.x - region.x) * image.width / region.width);
    const int top = int(int64_t(rect.y - region.y) * image.height / region.height);
    const int right = std::max(left + 1, int(int64_t(rect.x - region.x + rect.width) * image.width / region.width));
    const int bottom = std::max(top + 1, int(int64_t(rect.y - region.y + rect.height) * image.height / region.height));
    for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x)
        if (x - left < 3 || right - x <= 3 || y - top < 3 || bottom - y <= 3) Pixel(image, x, y, 0, 120, 255);
    const int scale = std::max(2, std::max(image.width, image.height) / 1200);
    const auto text = std::to_string(label);
    const int tx = std::max(0, right - int(text.size()) * 4 * scale - 2);
    const int ty = top >= 5 * scale + 4 ? top - 5 * scale - 2 : top + 2;
    Text(image, tx, std::max(0, std::min(ty, image.height - 5 * scale)), text);
}
}
