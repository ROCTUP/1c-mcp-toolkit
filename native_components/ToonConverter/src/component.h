#pragma once

#include "ComponentBase.h"
#include "AddInDefBase.h"
#include "IMemoryManager.h"

#include <string>

namespace toon {

class ToonConverterComponent : public IComponentBase {
public:
    ToonConverterComponent() = default;
    ~ToonConverterComponent() override = default;

    // IInitDoneBase
    bool ADDIN_API Init(void* disp) override;
    bool ADDIN_API setMemManager(void* mem) override;
    long ADDIN_API GetInfo() override { return 2000; }
    void ADDIN_API Done() override;

    // ILanguageExtenderBase
    bool ADDIN_API RegisterExtensionAs(WCHAR_T** wsExtensionName) override;

    long ADDIN_API GetNProps() override { return 0; }
    long ADDIN_API FindProp(const WCHAR_T* /*wsPropName*/) override { return -1; }
    const WCHAR_T* ADDIN_API GetPropName(long /*lPropNum*/, long /*lPropAlias*/) override { return nullptr; }
    bool ADDIN_API GetPropVal(const long /*lPropNum*/, tVariant* /*pvarPropVal*/) override { return false; }
    bool ADDIN_API SetPropVal(const long /*lPropNum*/, tVariant* /*varPropVal*/) override { return false; }
    bool ADDIN_API IsPropReadable(const long /*lPropNum*/) override { return false; }
    bool ADDIN_API IsPropWritable(const long /*lPropNum*/) override { return false; }

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
    void ADDIN_API SetLocale(const WCHAR_T* /*loc*/) override {}

private:
    enum Methods {
        eMethodJsonToToon = 0,
        eMethodLast
    };

    bool SetWStringToVariant(tVariant* var, const std::wstring& str);
    std::wstring GetWStringFromVariant(const tVariant* var);
    std::string WStringToUTF8(const std::wstring& wstr);
    std::wstring UTF8ToWString(const std::string& str);
    std::wstring FromWCHAR_T(const WCHAR_T* src, size_t len = 0);
    bool AllocWCHAR_T(WCHAR_T** dest, const std::wstring& src);

    IAddInDefBase* addin_base_ = nullptr;
    IMemoryManager* mem_manager_ = nullptr;

    static const wchar_t* method_names_en_[];
    static const wchar_t* method_names_ru_[];
};

} // namespace toon
