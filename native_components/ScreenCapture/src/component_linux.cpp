#include "component.h"

#if defined(__linux__)

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "miniz.h"

namespace screen_capture {
namespace {

constexpr int kHighlightRectsMax = 20;
const WCHAR_T kErrorSource[] = {
    'S', 'c', 'r', 'e', 'e', 'n', 'C', 'a', 'p', 't', 'u', 'r', 'e', 0
};
const WCHAR_T kXOpenDisplayError[] = {
    'X', 'O', 'p', 'e', 'n', 'D', 'i', 's', 'p', 'l', 'a', 'y', ' ',
    'f', 'a', 'i', 'l', 'e', 'd', 0
};
const WCHAR_T kPngEncodingError[] = {
    'P', 'N', 'G', ' ', 'e', 'n', 'c', 'o', 'd', 'i', 'n', 'g', ' ',
    'f', 'a', 'i', 'l', 'e', 'd', 0
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

struct HighlightRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

uint8_t ChannelToByte(unsigned long pixel, unsigned long mask) {
    if (mask == 0) return 0;
    unsigned int shift = 0;
    while ((mask & 1UL) == 0) {
        mask >>= 1;
        ++shift;
    }
    const unsigned long value = (pixel >> shift) & mask;
    return static_cast<uint8_t>((value * 255UL + mask / 2UL) / mask);
}

bool GetCardinalProperty(Display* display, Window window, Atom property, unsigned long& value) {
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(
        display, window, property, 0, 1, False, XA_CARDINAL,
        &actualType, &actualFormat, &itemCount, &bytesAfter, &data);
    if (status != Success || actualType != XA_CARDINAL || actualFormat != 32 ||
        itemCount != 1 || data == nullptr) {
        if (data != nullptr) XFree(data);
        return false;
    }
    value = *reinterpret_cast<unsigned long*>(data);
    XFree(data);
    return true;
}

bool GetWindowProperty(Display* display, Window window, Atom property, Window& value) {
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char* data = nullptr;
    const int status = XGetWindowProperty(
        display, window, property, 0, 1, False, XA_WINDOW,
        &actualType, &actualFormat, &itemCount, &bytesAfter, &data);
    if (status != Success || actualType != XA_WINDOW || actualFormat != 32 ||
        itemCount != 1 || data == nullptr) {
        if (data != nullptr) XFree(data);
        return false;
    }
    value = *reinterpret_cast<Window*>(data);
    XFree(data);
    return true;
}

bool IsWindowOfProcess(Display* display, Window window, pid_t pid, XWindowAttributes* attributes = nullptr) {
    XWindowAttributes localAttributes{};
    if (XGetWindowAttributes(display, window, &localAttributes) == 0 ||
        localAttributes.map_state != IsViewable || localAttributes.width <= 0 ||
        localAttributes.height <= 0) {
        return false;
    }
    const Atom pidAtom = XInternAtom(display, "_NET_WM_PID", True);
    if (pidAtom == None) return false;
    unsigned long windowPid = 0;
    if (!GetCardinalProperty(display, window, pidAtom, windowPid) ||
        windowPid != static_cast<unsigned long>(pid)) {
        return false;
    }
    if (attributes != nullptr) *attributes = localAttributes;
    return true;
}

void FindLargestProcessWindow(Display* display, Window parent, pid_t pid,
                              Window& bestWindow, int64_t& bestArea) {
    Window root = None;
    Window parentResult = None;
    Window* children = nullptr;
    unsigned int childCount = 0;
    if (XQueryTree(display, parent, &root, &parentResult, &children, &childCount) == 0) return;

    for (unsigned int index = 0; index < childCount; ++index) {
        XWindowAttributes attributes{};
        if (IsWindowOfProcess(display, children[index], pid, &attributes)) {
            const int64_t area = static_cast<int64_t>(attributes.width) * attributes.height;
            if (area > bestArea) {
                bestArea = area;
                bestWindow = children[index];
            }
        }
        FindLargestProcessWindow(display, children[index], pid, bestWindow, bestArea);
    }
    if (children != nullptr) XFree(children);
}

Window FindProcessWindow(Display* display, pid_t pid) {
    const Window root = DefaultRootWindow(display);
    const Atom activeWindowAtom = XInternAtom(display, "_NET_ACTIVE_WINDOW", True);
    Window activeWindow = None;
    if (activeWindowAtom != None &&
        GetWindowProperty(display, root, activeWindowAtom, activeWindow) &&
        IsWindowOfProcess(display, activeWindow, pid)) {
        return activeWindow;
    }

    Window bestWindow = None;
    int64_t bestArea = 0;
    FindLargestProcessWindow(display, root, pid, bestWindow, bestArea);
    return bestWindow;
}

Image CropImage(const Image& source, int x, int y, int width, int height) {
    Image result;
    result.width = width;
    result.height = height;
    result.rgba.resize(static_cast<size_t>(width) * height * 4);
    for (int row = 0; row < height; ++row) {
        const size_t sourceOffset =
            (static_cast<size_t>(y + row) * source.width + x) * 4;
        const size_t resultOffset = static_cast<size_t>(row) * width * 4;
        std::copy_n(source.rgba.data() + sourceOffset,
                    static_cast<size_t>(width) * 4,
                    result.rgba.data() + resultOffset);
    }
    return result;
}

Image ScaleImage(const Image& source, int width, int height) {
    if (source.width == width && source.height == height) return source;
    Image result;
    result.width = width;
    result.height = height;
    result.rgba.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const int sourceY = std::min(source.height - 1, y * source.height / height);
        for (int x = 0; x < width; ++x) {
            const int sourceX = std::min(source.width - 1, x * source.width / width);
            const size_t sourceOffset =
                (static_cast<size_t>(sourceY) * source.width + sourceX) * 4;
            const size_t resultOffset = (static_cast<size_t>(y) * width + x) * 4;
            std::copy_n(source.rgba.data() + sourceOffset, 4,
                        result.rgba.data() + resultOffset);
        }
    }
    return result;
}

void SetPixel(Image& image, int x, int y, uint8_t red, uint8_t green, uint8_t blue) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) return;
    const size_t offset = (static_cast<size_t>(y) * image.width + x) * 4;
    image.rgba[offset] = red;
    image.rgba[offset + 1] = green;
    image.rgba[offset + 2] = blue;
    image.rgba[offset + 3] = 255;
}

