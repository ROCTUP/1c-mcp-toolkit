#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bsl/bsl_types.h"
#include "utf_utils.h"

// The Результат description of the strict BSL checker. It follows БезопасноеЗначениеДляПередачи: the
// path syntax and the label set are documented in bsl/check_code.h.

namespace lineage::bsl {

bool IsPathKey(const std::string& key) {
    if (key.empty() || key.size() > 255) return false;
    for (size_t i = 0; i < key.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(key[i]);
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
        const bool digit = c >= '0' && c <= '9';
        if (!(letter || (digit && i > 0))) return false;
    }
    return true;
}

namespace {

constexpr int kMaxSerializerDepth = 5;     // БезопасноеЗначениеДляПередачи: a value deeper than this is "<max_depth>"
constexpr size_t kMaxNodes = 20000;        // description budget: a fan-out of aliased collections is C_UNLABELED_RESULT

const char* LabelName(Origin o) { return strict::OriginName(o); }

struct Field;

// A node of the description tree. The vectors elem / key hold at most one element (a value member of an incomplete
// type, without a pointer).
struct Node {
    enum class K { None, Scalar, Ref, Array, Object, Map, Table, Opaque };
    K k = K::None;
    Origin label = Origin::Open;                     // Scalar
    std::optional<Origin> shape;                     // containers: what the size / key set depends on (tainted only)
    std::vector<Field> fields;                       // Object / Table
    std::vector<Node> elem;                          // Array element / Map value (0 or 1)
    std::vector<Node> key;                           // Map key (0 or 1)
};

struct Field {
    std::string key;    // AllowlistKey of the name: two spellings that differ in case or in ё / е are one field
    std::string name;   // first spelling, written into the path
    Node node;
};

Node Opaque() {
    Node n;
    n.k = Node::K::Opaque;
    return n;
}

Node Scalar(Origin label) {
    Node n;
    n.k = Node::K::Scalar;
    n.label = label;
    return n;
}

void JoinShape(std::optional<Origin>& into, const std::optional<Origin>& from) {
    if (from) into = into ? JoinOrigin(*into, *from) : *from;
}

void Merge(Node& a, const Node& b);

void MergeChild(std::vector<Node>& into, const std::vector<Node>& from) {
    if (from.empty()) return;
    if (into.empty()) {
        into = from;
        return;
    }
    Merge(into[0], from[0]);
}

void Merge(Node& a, const Node& b) {
    using K = Node::K;
    if (b.k == K::None) return;
    if (a.k == K::None) {
        a = b;
        return;
    }
    if (a.k != b.k) {
        a = Opaque();
        return;
    }
    JoinShape(a.shape, b.shape);
    switch (a.k) {
        case K::Scalar: a.label = JoinOrigin(a.label, b.label); break;
        case K::Array:
            MergeChild(a.elem, b.elem);
            break;
        case K::Map:
            MergeChild(a.elem, b.elem);
            MergeChild(a.key, b.key);
            break;
        case K::Object:
        case K::Table:
            for (const Field& f : b.fields) {
                bool found = false;
                for (Field& g : a.fields) {
                    if (g.key != f.key) continue;
                    Merge(g.node, f.node);
                    if (NormalizeIdentifierKey(g.name) != NormalizeIdentifierKey(f.name)) g.node = Opaque();
                    found = true;
                    break;
                }
                if (!found) a.fields.push_back(f);
            }
            break;
        default: break;
    }
}

class Describer {
public:
    explicit Describer(const LabelState& state) : state_(state) {}

    Node Value(const ResultValue& v) {
        using K = Node::K;
        const bool tainted = IsTainted(v.label);
        switch (v.ty) {
            case Ty::Undefined:
            case Ty::Null:
            case Ty::Boolean:
            case Ty::Number:
            case Ty::String:
            case Ty::Date:
            case Ty::Uuid: return Scalar(v.label);
            case Ty::Ref: return tainted ? Scalar(v.label) : Leaf(K::Ref);
            case Ty::ValueTable: {
                if (!v.columns) return Opaque();
                Node n;
                n.k = K::Table;
                if (tainted) n.shape = v.label;
                for (const auto& c : v.columns->columns) {
                    if (!IsPathKey(c.second.name)) return Opaque();
                    n.fields.push_back(Field{c.first, c.second.name,
                                             c.second.name_collision ? Opaque() : Cell(c.second, 1)});
                }
                return n;
            }
            case Ty::Array:
            case Ty::Structure:
            case Ty::Map: {
                if (v.objs.empty()) return Opaque();
                Node n = Sites(v.objs, 0);
                if (tainted) JoinShape(n.shape, v.label);
                return n;
            }
            case Ty::Unknown:
                if (v.objs.empty()) return v.maybe_ref && !tainted ? Opaque() : Scalar(v.label);
                if (tainted) return Scalar(v.label);   // the whole value is replaced by a token
                return v.maybe_ref ? Opaque() : Sites(v.objs, 0);
            default: return Opaque();
        }
    }

    bool Over() const { return nodes_ > kMaxNodes; }

private:
    const LabelState& state_;
    size_t nodes_ = 0;

    static Node Leaf(Node::K k) {
        Node n;
        n.k = k;
        return n;
    }

    // A table cell at serializer depth `depth`.
    Node Cell(const ColumnInfo& c, int depth) {
        if (depth > kMaxSerializerDepth) return Opaque();   // "<max_depth>": fail closed, never open
        if (!c.refs.empty() && !IsTainted(c.origin)) return Leaf(Node::K::Ref);
        return Scalar(c.origin);
    }

