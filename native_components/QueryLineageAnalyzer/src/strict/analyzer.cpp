#include "strict/analyzer.h"

#include <map>
#include <utility>

#include "strict/ast_dump.h"
#include "strict/rules.h"
#include "utf_utils.h"

namespace lineage::strict {

namespace {

constexpr int kMaxSelectDepth = 128;   // nesting of subqueries; the parser already caps it, this is a backstop
constexpr const char* kConstantsTable = "Константы";   // the only source name without a dot that is not a temp table

struct TempTable {
    std::vector<Column> columns;
    bool complete = true;
    bool rows_protected = false;
};

struct SelectOut {
    std::vector<Column> columns;
    std::vector<Pos> positions;
    bool complete = true;
    bool rows_protected = false;   // the row set (any ОБЪЕДИНИТЬ part included) depends on a protected condition
    bool grouped = false;          // grouped by a protected key (even one returned as is) or РАЗЛИЧНЫЕ over protected
};

// A grouped / distinct select that is consumed (temp table, derived table, union part, subquery) hands its
// multiplicities on: every column of it becomes derived protected information for the consumer. Only the
// top-level result of a batch keeps the exemption for a group key that is returned as is.
void Consume(SelectOut& r) {
    if (!r.grouped) return;
    for (Column& c : r.columns) {
        if (c.origin != Origin::Unknown) c.origin = JoinOrigin(c.origin, Origin::ProtectedDerived);
        if (!c.refs.empty()) c.refs_known = false;   // the targets are dropped, not the reference
        c.refs.clear();
    }
}

bool IsTaintedOrigin(Origin o) {
    return o == Origin::ProtectedDirect || o == Origin::ProtectedDerived || o == Origin::Mixed;
}

// Result column names a single-part ORDER BY / TOTALS path may refer to, with the labels of those columns.
struct AliasInfo {
    const std::vector<std::string>* names = nullptr;
    const std::vector<Column>* columns = nullptr;
};

class Analyzer {
public:
    Analyzer(const FieldPolicy& policy, AnalysisResult& out, LabelTable& labels)
        : policy_(policy), out_(out), labels_(labels) {
        sink_.missing_tables = &out.missing_tables;
        sink_.issues = &out.resolution_issues;
    }

    void Run(const Batch& batch) {
        for (size_t i = 0; i < batch.statements.size(); ++i) {
            const bool last = i + 1 == batch.statements.size();
            RunStatement(batch.statements[i], last);
        }
    }

private:
    const FieldPolicy& policy_;
    AnalysisResult& out_;
    LabelTable& labels_;
    ResolveSink sink_;
    std::map<std::string, TempTable> temps_;   // normalized temp-table name -> columns

    // Exact (normalized) match only. A name that matches another spelling after e/yo folding, or a table whose
    // spelling collides with another one, is not found: the tokenizer treats such near-collisions as ambiguous.
    const TempTable* FindTemp(const std::string& name) const {
        const std::string key = ScopeKey(name);
        const std::string folded = FoldedKey(name);
        const TempTable* found = nullptr;
        for (const auto& entry : temps_) {
            if (entry.first == key) found = &entry.second;
            else if (FoldedKey(entry.first) == folded) return nullptr;
        }
        return found;
    }

    void RunStatement(const Statement& st, bool last) {
        if (st.kind == Statement::K::Drop) {
            if (FindTemp(st.drop_name) == nullptr) sink_.Issue(kResolveUnknownTemp, st.pos);
            else temps_.erase(ScopeKey(st.drop_name));
            return;
        }
        if (!st.select) {
            sink_.Issue(kResolveBadStatement, st.pos);
            return;
        }
        SelectOut result = AnalyzeSelect(*st.select, nullptr, 0, true);
        if (!st.select->into.empty()) StoreTemp(*st.select, result);
        if (last) PublishColumns(result);
    }

