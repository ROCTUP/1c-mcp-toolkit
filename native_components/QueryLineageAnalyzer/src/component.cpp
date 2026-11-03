#include "component.h"

#include "bsl/check_code.h"
#include "schema_enricher.h"
#include "strict/check_query.h"
#include "utf_utils.h"

#include <algorithm>
#include <cwctype>
#include <cstring>

#ifdef _WINDOWS
#include <windows.h>
#endif

namespace lineage {

namespace {
const wchar_t* const kCheckFailClosed =
    L"{\"api\":1,\"verdict\":\"confirm\",\"reasons\":[{\"code\":\"Q_INTERNAL\"}]}";
const wchar_t* const kCheckCodeFailClosed =
    L"{\"api\":1,\"verdict\":\"confirm\",\"reasons\":[{\"code\":\"C_INTERNAL\",\"line\":1,\"col\":1,\"offset\":0}],"
    L"\"risks\":[{\"code\":\"R_UNKNOWN_EFFECT\",\"line\":1,\"col\":1}],\"canonical\":\"\",\"result_labels\":{},"
    L"\"missing_tables\":[]}";
}  // namespace

const wchar_t* QueryLineageAnalyzerComponent::method_names_en_[] = {
    L"AnalyzeSources",
    L"Version",
    L"CheckQuery",
    L"CheckCode",
};

const wchar_t* QueryLineageAnalyzerComponent::method_names_ru_[] = {
    L"АнализироватьИсточники",
    L"Версия",
    L"ПроверитьЗапрос",
    L"ПроверитьКод",
};

bool QueryLineageAnalyzerComponent::Init(void* disp) {
    addin_base_ = static_cast<IAddInDefBase*>(disp);
    return addin_base_ != nullptr;
}

bool QueryLineageAnalyzerComponent::setMemManager(void* mem) {
    mem_manager_ = static_cast<IMemoryManager*>(mem);
    return mem_manager_ != nullptr;
}

void QueryLineageAnalyzerComponent::Done() {
    addin_base_ = nullptr;
    mem_manager_ = nullptr;
}

bool QueryLineageAnalyzerComponent::RegisterExtensionAs(WCHAR_T** wsExtensionName) {
    return AllocWCHAR_T(wsExtensionName, L"QueryLineageAnalyzer");
}

long QueryLineageAnalyzerComponent::GetNMethods() {
    return eMethodLast;
}

long QueryLineageAnalyzerComponent::FindMethod(const WCHAR_T* wsMethodName) {
    std::wstring name = FromWCHAR_T(wsMethodName);
    auto lower = [](std::wstring s) {
        for (auto& ch : s) ch = static_cast<wchar_t>(LowercaseU16(static_cast<uint32_t>(ch)));
        return s;
    };
    std::wstring lower_name = lower(name);
    for (long i = 0; i < eMethodLast; ++i) {
        if (lower(method_names_en_[i]) == lower_name || lower(method_names_ru_[i]) == lower_name) return i;
    }
    return -1;
}

const WCHAR_T* QueryLineageAnalyzerComponent::GetMethodName(const long lMethodNum, const long lMethodAlias) {
    if (lMethodNum < 0 || lMethodNum >= eMethodLast) return nullptr;
    const wchar_t* name = (lMethodAlias == 0) ? method_names_en_[lMethodNum] : method_names_ru_[lMethodNum];
    WCHAR_T* result = nullptr;
    if (AllocWCHAR_T(&result, std::wstring(name))) return result;
    return nullptr;
}

long QueryLineageAnalyzerComponent::GetNParams(const long lMethodNum) {
    if (lMethodNum == eMethodAnalyzeSources || lMethodNum == eMethodCheckQuery ||
        lMethodNum == eMethodCheckCode) {
        return 2;
    }
    return 0;
}

bool QueryLineageAnalyzerComponent::GetParamDefValue(const long, const long, tVariant*) {
    return false;
}

bool QueryLineageAnalyzerComponent::HasRetVal(const long) {
    return true;
}

bool QueryLineageAnalyzerComponent::CallAsProc(const long lMethodNum, tVariant* paParams, const long lSizeArray) {
    tVariant ret_val;
    tVarInit(&ret_val);
    return CallAsFunc(lMethodNum, &ret_val, paParams, lSizeArray);
}

bool QueryLineageAnalyzerComponent::CallAsFunc(const long lMethodNum, tVariant* pvarRetValue, tVariant* paParams, const long lSizeArray) {
    if (!pvarRetValue) return false;
    try {
        switch (lMethodNum) {
            case eMethodAnalyzeSources: {
                if (lSizeArray < 2) return false;
                std::string query_utf8 = WStringToUTF8(GetWStringFromVariant(&paParams[0]));
                std::wstring schema_wide = GetWStringFromVariant(&paParams[1]);
                std::string schema_utf8 = WStringToUTF8(schema_wide);
                if (schema_utf8.empty()) return SetWStringToVariant(pvarRetValue, schema_wide);
                std::string enriched = AnalyzeSourcesImpl(query_utf8, schema_utf8);
                return SetWStringToVariant(pvarRetValue, UTF8ToWString(enriched));
            }
            case eMethodCheckQuery: {
                if (lSizeArray < 2) return false;
                std::string out = strict::CheckQueryImpl(WStringToUTF8(GetWStringFromVariant(&paParams[0])),
                                                         WStringToUTF8(GetWStringFromVariant(&paParams[1])));
                return SetWStringToVariant(pvarRetValue, UTF8ToWString(out));
            }
            case eMethodCheckCode: {
                if (lSizeArray < 2) return SetWStringToVariant(pvarRetValue, kCheckCodeFailClosed);
                std::string out = bsl::CheckCodeImpl(WStringToUTF8(GetWStringFromVariant(&paParams[0])),
                                                     WStringToUTF8(GetWStringFromVariant(&paParams[1])));
                return SetWStringToVariant(pvarRetValue, UTF8ToWString(out));
            }
            case eMethodVersion:
                return SetWStringToVariant(pvarRetValue, L"0.2.0");
            default:
                return false;
        }
    } catch (...) {
        try {
            if (lMethodNum == eMethodAnalyzeSources && lSizeArray >= 2) {
                return SetWStringToVariant(pvarRetValue, GetWStringFromVariant(&paParams[1]));
            }
            if (lMethodNum == eMethodCheckQuery) return SetWStringToVariant(pvarRetValue, kCheckFailClosed);
            if (lMethodNum == eMethodCheckCode) return SetWStringToVariant(pvarRetValue, kCheckCodeFailClosed);
            return SetWStringToVariant(pvarRetValue, L"");
        } catch (...) {
            return false;
        }
    }
}

bool QueryLineageAnalyzerComponent::SetWStringToVariant(tVariant* var, const std::wstring& str) {
    if (!var || !mem_manager_) return false;
    TV_VT(var) = VTYPE_PWSTR;
    size_t byte_count = (str.size() + 1) * sizeof(WCHAR_T);
    if (!mem_manager_->AllocMemory(reinterpret_cast<void**>(&var->pwstrVal), static_cast<unsigned long>(byte_count))) {
        return false;
    }
#ifdef _WINDOWS
    memcpy(var->pwstrVal, str.c_str(), byte_count);
#else
    for (size_t i = 0; i <= str.size(); ++i) var->pwstrVal[i] = static_cast<WCHAR_T>(str[i]);
#endif
    var->wstrLen = static_cast<uint32_t>(str.size());
    return true;
}

std::wstring QueryLineageAnalyzerComponent::GetWStringFromVariant(const tVariant* var) {
    if (!var) return L"";
    if (TV_VT(var) == VTYPE_PWSTR && var->pwstrVal) {
#ifdef _WINDOWS
        return std::wstring(var->pwstrVal, var->wstrLen);
#else
        std::wstring result;
        for (uint32_t i = 0; i < var->wstrLen; ++i) result += static_cast<wchar_t>(var->pwstrVal[i]);
        return result;
#endif
    }
    return L"";
}

std::string QueryLineageAnalyzerComponent::WStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
#ifdef _WINDOWS
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), result.data(), size, nullptr, nullptr);
    return result;
