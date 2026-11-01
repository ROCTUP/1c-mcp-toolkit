#pragma once

#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"

#include "hbk.h"
#include "search_executor.h"

#include <string>

namespace syntax_help {

class SyntaxHelpReaderComponent : public IComponentBase {
public:
    SyntaxHelpReaderComponent() = default;
    ~SyntaxHelpReaderComponent() override;

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
    bool ADDIN_API GetParamDefValue(const long, const long, tVariant*) override { return false; }
    bool ADDIN_API HasRetVal(const long lMethodNum) override;
    bool ADDIN_API CallAsProc(const long, tVariant*, const long) override { return false; }
    bool ADDIN_API CallAsFunc(const long lMethodNum, tVariant* pvarRetValue,
                               tVariant* paParams, const long lSizeArray) override;

    void ADDIN_API SetLocale(const WCHAR_T*) override {}

private:
    enum Methods {
        eMethodInitialize = 0,  // Инициализировать(dir: String) → Integer
        eMethodSearch,          // Поиск(keywords_json: String, match_all: Bool) → String (JSON)
        eMethodGetTopic,        // ПолучитьТему(breadcrumb: String) → String (JSON)
        eMethodVersion,         // Версия() → String
        eMethodGetCapabilities,
        eMethodStartSearchExecutor,
        eMethodSubmitSearch,
        eMethodGetSearchStates,
        eMethodReadSearchResult,
        eMethodAcknowledgeSearch,
        eMethodCancelSearch,
        eMethodBeginShutdown,
        eMethodGetShutdownState,
        eMethodLast
    };

    // 1C string helpers
    bool SetStringToVariant(tVariant* var, const std::string& utf8);
    bool SetIntToVariant(tVariant* var, int32_t value);
    std::string GetStringFromVariant(const tVariant* var);
    bool GetBoolFromVariant(const tVariant* var);
    bool AllocWStr(WCHAR_T** dest, const std::wstring& src);

    // UTF-8 ↔ wstring helpers (Windows: wchar_t = UTF-16)
    static std::string WstrToUtf8(const std::wstring& ws);
    static std::wstring Utf8ToWstr(const std::string& s);
    // WCHAR_T* (uint16_t* on Linux) → wstring
    static std::wstring WcharTToWstr(const WCHAR_T* s);

    // JSON array of strings → vector<string>
    static std::vector<std::string> ParseJsonStringArray(const std::string& json);
    static std::vector<std::string> ParseStrictStringArray(const std::string& json);
    static bool SkipWs(const std::string& s, size_t& pos);
    static bool ParseJsonStr(const std::string& s, size_t& pos, std::string& out);

    IAddInDefBase* addin_base_  = nullptr;
    IMemoryManager* mem_manager_ = nullptr;

    std::mutex api_mutex_;
    std::shared_ptr<const HbkIndex> index_;
    std::unique_ptr<SearchExecutor> executor_;

    static const wchar_t* method_names_en_[];
    static const wchar_t* method_names_ru_[];
};

} // namespace syntax_help
