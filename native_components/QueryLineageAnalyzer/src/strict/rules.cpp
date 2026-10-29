#include "strict/rules.h"

#include <set>
#include <string>
#include <vector>

#include "strict/ast_dump.h"

namespace lineage::strict {

namespace {

enum class Ctx { SelectList, Condition, GroupBy, OrderBy, Totals, VtParam, JoinOn };

bool IsConditionCtx(Ctx c) { return c == Ctx::Condition || c == Ctx::JoinOn || c == Ctx::VtParam; }

// Anything that may hold protected content. Unknown is reported by its own rule.
bool Tainted(Origin o) {
    return o == Origin::ProtectedDirect || o == Origin::ProtectedDerived || o == Origin::Mixed;
}

bool IsLiteral(const Expr& e) {
    switch (e.kind) {
        case Expr::K::StringLit:
        case Expr::K::NumberLit:
        case Expr::K::BoolLit:
        case Expr::K::NullLit:
        case Expr::K::UndefinedLit: return true;
        default: return false;
    }
}

bool IsComparison(const std::string& op) {
    return op == "<>" || op == "<" || op == ">" || op == "<=" || op == ">=";
}

bool IsLogicalOp(const std::string& op) { return op == "И" || op == "ИЛИ" || op == "AND" || op == "OR"; }

bool Untainted(Origin o) { return o == Origin::Open || o == Origin::Client; }

class RuleEngine {
public:
    RuleEngine(const FieldPolicy& policy, const LabelTable& labels, AnalysisResult& out)
        : policy_(policy), labels_(labels), out_(out) {}

    void Run(const Batch& batch) {
        for (const Statement& st : batch.statements) {
            if (st.kind == Statement::K::Select && st.select) statements_.insert(st.select.get());
        }
        for (const Statement& st : batch.statements) {
            if (st.kind == Statement::K::Select && st.select) CheckSelect(*st.select);
        }
        CheckOutputColumns();
        for (const Statement& st : batch.statements) {
            if (st.kind == Statement::K::Select && st.select && st.select->into.empty()) CheckResultSet(*st.select);
        }
        MirrorResolution();
    }

private:
    const FieldPolicy& policy_;
    const LabelTable& labels_;
    AnalysisResult& out_;
    std::set<const Select*> statements_;   // the statement-level selects (ИТОГИ is considered only there)

    void Add(const char* code, const Pos& pos) {
        for (const Reason& r : out_.reasons) {
            if (r.code == code && r.pos.offset == pos.offset) return;
        }
        out_.reasons.push_back(Reason{code, pos});
    }

    Origin LabelOf(const Expr* e) const {
        if (e == nullptr) return Origin::Unknown;
        const auto it = labels_.expr.find(e);
        return it == labels_.expr.end() ? Origin::Unknown : it->second.origin;   // unlabelled: fail closed
    }

    bool TaintedExpr(const ExprP& e) const { return e && Tainted(LabelOf(e.get())); }

    // Join of the returned columns of a subquery; Unknown if they cannot be enumerated.
    Origin SubqueryOrigin(const Select& s) const {
        const auto it = labels_.select_columns.find(&s);
        if (it == labels_.select_columns.end() || it->second.empty()) return Origin::Unknown;
        Origin o = it->second[0].origin;
        for (size_t i = 1; i < it->second.size(); ++i) o = JoinOrigin(o, it->second[i].origin);
        if (o == Origin::Unknown) return o;
        const auto rows = labels_.result_protected.find(&s);
        if (rows == labels_.result_protected.end() || rows->second) o = JoinOrigin(o, Origin::ProtectedDerived);
        return o;
    }

    bool FlagOf(const std::map<const Select*, bool>& flags, const Select& s) const {
        const auto it = flags.find(&s);
        return it == flags.end() || it->second;   // unrecorded: fail closed
    }

    // ---- output and resolution ----

    void CheckOutputColumns() {
        for (const ColumnLabel& c : out_.columns) {
            switch (c.origin) {
                case Origin::Mixed: Add("Q_MIXED_COLUMN", c.pos); break;
                case Origin::Unknown: Add("Q_UNKNOWN_COLUMN", c.pos); break;
                case Origin::ProtectedDerived: Add("Q_FUNC_OVER_PROTECTED", c.pos); break;   // safety net
                default: break;
            }
        }
    }