void FillRect(Image& image, int x, int y, int width, int height,
              uint8_t red, uint8_t green, uint8_t blue) {
    for (int row = std::max(0, y); row < std::min(image.height, y + height); ++row) {
        for (int column = std::max(0, x); column < std::min(image.width, x + width); ++column) {
            SetPixel(image, column, row, red, green, blue);
        }
    }
}

const char* DigitPattern(char digit) {
    static const char* patterns[] = {
        "111101101101111", "010110010010111", "111001111100111",
        "111001111001111", "101101111001001", "111100111001111",
        "111100111101111", "111001001001001", "111101111101111",
        "111101111001111"
    };
    if (digit >= '0' && digit <= '9') return patterns[digit - '0'];
    return digit == '-' ? "000000111000000" : "000000000000000";
}

void DrawText(Image& image, int x, int y, const std::string& text, int pixelScale) {
    const int characterWidth = 3 * pixelScale;
    const int characterHeight = 5 * pixelScale;
    const int advance = characterWidth + pixelScale;
    FillRect(image, x - pixelScale, y - pixelScale,
             static_cast<int>(text.size()) * advance + pixelScale,
             characterHeight + 2 * pixelScale, 0, 0, 0);
    for (size_t index = 0; index < text.size(); ++index) {
        const char* pattern = DigitPattern(text[index]);
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if (pattern[row * 3 + column] == '1') {
                    FillRect(image, x + static_cast<int>(index) * advance + column * pixelScale,
                             y + row * pixelScale, pixelScale, pixelScale,
                             255, 255, 100);
                }
            }
        }
    }
}

void DrawGrid(Image& image, int originalWidth, int originalHeight,
              int offsetX, int offsetY, std::string& gridX, std::string& gridY) {
    constexpr int kMinCellPixels = 50;
    const int columns = std::min(10, image.width / kMinCellPixels);
    const int rows = std::min(10, image.height / kMinCellPixels);
    const int fontScale = std::max(1, std::max(image.width, image.height) / 1200);

    for (int index = 1; index < columns; ++index) {
        const int x = image.width * index / columns;
        const int originalX = offsetX + originalWidth * index / columns;
        if (!gridX.empty()) gridX += ',';
        gridX += std::to_string(originalX);
        for (int y = 0; y < image.height; ++y) SetPixel(image, x, y, 255, 80, 80);
        DrawText(image, x + 2, 2, std::to_string(originalX), fontScale);
    }
    for (int index = 1; index < rows; ++index) {
        const int y = image.height * index / rows;
        const int originalY = offsetY + originalHeight * index / rows;
        if (!gridY.empty()) gridY += ',';
        gridY += std::to_string(originalY);
        for (int x = 0; x < image.width; ++x) SetPixel(image, x, y, 255, 80, 80);
        DrawText(image, 2, y + 2, std::to_string(originalY), fontScale);
    }
}

