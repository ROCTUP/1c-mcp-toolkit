#include "component.h"

#include "overlay_selection.h"

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

// The Native API uses UTF-16 on Linux too, where wchar_t is 32 bits.
static std::wstring NativeToWide(const WCHAR_T* source, size_t length) {
    if (!source) return {};
#ifdef _WINDOWS
    return std::wstring(source, length);
#else
    std::wstring result;
    result.reserve(length);
    for (size_t index = 0; index < length; ++index) {
        uint32_t code = source[index];
        if (code >= 0xD800 && code <= 0xDBFF && index + 1 < length &&
            source[index + 1] >= 0xDC00 && source[index + 1] <= 0xDFFF) {
            code = 0x10000 + ((code - 0xD800) << 10) + source[++index] - 0xDC00;
        } else if (code >= 0xD800 && code <= 0xDFFF) {
            code = 0xFFFD;
        }
        result += static_cast<wchar_t>(code);
    }
    return result;
#endif
}

#ifndef _WINDOWS
static std::vector<WCHAR_T> WideToNative(const std::wstring& source) {
    std::vector<WCHAR_T> result;
    result.reserve(source.size() + 1);
    for (wchar_t character : source) {
        uint32_t code = static_cast<uint32_t>(character);
        if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) code = 0xFFFD;
        if (code >= 0x10000) {
            code -= 0x10000;
            result.push_back(static_cast<WCHAR_T>(0xD800 + (code >> 10)));
            result.push_back(static_cast<WCHAR_T>(0xDC00 + (code & 0x3FF)));
        } else {
            result.push_back(static_cast<WCHAR_T>(code));
        }
    }
    result.push_back(0);
    return result;
}
#endif

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
    if (!wsMethodName) return -1;
    size_t length = 0;
    while (wsMethodName[length]) ++length;
    std::wstring name = NativeToWide(wsMethodName, length);
    auto lower = [](std::wstring s) {
#ifdef _WINDOWS
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
#else
        // The hosting process may use the C locale. Method lookup must still be
        // case-insensitive for the Russian Native API alias.
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
            if (c >= L'\u0410' && c <= L'\u042F') return wchar_t(c + 0x20);
            if (c == L'\u0401') return L'\u0451';
            return wchar_t(::towlower(c));
        });
#endif
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

// Anchor search: the largest visible UNOWNED top-level window of the process.
//
// Unowned matters. 1C keeps several unowned auxiliary windows around — a notification
// (V8ConfirmationWindowTaxi, 255x85), a confirmation (V8StateDlg), a validation message
// (V8ValidationMessageWnd, 238x75). Their root owner is themselves, so a foreground-based anchor
// would return the 255x85 dialog and the screenshot would lose the form behind it. Nothing is lost
// by anchoring on the main frame instead: everything else is composited on top of it.
struct FindMainData { DWORD pid; HWND best; int bestArea; };

static BOOL CALLBACK EnumMainProc(HWND hwnd, LPARAM lp) {
    auto* d = reinterpret_cast<FindMainData*>(lp);
    if (!IsOurWindow(hwnd, d->pid)) return TRUE;
    if (GetParent(hwnd) != NULL) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;
    RECT rc; GetClientRect(hwnd, &rc);
    int area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > d->bestArea) { d->bestArea = area; d->best = hwnd; }
    return TRUE;
}

// ---------------------------------------------------------------------------
//  Overlay collection and painting
// ---------------------------------------------------------------------------

// Every getter here reads state the window manager already holds and sends no messages, so the
// collection itself can never block on a busy 1C.
struct CollectData {
    DWORD pid;
    HWND  anchor;
    int   anchorZ;
    int   index;
    std::vector<overlay::OverlayCandidate>* out;
};

