#include "component.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <vector>

#ifdef _WINDOWS
#include <windows.h>
#endif

// miniz for PNG encoding
#include "miniz.h"

namespace screen_capture {

static constexpr int kHighlightRectsMax = 20;

static std::wstring WcharTToWstr(const WCHAR_T* source, size_t length = 0) {
    if (source == nullptr) return L"";
#ifdef _WINDOWS
    return length == 0 ? std::wstring(source) : std::wstring(source, length);
#else
    std::wstring result;
    if (length == 0) {
        while (source[length] != 0) ++length;
    }
    result.reserve(length);
    for (size_t index = 0; index < length; ++index) {
        result += static_cast<wchar_t>(source[index]);
    }
    return result;
#endif
}

// ============================================================================
//  Name tables
// ============================================================================

const wchar_t* ScreenCaptureComponent::method_names_en_[] = {
    L"CaptureWindow",
};

const wchar_t* ScreenCaptureComponent::method_names_ru_[] = {
    L"ЗахватитьОкно",
};

// ============================================================================
//  IInitDoneBase
// ============================================================================

bool ScreenCaptureComponent::Init(void* disp) {
    addin_base_ = static_cast<IAddInDefBase*>(disp);
    return addin_base_ != nullptr;
}

bool ScreenCaptureComponent::setMemManager(void* mem) {
    mem_manager_ = static_cast<IMemoryManager*>(mem);
    return mem_manager_ != nullptr;
}

void ScreenCaptureComponent::Done() {
    addin_base_  = nullptr;
    mem_manager_ = nullptr;
}

// ============================================================================
//  RegisterExtensionAs
// ============================================================================

bool ScreenCaptureComponent::RegisterExtensionAs(WCHAR_T** wsExtensionName) {
    return AllocWStr(wsExtensionName, L"ScreenCapture");
}

// ============================================================================
//  Methods
// ============================================================================

long ScreenCaptureComponent::GetNMethods() { return eMethodLast; }

long ScreenCaptureComponent::FindMethod(const WCHAR_T* wsMethodName) {
    std::wstring name = WcharTToWstr(wsMethodName);
    auto lower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    };
    std::wstring lower_name = lower(name);
    for (long i = 0; i < eMethodLast; ++i) {
        if (lower(method_names_en_[i]) == lower_name ||
            lower(method_names_ru_[i]) == lower_name) {
            return i;
        }
    }
    return -1;
}

const WCHAR_T* ScreenCaptureComponent::GetMethodName(const long lMethodNum,
                                                       const long lMethodAlias) {
    if (lMethodNum < 0 || lMethodNum >= eMethodLast) return nullptr;
    const wchar_t* name = (lMethodAlias == 0) ? method_names_en_[lMethodNum]
                                               : method_names_ru_[lMethodNum];
    WCHAR_T* result = nullptr;
    if (AllocWStr(&result, std::wstring(name))) return result;
    return nullptr;
}

long ScreenCaptureComponent::GetNParams(const long lMethodNum) {
    switch (lMethodNum) {
        case eMethodCaptureWindow: return 7;
        default:                   return 0;
    }
}

bool ScreenCaptureComponent::GetParamDefValue(const long lMethodNum, const long lParamNum, tVariant* pDefVal) {
    if (lMethodNum != eMethodCaptureWindow) return false;
    if (lParamNum == 1) {
        // showGrid default = false
        TV_VT(pDefVal) = VTYPE_BOOL;
        TV_BOOL(pDefVal) = false;
        return true;
    }
    if (lParamNum >= 2 && lParamNum <= 5) {
        // regionX/Y/W/H default = -1 (sentinel "no region")
        TV_VT(pDefVal) = VTYPE_I4;
        TV_I4(pDefVal) = -1;
        return true;
    }
    if (lParamNum == 6) {
        // highlightRects default = "" (no rectangles)
        TV_VT(pDefVal) = VTYPE_PWSTR;
        pDefVal->wstrLen = 0;
        return AllocWStr(&pDefVal->pwstrVal, L"");
    }
    return false;
}

bool ScreenCaptureComponent::HasRetVal(const long lMethodNum) {
    return lMethodNum == eMethodCaptureWindow;
}

