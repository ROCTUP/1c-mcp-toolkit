#include "strict/field_policy.h"

#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "strict/strict_tokenizer.h"
#include "utf_utils.h"

namespace lineage::strict {

namespace {

using Json = nlohmann::json;
using FieldMap = std::map<std::string, FieldInfo>;

int Strictness(FieldClass cls) {
    switch (cls) {
        case FieldClass::Protected: return 2;
        case FieldClass::Unknown: return 1;
        case FieldClass::Open: return 0;
    }
    return 2;
}

FieldClass Stricter(FieldClass a, FieldClass b) { return Strictness(a) >= Strictness(b) ? a : b; }

bool ParseClass(const Json& v, FieldClass* out) {
    if (!v.is_string()) return false;
    const std::string s = v.get<std::string>();
    if (s == "open") *out = FieldClass::Open;
    else if (s == "protected") *out = FieldClass::Protected;
    else if (s == "unknown") *out = FieldClass::Unknown;
    else return false;
    return true;
}

bool ParseParamOrigin(const Json& v, ParamOrigin* out) {
    if (!v.is_string()) return false;
    const std::string s = v.get<std::string>();
    if (s == "client") *out = ParamOrigin::Client;
    else if (s == "token_protected") *out = ParamOrigin::TokenProtected;
    else if (s == "token_unknown") *out = ParamOrigin::TokenUnknown;
    else return false;
    return true;
}

// The collision key of a parameter name: the normalized key with the Cyrillic capital yo (U+0401) folded to the
// capital ie (U+0415). The lookup key stays NormalizeIdentifierKey.
std::string ParamCollisionKey(const std::string& normalized) {
    std::string key = normalized;
    for (size_t p = 0; p + 1 < key.size(); ++p) {
        if (static_cast<unsigned char>(key[p]) == 0xD0 && static_cast<unsigned char>(key[p + 1]) == 0x81) {
            key[p + 1] = static_cast<char>(0x95);
        }
    }
    return key;
}

// Adds a parameter origin under its normalized name. Names that collide after normalization are one parameter to
// the host, which binds whichever spelling it meets last: they must carry the same origin, false otherwise (no
// origin is safe to keep, a client label over a protected value returns it unmasked). Names that differ only in yo /
// ie are held to the same rule (`folds`, by ParamCollisionKey), whether or not the platform tells them apart. A
// name outside the alphabet of a query parameter (IsIdentifierName) is false as well: no query text can name it,
// and a host that trims or folds it would bind its value to another parameter. It is checked on the raw bytes,
// before normalization drops any. False changes nothing.
bool AddParam(std::map<std::string, ParamOrigin>& params, std::map<std::string, ParamOrigin>& folds,
              const std::string& name, ParamOrigin origin) {
    if (!IsIdentifierName(name)) return false;
    const std::string key = NormalizeIdentifierKey(name);
    const auto fold = folds.emplace(ParamCollisionKey(key), origin);
    if (!fold.second && fold.first->second != origin) return false;
    params.emplace(key, origin);
    return true;
}

// Merges a field entry under an already used (normalized) name: the class can only get stricter.
void MergeField(FieldMap& fields, const std::string& key, FieldInfo info) {
    auto it = fields.find(key);
    if (it == fields.end()) {
        fields.emplace(key, std::move(info));
        return;
    }
    it->second.cls = Stricter(it->second.cls, info.cls);
    for (auto& ref : info.refs) it->second.refs.push_back(std::move(ref));
}

bool ParseFieldInfo(const Json& v, FieldInfo* out) {
    if (!v.is_object()) return false;
    const auto cls = v.find("class");
    if (cls == v.end() || !ParseClass(*cls, &out->cls)) return false;
    const auto refs = v.find("refs");
    if (refs == v.end()) return true;
    if (!refs->is_array()) return false;
    for (const auto& r : *refs) {
        if (!r.is_string()) return false;
        out->refs.push_back(r.get<std::string>());
    }
    return true;
}

bool ParseFields(const Json& v, FieldMap* out) {
    if (!v.is_object()) return false;
    for (auto it = v.begin(); it != v.end(); ++it) {
        FieldInfo info;
        if (!ParseFieldInfo(it.value(), &info)) return false;
        MergeField(*out, NormalizeIdentifierKey(it.key()), std::move(info));
    }
    return true;
}

}  // namespace

// Friend of FieldPolicy: fills the private maps while parsing.
class PolicyReader {
public:
    static bool Read(const Json& root, FieldPolicy* policy, std::string* error) {
        const auto version = root.find("version");
        if (version == root.end() || !version->is_number_integer() || version->get<long long>() != 1) {
            *error = "Q_POLICY_VERSION";
            return false;
        }
        if (!ReadTables(root, policy) || !ReadVirtualTables(root, policy) || !ReadParams(root, policy)) {
            *error = "Q_POLICY_SHAPE";
            return false;
        }
        return true;
    }

private:
    static void AddTable(FieldPolicy* policy, const std::string& key, FieldPolicy::TableData data) {
        auto it = policy->tables_.find(key);
        if (it == policy->tables_.end()) {
            policy->tables_.emplace(key, std::move(data));
            return;
        }
        it->second.presentation = Stricter(it->second.presentation, data.presentation);
        for (auto& f : data.fields) MergeField(it->second.fields, f.first, std::move(f.second));
    }

