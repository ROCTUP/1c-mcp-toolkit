#include "bsl/bsl_types.h"

#include <set>
#include <utility>

#include "bsl/bsl_parser.h"
#include "bsl/bsl_taint.h"
#include "strict/strict_tokenizer.h"
#include "utf_utils.h"

namespace lineage::bsl {

std::string AllowlistKey(const std::string& name) {
    std::wstring units = Utf8ToUtf16Units(name);
    for (auto& unit : units) {
        uint32_t c = UppercaseU16(static_cast<uint32_t>(unit));
        if (c == 0x0401) c = 0x0415;   // Ё -> Е
        unit = static_cast<wchar_t>(c);
    }
    return Utf16UnitsToUtf8(units);
}

namespace {

constexpr int kMaxLoopIterations = 6;     // then the changed variables are widened to Unknown
constexpr int kMaxLoopRounds = 10;        // widening included; beyond it C_LIMIT

// ---- lookup tables built from the data in bsl_allowlist.h ------------------------------------------------------

struct Tables {
    std::map<std::string, const GlobalRule*> globals;
    std::map<std::string, const NewRule*> news;
    std::map<std::pair<int, std::string>, const MethodRule*> methods;
    std::map<std::pair<int, std::string>, const PropertyRule*> properties;
    std::set<std::string> context_names;
    std::set<std::string> query_text, property_method, eval_member;

    Tables() {
        for (const GlobalRule& r : kGlobalRules) {
            globals[AllowlistKey(r.ru)] = &r;
            globals[AllowlistKey(r.en)] = &r;
        }
        for (const NewRule& r : kNewRules) {
            news[AllowlistKey(r.ru)] = &r;
            news[AllowlistKey(r.en)] = &r;
        }
        for (const MethodRule& r : kMethodRules) {
            methods[{static_cast<int>(r.receiver), AllowlistKey(r.ru)}] = &r;
            methods[{static_cast<int>(r.receiver), AllowlistKey(r.en)}] = &r;
        }
        for (const PropertyRule& r : kPropertyRules) {
            properties[{static_cast<int>(r.receiver), AllowlistKey(r.ru)}] = &r;
            properties[{static_cast<int>(r.receiver), AllowlistKey(r.en)}] = &r;
        }
        for (const char* n : kContextNames) context_names.insert(AllowlistKey(n));
        query_text = {AllowlistKey(kQueryTextRu), AllowlistKey(kQueryTextEn)};
        property_method = {AllowlistKey(kPropertyMethodRu), AllowlistKey(kPropertyMethodEn)};
        eval_member = {AllowlistKey(kEvalMemberRu), AllowlistKey(kEvalMemberEn)};
    }
};

const Tables& T() {
    static const Tables tables;
    return tables;
}

// ---- labels ------------------------------------------------------------------------------------------------------

void JoinInto(std::optional<Origin>& acc, Origin v) { acc = acc ? JoinOrigin(*acc, v) : v; }

void JoinInto(std::optional<Origin>& acc, const std::optional<Origin>& v) {
    if (v) JoinInto(acc, *v);
}

// The label of a value computed from `dep` (a count, a test, a key lookup): open unless `dep` is tainted.
Origin Fresh(const std::optional<Origin>& dep) { return dep && IsTainted(*dep) ? Transform(*dep) : Origin::Open; }

Origin ClassLabel(strict::FieldClass c) {
    switch (c) {
        case strict::FieldClass::Open: return Origin::Open;
        case strict::FieldClass::Protected: return Origin::ProtectedDirect;
        case strict::FieldClass::Unknown: return Origin::Unknown;
    }
    return Origin::Unknown;
}

// A pc joined with a condition's label: only a tainted condition makes the code under it control-dependent.
Origin PcJoin(Origin pc, Origin cond) {
    if (!IsTainted(cond)) return pc;
    const Origin c = Transform(cond);
    return IsTainted(pc) ? JoinOrigin(pc, c) : c;
}

// ---- types and environments --------------------------------------------------------------------------------------

struct VType {
    Ty ty = Ty::Unknown;
    bool maybe_unassigned = false;               // assigned on some paths only
    const Expr* site = nullptr;                  // Query: the "Новый Запрос" node
    std::shared_ptr<const ColumnSet> columns;    // QueryResult / Selection / ValueTable / TableRow
    Origin label = Origin::Unknown;              // the value's own label (a collection's contents are in LabelState)
    std::set<const Expr*> objs;                  // collections (allocation sites) the value may be
    std::set<std::string> refs;                  // reference targets the value may have
    bool ref_unknown = true;                     // may be a reference with unknown targets
    // The cell of a query column without reference targets, moved unchanged: the description labels it like a table
    // cell (bsl_result.cpp Cell), by its column label. It never changes ref_unknown: a presentation read stays Unknown.
    bool cell = false;
};

bool SameColumns(const std::shared_ptr<const ColumnSet>& a, const std::shared_ptr<const ColumnSet>& b) {
    if (a == b) return true;
    return a && b && *a == *b;
}

bool SameVType(const VType& a, const VType& b) {
    return a.ty == b.ty && a.maybe_unassigned == b.maybe_unassigned && a.site == b.site &&
           SameColumns(a.columns, b.columns) && a.label == b.label && a.objs == b.objs && a.refs == b.refs &&
           a.ref_unknown == b.ref_unknown && a.cell == b.cell;
}

VType Typed(Ty ty, Origin label) {
    VType v;
    v.ty = ty;
    v.label = label;
    v.ref_unknown = ty == Ty::Unknown;
    return v;
}

bool IsCollection(Ty ty) {
    return ty == Ty::Array || ty == Ty::Structure || ty == Ty::Map || ty == Ty::ValueTable || ty == Ty::ValueList;
}

// A value whose type is not proven may be a reference.
bool MayBeRef(const VType& v) {
    return v.ty == Ty::Ref || (v.ty == Ty::Unknown && (v.ref_unknown || !v.refs.empty()));
}

// Two values that may reach the same place. Different types, or different Запрос objects, join to Unknown; the
// labels, collections and reference targets of both sides are kept.
VType JoinType(const VType& a, const VType& b) {
    VType r;
    r.maybe_unassigned = a.maybe_unassigned || b.maybe_unassigned;
    r.label = JoinOrigin(a.label, b.label);
    r.objs = a.objs;
    r.objs.insert(b.objs.begin(), b.objs.end());
    r.refs = a.refs;
    r.refs.insert(b.refs.begin(), b.refs.end());
    r.ref_unknown = a.ref_unknown || b.ref_unknown;
    r.cell = a.cell && b.cell;
    if (a.ty != b.ty) return r;
    if (a.ty == Ty::Query && a.site != b.site) return r;
    r.ty = a.ty;
    r.site = a.site;
    if (SameColumns(a.columns, b.columns)) r.columns = a.columns;
    return r;
}

using Env = std::map<std::string, VType>;

VType MaybeUnassigned() {
    VType v;
    v.maybe_unassigned = true;
    v.label = Origin::Open;
    v.ref_unknown = false;
    return v;
}

Env JoinEnv(const Env& a, const Env& b) {
    Env out;
    for (const auto& kv : a) {
        const auto it = b.find(kv.first);
        out[kv.first] = JoinType(kv.second, it == b.end() ? MaybeUnassigned() : it->second);
    }
    for (const auto& kv : b) {
        if (a.find(kv.first) == a.end()) out[kv.first] = JoinType(MaybeUnassigned(), kv.second);
    }
    return out;
}

bool IsStringLit(const Expr& e) { return e.kind == Expr::K::Lit && e.lit == Expr::L::String; }

// A literal, or a concatenation of literals. Nothing else counts as constant.
bool Fold(const Expr& e, std::string& out) {
    if (IsStringLit(e)) {
        out = e.text;
        return true;
    }
    if (e.kind == Expr::K::Binary && e.text == "+" && e.args.size() == 2) {
        std::string left, right;
        if (!Fold(*e.args[0], left) || !Fold(*e.args[1], right)) return false;
        out = left + right;
        return true;
    }
    return false;
}

// The key list of "Новый Структура(<keys>, ...)": comma-separated identifiers. False when a key is not an identifier
// (then the platform fails at run time, and the description cannot name the fields).
bool SplitStructureKeys(const std::string& list, std::vector<std::string>& out) {
    out.clear();
    const char* const blanks = " \t\r\n";
    if (list.find_first_not_of(blanks) == std::string::npos) return true;
    size_t start = 0;
    while (true) {
        const size_t comma = list.find(',', start);
        std::string key = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const size_t b = key.find_first_not_of(blanks);
        const size_t f = key.find_last_not_of(blanks);
        if (b == std::string::npos) return false;
        key = key.substr(b, f - b + 1);
        if (!IsPathKey(key)) return false;
        out.push_back(std::move(key));
        if (comma == std::string::npos) return true;
        start = comma + 1;
    }
}

bool IsReservedParam(const std::string& name) {
    static const std::string prefix = AllowlistKey("МСПТокен");
    return AllowlistKey(name).compare(0, prefix.size(), prefix) == 0;
}

// ---- variables assigned inside a block (loops and Попытка join them) -----------------------------------------------

void CollectExpr(const Expr& e, std::set<std::string>& out) {
    if (e.kind == Expr::K::MethodCall && e.args.size() == 3 && e.args[2] && e.args[2]->kind == Expr::K::Var &&
        T().property_method.count(AllowlistKey(e.text))) {
        out.insert(AllowlistKey(e.args[2]->text));
    }
    for (const ExprP& a : e.args) {
        if (a) CollectExpr(*a, out);
    }
}

void CollectBody(const Body& b, std::set<std::string>& out);

void CollectStmt(const Stmt& s, std::set<std::string>& out) {
    if (s.kind == Stmt::K::Assign && !s.exprs.empty() && s.exprs[0] && s.exprs[0]->kind == Expr::K::Var) {
        out.insert(AllowlistKey(s.exprs[0]->text));
    }
    if (s.kind == Stmt::K::ForEach || s.kind == Stmt::K::ForTo) out.insert(AllowlistKey(s.var));
    for (const ExprP& e : s.exprs) {
        if (e) CollectExpr(*e, out);
    }
    for (const Body& b : s.blocks) CollectBody(b, out);
}

void CollectBody(const Body& b, std::set<std::string>& out) {
    for (const StmtP& s : b.stmts) {
        if (s) CollectStmt(*s, out);
    }
}

// A loop or a Попытка body: every value assigned to a variable in it is joined into `acc` (the state at the loop head
// and after the loop, or at the start of Исключение). A Попытка also collects the inputs of tainted operations.
struct Frame {
    std::map<std::string, VType> acc;
    bool is_try = false;
    std::optional<Origin> taint;   // Попытка: inputs of the tainted operations of the body
    std::optional<Origin> jump;    // loop: pc of every Прервать / Продолжить under a tainted condition
};

bool SameAcc(const std::map<std::string, VType>& a, const std::map<std::string, VType>& b) {
    if (a.size() != b.size()) return false;
    for (auto ia = a.begin(), ib = b.begin(); ia != a.end(); ++ia, ++ib) {
        if (ia->first != ib->first || !SameVType(ia->second, ib->second)) return false;
    }
    return true;
}

// ---- the checker -----------------------------------------------------------------------------------------------------

class Checker {
public:
    Checker(const TypeCheckOptions& options, LabelState& state)
        : options_(options), state_(state), work_(options.work ? *options.work : own_work_) {}