bool ScreenCaptureComponent::CallAsFunc(const long lMethodNum,
                                         tVariant* pvarRetValue,
                                         tVariant* paParams,
                                         const long lSizeArray) {
    if (!pvarRetValue) return false;

    switch (lMethodNum) {
        case eMethodCaptureWindow: {
            int  scale    = (lSizeArray >= 1) ? GetIntFromVariant(&paParams[0], 100) : 100;
            bool showGrid = (lSizeArray >= 2) ? GetBoolFromVariant(&paParams[1])     : false;
            int  regionX  = (lSizeArray >= 3) ? GetIntFromVariant(&paParams[2], -1)  : -1;
            int  regionY  = (lSizeArray >= 4) ? GetIntFromVariant(&paParams[3], -1)  : -1;
            int  regionW  = (lSizeArray >= 5) ? GetIntFromVariant(&paParams[4], -1)  : -1;
            int  regionH  = (lSizeArray >= 6) ? GetIntFromVariant(&paParams[5], -1)  : -1;
            std::string hlRects = (lSizeArray >= 7) ? GetStringFromVariant(&paParams[6]) : "";
            std::string b64;
            bool ok = CaptureMainWindow(scale, showGrid, b64, regionX, regionY, regionW, regionH, hlRects);
            if (!ok) return false;  // AddError already called inside
            return SetStringToVariant(pvarRetValue, b64);  // "" → BSL retries
        }
        default:
            return false;
    }
}

// ============================================================================
//  CaptureMainWindow
// ============================================================================

#ifdef _WINDOWS

// Check that hwnd belongs to our PID and is visible
static bool IsOurWindow(HWND hwnd, DWORD pid) {
    if (!hwnd || !IsWindowVisible(hwnd)) return false;
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    return wpid == pid;
}

// Fallback: find the largest visible top-level window of the process
struct FindMainData { DWORD pid; HWND best; int bestArea; };

static BOOL CALLBACK EnumMainProc(HWND hwnd, LPARAM lp) {
    auto* d = reinterpret_cast<FindMainData*>(lp);
    if (!IsOurWindow(hwnd, d->pid)) return TRUE;
    if (GetParent(hwnd) != NULL) return TRUE;
    RECT rc; GetClientRect(hwnd, &rc);
    int area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > d->bestArea) { d->bestArea = area; d->best = hwnd; }
    return TRUE;
}

