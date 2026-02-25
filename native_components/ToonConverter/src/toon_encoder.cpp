#include "toon_encoder.h"
#include "ctoon.h"
#include <sstream>
#include <vector>
#include <string>

// ============================================================================
// String helpers
// ============================================================================

// JSON-экранирование строки (как json.dumps в Python):
// экранирует \, ", \n, \r, \t, \b, \f, управляющие символы 0x00..0x1F
static std::string JsonEscapeString(const std::string& s) {
    std::ostringstream oss;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n";  break;
            case '\r': oss << "\\r";  break;
            case '\t': oss << "\\t";  break;
            case '\b': oss << "\\b";  break;
            case '\f': oss << "\\f";  break;
            default:
                if (c < 0x20) {
                    const char hex[] = "0123456789abcdef";
                    oss << "\\u00" << hex[(c >> 4) & 0xF] << hex[c & 0xF];
                } else {
                    oss << c;
                }
        }
    }
    return oss.str();
}

// Кодирование ключа как в прокси (_encode_key_for_toon):
// Безопасный ключ: первый символ [A-Za-z_], остальные [A-Za-z0-9_.]
// Кириллица, цифра первой, пробелы, дефисы → с JSON-экранированием в кавычках
static bool IsAsciiWordChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static std::string EncodeKey(const std::string& k) {
    if (k.empty()) return "\"\"";
    unsigned char first = static_cast<unsigned char>(k[0]);
    bool safe = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_';
    if (safe) {
        for (size_t i = 1; i < k.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(k[i]);
            if (!IsAsciiWordChar(c) && c != '.') { safe = false; break; }
        }
    }
    if (safe) return k;
    return "\"" + JsonEscapeString(k) + "\"";
}

// Правила: строка требует кавычек если:
//   1. пустая
//   2. содержит TOON-спецсимволы: , : { } [ ] " \
//   3. содержит управляющие символы < 0x20 (включая \n \r \t)
//   4. есть пробел в начале или конце
//   5. равна "true", "false", "null" (TOON-литералы)
//   6. начинается с цифры, '-' или '.' (воспринимается как число)
static bool StringNeedsQuotes(const std::string& s) {
    if (s.empty()) return true;
    if (s == "true" || s == "false" || s == "null") return true;
    unsigned char first = static_cast<unsigned char>(s[0]);
    if (first == '-' || first == '.' || (first >= '0' && first <= '9')) return true;
    if (s.front() == ' ' || s.back() == ' ') return true;
    for (unsigned char c : s) {
        if (c == ',' || c == ':' || c == '{' || c == '}' ||
            c == '[' || c == ']' || c == '"' || c == '\\' || c < 0x20) return true;
    }
    return false;
}

// Inline-кодирование значения (как Python _encode_inline_nested_value).
// Строки: явные правила кавычения (StringNeedsQuotes + JsonEscapeString).
// Числа/bool/null — прямое форматирование.
static std::string EncodeInline(const ctoon::Value& v) {
    if (v.isPrimitive()) {
        const auto& p = v.asPrimitive();
        if (p.isNull())   return "null";
        if (p.isBool())   return p.getBool() ? "true" : "false";
        if (p.isInt())    return std::to_string(p.getInt());
        if (p.isDouble()) return p.asString();
        const std::string& s = p.getString();
        return StringNeedsQuotes(s) ? ("\"" + JsonEscapeString(s) + "\"") : s;
    }
    if (v.isObject()) {
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& [k, val] : v.asObject()) {
            if (!first) oss << ", ";
            oss << EncodeKey(k) << ": " << EncodeInline(val);
            first = false;
        }
        oss << "}";
        return oss.str();
    }
    if (v.isArray()) {
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        for (const auto& item : v.asArray()) {
            if (!first) oss << ", ";
            oss << EncodeInline(item);
            first = false;
        }
        oss << "]";
        return oss.str();
    }
    return "";
}

// ============================================================================
// Nested-tabular detection and encoding
// ============================================================================

// Проверить: массив объектов с uniform keys, хотя бы одно non-primitive поле.
// Возвращает список полей (по порядку первого объекта) или пустой вектор.
static std::vector<std::string> DetectNestedTabular(const ctoon::Value& val) {
    if (!val.isArray()) return {};
    const auto& arr = val.asArray();
    if (arr.empty()) return {};
    if (!arr[0].isObject()) return {};

    const auto& first_obj = arr[0].asObject();
    std::vector<std::string> fields;
    for (const auto& [k, unused] : first_obj) fields.push_back(k);

    bool has_nested = false;
    for (const auto& item : arr) {
        if (!item.isObject()) return {};
        const auto& obj = item.asObject();
        if (obj.size() != fields.size()) return {};
        for (size_t i = 0; i < fields.size(); ++i) {
            auto it = obj.find(fields[i]);
            if (it == obj.end()) return {};
            if (!it->second.isPrimitive()) has_nested = true;
        }
    }
    return has_nested ? fields : std::vector<std::string>{};
}

// Табличный вывод как в прокси: [N]{f1,f2}:\n  v1,v2\n  ...
// Без trailing newline (как Python "\n".join(lines))
static std::string EncodeNestedTabular(const ctoon::Array& arr,
                                        const std::vector<std::string>& fields) {
    std::vector<std::string> lines;

    // Заголовок
    std::ostringstream header;
    header << "[" << arr.size() << "]{";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) header << ",";
        header << EncodeKey(fields[i]);
    }
    header << "}:";
    lines.push_back(header.str());

    // Строки данных
    for (const auto& item : arr) {
        const auto& obj = item.asObject();
        std::ostringstream row;
        row << "  ";
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i) row << ",";
            row << EncodeInline(obj.at(fields[i]));
        }
        lines.push_back(row.str());
    }

    // Собираем без trailing newline
    std::ostringstream result;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) result << "\n";
        result << lines[i];
    }
    return result.str();
}

// ============================================================================
// Main entry point
// ============================================================================

std::string JsonToToon(const std::string& json_utf8) {
    ctoon::Value val = ctoon::loadsJson(json_utf8);
    auto fields = DetectNestedTabular(val);
    if (!fields.empty()) {
        return EncodeNestedTabular(val.asArray(), fields);
    }
    return ctoon::encode(val);
}
