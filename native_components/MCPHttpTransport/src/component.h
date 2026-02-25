#pragma once

#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"
#include "http_transport.h"

#include <string>
#include <vector>

namespace mcp {

class MCPHttpTransportComponent : public IComponentBase {
public:
    MCPHttpTransportComponent();
    ~MCPHttpTransportComponent() override;

    // IInitDoneBase
    bool ADDIN_API Init(void* disp) override;
    bool ADDIN_API setMemManager(void* mem) override;
    long ADDIN_API GetInfo() override;
    void ADDIN_API Done() override;

    // ILanguageExtenderBase
    bool ADDIN_API RegisterExtensionAs(WCHAR_T** wsExtensionName) override;

    long ADDIN_API GetNProps() override;
    long ADDIN_API FindProp(const WCHAR_T* wsPropName) override;
    const WCHAR_T* ADDIN_API GetPropName(long lPropNum, long lPropAlias) override;
    bool ADDIN_API GetPropVal(const long lPropNum, tVariant* pvarPropVal) override;
    bool ADDIN_API SetPropVal(const long lPropNum, tVariant* varPropVal) override;
    bool ADDIN_API IsPropReadable(const long lPropNum) override;
    bool ADDIN_API IsPropWritable(const long lPropNum) override;

    long ADDIN_API GetNMethods() override;
    long ADDIN_API FindMethod(const WCHAR_T* wsMethodName) override;
    const WCHAR_T* ADDIN_API GetMethodName(const long lMethodNum, const long lMethodAlias) override;
    long ADDIN_API GetNParams(const long lMethodNum) override;
    bool ADDIN_API GetParamDefValue(const long lMethodNum, const long lParamNum,
                                     tVariant* pvarParamDefValue) override;
    bool ADDIN_API HasRetVal(const long lMethodNum) override;
    bool ADDIN_API CallAsProc(const long lMethodNum, tVariant* paParams,
                               const long lSizeArray) override;
    bool ADDIN_API CallAsFunc(const long lMethodNum, tVariant* pvarRetValue,
                               tVariant* paParams, const long lSizeArray) override;

    // LocaleBase
    void ADDIN_API SetLocale(const WCHAR_T* loc) override;

private:
    // Property indices
    enum Props {
        ePropIsRunning = 0,
        ePropPort,
        ePropRequestTimeout,
        ePropMaxConcurrentRequests,
        ePropLast
    };

    // Method indices
    enum Methods {
        eMethodStart = 0,
        eMethodStop,
        eMethodSendResponse,
        eMethodSendSSEEvent,
        eMethodCloseSSEStream,
        eMethodGetRequestBody,
        eMethodLast
    };

    // Helpers
    bool SetWStringToVariant(tVariant* var, const std::wstring& str);
    std::wstring GetWStringFromVariant(const tVariant* var);
    std::string WStringToUTF8(const std::wstring& wstr);
    std::wstring UTF8ToWString(const std::string& str);

    // WCHAR_T helpers (1C uses uint16_t on non-Windows, wchar_t on Windows)
    std::wstring FromWCHAR_T(const WCHAR_T* src, size_t len = 0);
    bool AllocWCHAR_T(WCHAR_T** dest, const std::wstring& src);

    // Fire ExternalEvent to 1C (thread-safe, called from HTTP thread)
    bool FireExternalEvent(const std::string& source, const std::string& event,
                           const std::string& data);

    // Protected by addin_mutex_ for thread-safe access from HTTP threads
    std::mutex addin_mutex_;
    IAddInDefBase* addin_base_ = nullptr;
    IMemoryManager* mem_manager_ = nullptr;
    HttpTransport transport_;

    // Property/method name tables
    static const wchar_t* prop_names_en_[];
    static const wchar_t* prop_names_ru_[];
    static const wchar_t* method_names_en_[];
    static const wchar_t* method_names_ru_[];
};

} // namespace mcp