    void StoreTemp(const Select& head, const SelectOut& result) {
        if (head.into.find('.') != std::string::npos) {
            sink_.Issue(kResolveBadStatement, head.pos);
            return;
        }
        TempTable table;
        SelectOut consumed = result;
        Consume(consumed);
        table.columns = consumed.columns;
        table.complete = result.complete;
        table.rows_protected = result.rows_protected;
        temps_[ScopeKey(head.into)] = std::move(table);   // re-creating a name replaces the old table
    }

    void PublishColumns(const SelectOut& result) {
        for (size_t i = 0; i < result.columns.size(); ++i) {
            ColumnLabel label;
            label.name = result.columns[i].name;
            label.origin = result.columns[i].origin;
            label.refs = result.columns[i].refs;
            label.refs_known = result.columns[i].refs_known;
            if (i < result.positions.size()) label.pos = result.positions[i];
            out_.columns.push_back(std::move(label));
        }
    }

    // ---- sources ----

    Binding BuildBinding(const Source& src, int depth) {
        Binding b;
        switch (src.kind) {
            case Source::K::Table: BuildTable(src, b); break;
            case Source::K::VirtualTable: BuildVirtualTable(src, b); break;
            case Source::K::Subquery: BuildSubquery(src, depth, b); break;
        }
        SetBindingNames(b, src.kind == Source::K::Subquery ? std::string() : src.name, src.alias);
        if (src.kind == Source::K::VirtualTable) LabelVirtualTableParams(src, b, depth);
        return b;
    }

    void BuildTable(const Source& src, Binding& b) {
        if (src.name.find('.') != std::string::npos) {
            b.kind = Binding::Kind::Table;
            b.table = src.name;
            // A virtual table written without parentheses (РегистрНакопления.Р.Остатки) is the same table with every
            // parameter omitted: the binding equals the one BuildVirtualTable makes for an empty parameter list.
            b.known = policy_.HasTable(src.name) || !policy_.ResolveVirtualBase(src.name).empty();
            if (!b.known) {
                sink_.MissingTable(src.name);
                sink_.Issue(kResolveUnknownSource, src.pos);
            }
            return;
        }
        // A name without a dot is a temporary table, or else the constants table (Константы / Constants), whose
        // policy key is always "Константы". A temporary table created earlier in the batch under that name wins.
        const TempTable* temp = FindTemp(src.name);
        const std::string key = ScopeKey(src.name);
        if (temp == nullptr && (key == ScopeKey(kConstantsTable) || key == ScopeKey("Constants"))) {
            b.kind = Binding::Kind::Table;
            b.table = kConstantsTable;
            b.known = policy_.HasTable(kConstantsTable);
            if (!b.known) {
                sink_.MissingTable(kConstantsTable);
                sink_.Issue(kResolveUnknownSource, src.pos);
            }
            return;
        }
        b.kind = Binding::Kind::Temp;
        if (temp == nullptr) {
            b.known = false;
            sink_.Issue(kResolveUnknownTemp, src.pos);
            return;
        }
        b.columns = temp->columns;
        b.complete = temp->complete;
        b.rows_protected = temp->rows_protected;
    }

    void BuildVirtualTable(const Source& src, Binding& b) {
        // The parameters are labelled in LabelVirtualTableParams, once the binding has its names.
        b.kind = Binding::Kind::Table;
        b.table = src.name;
        b.known = src.name.find('.') != std::string::npos && !policy_.ResolveVirtualBase(src.name).empty();
        if (!b.known) {
            sink_.MissingTable(src.name);
            sink_.Issue(kResolveUnknownSource, src.pos);
        }
    }

    void BuildSubquery(const Source& src, int depth, Binding& b) {
        b.kind = Binding::Kind::Subquery;
        if (!src.subquery) {
            b.known = false;
            sink_.Issue(kResolveBadStatement, src.pos);
            return;
        }
        SelectOut r = AnalyzeSelect(*src.subquery, nullptr, depth + 1, false);   // a derived table sees no outer scope
        Consume(r);
        b.columns = std::move(r.columns);
        b.complete = r.complete;
        b.rows_protected = r.rows_protected;
    }