bool ScreenCaptureComponent::CaptureMainWindow(int scale, bool showGrid, std::string& outB64,
                                                int regionX, int regionY,
                                                int regionW, int regionH,
                                                const std::string& highlightRects) {
    // Invariant: always clear first
    outB64.clear();

    DWORD pid = GetCurrentProcessId();
    HWND hwnd = nullptr;

    // Primary: foreground window → root owner → PID check
    HWND fg = GetForegroundWindow();
    if (fg) {
        HWND root = GetAncestor(fg, GA_ROOTOWNER);
        if (root && IsOurWindow(root, pid)) {
            hwnd = root;
        }
    }

    // Fallback: largest visible top-level window of our process
    if (!hwnd) {
        FindMainData fd { pid, nullptr, 0 };
        EnumWindows(EnumMainProc, reinterpret_cast<LPARAM>(&fd));
        hwnd = fd.best;
    }

    if (!hwnd) {
        // No window found — BSL will retry
        return true;
    }

    // ---- Step 1: get client rect ----
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top;
    if (w == 0 || h == 0) {
        // Window found but not yet rendered (GetClientRect returned 0x0)
        outB64 = "RETRY:wh0";
        return true;
    }

    // Capture client-area screen position atomically with image capture
    POINT ptOrigin = { 0, 0 };
    ClientToScreen(hwnd, &ptOrigin);
    int coordLeft = static_cast<int>(ptOrigin.x);
    int coordTop  = static_cast<int>(ptOrigin.y);

    // Compute target dimensions (before any GDI resource creation)
    int sw = (scale == 100) ? w : std::max(1, w * scale / 100);
    int sh = (scale == 100) ? h : std::max(1, h * scale / 100);

    // ---- Step 2: screen DC ----
    HDC hScreenDC = GetDC(nullptr);
    if (!hScreenDC) {
        addin_base_->AddError(1, L"ScreenCapture", L"GetDC failed", 0);
        return false;
    }

    // ---- Step 3: source compatible DC ----
    HDC hdcSrc = CreateCompatibleDC(hScreenDC);
    if (!hdcSrc) {
        ReleaseDC(nullptr, hScreenDC);
        addin_base_->AddError(1, L"ScreenCapture", L"CreateCompatibleDC failed", 0);
        return false;
    }

    // ---- Step 4: source bitmap ----
    HBITMAP hbmpSrc = CreateCompatibleBitmap(hScreenDC, w, h);
    if (!hbmpSrc) {
        DeleteDC(hdcSrc);
        ReleaseDC(nullptr, hScreenDC);
        addin_base_->AddError(1, L"ScreenCapture", L"CreateCompatibleBitmap failed", 0);
        return false;
    }

    // ---- Step 5: select source bitmap ----
    HGDIOBJ oldSrc = SelectObject(hdcSrc, hbmpSrc);
    if (!oldSrc || oldSrc == HGDI_ERROR) {
        // hbmpSrc not yet selected — safe to delete directly
        DeleteObject(hbmpSrc);
        DeleteDC(hdcSrc);
        ReleaseDC(nullptr, hScreenDC);
        addin_base_->AddError(1, L"ScreenCapture", L"SelectObject(src) failed", 0);
        return false;
    }

    // ---- Step 6: PrintWindow ----
    // PW_CLIENTONLY (0x1) | PW_RENDERFULLCONTENT (0x2) = 0x3
    if (!PrintWindow(hwnd, hdcSrc, 0x3)) {
        SelectObject(hdcSrc, oldSrc);
        DeleteObject(hbmpSrc);
        DeleteDC(hdcSrc);
        ReleaseDC(nullptr, hScreenDC);
        outB64 = "RETRY:pw0";
        return true;
    }

    // ---- Step 6.5: crop to region (if specified) ----
    bool doCrop = (regionX >= 0 && regionY >= 0 && regionW > 0 && regionH > 0);
    int gridOffsetX = 0, gridOffsetY = 0;
    if (doCrop) {
        // Safe overflow-free bounds check
        if ((int64_t)regionX + regionW > (int64_t)w ||
            (int64_t)regionY + regionH > (int64_t)h ||
            regionX >= w || regionY >= h) {
            SelectObject(hdcSrc, oldSrc);
            DeleteObject(hbmpSrc);
            DeleteDC(hdcSrc);
            ReleaseDC(nullptr, hScreenDC);
            outB64 = "ERROR:region_oob";
            return true;
        }

        HDC hdcCrop = CreateCompatibleDC(hScreenDC);
        HBITMAP hbmpCrop = hdcCrop ? CreateCompatibleBitmap(hScreenDC, regionW, regionH) : nullptr;
        HGDIOBJ oldCrop  = hbmpCrop ? SelectObject(hdcCrop, hbmpCrop) : nullptr;
        if (!hdcCrop || !hbmpCrop || !oldCrop || oldCrop == HGDI_ERROR) {
            if (oldCrop && oldCrop != HGDI_ERROR) SelectObject(hdcCrop, oldCrop);
            if (hbmpCrop) DeleteObject(hbmpCrop);
            if (hdcCrop)  DeleteDC(hdcCrop);
            SelectObject(hdcSrc, oldSrc);
            DeleteObject(hbmpSrc);
            DeleteDC(hdcSrc);
            ReleaseDC(nullptr, hScreenDC);
            addin_base_->AddError(1, L"ScreenCapture", L"GDI crop setup failed", 0);
            return false;
        }

        if (!BitBlt(hdcCrop, 0, 0, regionW, regionH, hdcSrc, regionX, regionY, SRCCOPY)) {
            SelectObject(hdcCrop, oldCrop); DeleteObject(hbmpCrop); DeleteDC(hdcCrop);
            SelectObject(hdcSrc, oldSrc);  DeleteObject(hbmpSrc);  DeleteDC(hdcSrc);
            ReleaseDC(nullptr, hScreenDC);
            addin_base_->AddError(1, L"ScreenCapture", L"BitBlt(crop) failed", 0);
            return false;
        }

        SelectObject(hdcSrc, oldSrc); DeleteObject(hbmpSrc); DeleteDC(hdcSrc);
        hdcSrc  = hdcCrop; hbmpSrc = hbmpCrop; oldSrc = oldCrop;

        // Offset for grid labels (show original client-area coordinates, not region-relative)
        gridOffsetX = regionX;
        gridOffsetY = regionY;

        coordLeft += regionX;
        coordTop  += regionY;
        w = regionW;
        h = regionH;
        sw = (scale == 100) ? w : std::max(1, w * scale / 100);
        sh = (scale == 100) ? h : std::max(1, h * scale / 100);
    }

    // ---- Step 6.5: parse highlight_rects string ----
    std::vector<HighlightRect> hlRects;
    if (!highlightRects.empty()) {
        // Split by ';'
        std::vector<std::string> segments;
        {
            std::string seg;
            for (char c : highlightRects) {
                if (c == ';') { segments.push_back(seg); seg.clear(); }
                else          { seg += c; }
            }
            segments.push_back(seg);
        }
        if ((int)segments.size() > kHighlightRectsMax) {
            SelectObject(hdcSrc, oldSrc); DeleteObject(hbmpSrc); DeleteDC(hdcSrc);
            ReleaseDC(nullptr, hScreenDC);
            outB64 = "ERROR:highlight_fmt";
            return true;
        }
        for (const auto& seg : segments) {
            // Split by ','
            std::vector<std::string> tokens;
            {
                std::string tok;
                for (char c : seg) {
                    if (c == ',') { tokens.push_back(tok); tok.clear(); }
                    else          { tok += c; }
                }
                tokens.push_back(tok);
            }
            if (tokens.size() != 4) {
                SelectObject(hdcSrc, oldSrc); DeleteObject(hbmpSrc); DeleteDC(hdcSrc);
                ReleaseDC(nullptr, hScreenDC);
                outB64 = "ERROR:highlight_fmt";
                return true;
            }
            HighlightRect r{};
            try {
                size_t pos0, pos1, pos2, pos3;
                r.x = std::stoi(tokens[0], &pos0);
                r.y = std::stoi(tokens[1], &pos1);
                r.w = std::stoi(tokens[2], &pos2);
                r.h = std::stoi(tokens[3], &pos3);
                // Reject partial parses like "10abc", "1e2", "5 "
                if (pos0 != tokens[0].size() || pos1 != tokens[1].size() ||
                    pos2 != tokens[2].size() || pos3 != tokens[3].size()) {
                    SelectObject(hdcSrc, oldSrc); DeleteObject(hbmpSrc); DeleteDC(hdcSrc);
                    ReleaseDC(nullptr, hScreenDC);
                    outB64 = "ERROR:highlight_fmt";
                    return true;
                }
            } catch (...) {
                SelectObject(hdcSrc, oldSrc); DeleteObject(hbmpSrc); DeleteDC(hdcSrc);
                ReleaseDC(nullptr, hScreenDC);
                outB64 = "ERROR:highlight_fmt";
                return true;
            }
            if (r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 ||
                r.x > 32767 || r.y > 32767 || r.w > 32767 || r.h > 32767) {
                SelectObject(hdcSrc, oldSrc); DeleteObject(hbmpSrc); DeleteDC(hdcSrc);
                ReleaseDC(nullptr, hScreenDC);
                outB64 = "ERROR:highlight_fmt";
                return true;
            }
            hlRects.push_back(r);
        }
        // Overlap check (same formula as Python/BSL)
        for (int i = 0; i < (int)hlRects.size(); ++i) {
            for (int j = i + 1; j < (int)hlRects.size(); ++j) {
                const auto& a = hlRects[i];
                const auto& b = hlRects[j];
                if (!(a.x + a.w <= b.x || b.x + b.w <= a.x ||
                      a.y + a.h <= b.y || b.y + b.h <= a.y)) {
                    SelectObject(hdcSrc, oldSrc); DeleteObject(hbmpSrc); DeleteDC(hdcSrc);
                    ReleaseDC(nullptr, hScreenDC);
                    outB64 = "ERROR:highlight_fmt";
                    return true;
                }
            }
        }
    }

    // ---- Step 7: scale (only if scale != 100) ----
    HDC     hdcFinal  = nullptr;
    HBITMAP hbmpFinal = nullptr;
    HGDIOBJ oldFinal  = nullptr;

    if (scale != 100) {
        HDC hdcDst = CreateCompatibleDC(hScreenDC);
        if (!hdcDst) {
            SelectObject(hdcSrc, oldSrc);
            DeleteObject(hbmpSrc);
            DeleteDC(hdcSrc);
            ReleaseDC(nullptr, hScreenDC);
            addin_base_->AddError(1, L"ScreenCapture", L"CreateCompatibleDC(dst) failed", 0);
            return false;
        }

        HBITMAP hbmpDst = CreateCompatibleBitmap(hScreenDC, sw, sh);
        if (!hbmpDst) {
            SelectObject(hdcSrc, oldSrc);
            DeleteObject(hbmpSrc);
            DeleteDC(hdcSrc);
            DeleteDC(hdcDst);
            ReleaseDC(nullptr, hScreenDC);
            addin_base_->AddError(1, L"ScreenCapture", L"CreateCompatibleBitmap(dst) failed", 0);
            return false;
        }

        HGDIOBJ oldDst = SelectObject(hdcDst, hbmpDst);
        if (!oldDst || oldDst == HGDI_ERROR) {
            // hbmpDst not yet selected — safe to delete directly
            SelectObject(hdcSrc, oldSrc);
            DeleteObject(hbmpSrc);
            DeleteDC(hdcSrc);
            DeleteObject(hbmpDst);
            DeleteDC(hdcDst);
            ReleaseDC(nullptr, hScreenDC);
            addin_base_->AddError(1, L"ScreenCapture", L"SelectObject(dst) failed", 0);
            return false;
        }

        SetStretchBltMode(hdcDst, HALFTONE);
        if (!StretchBlt(hdcDst, 0, 0, sw, sh, hdcSrc, 0, 0, w, h, SRCCOPY)) {
            SelectObject(hdcSrc, oldSrc);
            DeleteObject(hbmpSrc);
            DeleteDC(hdcSrc);
            SelectObject(hdcDst, oldDst);
            DeleteObject(hbmpDst);
            DeleteDC(hdcDst);
            ReleaseDC(nullptr, hScreenDC);
            addin_base_->AddError(1, L"ScreenCapture", L"StretchBlt failed", 0);
            return false;
        }

        // Release source (bitmap must be deselected before deletion)
        SelectObject(hdcSrc, oldSrc);
        DeleteObject(hbmpSrc);
        DeleteDC(hdcSrc);

        hdcFinal  = hdcDst;
        hbmpFinal = hbmpDst;
        oldFinal  = oldDst;
    } else {
        hdcFinal  = hdcSrc;
        hbmpFinal = hbmpSrc;
        oldFinal  = oldSrc;
        sw = w;
        sh = h;
    }

    // ---- Step 7.5: draw grid if requested ----
    // Dynamic density: minimum 50px per cell on the scaled image.
    // Degradation is per-axis: a narrow-but-tall region gets only horizontal lines.
    // DrawGrid's loops are for(i=1; i<cols; ++i), so cols/rows <= 1 → no lines on that axis.
    const int kMinCellPx = 50;
    int gridCols = std::min(10, sw / kMinCellPx);
    int gridRows = std::min(10, sh / kMinCellPx);

    std::string gridXStr, gridYStr;
    if (showGrid) {
        if (gridCols > 0 || gridRows > 0) {
            DrawGrid(hdcFinal, sw, sh, w, h, scale, gridCols, gridRows, gridOffsetX, gridOffsetY);
        }
        // Collect coordinates of actually-drawn lines only: i=1..cols-1 (no edges)
        for (int i = 1; i < gridCols; ++i) {
            if (i > 1) gridXStr += ",";
            gridXStr += std::to_string(gridOffsetX + w * i / gridCols);
        }
        for (int i = 1; i < gridRows; ++i) {
            if (i > 1) gridYStr += ",";
            gridYStr += std::to_string(gridOffsetY + h * i / gridRows);
        }
    }

    // ---- Step 7.6: draw highlight rectangles ----
    for (int i = 0; i < (int)hlRects.size(); ++i) {
        if (!DrawHighlightRect(hdcFinal, sw, sh, w, h, hlRects[i], i + 1, gridOffsetX, gridOffsetY)) {
            SelectObject(hdcFinal, oldFinal);
            DeleteObject(hbmpFinal);
            DeleteDC(hdcFinal);
            ReleaseDC(nullptr, hScreenDC);
            outB64 = "ERROR:highlight_oob:" + std::to_string(i + 1);
            return true;
        }
    }

    // ---- Step 8: prepare buffer and BITMAPINFOHEADER, deselect, then GetDIBits ----
    BITMAPINFOHEADER bih = {};
    bih.biSize        = sizeof(BITMAPINFOHEADER);
    bih.biWidth       = sw;
    bih.biHeight      = -sh;   // negative → top-down (rows from top to bottom)
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;    // BGRA, 4 bytes per pixel
    bih.biCompression = BI_RGB;

    BITMAPINFO bmi = {};
    bmi.bmiHeader = bih;

    std::vector<uint8_t> pixels(static_cast<size_t>(sw) * static_cast<size_t>(sh) * 4);

    // MANDATORY: deselect bitmap BEFORE GetDIBits.
    // WinAPI does not allow GetDIBits on a bitmap selected into any DC.
    SelectObject(hdcFinal, oldFinal);

    int rows = GetDIBits(hdcFinal, hbmpFinal, 0, static_cast<UINT>(sh),
                         pixels.data(), &bmi, DIB_RGB_COLORS);
    // Check rows == sh, not rows != 0: a partial return (0 < rows < sh) means
    // incomplete capture — the PNG would encode a partially uninitialised buffer.
    if (rows != sh) {
        DeleteObject(hbmpFinal);
        DeleteDC(hdcFinal);
        ReleaseDC(nullptr, hScreenDC);
        addin_base_->AddError(1, L"ScreenCapture", L"GetDIBits failed", 0);
        return false;
    }

    // Cleanup (SelectObject already done above)
    DeleteObject(hbmpFinal);
    DeleteDC(hdcFinal);
    ReleaseDC(nullptr, hScreenDC);

    // ---- Step 9: swap B<->R → RGBA (miniz expects RGBA) ----
    for (size_t i = 0; i < pixels.size(); i += 4) {
        std::swap(pixels[i], pixels[i + 2]);  // B <-> R
        pixels[i + 3] = 0xFF;                 // alpha = opaque
    }

    // ---- Step 10: encode PNG ----
    size_t pngLen = 0;
    void* pngBuf = tdefl_write_image_to_png_file_in_memory(
        pixels.data(), sw, sh, 4, &pngLen);
    if (!pngBuf) {
        addin_base_->AddError(1, L"ScreenCapture", L"PNG encoding failed", 0);
        return false;
    }

    std::vector<uint8_t> rawPng(
        static_cast<uint8_t*>(pngBuf),
        static_cast<uint8_t*>(pngBuf) + pngLen);
    mz_free(pngBuf);  // miniz allocates via mz_malloc — must free

    // ---- Step 11: base64 encode ----
    // Format (show_grid=false): "left|top|width|height|<base64>"          — unchanged
    // Format (show_grid=true):  "left|top|width|height|gridX|gridY|<base64>"
    //   gridX/gridY are comma-separated original-pixel coords of drawn lines (may be empty
    //   if the image is too small for lines on that axis).
    // '|' never appears in base64 (A-Za-z0-9+/=), so the separator is unambiguous.
    // w and h are the actual window dimensions regardless of scale_percent.
    std::string b64 = Base64Encode(rawPng);
    outB64 = std::to_string(coordLeft) + "|"
           + std::to_string(coordTop)  + "|"
           + std::to_string(w)         + "|"
           + std::to_string(h);
    if (showGrid) {
        outB64 += "|" + gridXStr + "|" + gridYStr;
    }
    outB64 += "|" + b64;
    return true;
}

