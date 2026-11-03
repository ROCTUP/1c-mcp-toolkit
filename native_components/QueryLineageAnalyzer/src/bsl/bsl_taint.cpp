#include "bsl/bsl_taint.h"

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "bsl/bsl_allowlist.h"
#include "bsl/bsl_tokenizer.h"
#include "strict/check_query.h"
#include "utf_utils.h"

namespace lineage::bsl {

namespace {

using Json = nlohmann::json;

constexpr int kMaxPasses = 6;    // passes until the query parameters bound by the code are stable
constexpr int kMaxRounds = 10;   // walks of the whole body in one pass until the collection state is stable

bool IsAsciiLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// A token starts at s[open] == '['; on success `end` is the index after its ']'.
bool TokenAt(const std::string& s, size_t open, size_t& end) {
    if (open >= s.size() || s[open] != '[') return false;
    size_t i = open + 1;
    const size_t letters = i;
    while (i < s.size() && IsAsciiLetter(s[i])) ++i;
    if (i == letters || i >= s.size() || s[i] != '-') return false;
    const size_t digits = ++i;
    while (i < s.size() && IsDigit(s[i])) ++i;
    if (i - digits < 5 || i >= s.size() || s[i] != ']') return false;
    end = i + 1;
    return true;
}

std::string AsciiUpper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

strict::ParamOrigin ParamOriginOf(Origin label) {
    switch (label) {
        case Origin::Open:
        case Origin::Client: return strict::ParamOrigin::Client;
        case Origin::ProtectedDirect: return strict::ParamOrigin::TokenProtected;
        default: return strict::ParamOrigin::TokenUnknown;
    }
}

using Params = std::map<std::string, strict::ParamOrigin>;   // spelling -> origin

Params ParamsOf(const LabelState& state, const Expr* site) {
    Params out;
    const auto it = state.params.find(site);
    if (it == state.params.end()) return out;
    for (const auto& kv : it->second) {
        for (const std::string& spelling : kv.second.spellings) out[spelling] = ParamOriginOf(kv.second.label);
    }
    return out;
}

// CheckQuery over the texts of each Запрос site, cached per text and parameter set.
class QueryChecker {
public:
    QueryChecker(const strict::FieldPolicy& policy, std::map<const Expr*, std::vector<QueryTextUse>> sites,
                 size_t* work)
        : policy_(policy), sites_(std::move(sites)), work_(work) {}

    const std::map<const Expr*, std::vector<QueryTextUse>>& Sites() const { return sites_; }

    // Each check is charged to the analysis budget; over it the query is not checked at all (Q_LIMIT).
    const strict::QueryCheckResult& Check(const std::string& text, const Params& params) {
        std::string params_key;
        for (const auto& kv : params) {
            params_key += kv.first + '=' + std::to_string(static_cast<int>(kv.second)) + '\n';
        }
        const std::string key = params_key + '\n' + text;
        auto it = checks_.find(key);
        if (it != checks_.end()) return it->second;
        *work_ += text.size();
        if (*work_ > kMaxCodeWork) {
            strict::QueryCheckResult over;
            strict::QueryReason reason;
            reason.code = "Q_LIMIT";
            over.reasons.push_back(reason);
            return checks_.emplace(key, std::move(over)).first->second;
        }
        const strict::FieldPolicy* policy = PolicyWith(params_key, params);
        if (!policy) {
            strict::QueryCheckResult invalid;
            strict::QueryReason reason;
            reason.code = "Q_POLICY_INVALID";
            invalid.reasons.push_back(reason);
            return checks_.emplace(key, std::move(invalid)).first->second;
        }
        return checks_.emplace(key, strict::CheckQueryWithPolicy(text, *policy)).first->second;
    }