    // Parameters of a virtual table (period slots and a condition slot) are expressions over the fields of the
    // register's base table; they are labelled in a scope made of that one binding and are checked by the rule
    // engine like any condition. The slot position is not known here, so every slot gets the same treatment.
    // A bare period name (a periodicity slot, e.g. МЕСЯЦ) is a constant unless the register has such a field.
    // A parameter that reads other tables of the query is not resolvable here: Unknown, hence Confirm.
    void LabelVirtualTableParams(const Source& src, Binding& b, int depth) {
        Scope vt_scope;
        vt_scope.bindings.push_back(b);
        for (const ExprP& p : src.vt_params) {
            if (!p) continue;
            const bool period_slot = p->kind == Expr::K::Field && p->parts.size() == 1 && IsPeriodName(p->parts[0]) &&
                                     policy_.Field(src.name, p->parts[0]) == nullptr;
            if (period_slot) labels_.expr[p.get()] = Plain(Origin::Open, true);
            else Label(*p, vt_scope, depth, nullptr);
            if (SubtreeTainted(p)) b.rows_protected = true;
        }
    }

    bool SubtreeTainted(const ExprP& e) const {
        if (!e) return false;
        const auto it = labels_.expr.find(e.get());
        if (it != labels_.expr.end() && IsTaintedOrigin(it->second.origin)) return true;
        for (const ExprP& a : e->args) {
            if (SubtreeTainted(a)) return true;
        }
        if (e->subquery) {
            const auto sub = labels_.result_protected.find(e->subquery.get());
            if (sub == labels_.result_protected.end() || sub->second) return true;
        }
        return false;
    }

    // ---- expressions ----

    // `aliases` (ORDER BY / TOTALS only): result column names that a single-part path may refer to.
    // Label rules (fail closed; every operand is resolved even when the label ignores it, so unknown names and
    // missing tables inside it are reported):
    //   Field -> its policy class; Param -> the policy origin of the parameter; literals -> Client;
    //   ЗНАЧЕНИЕ / ТИП / ТИПЗНАЧЕНИЯ / КОЛИЧЕСТВО -> Open; ПРЕДСТАВЛЕНИЕ(ref) -> presentation class of the targets;
    //   allowlisted scalar and aggregate functions, operators, predicates, ВЫРАЗИТЬ, ВЫБОР (all branches AND all
    //   conditions: implicit flow) and subquery predicates -> Transform(Join(all operands));
    //   an unlisted function or an unset node -> Unknown.
    FieldLabel Label(const Expr& e, const Scope& scope, int depth, const AliasInfo* aliases) {
        FieldLabel l = LabelNode(e, scope, depth, aliases);
        labels_.expr[&e] = l;   // the rule engine reads the label of every node from here
        return l;
    }

    FieldLabel LabelNode(const Expr& e, const Scope& scope, int depth, const AliasInfo* aliases) {
        switch (e.kind) {
            case Expr::K::Field: return LabelField(e, scope, aliases);
            case Expr::K::Param: return LabelParam(e);
            case Expr::K::StringLit:
            case Expr::K::NumberLit:
            case Expr::K::BoolLit:
            case Expr::K::NullLit:
            case Expr::K::UndefinedLit: return Plain(Origin::Client, true);
            case Expr::K::ValueFn: return Plain(Origin::Open, false);   // ЗНАЧЕНИЕ(Справочник.А.Пустая) is a reference
            case Expr::K::TypeFn: return Plain(Origin::Open, true);
            case Expr::K::Call: return LabelCall(e, scope, depth, aliases);
            case Expr::K::Unset: return FieldLabel();
            default: break;
        }
        if (e.kind == Expr::K::Cast) return LabelCast(e, scope, depth, aliases);
        // Unary, Binary, Case, InList, InSubquery, InHierarchy, Between, Like, IsNullTest, RefsTest, Row. Only ВЫБОР
        // returns one of its operands; the others yield a number, a string or a Boolean.
        std::vector<FieldLabel> operands = LabelOperands(e, scope, depth, aliases);
        return Plain(DerivedFrom(operands), e.kind != Expr::K::Case || PassesNoReference(operands));
    }