void ScreenCaptureComponent::DrawGrid(HDC hdc, int sw, int sh, int origW, int origH, int scale, int cols, int rows, int offsetX, int offsetY) {
    HPEN    hPen    = CreatePen(PS_SOLID, 1, RGB(255, 80, 80));
    HGDIOBJ oldPen  = SelectObject(hdc, hPen);

    int side     = std::max(sw, sh);
    int fontSize = std::max(8, 11 + 11 * (side - 1000) / 2000);
    HFONT   hFont   = CreateFontA(-fontSize, 0, 0, 0, FW_NORMAL,
                        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, "Arial");
    HGDIOBJ oldFont = SelectObject(hdc, hFont ? (HGDIOBJ)hFont : GetStockObject(DEFAULT_GUI_FONT));

    SetBkMode(hdc, OPAQUE);
    SetBkColor(hdc, RGB(0, 0, 0));
    SetTextColor(hdc, RGB(255, 255, 100));

    char buf[16];

    // Vertical lines + original x-coordinate labels at top
    for (int i = 1; i < cols; ++i) {
        int x     = sw * i / cols;
        int origX = offsetX + origW * i / cols;  // label shows original client-area coordinate
        MoveToEx(hdc, x, 0, nullptr);
        LineTo(hdc, x, sh);
        wsprintfA(buf, "%d", origX);
        TextOutA(hdc, x + 2, 2, buf, lstrlenA(buf));
    }

    // Horizontal lines + original y-coordinate labels at left
    for (int i = 1; i < rows; ++i) {
        int y     = sh * i / rows;
        int origY = offsetY + origH * i / rows;  // label shows original client-area coordinate
        MoveToEx(hdc, 0, y, nullptr);
        LineTo(hdc, sw, y);
        wsprintfA(buf, "%d", origY);
        TextOutA(hdc, 2, y + 2, buf, lstrlenA(buf));
    }

    // Dots at intersections — filled circle centered on intersection pixel
    HBRUSH  hBrush   = CreateSolidBrush(RGB(255, 80, 80));
    HGDIOBJ oldBrush = SelectObject(hdc, hBrush);
    SelectObject(hdc, GetStockObject(NULL_PEN));  // no border on ellipse
    for (int i = 1; i < cols; ++i) {
        int x = sw * i / cols;
        for (int j = 1; j < rows; ++j) {
            int y = sh * j / rows;
            Ellipse(hdc, x - 3, y - 3, x + 4, y + 4);
        }
    }
    SelectObject(hdc, oldBrush);
    DeleteObject(hBrush);

    SelectObject(hdc, oldFont);
    if (hFont) DeleteObject(hFont);
    SelectObject(hdc, oldPen);
    DeleteObject(hPen);
}