    static bool ReadSections(const Json& table, const std::string& table_key, FieldClass presentation, FieldPolicy* policy) {
        const auto sections = table.find("tabular_sections");
        if (sections == table.end()) return true;
        if (!sections->is_object()) return false;
        for (auto it = sections->begin(); it != sections->end(); ++it) {
            if (!it.value().is_object()) return false;
            FieldPolicy::TableData data;
            data.presentation = presentation;
            const auto fields = it.value().find("fields");
            if (fields != it.value().end() && !ParseFields(*fields, &data.fields)) return false;
            AddTable(policy, table_key + "." + NormalizeIdentifierKey(it.key()), std::move(data));
        }
        return true;
    }

    static bool ReadTables(const Json& root, FieldPolicy* policy) {
        const auto tables = root.find("tables");
        if (tables == root.end()) return true;
        if (!tables->is_object()) return false;
        for (auto it = tables->begin(); it != tables->end(); ++it) {
            const Json& table = it.value();
            if (!table.is_object()) return false;
            FieldPolicy::TableData data;   // presentation defaults to Protected
            const auto presentation = table.find("presentation");
            if (presentation != table.end() && !ParseClass(*presentation, &data.presentation)) return false;
            const auto fields = table.find("fields");
            if (fields != table.end() && !ParseFields(*fields, &data.fields)) return false;
            const std::string key = NormalizeIdentifierKey(it.key());
            const FieldClass table_presentation = data.presentation;
            AddTable(policy, key, std::move(data));
            if (!ReadSections(table, key, table_presentation, policy)) return false;
        }
        return true;
    }

    static bool ReadVirtualTables(const Json& root, FieldPolicy* policy) {
        const auto vts = root.find("virtual_tables");
        if (vts == root.end()) return true;
        if (!vts->is_object()) return false;
        for (auto it = vts->begin(); it != vts->end(); ++it) {
            const Json& vt = it.value();
            if (!vt.is_object()) return false;
            const auto base = vt.find("base");
            if (base == vt.end() || !base->is_string()) return false;
            FieldPolicy::VirtualData data;
            data.base = base->get<std::string>();
            if (policy->tables_.find(NormalizeIdentifierKey(data.base)) == policy->tables_.end()) return false;
            const auto extra = vt.find("extra_fields");
            if (extra != vt.end() && !ParseFields(*extra, &data.extra_fields)) return false;
            const std::string key = NormalizeIdentifierKey(it.key());
            // no silent override, and a virtual table never shadows a real one (the base lookup would loop)
            if (policy->virtuals_.find(key) != policy->virtuals_.end()) return false;
            if (policy->tables_.find(key) != policy->tables_.end()) return false;
            policy->virtuals_.emplace(key, std::move(data));
        }
        return true;
    }