    // A value chosen among `operands` (ВЫБОР, ЕСТЬNULL, МИНИМУМ / МАКСИМУМ) is provably not a reference only when
    // none of them may be one; its targets are not tracked otherwise.
    static bool PassesNoReference(const std::vector<FieldLabel>& operands) {
        for (const FieldLabel& o : operands) {
            if (!o.refs_known || !o.refs.empty()) return false;
        }
        return true;
    }

    // Casting a reference to a non-reference type may expose its presentation (unverified platform behaviour):
    // Unknown when the operand holds or may hold a reference (a parameter of unknown type, a value whose targets were
    // lost). A reference target type is a dotted name (Справочник.А); everything else (Строка, Число, Дата, Булево) is
    // not, and the result of a cast to it is not a reference; a cast to a reference type has unknown targets.
    FieldLabel LabelCast(const Expr& e, const Scope& scope, int depth, const AliasInfo* aliases) {
        const std::vector<FieldLabel> operands = LabelOperands(e, scope, depth, aliases);
        const bool to_reference = e.type_name.find('.') != std::string::npos;
        if (!to_reference && !e.args.empty() && e.args[0] &&
            (!operands[0].refs.empty() || !operands[0].refs_known || e.args[0]->kind == Expr::K::Param)) {
            return FieldLabel();
        }
        return Plain(DerivedFrom(operands), !to_reference);
    }

    // `refs_known`: true when the value is provably not a reference (FieldLabel::refs_known).
    static FieldLabel Plain(Origin origin, bool refs_known) {
        FieldLabel l;
        l.origin = origin;
        l.refs_known = refs_known;
        return l;
    }

    // Transform(Join(all operands)); no operands (e.g. a constant call) is Open.
    static Origin DerivedFrom(const std::vector<FieldLabel>& operands) {
        if (operands.empty()) return Origin::Open;
        Origin o = operands[0].origin;
        for (size_t i = 1; i < operands.size(); ++i) o = JoinOrigin(o, operands[i].origin);
        return Transform(o);
    }

    // Labels of the argument expressions, plus one entry for the columns of a subquery operand.
    std::vector<FieldLabel> LabelOperands(const Expr& e, const Scope& scope, int depth,
                                          const AliasInfo* aliases, int period_index = -1) {
        std::vector<FieldLabel> out;
        for (size_t i = 0; i < e.args.size(); ++i) {
            const ExprP& a = e.args[i];
            if (a && static_cast<int>(i) == period_index && a->kind == Expr::K::Field && a->parts.size() == 1 &&
                IsPeriodName(a->parts[0])) {
                out.push_back(Plain(Origin::Open, true));   // a period constant, not a field lookup
            } else if (a) out.push_back(Label(*a, scope, depth, aliases));
            else out.push_back(FieldLabel());   // a null operand is malformed: Unknown
        }
        if (e.subquery) {
            SelectOut sub = AnalyzeSelect(*e.subquery, &scope, depth + 1, false);
            Consume(sub);
            out.push_back(Plain(SubqueryOrigin(sub), false));
        }
        return out;
    }

    // Join of every column the subquery returns; Unknown when the column list cannot be enumerated. A subquery
    // whose rows were chosen by a protected condition returns derived protected information even through open
    // columns (the presence of a row is the answer), so it is joined with ProtectedDerived.
    static Origin SubqueryOrigin(const SelectOut& sub) {
        if (!sub.complete || sub.columns.empty()) return Origin::Unknown;
        Origin o = sub.columns[0].origin;
        for (size_t i = 1; i < sub.columns.size(); ++i) o = JoinOrigin(o, sub.columns[i].origin);
        if (sub.rows_protected && o != Origin::Unknown) o = JoinOrigin(o, Origin::ProtectedDerived);
        return o;
    }

