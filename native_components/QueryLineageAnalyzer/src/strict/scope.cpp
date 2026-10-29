#include "strict/scope.h"

#include <utility>

#include "utf_utils.h"

namespace lineage::strict {

namespace {

// Replaces Ё (U+0401, upper-cased key form) with Е (U+0415) in an already normalized key.
std::string FoldKey(std::string key) {
    for (size_t p = 0; p + 1 < key.size(); ++p) {
        if (static_cast<unsigned char>(key[p]) == 0xD0 && static_cast<unsigned char>(key[p + 1]) == 0x81) {
            key[p + 1] = static_cast<char>(0x95);
        }
    }
    return key;
}

std::vector<std::string> SplitKeys(const std::string& dotted) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t dot = dotted.find('.', start);
        out.push_back(ScopeKey(dotted.substr(start, dot == std::string::npos ? std::string::npos : dot - start)));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return out;
}

Origin FromClass(FieldClass cls) {
    switch (cls) {
        case FieldClass::Open: return Origin::Open;
        case FieldClass::Protected: return Origin::ProtectedDirect;
        case FieldClass::Unknown: return Origin::Unknown;
    }
    return Origin::Unknown;
}

bool Opaque(const Binding& b) { return !b.known || !b.complete; }

bool TableUsable(const FieldPolicy& policy, const std::string& table) {
    return policy.HasTable(table) || !policy.ResolveVirtualBase(table).empty();
}

void AddUnique(std::vector<std::string>& list, const std::string& table) {
    const std::string key = ScopeKey(table);
    for (const std::string& t : list) {
        if (ScopeKey(t) == key) return;
    }
    list.push_back(table);
}

struct Candidate {
    const Binding* binding = nullptr;
    size_t start = 0;          // index of the first path part that names a column
    bool exact = true;         // false: matched only after e/yo folding, or matched several spellings
    size_t level = 0;          // 0 = innermost scope, 1 = its parent, ...
};

// Alias candidates: the leading parts equal one of the binding's name paths and a column part follows.
void AddAliasCandidates(const Binding& b, const std::vector<std::string>& keys, size_t level,
                        std::vector<Candidate>& out) {
    for (const auto& path : b.name_paths) {
        if (path.size() >= keys.size()) continue;
        bool folded_equal = true;
        bool exact = true;
        for (size_t i = 0; i < path.size(); ++i) {
            if (FoldKey(path[i]) != FoldKey(keys[i])) { folded_equal = false; break; }
            if (path[i] != keys[i]) exact = false;
        }
        if (!folded_equal) continue;
        Candidate c;
        c.binding = &b;
        c.start = path.size();
        c.exact = exact;
        c.level = level;
        out.push_back(c);
    }
}

// Field candidates: the first part is a column of the binding. An opaque binding may hold any column.
void AddFieldCandidate(const Binding& b, const std::string& first, const FieldPolicy& policy, size_t level,
                       const std::vector<Candidate>& alias_cands, std::vector<Candidate>& out) {
    Candidate c;
    c.binding = &b;
    c.level = level;
    if (Opaque(b)) {
        for (const Candidate& a : alias_cands) {
            if (a.binding == &b) return;   // already counted through its alias
        }
        out.push_back(c);
        return;
    }
    if (b.kind == Binding::Kind::Table) {
        if (policy.Field(b.table, first) != nullptr || ScopeKey(first) == ScopeKey("Представление")) out.push_back(c);
        return;
    }
    const std::string exact_key = ScopeKey(first);
    const std::string folded = FoldKey(exact_key);
    size_t matches = 0;
    bool exact_hit = false;
    for (const Column& col : b.columns) {
        const std::string col_key = ScopeKey(col.name);
        if (FoldKey(col_key) != folded) continue;
        ++matches;
        if (col_key == exact_key) exact_hit = true;
    }
    if (matches == 0) return;
    c.exact = matches == 1 && exact_hit;
    out.push_back(c);
}

const Column* FindColumn(const Binding& b, const std::string& name) {
    const std::string key = ScopeKey(name);
    for (const Column& col : b.columns) {
        if (ScopeKey(col.name) == key) return &col;
    }
    return nullptr;
}

// Follows `parts[from..]` through policy tables. `tables` are the candidate owners of parts[from]; a
// reference field with several targets joins the labels of all of them.
// Which object a reference points to is itself data: the origin of every reference followed on the way
// (`inter`, seeded with the origin of the column that holds the first reference) is folded into the result.
Origin Combine(Origin inter, Origin fin) {
    if (inter == Origin::Unknown || fin == Origin::Unknown) return Origin::Unknown;
    if (inter == Origin::Open || inter == Origin::Client) return fin;
    return Origin::ProtectedDerived;
}

FieldLabel Dereference(std::vector<std::string> tables, const std::vector<std::string>& parts, size_t from,
                       Origin inter, const FieldPolicy& policy, const ResolveSink& sink) {
    FieldLabel label;
    for (size_t i = from; i < parts.size(); ++i) {
        const bool last = i + 1 == parts.size();
        bool first_table = true;
        Origin origin = Origin::Unknown;
        std::vector<std::string> next;
        bool broken = false;
        Origin step = Origin::Open;
        bool have_step = false;
        for (const std::string& table : tables) {
            Origin table_origin = Origin::Unknown;
            if (!TableUsable(policy, table)) {
                sink.MissingTable(table);
                broken = true;
            } else if (last) {
                table_origin = FromClass(policy.ClassOf(table, parts[i]));
                if (const FieldInfo* info = policy.Field(table, parts[i])) {
                    for (const std::string& r : info->refs) AddUnique(label.refs, r);
                }
            } else {
                const FieldInfo* info = policy.Field(table, parts[i]);
                if (info == nullptr || info->refs.empty()) {
                    broken = true;
                } else {
                    for (const std::string& r : info->refs) AddUnique(next, r);
                    step = have_step ? JoinOrigin(step, FromClass(policy.ClassOf(table, parts[i])))
                                     : FromClass(policy.ClassOf(table, parts[i]));
                    have_step = true;
                }
            }
            origin = first_table ? table_origin : JoinOrigin(origin, table_origin);
            first_table = false;
        }
        if (last) {
            label.origin = tables.empty() || broken ? Origin::Unknown : Combine(inter, origin);
            if (label.origin == Origin::Unknown) label.refs.clear();
            label.refs_known = label.origin != Origin::Unknown;   // the policy lists the targets of every field
            return label;
        }
        if (broken || next.empty()) return FieldLabel();
        if (have_step) inter = inter == Origin::Open ? step : JoinOrigin(inter, step);
        tables = std::move(next);
    }
    return FieldLabel();
}

FieldLabel ResolveInBinding(const Binding& b, const std::vector<std::string>& parts, size_t start,
                            const FieldPolicy& policy, const ResolveSink& sink) {
    if (Opaque(b) || start >= parts.size()) return FieldLabel();
    if (b.kind == Binding::Kind::Table) return Dereference({b.table}, parts, start, Origin::Open, policy, sink);
    const Column* col = FindColumn(b, parts[start]);
    if (col == nullptr || col->origin == Origin::Unknown) return FieldLabel();
    if (start + 1 == parts.size()) {
        FieldLabel label;
        label.origin = col->origin;
        label.refs = col->refs;
        label.refs_known = col->refs_known;
        return label;
    }
    if (col->refs.empty()) return FieldLabel();
    return Dereference(col->refs, parts, start + 1, col->origin, policy, sink);
}

// A name that resolves in an outer query while an inner query has a policy table that does not list it: the
// platform would bind the name to that inner table field, so the policy cannot prove which one is meant.
// (Assumes the policy lists every field of every table; an unlisted field is treated as possibly present.)
bool ShadowedByInnerTable(const Scope& scope, size_t chosen_level, const std::string& first, const FieldPolicy& policy) {
    size_t level = 0;
    for (const Scope* s = &scope; s != nullptr && level < chosen_level; s = s->parent, ++level) {
        for (const Binding& b : s->bindings) {
            if (b.kind != Binding::Kind::Table || Opaque(b)) continue;
            if (policy.Field(b.table, first) == nullptr && ScopeKey(first) != ScopeKey("Представление")) return true;
        }
    }
    return false;
}

}  // namespace