    CodeAnalysis Run(const Body& body) {
        ParseError err;
        const bool valid = ValidateBody(body, err);
        if (!valid) {
            Add(err.code.empty() ? "C_PARSE" : err.code.c_str(), err.pos);
        } else {
            Block(body);
        }
        const auto it = env_.find(AllowlistKey("Результат"));
        result_.result_label =
            it == env_.end() || it->second.maybe_unassigned ? Origin::Unknown : ValueLabel(it->second);
        if (valid) {
            DescribeFinalResult(it);
        } else {   // nothing was walked: the result is not known
            result_.result_labels.assign(1, ResultPath{"$", "unknown"});
            result_.result_reasons.assign(1, CodeReason{"C_UNLABELED_RESULT", Pos{}});
        }
        result_.verdict =
            result_.reasons.empty() && result_.missing_tables.empty() ? CodeVerdict::Safe : CodeVerdict::Confirm;
        return std::move(result_);
    }

private:
    const TypeCheckOptions& options_;
    LabelState& state_;
    CodeAnalysis result_;
    Env env_;
    std::set<std::pair<std::string, size_t>> seen_;
    std::map<std::string, std::string> spelling_;   // allowlist key -> first spelling (upper case, ё kept)
    std::set<const Expr*> noted_texts_;
    Origin pc_ = Origin::Open;                      // Open: no control dependence on tainted data
    std::vector<Frame*> frames_;
    int in_query_text_ = 0;                         // > 0 while a query text is evaluated (its literals are not values)
    size_t own_work_ = 0;
    size_t& work_;                                  // the analysis-wide budget (TypeCheckOptions::work)
    bool exhausted_ = false;

    void Add(const char* code, const Pos& pos) {
        if (!seen_.insert({code, pos.offset}).second) return;
        result_.reasons.push_back(CodeReason{code, pos});
    }

    // A call whose callee the allowlist does not know can do anything, writes included (R_UNKNOWN_EFFECT).
    void AddUnknownEffect(const Pos& pos) {
        if (!seen_.insert({"R_UNKNOWN_EFFECT", pos.offset}).second) return;
        result_.risks.push_back(CodeReason{"R_UNKNOWN_EFFECT", pos});
    }

    // A use of a Запрос value the walk does not model (see "Lost queries" in bsl_types.h); a no-op on other types.
    // `text`: the text may have changed too, so not even the column names are known.
    void LoseQuery(const VType& v, bool text) {
        if (v.ty == Ty::Query) LoseSite(v.site, text);
    }

    void LoseSite(const Expr* site, bool text) {
        if (!site) return;
        (text ? state_.untracked_queries : state_.unlabeled_queries).insert(site);
    }

    // The environment key of a variable. Two spellings that differ only in ё / е are reported.
    std::string VarKey(const std::string& name, const Pos& pos) {
        const std::string key = AllowlistKey(name);
        const std::string raw = NormalizeIdentifierKey(name);
        const auto it = spelling_.find(key);
        if (it == spelling_.end()) {
            spelling_[key] = raw;
        } else if (it->second != raw) {
            Add("C_AMBIGUOUS_IDENT", pos);
        }
        return key;
    }

    // The walk's budget: every statement, and every variable of every environment copied for a branch or a loop.
    void Charge(size_t n) {
        if (exhausted_) return;
        work_ += n;
        if (work_ <= kMaxCodeWork) return;
        Add("C_LIMIT", Pos{});
        exhausted_ = true;
    }

    void Block(const Body& b) {
        for (const StmtP& s : b.stmts) {
            if (exhausted_) return;
            Statement(*s);
        }
    }

    // The per-path labels of Результат. An unassigned Результат keeps the host's initial Неопределено.
    void DescribeFinalResult(Env::const_iterator it) {
        if (it == env_.end()) {
            DescribeResult(nullptr, state_, result_);
            return;
        }
        const VType& v = it->second;
        ResultValue value;
        value.ty = v.ty;
        value.label = v.label;
        value.maybe_ref = MayBeRef(v) && !v.cell;
        value.objs = v.objs;
        value.columns = v.columns;
        DescribeResult(&value, state_, result_);
    }