    // The columns of a site: the worst label across its texts. Null when a text did not parse, a column has no
    // name, or the texts disagree on the column names.
    std::shared_ptr<const ColumnSet> Columns(const Expr* site, const Params& params) {
        const auto it = sites_.find(site);
        if (it == sites_.end() || it->second.empty()) return nullptr;
        std::shared_ptr<ColumnSet> merged;
        for (const QueryTextUse& use : it->second) {
            const strict::QueryCheckResult& r = Check(use.text, params);
            if (!r.parsed) return nullptr;
            ColumnSet set;
            for (const strict::ColumnLabel& c : r.columns) {
                if (c.name.empty()) return nullptr;
                ColumnInfo info;
                info.name = c.name;
                info.origin = c.origin;
                info.refs = c.refs;
                info.refs_known = c.refs_known;
                const std::string name = AllowlistKey(c.name);
                const auto existing = set.columns.find(name);
                if (existing == set.columns.end()) {
                    set.columns.emplace(name, std::move(info));
                } else {
                    if (NormalizeIdentifierKey(existing->second.name) != NormalizeIdentifierKey(info.name)) {
                        existing->second.name_collision = true;
                    }
                    MergeInto(existing->second, info);
                }
            }
            if (!merged) {
                merged = std::make_shared<ColumnSet>(std::move(set));
                continue;
            }
            if (merged->columns.size() != set.columns.size()) return nullptr;
            for (auto& kv : merged->columns) {
                const auto other = set.columns.find(kv.first);
                if (other == set.columns.end()) return nullptr;
                if (NormalizeIdentifierKey(kv.second.name) != NormalizeIdentifierKey(other->second.name) ||
                    other->second.name_collision) {
                    kv.second.name_collision = true;
                }
                MergeInto(kv.second, other->second);
            }
        }
        return merged;
    }

private:
    // One policy copy per distinct parameter set, shared by every text checked with it. Null when two spellings
    // that collide as parameter names carry different origins (FieldPolicy::SetParams). ParamsOf never yields
    // that today: it gives every spelling of one binding the binding's origin, and a binding groups spellings by
    // AllowlistKey, which folds at least what the parameter name normalization folds.
    const strict::FieldPolicy* PolicyWith(const std::string& params_key, const Params& params) {
        auto it = policies_.find(params_key);
        if (it == policies_.end()) {
            auto policy = std::make_unique<strict::FieldPolicy>(policy_);
            if (!policy->SetParams(params)) policy.reset();
            it = policies_.emplace(params_key, std::move(policy)).first;
        }
        return it->second.get();
    }

    // Targets survive only when both sides are references (as for ОБЪЕДИНИТЬ in the query checker).
    static void MergeInto(ColumnInfo& into, const ColumnInfo& other) {
        into.origin = JoinOrigin(into.origin, other.origin);
        if (into.refs.empty() || other.refs.empty()) {
            const bool lost = !into.refs.empty() || !other.refs.empty();   // a reference on one side only
            into.refs_known = into.refs_known && other.refs_known && !lost;
            into.refs.clear();
            return;
        }
        into.refs_known = into.refs_known && other.refs_known;
        for (const std::string& r : other.refs) {
            bool have = false;
            for (const std::string& h : into.refs) have = have || h == r;
            if (!have) into.refs.push_back(r);
        }
    }

