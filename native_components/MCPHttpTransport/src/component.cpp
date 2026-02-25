#include "component.h"

#include <cstring>
#include <algorithm>
#include <mutex>

namespace mcp {

// ============================================================================
// Name tables
// ============================================================================

const wchar_t* MCPHttpTransportComponent::prop_names_en_[] = {
    L"IsRunning",
    L"Port",
    L"RequestTimeout",
    L"MaxConcurrentRequests",
};

const wchar_t* MCPHttpTransportComponent::prop_names_ru_[] = {
    L"Работает",
    L"Порт",
    L"ТаймаутЗапроса",
    L"МаксПараллельныхЗапросов",
};

const wchar_t* MCPHttpTransportComponent::method_names_en_[] = {
    L"Start",
    L"Stop",
    L"SendResponse",
    L"SendSSEEvent",
    L"CloseSSEStream",
    L"GetRequestBody",
};

const wchar_t* MCPHttpTransportComponent::method_names_ru_[] = {
    L"Старт",
    L"Стоп",
    L"ОтправитьОтвет",
    L"ОтправитьSSEСобытие",
    L"ЗакрытьSSEПоток",
    L"ПолучитьТелоЗапроса",
};

// ============================================================================
// Constructor/Destructor
// ============================================================================

MCPHttpTransportComponent::MCPHttpTransportComponent() = default;

MCPHttpTransportComponent::~MCPHttpTransportComponent() {
    transport_.Stop();
}

// ============================================================================
// IInitDoneBase
// ============================================================================

bool MCPHttpTransportComponent::Init(void* disp) {
    addin_base_ = static_cast<IAddInDefBase*>(disp);
    if (addin_base_) {
        addin_base_->SetEventBufferDepth(1000);
    }
    return addin_base_ != nullptr;
}

bool MCPHttpTransportComponent::setMemManager(void* mem) {
    mem_manager_ = static_cast<IMemoryManager*>(mem);
    return mem_manager_ != nullptr;
}

long MCPHttpTransportComponent::GetInfo() {
    return 2000;  // Version 2
}

void MCPHttpTransportComponent::Done() {
    transport_.Stop();
    std::lock_guard<std::mutex> lock(addin_mutex_);
    addin_base_ = nullptr;
    mem_manager_ = nullptr;
}

// ============================================================================
// RegisterExtensionAs
// ============================================================================

bool MCPHttpTransportComponent::RegisterExtensionAs(WCHAR_T** wsExtensionName) {
    const std::wstring name = L"MCPHttpTransport";
    return AllocWCHAR_T(wsExtensionName, name);
}

// ============================================================================
// Properties
// ============================================================================

long MCPHttpTransportComponent::GetNProps() {
    return ePropLast;
}

long MCPHttpTransportComponent::FindProp(const WCHAR_T* wsPropName) {
    std::wstring name = FromWCHAR_T(wsPropName);

    // Case-insensitive compare
    auto lower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    };

    std::wstring lower_name = lower(name);

    for (long i = 0; i < ePropLast; ++i) {
        if (lower(prop_names_en_[i]) == lower_name || lower(prop_names_ru_[i]) == lower_name) {
            return i;
        }
    }
    return -1;
}

const WCHAR_T* MCPHttpTransportComponent::GetPropName(long lPropNum, long lPropAlias) {
    if (lPropNum < 0 || lPropNum >= ePropLast) return nullptr;

    const wchar_t* name = (lPropAlias == 0) ? prop_names_en_[lPropNum] : prop_names_ru_[lPropNum];

    WCHAR_T* result = nullptr;
    std::wstring wname(name);
    if (AllocWCHAR_T(&result, wname)) {
        return result;
    }
    return nullptr;
}

bool MCPHttpTransportComponent::GetPropVal(const long lPropNum, tVariant* pvarPropVal) {
    if (!pvarPropVal) return false;

    switch (lPropNum) {
        case ePropIsRunning:
            TV_VT(pvarPropVal) = VTYPE_BOOL;
            TV_BOOL(pvarPropVal) = transport_.IsRunning();
            return true;

        case ePropPort:
            TV_VT(pvarPropVal) = VTYPE_I4;
            TV_I4(pvarPropVal) = transport_.GetPort();
            return true;

        case ePropRequestTimeout:
            TV_VT(pvarPropVal) = VTYPE_I4;
            TV_I4(pvarPropVal) = transport_.GetRequestTimeout();
            return true;

        case ePropMaxConcurrentRequests:
            TV_VT(pvarPropVal) = VTYPE_I4;
            TV_I4(pvarPropVal) = transport_.GetMaxConcurrentRequests();
            return true;

        default:
            return false;
    }
}

