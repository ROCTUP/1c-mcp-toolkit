#pragma once
#include "raster.h"
#include <cstdint>
#include <string>
#include <vector>

namespace screen_capture::linux_capture {
struct Layer {
    uint32_t id = 0;
    raster::Rect rect;
    bool clipped = false;
};
struct Capture {
    raster::Image image;
    raster::Rect region; // Coordinates inside the stable main client area.
    raster::Rect screen; // Screen coordinates of the returned region, before scaling.
    std::vector<Layer> layers; // Relative to the main client area, bottom to top.
    uint32_t anchor = 0, foreground = 0;
    bool foregroundInFrame = false;
};
// Throws a protocol string (via runtime_error) on failure. No Xlib state is touched.
Capture Read(uint32_t pid, const raster::Rect* region = nullptr);
std::string Metadata(const Capture& capture, bool grid, const std::string& gridX, const std::string& gridY);
}