    // What a stored value is, for the Результат description: a scalar label, an untainted reference, a collection,
    // or something the description does not model.
    void NoteSlot(SlotLabels& s, const VType& value) const {
        const Origin label = WithPc(value.label);
        switch (value.ty) {
            case Ty::Undefined:
            case Ty::Null:
            case Ty::Boolean:
            case Ty::Number:
            case Ty::String:
            case Ty::Date:
            case Ty::Uuid: JoinInto(s.scalar, label); return;
            case Ty::Ref:
                if (IsTainted(label)) {
                    JoinInto(s.scalar, label);
                } else {
                    s.ref = true;
                }
                return;
            case Ty::Array:
            case Ty::Structure:
            case Ty::Map:
                s.nested.insert(value.objs.begin(), value.objs.end());
                if (value.objs.empty()) s.opaque = true;
                return;
            case Ty::Unknown:
                if (!value.objs.empty()) {
                    s.nested.insert(value.objs.begin(), value.objs.end());
                    if (IsTainted(label) || MayBeRef(value)) s.opaque = true;
                } else if (MayBeRef(value) && !IsTainted(label) && !value.cell) {
                    s.opaque = true;
                } else {
                    JoinInto(s.scalar, label);
                }
                return;
            default: s.opaque = true; return;
        }
    }

    // ---- labels of values and collections ----

    Origin WithPc(Origin v) const { return IsTainted(pc_) ? JoinOrigin(v, pc_) : v; }

    // An operation over a tainted input: a reason, and the Исключение blocks of enclosing Попытка depend on it.
    void TaintedOp(const Pos& pos, Origin input) {
        Add("C_FUNC_OVER_PROTECTED", pos);
        for (Frame* f : frames_) {
            if (f->is_try) JoinInto(f->taint, Transform(input));
        }
    }

    void CheckOp(const Pos& pos, Origin input) {
        if (IsTainted(input)) TaintedOp(pos, input);
    }

    // Inserting a key compares it with every key already stored (de-duplication), an equality oracle: a tainted key,
    // or any key inserted next to tainted ones, needs confirmation.
    void KeyOp(const Pos& pos, Origin input) {
        if (!IsTainted(input)) return;
        Add("C_TAINTED_KEY", pos);
        for (Frame* f : frames_) {
            if (f->is_try) JoinInto(f->taint, Transform(input));
        }
    }

    const ObjectLabels* FindObj(const Expr* site) const {
        const auto it = state_.objects.find(site);
        return it == state_.objects.end() ? nullptr : &it->second;
    }

    std::optional<Origin> ObjLabel(const Expr* site, std::set<const Expr*>& visited) const {
        std::optional<Origin> acc;
        if (!visited.insert(site).second) return acc;
        const ObjectLabels* o = FindObj(site);
        if (!o) return acc;
        JoinInto(acc, o->content);
        JoinInto(acc, o->shape);
        JoinInto(acc, o->keys);
        for (const Expr* n : o->nested) JoinInto(acc, ObjLabel(n, visited));
        return acc;
    }

    // Everything a value may carry: its own label (for a collection or a query result only when the pc tainted it),
    // the contents of the collections it may be, and the labels of its columns. Empty for an empty collection.
    std::optional<Origin> HeldLabel(const VType& v) const {
        std::optional<Origin> acc;
        const bool holder = IsCollection(v.ty) || v.ty == Ty::QueryResult || v.ty == Ty::Selection ||
                            v.ty == Ty::TableRow;
        if (!holder || IsTainted(v.label)) acc = v.label;
        std::set<const Expr*> visited;
        for (const Expr* o : v.objs) JoinInto(acc, ObjLabel(o, visited));
        if (v.ty == Ty::QueryResult || v.ty == Ty::Selection || v.ty == Ty::ValueTable || v.ty == Ty::TableRow) {
            if (v.columns) {
                for (const auto& c : v.columns->columns) JoinInto(acc, c.second.origin);
            } else if (v.objs.empty()) {
                JoinInto(acc, Origin::Unknown);
            }
        }
        return acc;
    }

    Origin ValueLabel(const VType& v) const {
        const std::optional<Origin> held = HeldLabel(v);
        return held ? *held : Origin::Client;
    }

    // What the size / key set of a collection depends on.
    std::optional<Origin> ShapeOf(const VType& v) const {
        std::optional<Origin> acc;
        if (IsTainted(v.label)) acc = v.label;
        for (const Expr* o : v.objs) {
            if (const ObjectLabels* ol = FindObj(o)) JoinInto(acc, ol->shape);
        }
        return acc;
    }

    std::optional<Origin> KeysOf(const VType& v) const {
        std::optional<Origin> acc;
        for (const Expr* o : v.objs) {
            if (const ObjectLabels* ol = FindObj(o)) JoinInto(acc, ol->keys);
        }
        return acc;
    }

    // An element read from a collection: Unknown type, the contents' label and reference targets.
    VType ElementOf(const VType& coll, Origin index_label) const {
        VType e;
        e.ref_unknown = false;
        std::optional<Origin> acc;
        if (IsTainted(coll.label)) acc = coll.label;
        JoinInto(acc, ShapeOf(coll));
        if (IsTainted(index_label)) JoinInto(acc, Transform(index_label));
        for (const Expr* o : coll.objs) {
            const ObjectLabels* ol = FindObj(o);
            if (!ol) continue;
            JoinInto(acc, ol->content);
            e.refs.insert(ol->refs.begin(), ol->refs.end());
            e.ref_unknown = e.ref_unknown || ol->ref_unknown;
            e.objs.insert(ol->nested.begin(), ol->nested.end());
        }
        if (coll.objs.empty()) {
            acc = Origin::Unknown;
            e.ref_unknown = true;
        }
        e.label = acc ? *acc : Origin::Open;
        return e;
    }

    // Stores a value into every collection `coll` may be (an element, a structure value, a map value).
    void Store(const VType& coll, const VType& value, const Pos& pos, const std::string* field = nullptr) {
        if (coll.objs.empty()) {
            Add("C_INTERNAL", pos);   // a collection without an allocation site: its contents could not be tracked
            return;
        }
        // An empty collection adds nothing now; its later contents are reached through `nested`.
        std::optional<Origin> label = HeldLabel(value);
        if (label) label = WithPc(*label);
        for (const Expr* o : coll.objs) {
            ObjectLabels& ol = state_.objects[o];
            SlotLabels* slot = &ol.elem;
            if (ol.kind == Ty::Structure && field) {
                FieldSlot& f = ol.fields[AllowlistKey(*field)];
                if (f.name.empty()) {
                    f.name = *field;
                } else if (NormalizeIdentifierKey(f.name) != NormalizeIdentifierKey(*field)) {
                    f.collide = true;   // spellings that differ in ё / е: the serialized one is not known
                }
                slot = &f.slot;
            }
            NoteSlot(*slot, value);
            JoinInto(ol.content, label);
            if (IsTainted(pc_)) JoinInto(ol.shape, pc_);
            if (MayBeRef(value)) {
                ol.refs.insert(value.refs.begin(), value.refs.end());
                ol.ref_unknown = ol.ref_unknown || value.ref_unknown;
            }
            ol.nested.insert(value.objs.begin(), value.objs.end());
        }
    }

    void StoreKey(const VType& coll, const VType& key, const Pos& pos) {
        const Origin label = WithPc(ValueLabel(key));
        std::optional<Origin> input = label;
        JoinInto(input, KeysOf(coll));
        KeyOp(pos, *input);
        for (const Expr* o : coll.objs) {
            JoinInto(state_.objects[o].keys, label);
            NoteSlot(state_.objects[o].key_slot, key);
        }
    }