    FieldLabel LabelField(const Expr& e, const Scope& scope, const AliasInfo* aliases) {
        if (aliases != nullptr && e.parts.size() == 1) {
            const std::string key = FoldedKey(e.parts[0]);
            for (size_t i = 0; i < aliases->names->size(); ++i) {
                const std::string& name = (*aliases->names)[i];
                if (name.empty() || FoldedKey(name) != key) continue;
                FieldLabel l;   // the label of the result column the alias names
                if (i < aliases->columns->size()) {
                    l.origin = (*aliases->columns)[i].origin;
                    l.refs = (*aliases->columns)[i].refs;
                    l.refs_known = (*aliases->columns)[i].refs_known;
                }
                return l;
            }
        }
        // A path of a subquery that its own sources cannot resolve reads the enclosing query (outer reference): an
        // aggregate over it is computed over the outer row set (rules: Q_AGGREGATE_ROWSET).
        if (scope.parent != nullptr) {
            Scope own;
            own.bindings = scope.bindings;
            if (ResolveFieldPath(e.parts, e.pos, own, policy_, ResolveSink()).origin == Origin::Unknown) {
                labels_.outer_refs.insert(&e);
            }
        }
        return ResolveFieldPath(e.parts, e.pos, scope, policy_, sink_);
    }

    FieldLabel LabelParam(const Expr& e) {
        switch (policy_.OriginOfParam(e.text)) {
            // A parameter may be a reference whose targets are not known.
            case ParamOrigin::Client: return Plain(Origin::Client, false);
            case ParamOrigin::TokenProtected: return Plain(Origin::ProtectedDirect, false);   // equality operand
            case ParamOrigin::TokenUnknown: break;
            case ParamOrigin::Missing: sink_.Issue(kResolveUnknownParam, e.pos); break;
        }
        return FieldLabel();
    }

    FieldLabel LabelCall(const Expr& e, const Scope& scope, int depth, const AliasInfo* aliases) {
        const std::vector<FieldLabel> operands = LabelOperands(e, scope, depth, aliases, PeriodArgIndex(e.text));
        switch (ClassifyCall(e.text)) {
            case CallClass::Count: return Plain(Origin::Open, true);   // a number, not stored data
            case CallClass::TypeOf: {
                // a type name is public, but the choice of the type may depend on a protected value
                const bool open = operands.size() == 1 &&
                                  (operands[0].origin == Origin::Open || operands[0].origin == Origin::Client);
                return Plain(open ? Origin::Open : DerivedFrom(operands), true);
            }
            case CallClass::Sum: return Plain(DerivedFrom(operands), true);   // a number
            case CallClass::MinMax: return Plain(DerivedFrom(operands), PassesNoReference(operands));
            case CallClass::Plain: {
                // ЕСТЬNULL returns one of its arguments; the other functions return a number, a string or a date.
                const std::string key = NormalizeIdentifierKey(e.text);
                const bool chooses = key == "ЕСТЬNULL" || key == "ISNULL";
                return Plain(DerivedFrom(operands), !chooses || PassesNoReference(operands));
            }
            case CallClass::Presentation: return LabelPresentation(e, operands);
            case CallClass::Unrecognized: break;
        }
        return FieldLabel();
    }

    static Origin FromClass(FieldClass cls) {
        switch (cls) {
            case FieldClass::Open: return Origin::Open;
            case FieldClass::Protected: return Origin::ProtectedDirect;
            case FieldClass::Unknown: break;
        }
        return Origin::Unknown;
    }

    // The presentation of a reference is computed by the platform from stored data of the target: it is a direct
    // read of the target's presentation (main token space), joined over every possible target.
    Origin PresentationOfTargets(const std::vector<std::string>& targets) {
        Origin o = Origin::Open;
        bool first = true;
        for (const std::string& t : targets) {
            Origin one = Origin::Unknown;
            if (policy_.HasTable(t)) one = FromClass(policy_.PresentationClass(t));
            else sink_.MissingTable(t);
            o = first ? one : JoinOrigin(o, one);
            first = false;
        }
        return o;
    }