bool ScreenCaptureComponent::DrawHighlightRect(HDC hdc, int sw, int sh, int origW, int origH,
                                               const HighlightRect& r, int label,
                                               int offsetX, int offsetY) {
    // Coordinates relative to the captured area
    int relX = r.x - offsetX;
    int relY = r.y - offsetY;

    // Out-of-bounds check in original space
    if (relX < 0 || relY < 0 || relX + r.w > origW || relY + r.h > origH) {
        return false;
    }

    // Map to scaled space (endpoint mapping, not independent floor)
    int rx     = relX * sw / origW;
    int ry     = relY * sh / origH;
    int rx_end = (relX + r.w) * sw / origW;
    int ry_end = (relY + r.h) * sh / origH;
    int rw = std::max(1, rx_end - rx);
    int rh = std::max(1, ry_end - ry);

    // Font — same size formula as DrawGrid
    int side     = std::max(sw, sh);
    int fontSize = std::max(8, 11 + 11 * (side - 1000) / 2000);
    HFONT hFont  = CreateFontA(-fontSize, 0, 0, 0, FW_NORMAL,
                               FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, "Arial");
    HGDIOBJ oldFont = SelectObject(hdc, hFont ? (HGDIOBJ)hFont : GetStockObject(DEFAULT_GUI_FONT));

    // Draw blue rectangle (3 px, NULL_BRUSH = no fill)
    HPEN    hPen     = CreatePen(PS_SOLID, 3, RGB(0, 120, 255));
    HGDIOBJ oldPen   = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rx, ry, rx + rw, ry + rh);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hPen);

    // Label position: outside above top-right corner; inside if no space above
    char buf[16];
    wsprintfA(buf, "%d", label);
    SIZE sz;
    GetTextExtentPoint32A(hdc, buf, lstrlenA(buf), &sz);

    int tx = rx + rw - sz.cx - 2;
    tx = std::max(0, tx);                          // clamp: don't go past left edge
    int ty = (ry >= sz.cy + 4)
             ? ry - sz.cy - 2                      // outside above
             : ry + 2;                             // inside (not enough space above)
    ty = std::max(0, std::min(ty, sh - (int)sz.cy));  // clamp: stay within bitmap

    SetBkMode(hdc, OPAQUE);
    SetBkColor(hdc, RGB(0, 0, 0));
    SetTextColor(hdc, RGB(255, 255, 100));
    TextOutA(hdc, tx, ty, buf, lstrlenA(buf));

    SelectObject(hdc, oldFont);
    if (hFont) DeleteObject(hFont);

    return true;
}