    // The label of a presentation read ("x" + Ссылка, Строка(Ссылка)): the presentation class of every target,
    // Unknown when the value may be a reference whose targets are not known.
    std::optional<Origin> Presentation(const VType& v) {
        std::optional<Origin> acc;
        if (!MayBeRef(v)) return acc;
        for (const std::string& t : v.refs) {
            if (!options_.policy) {
                JoinInto(acc, Origin::Unknown);
                continue;
            }
            const strict::FieldClass cls = options_.policy->PresentationClass(t);
            if (cls == strict::FieldClass::Unknown && !options_.policy->HasTable(t) &&
                options_.policy->ResolveVirtualBase(t).empty()) {
                result_.missing_tables.insert(t);
            }
            JoinInto(acc, ClassLabel(cls));
        }
        if (v.ref_unknown || v.refs.empty()) JoinInto(acc, Origin::Unknown);
        if (IsTainted(v.label)) JoinInto(acc, Transform(v.label));
        return acc;
    }

    // The label of a value converted to a string or a number by a function.
    Origin ReadAsValue(const VType& v) {
        std::optional<Origin> acc = ValueLabel(v);
        JoinInto(acc, Presentation(v));
        return *acc;
    }

    // A literal in a name position (parameter, key, column, type): a token there is not a value.
    void NameLiteral(const Expr& e) {
        if (!IsStringLit(e)) return;
        TokenBinding b;
        if (ClassifyTokenLiteral(e.text, options_.tokens, &b) == TokenUse::None) return;
        result_.bound_tokens.erase(&e);
        Add("C_TOKEN_AS_NAME", e.pos);
    }

    // ---- expressions ----

    // `recv` is true when the value is only used as the receiver of a call, member or index.
    // A Запрос value outside a receiver position (argument, element, copy) is C_QUERY_ESCAPES; the one exception is
    // the allocation assigned straight to a variable.
    VType Eval(const Expr& e, bool recv = false) {
        const VType t = EvalInner(e);
        if (!recv && t.ty == Ty::Query) {
            Add("C_QUERY_ESCAPES", e.pos);
            LoseQuery(t, true);
        }
        return t;
    }

    VType EvalInner(const Expr& e) {
        switch (e.kind) {
            case Expr::K::Lit: return EvalLit(e);
            case Expr::K::Var: return EvalVar(e);
            case Expr::K::Member: return EvalMember(e);
            case Expr::K::Index: return EvalIndex(e);
            case Expr::K::Call: return EvalCall(e);
            case Expr::K::MethodCall: return EvalMethod(e);
            case Expr::K::New: return EvalNew(e);
            case Expr::K::Ternary: return EvalTernary(e);
            case Expr::K::Unary: return EvalUnary(e);
            case Expr::K::Binary: return EvalBinary(e);
            default: Add("C_PARSE", e.pos); return VType();
        }
    }

    VType EvalLit(const Expr& e) {
        switch (e.lit) {
            case Expr::L::Number: return Typed(Ty::Number, Origin::Client);
            case Expr::L::String: return EvalString(e);
            case Expr::L::Date: return Typed(Ty::Date, Origin::Client);
            case Expr::L::True:
            case Expr::L::False: return Typed(Ty::Boolean, Origin::Client);
            case Expr::L::Undefined: return Typed(Ty::Undefined, Origin::Client);
            case Expr::L::Null: return Typed(Ty::Null, Origin::Client);
            default: return VType();
        }
    }

    VType EvalString(const Expr& e) {
        VType v = Typed(Ty::String, Origin::Client);
        if (in_query_text_ > 0) return v;   // checked as a query text in NoteQueryText
        TokenBinding b;
        switch (ClassifyTokenLiteral(e.text, options_.tokens, &b)) {
            case TokenUse::Bound: {
                // The host's real value: its type is not known, so it is not assumed to be a string.
                result_.bound_tokens[&e] = b.index;
                VType bound;
                bound.label = b.origin;
                return bound;
            }
            case TokenUse::InString: Add("C_TOKEN_IN_STRING", e.pos); break;
            case TokenUse::Unknown:
                Add("C_TOKEN_UNKNOWN", e.pos);
                v.label = Origin::Unknown;
                break;
            case TokenUse::None: break;
        }
        return v;
    }

    VType EvalVar(const Expr& e) {
        const std::string key = VarKey(e.text, e.pos);
        const auto it = env_.find(key);
        if (it == env_.end() || it->second.maybe_unassigned) {
            Add("C_UNKNOWN_IDENT", e.pos);
            return VType();
        }
        return it->second;
    }

    static VType ColumnType(const ColumnInfo& c, const VType& holder) {
        VType v;
        v.label = c.origin;
        if (IsTainted(holder.label)) v.label = JoinOrigin(v.label, holder.label);
        if (!c.refs_known) {
            v.refs.insert(c.refs.begin(), c.refs.end());   // may be a reference with other targets: Unknown type
        } else if (!c.refs.empty()) {
            v.ty = Ty::Ref;
            v.refs.insert(c.refs.begin(), c.refs.end());
            v.ref_unknown = false;
        } else {
            v.cell = true;   // provably not a reference
        }
        return v;
    }

    VType EvalMember(const Expr& e) {
        const VType obj = Eval(*e.args[0], true);
        const std::string key = AllowlistKey(e.text);
        if (obj.ty == Ty::Unknown) {
            Add("C_UNKNOWN_RECEIVER", e.pos);
            return VType();
        }
        if (obj.ty == Ty::Selection || obj.ty == Ty::TableRow) {
            if (obj.columns) {
                const auto it = obj.columns->columns.find(key);
                if (it != obj.columns->columns.end()) return ColumnType(it->second, obj);
            }
            Add("C_UNKNOWN_MEMBER", e.pos);
            return VType();
        }
        if (obj.ty == Ty::Ref) return Dereference(obj, e);
        const auto it = T().properties.find({static_cast<int>(obj.ty), key});
        if (it == T().properties.end()) {
            Add("C_UNKNOWN_MEMBER", e.pos);
            LoseQuery(obj, false);   // З.Параметры and any other member
            return VType();
        }
        return Typed(it->second->result, Fresh(ShapeOf(obj)));
    }

    // Ссылка.Реквизит: the field's policy class over every target; a field the policy does not list is refused.
    VType Dereference(const VType& obj, const Expr& e) {
        const strict::FieldPolicy* policy = options_.policy;
        if (!policy || obj.refs.empty() || obj.ref_unknown) {
            Add("C_UNKNOWN_MEMBER", e.pos);
            return VType();
        }
        const bool presentation = NormalizeIdentifierKey(e.text) == NormalizeIdentifierKey("Представление");
        std::optional<Origin> label;
        VType v;
        v.ref_unknown = false;
        bool all_refs = !presentation;
        for (const std::string& t : obj.refs) {
            if (!policy->HasTable(t) && policy->ResolveVirtualBase(t).empty()) {
                result_.missing_tables.insert(t);
                JoinInto(label, Origin::Unknown);
                all_refs = false;
                continue;
            }
            const strict::FieldInfo* field = policy->Field(t, e.text);
            if (!field && !presentation) {
                Add("C_UNKNOWN_MEMBER", e.pos);
                return VType();
            }
            JoinInto(label, ClassLabel(policy->ClassOf(t, e.text)));
            if (field && !field->refs.empty()) {
                v.refs.insert(field->refs.begin(), field->refs.end());
            } else {
                all_refs = false;
            }
        }
        if (IsTainted(obj.label)) {
            TaintedOp(e.pos, obj.label);   // which row is read depends on the tainted reference
            JoinInto(label, Transform(obj.label));
        }
        v.label = label ? *label : Origin::Unknown;
        if (all_refs && !v.refs.empty()) {
            v.ty = Ty::Ref;
        } else {
            v.ref_unknown = !presentation;
        }
        return v;
    }

    VType EvalIndex(const Expr& e) {
        const VType obj = Eval(*e.args[0], true);
        const VType index = Eval(*e.args[1]);
        return IndexRead(obj, index, *e.args[1], e.pos);
    }

