#pragma once

#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"

#include <string>
#include <vector>

#ifdef _WINDOWS
#include <windows.h>
#endif

namespace screen_capture {

class ScreenCaptureComponent : public IComponentBase {
public:
    ScreenCaptureComponent() = default;
    ~ScreenCaptureComponent() override = default;

    // IInitDoneBase
    bool ADDIN_API Init(void* disp) override;
    bool ADDIN_API setMemManager(void* mem) override;
    long ADDIN_API GetInfo() override { return 2000; }
    void ADDIN_API Done() override;

    // ILanguageExtenderBase
    bool ADDIN_API RegisterExtensionAs(WCHAR_T** wsExtensionName) override;

    long ADDIN_API GetNProps() override { return 0; }
    long ADDIN_API FindProp(const WCHAR_T*) override { return -1; }
    const WCHAR_T* ADDIN_API GetPropName(long, long) override { return nullptr; }
    bool ADDIN_API GetPropVal(const long, tVariant*) override { return false; }
    bool ADDIN_API SetPropVal(const long, tVariant*) override { return false; }
    bool ADDIN_API IsPropReadable(const long) override { return false; }
    bool ADDIN_API IsPropWritable(const long) override { return false; }

    long ADDIN_API GetNMethods() override;
    long ADDIN_API FindMethod(const WCHAR_T* wsMethodName) override;
    const WCHAR_T* ADDIN_API GetMethodName(const long lMethodNum, const long lMethodAlias) override;
    long ADDIN_API GetNParams(const long lMethodNum) override;
    bool ADDIN_API GetParamDefValue(const long lMethodNum, const long lParamNum, tVariant* pDefVal) override;
    bool ADDIN_API HasRetVal(const long lMethodNum) override;
    bool ADDIN_API CallAsProc(const long, tVariant*, const long) override { return false; }
    bool ADDIN_API CallAsFunc(const long lMethodNum, tVariant* pvarRetValue,
                               tVariant* paParams, const long lSizeArray) override;

    void ADDIN_API SetLocale(const WCHAR_T*) override {}

private:
    enum Methods {
        eMethodCaptureWindow = 0,  // CaptureWindow(scale_percent) → String (base64 PNG)
        eMethodLast
    };

    // Window capture. The anchor is the largest visible UNOWNED top-level window of the process —
    // the main frame — and every other visible top-level window of the process that lies above it
    // in Z-order is composited on top of the capture. That is what puts 1C's choice lists, menus,
    // tooltips and dialogs into the picture: they are separate top-level windows, and PrintWindow
    // never renders those. Foreground → GA_ROOTOWNER remains only as a fallback for a process with
    // no unowned visible window.
    // Returns true in all "expected" cases (window not found / not ready yet).
    // outB64 is empty → BSL retries.
    // Returns false only on unrecoverable GDI/PNG error (AddError already called).
    bool CaptureMainWindow(int scale, bool showGrid, std::string& outB64,
                           int regionX = -1, int regionY = -1,
                           int regionW = -1, int regionH = -1,
                           const std::string& highlightRects = "");

#ifdef _WINDOWS
    // sw, sh   — scaled image dimensions (for line positions)
    // origW, origH — original window dimensions (for coordinate labels)
    // scale    — scale_percent (used to keep label font visually constant size on screen)
    void DrawGrid(HDC hdc, int sw, int sh, int origW, int origH, int scale,
                  int cols = 10, int rows = 10, int offsetX = 0, int offsetY = 0);

    struct HighlightRect { int x, y, w, h; };

    // Returns true if rectangle was drawn, false if it is outside the captured area.
    bool DrawHighlightRect(HDC hdc, int sw, int sh, int origW, int origH,
                           const HighlightRect& r, int label,
                           int offsetX, int offsetY);
#endif

    // 1C string/variant helpers
    bool SetStringToVariant(tVariant* var, const std::string& utf8);
    std::string GetStringFromVariant(const tVariant* var);
    int GetIntFromVariant(const tVariant* var, int default_val);
    bool GetBoolFromVariant(const tVariant* var);
    bool AllocWStr(WCHAR_T** dest, const std::wstring& src);

    // UTF-8 <-> wstring helpers
    static std::string WstrToUtf8(const std::wstring& ws);
    static std::wstring Utf8ToWstr(const std::string& s);

    // Base64 encoding
    static std::string Base64Encode(const std::vector<uint8_t>& data);

    IAddInDefBase*  addin_base_  = nullptr;
    IMemoryManager* mem_manager_ = nullptr;

    static const wchar_t* method_names_en_[];
    static const wchar_t* method_names_ru_[];
};

} // namespace screen_capture
