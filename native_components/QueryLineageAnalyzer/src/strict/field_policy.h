#pragma once
#include <map>
#include <string>
#include <vector>

// Field policy of the strict checker: which fields of which tables carry protected data, built by the host from
// metadata and passed in as JSON (version 1). The component never guesses:
//   - a table, field or parameter the policy does not name is "missing"/Unknown, never open;
//   - a malformed policy (bad JSON, wrong version, unknown class or origin, wrong value type) is an error, and the
//     caller treats it as a reason to ask for confirmation;
//   - names are matched case-insensitively (NormalizeIdentifierKey); two table or field names that collide after
//     normalization merge to the stricter class (Protected over Unknown over Open), so a duplicate can only tighten;
//     two parameter names that collide must carry the same origin, otherwise the policy is malformed (the host binds
//     one value to both, and no merged origin would describe it); for this rule only, names that differ in yo / ie
//     (U+0401 / U+0415 after normalization) collide too, the lookup still tells them apart;
//   - a parameter name must be spelled in the alphabet of a query parameter (IsIdentifierName of strict_tokenizer.h:
//     ASCII and Russian letters, '_', digits after the first character), otherwise the policy is malformed.
// A missing "presentation" of a table means Protected. The field "Представление" always has the class of the
// table's presentation, whatever the "fields" map says. A tabular section is addressed as "<Table>.<Section>".
// A virtual table resolves its own "extra_fields" first, then the fields of its base table.

namespace lineage::strict {

enum class FieldClass { Open, Protected, Unknown };
enum class ParamOrigin { Client, TokenProtected, TokenUnknown, Missing };

struct FieldInfo {
    FieldClass cls = FieldClass::Unknown;
    std::vector<std::string> refs;   // target tables of a reference field (original spelling)
};

class FieldPolicy {
public:
    struct Parsed;   // defined below: it holds a FieldPolicy by value

    static Parsed FromJson(const std::string& json);

    bool HasTable(const std::string& table) const;
    // Not "FieldClass(...)": GCC rejects a member named like its type.
    FieldClass ClassOf(const std::string& table, const std::string& field) const;
    const FieldInfo* Field(const std::string& table, const std::string& field) const;
    FieldClass PresentationClass(const std::string& table) const;   // Unknown if the table is missing
    ParamOrigin OriginOfParam(const std::string& name) const;       // name without '&'
    std::string ResolveVirtualBase(const std::string& vt) const;    // "" if not virtual

    // Replaces every parameter origin (names normalized as in the JSON). Colliding names of different origins (yo /
    // ie variants included), and names outside the parameter alphabet, are false and change nothing, as in the JSON. The code checker uses it: a query built in client code knows only
    // the parameters the code sets.
    bool SetParams(const std::map<std::string, ParamOrigin>& params);
    // Gives the parameter `name` the origin `origin` if the policy does not name it. A name already present, or one
    // differing from it only in yo / ie, must carry the same origin: false otherwise (nothing changes), whichever of
    // the two origins is stricter. A name
    // outside the parameter alphabet is false too.
    bool AgreeParam(const std::string& name, ParamOrigin origin);

    struct TableData {
        FieldClass presentation = FieldClass::Protected;
        std::map<std::string, FieldInfo> fields;
    };
    struct VirtualData {
        std::string base;
        std::map<std::string, FieldInfo> extra_fields;
    };

private:
    friend class PolicyReader;

    std::map<std::string, TableData> tables_;
    std::map<std::string, VirtualData> virtuals_;
    std::map<std::string, ParamOrigin> params_;
    std::map<std::string, ParamOrigin> param_folds_;   // collision key (yo folded) -> the origin of its names
};

struct FieldPolicy::Parsed {
    bool ok = false;
    FieldPolicy policy;
    std::string error;   // fixed text without echoing policy content
};

}  // namespace lineage::strict
