#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lineage {

using LineageSet = std::unordered_set<std::string>;

struct ExprNode {
    enum class Kind {
        FieldRef,
        FunctionCall,
        CaseExpr,
        Literal,
        Parameter,
        BinaryOp,
        Aggregate,
        Wildcard,
        Composite
    };

    Kind kind = Kind::Composite;
    std::vector<std::string> parts;
    std::string text;
    std::vector<std::shared_ptr<ExprNode>> children;
};

struct SelectItemNode {
    std::string result_name;
    std::shared_ptr<ExprNode> expr;
};

struct SourceNode;

struct SelectStatementNode {
    std::vector<std::string> select_modifiers;
    std::vector<SelectItemNode> select_items;
    std::vector<SourceNode> from_sources;
    std::string into_temp_table;
    std::vector<SelectStatementNode> union_parts;
};

struct DestroyStatementNode {
    std::string table_name;
};

struct SourceNode {
    enum class Kind {
        Metadata,
        Subquery,
        TempTable
    };

    Kind kind = Kind::Metadata;
    std::string source_name;
    std::string alias;
    std::shared_ptr<SelectStatementNode> subquery;
};

struct StatementNode {
    enum class Kind {
        Select,
        Destroy
    };

    Kind kind = Kind::Select;
    std::shared_ptr<SelectStatementNode> select_stmt;
    std::shared_ptr<DestroyStatementNode> destroy_stmt;
};

struct QueryBatchNode {
    std::vector<StatementNode> statements;
};

// Table and output-column keys use NormalizeIdentifierKey on insertion and lookup.
// Values (source paths) and AST names retain their original spelling.
using TempTableMap = std::unordered_map<std::string, std::unordered_map<std::string, LineageSet>>;
using OutputLineageMap = std::map<std::string, LineageSet>;

}  // namespace lineage