    const strict::FieldPolicy& policy_;
    std::map<const Expr*, std::vector<QueryTextUse>> sites_;
    size_t* work_;
    std::map<std::string, std::unique_ptr<strict::FieldPolicy>> policies_;
    std::map<std::string, strict::QueryCheckResult> checks_;
};

void AddReason(CodeAnalysis& a, const std::string& code, const Pos& pos) {
    for (const CodeReason& r : a.reasons) {
        if (r.code == code && r.pos.offset == pos.offset) return;
    }
    a.reasons.push_back(CodeReason{code, pos});
}

std::set<const Expr*> TextExprs(const CodeAnalysis& a) {
    std::set<const Expr*> out;
    for (const QueryTextUse& use : a.query_texts) out.insert(use.text_expr);
    return out;
}

// ---- Risk classes ---------------------------------------------------------------------------------------

constexpr unsigned kRiskProtection = 1, kRiskDeferred = 2, kRiskWrite = 4, kRiskCredentials = 8, kRiskUnknown = 16;
constexpr size_t kMaxRisksPerClass = 50;   // the gate shows markers, not every occurrence

struct RiskEntry {
    std::string key;
    RiskMatch match;
    unsigned bit;
};

unsigned RiskBit(RiskClass c) {
    switch (c) {
        case RiskClass::Protection: return kRiskProtection;
        case RiskClass::Deferred: return kRiskDeferred;
        case RiskClass::Write: return kRiskWrite;
        case RiskClass::Credentials: return kRiskCredentials;
    }
    return kRiskProtection;
}

const char* RiskCode(unsigned bit) {
    switch (bit) {
        case kRiskDeferred: return "R_DEFERRED";
        case kRiskWrite: return "R_WRITE";
        case kRiskCredentials: return "R_CREDENTIALS";
        case kRiskUnknown: return "R_UNKNOWN_EFFECT";
        default: return "R_PROTECTION";
    }
}

const std::vector<RiskEntry>& RiskEntries() {
    static const std::vector<RiskEntry> entries = [] {
        std::vector<RiskEntry> v;
        for (const RiskRule& r : kRiskRules) v.push_back(RiskEntry{AllowlistKey(r.name), r.match, RiskBit(r.cls)});
        return v;
    }();
    return entries;
}

bool RuleMatches(const RiskEntry& e, const std::string& key) {
    switch (e.match) {
        case RiskMatch::Exact: return key == e.key;
        case RiskMatch::Prefix: return key.compare(0, e.key.size(), e.key) == 0;
        case RiskMatch::Contains: return key.find(e.key) != std::string::npos;
    }
    return false;
}

struct AllowedNames {
    std::set<std::string> globals, news, methods;
};

const AllowedNames& Allowed() {
    static const AllowedNames names = [] {
        AllowedNames n;
        for (const GlobalRule& r : kGlobalRules) {
            n.globals.insert(AllowlistKey(r.ru));
            n.globals.insert(AllowlistKey(r.en));
        }
        for (const NewRule& r : kNewRules) {
            n.news.insert(AllowlistKey(r.ru));
            n.news.insert(AllowlistKey(r.en));
        }
        for (const MethodRule& r : kMethodRules) {
            n.methods.insert(AllowlistKey(r.ru));
            n.methods.insert(AllowlistKey(r.en));
        }
        return n;
    }();
    return names;
}

// Classes of one name (AllowlistKey already applied). An allowlisted method name (УстановитьПараметр) is exempt from
// the Prefix rows: it is the allowlist's own, on its own receiver type; on any other receiver the unknown-effect
// risk applies.
unsigned RiskMaskOfKey(const std::string& key) {
    unsigned mask = 0;
    const bool allowed = Allowed().methods.count(key) != 0;
    for (const RiskEntry& e : RiskEntries()) {
        if ((mask & e.bit) != 0 || !RuleMatches(e, key)) continue;
        if (allowed && e.match == RiskMatch::Prefix) continue;
        mask |= e.bit;
    }
    return mask;
}

// Characters that look alike across scripts: Latin capitals with their Cyrillic (including Ukrainian / Serbian) and
// Greek twins. A mixed-script identifier is refused by the parser, but the gate still has to show what it imitates.
struct Lookalike {
    wchar_t latin, cyrillic, greek;
};
constexpr Lookalike kLookalikes[] = {
    {L'A', 0x0410, 0x0391}, {L'B', 0x0412, 0x0392}, {L'C', 0x0421, 0},      {L'E', 0x0415, 0x0395},
    {L'H', 0x041D, 0x0397}, {L'K', 0x041A, 0x039A}, {L'M', 0x041C, 0x039C}, {L'O', 0x041E, 0x039F},
    {L'P', 0x0420, 0x03A1}, {L'T', 0x0422, 0x03A4}, {L'X', 0x0425, 0x03A7}, {L'Y', 0x0423, 0x03A5},
    {L'I', 0x0406, 0x0399}, {L'S', 0x0405, 0},      {L'J', 0x0408, 0},      {L'Z', 0, 0x0396},
    {L'N', 0, 0x039D},
};

// Greek twins always become Latin; then Latin <-> Cyrillic in the requested direction.
std::string FoldHomoglyphs(const std::string& key, bool to_cyrillic) {
    std::wstring units = Utf8ToUtf16Units(key);
    for (wchar_t& u : units) {
        for (const Lookalike& l : kLookalikes) {
            if (l.greek != 0 && u == l.greek) u = l.latin;
        }
        for (const Lookalike& l : kLookalikes) {
            if (l.cyrillic == 0) continue;
            if (to_cyrillic && u == l.latin) { u = l.cyrillic; break; }
            if (!to_cyrillic && u == l.cyrillic) { u = l.latin; break; }
        }
    }
    return Utf16UnitsToUtf8(units);
}

// A name that may hide behind a script mix: any non-ASCII, non-Cyrillic character, or Latin together with Cyrillic.
bool NeedsFold(const std::string& key) {
    bool ascii = false, cyrillic = false;
    for (wchar_t u : Utf8ToUtf16Units(key)) {
        if (u < 0x80) {
            if (u >= L'A' && u <= L'Z') ascii = true;
        } else if (u >= 0x0400 && u <= 0x04FF) {
            cyrillic = true;
        } else {
            return true;
        }
    }
    return ascii && cyrillic;
}

class RiskCollector {
public:
    explicit RiskCollector(std::vector<CodeReason>& out) : out_(out) {}