bool ParseHighlightRects(const std::string& value, std::vector<HighlightRect>& rectangles) {
    if (value.empty()) return true;
    size_t segmentStart = 0;
    while (segmentStart <= value.size()) {
        const size_t segmentEnd = value.find(';', segmentStart);
        const std::string segment = value.substr(
            segmentStart, segmentEnd == std::string::npos ? std::string::npos : segmentEnd - segmentStart);
        std::vector<int> numbers;
        size_t tokenStart = 0;
        while (tokenStart <= segment.size()) {
            const size_t tokenEnd = segment.find(',', tokenStart);
            const std::string token = segment.substr(
                tokenStart, tokenEnd == std::string::npos ? std::string::npos : tokenEnd - tokenStart);
            try {
                size_t parsed = 0;
                const long number = std::stol(token, &parsed);
                if (parsed != token.size() || number < 0 || number > 32767) return false;
                numbers.push_back(static_cast<int>(number));
            } catch (...) {
                return false;
            }
            if (tokenEnd == std::string::npos) break;
            tokenStart = tokenEnd + 1;
        }
        if (numbers.size() != 4 || numbers[2] <= 0 || numbers[3] <= 0) return false;
        rectangles.push_back({numbers[0], numbers[1], numbers[2], numbers[3]});
        if (rectangles.size() > kHighlightRectsMax) return false;
        if (segmentEnd == std::string::npos) break;
        segmentStart = segmentEnd + 1;
    }
    for (size_t first = 0; first < rectangles.size(); ++first) {
        for (size_t second = first + 1; second < rectangles.size(); ++second) {
            const auto& a = rectangles[first];
            const auto& b = rectangles[second];
            if (!(a.x + a.width <= b.x || b.x + b.width <= a.x ||
                  a.y + a.height <= b.y || b.y + b.height <= a.y)) {
                return false;
            }
        }
    }
    return true;
}

bool DrawHighlightRect(Image& image, const HighlightRect& rectangle, int label,
                       int originalWidth, int originalHeight, int offsetX, int offsetY) {
    const int relativeX = rectangle.x - offsetX;
    const int relativeY = rectangle.y - offsetY;
    if (relativeX < 0 || relativeY < 0 ||
        relativeX + rectangle.width > originalWidth ||
        relativeY + rectangle.height > originalHeight) {
        return false;
    }
    const int left = relativeX * image.width / originalWidth;
    const int top = relativeY * image.height / originalHeight;
    const int right = (relativeX + rectangle.width) * image.width / originalWidth;
    const int bottom = (relativeY + rectangle.height) * image.height / originalHeight;
    for (int thickness = 0; thickness < 3; ++thickness) {
        for (int x = left; x < right; ++x) {
            SetPixel(image, x, top + thickness, 0, 120, 255);
            SetPixel(image, x, bottom - 1 - thickness, 0, 120, 255);
        }
        for (int y = top; y < bottom; ++y) {
            SetPixel(image, left + thickness, y, 0, 120, 255);
            SetPixel(image, right - 1 - thickness, y, 0, 120, 255);
        }
    }
    const std::string labelText = std::to_string(label);
    const int fontScale = std::max(1, std::max(image.width, image.height) / 1200);
    const int textWidth = static_cast<int>(labelText.size()) * 4 * fontScale;
    const int textHeight = 5 * fontScale;
    const int textX = std::max(0, right - textWidth - 2);
    const int textY = top >= textHeight + 4 ? top - textHeight - 2 : top + 2;
    DrawText(image, textX, std::min(textY, image.height - textHeight), labelText, fontScale);
    return true;
}

} // namespace