void ResolveSink::Issue(const char* code, const Pos& pos) const {
    if (issues == nullptr) return;
    ResolveIssue issue;
    issue.code = code;
    issue.pos = pos;
    issues->push_back(std::move(issue));
}

void ResolveSink::MissingTable(const std::string& name) const {
    if (missing_tables != nullptr) missing_tables->insert(name);
}

std::string ScopeKey(const std::string& name) { return NormalizeIdentifierKey(name); }

std::string FoldedKey(const std::string& name) { return FoldKey(NormalizeIdentifierKey(name)); }

void SetBindingNames(Binding& b, const std::string& source_name, const std::string& alias) {
    b.name_paths.clear();
    if (!alias.empty()) {
        b.name_paths.push_back({ScopeKey(alias)});
        return;
    }
    if (source_name.empty()) return;
    std::vector<std::string> keys = SplitKeys(source_name);
    if (keys.size() > 1) b.name_paths.push_back(keys);
    b.name_paths.push_back({keys.back()});
}

FieldLabel ResolveFieldPath(const std::vector<std::string>& parts, const Pos& pos, const Scope& scope,
                            const FieldPolicy& policy, const ResolveSink& sink) {
    if (parts.empty()) {
        sink.Issue(kResolveUnknownName, pos);
        return FieldLabel();
    }
    std::vector<std::string> keys;
    keys.reserve(parts.size());
    for (const std::string& p : parts) keys.push_back(ScopeKey(p));

    const size_t before = sink.issues != nullptr ? sink.issues->size() : 0;
    std::vector<Candidate> alias_cands;
    if (parts.size() > 1) {
        size_t level = 0;
        for (const Scope* s = &scope; s != nullptr; s = s->parent, ++level) {
            for (const Binding& b : s->bindings) AddAliasCandidates(b, keys, level, alias_cands);
        }
    }
    std::vector<Candidate> field_cands;
    size_t level = 0;
    for (const Scope* s = &scope; s != nullptr; s = s->parent, ++level) {
        for (const Binding& b : s->bindings) AddFieldCandidate(b, parts[0], policy, level, alias_cands, field_cands);
    }

    std::vector<Candidate> all = alias_cands;
    all.insert(all.end(), field_cands.begin(), field_cands.end());
    if (all.empty()) {
        sink.Issue(kResolveUnknownName, pos);
        return FieldLabel();
    }
    bool exact = true;
    for (const Candidate& c : all) exact = exact && c.exact;
    if (all.size() > 1 || !exact || ShadowedByInnerTable(scope, all[0].level, parts[0], policy)) {
        sink.Issue(kResolveAmbiguousName, pos);
        return FieldLabel();
    }
    FieldLabel label = ResolveInBinding(*all[0].binding, parts, all[0].start, policy, sink);
    if (label.origin == Origin::Unknown) {
        label.refs.clear();
        // Every Unknown that resolution produces leaves a trace, even when a more specific issue is missing.
        if (sink.issues == nullptr || sink.issues->size() == before) sink.Issue(kResolveUnknownField, pos);
    }
    return label;
}

}  // namespace lineage::strict