bool MCPHttpTransportComponent::SetPropVal(const long lPropNum, tVariant* varPropVal) {
    if (!varPropVal) return false;

    auto get_int = [](tVariant* v) -> std::pair<bool, int> {
        if (TV_VT(v) == VTYPE_I4) return {true, TV_I4(v)};
        if (TV_VT(v) == VTYPE_R8) return {true, static_cast<int>(TV_R8(v))};
        return {false, 0};
    };

    switch (lPropNum) {
        case ePropRequestTimeout: {
            auto [ok, val] = get_int(varPropVal);
            if (ok) { transport_.SetRequestTimeout(val); return true; }
            return false;
        }

        case ePropMaxConcurrentRequests: {
            auto [ok, val] = get_int(varPropVal);
            if (ok) { transport_.SetMaxConcurrentRequests(val); return true; }
            return false;
        }

        default:
            return false;
    }
}

bool MCPHttpTransportComponent::IsPropReadable(const long lPropNum) {
    return lPropNum >= 0 && lPropNum < ePropLast;
}

bool MCPHttpTransportComponent::IsPropWritable(const long lPropNum) {
    return lPropNum == ePropRequestTimeout || lPropNum == ePropMaxConcurrentRequests;
}

// ============================================================================
// Methods
// ============================================================================

long MCPHttpTransportComponent::GetNMethods() {
    return eMethodLast;
}

long MCPHttpTransportComponent::FindMethod(const WCHAR_T* wsMethodName) {
    std::wstring name = FromWCHAR_T(wsMethodName);

    auto lower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    };

    std::wstring lower_name = lower(name);

    for (long i = 0; i < eMethodLast; ++i) {
        if (lower(method_names_en_[i]) == lower_name || lower(method_names_ru_[i]) == lower_name) {
            return i;
        }
    }
    return -1;
}

const WCHAR_T* MCPHttpTransportComponent::GetMethodName(const long lMethodNum, const long lMethodAlias) {
    if (lMethodNum < 0 || lMethodNum >= eMethodLast) return nullptr;

    const wchar_t* name = (lMethodAlias == 0) ? method_names_en_[lMethodNum] : method_names_ru_[lMethodNum];

    WCHAR_T* result = nullptr;
    std::wstring wname(name);
    if (AllocWCHAR_T(&result, wname)) {
        return result;
    }
    return nullptr;
}

long MCPHttpTransportComponent::GetNParams(const long lMethodNum) {
    switch (lMethodNum) {
        case eMethodStart:            return 1;  // port
        case eMethodStop:             return 0;
        case eMethodSendResponse:     return 4;  // requestId, statusCode, headersJson, body
        case eMethodSendSSEEvent:     return 4;  // requestId, eventData, headersJson, eventType
        case eMethodCloseSSEStream:   return 1;  // requestId
        case eMethodGetRequestBody:   return 1;  // requestId
        default: return 0;
    }
}

bool MCPHttpTransportComponent::GetParamDefValue(const long lMethodNum, const long lParamNum,
                                                   tVariant* pvarParamDefValue) {
    if (lMethodNum == eMethodSendSSEEvent && lParamNum == 3) {
        return SetWStringToVariant(pvarParamDefValue, L"message");
    }
    TV_VT(pvarParamDefValue) = VTYPE_EMPTY;
    return false;
}

bool MCPHttpTransportComponent::HasRetVal(const long lMethodNum) {
    // All methods return a value
    return true;
}

bool MCPHttpTransportComponent::CallAsProc(const long lMethodNum, tVariant* paParams,
                                            const long lSizeArray) {
    // Forward to CallAsFunc, ignoring return value
    tVariant retVal;
    tVarInit(&retVal);
    return CallAsFunc(lMethodNum, &retVal, paParams, lSizeArray);
}