    void Name(const std::string& name, const Pos& pos, bool mixed = false) {
        if (name.empty()) return;
        const std::string key = AllowlistKey(name);
        unsigned mask = Cached(key);
        if (mixed || NeedsFold(key)) {
            mask |= Cached(AllowlistKey(FoldHomoglyphs(key, true)));
            mask |= Cached(AllowlistKey(FoldHomoglyphs(key, false)));
        }
        Add(mask, pos);
    }

    void Add(unsigned mask, const Pos& pos) {
        for (unsigned bit = 1; bit <= kRiskUnknown; bit <<= 1) {
            if ((mask & bit) == 0 || counts_[bit] >= kMaxRisksPerClass) continue;
            ++counts_[bit];
            out_.push_back(CodeReason{RiskCode(bit), pos});
        }
    }

private:
    unsigned Cached(const std::string& key) {
        const auto it = memo_.find(key);
        if (it != memo_.end()) return it->second;
        return memo_[key] = RiskMaskOfKey(key);
    }

    std::vector<CodeReason>& out_;
    std::map<std::string, unsigned> memo_;
    std::map<unsigned, size_t> counts_;
};

void CollectExprNames(const Expr* root, RiskCollector& risks) {
    std::vector<const Expr*> stack{root};
    while (!stack.empty()) {
        const Expr* e = stack.back();
        stack.pop_back();
        if (!e) continue;
        const bool named = e->kind == Expr::K::Var || e->kind == Expr::K::Member || e->kind == Expr::K::Call ||
                           e->kind == Expr::K::MethodCall || e->kind == Expr::K::New;
        if (named) risks.Name(e->text, e->pos);
        // Syntactic pass, independent of the type walk: a callee or "Новый" type that is on no allowlist table.
        const std::string key = AllowlistKey(e->text);
        if ((e->kind == Expr::K::Call && !Allowed().globals.count(key)) ||
            (e->kind == Expr::K::New && !Allowed().news.count(key)) ||
            (e->kind == Expr::K::MethodCall && !Allowed().methods.count(key))) {
            risks.Add(kRiskUnknown, e->pos);
        }
        for (const ExprP& a : e->args) stack.push_back(a.get());
    }
}

void CollectBodyNames(const Body& root, RiskCollector& risks) {
    std::vector<const Body*> stack{&root};
    while (!stack.empty()) {
        const Body* b = stack.back();
        stack.pop_back();
        for (const StmtP& st : b->stmts) {
            if (!st) continue;
            if (!st->var.empty()) risks.Name(st->var, st->pos);
            for (const ExprP& e : st->exprs) CollectExprNames(e.get(), risks);
            for (const Body& blk : st->blocks) stack.push_back(&blk);
        }
    }
}

// A byte of an identifier in the key text: ASCII letters, digits, '_' and the lead bytes of Cyrillic (U+0400-047F).
// Any other non-ASCII character counts as a boundary (fail closed: it cannot hide a word).
bool IsIdentByte(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == 0xD0 ||
           c == 0xD1;
}

// Invisible characters that split a word for a reader but not for the platform.
bool IsInvisibleCodePoint(unsigned long u) {
    return u == 0x00AD || u == 0x034F || u == 0x061C || u == 0x180E || (u >= 0x200B && u <= 0x200F) ||
           (u >= 0x202A && u <= 0x202E) || (u >= 0x2060 && u <= 0x206F) || u == 0x2800 || u == 0x3164 ||
           u == 0xFE0F || u == 0xFEFF || (u >= 0xE0001 && u <= 0xE007F) ||
           (u < 0x20 && u != 10 && u != 13 && u != 9);
}

// Drops them (a blank would split the word). Handles UTF-16 units (Windows) and code points (Linux) alike.
std::wstring DropInvisible(const std::wstring& in) {
    std::wstring out;
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned long u = static_cast<unsigned long>(in[i]);
        if (u == 0xDB40 && i + 1 < in.size() && static_cast<unsigned long>(in[i + 1]) >= 0xDC00 &&
            static_cast<unsigned long>(in[i + 1]) <= 0xDC7F) {   // U+E0000..E007F as a surrogate pair
            ++i;
            continue;
        }
        if (!IsInvisibleCodePoint(u)) out.push_back(in[i]);
    }
    return out;
}

void ScanRawKey(const std::string& text, RiskCollector& risks);

// Last resort for text the tokenizer refuses (an Unknown token): every rule is searched for in the whole normalised
// text, strings and comments included, with word boundaries for Exact / Prefix. Positions are unknown (1:1).
void ScanRawText(const std::string& utf8, RiskCollector& risks) {
    const std::string base = AllowlistKey(Utf16UnitsToUtf8(DropInvisible(Utf8ToUtf16Units(utf8))));
    ScanRawKey(base, risks);
    ScanRawKey(FoldHomoglyphs(base, true), risks);
    ScanRawKey(FoldHomoglyphs(base, false), risks);
}

void ScanRawKey(const std::string& text, RiskCollector& risks) {
    for (const RiskEntry& e : RiskEntries()) {
        for (size_t p = text.find(e.key); p != std::string::npos; p = text.find(e.key, p + 1)) {
            const bool left = p == 0 || !IsIdentByte(static_cast<unsigned char>(text[p - 1]));
            const size_t end = p + e.key.size();
            const bool right = end >= text.size() || !IsIdentByte(static_cast<unsigned char>(text[end]));
            if (e.match == RiskMatch::Contains || (left && (e.match == RiskMatch::Prefix || right))) {
                risks.Add(e.bit, Pos{});
                break;
            }
        }
    }
}

std::vector<CodeReason> ScanBodyRisks(const Body& body) {
    std::vector<CodeReason> out;
    RiskCollector risks(out);
    CollectBodyNames(body, risks);
    return out;
}

CodeAnalysis Drive(const Body& body, const strict::FieldPolicy& policy, const TokenTable& tokens) {
    size_t work = 0;   // one budget for every walk below
    TypeCheckOptions base;
    base.policy = &policy;
    base.tokens = &tokens;
    base.work = &work;

    // The query texts depend on the Запрос types only, never on labels: one walk collects them.
    LabelState scratch;
    const CodeAnalysis texts = AnalyzeCodeWalk(body, base, scratch);
    std::map<const Expr*, std::vector<QueryTextUse>> sites;
    for (const QueryTextUse& use : texts.query_texts) sites[use.site].push_back(use);
    QueryChecker checker(policy, std::move(sites), &work);

    // Outer passes: the query parameters (and with them the column labels) are fixed for a pass, and every pass
    // starts from a fresh state, so labels from columns checked with not-yet-known parameters do not survive.
    // Inner rounds: the walk is repeated until the collection contents and parameter bindings are stable.
    std::map<const Expr*, Params> seeds;
    LabelState state;
    CodeAnalysis result;
    bool stable = false;
    for (int pass = 0; pass < kMaxPasses && !stable; ++pass) {
        TypeCheckOptions options = base;
        options.columns_for_query = [&checker, &seeds](const Expr* site) {
            const auto it = seeds.find(site);
            return checker.Columns(site, it == seeds.end() ? Params() : it->second);
        };
        state = LabelState();
        bool settled = false;
        for (int round = 0; round < kMaxRounds && !settled; ++round) {
            const LabelState in = state;
            result = AnalyzeCodeWalk(body, options, state);
            settled = state == in;
        }
        std::map<const Expr*, Params> bound;
        for (const auto& site : state.params) bound[site.first] = ParamsOf(state, site.first);
        stable = settled && bound == seeds;
        seeds = std::move(bound);
    }
    if (!stable) AddReason(result, "C_LIMIT", Pos{});
    if (TextExprs(result) != TextExprs(texts)) AddReason(result, "C_INTERNAL", Pos{});

    // Every text of every site, with the parameters the code binds on that site.
    for (const auto& site : checker.Sites()) {
        const Params params = ParamsOf(state, site.first);
        for (const QueryTextUse& use : site.second) {
            const strict::QueryCheckResult& r = checker.Check(use.text, params);
            for (const strict::QueryReason& reason : r.reasons) AddReason(result, reason.code, use.text_expr->pos);
            result.missing_tables.insert(r.missing_tables.begin(), r.missing_tables.end());
            if (r.verdict != "safe" && r.reasons.empty() && r.missing_tables.empty()) {
                AddReason(result, "Q_INTERNAL", use.text_expr->pos);
            }
            if (!r.canonical.empty()) result.query_canonical[use.text_expr] = r.canonical;
        }
    }
    // The walk did not complete: nothing it saw can be trusted, so the code may do anything.
    for (const CodeReason& r : result.reasons) {
        if (r.code == "C_LIMIT" || r.code == "C_INTERNAL" || r.code == "Q_INTERNAL") {
            result.risks.push_back(CodeReason{"R_UNKNOWN_EFFECT", r.pos});
        }
    }
    // R_UNKNOWN_EFFECT comes from the walk, the other risks from the names; any risk forces Confirm.
    MergeRisks(result.risks, ScanBodyRisks(body));
    result.verdict = result.reasons.empty() && result.missing_tables.empty() && result.risks.empty()
                         ? CodeVerdict::Safe
                         : CodeVerdict::Confirm;
    return result;
}

}  // namespace