static BOOL CALLBACK EnumCollectProc(HWND hwnd, LPARAM lp) {
    auto* d = reinterpret_cast<CollectData*>(lp);
    DWORD wpid = 0;
    DWORD wtid = GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != d->pid) return TRUE;

    overlay::OverlayCandidate c;
    c.hwnd = reinterpret_cast<unsigned long long>(hwnd);
    c.pid = wpid;
    c.tid = wtid;
    c.visible = IsWindowVisible(hwnd) != FALSE;
    RECT wr = {};
    GetWindowRect(hwnd, &wr);
    c.left = wr.left;
    c.top = wr.top;
    c.width = wr.right - wr.left;
    c.height = wr.bottom - wr.top;
    // EnumWindows walks top-level windows in Z-order, topmost first, so the running index over our
    // own windows preserves that order.
    c.z = d->index++;
    if (hwnd == d->anchor) d->anchorZ = c.z;
    d->out->push_back(c);
    return TRUE;
}

// pid is stored on every candidate and is NOT treated as already checked: the early filter here is
// an optimisation, the guarantee is carried by SelectOverlays.
static std::vector<overlay::OverlayCandidate> CollectWindows(DWORD pid, HWND anchor, int* anchorZ) {
    std::vector<overlay::OverlayCandidate> out;
    CollectData d { pid, anchor, -1, 0, &out };
    EnumWindows(EnumCollectProc, reinterpret_cast<LPARAM>(&d));
    if (anchorZ) *anchorZ = d.anchorZ;
    return out;
}

// One layer that made it into the frame, as reported back to the agent.
struct OverlayReport {
    unsigned long long hwnd = 0;
    int  x = 0, y = 0, w = 0, h = 0;
    bool clipped = false;
    overlay::OverlayBackend source = overlay::OverlayBackend::PrintWindow;
    std::wstring cls;
};

// A layer that was still on screen and could not be captured by any permitted backend. Without the
// list, capture_complete = false would not say which layer went missing.
struct OverlayFailure {
    unsigned long long hwnd = 0;
    int  x = 0, y = 0, w = 0, h = 0;  // тот же клиентский прямоугольник, что у работы
    const char* reason = "";
};

// What the overlay pass ended with. It cannot be void: it needs the frame for the screen-source
// gate, and it has to be able to say "the layer is visible but was not captured" — otherwise the
// pipeline would go on to GetDIBits and return an ordinary successful PNG without the layer, which
// is exactly the untrustworthy frame this whole change exists to remove.
struct OverlayPaintResult {
    // false only while the screen source is forbidden: then a layer that cannot be captured has
    // nowhere to be reported, so the whole capture fails loudly instead of lying.
    bool ok = true;
    unsigned long long failedHwnd = 0;
    std::vector<OverlayReport>  painted;
    std::vector<OverlayFailure> failures;
};

// A uniform bitmap is the classic DirectComposition symptom: PrintWindow reports success and
// renders nothing. 32 sampled points are microseconds on a 356x182 window.
static bool BitmapLooksBlank(HDC dc, int w, int h) {
    COLORREF first = GetPixel(dc, 0, 0);
    if (first == CLR_INVALID) return false;
    for (int i = 0; i < 32; ++i) {
        int x = (w - 1) * (i % 8) / 7;
        int y = (h - 1) * (i / 8) / 3;
        if (GetPixel(dc, x, y) != first) return false;
    }
    return true;
}