    // Every statement that returns rows to the client (not ПОМЕСТИТЬ), whether or not it is the last one.
    void CheckResultSet(const Select& s) {
        const auto it = labels_.select_columns.find(&s);
        if (it == labels_.select_columns.end()) {
            Add("Q_UNKNOWN_COLUMN", s.pos);
            return;
        }
        for (size_t i = 0; i < it->second.size(); ++i) {
            const Pos& pos = it->second.size() == s.items.size() ? s.items[i].pos : s.pos;
            switch (it->second[i].origin) {
                case Origin::Mixed: Add("Q_MIXED_COLUMN", pos); break;
                case Origin::Unknown: Add("Q_UNKNOWN_COLUMN", pos); break;
                case Origin::ProtectedDerived: Add("Q_FUNC_OVER_PROTECTED", pos); break;
                default: break;
            }
        }
    }

    // A literal as an ordering / grouping item may be read by the platform as a column ordinal.
    void CheckOrdinal(const ExprP& e) {
        if (e && IsLiteral(*e)) Add("Q_ORDER_PROTECTED", e->pos);
    }

    // УПОРЯДОЧИТЬ ПО <n>: true when `e` is a plain ordinal (1..N, no leading zero) of a result column whose label is
    // open or client, i.e. the ordering is the one by that column. Every column must be enumerable (no wildcard).
    bool OrdinalOfOpenColumn(const Select& s, const Expr& e) const {
        if (e.kind != Expr::K::NumberLit || e.text.empty() || e.text.size() > 4 || e.text[0] == '0') return false;
        size_t n = 0;
        for (const char c : e.text) {
            if (c < '0' || c > '9') return false;
            n = n * 10 + static_cast<size_t>(c - '0');
        }
        const auto it = labels_.select_columns.find(&s);
        if (it == labels_.select_columns.end() || it->second.size() != s.items.size()) return false;
        for (const SelectItem& item : s.items) {
            if (item.wildcard) return false;
        }
        if (n == 0 || n > it->second.size()) return false;
        const Origin o = it->second[n - 1].origin;
        return (o == Origin::Open || o == Origin::Client) && it->second[n - 1].refs_known &&
               PresentationsOpen(it->second[n - 1].refs);
    }

    // Ordering by a reference: the platform documentation states only that АВТОУПОРЯДОЧИВАНИЕ replaces a reference by
    // the default sort fields of its table (код / наименование / дата); it does not say that a plain УПОРЯДОЧИТЬ ПО,
    // an ordinal or ИТОГИ ПО avoid the presentation, and ТаблицаЗначений.Сортировать without a comparison object
    // compares "элементы остальных типов ... по строковому представлению". So any ordering by a reference is
    // allowed only when every target's presentation is open (fail closed).
    bool PresentationsOpen(const std::vector<std::string>& targets) const {
        for (const std::string& t : targets) {
            if (!policy_.HasTable(t) || policy_.PresentationClass(t) != FieldClass::Open) return false;
        }
        return true;
    }

    bool OrdersByHiddenPresentation(const ExprP& e) const {
        if (!e) return false;
        const auto it = labels_.expr.find(e.get());
        // unlabelled, or a value that may be a reference with unknown targets: fail closed
        return it == labels_.expr.end() || !it->second.refs_known || !PresentationsOpen(it->second.refs);
    }

    void MirrorResolution() {
        for (const ResolveIssue& issue : out_.resolution_issues) {
            const std::string code = "Q_" + issue.code;   // RESOLVE_X -> Q_RESOLVE_X
            bool have = false;
            for (const Reason& r : out_.reasons) have = have || (r.code == code && r.pos.offset == issue.pos.offset);
            if (!have) out_.reasons.push_back(Reason{code, issue.pos});
        }
        if (!out_.missing_tables.empty()) out_.reasons.push_back(Reason{"Q_INCOMPLETE", Pos()});
        const bool clean = out_.reasons.empty() && out_.missing_tables.empty() && out_.resolution_issues.empty();
        out_.verdict = clean ? Verdict::Safe : Verdict::Confirm;
    }

    // ---- statements ----

    void CheckSource(const Source& src) {
        if (src.kind == Source::K::Subquery && src.subquery) CheckSelect(*src.subquery);
        if (src.kind != Source::K::VirtualTable) return;
        // The slot meaning per kind of virtual table (period, boundary, periodicity, condition) is not derivable
        // here, so fail closed: only the LAST slot (the condition of Остатки / Обороты / Срез*) may hold
        // protected data, and only as a condition; any tainted earlier slot is Q_VT_PARAM.
        for (size_t i = 0; i < src.vt_params.size(); ++i) {
            const ExprP& p = src.vt_params[i];
            if (!p) continue;   // an empty slot is normal
            Walk(*p, Ctx::VtParam, false);
            const bool last = i + 1 == src.vt_params.size();
            if (TaintedExpr(p) && (!last || !ConditionShaped(*p))) Add("Q_VT_PARAM", p->pos);
        }
    }

