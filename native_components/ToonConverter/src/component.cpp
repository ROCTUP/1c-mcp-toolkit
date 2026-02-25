#include "component.h"
#include "toon_encoder.h"

#include <cstring>
#include <algorithm>

#ifdef _WINDOWS
#include <windows.h>
#endif

namespace toon {

// ============================================================================
// Name tables
// ============================================================================

const wchar_t* ToonConverterComponent::method_names_en_[] = {
    L"JsonToToon",
};

const wchar_t* ToonConverterComponent::method_names_ru_[] = {
    L"JsonВТун",
};

// ============================================================================
// IInitDoneBase
// ============================================================================

bool ToonConverterComponent::Init(void* disp) {
    addin_base_ = static_cast<IAddInDefBase*>(disp);
    return addin_base_ != nullptr;
}

bool ToonConverterComponent::setMemManager(void* mem) {
    mem_manager_ = static_cast<IMemoryManager*>(mem);
    return mem_manager_ != nullptr;
}

void ToonConverterComponent::Done() {
    addin_base_ = nullptr;
    mem_manager_ = nullptr;
}

// ============================================================================
// RegisterExtensionAs
// ============================================================================

bool ToonConverterComponent::RegisterExtensionAs(WCHAR_T** wsExtensionName) {
    return AllocWCHAR_T(wsExtensionName, L"ToonConverter");
}

// ============================================================================
// Methods
// ============================================================================

long ToonConverterComponent::GetNMethods() {
    return eMethodLast;
}

long ToonConverterComponent::FindMethod(const WCHAR_T* wsMethodName) {
    std::wstring name = FromWCHAR_T(wsMethodName);
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

const WCHAR_T* ToonConverterComponent::GetMethodName(const long lMethodNum,
                                                       const long lMethodAlias) {
    if (lMethodNum < 0 || lMethodNum >= eMethodLast) return nullptr;
    const wchar_t* name = (lMethodAlias == 0) ? method_names_en_[lMethodNum]
                                               : method_names_ru_[lMethodNum];
    WCHAR_T* result = nullptr;
    if (AllocWCHAR_T(&result, std::wstring(name))) return result;
    return nullptr;
}

long ToonConverterComponent::GetNParams(const long lMethodNum) {
    if (lMethodNum == eMethodJsonToToon) return 1;
    return 0;
}

bool ToonConverterComponent::GetParamDefValue(const long /*lMethodNum*/,
                                               const long /*lParamNum*/,
                                               tVariant* /*pvarParamDefValue*/) {
    return false;
}

bool ToonConverterComponent::HasRetVal(const long lMethodNum) {
    return lMethodNum == eMethodJsonToToon;
}

bool ToonConverterComponent::CallAsProc(const long /*lMethodNum*/,
                                         tVariant* /*paParams*/,
                                         const long /*lSizeArray*/) {
    return false;
}

bool ToonConverterComponent::CallAsFunc(const long lMethodNum,
                                         tVariant* pvarRetValue,
                                         tVariant* paParams,
                                         const long lSizeArray) {
    if (!pvarRetValue) return false;

    switch (lMethodNum) {
        case eMethodJsonToToon: {
            if (lSizeArray < 1) return false;
            std::string json_utf8 = WStringToUTF8(GetWStringFromVariant(&paParams[0]));
            try {
                std::string toon = JsonToToon(json_utf8);
                return SetWStringToVariant(pvarRetValue, UTF8ToWString(toon));
            } catch (...) {
                return SetWStringToVariant(pvarRetValue, L"");
            }
        }
        default:
            return false;
    }
}

// ============================================================================
// Helpers
// ============================================================================

bool ToonConverterComponent::SetWStringToVariant(tVariant* var, const std::wstring& str) {
    if (!var || !mem_manager_) return false;

    TV_VT(var) = VTYPE_PWSTR;
    size_t byte_count = (str.size() + 1) * sizeof(WCHAR_T);
    if (!mem_manager_->AllocMemory(reinterpret_cast<void**>(&var->pwstrVal),
                                    static_cast<unsigned long>(byte_count))) {
        return false;
    }

#ifdef _WINDOWS
    memcpy(var->pwstrVal, str.c_str(), byte_count);
#else
    for (size_t i = 0; i <= str.size(); ++i)
        var->pwstrVal[i] = static_cast<WCHAR_T>(str[i]);
#endif

    var->wstrLen = static_cast<uint32_t>(str.size());
    return true;
}

std::wstring ToonConverterComponent::GetWStringFromVariant(const tVariant* var) {
    if (!var) return L"";
    if (TV_VT(var) == VTYPE_PWSTR && var->pwstrVal) {
#ifdef _WINDOWS
        return std::wstring(var->pwstrVal, var->wstrLen);
#else
        std::wstring result;
        result.reserve(var->wstrLen);
        for (uint32_t i = 0; i < var->wstrLen; ++i)
            result += static_cast<wchar_t>(var->pwstrVal[i]);
        return result;
#endif
    }
    return L"";
}

std::string ToonConverterComponent::WStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
#ifdef _WINDOWS
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                                    static_cast<int>(wstr.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                         static_cast<int>(wstr.size()),
                         result.data(), size, nullptr, nullptr);
    return result;
#else
    std::string result;
    for (wchar_t ch : wstr) {
        if (ch < 0x80) {
            result += static_cast<char>(ch);
        } else if (ch < 0x800) {
            result += static_cast<char>(0xC0 | (ch >> 6));
            result += static_cast<char>(0x80 | (ch & 0x3F));
        } else if (ch < 0x10000) {
            result += static_cast<char>(0xE0 | (ch >> 12));
            result += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (ch & 0x3F));
        } else {
            result += static_cast<char>(0xF0 | (ch >> 18));
            result += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (ch & 0x3F));
        }
    }
    return result;