    // The union of the collections `objs` as a value at serializer depth `depth`.
    Node Sites(const std::set<const Expr*>& objs, int depth) {
        if (depth > kMaxSerializerDepth) return Opaque();   // "<max_depth>": fail closed, never open
        Node merged;
        for (const Expr* site : objs) Merge(merged, Site(site, depth));
        return merged;
    }

    Node Site(const Expr* site, int depth) {
        using K = Node::K;
        if (++nodes_ > kMaxNodes) return Opaque();
        const auto it = state_.objects.find(site);
        if (it == state_.objects.end()) return Opaque();
        const ObjectLabels& ol = it->second;
        Node n;
        if (ol.shape && IsTainted(*ol.shape)) n.shape = ol.shape;
        switch (ol.kind) {
            case Ty::Array:
                n.k = K::Array;
                Child(n.elem, ol.elem, depth + 1);
                return n;
            case Ty::Structure:
                n.k = K::Object;
                if (!ol.elem.Empty()) return Opaque();   // a value under a key the description cannot name
                for (const auto& f : ol.fields) {
                    if (!IsPathKey(f.second.name)) return Opaque();
                    n.fields.push_back(Field{f.first, f.second.name,
                                             f.second.collide ? Opaque() : Slot(f.second.slot, depth + 1)});
                }
                return n;
            case Ty::Map:
                n.k = K::Map;
                Child(n.elem, ol.elem, depth + 1);
                Child(n.key, ol.key_slot, depth + 1);
                if (ol.keys && IsTainted(*ol.keys)) JoinShape(n.shape, ol.keys);
                return n;
            default: return Opaque();
        }
    }

    void Child(std::vector<Node>& out, const SlotLabels& slot, int depth) {
        Node n = Slot(slot, depth);
        if (n.k != Node::K::None) out.push_back(std::move(n));
    }

    // Everything ever stored in one position, as a value at serializer depth `depth`.
    Node Slot(const SlotLabels& slot, int depth) {
        using K = Node::K;
        if (++nodes_ > kMaxNodes) return Opaque();
        if (slot.Empty()) return Node();
        if (depth > kMaxSerializerDepth) return Opaque();   // "<max_depth>": fail closed, never open
        Node n;
        if (slot.opaque) return Opaque();
        if (slot.scalar) Merge(n, Scalar(*slot.scalar));
        if (slot.ref) Merge(n, Leaf(K::Ref));
        if (!slot.nested.empty()) Merge(n, Sites(slot.nested, depth));
        return n;
    }
};

class Emitter {
public:
    explicit Emitter(CodeAnalysis& out) : out_(out) {}

    void Run(const Node& n, const std::string& path) {
        using K = Node::K;
        switch (n.k) {
            case K::None: return;
            case K::Scalar: Leaf(path, n.label); return;
            case K::Ref: Put(path, "ref"); return;
            case K::Opaque: Put(path, "unknown"); Reason("C_UNLABELED_RESULT"); return;
            case K::Array:
                Container(path, n);
                if (!n.elem.empty()) Run(n.elem[0], path + "[*]");
                return;
            case K::Object:
                Container(path, n);
                for (const Field& f : n.fields) Run(f.node, path + "." + f.name);
                return;
            case K::Map:
                Container(path, n);
                Put(path + "[*]", "container");
                if (!n.key.empty()) Run(n.key[0], path + "[*].key");
                if (!n.elem.empty()) Run(n.elem[0], path + "[*].value");
                return;
            case K::Table:
                Container(path, n);
                Put(path + "[*]", "container");
                for (const Field& f : n.fields) Run(f.node, path + "[*]." + f.name);
                return;
        }
    }

private:
    CodeAnalysis& out_;
    std::set<std::string> reasons_;

    void Put(const std::string& path, const char* label) {
        for (ResultPath& p : out_.result_labels) {
            if (p.path != path) continue;
            if (p.label != label) {   // two nodes on one path: cannot happen with identifier keys; fail closed
                p.label = "unknown";
                Reason("C_UNLABELED_RESULT");
            }
            return;
        }
        out_.result_labels.push_back(ResultPath{path, label});
    }

    void Reason(const char* code) {
        if (reasons_.insert(code).second) out_.result_reasons.push_back(CodeReason{code, Pos{}});
    }

    void Container(const std::string& path, const Node& n) {
        Put(path, "container");
        if (n.shape && IsTainted(*n.shape)) Reason("C_DERIVED_RESULT");   // the size or the keys depend on tainted data
    }

    void Leaf(const std::string& path, Origin label) {
        Put(path, LabelName(label));
        switch (label) {
            case Origin::Mixed: Reason("C_MIXED_RESULT"); break;
            case Origin::ProtectedDerived: Reason("C_DERIVED_RESULT"); break;
            case Origin::Unknown: Reason("C_UNLABELED_RESULT"); break;
            default: break;
        }
    }
};

}  // namespace

void DescribeResult(const ResultValue* value, const LabelState& state, CodeAnalysis& out) {
    out.result_labels.clear();
    out.result_reasons.clear();
    try {
        Node root;
        if (!value) {
            root = Scalar(Origin::Open);   // never assigned: the host's initial Неопределено
        } else {
            Describer describer(state);
            root = describer.Value(*value);
            if (describer.Over()) root = Opaque();
        }
        Emitter(out).Run(root, "$");
    } catch (...) {
        out.result_labels.clear();
        out.result_reasons.clear();
        out.result_labels.push_back(ResultPath{"$", "unknown"});
        out.result_reasons.push_back(CodeReason{"C_UNLABELED_RESULT", Pos{}});
    }
}

}  // namespace lineage::bsl