    // A slot may hold a protected value only as part of a condition over the register's fields.
    static bool ConditionShaped(const Expr& e) {
        switch (e.kind) {
            case Expr::K::Binary: return e.text == "=" || IsComparison(e.text) || IsLogicalOp(e.text);
            case Expr::K::Unary: return e.text == "НЕ" || e.text == "NOT";
            case Expr::K::InList:
            case Expr::K::InSubquery:
            case Expr::K::InHierarchy:
            case Expr::K::Between:
            case Expr::K::Like:
            case Expr::K::IsNullTest:
            case Expr::K::RefsTest: return true;
            default: return false;
        }
    }

    // Aggregates of this select (not those of its subqueries, which have their own row sets).
    void CollectAggregates(const Expr& e, std::vector<const Expr*>& out) const {
        if (e.kind == Expr::K::Call) {
            const CallClass c = ClassifyCall(e.text);
            if (c == CallClass::Count || c == CallClass::Sum || c == CallClass::MinMax) out.push_back(&e);
        }
        for (const ExprP& a : e.args) {
            if (a) CollectAggregates(*a, out);
        }
    }

    // A COUNT / SUM / MIN / MAX over a row set chosen by protected data reveals the cardinality of that choice.
    void CheckAggregateRows(const Select& s) {
        const bool own = FlagOf(labels_.rows_protected, s);
        const bool whole = FlagOf(labels_.result_protected, s);
        std::vector<const Expr*> own_aggs;
        std::vector<const Expr*> other_aggs;
        for (const SelectItem& item : s.items) {
            if (item.expr) CollectAggregates(*item.expr, own_aggs);
        }
        if (s.having) CollectAggregates(*s.having, own_aggs);
        for (const OrderItem& o : s.order_by) {
            if (o.expr) CollectAggregates(*o.expr, other_aggs);
        }
        for (const ExprP& a : s.totals_aggregates) {
            if (a) CollectAggregates(*a, other_aggs);
        }
        for (const Expr* agg : own_aggs) {
            if (own || AnyArgTainted(*agg) || ReadsOuter(*agg)) Add("Q_AGGREGATE_ROWSET", agg->pos);
        }
        for (const Expr* agg : other_aggs) {
            if (own || whole || AnyArgTainted(*agg) || ReadsOuter(*agg)) Add("Q_AGGREGATE_ROWSET", agg->pos);
        }
    }

    // An aggregate of a subquery over an outer reference is computed over the enclosing query's row set, which
    // this select's flags do not describe: fail closed.
    bool ReadsOuter(const Expr& e) const {
        if (labels_.outer_refs.count(&e) != 0) return true;
        for (const ExprP& a : e.args) {
            if (a && ReadsOuter(*a)) return true;
        }
        return false;
    }

    // Grouping by protected data hides the key unless it is returned as is: the group counts and sums then show
    // how many rows share a hidden value.
    void CheckGrouping(const Select& s) {
        for (const ExprP& g : s.group_by) {
            if (TaintedExpr(g) && !GroupItemIsSelected(s, *g)) Add("Q_GROUP_PROTECTED", g->pos);
        }
    }

    bool HasRefs(const Expr* e) const {
        const auto it = labels_.expr.find(e);
        // unlabelled, a reference, or a value that may be one: fail closed
        return it == labels_.expr.end() || !it->second.refs_known || !it->second.refs.empty();
    }

    // A ИТОГИ ПО item that names one result column (a single-part path equal to exactly one result name).
    static bool NamesResultColumn(const Expr& e, const std::vector<std::string>& names) {
        if (e.kind != Expr::K::Field || e.parts.size() != 1) return false;
        size_t folded = 0;
        bool exact = false;
        for (const std::string& n : names) {
            if (FoldedKey(n) != FoldedKey(e.parts[0])) continue;
            ++folded;
            exact = exact || ScopeKey(n) == ScopeKey(e.parts[0]);
        }
        return folded == 1 && exact;
    }