    // Key and index checks shared by reads and writes of "obj[index]". False when the receiver has no index.
    bool IndexCheck(const VType& obj, const VType& index, const Expr& index_expr, const Pos& pos) {
        switch (obj.ty) {
            case Ty::Unknown:
                Add("C_UNKNOWN_RECEIVER", pos);
                return false;
            case Ty::ValueTable:
            case Ty::Array:
                CheckOp(pos, ValueLabel(index));
                return true;
            case Ty::Structure:
                if (!IsStringLit(index_expr)) Add("C_DYNAMIC_KEY", index_expr.pos);
                NameLiteral(index_expr);
                return true;
            case Ty::Map: {
                if (index_expr.kind != Expr::K::Lit) Add("C_DYNAMIC_KEY", index_expr.pos);
                std::optional<Origin> input = ValueLabel(index);
                JoinInto(input, KeysOf(obj));
                CheckOp(pos, *input);
                return true;
            }
            default:
                Add("C_UNKNOWN_INDEX", pos);
                LoseQuery(obj, false);
                return false;
        }
    }

    VType IndexRead(const VType& obj, const VType& index, const Expr& index_expr, const Pos& pos) {
        if (!IndexCheck(obj, index, index_expr, pos)) return VType();
        if (obj.ty == Ty::ValueTable) {
            VType row = Typed(Ty::TableRow, Fresh(ShapeOf(obj)));
            if (IsTainted(index.label)) row.label = JoinOrigin(row.label, Transform(ValueLabel(index)));
            row.columns = obj.columns;
            row.objs = obj.objs;
            return row;
        }
        std::optional<Origin> key = ValueLabel(index);
        if (obj.ty == Ty::Map) JoinInto(key, KeysOf(obj));
        return ElementOf(obj, *key);
    }

    bool CheckArity(int given, int min_args, int max_args, const Pos& pos) {
        if (given >= min_args && given <= max_args) return true;
        Add("C_BAD_ARGS", pos);
        return false;
    }

    VType EvalCall(const Expr& e) {
        std::vector<VType> args;
        for (const ExprP& a : e.args) args.push_back(Eval(*a));
        const auto it = T().globals.find(AllowlistKey(e.text));
        if (it == T().globals.end()) {
            Add("C_UNKNOWN_IDENT", e.pos);
            AddUnknownEffect(e.pos);
            return VType();
        }
        const GlobalRule& rule = *it->second;
        if (CheckArity(static_cast<int>(e.args.size()), rule.min_args, rule.max_args, e.pos) &&
            rule.arg0 == ArgRule::LiteralString) {
            if (!IsStringLit(*e.args[0])) Add("C_DYNAMIC_KEY", e.args[0]->pos);
            NameLiteral(*e.args[0]);
        }
        // Open: the arguments are read as values (a reference as its presentation); Propagate: as they are.
        std::optional<Origin> input;
        for (const VType& a : args) JoinInto(input, rule.taint == Taint::Open ? ReadAsValue(a) : ValueLabel(a));
        if (rule.taint == Taint::Confirm) Add("C_FUNC_OVER_PROTECTED", e.pos);
        if (input) CheckOp(e.pos, *input);
        return Typed(rule.result, input ? Transform(*input) : Origin::Open);
    }

    VType EvalNew(const Expr& e) {
        const auto it = T().news.find(AllowlistKey(e.text));
        const NewRule* rule = it == T().news.end() ? nullptr : it->second;
        const bool query = rule && rule->result == Ty::Query;
        std::vector<VType> args;
        for (const ExprP& a : e.args) {
            if (query) ++in_query_text_;
            args.push_back(Eval(*a));
            if (query) --in_query_text_;
        }
        if (!rule) {
            Add("C_UNKNOWN_TYPE", e.pos);
            AddUnknownEffect(e.pos);
            return VType();
        }
        CheckArity(static_cast<int>(e.args.size()), rule->min_args, rule->max_args, e.pos);
        VType v = Typed(rule->result, Origin::Client);
        if (query) {
            v.site = &e;
            if (e.args.size() == 1) NoteQueryText(&e, *e.args[0]);
            if (e.args.size() > 1) LoseSite(&e, true);   // C_BAD_ARGS: no text recorded
            return v;
        }
        if (rule->result == Ty::Uuid) {
            std::optional<Origin> input;
            for (const VType& a : args) JoinInto(input, ReadAsValue(a));
            if (input) {
                CheckOp(e.pos, *input);
                v.label = Transform(*input);
            }
            return v;
        }
        v.objs.insert(&e);
        state_.objects[&e].kind = rule->result;   // the collection exists even when nothing is stored in it
        std::vector<std::string> keys;
        const bool keys_ok = rule->result == Ty::Structure && !e.args.empty() && IsStringLit(*e.args[0]) &&
                             SplitStructureKeys(e.args[0]->text, keys) && keys.size() + 1 == e.args.size();
        for (size_t i = 0; i < args.size(); ++i) {
            if (rule->result == Ty::Structure && i > 0) {
                // values; the first argument is the key list
                Store(v, args[i], e.args[i]->pos, keys_ok ? &keys[i - 1] : nullptr);
                continue;
            }
            if (rule->result == Ty::Structure) NameLiteral(*e.args[i]);
            CheckOp(e.args[i]->pos, ValueLabel(args[i]));   // a size, a key list or a source collection
            if (rule->result != Ty::Structure) Store(v, args[i], e.args[i]->pos);
        }
        return v;
    }

    void NoteQueryText(const Expr* site, const Expr& text_expr) {
        QueryTextUse use;
        if (!Fold(text_expr, use.text)) {
            Add("C_DYNAMIC_QUERY", text_expr.pos);
            LoseSite(site, true);   // the site's other texts do not describe this one
            return;
        }
        TokenBinding b;
        if (ClassifyTokenLiteral(use.text, options_.tokens, &b) != TokenUse::None) Add("C_TOKEN_IN_QUERY", text_expr.pos);
        for (const strict::Token& t : strict::Tokenize(use.text)) {
            if (t.kind == strict::Tk::Parameter && t.text.size() > 1 && IsReservedParam(t.text.substr(1))) {
                Add("C_RESERVED", text_expr.pos);
            }
        }
        if (!noted_texts_.insert(&text_expr).second) return;
        use.site = site;
        use.text_expr = &text_expr;
        result_.query_texts.push_back(std::move(use));
    }

    VType EvalMethod(const Expr& e) {
        const Expr& recv_expr = *e.args[0];
        const int nargs = static_cast<int>(e.args.size()) - 1;
        if (recv_expr.kind == Expr::K::Var && Unassigned(recv_expr)) {
            // "Имя.Метод(...)" on a name the code never assigned: a module call, or a context object.
            for (int i = 1; i <= nargs; ++i) Eval(*e.args[i]);
            const bool context = T().context_names.count(AllowlistKey(recv_expr.text)) != 0;
            Add(context ? "C_UNKNOWN_IDENT" : "C_COMMON_MODULE", recv_expr.pos);
            AddUnknownEffect(e.pos);
            return VType();
        }
        const VType recv = Eval(recv_expr, true);
        const std::string key = AllowlistKey(e.text);
        if (T().eval_member.count(key)) Add("C_EVAL", e.pos);

        const MethodRule* rule = nullptr;
        if (recv.ty == Ty::Unknown) {
            Add("C_UNKNOWN_RECEIVER", e.pos);
            AddUnknownEffect(e.pos);
        } else {
            const auto it = T().methods.find({static_cast<int>(recv.ty), key});
            if (it == T().methods.end()) {
                Add("C_UNKNOWN_METHOD", e.pos);
                AddUnknownEffect(e.pos);
                LoseQuery(recv, false);
            } else {
                rule = it->second;
            }
        }
        const bool out_param = rule && nargs == 2 && T().property_method.count(key);
        std::vector<VType> args;
        for (int i = 1; i <= nargs; ++i) args.push_back(out_param && i == 2 ? VType() : Eval(*e.args[i]));
        if (!rule) return VType();
        const bool arity = CheckArity(nargs, rule->min_args, rule->max_args, e.pos);
        if (!arity) LoseQuery(recv, false);
        if (arity && rule->arg0 == ArgRule::LiteralString) {
            if (!IsStringLit(*e.args[1])) Add(rule->receiver == Ty::Query ? "C_DYNAMIC_PARAM" : "C_DYNAMIC_KEY", e.args[1]->pos);
            NameLiteral(*e.args[1]);
        }
        const VType result = arity ? MethodResult(*rule, recv, args, e) : VType();
        if (out_param) AssignOutParam(*e.args[2], ElementOf(recv, Origin::Open));
        return result;
    }