#endif
}

std::wstring ToonConverterComponent::UTF8ToWString(const std::string& str) {
    if (str.empty()) return L"";
#ifdef _WINDOWS
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
                                    static_cast<int>(str.size()),
                                    nullptr, 0);
    if (size <= 0) return L"";
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
                         static_cast<int>(str.size()),
                         result.data(), size);
    return result;
#else
    std::wstring result;
    size_t i = 0;
    while (i < str.size()) {
        uint32_t ch = 0;
        unsigned char c = str[i];
        if (c < 0x80) {
            ch = c; ++i;
        } else if (c < 0xE0) {
            ch = (c & 0x1F) << 6;
            if (i + 1 < str.size()) ch |= (str[i + 1] & 0x3F);
            i += 2;
        } else if (c < 0xF0) {
            ch = (c & 0x0F) << 12;
            if (i + 1 < str.size()) ch |= (str[i + 1] & 0x3F) << 6;
            if (i + 2 < str.size()) ch |= (str[i + 2] & 0x3F);
            i += 3;
        } else {
            ch = (c & 0x07) << 18;
            if (i + 1 < str.size()) ch |= (str[i + 1] & 0x3F) << 12;
            if (i + 2 < str.size()) ch |= (str[i + 2] & 0x3F) << 6;
            if (i + 3 < str.size()) ch |= (str[i + 3] & 0x3F);
            i += 4;
        }
        result += static_cast<wchar_t>(ch);
    }
    return result;
#endif
}

std::wstring ToonConverterComponent::FromWCHAR_T(const WCHAR_T* src, size_t len) {
    if (!src) return L"";
#ifdef _WINDOWS
    if (len > 0) return std::wstring(src, len);
    return std::wstring(src);
#else
    std::wstring result;
    if (len > 0) {
        result.reserve(len);
        for (size_t i = 0; i < len; ++i)
            result += static_cast<wchar_t>(src[i]);
    } else {
        while (*src) {
            result += static_cast<wchar_t>(*src);
            ++src;
        }
    }
    return result;
#endif
}

bool ToonConverterComponent::AllocWCHAR_T(WCHAR_T** dest, const std::wstring& src) {
    if (!dest || !mem_manager_) return false;
    size_t byte_count = (src.size() + 1) * sizeof(WCHAR_T);
    if (!mem_manager_->AllocMemory(reinterpret_cast<void**>(dest),
                                    static_cast<unsigned long>(byte_count))) {
        return false;
    }
#ifdef _WINDOWS
    memcpy(*dest, src.c_str(), byte_count);
#else
    for (size_t i = 0; i <= src.size(); ++i)
        (*dest)[i] = static_cast<WCHAR_T>(src[i]);
#endif
    return true;
}

} // namespace toon