    static bool ReadParams(const Json& root, FieldPolicy* policy) {
        const auto params = root.find("params");
        if (params == root.end()) return true;
        if (!params->is_object()) return false;
        for (auto it = params->begin(); it != params->end(); ++it) {
            ParamOrigin origin = ParamOrigin::Missing;
            if (!ParseParamOrigin(it.value(), &origin)) return false;
            if (!AddParam(policy->params_, policy->param_folds_, it.key(), origin)) return false;
        }
        return true;
    }
};

FieldPolicy::Parsed FieldPolicy::FromJson(const std::string& json) {
    Parsed result;
    try {
        const Json root = Json::parse(json, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            result.error = "Q_POLICY_JSON";
            return result;
        }
        FieldPolicy policy;
        if (!PolicyReader::Read(root, &policy, &result.error)) return result;
        result.policy = std::move(policy);
        result.ok = true;
    } catch (const std::exception&) {
        result = Parsed();
        result.error = "Q_POLICY_JSON";
    }
    return result;
}

bool FieldPolicy::HasTable(const std::string& table) const {
    return tables_.find(NormalizeIdentifierKey(table)) != tables_.end();
}

ParamOrigin FieldPolicy::OriginOfParam(const std::string& name) const {
    const auto it = params_.find(NormalizeIdentifierKey(name));
    return it == params_.end() ? ParamOrigin::Missing : it->second;
}

bool FieldPolicy::SetParams(const std::map<std::string, ParamOrigin>& params) {
    std::map<std::string, ParamOrigin> normalized, folds;
    for (const auto& kv : params) {
        if (!AddParam(normalized, folds, kv.first, kv.second)) return false;
    }
    params_ = std::move(normalized);
    param_folds_ = std::move(folds);
    return true;
}

bool FieldPolicy::AgreeParam(const std::string& name, ParamOrigin origin) {
    return AddParam(params_, param_folds_, name, origin);
}

std::string FieldPolicy::ResolveVirtualBase(const std::string& vt) const {
    const auto it = virtuals_.find(NormalizeIdentifierKey(vt));
    return it == virtuals_.end() ? std::string() : it->second.base;
}

FieldClass FieldPolicy::PresentationClass(const std::string& table) const {
    const std::string key = NormalizeIdentifierKey(table);
    auto it = tables_.find(key);
    if (it != tables_.end()) return it->second.presentation;
    const auto vt = virtuals_.find(key);
    if (vt == virtuals_.end()) return FieldClass::Unknown;
    it = tables_.find(NormalizeIdentifierKey(vt->second.base));
    return it == tables_.end() ? FieldClass::Unknown : it->second.presentation;
}

const FieldInfo* FieldPolicy::Field(const std::string& table, const std::string& field) const {
    const std::string key = NormalizeIdentifierKey(table);
    const std::string name = NormalizeIdentifierKey(field);
    const auto vt = virtuals_.find(key);
    if (vt != virtuals_.end()) {
        const auto extra = vt->second.extra_fields.find(name);
        if (extra != vt->second.extra_fields.end()) return &extra->second;
        return Field(vt->second.base, field);   // the base is a declared table (checked when parsing)
    }
    const auto it = tables_.find(key);
    if (it == tables_.end()) return nullptr;
    const auto f = it->second.fields.find(name);
    return f == it->second.fields.end() ? nullptr : &f->second;
}

FieldClass FieldPolicy::ClassOf(const std::string& table, const std::string& field) const {
    if (NormalizeIdentifierKey(field) == NormalizeIdentifierKey("Представление")) return PresentationClass(table);
    const FieldInfo* info = Field(table, field);
    return info == nullptr ? FieldClass::Unknown : info->cls;
}

}  // namespace lineage::strict
