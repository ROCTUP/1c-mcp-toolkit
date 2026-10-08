#include "parser.h"

#include <algorithm>

namespace lineage {

namespace {

std::shared_ptr<ExprNode> MakeExpr(ExprNode::Kind kind, std::string text = {}) {
    auto node = std::make_shared<ExprNode>();
    node->kind = kind;
    node->text = std::move(text);
    return node;
}

std::shared_ptr<ExprNode> MakeFieldRefFromToken(const Token& token) {
    auto node = MakeExpr(ExprNode::Kind::FieldRef);
    node->parts.push_back(token.text);
    return node;
}

bool IsSectionStart(TokenKind kind) {
    return kind == TokenKind::KeywordWhere
        || kind == TokenKind::KeywordGroup
        || kind == TokenKind::KeywordOrder
        || kind == TokenKind::KeywordHaving
        || kind == TokenKind::KeywordTotals
        || kind == TokenKind::KeywordUnion
        || kind == TokenKind::Semicolon
        || kind == TokenKind::End;
}

}  // namespace

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

QueryBatchNode Parser::ParseBatch() {
    QueryBatchNode batch;
    while (!IsAtEnd()) {
        if (Match(TokenKind::Semicolon)) continue;
        batch.statements.push_back(ParseStatement());
        Match(TokenKind::Semicolon);
    }
    return batch;
}

StatementNode Parser::ParseStatement() {
    if (Check(TokenKind::KeywordDestroy)) {
        StatementNode node;
        node.kind = StatementNode::Kind::Destroy;
        node.destroy_stmt = ParseDestroyStatement();
        return node;
    }

    StatementNode node;
    node.kind = StatementNode::Kind::Select;
    node.select_stmt = ParseSelectStatement();
    return node;
}

std::shared_ptr<DestroyStatementNode> Parser::ParseDestroyStatement() {
    Match(TokenKind::KeywordDestroy);
    auto stmt = std::make_shared<DestroyStatementNode>();
    stmt->table_name = ParseNamePath();
    if (stmt->table_name.empty() && !IsAtEnd()) Advance();
    return stmt;
}

std::shared_ptr<SelectStatementNode> Parser::ParseSelectStatement() {
    auto stmt = std::make_shared<SelectStatementNode>();
    Match(TokenKind::KeywordSelect);
    ParseSelectModifiers(*stmt);
    stmt->select_items = ParseSelectItems();
    ParseOptionalInto(*stmt);
    if (Match(TokenKind::KeywordFrom)) stmt->from_sources = ParseFromClause();
    ParseOptionalWhereGroupHavingOrderTotals();
    ParseOptionalUnion(*stmt);
    return stmt;
}

void Parser::ParseSelectModifiers(SelectStatementNode& stmt) {
    while (true) {
        if (Match(TokenKind::KeywordAllowed)) {
            stmt.select_modifiers.push_back("ALLOWED");
        } else if (Match(TokenKind::KeywordDistinct)) {
            stmt.select_modifiers.push_back("DISTINCT");
        } else if (Match(TokenKind::KeywordTop)) {
            stmt.select_modifiers.push_back("TOP");
            if (Check(TokenKind::NumberLiteral)) stmt.select_modifiers.push_back(Advance().text);
        } else {
            break;
        }
    }
}

std::vector<SelectItemNode> Parser::ParseSelectItems() {
    std::vector<SelectItemNode> items;
    while (!IsAtEnd()) {
        if (Check(TokenKind::KeywordFrom) || Check(TokenKind::KeywordInto) || IsSectionStart(Peek().kind)) break;
        items.push_back(ParseSelectItem());
        if (!Match(TokenKind::Comma)) break;
    }
    return items;
}

SelectItemNode Parser::ParseSelectItem() {
    SelectItemNode item;
    item.expr = ParseExpressionUntil({
        TokenKind::Comma, TokenKind::KeywordAs, TokenKind::KeywordFrom, TokenKind::KeywordInto,
        TokenKind::KeywordWhere, TokenKind::KeywordGroup, TokenKind::KeywordOrder,
        TokenKind::KeywordHaving, TokenKind::KeywordTotals, TokenKind::KeywordUnion,
        TokenKind::Semicolon, TokenKind::End
    });
    item.result_name = ParseOptionalAlias();
    if (item.result_name.empty()) item.result_name = GuessResultName(item.expr);
    return item;
}

std::vector<SourceNode> Parser::ParseFromClause() {
    std::vector<SourceNode> sources;
    sources.push_back(ParseSource());
    while (!IsAtEnd()) {
        if (!IsJoinStart()) break;
        while (Check(TokenKind::KeywordLeft) || Check(TokenKind::KeywordRight)
            || Check(TokenKind::KeywordInner) || Check(TokenKind::KeywordFull)) {
            Advance();
        }
        Match(TokenKind::KeywordJoin);
        sources.push_back(ParseSource());
        if (Check(TokenKind::KeywordOn) || Check(TokenKind::KeywordPo)) {
            Advance();
            ParseSkipSectionUntil({
                TokenKind::KeywordLeft, TokenKind::KeywordRight, TokenKind::KeywordInner,
                TokenKind::KeywordFull, TokenKind::KeywordJoin, TokenKind::KeywordWhere,
                TokenKind::KeywordGroup, TokenKind::KeywordOrder, TokenKind::KeywordHaving,
                TokenKind::KeywordTotals, TokenKind::KeywordUnion, TokenKind::Semicolon, TokenKind::End
            });
        }
    }
    return sources;
}

SourceNode Parser::ParseSource() {
    SourceNode source;
    if (Match(TokenKind::LParen)) {
        source.kind = SourceNode::Kind::Subquery;
        source.subquery = ParseSelectStatement();
        Match(TokenKind::RParen);
    } else {
        source.source_name = ParseNamePath();
        if (source.source_name.empty() && !IsAtEnd()) {
            Advance();
            return source;
        }
    }

    if (Match(TokenKind::KeywordAs) && (Check(TokenKind::Identifier) || IsKeyword(Peek().kind))) {
        source.alias = Advance().text;
    } else if ((Check(TokenKind::Identifier) || IsKeyword(Peek().kind))
        && !IsJoinStart() && !IsSectionStart(Peek().kind)
        && !Check(TokenKind::KeywordOn) && !Check(TokenKind::KeywordPo)) {
        source.alias = Advance().text;
    }
    return source;
}

std::shared_ptr<ExprNode> Parser::ParseExpressionUntil(const std::vector<TokenKind>& stop_tokens) {
    if (recursion_depth_++ > kMaxRecursionDepth) {
        --recursion_depth_;
        return MakeExpr(ExprNode::Kind::Composite);
    }

    auto root = MakeExpr(ExprNode::Kind::Composite);
    int paren_depth = 0;
    while (!IsAtEnd()) {
        TokenKind kind = Peek().kind;
        if (paren_depth == 0 && IsStopToken(kind, stop_tokens)) break;
        if (kind == TokenKind::LParen) ++paren_depth;
        if (kind == TokenKind::RParen) {
            if (paren_depth == 0) break;
            --paren_depth;
        }
        root->children.push_back(ParsePrimary());
    }

    --recursion_depth_;
    return root;
}

std::shared_ptr<ExprNode> Parser::ParsePrimary() {
    Token token = Advance();
    auto parse_field_ref = [&](const Token& first_token) {
        auto node = MakeFieldRefFromToken(first_token);
        while (Match(TokenKind::Dot)) {
            if (Check(TokenKind::Operator) && Peek().text == "*") {
                auto wild = MakeExpr(ExprNode::Kind::Wildcard, "*");
                wild->parts = node->parts;
                wild->parts.push_back("*");
                return wild;
            }
            if (Check(TokenKind::Identifier) || IsKeyword(Peek().kind)) {
                node->parts.push_back(Advance().text);
            } else {
                break;
            }
        }
        return std::static_pointer_cast<ExprNode>(node);
    };

    if ((token.kind == TokenKind::Identifier || IsKeyword(token.kind)) && Check(TokenKind::Dot)) {
        return parse_field_ref(token);
    }

    switch (token.kind) {
        case TokenKind::Identifier:
        case TokenKind::KeywordIsNull:
        case TokenKind::KeywordCast:
        case TokenKind::KeywordPresentation: {
            if (Check(TokenKind::LParen)) return ParseFunctionCall(token);
            return parse_field_ref(token);
        }
        case TokenKind::KeywordNull:
        case TokenKind::KeywordTrue:
        case TokenKind::KeywordFalse:
            return MakeExpr(ExprNode::Kind::Literal, token.text);
        case TokenKind::KeywordRefs: {
            auto node = MakeExpr(ExprNode::Kind::Literal, token.text);
            if (Check(TokenKind::Identifier) || IsKeyword(Peek().kind)) {
                node->text += " " + ParseNamePath();
            }
            return node;
        }
        case TokenKind::KeywordCase:
            return ParseCaseExpression();
        case TokenKind::StringLiteral:
        case TokenKind::NumberLiteral:
            return MakeExpr(ExprNode::Kind::Literal, token.text);
        case TokenKind::Parameter:
            return MakeExpr(ExprNode::Kind::Parameter, token.text);
        case TokenKind::Operator:
            if (token.text == "*") return MakeExpr(ExprNode::Kind::Wildcard, token.text);
            return MakeExpr(ExprNode::Kind::BinaryOp, token.text);
        case TokenKind::LParen: {
            auto inner = ParseExpressionUntil({TokenKind::RParen, TokenKind::End});
            Match(TokenKind::RParen);
            return inner;
        }
        default:
            return MakeExpr(ExprNode::Kind::Literal, token.text);
    }
}

std::shared_ptr<ExprNode> Parser::ParseCaseExpression() {
    auto node = MakeExpr(ExprNode::Kind::CaseExpr, "CASE");
    while (!IsAtEnd() && !Check(TokenKind::KeywordEnd)) {
        if (Match(TokenKind::KeywordWhen) || Match(TokenKind::KeywordThen) || Match(TokenKind::KeywordElse)) {
            node->children.push_back(ParseExpressionUntil({
                TokenKind::KeywordWhen, TokenKind::KeywordThen, TokenKind::KeywordElse,
                TokenKind::KeywordEnd, TokenKind::End
            }));
        } else {
            Advance();
        }
    }
    Match(TokenKind::KeywordEnd);
    return node;
}

std::shared_ptr<ExprNode> Parser::ParseFunctionCall(const Token& function_name) {
    auto node = MakeExpr(ExprNode::Kind::FunctionCall, function_name.text);
    Match(TokenKind::LParen);
    node->children = ParseArgumentList();
    Match(TokenKind::RParen);
    return node;
}

std::vector<std::shared_ptr<ExprNode>> Parser::ParseArgumentList() {
    std::vector<std::shared_ptr<ExprNode>> args;
    while (!IsAtEnd() && !Check(TokenKind::RParen)) {
        args.push_back(ParseExpressionUntil({TokenKind::Comma, TokenKind::RParen, TokenKind::End}));
        if (!Match(TokenKind::Comma)) break;
    }
    return args;
}

std::string Parser::ParseOptionalAlias() {
    if (Match(TokenKind::KeywordAs)) {
        if (Check(TokenKind::Identifier) || IsKeyword(Peek().kind)) return Advance().text;
    }
    return "";
}

std::string Parser::GuessResultName(const std::shared_ptr<ExprNode>& expr) const {
    if (!expr) return "";
    if (expr->kind == ExprNode::Kind::FieldRef && !expr->parts.empty()) {
        if (expr->parts.size() <= 2) return expr->parts.back();
        // 3+ частей: "Алиас.Ссылка.Поле" → "СсылкаПоле" (1С-соглашение об автоимени результата)
        // parts[0] — идентификатор источника из FROM (алиас или source_name)
        std::string result;
        for (size_t i = 1; i < expr->parts.size(); ++i) result += expr->parts[i];
        return result;
    }
    if (expr->kind == ExprNode::Kind::FunctionCall && !expr->text.empty()) return expr->text;
    if (expr->kind == ExprNode::Kind::Composite && expr->children.size() == 1) return GuessResultName(expr->children.front());
    return "";
}

void Parser::ParseOptionalInto(SelectStatementNode& stmt) {
    if (Match(TokenKind::KeywordInto)) stmt.into_temp_table = ParseNamePath();
}

void Parser::ParseOptionalWhereGroupHavingOrderTotals() {
    while (!IsAtEnd()) {
        if (Match(TokenKind::KeywordWhere)) {
            ParseSkipSectionUntil({TokenKind::KeywordGroup, TokenKind::KeywordOrder, TokenKind::KeywordHaving, TokenKind::KeywordTotals, TokenKind::KeywordUnion, TokenKind::Semicolon, TokenKind::End});
        } else if (Match(TokenKind::KeywordGroup)) {
            if (Check(TokenKind::KeywordBy) || Check(TokenKind::KeywordPo)) Advance();
            ParseSkipSectionUntil({TokenKind::KeywordHaving, TokenKind::KeywordOrder, TokenKind::KeywordTotals, TokenKind::KeywordUnion, TokenKind::Semicolon, TokenKind::End});
        } else if (Match(TokenKind::KeywordHaving)) {
            ParseSkipSectionUntil({TokenKind::KeywordOrder, TokenKind::KeywordTotals, TokenKind::KeywordUnion, TokenKind::Semicolon, TokenKind::End});
        } else if (Match(TokenKind::KeywordOrder)) {
            if (Check(TokenKind::KeywordBy) || Check(TokenKind::KeywordPo)) Advance();
            ParseSkipSectionUntil({TokenKind::KeywordTotals, TokenKind::KeywordUnion, TokenKind::Semicolon, TokenKind::End});
        } else if (Match(TokenKind::KeywordTotals)) {
            ParseSkipSectionUntil({TokenKind::KeywordUnion, TokenKind::Semicolon, TokenKind::End});
        } else {
            break;
        }
    }
}

void Parser::ParseSkipSectionUntil(const std::vector<TokenKind>& stop_tokens) {
    int depth = 0;
    while (!IsAtEnd()) {
        if (Check(TokenKind::LParen)) {
            ++depth;
            Advance();
            continue;
        }
        if (Check(TokenKind::RParen)) {
            if (depth == 0) break;
            --depth;
            Advance();
            continue;
        }
        if (depth == 0 && IsStopToken(Peek().kind, stop_tokens)) break;
        Advance();
    }
}

void Parser::ParseOptionalUnion(SelectStatementNode& stmt) {
    while (Match(TokenKind::KeywordUnion)) {
        Match(TokenKind::KeywordAll);
        auto union_part = ParseSelectStatement();
        stmt.union_parts.push_back(*union_part);
    }
}

bool Parser::IsJoinStart() const {
    return Check(TokenKind::KeywordJoin)
        || Check(TokenKind::KeywordLeft)
        || Check(TokenKind::KeywordRight)
        || Check(TokenKind::KeywordInner)
        || Check(TokenKind::KeywordFull);
}

bool Parser::IsStopToken(TokenKind kind, const std::vector<TokenKind>& stop_tokens) const {
    return std::find(stop_tokens.begin(), stop_tokens.end(), kind) != stop_tokens.end();
}

bool Parser::Match(TokenKind kind) {
    if (!Check(kind)) return false;
    Advance();
    return true;
}

bool Parser::Check(TokenKind kind) const {
    if (IsAtEnd()) return kind == TokenKind::End;
    return Peek().kind == kind;
}

const Token& Parser::Advance() {
    if (!IsAtEnd()) ++current_;
    return tokens_[current_ - 1];
}

const Token& Parser::Peek(int offset) const {
    size_t index = current_ + static_cast<size_t>(std::max(offset, 0));
    if (index >= tokens_.size()) return tokens_.back();
    return tokens_[index];
}

bool Parser::IsAtEnd() const {
    return Peek().kind == TokenKind::End;
}

std::string Parser::ParseNamePath() {
    std::string result;
    if (!(Check(TokenKind::Identifier) || IsKeyword(Peek().kind))) return result;
    result = Advance().text;
    while (Match(TokenKind::Dot)) {
        if (!(Check(TokenKind::Identifier) || IsKeyword(Peek().kind))) break;
        result += "." + Advance().text;
    }
    return result;
}

QueryBatchNode ParseQueryBatch(const std::string& query_text) {
    Parser parser(Tokenize(query_text));
    return parser.ParseBatch();
}

}  // namespace lineage