    // Labels of an allowlisted method call (the arity is already checked).
    VType MethodResult(const MethodRule& rule, const VType& recv, const std::vector<VType>& args, const Expr& e) {
        const std::string m = AllowlistKey(rule.ru);
        switch (rule.receiver) {
            case Ty::Query:
                if (m == AllowlistKey("УстановитьПараметр")) {
                    BindParam(recv, *e.args[1], args[1], e.args[1]->pos);
                    return Typed(Ty::Undefined, Origin::Client);
                }
                return Executed(recv);
            case Ty::QueryResult:
            case Ty::Selection:
            case Ty::ColumnCollection: {
                VType v = Typed(rule.result, Fresh(ShapeOf(recv)));
                if (rule.result == Ty::Selection || rule.result == Ty::ValueTable) {
                    v.label = recv.label;
                    v.columns = recv.columns;
                }
                return v;
            }
            case Ty::ValueTable:
                if (m == AllowlistKey("ВыгрузитьКолонку")) return UnloadColumn(recv, *e.args[1], e);
                if (m == AllowlistKey("Сортировать")) return SortTable(recv, *e.args[1], e);
                return Typed(rule.result, Fresh(ShapeOf(recv)));
            case Ty::Array:
            case Ty::Structure:
            case Ty::Map: return CollectionMethod(m, recv, args, e);
            case Ty::Ref:
            case Ty::MetaObject:
                // A scalar receiver: the method reads the value itself.
                CheckOp(e.pos, recv.label);
                return Typed(rule.result, Transform(recv.label));
            default: break;
        }
        Add("C_INTERNAL", e.pos);
        return VType();
    }

    VType Executed(const VType& query) {
        VType v = Typed(Ty::QueryResult, IsTainted(query.label) ? query.label : Origin::Open);
        if (!options_.columns_for_query || state_.untracked_queries.count(query.site)) return v;
        v.columns = options_.columns_for_query(query.site);
        if (v.columns && state_.unlabeled_queries.count(query.site)) {
            auto unlabeled = std::make_shared<ColumnSet>(*v.columns);
            for (auto& c : unlabeled->columns) {
                c.second.origin = Origin::Unknown;
                c.second.refs_known = false;
            }
            v.columns = std::move(unlabeled);
        }
        return v;
    }

    void BindParam(const VType& query, const Expr& name_expr, const VType& value, const Pos& pos) {
        if (!IsStringLit(name_expr)) {   // C_DYNAMIC_PARAM already
            LoseQuery(query, false);
            return;
        }
        // A name no query text can spell (a space, an invisible or look-alike character): the host may trim or fold
        // it onto another parameter. Still bound, so that the parameter set fails the policy and no column keeps a
        // label of the other spelling.
        if (!strict::IsIdentifierName(name_expr.text)) Add("C_BAD_PARAM_NAME", pos);
        if (IsReservedParam(name_expr.text)) {
            Add("C_RESERVED", pos);
            LoseQuery(query, false);   // not bound: the host may bind it with its own value
            return;
        }
        const Origin label = WithPc(ValueLabel(value));
        auto& params = state_.params[query.site];
        const std::string key = AllowlistKey(name_expr.text);
        const auto it = params.find(key);
        if (it == params.end()) {
            ParamBinding b;
            b.label = label;
            b.spellings.insert(name_expr.text);
            params.emplace(key, std::move(b));
        } else {
            it->second.label = JoinOrigin(it->second.label, label);
            it->second.spellings.insert(name_expr.text);
        }
    }

    VType UnloadColumn(const VType& table, const Expr& name_expr, const Expr& call) {
        VType v = Typed(Ty::Array, Origin::Client);
        v.objs.insert(&call);
        ObjectLabels& ol = state_.objects[&call];
        ol.kind = Ty::Array;
        const ColumnInfo* column = nullptr;
        if (table.columns && IsStringLit(name_expr)) {
            const auto it = table.columns->columns.find(AllowlistKey(name_expr.text));
            if (it != table.columns->columns.end()) column = &it->second;
        }
        if (column) {
            NoteSlot(ol.elem, ColumnType(*column, table));
            JoinInto(ol.content, column->origin);
            ol.refs.insert(column->refs.begin(), column->refs.end());
            ol.ref_unknown = ol.ref_unknown || column->refs.empty();
        } else {
            Add("C_UNKNOWN_MEMBER", name_expr.pos);   // a column the query check does not report
            JoinInto(ol.elem.scalar, Origin::Unknown);
            JoinInto(ol.content, Origin::Unknown);
            ol.ref_unknown = true;
        }
        const std::optional<Origin> shape = ShapeOf(table);
        if (shape && IsTainted(*shape)) JoinInto(ol.shape, *shape);
        return v;
    }

    // Т.Сортировать("<list>"): only the order of rows the client receives anyway changes, so the column labels stay.
    // Anything not proven so is refused like an unknown method: a list that is not a literal, a column that is not
    // open, a reference unless every target's presentation is open, a tainted table or pc. The platform reference of
    // ТаблицаЗначений.Сортировать: without <ОбъектСравнения> "элементы остальных типов сравниваются по строковому
    // представлению", i.e. references are ordered by their presentation. A column that may hold a reference whose
    // targets were lost (ColumnInfo::refs_known false) is refused for the same reason.
    VType SortTable(const VType& table, const Expr& list, const Expr& call) {
        if (!SortIsOverOpenColumns(table, list)) {
            Add("C_UNKNOWN_METHOD", call.pos);
            AddUnknownEffect(call.pos);
        }
        return Typed(Ty::Undefined, Origin::Client);
    }

    bool SortIsOverOpenColumns(const VType& table, const Expr& list) const {
        if (!table.columns || !IsStringLit(list) || IsTainted(pc_) || IsTainted(table.label)) return false;
        const std::optional<Origin> shape = ShapeOf(table);
        if (shape && IsTainted(*shape)) return false;
        const char* const blanks = " \t\r\n";
        const std::string& text = list.text;
        size_t start = 0;
        while (true) {
            const size_t comma = text.find(',', start);
            const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            std::vector<std::string> words;
            for (size_t p = item.find_first_not_of(blanks); p != std::string::npos; p = item.find_first_not_of(blanks, p)) {
                const size_t end = item.find_first_of(blanks, p);
                words.push_back(item.substr(p, end == std::string::npos ? std::string::npos : end - p));
                p = end;
            }
            if (words.empty() || words.size() > 2 || !IsPathKey(words[0])) return false;
            if (words.size() == 2) {
                const std::string dir = AllowlistKey(words[1]);
                if (dir != AllowlistKey("Возр") && dir != AllowlistKey("Убыв") && dir != "ASC" && dir != "DESC") {
                    return false;
                }
            }
            const auto it = table.columns->columns.find(AllowlistKey(words[0]));
            if (it == table.columns->columns.end() || it->second.name_collision || IsTainted(it->second.origin) ||
                !it->second.refs_known) {   // a value that may be a reference with unknown targets
                return false;
            }
            for (const std::string& t : it->second.refs) {
                if (!options_.policy || !options_.policy->HasTable(t) ||
                    options_.policy->PresentationClass(t) != strict::FieldClass::Open) {
                    return false;
                }
            }
            if (comma == std::string::npos) return true;
            start = comma + 1;
        }
    }