#elif !defined(__linux__) // unsupported non-Windows platforms

bool ScreenCaptureComponent::CaptureMainWindow(int /*scale*/, bool /*showGrid*/, std::string& outB64,
                                                int /*regionX*/, int /*regionY*/,
                                                int /*regionW*/, int /*regionH*/,
                                                const std::string& /*highlightRects*/) {
    outB64.clear();
    addin_base_->AddError(1, L"ScreenCapture", L"Not supported on this platform", 0);
    return false;
}

#endif // _WINDOWS / __linux__

// ============================================================================
//  Helpers: 1C variant
// ============================================================================

bool ScreenCaptureComponent::SetStringToVariant(tVariant* var, const std::string& utf8) {
    if (!var || !mem_manager_) return false;
    std::wstring ws = Utf8ToWstr(utf8);
    TV_VT(var) = VTYPE_PWSTR;
    size_t byte_count = (ws.size() + 1) * sizeof(WCHAR_T);
    if (!mem_manager_->AllocMemory(reinterpret_cast<void**>(&var->pwstrVal),
                                    static_cast<unsigned long>(byte_count)))
        return false;
#ifdef _WINDOWS
    memcpy(var->pwstrVal, ws.c_str(), byte_count);
#else
    for (size_t i = 0; i <= ws.size(); ++i)
        var->pwstrVal[i] = static_cast<WCHAR_T>(ws[i]);
#endif
    var->wstrLen = static_cast<uint32_t>(ws.size());
    return true;
}