bool MCPHttpTransportComponent::CallAsFunc(const long lMethodNum, tVariant* pvarRetValue,
                                            tVariant* paParams, const long lSizeArray) {
    if (!pvarRetValue) return false;

    switch (lMethodNum) {
        case eMethodStart: {
            if (lSizeArray < 1) return false;
            int port = 0;
            if (TV_VT(&paParams[0]) == VTYPE_I4) port = TV_I4(&paParams[0]);
            else if (TV_VT(&paParams[0]) == VTYPE_R8) port = static_cast<int>(TV_R8(&paParams[0]));
            else return false;

            auto callback = [this](const std::string& source, const std::string& event,
                                   const std::string& data) -> bool {
                return FireExternalEvent(source, event, data);
            };

            bool result = transport_.Start(port, callback);
            TV_VT(pvarRetValue) = VTYPE_BOOL;
            TV_BOOL(pvarRetValue) = result;
            return true;
        }

        case eMethodStop: {
            bool result = transport_.Stop();
            TV_VT(pvarRetValue) = VTYPE_BOOL;
            TV_BOOL(pvarRetValue) = result;
            return true;
        }

        case eMethodSendResponse: {
            if (lSizeArray < 4) return false;
            std::string request_id = WStringToUTF8(GetWStringFromVariant(&paParams[0]));

            int status_code = 0;
            if (TV_VT(&paParams[1]) == VTYPE_I4) status_code = TV_I4(&paParams[1]);
            else if (TV_VT(&paParams[1]) == VTYPE_R8) status_code = static_cast<int>(TV_R8(&paParams[1]));

            std::string headers_json = WStringToUTF8(GetWStringFromVariant(&paParams[2]));
            std::string body = WStringToUTF8(GetWStringFromVariant(&paParams[3]));

            bool result = transport_.SendResponse(request_id, status_code, headers_json, body);
            TV_VT(pvarRetValue) = VTYPE_BOOL;
            TV_BOOL(pvarRetValue) = result;
            return true;
        }

        case eMethodSendSSEEvent: {
            if (lSizeArray < 3) return false;
            std::string request_id = WStringToUTF8(GetWStringFromVariant(&paParams[0]));
            std::string event_data  = WStringToUTF8(GetWStringFromVariant(&paParams[1]));
            std::string headers_json = WStringToUTF8(GetWStringFromVariant(&paParams[2]));
            // 4th param is optional: if absent, empty, or VTYPE_EMPTY — default to "message"
            std::string event_type = "message";
            if (lSizeArray >= 4 && TV_VT(&paParams[3]) != VTYPE_EMPTY) {
                std::string et = WStringToUTF8(GetWStringFromVariant(&paParams[3]));
                if (!et.empty()) event_type = et;
            }

            bool result = transport_.SendSSEEvent(request_id, event_data, headers_json, event_type);
            TV_VT(pvarRetValue) = VTYPE_BOOL;
            TV_BOOL(pvarRetValue) = result;
            return true;
        }

        case eMethodCloseSSEStream: {
            if (lSizeArray < 1) return false;
            std::string request_id = WStringToUTF8(GetWStringFromVariant(&paParams[0]));

            bool result = transport_.CloseSSEStream(request_id);
            TV_VT(pvarRetValue) = VTYPE_BOOL;
            TV_BOOL(pvarRetValue) = result;
            return true;
        }

        case eMethodGetRequestBody: {
            if (lSizeArray < 1) return false;
            std::string request_id = WStringToUTF8(GetWStringFromVariant(&paParams[0]));

            std::string body = transport_.GetRequestBody(request_id);
            std::wstring wbody = UTF8ToWString(body);
            return SetWStringToVariant(pvarRetValue, wbody);
        }

        default:
            return false;
    }
}

// ============================================================================
// LocaleBase
// ============================================================================

void MCPHttpTransportComponent::SetLocale(const WCHAR_T* /*loc*/) {
    // No locale-dependent behavior
}

// ============================================================================
// Helpers: string conversion
// ============================================================================

bool MCPHttpTransportComponent::SetWStringToVariant(tVariant* var, const std::wstring& str) {
    if (!var || !mem_manager_) return false;

    TV_VT(var) = VTYPE_PWSTR;
    size_t byte_count = (str.size() + 1) * sizeof(WCHAR_T);
    if (!mem_manager_->AllocMemory(reinterpret_cast<void**>(&var->pwstrVal),
                                    static_cast<unsigned long>(byte_count))) {
        return false;
    }

#ifdef _WINDOWS
    // WCHAR_T == wchar_t on Windows
    memcpy(var->pwstrVal, str.c_str(), byte_count);
#else
    // Convert wchar_t (32-bit) to uint16_t
    for (size_t i = 0; i <= str.size(); ++i) {
        var->pwstrVal[i] = static_cast<WCHAR_T>(str[i]);
    }
#endif

    var->wstrLen = static_cast<uint32_t>(str.size());
    return true;
}