    VType CollectionMethod(const std::string& m, const VType& recv, const std::vector<VType>& args, const Expr& e) {
        if (m == AllowlistKey("Добавить")) {
            Store(recv, args[0], e.pos);
            return Typed(Ty::Undefined, Origin::Client);
        }
        if (m == AllowlistKey("Вставить")) {
            if (recv.ty != Ty::Map) NameLiteral(*e.args[1]);
            StoreKey(recv, args[0], e.args[1]->pos);
            if (IsTainted(pc_)) {
                for (const Expr* o : recv.objs) JoinInto(state_.objects[o].shape, pc_);
            }
            Store(recv, args.size() > 1 ? args[1] : Typed(Ty::Undefined, Origin::Client), e.pos,
                  IsStringLit(*e.args[1]) ? &e.args[1]->text : nullptr);
            return Typed(Ty::Undefined, Origin::Client);
        }
        if (m == AllowlistKey("Количество")) {
            std::optional<Origin> dep = ShapeOf(recv);
            if (recv.ty == Ty::Map) {
                const std::optional<Origin> keys = KeysOf(recv);
                if (keys) CheckOp(e.pos, *keys);   // the number of distinct keys
                JoinInto(dep, keys);
            }
            return Typed(Ty::Number, Fresh(dep));
        }
        if (m == AllowlistKey("Получить")) {
            std::optional<Origin> key = ValueLabel(args[0]);
            if (recv.ty == Ty::Map) JoinInto(key, KeysOf(recv));
            CheckOp(e.pos, *key);
            VType v = ElementOf(recv, *key);
            if (args.size() > 1) v = JoinType(v, args[1]);
            v.ty = Ty::Unknown;
            return v;
        }
        if (m == AllowlistKey("Свойство")) {
            return Typed(Ty::Boolean, Fresh(ShapeOf(recv)));
        }
        Add("C_INTERNAL", e.pos);
        return VType();
    }

    bool Unassigned(const Expr& var) {
        const std::string key = VarKey(var.text, var.pos);
        return env_.find(key) == env_.end();
    }

    void AssignOutParam(const Expr& target, const VType& value) {
        if (target.kind != Expr::K::Var) {
            Add("C_BAD_ARGS", target.pos);
            return;
        }
        AssignVar(target, value);
    }

    // The condition of ?(), Если, Пока or the left side of И / ИЛИ: tainted -> implicit flow.
    Origin Condition(const VType& c, const Pos& pos) {
        const Origin label = ValueLabel(c);
        if (IsTainted(label)) Add("C_IMPLICIT_FLOW", pos);
        return label;
    }

    VType EvalTernary(const Expr& e) {
        const Origin cond = Condition(Eval(*e.args[0]), e.pos);
        const Origin saved = pc_;
        pc_ = PcJoin(pc_, cond);
        Charge(env_.size());
        const Env before = env_;
        const VType a = Eval(*e.args[1]);
        const Env after_a = env_;
        env_ = before;
        const VType b = Eval(*e.args[2]);
        env_ = JoinEnv(after_a, env_);
        pc_ = saved;
        VType r = JoinType(a, b);
        if (IsTainted(cond)) r.label = JoinOrigin(r.label, Transform(cond));
        return r;
    }

    VType EvalUnary(const Expr& e) {
        const VType a = Eval(*e.args[0]);
        const Origin input = ValueLabel(a);
        CheckOp(e.pos, input);
        return Typed(e.text == "-" ? Ty::Number : Ty::Boolean, Transform(input));
    }

    VType EvalBinary(const Expr& e) {
        const VType a = Eval(*e.args[0]);
        const std::string& op = e.text;
        const bool logic = op == "И" || op == "ИЛИ";
        VType b;
        if (logic) {
            // The right side runs only for some values of the left side.
            const Origin left = Condition(a, e.pos);
            const Origin saved = pc_;
            pc_ = PcJoin(pc_, left);
            Charge(env_.size());
            const Env before = env_;
            b = Eval(*e.args[1]);
            env_ = JoinEnv(before, env_);
            pc_ = saved;
        } else {
            b = Eval(*e.args[1]);
        }
        std::optional<Origin> input = ValueLabel(a);
        JoinInto(input, ValueLabel(b));
        if (op == "+" && a.ty != Ty::Number) {
            // String concatenation converts a reference to its presentation.
            JoinInto(input, Presentation(a));
            JoinInto(input, Presentation(b));
        }
        CheckOp(e.pos, *input);
        const Origin label = Transform(*input);
        if (logic || op == "=" || op == "<>" || op == "<" || op == ">" || op == "<=" || op == ">=") {
            return Typed(Ty::Boolean, label);
        }
        // The left operand decides the operation: a number stays a number (or the call fails), a string concatenates.
        if (a.ty == Ty::Number) return Typed(Ty::Number, label);
        if (op == "+" && a.ty == Ty::String) return Typed(Ty::String, label);
        VType r;
        r.label = label;
        return r;
    }

    // ---- statements ----

    void RecordAssign(const std::string& key, const VType& v) {
        for (Frame* f : frames_) {
            const auto it = f->acc.find(key);
            if (it != f->acc.end()) {
                it->second = JoinType(it->second, v);
            } else {
                f->acc[key] = JoinType(MaybeUnassigned(), v);   // not collected: assume it may be unassigned
            }
        }
    }

    void AssignVar(const Expr& target, const VType& value) {
        BindVar(target.text, target.pos, value);
    }

    void BindVar(const std::string& name, const Pos& pos, const VType& value) {
        const std::string key = VarKey(name, pos);
        if (T().context_names.count(key)) {
            Add("C_UNKNOWN_IDENT", pos);
            return;
        }
        VType v = value;
        v.maybe_unassigned = false;
        v.label = WithPc(v.label);
        RecordAssign(key, v);
        env_[key] = std::move(v);
    }

    void Assign(const Stmt& s) {
        const Expr& target = *s.exprs[0];
        const Expr& value_expr = *s.exprs[1];
        const bool text = target.kind == Expr::K::Member && T().query_text.count(AllowlistKey(target.text));
        if (text) ++in_query_text_;
        const VType value = Eval(value_expr, target.kind == Expr::K::Var && value_expr.kind == Expr::K::New);
        if (text) --in_query_text_;
        if (target.kind == Expr::K::Var) {
            AssignVar(target, value);
        } else if (target.kind == Expr::K::Member) {
            const VType obj = Eval(*target.args[0], true);
            if (obj.ty == Ty::Unknown) {
                Add("C_UNKNOWN_RECEIVER", target.pos);
            } else if (obj.ty == Ty::Query && text) {
                NoteQueryText(obj.site, value_expr);
            } else {
                Add("C_UNKNOWN_MEMBER", target.pos);
                LoseQuery(obj, false);
            }
        } else {
            const VType obj = Eval(*target.args[0], true);
            const VType index = Eval(*target.args[1]);
            if (obj.ty == Ty::Array || obj.ty == Ty::Structure || obj.ty == Ty::Map) {
                if (IndexCheck(obj, index, *target.args[1], target.pos)) {
                    if (obj.ty != Ty::Array && IsTainted(pc_)) {
                        for (const Expr* o : obj.objs) JoinInto(state_.objects[o].shape, pc_);
                    }
                    if (obj.ty == Ty::Map) StoreKey(obj, index, target.args[1]->pos);
                    Store(obj, value, target.pos, IsStringLit(*target.args[1]) ? &target.args[1]->text : nullptr);
                }
            } else if (obj.ty == Ty::Unknown) {
                Add("C_UNKNOWN_RECEIVER", target.pos);
            } else {
                Add("C_UNKNOWN_INDEX", target.pos);
                LoseQuery(obj, false);
            }
        }
    }

