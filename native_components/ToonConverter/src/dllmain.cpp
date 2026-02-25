#include "component.h"
#include <new>

#ifdef _WINDOWS
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}
#endif

static const WCHAR_T kClassName[] =
#ifdef _WINDOWS
    L"ToonConverter";
#else
    { 'T','o','o','n','C','o','n','v','e','r','t','e','r', 0 };
#endif

static AppCapabilities g_capabilities = eAppCapabilitiesInvalid;

extern "C" const WCHAR_T* GetClassNames() {
    return kClassName;
}

extern "C" long GetClassObject(const WCHAR_T* /*wsName*/, IComponentBase** pIntf) {
    if (!pIntf) return 0;
    *pIntf = new (std::nothrow) toon::ToonConverterComponent();
    return (*pIntf) ? 1 : 0;
}

extern "C" long DestroyObject(IComponentBase** pIntf) {
    if (!pIntf || !*pIntf) return -1;
    delete *pIntf;
    *pIntf = nullptr;
    return 0;
}

extern "C" AppCapabilities SetPlatformCapabilities(const AppCapabilities capabilities) {
    g_capabilities = capabilities;
    return eAppCapabilitiesLast;
}
