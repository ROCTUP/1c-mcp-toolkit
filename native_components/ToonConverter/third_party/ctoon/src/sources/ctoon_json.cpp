#include "ctoon.h"
#include "yyjson.h"
#include "utils.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ctoon {

// =====================
// Internal helpers
// =====================

static Value parseYyjson(yyjson_val *val) {
    if (yyjson_is_null(val)) return Value(nullptr);
    if (yyjson_is_bool(val)) return Value(yyjson_get_bool(val));
    if (yyjson_is_int(val)) return Value(yyjson_get_sint(val));
    if (yyjson_is_num(val))  return Value(yyjson_get_real(val));
    if (yyjson_is_str(val)) return Value(yyjson_get_str(val));

    if (yyjson_is_arr(val)) {
        Array arr;
        size_t len = yyjson_arr_size(val);
        for (size_t i = 0; i < len; ++i) {
            arr.push_back(parseYyjson(yyjson_arr_get(val, i)));
        }
        return Value(arr);
    }

    if (yyjson_is_obj(val)) {
        Object obj;
        yyjson_obj_iter iter = yyjson_obj_iter_with(val);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&iter))) {
            yyjson_val *item = yyjson_obj_iter_get_val(key);
            obj[yyjson_get_str(key)] = parseYyjson(item);
        }
        return Value(obj);
    }

    throw std::runtime_error("Unsupported JSON type");
}

// =====================
// JSON Serialization
// =====================

Value loadsJson(const std::string& jsonString) {
    yyjson_doc *doc = yyjson_read(jsonString.c_str(), jsonString.length(), 0);
    if (!doc) throw std::runtime_error("Invalid JSON");

    yyjson_val *root = yyjson_doc_get_root(doc);
    Value value = parseYyjson(root);

    yyjson_doc_free(doc); 
    return value;
}

Value loadJson(const std::string& filename) {
    return loadsJson(readStringFromFile(filename));
}

//#define BUILD_YYJSON
//#ifndef BUILD_YYJSON
//static void dumpsJsonImpl(const Value& value, std::ostringstream& out, int indent, int level) {
//        auto writeIndent = [&](int n) {
//            if (indent > 0) {
//                for (int i = 0; i < n; ++i)
//                    out << ' ';
//            }
//        };
//
//        auto newline = [&]() {
//            if (indent > 0) out << "\n";
//        };
//
//        if (value.isPrimitive()) {
//            const auto& p = value.asPrimitive();
//            std::visit([&](auto&& arg) {
//                using T = std::decay_t<decltype(arg)>;
//                if constexpr (std::is_same_v<T, std::nullptr_t>) out << "null";
//                else if constexpr (std::is_same_v<T, bool>) out << (arg ? "true" : "false");
//                else if constexpr (std::is_same_v<T, std::string>) out << "\"" << arg << "\"";
//                else out << arg;
//            }, p);
//        }
//        else if (value.isArray()) {
//            const Array& arr = value.asArray();
//            if (arr.empty()) { out << "[]"; return; }
//
//            out << "[";
//            newline();
//
//            for (size_t i = 0; i < arr.size(); ++i) {
//                writeIndent((level + 1) * indent);
//                dumpsJsonImpl(arr[i], out, indent, level + 1);
//                if (i + 1 < arr.size()) out << ",";
//                newline();
//            }
//
//            writeIndent(level * indent);
//            out << "]";
//        }
//        else if (value.isObject()) {
//            const Object& obj = value.asObject();
//            if (obj.empty()) { out << "{}"; return; }
//
//            out << "{";
//            newline();
//
//            size_t idx = 0;
//            for (const auto& kv : obj) {
//                writeIndent((level + 1) * indent);
//                out << "\"" << kv.first << "\":";
//                if (indent > 0) out << " ";
//                dumpsJsonImpl(kv.second, out, indent, level + 1);
//                if (idx + 1 < obj.size()) out << ",";
//                newline();
//                ++idx;
//            }
//
//            writeIndent(level * indent);
//            out << "}";
//        }
//    }
//    std::string dumpsJson(const Value& value, int indent) {
//        std::ostringstream out;
//        if (indent < 0) indent = 0;
//        dumpsJsonImpl(value, out, indent, 0);
//        return out.str();
//    }
//
//#else
    static yyjson_mut_val* buildYyjson(yyjson_mut_doc* doc, const Value& val) {
        if (val.isPrimitive()) {
            const auto& p = val.asPrimitive();
            return std::visit([&](auto&& arg) -> yyjson_mut_val* {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, std::nullptr_t>)
                    return yyjson_mut_null(doc);
                else if constexpr (std::is_same_v<T, bool>)
                    return yyjson_mut_bool(doc, arg);
                else if constexpr (std::is_same_v<T, int64_t>)
                    return yyjson_mut_sint(doc, arg);
                else if constexpr (std::is_same_v<T, double>)
                    return yyjson_mut_real(doc, arg);
                else // std::string
                    return yyjson_mut_strcpy(doc, arg.c_str());
            }, p);
        }

        if (val.isArray()) {
            yyjson_mut_val* arr = yyjson_mut_arr(doc);
            for (const auto& e : val.asArray()) {
                yyjson_mut_val* child = buildYyjson(doc, e);
                yyjson_mut_arr_append(arr, child);
            }
            return arr;
        }

        // Object
        yyjson_mut_val* obj = yyjson_mut_obj(doc);
        for (const auto& kv : val.asObject()) {
            yyjson_mut_val* child = buildYyjson(doc, kv.second);
            yyjson_mut_val* key = yyjson_mut_strcpy(doc, kv.first.c_str());
            yyjson_mut_obj_add(obj, key, child);
        }
        return obj;
    }

// Serialize ctoon::Value → JSON string
    std::string dumpsJson(const Value& value, int indent) {
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* root = buildYyjson(doc, value);
        yyjson_mut_doc_set_root(doc, root);

        if (indent == 0) indent = -1;
        char* json_cstr = yyjson_mut_write(doc, indent, nullptr);
        std::string json(json_cstr);
        free(json_cstr);
        yyjson_mut_doc_free(doc);
        return json;
    }

//#endif





void dumpJson(const Value& value, const std::string& filename, int indent) {
    writeStringToFile(dumpsJson(value, indent), filename);
}

// =====================
// JSON Parsing from File
// =====================


} // namespace ctoon
