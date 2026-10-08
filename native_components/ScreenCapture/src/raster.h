#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace screen_capture::raster {
constexpr uint64_t kMaxPixels = 32ULL * 1024 * 1024;
struct Rect { int x = 0, y = 0, width = 0, height = 0; };
struct Image { int width = 0, height = 0; std::vector<uint8_t> rgba; };
bool Inside(const Rect& inner, const Rect& outer);
bool Intersects(const Rect& a, const Rect& b);
Image Resize(const Image& image, int width, int height);
bool ParseRects(const std::string& input, std::vector<Rect>& output);
void Grid(Image& image, const Rect& region, std::string& gridX, std::string& gridY);
void Highlight(Image& image, const Rect& region, const Rect& rect, int label);
// XRender pixmaps contain premultiplied RGB when an alpha mask is present.
// Keep that representation until layers have been composed over the opaque anchor.
Image Decode(const uint8_t* data, size_t length, int width, int height,
             int bitsPerPixel, int scanlinePad, bool littleEndian,
             uint32_t redMask, uint32_t greenMask, uint32_t blueMask, uint32_t alphaMask = 0);
void Composite(Image& destination, const Image& source, int x, int y, uint32_t opacity = UINT32_MAX);
}