#else
    return Utf16UnitsToUtf8(wstr);
#endif
}

std::wstring QueryLineageAnalyzerComponent::UTF8ToWString(const std::string& str) {
    if (str.empty()) return L"";
#ifdef _WINDOWS
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (size <= 0) return L"";
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), result.data(), size);
    return result;
#else
    return Utf8ToUtf16Units(str);
#endif
}

std::wstring QueryLineageAnalyzerComponent::FromWCHAR_T(const WCHAR_T* src, size_t len) {
    if (!src) return L"";
#ifdef _WINDOWS
    if (len > 0) return std::wstring(src, len);
    return std::wstring(src);
#else
    std::wstring result;
    if (len > 0) {
        for (size_t i = 0; i < len; ++i) result += static_cast<wchar_t>(src[i]);
    } else {
        while (*src) { result += static_cast<wchar_t>(*src); ++src; }
    }
    return result;
#endif
}

bool QueryLineageAnalyzerComponent::AllocWCHAR_T(WCHAR_T** dest, const std::wstring& src) {
    if (!dest || !mem_manager_) return false;
    size_t byte_count = (src.size() + 1) * sizeof(WCHAR_T);
    if (!mem_manager_->AllocMemory(reinterpret_cast<void**>(dest), static_cast<unsigned long>(byte_count))) {
        return false;
    }
#ifdef _WINDOWS
    memcpy(*dest, src.c_str(), byte_count);
#else
    for (size_t i = 0; i <= src.size(); ++i) (*dest)[i] = static_cast<WCHAR_T>(src[i]);
#endif
    return true;
}

}  // namespace lineage