std::string ScreenCaptureComponent::GetStringFromVariant(const tVariant* var) {
    if (!var) return "";
    if (TV_VT(var) == VTYPE_PWSTR && var->pwstrVal) {
#ifdef _WINDOWS
        std::wstring ws(var->pwstrVal, var->wstrLen);
        return WstrToUtf8(ws);
#else
        std::wstring ws;
        ws.reserve(var->wstrLen);
        for (uint32_t i = 0; i < var->wstrLen; ++i)
            ws += static_cast<wchar_t>(var->pwstrVal[i]);
        return WstrToUtf8(ws);
#endif
    }
    if (TV_VT(var) == VTYPE_PSTR && var->pstrVal) {
        return std::string(var->pstrVal, var->strLen);
    }
    return "";
}

bool ScreenCaptureComponent::GetBoolFromVariant(const tVariant* var) {
    if (!var) return false;
    if (TV_VT(var) == VTYPE_BOOL) return var->bVal;
    return GetIntFromVariant(var, 0) != 0;
}

int ScreenCaptureComponent::GetIntFromVariant(const tVariant* var, int default_val) {
    if (!var) return default_val;
    if (TV_VT(var) == VTYPE_I4)  return static_cast<int>(var->lVal);
    if (TV_VT(var) == VTYPE_I2)  return static_cast<int>(var->shortVal);
    if (TV_VT(var) == VTYPE_R4)  return static_cast<int>(var->fltVal);
    if (TV_VT(var) == VTYPE_R8)  return static_cast<int>(var->dblVal);
    if (TV_VT(var) == VTYPE_PWSTR && var->pwstrVal && var->wstrLen > 0) {
        try {
            return std::stoi(WcharTToWstr(var->pwstrVal, var->wstrLen));
        } catch (...) {}
    }
    return default_val;
}