    // ИТОГИ adds rows computed from the detail rows only when nothing about the query is protected: every result
    // column open or client, the row set not chosen by protected data, the grouping items returned columns (open, or
    // references whose every target has an open presentation), the aggregates СУММА / СРЕДНЕЕ / КОЛИЧЕСТВО / МИНИМУМ / МАКСИМУМ over open non-reference data.
    // ОБЩИЕ, ИЕРАРХИЯ, ПЕРИОДАМИ, ПЕРВЫЕ, ПОМЕСТИТЬ and a subquery keep the refusal (Q_TOTALS).
    bool TotalsAllowed(const Select& s) const {
        if (!statements_.count(&s) || !s.into.empty() || !s.top.empty() || s.totals_by.empty()) return false;
        if (FlagOf(labels_.rows_protected, s) || FlagOf(labels_.result_protected, s)) return false;
        const auto it = labels_.select_columns.find(&s);
        if (it == labels_.select_columns.end() || it->second.size() != s.items.size()) return false;
        for (const SelectItem& item : s.items) {
            if (item.wildcard) return false;
        }
        for (const Column& c : it->second) {
            if (!Untainted(c.origin)) return false;
        }
        std::vector<std::string> names;
        if (!ComputeResultNames(s, names)) return false;
        for (const TotalsItem& t : s.totals_by) {
            if (t.overall || t.hierarchy || t.only_hierarchy || t.periods || !t.expr) return false;
            if (!NamesResultColumn(*t.expr, names) || !Untainted(LabelOf(t.expr.get()))) return false;
            if (OrdersByHiddenPresentation(t.expr)) return false;   // totals rows are ordered by the grouping
        }
        for (const ExprP& a : s.totals_aggregates) {
            if (!a || a->kind != Expr::K::Call || !Untainted(LabelOf(a.get()))) return false;
            const CallClass c = ClassifyCall(a->text);
            if (c != CallClass::Count && c != CallClass::Sum && c != CallClass::MinMax) return false;
            for (const ExprP& x : a->args) {
                if (!x || !Untainted(LabelOf(x.get())) || HasRefs(x.get())) return false;
            }
        }
        return true;
    }

    bool AutoorderTainted(const Select& s) const {
        const auto it = labels_.select_columns.find(&s);
        if (it == labels_.select_columns.end()) return true;
        for (const Column& c : it->second) {
            if (Tainted(c.origin) || c.origin == Origin::Unknown) return true;
            // automatic ordering sorts a reference by its presentation
            if (!c.refs_known) return true;   // may be a reference whose targets were lost
            for (const std::string& target : c.refs) {
                if (!policy_.HasTable(target) || policy_.PresentationClass(target) != FieldClass::Open) return true;
            }
        }
        return false;
    }

    void CheckSelect(const Select& s) {
        if (s.for_update) Add("Q_FOR_UPDATE", s.pos);
        for (const SelectItem& item : s.items) {
            if (item.wildcard) Add("Q_WILDCARD", item.pos);
            else if (item.expr) Walk(*item.expr, Ctx::SelectList, true);
            else Add("Q_UNKNOWN_COLUMN", item.pos);
        }
        for (size_t i = 0; i < s.from.size(); ++i) {
            CheckSource(s.from[i]);
            if (i >= s.joins.size()) continue;
            for (const Join& j : s.joins[i]) {
                CheckSource(j.source);
                WalkOpt(j.on, Ctx::JoinOn, j.pos, false);
            }
        }
        WalkOpt(s.where, Ctx::Condition, s.pos, false);
        for (const ExprP& g : s.group_by) {
            WalkOpt(g, Ctx::GroupBy, s.pos);
            CheckOrdinal(g);
        }
        CheckGrouping(s);
        WalkOpt(s.having, Ctx::Condition, s.pos, false);
        for (const ExprP& g : s.index_by) WalkOpt(g, Ctx::GroupBy, s.pos);

        bool ordering = false;
        for (const OrderItem& o : s.order_by) {
            WalkOpt(o.expr, Ctx::OrderBy, s.pos);
            if (o.expr && IsLiteral(*o.expr) && !OrdinalOfOpenColumn(s, *o.expr)) {
                CheckOrdinal(o.expr);
                ordering = true;
            }
            if (TaintedExpr(o.expr) || OrdersByHiddenPresentation(o.expr)) {
                Add("Q_ORDER_PROTECTED", o.expr->pos);
                ordering = true;
            }
        }
        if (s.autoorder && AutoorderTainted(s)) {
            Add("Q_ORDER_PROTECTED", s.pos);
            ordering = true;
        }
        if (ordering && !s.top.empty()) Add("Q_TOP_WITH_ORDER", s.pos);

        if (s.has_totals && !TotalsAllowed(s)) Add("Q_TOTALS", s.pos);
        for (const ExprP& a : s.totals_aggregates) WalkOpt(a, Ctx::Totals, s.pos);
        for (const TotalsItem& t : s.totals_by) {
            if (t.periods) Add("Q_PERIODS", s.pos);
            WalkOpt(t.expr, Ctx::Totals, s.pos, false);
            CheckOrdinal(t.expr);
            for (const ExprP& a : t.periods_args) WalkOpt(a, Ctx::Totals, s.pos);
        }
        CheckAggregateRows(s);
        for (const Select::UnionPart& u : s.unions) {
            if (u.select) CheckSelect(*u.select);
        }
    }