// PW_RENDERFULLCONTENT — no PW_CLIENTONLY here: the layer's frame is part of what a human sees, and
// GetWindowRect includes it.
static OverlayPaintResult PaintOverlays(HDC dst, HDC screen, const overlay::CaptureFrame& frame,
                                        const std::vector<overlay::OverlayPaint>& jobs) {
    OverlayPaintResult result;
    result.painted.reserve(jobs.size());

    for (size_t i = 0; i < jobs.size(); ++i) {
        const overlay::OverlayPaint& job = jobs[i];
        HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(job.hwnd));

        // The window may have gone between enumeration and painting. Not a defect: it is not on
        // screen either, so the job is simply dropped.
        if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) continue;

        HDC tmp = CreateCompatibleDC(screen);
        if (!tmp) { result.ok = false; result.failedHwnd = job.hwnd; return result; }

        HBITMAP bmp = CreateCompatibleBitmap(screen, job.w, job.h);
        if (!bmp) {
            DeleteDC(tmp);
            result.ok = false; result.failedHwnd = job.hwnd; return result;
        }

        HGDIOBJ oldBmp = SelectObject(tmp, bmp);
        if (!oldBmp || oldBmp == HGDI_ERROR) {
            DeleteObject(bmp);
            DeleteDC(tmp);
            result.ok = false; result.failedHwnd = job.hwnd; return result;
        }

        overlay::OverlayBackend used = job.backend;
        bool drawn = false;
        if (job.backend == overlay::OverlayBackend::PrintWindow) {
            drawn = PrintWindow(hwnd, tmp, 0x2) != FALSE;  // PW_RENDERFULLCONTENT
            if (drawn && BitmapLooksBlank(tmp, job.w, job.h)) drawn = false;
        }
        if (!drawn && frame.screenSourceAllowed) {
            drawn = BitBlt(tmp, 0, 0, job.w, job.h, screen, job.srcLeft, job.srcTop, SRCCOPY) != FALSE;
            if (drawn) used = overlay::OverlayBackend::Screen;
        }

        if (drawn) {
            // GDI clips to the destination surface, so negative dstX/dstY and an overhang past the
            // right or bottom edge are safe; job.clipped is bookkeeping for the report, not a guard.
            drawn = BitBlt(dst, job.dstX, job.dstY, job.w, job.h, tmp, 0, 0, SRCCOPY) != FALSE;
        }

        SelectObject(tmp, oldBmp);
        DeleteObject(bmp);
        DeleteDC(tmp);

        if (drawn) {
            OverlayReport rep;
            rep.hwnd = job.hwnd;
            rep.x = job.dstX;
            rep.y = job.dstY;
            rep.w = job.w;
            rep.h = job.h;
            rep.clipped = job.clipped;
            rep.source = used;
            wchar_t clsBuf[128] = {0};
            int clsLen = GetClassNameW(hwnd, clsBuf, 128);
            if (clsLen > 0) rep.cls.assign(clsBuf, static_cast<size_t>(clsLen));
            result.painted.push_back(rep);
            continue;
        }

        // Nothing was captured. If the window is gone by now, that is the race above and it is
        // fine — it is not on screen either.
        if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) continue;

        // Still visible. Dropping it silently would hand back a successful frame with a hole in it,
        // which is the exact defect this change removes. Where provenance can carry the fact, it is
        // reported and the frame is marked incomplete; where it cannot, the capture fails loudly.
        if (frame.screenSourceAllowed) {
            OverlayFailure f;
            f.hwnd = job.hwnd;
            f.x = job.dstX;
            f.y = job.dstY;
            f.w = job.w;
            f.h = job.h;
            f.reason = "backend_failed";
            result.failures.push_back(f);
            continue;
        }
        result.ok = false;
        result.failedHwnd = job.hwnd;
        return result;
    }

    return result;
}

// ---------------------------------------------------------------------------
//  Metadata envelope
// ---------------------------------------------------------------------------
// A flat JSON writer, forty-odd lines, instead of a dependency on
// native_components/UIAutomation/common/json.h: ScreenCapture is an independent binary with no ties
// to that tree, and coupling two separate components by source for a flat writer is not worth it.
//
// ASCII only — everything above 0x7F leaves as \uXXXX — so the character length of the JSON equals
// its byte length, and the length prefix in the protocol string is unambiguous.
static void JsonAppendString(std::string& out, const std::wstring& value) {
    out += '"';
    for (wchar_t ch : value) {
        switch (ch) {
            case L'"':  out += "\\\""; continue;
            case L'\\': out += "\\\\"; continue;
            case L'\n': out += "\\n";  continue;
            case L'\r': out += "\\r";  continue;
            case L'\t': out += "\\t";  continue;
            default: break;
        }
        unsigned code = static_cast<unsigned>(ch);
        if (code < 0x20 || code > 0x7E) {
            char esc[8];
            wsprintfA(esc, "\\u%04X", code & 0xFFFF);
            out += esc;
        } else {
            out += static_cast<char>(code);
        }
    }
    out += '"';
}