    FieldLabel LabelPresentation(const Expr& e, const std::vector<FieldLabel>& operands) {
        if (e.args.size() != 1 || !e.args[0] || operands.size() != 1) return FieldLabel();
        const FieldLabel& x = operands[0];
        const Expr::K kind = e.args[0]->kind;
        if (x.origin == Origin::Unknown) return FieldLabel();
        if (!x.refs.empty()) {
            Origin o = PresentationOfTargets(x.refs);
            if (x.origin != Origin::Open && x.origin != Origin::Client) o = JoinOrigin(o, Transform(x.origin));
            return Plain(o, true);   // a string
        }
        if (kind == Expr::K::StringLit || kind == Expr::K::NumberLit || kind == Expr::K::BoolLit) return Plain(x.origin, true);
        // No known target: a public-looking operand may still hold a reference (a parameter, a column whose
        // targets were lost), whose presentation is stored data. Only operands that are protected anyway are
        // a plain function over protected data.
        if (x.origin == Origin::Open || x.origin == Origin::Client) return FieldLabel();
        return Plain(Transform(x.origin), true);
    }

    void Walk(const ExprP& e, const Scope& scope, int depth, const AliasInfo* aliases = nullptr) {
        if (e) Label(*e, scope, depth, aliases);
    }

    void WalkList(const std::vector<ExprP>& list, const Scope& scope, int depth,
                  const AliasInfo* aliases = nullptr) {
        for (const ExprP& e : list) Walk(e, scope, depth, aliases);
    }

    // ---- queries ----

    Scope BuildScope(const Select& s, const Scope* parent, int depth) {
        Scope scope;
        scope.parent = parent;
        for (size_t i = 0; i < s.from.size(); ++i) {
            scope.bindings.push_back(BuildBinding(s.from[i], depth));
            if (i >= s.joins.size()) continue;
            for (const Join& j : s.joins[i]) scope.bindings.push_back(BuildBinding(j.source, depth));
        }
        return scope;
    }

    void WalkConditions(const Select& s, const Scope& scope, int depth) {
        for (size_t i = 0; i < s.joins.size(); ++i) {
            for (const Join& j : s.joins[i]) Walk(j.on, scope, depth);
        }
        Walk(s.where, scope, depth);
        WalkList(s.group_by, scope, depth);
        Walk(s.having, scope, depth);
        WalkList(s.index_by, scope, depth);
    }

    void WalkOrderAndTotals(const Select& s, const Scope& scope, int depth, const AliasInfo& names) {
        for (const OrderItem& o : s.order_by) Walk(o.expr, scope, depth, &names);
        WalkList(s.totals_aggregates, scope, depth, &names);
        for (const TotalsItem& t : s.totals_by) {
            Walk(t.expr, scope, depth, &names);
            WalkList(t.periods_args, scope, depth, &names);
        }
    }

    // The row set is chosen, joined or grouped by protected data: a source that already is (temp table, derived
    // table, virtual table), a tainted ON / ГДЕ / ИМЕЮЩИЕ, or a tainted grouping whose key is not returned as is.
    bool RowsProtected(const Select& s, const Scope& scope) const {
        for (const Binding& b : scope.bindings) {
            if (b.rows_protected) return true;
        }
        for (const std::vector<Join>& joins : s.joins) {
            for (const Join& j : joins) {
                if (SubtreeTainted(j.on)) return true;
            }
        }
        if (SubtreeTainted(s.where) || SubtreeTainted(s.having)) return true;
        for (const ExprP& g : s.group_by) {
            if (SubtreeTainted(g) && !(g && GroupItemIsSelected(s, *g))) return true;
        }
        return false;
    }

    // Any tainted grouping key, or РАЗЛИЧНЫЕ over a tainted output column: the rows left are the distinct
    // protected values, so a consumer that counts them learns how many there are.
    bool GroupedByProtected(const Select& s, const SelectOut& out) const {
        for (const ExprP& g : s.group_by) {
            if (SubtreeTainted(g)) return true;
        }
        if (!s.distinct) return false;
        for (const Column& c : out.columns) {
            if (IsTaintedOrigin(c.origin)) return true;
        }
        return false;
    }