    // A null slot is malformed (fail closed); `required` false: null is the normal "absent" (WHERE, ИТОГИ ОБЩИЕ).
    void WalkOpt(const ExprP& e, Ctx ctx, const Pos& parent, bool required = true) {
        if (e) Walk(*e, ctx, false);
        else if (required) Add("Q_UNKNOWN_EXPR", parent);
    }

    // ---- expressions ----

    bool ChildUnknown(const Expr& e) const {
        for (const ExprP& a : e.args) {
            if (!a || LabelOf(a.get()) == Origin::Unknown) return true;
        }
        return e.subquery && SubqueryOrigin(*e.subquery) == Origin::Unknown;
    }

    void Walk(const Expr& e, Ctx ctx, bool item_root) {
        const int period = e.kind == Expr::K::Call ? PeriodArgIndex(e.text) : -1;
        for (size_t i = 0; i < e.args.size(); ++i) {
            const ExprP& a = e.args[i];
            if (!a) {
                Add("Q_UNKNOWN_EXPR", e.pos);   // malformed node: fail closed
                continue;
            }
            if (static_cast<int>(i) == period && a->kind == Expr::K::Field && a->parts.size() == 1 &&
                IsPeriodName(a->parts[0])) {
                continue;
            }
            Walk(*a, ctx, false);
        }
        if (e.subquery) CheckSelect(*e.subquery);

        const bool unrecognized = e.kind == Expr::K::Call && ClassifyCall(e.text) == CallClass::Unrecognized;
        if (unrecognized) Add("Q_UNKNOWN_FUNCTION", e.pos);
        if (LabelOf(&e) == Origin::Unknown && !unrecognized) {
            if (item_root) Add("Q_UNKNOWN_COLUMN", e.pos);
            else if (!ChildUnknown(e)) Add("Q_UNKNOWN_EXPR", e.pos);
        }
        ApplyNodeRules(e, ctx, item_root);
    }

    // Reason for "a protected value is compared with `other`" when exactly one side is a direct protected read
    // (a bound token or a protected field). `la`/`lb` are the operand labels; `other` the non-protected operand.
    void EqRule(Origin la, Origin lb, const Expr* a, const Expr* b, Ctx ctx, const Pos& pos) {
        const bool ta = Tainted(la);
        const bool tb = Tainted(lb);
        if (!ta && !tb) return;
        if (!IsConditionCtx(ctx)) Add("Q_FUNC_OVER_PROTECTED", pos);   // a computed bit of a protected value
        if (la == Origin::ProtectedDirect && lb == Origin::ProtectedDirect) return;
        if ((ta && la != Origin::ProtectedDirect) || (tb && lb != Origin::ProtectedDirect)) {
            Add("Q_COMPARE_PROTECTED", pos);
            return;
        }
        const Expr* other = ta ? b : a;
        if (other != nullptr && other->kind == Expr::K::Param &&
            policy_.OriginOfParam(other->text) == ParamOrigin::TokenUnknown) {
            Add("Q_EQ_UNKNOWN_TOKEN", pos);
        } else if (other != nullptr && IsLiteral(*other)) {
            Add("Q_EQ_LITERAL", pos);
        } else {
            Add("Q_EQ_CLIENT_PARAM", pos);   // client parameter, client column or an open (client-readable) column
        }
    }