bool ScreenCaptureComponent::AllocWStr(WCHAR_T** dest, const std::wstring& src) {
    if (!dest || !mem_manager_) return false;
    size_t byte_count = (src.size() + 1) * sizeof(WCHAR_T);
    if (!mem_manager_->AllocMemory(reinterpret_cast<void**>(dest),
                                    static_cast<unsigned long>(byte_count)))
        return false;
#ifdef _WINDOWS
    memcpy(*dest, src.c_str(), byte_count);
#else
    for (size_t i = 0; i <= src.size(); ++i)
        (*dest)[i] = static_cast<WCHAR_T>(src[i]);
#endif
    return true;
}

// ============================================================================
//  UTF-8 <-> wstring
// ============================================================================

std::string ScreenCaptureComponent::WstrToUtf8(const std::wstring& ws) {
#ifdef _WINDOWS
    if (ws.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(),
                                  static_cast<int>(ws.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (sz <= 0) return "";
    std::string out(static_cast<size_t>(sz), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
                        out.data(), sz, nullptr, nullptr);
    return out;
#else
    std::string out;
    for (wchar_t wc : ws) {
        uint32_t cp = static_cast<uint32_t>(wc);
        if (cp < 0x80) { out += static_cast<char>(cp); }
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
#endif
}

std::wstring ScreenCaptureComponent::Utf8ToWstr(const std::string& s) {
#ifdef _WINDOWS
    if (s.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()),
                                  nullptr, 0);
    if (sz <= 0) return L"";
    std::wstring out(static_cast<size_t>(sz), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), sz);
    return out;
#else
    std::wstring out;
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        uint8_t b0 = (uint8_t)s[i];
        uint32_t cp = 0; size_t seq = 1;
        if      (b0 < 0x80)                                    { cp = b0; seq = 1; }
        else if ((b0 & 0xE0) == 0xC0 && i+1 < n)              { cp = ((b0&0x1F)<<6)|((uint8_t)s[i+1]&0x3F); seq = 2; }
        else if ((b0 & 0xF0) == 0xE0 && i+2 < n)              { cp = ((b0&0x0F)<<12)|(((uint8_t)s[i+1]&0x3F)<<6)|((uint8_t)s[i+2]&0x3F); seq = 3; }
        else if ((b0 & 0xF8) == 0xF0 && i+3 < n)              { cp = ((b0&0x07)<<18)|(((uint8_t)s[i+1]&0x3F)<<12)|(((uint8_t)s[i+2]&0x3F)<<6)|((uint8_t)s[i+3]&0x3F); seq = 4; }
        else                                                    { cp = b0; seq = 1; }
        out += static_cast<wchar_t>(cp);
        i += seq;
    }
    return out;
#endif
}

// ============================================================================
//  Base64 encoding
// ============================================================================

std::string ScreenCaptureComponent::Base64Encode(const std::vector<uint8_t>& data) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = (static_cast<uint32_t>(data[i])   << 16) |
                     (static_cast<uint32_t>(data[i+1]) <<  8) |
                      static_cast<uint32_t>(data[i+2]);
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += kTable[(v >>  6) & 0x3F];
        out += kTable[(v      ) & 0x3F];
    }
    if (i + 1 == data.size()) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += '=';
        out += '=';
    } else if (i + 2 == data.size()) {
        uint32_t v = (static_cast<uint32_t>(data[i])   << 16) |
                     (static_cast<uint32_t>(data[i+1]) <<  8);
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += kTable[(v >>  6) & 0x3F];
        out += '=';
    }
    return out;
}

} // namespace screen_capture
