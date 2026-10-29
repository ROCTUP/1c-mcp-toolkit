#pragma once
#include <ostream>
#include <string>

namespace lineage::strict {

enum class Origin { Open, Client, ProtectedDirect, ProtectedDerived, Mixed, Unknown };

// Named JoinOrigin: lineage::strict::Join is the AST join clause.
// Partial order: Unknown is top. Mixed = protected and non-protected may coexist in one column.
inline Origin JoinOrigin(Origin a, Origin b) {
    if (a == b) return a;
    if (a == Origin::Unknown || b == Origin::Unknown) return Origin::Unknown;
    auto prot = [](Origin o) { return o == Origin::ProtectedDirect || o == Origin::ProtectedDerived; };
    if (prot(a) && prot(b)) return Origin::ProtectedDerived;
    if (prot(a) || prot(b) || a == Origin::Mixed || b == Origin::Mixed) return Origin::Mixed;
    return Origin::Client;   // {Open, Client}: no protected content; client values are returned as is
}

inline Origin Transform(Origin a) { return a == Origin::ProtectedDirect ? Origin::ProtectedDerived : a; }

const char* OriginName(Origin o);   // "open" "client" "protected" "protected_derived" "mixed" "unknown"

// How an expression call is labelled (the allowlist of callable functions lives here, not in the parser).
// Count: always an open number. Sum: SUM / AVG, a computation over its argument. MinMax: MIN / MAX.
// Presentation: ПРЕДСТАВЛЕНИЕ / ПРЕДСТАВЛЕНИЕССЫЛКИ. TypeOf: ТИПЗНАЧЕНИЯ. Plain: an allowlisted scalar function,
// labelled as a transformation of all its operands. Unrecognized: any other name, labelled Unknown.
enum class CallClass { Count, Sum, MinMax, Presentation, TypeOf, Plain, Unrecognized };

// `name` is the call's text from the AST (any case, Russian or English spelling).
CallClass ClassifyCall(const std::string& name);

// Index of the period argument of a date function (НАЧАЛОПЕРИОДА, КОНЕЦПЕРИОДА, ДОБАВИТЬКДАТЕ, РАЗНОСТЬДАТ and
// English spellings), or -1 for any other call. A bare period name (ГОД, КВАРТАЛ, ДЕНЬ, ...) there is a constant.
int PeriodArgIndex(const std::string& call_name);
bool IsPeriodName(const std::string& identifier);

inline std::ostream& operator<<(std::ostream& os, Origin o) { return os << OriginName(o); }

}  // namespace lineage::strict