    void ApplyNodeRules(const Expr& e, Ctx ctx, bool item_root) {
        const bool any = AnyArgTainted(e);
        switch (e.kind) {
            case Expr::K::Call: CallRules(e, ctx, item_root, any); break;
            case Expr::K::Cast:
            case Expr::K::Case:
            case Expr::K::IsNullTest:
                if (any) Add("Q_FUNC_OVER_PROTECTED", e.pos);
                break;
            case Expr::K::Unary: UnaryRules(e, ctx, any); break;
            case Expr::K::Binary: BinaryRules(e, ctx, any); break;
            case Expr::K::InList: InListRules(e, ctx); break;
            case Expr::K::InSubquery:
            case Expr::K::InHierarchy: InSubqueryRules(e, ctx); break;
            case Expr::K::Between:
                if (any) Add("Q_COMPARE_PROTECTED", e.pos);
                break;
            case Expr::K::Like:
                if (any) Add("Q_LIKE_PROTECTED", e.pos);
                break;
            default: break;   // leaves, ССЫЛКА type test, Row
        }
    }

    bool AnyArgTainted(const Expr& e) const {
        for (const ExprP& a : e.args) {
            if (TaintedExpr(a)) return true;
        }
        return false;
    }

    void CallRules(const Expr& e, Ctx ctx, bool item_root, bool any) {
        switch (ClassifyCall(e.text)) {
            case CallClass::Count: return;   // COUNT([DISTINCT] x) is an open number
            case CallClass::MinMax:
                if (any) Add("Q_MINMAX_PROTECTED", e.pos);
                return;
            case CallClass::Presentation:
                if (any) Add("Q_FUNC_OVER_PROTECTED", e.pos);   // a presentation of a protected scalar
                else if (Tainted(LabelOf(&e)) && !(item_root && ctx == Ctx::SelectList)) Add("Q_PRESENTATION", e.pos);
                return;
            case CallClass::Sum:
            case CallClass::TypeOf:
            case CallClass::Plain:
                if (any) Add("Q_FUNC_OVER_PROTECTED", e.pos);
                return;
            case CallClass::Unrecognized: return;   // Q_UNKNOWN_FUNCTION already added
        }
    }

    void UnaryRules(const Expr& e, Ctx ctx, bool any) {
        if (!any) return;
        const bool logical = e.text == "НЕ" || e.text == "NOT";
        if (!logical || !IsConditionCtx(ctx)) Add("Q_FUNC_OVER_PROTECTED", e.pos);
    }

    void BinaryRules(const Expr& e, Ctx ctx, bool any) {
        if (e.args.size() != 2 || !e.args[0] || !e.args[1]) {
            Add("Q_UNKNOWN_EXPR", e.pos);
            return;
        }
        if (!any) return;
        if (e.text == "=") {
            EqRule(LabelOf(e.args[0].get()), LabelOf(e.args[1].get()), e.args[0].get(), e.args[1].get(), ctx, e.pos);
        } else if (IsComparison(e.text)) {
            Add("Q_COMPARE_PROTECTED", e.pos);
            if (!IsConditionCtx(ctx)) Add("Q_FUNC_OVER_PROTECTED", e.pos);
        } else if (IsLogicalOp(e.text)) {
            if (!IsConditionCtx(ctx)) Add("Q_FUNC_OVER_PROTECTED", e.pos);
        } else {
            Add("Q_FUNC_OVER_PROTECTED", e.pos);   // arithmetic (and any operator this engine does not know)
        }
    }

    void InListRules(const Expr& e, Ctx ctx) {
        if (e.args.empty() || !e.args[0]) {
            Add("Q_UNKNOWN_EXPR", e.pos);
            return;
        }
        const Origin l0 = LabelOf(e.args[0].get());
        for (size_t i = 1; i < e.args.size(); ++i) {
            if (!e.args[i]) continue;
            EqRule(l0, LabelOf(e.args[i].get()), e.args[0].get(), e.args[i].get(), ctx, e.pos);
        }
    }

    void InSubqueryRules(const Expr& e, Ctx ctx) {
        if (e.args.empty() || !e.args[0]) {
            Add("Q_UNKNOWN_EXPR", e.pos);
            return;
        }
        const Origin l0 = LabelOf(e.args[0].get());
        const Origin ls = e.subquery ? SubqueryOrigin(*e.subquery) : Origin::Unknown;
        if (e.kind == Expr::K::InHierarchy) {
            if (Tainted(l0) || Tainted(ls) || AnyArgTainted(e)) Add("Q_COMPARE_PROTECTED", e.pos);
            return;
        }
        EqRule(l0, ls, e.args[0].get(), nullptr, ctx, e.pos);
    }
};

}  // namespace

void ApplyRules(const Batch& batch, const FieldPolicy& policy, const LabelTable& labels, AnalysisResult& result) {
    RuleEngine engine(policy, labels, result);
    engine.Run(batch);
}

}  // namespace lineage::strict