static const char* SourceName(overlay::OverlayBackend backend) {
    return backend == overlay::OverlayBackend::Screen ? "screen" : "print_window";
}

// Активное окно процесса и лежит ли оно в этом кадре. Якорь выбирается детерминированно — самое
// большое неowned окно, — и это верно для координат, но означает, что окно, которое сейчас в фокусе,
// может в кадр не попасть: например второй независимый фрейм, отведённый в сторону от главного.
// Молчать об этом нельзя, иначе агент примет снимок главного окна за снимок того, с чем работает.
struct ForegroundInfo {
    bool known = false;              // фокус принадлежит нашему процессу
    unsigned long long hwnd = 0;
    bool inFrame = false;            // это якорь или один из вклеенных слоёв
};

static std::string BuildMetadataJson(int coordLeft, int coordTop, int w, int h,
                                     bool showGrid,
                                     const std::string& gridXStr, const std::string& gridYStr,
                                     const OverlayPaintResult& overlays,
                                     const ForegroundInfo& foreground) {
    std::string json = "{";

    json += "\"window_rect\":{\"left\":" + std::to_string(coordLeft) +
            ",\"top\":" + std::to_string(coordTop) +
            ",\"width\":" + std::to_string(w) +
            ",\"height\":" + std::to_string(h) + "}";

    if (showGrid) {
        // gridXStr/gridYStr are already comma-separated decimal numbers, so they drop straight into
        // JSON arrays; empty means the image was too small for lines on that axis.
        json += ",\"grid_coords\":{\"grid_x\":[" + gridXStr + "],\"grid_y\":[" + gridYStr + "]}";
    }

    json += ",\"capture_complete\":";
    json += overlays.failures.empty() ? "true" : "false";

    json += ",\"overlays\":[";
    for (size_t i = 0; i < overlays.painted.size(); ++i) {
        const OverlayReport& o = overlays.painted[i];
        if (i) json += ',';
        json += "{\"hwnd\":" + std::to_string(o.hwnd) +
                ",\"x\":" + std::to_string(o.x) +
                ",\"y\":" + std::to_string(o.y) +
                ",\"width\":" + std::to_string(o.w) +
                ",\"height\":" + std::to_string(o.h) +
                ",\"clipped\":" + (o.clipped ? "true" : "false") +
                ",\"source\":\"" + SourceName(o.source) + "\"" +
                ",\"class_name\":";
        JsonAppendString(json, o.cls);
        json += '}';
    }
    json += "]";

    json += ",\"overlay_failures\":[";
    for (size_t i = 0; i < overlays.failures.size(); ++i) {
        if (i) json += ',';
        json += "{\"hwnd\":" + std::to_string(overlays.failures[i].hwnd) +
                ",\"reason\":\"" + overlays.failures[i].reason + "\"}";
    }
    json += "]";

    if (foreground.known) {
        json += ",\"foreground\":{\"hwnd\":" + std::to_string(foreground.hwnd) +
                ",\"in_frame\":" + (foreground.inFrame ? "true" : "false") + "}";
    }

    json += "}";
    return json;
}