    SelectOut LabelItems(const Select& head, const Scope& scope, int depth, const std::vector<std::string>& names,
                         bool names_ok) {
        SelectOut out;
        for (size_t i = 0; i < head.items.size(); ++i) {
            const SelectItem& item = head.items[i];
            Column col;
            if (names_ok && i < names.size()) col.name = names[i];
            if (item.wildcard) {
                out.complete = false;   // not expanded: the column list is no longer enumerable
            } else if (item.expr) {
                const FieldLabel l = Label(*item.expr, scope, depth, nullptr);
                col.origin = l.origin;
                col.refs = l.refs;
                col.refs_known = l.refs_known;
            }
            out.columns.push_back(std::move(col));
            out.positions.push_back(item.pos);
        }
        return out;
    }

    void JoinUnionPart(SelectOut& head, const SelectOut& part, const Pos& pos) {
        if (part.columns.size() != head.columns.size()) {
            sink_.Issue(kResolveUnionArity, pos);
            for (Column& c : head.columns) {
                c.origin = Origin::Unknown;
                c.refs.clear();
                c.refs_known = false;
            }
            head.complete = false;
            return;
        }
        for (size_t i = 0; i < head.columns.size(); ++i) {
            head.columns[i].origin = JoinOrigin(head.columns[i].origin, part.columns[i].origin);
            // Targets survive only when BOTH sides are references; otherwise a dereference would be a guess.
            if (head.columns[i].refs.empty() || part.columns[i].refs.empty()) {
                // A reference joined with a non-reference may be a reference with unknown targets.
                const bool lost = !head.columns[i].refs.empty() || !part.columns[i].refs.empty();
                head.columns[i].refs_known = head.columns[i].refs_known && part.columns[i].refs_known && !lost;
                head.columns[i].refs.clear();
                continue;
            }
            head.columns[i].refs_known = head.columns[i].refs_known && part.columns[i].refs_known;
            for (const std::string& r : part.columns[i].refs) {
                bool have = false;
                for (const std::string& h : head.columns[i].refs) have = have || ScopeKey(h) == ScopeKey(r);
                if (!have) head.columns[i].refs.push_back(r);
            }
        }
        head.complete = head.complete && part.complete;
    }

    SelectOut AnalyzeSelect(const Select& head, const Scope* parent, int depth, bool allow_into) {
        if (depth > kMaxSelectDepth) {
            sink_.Issue(kResolveBadStatement, head.pos);
            return SelectOut();
        }
        if (!allow_into && !head.into.empty()) sink_.Issue(kResolveBadStatement, head.pos);

        std::vector<std::string> names;
        const bool names_ok = ComputeResultNames(head, names);
        if (!names_ok) sink_.Issue(kResolveColumnMismatch, head.pos);   // colliding explicit aliases

        const Scope scope = BuildScope(head, parent, depth);
        WalkConditions(head, scope, depth);
        SelectOut out = LabelItems(head, scope, depth, names, names_ok);
        if (!names_ok) out.complete = false;
        const bool own_rows = RowsProtected(head, scope);
        out.grouped = GroupedByProtected(head, out);
        out.rows_protected = own_rows || out.grouped;
        labels_.rows_protected[&head] = own_rows;

        for (const Select::UnionPart& u : head.unions) {
            if (!u.select) {
                sink_.Issue(kResolveBadStatement, head.pos);
                out.complete = false;
                continue;
            }
            SelectOut part = AnalyzeSelect(*u.select, parent, depth + 1, false);
            Consume(part);
            JoinUnionPart(out, part, u.select->pos);
            out.rows_protected = out.rows_protected || part.rows_protected;
        }
        labels_.result_protected[&head] = out.rows_protected;
        // ORDER BY / TOTALS belong to the whole union: their aliases carry the joined labels of the result columns.
        AliasInfo alias_info;
        alias_info.names = &names;
        alias_info.columns = &out.columns;
        WalkOrderAndTotals(head, scope, depth, alias_info);
        labels_.select_columns[&head] = out.columns;
        return out;
    }
};

}  // namespace

AnalysisResult AnalyzeQuery(const Batch& batch, const FieldPolicy& policy) {
    AnalysisResult result;
    LabelTable labels;
    Analyzer analyzer(policy, result, labels);
    analyzer.Run(batch);
    ApplyRules(batch, policy, labels, result);
    return result;
}

}  // namespace lineage::strict