bool ScreenCaptureComponent::CaptureMainWindow(int scale, bool showGrid, std::string& outB64,
                                                int regionX, int regionY,
                                                int regionW, int regionH,
                                                const std::string& highlightRects) {
    outB64.clear();
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        addin_base_->AddError(1, kErrorSource, kXOpenDisplayError, 0);
        return false;
    }

    const Window window = FindProcessWindow(display, getpid());
    if (window == None) {
        XCloseDisplay(display);
        return true;
    }

    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window, &attributes) == 0 ||
        attributes.width <= 0 || attributes.height <= 0) {
        XCloseDisplay(display);
        outB64 = "RETRY:wh0";
        return true;
    }

    Window child = None;
    int coordinateLeft = 0;
    int coordinateTop = 0;
    XTranslateCoordinates(display, window, DefaultRootWindow(display), 0, 0,
                          &coordinateLeft, &coordinateTop, &child);

    XImage* ximage = XGetImage(display, window, 0, 0,
                              static_cast<unsigned int>(attributes.width),
                              static_cast<unsigned int>(attributes.height),
                              AllPlanes, ZPixmap);
    if (ximage == nullptr) {
        XCloseDisplay(display);
        outB64 = "RETRY:xgetimage";
        return true;
    }

    Image image;
    image.width = attributes.width;
    image.height = attributes.height;
    image.rgba.resize(static_cast<size_t>(image.width) * image.height * 4);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const unsigned long pixel = XGetPixel(ximage, x, y);
            const size_t offset = (static_cast<size_t>(y) * image.width + x) * 4;
            image.rgba[offset] = ChannelToByte(pixel, ximage->red_mask);
            image.rgba[offset + 1] = ChannelToByte(pixel, ximage->green_mask);
            image.rgba[offset + 2] = ChannelToByte(pixel, ximage->blue_mask);
            image.rgba[offset + 3] = 255;
        }
    }
    XDestroyImage(ximage);
    XCloseDisplay(display);

    int originalWidth = image.width;
    int originalHeight = image.height;
    int offsetX = 0;
    int offsetY = 0;
    const bool crop = regionX >= 0 && regionY >= 0 && regionW > 0 && regionH > 0;
    if (crop) {
        if (static_cast<int64_t>(regionX) + regionW > image.width ||
            static_cast<int64_t>(regionY) + regionH > image.height) {
            outB64 = "ERROR:region_oob";
            return true;
        }
        image = CropImage(image, regionX, regionY, regionW, regionH);
        coordinateLeft += regionX;
        coordinateTop += regionY;
        originalWidth = regionW;
        originalHeight = regionH;
        offsetX = regionX;
        offsetY = regionY;
    }

    std::vector<HighlightRect> rectangles;
    if (!ParseHighlightRects(highlightRects, rectangles)) {
        outB64 = "ERROR:highlight_fmt";
        return true;
    }

    const int scaledWidth = std::max(1, originalWidth * scale / 100);
    const int scaledHeight = std::max(1, originalHeight * scale / 100);
    image = ScaleImage(image, scaledWidth, scaledHeight);

    std::string gridX;
    std::string gridY;
    if (showGrid) {
        DrawGrid(image, originalWidth, originalHeight, offsetX, offsetY, gridX, gridY);
    }
    for (size_t index = 0; index < rectangles.size(); ++index) {
        if (!DrawHighlightRect(image, rectangles[index], static_cast<int>(index + 1),
                               originalWidth, originalHeight, offsetX, offsetY)) {
            outB64 = "ERROR:highlight_oob:" + std::to_string(index + 1);
            return true;
        }
    }

    size_t pngLength = 0;
    void* pngBuffer = tdefl_write_image_to_png_file_in_memory(
        image.rgba.data(), image.width, image.height, 4, &pngLength);
    if (pngBuffer == nullptr) {
        addin_base_->AddError(1, kErrorSource, kPngEncodingError, 0);
        return false;
    }
    const auto* pngBytes = static_cast<uint8_t*>(pngBuffer);
    std::vector<uint8_t> png(pngBytes, pngBytes + pngLength);
    mz_free(pngBuffer);

    outB64 = std::to_string(coordinateLeft) + "|" +
             std::to_string(coordinateTop) + "|" +
             std::to_string(originalWidth) + "|" +
             std::to_string(originalHeight);
    if (showGrid) outB64 += "|" + gridX + "|" + gridY;
    outB64 += "|" + Base64Encode(png);
    return true;
}

} // namespace screen_capture

#endif // __linux__