bool ScreenCaptureComponent::CaptureMainWindow(int scale, bool showGrid, std::string& outB64,
                                                int regionX, int regionY,
                                                int regionW, int regionH,
                                                const std::string& highlightRects) {
    // Invariant: always clear first
    outB64.clear();

    DWORD pid = GetCurrentProcessId();
    HWND hwnd = nullptr;

    // Primary: the largest visible unowned top-level window — the main frame. Deterministic on
    // purpose: the agent gets the same coordinate frame between calls, which is what makes region
    // and highlight_rects reusable across a sequence of screenshots. This is the same notion of
    // "main window" the UI automation side publishes as isMain.
    {
        FindMainData fd { pid, nullptr, 0 };
        EnumWindows(EnumMainProc, reinterpret_cast<LPARAM>(&fd));
        hwnd = fd.best;
    }

    // Fallback: foreground window → root owner → PID check. Only reached when the process has no
    // unowned visible window at all.
    if (!hwnd) {
        HWND fg = GetForegroundWindow();
        if (fg) {
            HWND root = GetAncestor(fg, GA_ROOTOWNER);
            if (root && IsOurWindow(root, pid)) {
                hwnd = root;
            }
        }
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

    // ---- Step 6.2: composite the transient layers of this process ----
    // PrintWindow renders one HWND and its WS_CHILD children; owned top-level windows are not part
    // of it by design of the API. 1C draws its choice lists, menus, tooltips and dialogs as separate
    // top-level windows, so without this pass they can never appear in the picture.
    //
    // Runs before the crop so that everything downstream — crop, scale, grid, highlight — works on
    // the finished frame and needs no changes.
    OverlayPaintResult overlaysPainted;
    {
        int anchorZ = -1;
        std::vector<overlay::OverlayCandidate> candidates = CollectWindows(pid, hwnd, &anchorZ);

        overlay::CaptureFrame frame;
        frame.targetPid = pid;
        frame.selfTid = GetCurrentThreadId();
        frame.originX = coordLeft;
        frame.originY = coordTop;
        frame.width = w;
        frame.height = h;
        frame.anchorZ = (anchorZ >= 0) ? anchorZ : 0;
        // The screen is allowed as a pixel source because the answer can now say so: every layer
        // carries its source, and a frame with an uncaptured layer carries capture_complete=false
        // plus the failing handle. Without that contract this must stay false — an unannounced
        // screen pixel can contain another application's window.
        frame.screenSourceAllowed = true;

        std::vector<overlay::OverlayPaint> jobs = overlay::SelectOverlays(candidates, frame);
        overlaysPainted = PaintOverlays(hdcSrc, hScreenDC, frame, jobs);
        if (!overlaysPainted.ok) {
            SelectObject(hdcSrc, oldSrc);
            DeleteObject(hbmpSrc);
            DeleteDC(hdcSrc);
            ReleaseDC(nullptr, hScreenDC);
            outB64 = "ERROR:overlay:" + std::to_string(overlaysPainted.failedHwnd);
            return true;
        }
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

    // ---- Step 11: base64 encode and build the answer ----
    // Format: "V2|<json length in characters>|<json>|<base64>"
    //
    // The metadata used to travel as positional fields, and that stopped scaling exactly here: the
    // layer list carries a class name and a pixel source per entry, and every further field would
    // have needed its own separator and its own escaping rule. The length prefix removes all
    // separator ambiguity — read the number, take that many characters, skip one '|', the rest is
    // base64 — and the writer emits ASCII only, so characters and bytes agree.
    //
    // w and h inside window_rect are the actual window dimensions regardless of scale_percent.
    // Отчёт о слоях обязан описывать ТУ картинку, которая уезжает наружу. Композит шёл по полной
    // клиентской области, а region мог вырезать из неё кусок, в котором слоя нет вовсе — тогда
    // «слой на кадре» было бы неправдой, и ровно так же неправдой была бы неполнота кадра из-за
    // слоя, не попавшего в вырезку. Поэтому фильтруем по отданному прямоугольнику и пересчитываем
    // clipped относительно него же.
    const int frameX = doCrop ? regionX : 0;
    const int frameY = doCrop ? regionY : 0;
    const int frameW = doCrop ? regionW : w;
    const int frameH = doCrop ? regionH : h;

    OverlayPaintResult reported;
    reported.ok = overlaysPainted.ok;
    for (const OverlayReport& o : overlaysPainted.painted) {
        if (!overlay::RectsIntersect(o.x, o.y, o.w, o.h, frameX, frameY, frameW, frameH)) continue;
        OverlayReport r = o;
        r.clipped = !overlay::RectInside(o.x, o.y, o.w, o.h, frameX, frameY, frameW, frameH);
        reported.painted.push_back(r);
    }
    for (const OverlayFailure& f : overlaysPainted.failures) {
        if (!overlay::RectsIntersect(f.x, f.y, f.w, f.h, frameX, frameY, frameW, frameH)) continue;
        reported.failures.push_back(f);
    }

    // Где сейчас фокус и виден ли он в этом кадре.
    ForegroundInfo foreground;
    {
        // Именно GetForegroundWindow, без GA_ROOTOWNER. Владелец здесь не годится: у owned-диалога
        // root owner — само главное окно, и поле сообщало бы «активно главное окно, оно в кадре»
        // ровно тогда, когда активен диалог, которого в кадре нет. Это уничтожило бы различие,
        // ради которого поле и заведено. Окно переднего плана всегда top-level, так что подниматься
        // по цепочке владения не нужно.
        HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        if (fg && fgPid == pid) {
            foreground.known = true;
            foreground.hwnd = reinterpret_cast<unsigned long long>(fg);
            foreground.inFrame = (fg == hwnd);
            for (const OverlayReport& o : reported.painted) {
                if (o.hwnd == foreground.hwnd) { foreground.inFrame = true; break; }
            }
        }
    }

    std::string meta = BuildMetadataJson(coordLeft, coordTop, w, h,
                                         showGrid, gridXStr, gridYStr, reported, foreground);
    std::string b64 = Base64Encode(rawPng);
    outB64 = "V2|" + std::to_string(meta.size()) + "|" + meta + "|" + b64;
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

#elif !defined(__linux__) // unsupported platforms (rejected by CMake)

bool ScreenCaptureComponent::CaptureMainWindow(int /*scale*/, bool /*showGrid*/, std::string& outB64,
                                                int /*regionX*/, int /*regionY*/,
                                                int /*regionW*/, int /*regionH*/,
                                                const std::string& /*highlightRects*/) {
    outB64.clear();
    addin_base_->AddError(1, L"ScreenCapture", L"Not supported on this platform", 0);
    return false;
}

#endif // _WINDOWS

// ============================================================================
//  Helpers: 1C variant
// ============================================================================

bool ScreenCaptureComponent::SetStringToVariant(tVariant* var, const std::string& utf8) {
    if (!var || !mem_manager_) return false;
    std::wstring ws = Utf8ToWstr(utf8);
    TV_VT(var) = VTYPE_PWSTR;
#ifdef _WINDOWS
    size_t byte_count = (ws.size() + 1) * sizeof(WCHAR_T);
#else
    const auto native = WideToNative(ws);
    size_t byte_count = native.size() * sizeof(WCHAR_T);
#endif
    if (!mem_manager_->AllocMemory(reinterpret_cast<void**>(&var->pwstrVal),
                                    static_cast<unsigned long>(byte_count)))
        return false;
#ifdef _WINDOWS
    memcpy(var->pwstrVal, ws.c_str(), byte_count);
#else
    memcpy(var->pwstrVal, native.data(), byte_count);
#endif
    var->wstrLen = static_cast<uint32_t>(byte_count / sizeof(WCHAR_T) - 1);
    return true;
}

std::string ScreenCaptureComponent::GetStringFromVariant(const tVariant* var) {
    if (!var) return "";
    if (TV_VT(var) == VTYPE_PWSTR && var->pwstrVal) {
#ifdef _WINDOWS
        std::wstring ws(var->pwstrVal, var->wstrLen);
        return WstrToUtf8(ws);
#else
        std::wstring ws = NativeToWide(var->pwstrVal, var->wstrLen);
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
            return std::stoi(NativeToWide(var->pwstrVal, var->wstrLen));
        } catch (...) {}
    }
    return default_val;
}

bool ScreenCaptureComponent::AllocWStr(WCHAR_T** dest, const std::wstring& src) {
    if (!dest || !mem_manager_) return false;
#ifdef _WINDOWS
    size_t byte_count = (src.size() + 1) * sizeof(WCHAR_T);
#else
    const auto native = WideToNative(src);
    size_t byte_count = native.size() * sizeof(WCHAR_T);
#endif
    if (!mem_manager_->AllocMemory(reinterpret_cast<void**>(dest),
                                    static_cast<unsigned long>(byte_count)))
        return false;
#ifdef _WINDOWS
    memcpy(*dest, src.c_str(), byte_count);
#else
    memcpy(*dest, native.data(), byte_count);
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