std::wstring MCPHttpTransportComponent::GetWStringFromVariant(const tVariant* var) {
    if (!var) return L"";

    if (TV_VT(var) == VTYPE_PWSTR && var->pwstrVal) {
#ifdef _WINDOWS
        return std::wstring(var->pwstrVal, var->wstrLen);
#else
        std::wstring result;
        result.reserve(var->wstrLen);
        for (uint32_t i = 0; i < var->wstrLen; ++i) {
            result += static_cast<wchar_t>(var->pwstrVal[i]);
        }
        return result;
#endif
    }

    return L"";
}

std::string MCPHttpTransportComponent::WStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return "";

#ifdef _WINDOWS
    // Use WideCharToMultiByte on Windows
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()),
                         result.data(), size, nullptr, nullptr);
    return result;
#else
    // Simple conversion for non-Windows (wchar_t is 32-bit)
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

std::wstring MCPHttpTransportComponent::UTF8ToWString(const std::string& str) {
    if (str.empty()) return L"";

#ifdef _WINDOWS
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()),
                                    nullptr, 0);
    if (size <= 0) return L"";
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()),
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

std::wstring MCPHttpTransportComponent::FromWCHAR_T(const WCHAR_T* src, size_t len) {
    if (!src) return L"";

#ifdef _WINDOWS
    // WCHAR_T == wchar_t on Windows
    if (len > 0) return std::wstring(src, len);
    return std::wstring(src);
#else
    std::wstring result;
    if (len > 0) {
        result.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            result += static_cast<wchar_t>(src[i]);
        }
    } else {
        while (*src) {
            result += static_cast<wchar_t>(*src);
            ++src;
        }
    }
    return result;
#endif
}

bool MCPHttpTransportComponent::AllocWCHAR_T(WCHAR_T** dest, const std::wstring& src) {
    if (!dest || !mem_manager_) return false;

    size_t byte_count = (src.size() + 1) * sizeof(WCHAR_T);
    if (!mem_manager_->AllocMemory(reinterpret_cast<void**>(dest),
                                    static_cast<unsigned long>(byte_count))) {
        return false;
    }

#ifdef _WINDOWS
    memcpy(*dest, src.c_str(), byte_count);
#else
    for (size_t i = 0; i <= src.size(); ++i) {
        (*dest)[i] = static_cast<WCHAR_T>(src[i]);
    }
#endif
    return true;
}

// ============================================================================
// Fire ExternalEvent (called from HTTP threads)
// ============================================================================

bool MCPHttpTransportComponent::FireExternalEvent(const std::string& source,
                                                    const std::string& event,
                                                    const std::string& data) {
    // Convert strings outside the lock (no shared state needed)
    std::wstring wsource = UTF8ToWString(source);
    std::wstring wevent = UTF8ToWString(event);
    std::wstring wdata = UTF8ToWString(data);

    // Lock protects addin_base_ and mem_manager_ from concurrent Done()
    std::lock_guard<std::mutex> lock(addin_mutex_);

    if (!addin_base_ || !mem_manager_) return false;

    // Allocate WCHAR_T* for ExternalEvent (1C takes ownership)
    WCHAR_T* ws_source = nullptr;
    WCHAR_T* ws_event = nullptr;
    WCHAR_T* ws_data = nullptr;

    if (!AllocWCHAR_T(&ws_source, wsource) ||
        !AllocWCHAR_T(&ws_event, wevent) ||
        !AllocWCHAR_T(&ws_data, wdata)) {
        // Cleanup on failure
        if (ws_source) mem_manager_->FreeMemory(reinterpret_cast<void**>(&ws_source));
        if (ws_event) mem_manager_->FreeMemory(reinterpret_cast<void**>(&ws_event));
        if (ws_data) mem_manager_->FreeMemory(reinterpret_cast<void**>(&ws_data));
        return false;
    }

    return addin_base_->ExternalEvent(ws_source, ws_event, ws_data);
}

} // namespace mcp