    void If(const Stmt& s) {
        std::vector<Env> outs;
        const Origin saved = pc_;
        Origin cond_pc = pc_;   // a later condition is evaluated only when the earlier ones are false
        for (size_t i = 0; i < s.exprs.size(); ++i) {
            pc_ = cond_pc;
            cond_pc = PcJoin(cond_pc, Condition(Eval(*s.exprs[i]), s.exprs[i]->pos));
            Charge(env_.size());
            const Env base = env_;   // the state when this condition is false
            pc_ = cond_pc;
            Block(s.blocks[i]);
            outs.push_back(env_);
            env_ = base;
        }
        pc_ = cond_pc;
        if (s.has_else) Block(s.blocks[s.exprs.size()]);
        pc_ = saved;
        outs.push_back(env_);
        env_ = outs[0];
        for (size_t i = 1; i < outs.size(); ++i) {
            Charge(env_.size() + outs[i].size());
            env_ = JoinEnv(env_, outs[i]);
        }
    }

    Frame OpenFrame(const Body& body, const Expr* condition, const std::string* loop_var, bool is_try) const {
        std::set<std::string> assigned;
        CollectBody(body, assigned);
        if (condition) CollectExpr(*condition, assigned);
        if (loop_var) assigned.insert(AllowlistKey(*loop_var));
        Frame f;
        f.is_try = is_try;
        for (const std::string& n : assigned) {
            const auto it = env_.find(n);
            f.acc[n] = it == env_.end() ? MaybeUnassigned() : it->second;
        }
        return f;
    }

    // Any loop: the body is walked until the values at the loop head (the state before the loop joined with every
    // value assigned in the loop) stop changing. `extra` is what the number of iterations depends on besides the
    // condition (bounds, a collection's shape).
    void Loop(const Stmt& s, const Expr* condition, const std::string* loop_var, const Pos& var_pos,
              const VType& element, Origin extra) {
        const Env entry = env_;
        Frame f = OpenFrame(s.blocks[0], condition, loop_var, false);
        const Origin saved = pc_;
        Env head;
        for (int round = 0;; ++round) {
            Charge(entry.size());
            head = entry;
            for (const auto& kv : f.acc) head[kv.first] = kv.second;
            env_ = head;
            if (exhausted_) break;
            const std::map<std::string, VType> before = f.acc;
            const std::optional<Origin> jump_before = f.jump;
            frames_.push_back(&f);
            pc_ = PcJoin(saved, extra);
            if (f.jump) pc_ = IsTainted(pc_) ? JoinOrigin(pc_, *f.jump) : *f.jump;   // an earlier conditional exit
            if (condition) pc_ = PcJoin(pc_, Condition(Eval(*condition), condition->pos));
            if (loop_var) BindVar(*loop_var, var_pos, element);
            Block(s.blocks[0]);
            frames_.pop_back();
            if ((SameAcc(before, f.acc) && f.jump == jump_before) || exhausted_) break;
            if (round + 1 >= kMaxLoopRounds) {
                Add("C_LIMIT", s.pos);
                break;
            }
            if (round + 1 >= kMaxLoopIterations) Widen(f.acc, before);
        }
        pc_ = saved;
        env_ = head;
        if (f.jump) {
            // How many iterations ran depends on the tainted exit condition.
            for (const auto& kv : f.acc) env_[kv.first].label = JoinOrigin(env_[kv.first].label, *f.jump);
        }
    }

    // Values still changing after kMaxLoopIterations rounds lose their type and label.
    static void Widen(std::map<std::string, VType>& acc, const std::map<std::string, VType>& before) {
        for (auto& kv : acc) {
            const auto it = before.find(kv.first);
            if (it != before.end() && SameVType(it->second, kv.second)) continue;
            VType w;
            w.maybe_unassigned = kv.second.maybe_unassigned;
            w.objs = kv.second.objs;
            w.refs = kv.second.refs;
            kv.second = std::move(w);
        }
    }

    void ForEach(const Stmt& s) {
        const VType coll = Eval(*s.exprs[0]);
        VType element;
        if (coll.ty == Ty::ValueTable) {
            element = Typed(Ty::TableRow, IsTainted(coll.label) ? coll.label : Origin::Open);
            element.columns = coll.columns;
            element.objs = coll.objs;
        } else if (coll.ty == Ty::Array) {
            element = ElementOf(coll, Origin::Open);
        } else if (coll.ty == Ty::Unknown) {
            Add("C_UNKNOWN_RECEIVER", s.exprs[0]->pos);
        } else {
            Add("C_UNKNOWN_ITER", s.exprs[0]->pos);
            LoseQuery(coll, false);
        }
        const std::optional<Origin> shape = ShapeOf(coll);
        const Origin count = shape ? *shape : Origin::Open;
        if (IsTainted(count)) Add("C_IMPLICIT_FLOW", s.exprs[0]->pos);
        Loop(s, nullptr, &s.var, s.pos, element, count);
    }

    void ForTo(const Stmt& s) {
        std::optional<Origin> bounds = ValueLabel(Eval(*s.exprs[0]));
        JoinInto(bounds, ValueLabel(Eval(*s.exprs[1])));
        if (IsTainted(*bounds)) Add("C_IMPLICIT_FLOW", s.pos);
        Loop(s, nullptr, &s.var, s.pos, Typed(Ty::Number, Transform(*bounds)), *bounds);
    }

    // The Исключение block may start after any statement of the body, and runs only when an operation failed: it
    // depends on every tainted operation of the body.
    void Try(const Stmt& s) {
        Frame f = OpenFrame(s.blocks[0], nullptr, nullptr, true);
        Charge(env_.size());
        const Env entry = env_;
        frames_.push_back(&f);
        Block(s.blocks[0]);
        frames_.pop_back();
        const Env after_body = env_;
        env_ = entry;
        for (const auto& kv : f.acc) env_[kv.first] = kv.second;
        const Origin saved = pc_;
        if (f.taint) {
            Add("C_IMPLICIT_FLOW", s.pos);
            for (const auto& kv : f.acc) env_[kv.first].label = JoinOrigin(env_[kv.first].label, *f.taint);
            pc_ = PcJoin(pc_, *f.taint);
        }
        Block(s.blocks[1]);
        pc_ = saved;
        env_ = JoinEnv(after_body, env_);
    }

    // Прервать / Продолжить under a tainted pc: the rest of the loop, and the values it leaves, depend on it.
    void Jump() {
        if (!IsTainted(pc_)) return;
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            if ((*it)->is_try) continue;
            JoinInto((*it)->jump, pc_);
            return;
        }
    }

    void Statement(const Stmt& s) {
        Charge(1);
        if (exhausted_) return;
        switch (s.kind) {
            case Stmt::K::Assign: Assign(s); break;
            case Stmt::K::CallStmt: Eval(*s.exprs[0]); break;
            case Stmt::K::If: If(s); break;
            case Stmt::K::ForEach: ForEach(s); break;
            case Stmt::K::ForTo: ForTo(s); break;
            case Stmt::K::While: Loop(s, s.exprs[0].get(), nullptr, s.pos, VType(), Origin::Open); break;
            case Stmt::K::Try: Try(s); break;
            case Stmt::K::Break:
            case Stmt::K::Continue: Jump(); break;   // the loop head joins every value assigned in the loop
            default: Add("C_PARSE", s.pos); break;
        }
    }
};

}  // namespace

CodeAnalysis AnalyzeCodeWalk(const Body& body, const TypeCheckOptions& options, LabelState& state) {
    Checker checker(options, state);
    return checker.Run(body);
}

}  // namespace lineage::bsl