bool IsTokenFormat(const std::string& s) {
    size_t end = 0;
    return TokenAt(s, 0, end) && end == s.size();
}

TokenUse ClassifyTokenLiteral(const std::string& value, const TokenTable* tokens, TokenBinding* binding) {
    const std::string upper = AsciiUpper(value);
    if (tokens) {
        const auto it = tokens->find(upper);
        if (it != tokens->end()) {
            if (binding) *binding = it->second;
            return TokenUse::Bound;
        }
    }
    if (IsTokenFormat(value)) return TokenUse::Unknown;
    for (size_t p = value.find('['); p != std::string::npos; p = value.find('[', p + 1)) {
        size_t end = 0;
        if (TokenAt(value, p, end)) return TokenUse::InString;
    }
    if (tokens) {
        for (const auto& kv : *tokens) {
            if (upper.find(kv.first) != std::string::npos) return TokenUse::InString;
        }
    }
    return TokenUse::None;
}

bool ParseTokenTable(const std::string& policy_json, TokenTable& out) {
    out.clear();
    TokenTable table;
    try {
        const Json root = Json::parse(policy_json, nullptr, false);
        if (root.is_discarded() || !root.is_object()) return false;
        const auto tokens = root.find("tokens");
        if (tokens == root.end()) return true;   // no tokens issued
        if (!tokens->is_object()) return false;
        std::set<long long> indexes;
        for (auto it = tokens->begin(); it != tokens->end(); ++it) {
            const Json& entry = it.value();
            if (!IsTokenFormat(it.key()) || !entry.is_object() || entry.size() != 2) return false;
            const auto index = entry.find("index");
            const auto origin = entry.find("origin");
            if (index == entry.end() || origin == entry.end()) return false;
            if (!index->is_number_integer() || !origin->is_string()) return false;
            const long long n = index->get<long long>();
            if (n < 0 || n > 1000000 || !indexes.insert(n).second) return false;
            const std::string o = origin->get<std::string>();
            TokenBinding b;
            b.index = static_cast<int>(n);
            if (o == "protected") {
                b.origin = Origin::ProtectedDirect;
            } else if (o == "unknown") {
                b.origin = Origin::Unknown;
            } else {
                return false;
            }
            if (!table.emplace(AsciiUpper(it.key()), b).second) return false;
        }
        out = std::move(table);
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<CodeReason> ScanRisksInSource(const std::string& utf8) {
    std::vector<CodeReason> raw;
    try {
        RiskCollector risks(raw);
        const std::vector<Token> tokens = Tokenize(utf8);
        bool refused = utf8.size() > kMaxSourceBytes;
        for (size_t i = 0; i < tokens.size(); ++i) {
            const Token& t = tokens[i];
            const Pos pos{t.pos, t.line, t.col};
            const bool after_dot = i > 0 && tokens[i - 1].kind == Bt::Dot;
            const bool after_new = i > 0 && tokens[i - 1].kind == Bt::Keyword && tokens[i - 1].kw == Bk::New;
            if (t.kind == Bt::Unknown) refused = true;
            if (t.kind == Bt::Identifier || t.kind == Bt::Keyword) risks.Name(t.text, pos, t.mixed_script);
            // Syntactic mirror of the tree pass: a callee or "Новый" type that is on no allowlist table.
            const bool name = t.kind == Bt::Identifier || (t.kind == Bt::Keyword && after_dot);
            const bool call = i + 1 < tokens.size() && tokens[i + 1].kind == Bt::LParen;
            const std::string key = AllowlistKey(t.text);
            if (name && call && !after_new &&
                !(after_dot ? Allowed().methods.count(key) : Allowed().globals.count(key))) {
                risks.Add(kRiskUnknown, pos);
            }
            if (t.kind == Bt::Keyword && t.kw == Bk::New) {
                const Token* type = i + 1 < tokens.size() ? &tokens[i + 1] : nullptr;
                if (!type || type->kind != Bt::Identifier || !Allowed().news.count(AllowlistKey(type->text))) {
                    risks.Add(kRiskUnknown, pos);   // an unknown type, or a type given as an expression
                }
            }
            // Новый("Тип"): the parser refuses a type name given as a string (C_DYNAMIC_NEW); the gate still shows it.
            if (t.kind == Bt::Keyword && t.kw == Bk::New && i + 2 < tokens.size() &&
                tokens[i + 1].kind == Bt::LParen && tokens[i + 2].kind == Bt::String) {
                risks.Name(tokens[i + 2].value, pos);
            }
        }
        if (refused) ScanRawText(utf8, risks);
    } catch (...) {
        raw.assign(1, CodeReason{"R_PROTECTION", Pos{}});   // unreadable text: show the strongest marker
    }
    std::vector<CodeReason> out;
    MergeRisks(out, raw);
    return out;
}

std::vector<CodeReason> RefusedCodeRisks(const std::string& utf8, const std::string& parse_error_code) {
    std::vector<CodeReason> out = ScanRisksInSource(utf8);
    std::vector<CodeReason> more{CodeReason{"R_UNKNOWN_EFFECT", Pos{}}};   // no tree: nothing is known about the code
    try {
        if (parse_error_code == "C_EVAL" || parse_error_code == "C_DYNAMIC_NEW") {
            more.push_back(CodeReason{"R_PROTECTION", Pos{}});
            // The text of an evaluated string or a dynamic type name is code: search every string literal, and all of
            // them joined ("За" + "пись").
            RiskCollector risks(more);
            std::string joined;
            for (const Token& t : Tokenize(utf8)) {
                if (t.kind != Bt::String) continue;
                ScanRawText(t.value, risks);
                joined += t.value;
            }
            ScanRawText(joined, risks);
        }
    } catch (...) {
        more.push_back(CodeReason{"R_PROTECTION", Pos{}});
    }
    MergeRisks(out, more);
    return out;
}

void MergeRisks(std::vector<CodeReason>& into, const std::vector<CodeReason>& more) {
    for (const CodeReason& r : more) {
        bool have = false;
        for (const CodeReason& h : into) have = have || (h.code == r.code && h.pos.offset == r.pos.offset);
        if (!have) into.push_back(r);
    }
}

CodeAnalysis AnalyzeCode(const Body& body, const strict::FieldPolicy& policy, const TokenTable& tokens) {
    try {
        return Drive(body, policy, tokens);
    } catch (...) {
        CodeAnalysis r;
        r.reasons.push_back(CodeReason{"C_INTERNAL", Pos{}});
        r.risks.push_back(CodeReason{"R_UNKNOWN_EFFECT", Pos{}});
        try {
            MergeRisks(r.risks, ScanBodyRisks(body));
        } catch (...) {
            r.risks.push_back(CodeReason{"R_PROTECTION", Pos{}});
        }
        return r;
    }
}

}  // namespace lineage::bsl
