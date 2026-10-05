// cypher.cpp - lexer, parser and executor for the supported Cypher subset.
//
// Scope: CRUD, and nothing else. The dialect must be *standard* Cypher for the
// constructs it accepts, because the user of this database is a language model
// whose prior is standard Cypher -- every avoidable deviation is a query that
// fails on the first try. What is not implemented is rejected with a message
// that says what to write instead.
//
//   MATCH / OPTIONAL-free patterns, WHERE
//   CREATE, MERGE, SET (=, +=, .key), DELETE (and DETACH DELETE)
//   RETURN [DISTINCT] ... [ORDER BY ...] [SKIP n] [LIMIT n]
//   = <> < <= > >=, AND OR NOT, IS [NOT] NULL, IN [...], CONTAINS / STARTS WITH / ENDS WITH
//   + - * / % on numbers, + as string concatenation
//   id(), labels(), type(), count(*), count(x)
//   $parameters, property maps

#include "internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace gdb {
namespace {

// ---------------------------------------------------------------------------
// tokens
// ---------------------------------------------------------------------------

enum class T {
    End, Ident, Integer, Real, String, Param,
    LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Comma, Colon, Dot, Semicolon, Star, Pipe,
    Minus, Arrow, LtArrow, Plus, Slash, Percent,
    Eq, Neq, Lt, Le, Gt, Ge
};

struct Token {
    T kind = T::End;
    std::string text;
    std::int64_t ival = 0;
    double rval = 0.0;
    std::size_t pos = 0;
};

std::string toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Cypher allows Unicode labels, and a memory store written in Chinese will use
// them. Bytes at or above 0x80 can never be part of ASCII syntax, so treating
// them as identifier characters makes any UTF-8 label work without dragging in
// a Unicode property table.
bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}
bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
}

class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        for (;;) {
            Token t = next();
            const bool end = t.kind == T::End;
            out.push_back(std::move(t));
            if (end) break;
        }
        return out;
    }

private:
    const std::string& src_;
    std::size_t i_ = 0;

    [[noreturn]] void fail(const std::string& why) const {
        throw Error("Cypher syntax error at offset " + std::to_string(i_) + ": " + why);
    }

    char peek(std::size_t ahead = 0) const {
        const std::size_t at = i_ + ahead;
        return at < src_.size() ? src_[at] : '\0';
    }

    void skipSpace() {
        for (;;) {
            while (i_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[i_]))) ++i_;
            if (peek() == '/' && peek(1) == '/') {
                while (i_ < src_.size() && src_[i_] != '\n') ++i_;
                continue;
            }
            if (peek() == '/' && peek(1) == '*') {
                i_ += 2;
                while (i_ < src_.size() && !(peek() == '*' && peek(1) == '/')) ++i_;
                if (i_ >= src_.size()) fail("unterminated block comment");
                i_ += 2;
                continue;
            }
            break;
        }
    }

    Token make(T kind, std::string text, std::size_t pos) const {
        Token t;
        t.kind = kind;
        t.text = std::move(text);
        t.pos = pos;
        return t;
    }

    Token next() {
        skipSpace();
        if (i_ >= src_.size()) return make(T::End, "", i_);
        const std::size_t start = i_;
        const char c = src_[i_];

        if (isIdentStart(c)) {
            while (i_ < src_.size() && isIdentChar(src_[i_])) ++i_;
            return make(T::Ident, src_.substr(start, i_ - start), start);
        }
        if (c == '`') {
            ++i_;
            const std::size_t from = i_;
            while (i_ < src_.size() && src_[i_] != '`') ++i_;
            if (i_ >= src_.size()) fail("unterminated backtick identifier");
            std::string name = src_.substr(from, i_ - from);
            ++i_;
            return make(T::Ident, std::move(name), start);
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            while (i_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[i_]))) ++i_;
            bool real = false;
            if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
                real = true;
                ++i_;
                while (i_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[i_]))) ++i_;
            }
            if (peek() == 'e' || peek() == 'E') {
                std::size_t j = i_ + 1;
                if (j < src_.size() && (src_[j] == '+' || src_[j] == '-')) ++j;
                if (j < src_.size() && std::isdigit(static_cast<unsigned char>(src_[j]))) {
                    real = true;
                    i_ = j;
                    while (i_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[i_]))) ++i_;
                }
            }
            const std::string text = src_.substr(start, i_ - start);
            Token t = make(real ? T::Real : T::Integer, text, start);
            if (real) t.rval = std::stod(text);
            else t.ival = std::stoll(text);
            return t;
        }
        if (c == '\'' || c == '"') {
            const char quote = c;
            ++i_;
            std::string value;
            while (i_ < src_.size() && src_[i_] != quote) {
                if (src_[i_] == '\\' && i_ + 1 < src_.size()) {
                    ++i_;
                    switch (src_[i_]) {
                    case 'n': value.push_back('\n'); break;
                    case 't': value.push_back('\t'); break;
                    case 'r': value.push_back('\r'); break;
                    default: value.push_back(src_[i_]); break;
                    }
                } else {
                    value.push_back(src_[i_]);
                }
                ++i_;
            }
            if (i_ >= src_.size()) fail("unterminated string literal");
            ++i_;
            return make(T::String, std::move(value), start);
        }
        if (c == '$') {
            ++i_;
            const std::size_t from = i_;
            while (i_ < src_.size() && isIdentChar(src_[i_])) ++i_;
            if (i_ == from) fail("expected a parameter name after '$'");
            return make(T::Param, src_.substr(from, i_ - from), start);
        }

        ++i_;
        switch (c) {
        case '(': return make(T::LParen, "(", start);
        case ')': return make(T::RParen, ")", start);
        case '[': return make(T::LBracket, "[", start);
        case ']': return make(T::RBracket, "]", start);
        case '{': return make(T::LBrace, "{", start);
        case '}': return make(T::RBrace, "}", start);
        case ',': return make(T::Comma, ",", start);
        case ':': return make(T::Colon, ":", start);
        case '.': return make(T::Dot, ".", start);
        case ';': return make(T::Semicolon, ";", start);
        case '*': return make(T::Star, "*", start);
        case '|': return make(T::Pipe, "|", start);
        case '+': return make(T::Plus, "+", start);
        case '/': return make(T::Slash, "/", start);
        case '%': return make(T::Percent, "%", start);
        case '-':
            if (peek() == '>') { ++i_; return make(T::Arrow, "->", start); }
            return make(T::Minus, "-", start);
        case '<':
            if (peek() == '-') { ++i_; return make(T::LtArrow, "<-", start); }
            if (peek() == '=') { ++i_; return make(T::Le, "<=", start); }
            if (peek() == '>') { ++i_; return make(T::Neq, "<>", start); }
            return make(T::Lt, "<", start);
        case '>':
            if (peek() == '=') { ++i_; return make(T::Ge, ">=", start); }
            return make(T::Gt, ">", start);
        case '=': return make(T::Eq, "=", start);
        case '!':
            if (peek() == '=') { ++i_; return make(T::Neq, "!=", start); }
            fail("unexpected '!'");
        default:
            fail(std::string("unexpected character '") + c + "'");
        }
    }
};

// ---------------------------------------------------------------------------
// AST
// ---------------------------------------------------------------------------

struct Expr;
using ExprP = std::unique_ptr<Expr>;

struct MapItem {
    std::string key;
    ExprP value;
};

struct Expr {
    enum class K { Literal, Var, Prop, Param, Unary, Binary, Count, Star, List, Map } k = K::Literal;
    Value lit;
    std::string name;  // variable, property key or parameter name
    std::string op;    // binary/unary operator, or the function name for count
    ExprP a, b;
    std::vector<ExprP> args;   // list elements
    std::vector<MapItem> entries;  // map entries
    bool star = false;         // for count(*) and RETURN *
};

struct NodePat {
    std::string var;
    std::string label;
    bool has_label = false;
    std::vector<MapItem> props;
};

struct RelPat {
    std::string var;
    std::string type;
    bool has_type = false;
    std::vector<MapItem> props;
    int dir = 0;  // 1 = left to right, -1 = right to left, 0 = either
};

struct PatElem {
    bool is_node = true;
    NodePat node;
    RelPat rel;
};

using Pattern = std::vector<PatElem>;

struct ReturnItem {
    ExprP expr;
    std::string label;
    bool star = false;
};

enum class SetMode { Assign, Replace, Merge };

struct SetItem {
    std::string var;
    std::string key;
    ExprP value;
    SetMode mode = SetMode::Assign;
};

struct OrderItem {
    ExprP expr;
    std::string label;
    bool desc = false;
};

struct Clause {
    enum class K { Match, Create, Merge, Where, Set, Delete, Return } k = K::Match;
    std::vector<Pattern> patterns;
    ExprP expr;
    std::vector<SetItem> sets;
    std::vector<std::string> vars;
    std::vector<ReturnItem> items;
    std::vector<OrderItem> order;
    bool distinct = false;
    std::int64_t skip = 0;
    bool has_skip = false;
    std::int64_t limit = 0;
    bool has_limit = false;
};

// ---------------------------------------------------------------------------
// parser
// ---------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(const std::string& source) : tokens_(Lexer(source).run()) {}

    std::vector<Clause> parseProgram() {
        std::vector<Clause> clauses;
        while (!at(T::End) && !at(T::Semicolon)) {
            clauses.push_back(parseClause());
            match(T::Semicolon);
        }
        if (clauses.empty()) throw Error("empty Cypher statement");
        return clauses;
    }

private:
    std::vector<Token> tokens_;
    std::size_t p_ = 0;

    const Token& tok(std::size_t ahead = 0) const {
        const std::size_t at = p_ + ahead;
        return tokens_[at < tokens_.size() ? at : tokens_.size() - 1];
    }
    bool at(T kind, std::size_t ahead = 0) const { return tok(ahead).kind == kind; }
    bool atKeyword(const char* word, std::size_t ahead = 0) const {
        return tok(ahead).kind == T::Ident && toUpper(tok(ahead).text) == word;
    }
    bool match(T kind) {
        if (!at(kind)) return false;
        ++p_;
        return true;
    }
    bool matchKeyword(const char* word) {
        if (!atKeyword(word)) return false;
        ++p_;
        return true;
    }
    void expect(T kind, const char* what) {
        if (!match(kind)) fail(std::string("expected ") + what);
    }
    [[noreturn]] void fail(const std::string& why) const {
        throw Error("Cypher syntax error at offset " + std::to_string(tok().pos) + ": " + why +
                    " (found '" + (at(T::End) ? std::string("<end>") : tok().text) + "')");
    }

    std::string textBetween(std::size_t from, std::size_t to) const {
        std::string out;
        std::size_t previous_end = 0;
        bool have_previous = false;
        for (std::size_t i = from; i < to && i < tokens_.size(); ++i) {
            const Token& t = tokens_[i];
            if (t.kind == T::End) break;
            const bool wordish = t.kind == T::Ident || t.kind == T::Integer ||
                                 t.kind == T::Real || t.kind == T::String || t.kind == T::Param;
            if (have_previous && wordish && t.pos == previous_end) out.push_back(' ');
            out.append(t.text);
            previous_end = t.pos + t.text.size();
            have_previous = true;
        }
        return out.empty() ? std::string("expr") : out;
    }

    // --- clauses -----------------------------------------------------------
    Clause parseClause() {
        if (atKeyword("MATCH")) return parseMatch();
        if (atKeyword("CREATE")) return parseCreate();
        if (atKeyword("MERGE")) return parseMerge();
        if (atKeyword("WHERE")) return parseWhere();
        if (atKeyword("SET")) return parseSet();
        if (atKeyword("DELETE")) return parseDelete();
        if (atKeyword("DETACH")) {
            // Standard Cypher spells the cascading form DETACH DELETE. Incident
            // edges are always removed here, so both spellings do the same thing.
            ++p_;
            if (!atKeyword("DELETE")) fail("expected DELETE after DETACH");
            return parseDelete();
        }
        if (atKeyword("RETURN")) return parseReturn();
        if (atKeyword("OPTIONAL")) {
            fail("OPTIONAL MATCH is not supported. Query the missing case with its own WHERE, "
                 "for example MATCH (n:Label) WHERE NOT (n)-[:R]->() RETURN n");
        }
        if (atKeyword("WITH")) fail("WITH is not supported. Send two queries instead.");
        if (atKeyword("UNWIND")) {
            fail("UNWIND is not supported. Write one clause per item, or run one query per row.");
        }
        if (atKeyword("CALL")) fail("CALL is not supported.");
        fail("expected a clause keyword (MATCH, CREATE, MERGE, WHERE, SET, DELETE, RETURN)");
    }

    Clause parseMatch() {
        matchKeyword("MATCH");
        Clause c;
        c.k = Clause::K::Match;
        c.patterns.push_back(parsePattern());
        while (match(T::Comma)) c.patterns.push_back(parsePattern());
        return c;
    }

    Clause parseCreate() {
        matchKeyword("CREATE");
        Clause c;
        c.k = Clause::K::Create;
        c.patterns.push_back(parsePattern());
        while (match(T::Comma)) c.patterns.push_back(parsePattern());
        return c;
    }

    Clause parseMerge() {
        matchKeyword("MERGE");
        Clause c;
        c.k = Clause::K::Merge;
        c.patterns.push_back(parsePattern());
        while (match(T::Comma)) c.patterns.push_back(parsePattern());
        if (atKeyword("ON")) {
            fail("MERGE ... ON CREATE SET / ON MATCH SET is not supported. Follow MERGE with a "
                 "plain SET, or use MATCH when the node is expected to exist.");
        }
        return c;
    }

    Clause parseWhere() {
        matchKeyword("WHERE");
        Clause c;
        c.k = Clause::K::Where;
        c.expr = parseExpr();
        return c;
    }

    Clause parseSet() {
        matchKeyword("SET");
        Clause c;
        c.k = Clause::K::Set;
        for (;;) {
            SetItem item;
            if (!at(T::Ident)) fail("expected a variable after SET");
            item.var = tok().text;
            ++p_;
            if (match(T::Dot)) {
                if (!at(T::Ident)) fail("expected a property name after '.'");
                item.key = tok().text;
                ++p_;
                expect(T::Eq, "'=' after the property name");
                item.mode = SetMode::Assign;
                item.value = parseExpr();
            } else if (match(T::Plus)) {
                expect(T::Eq, "'=' after '+='");
                item.mode = SetMode::Merge;
                item.value = parseMapExpression();
            } else if (match(T::Eq)) {
                item.mode = SetMode::Replace;
                item.value = parseMapExpression();
            } else {
                fail("expected '.', '=' or '+=' after the variable");
            }
            c.sets.push_back(std::move(item));
            if (!match(T::Comma)) break;
        }
        return c;
    }

    Clause parseDelete() {
        matchKeyword("DELETE");
        Clause c;
        c.k = Clause::K::Delete;
        for (;;) {
            if (!at(T::Ident)) fail("expected a variable after DELETE");
            c.vars.push_back(tok().text);
            ++p_;
            if (!match(T::Comma)) break;
        }
        return c;
    }

    Clause parseReturn() {
        matchKeyword("RETURN");
        Clause c;
        c.k = Clause::K::Return;
        if (matchKeyword("DISTINCT")) c.distinct = true;
        if (match(T::Star)) {
            ReturnItem star;
            star.star = true;
            star.label = "*";
            c.items.push_back(std::move(star));
        } else {
            for (;;) {
                const std::size_t from = p_;
                ReturnItem item;
                item.expr = parseExpr();
                item.label = textBetween(from, p_);
                if (matchKeyword("AS")) {
                    if (!at(T::Ident)) fail("expected an alias after AS");
                    item.label = tok().text;
                    ++p_;
                }
                c.items.push_back(std::move(item));
                if (!match(T::Comma)) break;
            }
        }
        if (matchKeyword("ORDER")) {
            if (!matchKeyword("BY")) fail("expected BY after ORDER");
            for (;;) {
                OrderItem item;
                const std::size_t from = p_;
                item.expr = parseExpr();
                item.label = textBetween(from, p_);
                if (matchKeyword("DESC") || matchKeyword("DESCENDING")) item.desc = true;
                else if (matchKeyword("ASC") || matchKeyword("ASCENDING")) item.desc = false;
                c.order.push_back(std::move(item));
                if (!match(T::Comma)) break;
            }
        }
        if (matchKeyword("SKIP")) {
            if (!at(T::Integer)) fail("expected an integer after SKIP");
            c.skip = tok().ival;
            c.has_skip = true;
            ++p_;
        }
        if (matchKeyword("LIMIT")) {
            if (!at(T::Integer)) fail("expected an integer after LIMIT");
            c.limit = tok().ival;
            c.has_limit = true;
            ++p_;
        }
        // Paging is a table concept: it assumes a stable total order. Without an
        // explicit ORDER BY the row order here is an implementation detail (the
        // adjacency lists are insertion ordered and change as edges are added),
        // so pages would silently overlap and drop rows. LIMIT alone is fine --
        // it is a budget, not a slice, and claims nothing about order.
        if (c.has_skip && c.order.empty()) {
            fail("SKIP needs an ORDER BY. Without one the row order is not defined, so pages "
                 "would overlap or drop rows. Add ORDER BY, or use LIMIT alone to take the first "
                 "few.");
        }
        return c;
    }

    // --- patterns ----------------------------------------------------------
    Pattern parsePattern() {
        Pattern pattern;
        PatElem first;
        first.is_node = true;
        first.node = parseNodePattern();
        pattern.push_back(std::move(first));

        while (at(T::Minus) || at(T::LtArrow)) {
            PatElem rel;
            rel.is_node = false;
            rel.rel = parseRelPattern();
            pattern.push_back(std::move(rel));

            PatElem next;
            next.is_node = true;
            next.node = parseNodePattern();
            pattern.push_back(std::move(next));
        }
        return pattern;
    }

    NodePat parseNodePattern() {
        expect(T::LParen, "'(' to start a node pattern");
        NodePat node;
        if (at(T::Ident)) {
            node.var = tok().text;
            ++p_;
        }
        if (match(T::Colon)) {
            if (!at(T::Ident)) fail("expected a label after ':'");
            node.label = tok().text;
            node.has_label = true;
            ++p_;
            if (at(T::Colon)) fail("a node has exactly one label in this database");
        }
        if (at(T::LBrace)) node.props = parseMapLiteral();
        expect(T::RParen, "')' to close the node pattern");
        return node;
    }

    RelPat parseRelPattern() {
        RelPat rel;
        if (match(T::LtArrow)) rel.dir = -1;
        else expect(T::Minus, "'-' to start a relationship pattern");

        if (match(T::LBracket)) {
            if (at(T::Ident)) {
                rel.var = tok().text;
                ++p_;
            }
            if (match(T::Colon)) {
                if (!at(T::Ident)) fail("expected a relationship type after ':'");
                rel.type = tok().text;
                rel.has_type = true;
                ++p_;
                if (at(T::Pipe)) fail("relationship type alternatives are not supported");
                if (at(T::Colon)) fail("a relationship has exactly one type in this database");
            }
            if (at(T::Star)) {
                fail("variable length paths are not supported. Write the hops explicitly, "
                     "for example (a)-[:R]->(b)-[:R]->(c).");
            }
            if (at(T::LBrace)) rel.props = parseMapLiteral();
            expect(T::RBracket, "']' to close the relationship pattern");
        }

        if (match(T::Arrow)) {
            if (rel.dir == -1) fail("a relationship pattern cannot point both ways");
            rel.dir = 1;
        } else {
            expect(T::Minus, "'-' or '->' to finish the relationship pattern");
        }
        return rel;
    }

    std::vector<MapItem> parseMapLiteral() {
        expect(T::LBrace, "'{'");
        std::vector<MapItem> items;
        if (match(T::RBrace)) return items;
        for (;;) {
            MapItem item;
            if (at(T::Ident) || at(T::String)) {
                item.key = tok().text;
                ++p_;
            } else {
                fail("expected a property name");
            }
            expect(T::Colon, "':' in a property map");
            item.value = parseExpr();
            items.push_back(std::move(item));
            if (!match(T::Comma)) break;
        }
        expect(T::RBrace, "'}' to close the property map");
        return items;
    }

    ExprP parseMapExpression() {
        if (!at(T::LBrace)) fail("expected a property map, for example {name: 'value'}");
        auto node = std::make_unique<Expr>();
        node->k = Expr::K::Map;
        node->entries = parseMapLiteral();
        return node;
    }

    // --- expressions -------------------------------------------------------
    ExprP parseExpr() { return parseOr(); }

    ExprP parseOr() {
        ExprP left = parseAnd();
        while (atKeyword("OR")) {
            ++p_;
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Binary;
            node->op = "OR";
            node->a = std::move(left);
            node->b = parseAnd();
            left = std::move(node);
        }
        return left;
    }

    ExprP parseAnd() {
        ExprP left = parseNot();
        while (atKeyword("AND")) {
            ++p_;
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Binary;
            node->op = "AND";
            node->a = std::move(left);
            node->b = parseNot();
            left = std::move(node);
        }
        return left;
    }

    ExprP parseNot() {
        if (atKeyword("NOT")) {
            ++p_;
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Unary;
            node->op = "NOT";
            node->a = parseNot();
            return node;
        }
        return parseComparison();
    }

    ExprP parseComparison() {
        ExprP left = parseAdditive();

        bool negated = false;
        if (atKeyword("NOT") &&
            (atKeyword("IN", 1) || atKeyword("CONTAINS", 1) || atKeyword("STARTS", 1) ||
             atKeyword("ENDS", 1))) {
            negated = true;
            ++p_;
        }

        const char* op = nullptr;
        switch (tok().kind) {
        case T::Eq: op = "="; break;
        case T::Neq: op = "<>"; break;
        case T::Lt: op = "<"; break;
        case T::Le: op = "<="; break;
        case T::Gt: op = ">"; break;
        case T::Ge: op = ">="; break;
        default: break;
        }
        if (op) {
            if (negated) fail("misplaced NOT");
            ++p_;
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Binary;
            node->op = op;
            node->a = std::move(left);
            node->b = parseAdditive();
            return node;
        }

        if (atKeyword("IS")) {
            if (negated) fail("misplaced NOT");
            ++p_;
            const bool is_not = matchKeyword("NOT");
            if (!matchKeyword("NULL")) fail("expected NULL after IS");
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Unary;
            node->op = is_not ? "IS NOT NULL" : "IS NULL";
            node->a = std::move(left);
            return node;
        }

        std::string word;
        if (atKeyword("IN")) word = "IN";
        else if (atKeyword("CONTAINS")) word = "CONTAINS";
        else if (atKeyword("STARTS")) word = "STARTS WITH";
        else if (atKeyword("ENDS")) word = "ENDS WITH";

        if (!word.empty()) {
            ++p_;
            if (word == "STARTS WITH" && !matchKeyword("WITH")) fail("expected WITH after STARTS");
            if (word == "ENDS WITH" && !matchKeyword("WITH")) fail("expected WITH after ENDS");
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Binary;
            node->op = negated ? ("NOT " + word) : word;
            node->a = std::move(left);
            // The right side of IN is any expression that yields a list: a
            // literal, a parameter, or a multi-valued property.
            node->b = parseAdditive();
            return node;
        }

        if (negated) fail("expected IN, CONTAINS, STARTS WITH or ENDS WITH after NOT");
        return left;
    }

    ExprP parseAdditive() {
        ExprP left = parseMultiplicative();
        while (at(T::Plus) || at(T::Minus)) {
            const std::string op = at(T::Plus) ? "+" : "-";
            ++p_;
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Binary;
            node->op = op;
            node->a = std::move(left);
            node->b = parseMultiplicative();
            left = std::move(node);
        }
        return left;
    }

    ExprP parseMultiplicative() {
        ExprP left = parseUnary();
        while (at(T::Star) || at(T::Slash) || at(T::Percent)) {
            const std::string op = at(T::Star) ? "*" : (at(T::Slash) ? "/" : "%");
            ++p_;
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Binary;
            node->op = op;
            node->a = std::move(left);
            node->b = parseUnary();
            left = std::move(node);
        }
        return left;
    }

    ExprP parseUnary() {
        if (match(T::Minus)) {
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Unary;
            node->op = "-";
            node->a = parseUnary();
            return node;
        }
        if (match(T::Plus)) return parseUnary();
        return parsePrimary();
    }

    ExprP parseListLiteral() {
        if (!at(T::LBracket)) fail("expected a list, for example ['a', 'b']");
        ++p_;
        auto node = std::make_unique<Expr>();
        node->k = Expr::K::List;
        if (match(T::RBracket)) return node;
        for (;;) {
            node->args.push_back(parseExpr());
            if (!match(T::Comma)) break;
        }
        expect(T::RBracket, "']' to close the list");
        return node;
    }

    ExprP parsePrimary() {
        const Token& t = tok();
        switch (t.kind) {
        case T::Integer:
        case T::Real:
        case T::String: {
            // Numbers and booleans are stored as the text that was written.
            // Keeping t.text rather than reformatting means 30 stays "30" and
            // 0.1234 stays "0.1234" -- what went in is what comes back.
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Literal;
            node->lit = Value::text(t.text);
            ++p_;
            return node;
        }
        case T::Param: {
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Param;
            node->name = t.text;
            ++p_;
            return node;
        }
        case T::LParen: {
            ++p_;
            ExprP inner = parseExpr();
            expect(T::RParen, "')'");
            return inner;
        }
        case T::LBracket:
            return parseListLiteral();
        case T::Star: {
            ++p_;
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Star;
            return node;
        }
        case T::Ident: {
            const std::string name = t.text;
            const std::string upper = toUpper(name);
            if (upper == "NULL") {
                ++p_;
                auto node = std::make_unique<Expr>();
                node->k = Expr::K::Literal;
                node->lit = Value::null();
                return node;
            }
            if (upper == "TRUE" || upper == "FALSE") {
                ++p_;
                auto node = std::make_unique<Expr>();
                node->k = Expr::K::Literal;
                node->lit = Value::text(upper == "TRUE" ? "true" : "false");
                return node;
            }
            if (upper == "CASE") fail("CASE is not supported. Filter with WHERE instead.");
            ++p_;
            if (at(T::LParen)) {
                ++p_;
                auto node = std::make_unique<Expr>();
                if (upper == "COUNT") {
                    node->k = Expr::K::Count;
                    if (at(T::Star)) {
                        node->star = true;
                        ++p_;
                    } else {
                        node->a = parseExpr();
                    }
                    expect(T::RParen, "')' to close count()");
                    return node;
                }
                if (upper == "ID" || upper == "LABELS" || upper == "TYPE") {
                    node->k = Expr::K::Unary;
                    node->op = upper;
                    node->a = parseExpr();
                    expect(T::RParen, "')' to close the function call");
                    return node;
                }
                fail("unknown function '" + name +
                     "'. Available: id(), labels(), type(), count()");
            }
            if (match(T::Dot)) {
                if (!at(T::Ident)) fail("expected a property name after '.'");
                auto node = std::make_unique<Expr>();
                node->k = Expr::K::Prop;
                node->name = tok().text;
                ++p_;
                auto base = std::make_unique<Expr>();
                base->k = Expr::K::Var;
                base->name = name;
                node->a = std::move(base);
                return node;
            }
            auto node = std::make_unique<Expr>();
            node->k = Expr::K::Var;
            node->name = name;
            return node;
        }
        default:
            fail("expected an expression");
        }
    }
};

// ---------------------------------------------------------------------------
// evaluation
// ---------------------------------------------------------------------------

using Bindings = std::unordered_map<std::string, Value>;

struct EvalCtx {
    Database* db = nullptr;
    const Parameters* params = nullptr;
    const std::vector<Bindings>* all = nullptr;  // set while projecting count()
};

Value evalExpr(EvalCtx& ctx, const Expr& e, const Bindings& row);

bool hasCount(const Expr& e) {
    if (e.k == Expr::K::Count) return true;
    if (e.a && hasCount(*e.a)) return true;
    if (e.b && hasCount(*e.b)) return true;
    return false;
}

bool truthy(const Value& v) { return !v.isNull() && v.asBool(); }

// Comparison results are text too: there is no boolean property type, so a
// predicate yields the strings "true" and "false".
Value truth(bool value) { return Value::text(value ? "true" : "false"); }

// Properties are text, so ordering is text ordering. That is exactly right for
// zero-padded values like ISO dates ('1990-03-05'), which sort chronologically
// as strings. It is not numeric ordering: '10' sorts before '9'. Pad numbers to
// a fixed width if they need to be ordered.
int compareValues(const Value& a, const Value& b) {
    if (a.type == ValueType::Str && b.type == ValueType::Str) {
        return a.s < b.s ? -1 : (a.s > b.s ? 1 : 0);
    }
    if (a.isGraph() && b.isGraph()) {
        return a.ref < b.ref ? -1 : (a.ref > b.ref ? 1 : 0);
    }
    throw Error("cannot order '" + a.toString() + "' against '" + b.toString() + "'");
}

// '+' is the only operator property values support, and it concatenates: text
// with text, list with list or with one element. There is no arithmetic on
// properties, because their values are text.
Value plusValues(const Value& a, const Value& b) {
    if (a.type == ValueType::List || b.type == ValueType::List) {
        std::vector<std::string> out;
        auto append = [&out](const Value& v) {
            if (v.type == ValueType::List) out.insert(out.end(), v.items.begin(), v.items.end());
            else if (!v.isNull()) out.push_back(v.asText());
        };
        append(a);
        append(b);
        return Value::list(std::move(out));
    }
    return Value::text(a.asText() + b.asText());
}

// count() yields text like every other value, so `count(*) > 5` written against
// a literal compares as text rather than never matching.
Value countResult(std::int64_t n) { return Value::text(std::to_string(n)); }

Value evalCount(EvalCtx& ctx, const Expr& e, const Bindings& row) {
    const std::vector<Bindings>* rows = ctx.all;
    if (!rows) {
        if (e.star) return countResult(1);
        return evalExpr(ctx, *e.a, row).isNull() ? countResult(0) : countResult(1);
    }
    if (e.star) return countResult(static_cast<std::int64_t>(rows->size()));
    std::int64_t seen = 0;
    for (const Bindings& r : *rows) {
        if (!evalExpr(ctx, *e.a, r).isNull()) ++seen;
    }
    return countResult(seen);
}

Value evalExpr(EvalCtx& ctx, const Expr& e, const Bindings& row) {
    switch (e.k) {
    case Expr::K::Literal: return e.lit;
    case Expr::K::Param: {
        if (!ctx.params) throw Error("query uses $" + e.name + " but no parameters were given");
        auto it = ctx.params->find(e.name);
        if (it == ctx.params->end()) throw Error("missing query parameter $" + e.name);
        return it->second;
    }
    case Expr::K::Star:
        throw Error("'*' is only valid inside count(*) or RETURN *");
    case Expr::K::List: {
        std::vector<std::string> items;
        for (const ExprP& arg : e.args) {
            const Value v = evalExpr(ctx, *arg, row);
            if (!v.isNull()) items.push_back(v.asText());
        }
        return Value::list(std::move(items));
    }
    case Expr::K::Map:
        throw Error("a property map is only valid on the right of SET");
    case Expr::K::Var: {
        auto it = row.find(e.name);
        if (it == row.end()) throw Error("variable '" + e.name + "' is not defined here");
        return it->second;
    }
    case Expr::K::Prop: {
        const Value base = evalExpr(ctx, *e.a, row);
        if (base.type == ValueType::Node) return ctx.db->nodeProp(base.ref, e.name);
        if (base.type == ValueType::Rel) return ctx.db->edgeProp(base.ref, e.name);
        if (base.isNull()) return Value::null();
        throw Error("cannot read property '" + e.name + "' from '" + base.toString() + "'");
    }
    case Expr::K::Count:
        return evalCount(ctx, e, row);
    case Expr::K::Unary: {
        if (e.op == "NOT") {
            const Value v = evalExpr(ctx, *e.a, row);
            return v.isNull() ? Value::null() : truth(!v.asBool());
        }
        if (e.op == "IS NULL") return truth(evalExpr(ctx, *e.a, row).isNull());
        if (e.op == "IS NOT NULL") return truth(!evalExpr(ctx, *e.a, row).isNull());
        if (e.op == "ID") {
            const Value v = evalExpr(ctx, *e.a, row);
            return v.isGraph() ? countResult(static_cast<std::int64_t>(v.ref)) : Value::null();
        }
        if (e.op == "LABELS") {
            const Value v = evalExpr(ctx, *e.a, row);
            return v.type == ValueType::Node ? Value::text(ctx.db->nodeLabel(v.ref)) : Value::null();
        }
        if (e.op == "TYPE") {
            const Value v = evalExpr(ctx, *e.a, row);
            return v.type == ValueType::Rel ? Value::text(ctx.db->edgeType(v.ref)) : Value::null();
        }
        if (e.op == "-") {
            // Everything is text, so a leading '-' is just part of the text.
            const Value v = evalExpr(ctx, *e.a, row);
            return v.isNull() ? Value::null() : Value::text("-" + v.asText());
        }
        throw Error("unsupported unary operator '" + e.op + "'");
    }
    case Expr::K::Binary: {
        const std::string& op = e.op;
        if (op == "AND" || op == "OR") {
            const Value l = evalExpr(ctx, *e.a, row);
            const Value r = evalExpr(ctx, *e.b, row);
            if (op == "AND") {
                if (!l.isNull() && !l.asBool()) return truth(false);
                if (!r.isNull() && !r.asBool()) return truth(false);
                if (l.isNull() || r.isNull()) return Value::null();
                return truth(true);
            }
            if (!l.isNull() && l.asBool()) return truth(true);
            if (!r.isNull() && r.asBool()) return truth(true);
            if (l.isNull() || r.isNull()) return Value::null();
            return truth(false);
        }

        const Value l = evalExpr(ctx, *e.a, row);

        if (op == "IN" || op == "NOT IN") {
            if (l.isNull()) return Value::null();
            // The right side is any expression that yields a list: a literal, a
            // parameter, or a multi-valued property.
            const Value haystack = evalExpr(ctx, *e.b, row);
            if (haystack.isNull()) return Value::null();
            if (haystack.type != ValueType::List) {
                throw Error("the right side of IN must be a list, for example ['a', 'b'] or a "
                            "multi-valued property");
            }
            bool found = false;
            for (const std::string& item : haystack.items) {
                if (item == l.asText()) {
                    found = true;
                    break;
                }
            }
            return truth(op == "IN" ? found : !found);
        }

        const Value r = evalExpr(ctx, *e.b, row);

        if (op == "CONTAINS" || op == "NOT CONTAINS" || op == "STARTS WITH" ||
            op == "NOT STARTS WITH" || op == "ENDS WITH" || op == "NOT ENDS WITH") {
            if (l.isNull() || r.isNull()) return Value::null();
            const std::string needle = r.asText();
            auto matches = [&](const std::string& haystack) {
                if (op == "CONTAINS" || op == "NOT CONTAINS") {
                    return haystack.find(needle) != std::string::npos;
                }
                if (op == "STARTS WITH" || op == "NOT STARTS WITH") {
                    return haystack.size() >= needle.size() &&
                           haystack.compare(0, needle.size(), needle) == 0;
                }
                return haystack.size() >= needle.size() &&
                       haystack.compare(haystack.size() - needle.size(), needle.size(), needle) == 0;
            };
            // A multi-valued property matches when any one of its values does.
            // Testing the joined text instead would let "学, 编" match a list of
            // ["天文学", "编程"] -- a wrong answer rather than an error, and
            // searching an alias or interest list by fragment is the normal case.
            bool found = false;
            if (l.type == ValueType::List) {
                for (const std::string& item : l.items) {
                    if (matches(item)) {
                        found = true;
                        break;
                    }
                }
            } else {
                found = matches(l.asText());
            }
            return truth(op.rfind("NOT ", 0) == 0 ? !found : found);
        }

        if (op == "=" || op == "<>") {
            if (l.isNull() || r.isNull()) return Value::null();
            const bool equal = (l == r);
            return truth(op == "=" ? equal : !equal);
        }
        if (op == "+") {
            if (l.isNull() || r.isNull()) return Value::null();
            return plusValues(l, r);
        }
        if (op == "-" || op == "*" || op == "/" || op == "%") {
            throw Error("'" + op + "' is not available: property values are text. Do the "
                        "arithmetic in the caller and write the result back.");
        }
        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (l.isNull() || r.isNull()) return Value::null();
            const int cmp = compareValues(l, r);
            if (op == "<") return truth(cmp < 0);
            if (op == "<=") return truth(cmp <= 0);
            if (op == ">") return truth(cmp > 0);
            return truth(cmp >= 0);
        }
        throw Error("unsupported operator '" + op + "'");
    }
    }
    return Value::null();
}

// ---------------------------------------------------------------------------
// execution
// ---------------------------------------------------------------------------

struct Engine {
    Database& db;
    const Parameters& params;
    QueryResult result;
    EvalCtx ctx;
    std::vector<Bindings> rows;
    std::size_t rowLimit = kDefaultRowLimit;
    std::size_t rowsProduced = 0;

    Engine(Database& database, const Parameters& parameters)
        : db(database), params(parameters) {
        ctx.db = &db;
        ctx.params = &params;
        rows.emplace_back();
    }

    // Traversal depth is bounded by the query text, but the row count is not:
    // two hops on a dense graph is degree squared and three is cubed. LIMIT does
    // not help, because it is applied after the rows exist. Fail loudly instead
    // of exhausting memory.
    void noteRow() {
        if (++rowsProduced > rowLimit) {
            throw Error("this query materialised more than " + std::to_string(rowLimit) +
                        " intermediate rows. Add a WHERE filter, use fewer hops, or raise the "
                        "limit with Database::setRowLimit().");
        }
    }

    const Store& store() const { return db.store(); }

    Value constantValue(const Expr& e) const {
        if (e.k == Expr::K::Literal) return e.lit;
        if (e.k == Expr::K::Param) {
            auto it = params.find(e.name);
            if (it == params.end()) throw Error("missing query parameter $" + e.name);
            return it->second;
        }
        if (e.k == Expr::K::Unary && e.op == "-") {
            return Value::text("-" + constantValue(*e.a).asText());
        }
        if (e.k == Expr::K::List) {
            std::vector<std::string> items;
            for (const ExprP& arg : e.args) items.push_back(constantValue(*arg).asText());
            return Value::list(std::move(items));
        }
        throw Error("values in a pattern must be literals or parameters");
    }

    std::vector<Id> candidates(const NodePat& node, const Bindings& row) const {
        if (!node.var.empty()) {
            auto it = row.find(node.var);
            if (it != row.end()) {
                if (it->second.type != ValueType::Node) return {};
                return {it->second.ref};
            }
        }
        for (const MapItem& item : node.props) {
            const Value v = constantValue(*item.value);
            if (v.isNull()) continue;
            // The index is exact on the canonical key, so an empty bucket really
            // does mean "no node carries this value".
            const std::uint32_t key = store().find(item.key);
            if (key == kNoStr) return {};
            return store().nodesByProp(key, v);
        }
        if (node.has_label) {
            const std::uint32_t sid = store().find(node.label);
            if (sid == kNoStr) return {};
            return store().nodesByLabel(sid);
        }
        return db.allNodes();
    }

    bool nodeMatches(const NodePat& node, Id id) const {
        if (node.has_label && db.nodeLabel(id) != node.label) return false;
        for (const MapItem& item : node.props) {
            const Value want = constantValue(*item.value);
            if (want.isNull()) {
                if (db.nodeHasProp(id, item.key)) return false;
            } else if (!(db.nodeProp(id, item.key) == want)) {
                return false;
            }
        }
        return true;
    }

    bool relMatches(const RelPat& rel, Id edge) const {
        if (rel.has_type && db.edgeType(edge) != rel.type) return false;
        for (const MapItem& item : rel.props) {
            const Value want = constantValue(*item.value);
            if (want.isNull()) {
                if (!db.edgeProp(edge, item.key).isNull()) return false;
            } else if (!(db.edgeProp(edge, item.key) == want)) {
                return false;
            }
        }
        return true;
    }

    std::vector<Id> incidentEdges(Id from, int dir) const {
        std::vector<Id> out;
        if (dir >= 0) {
            for (const EdgeRef& e : db.outEdges(from)) out.push_back(e.id);
        }
        if (dir <= 0) {
            for (const EdgeRef& e : db.inEdges(from)) {
                // A self loop already appeared in the outgoing pass.
                if (dir == 0 && db.edgeSource(e.id) == db.edgeTarget(e.id)) continue;
                out.push_back(e.id);
            }
        }
        return out;
    }

    bool bindNode(const NodePat& node, Id id, Bindings& row) const {
        if (!node.var.empty()) {
            auto it = row.find(node.var);
            if (it != row.end()) {
                if (it->second.type != ValueType::Node || it->second.ref != id) return false;
            } else {
                row.emplace(node.var, Value::node(id));
            }
        }
        return true;
    }

    std::string describe(const NodePat& node) const {
        std::string out = "(";
        if (!node.var.empty()) out += node.var;
        if (node.has_label) out += ":" + node.label;
        out += ")";
        return out;
    }

    // predicate, when set, is the WHERE clause that immediately follows this
    // MATCH. Filtering inside the traversal instead of afterwards is what keeps
    // a scan query from building one binding map per candidate and throwing
    // almost all of them away.
    void walkRel(const Pattern& pattern, std::size_t index, Id from, Bindings row,
                 const Expr* predicate, std::vector<Bindings>& out) {
        if (index >= pattern.size()) {
            if (predicate && !truthy(evalExpr(ctx, *predicate, row))) return;
            noteRow();
            out.push_back(std::move(row));
            return;
        }
        const RelPat& rel = pattern[index].rel;
        const NodePat& target = pattern[index + 1].node;

        for (Id edge : incidentEdges(from, rel.dir)) {
            if (!relMatches(rel, edge)) continue;

            Bindings next = row;
            if (!rel.var.empty()) {
                auto it = next.find(rel.var);
                if (it != next.end()) {
                    if (it->second.type != ValueType::Rel || it->second.ref != edge) continue;
                } else {
                    next.emplace(rel.var, Value::rel(edge));
                }
            }

            const Id other =
                (db.edgeSource(edge) == from) ? db.edgeTarget(edge) : db.edgeSource(edge);
            if (!db.nodeExists(other)) continue;
            if (!nodeMatches(target, other)) continue;
            if (!bindNode(target, other, next)) continue;

            walkRel(pattern, index + 2, other, std::move(next), predicate, out);
        }
    }

    void matchPattern(const Pattern& pattern, const Bindings& start, const Expr* predicate,
                      std::vector<Bindings>& out) {
        std::vector<Bindings> current{start};

        for (std::size_t i = 0; i < pattern.size(); i += 2) {
            const NodePat& node = pattern[i].node;
            const bool last = (i + 1 >= pattern.size());
            std::vector<Bindings> bound;
            std::vector<Id> boundIds;
            for (const Bindings& row : current) {
                for (Id id : candidates(node, row)) {
                    if (!db.nodeExists(id) || !nodeMatches(node, id)) continue;
                    Bindings copy = row;
                    if (!bindNode(node, id, copy)) continue;
                    // For the final node the row is complete, so the predicate can
                    // be tested before this row is kept.
                    if (last && predicate && !truthy(evalExpr(ctx, *predicate, copy))) continue;
                    noteRow();
                    bound.push_back(std::move(copy));
                    boundIds.push_back(id);
                }
            }
            if (bound.empty()) return;

            std::vector<Bindings> expanded;
            for (std::size_t k = 0; k < bound.size(); ++k) {
                if (last) {
                    expanded.push_back(std::move(bound[k]));
                    continue;
                }
                walkRel(pattern, i + 1, boundIds[k], std::move(bound[k]), predicate, expanded);
            }
            current = std::move(expanded);
            if (current.empty()) return;
        }
        for (Bindings& row : current) out.push_back(std::move(row));
    }

    void applyMatch(const Clause& clause, const Expr* predicate) {
        std::vector<Bindings> next;
        for (const Bindings& row : rows) {
            std::vector<Bindings> current{row};
            for (std::size_t p = 0; p < clause.patterns.size(); ++p) {
                // Only the final pattern has every variable bound, so only there
                // is the predicate safe to evaluate.
                const Expr* local = (p + 1 == clause.patterns.size()) ? predicate : nullptr;
                std::vector<Bindings> out;
                for (const Bindings& r : current) matchPattern(clause.patterns[p], r, local, out);
                current = std::move(out);
                if (current.empty()) break;
            }
            for (Bindings& r : current) next.push_back(std::move(r));
        }
        rows = std::move(next);
    }

    void applyWhere(const Clause& clause) {
        std::vector<Bindings> next;
        for (const Bindings& row : rows) {
            if (truthy(evalExpr(ctx, *clause.expr, row))) next.push_back(row);
        }
        rows = std::move(next);
    }

    bool nodeHasPatternProps(const NodePat& node, Id id) const {
        for (const MapItem& item : node.props) {
            if (!(db.nodeProp(id, item.key) == constantValue(*item.value))) return false;
        }
        return true;
    }

    Properties mapToProperties(const std::vector<MapItem>& entries, const Bindings& row) {
        Properties props;
        for (const MapItem& item : entries) {
            const Value v = evalExpr(ctx, *item.value, row);
            if (!v.isNull()) props.push_back(Prop{item.key, v});
        }
        return props;
    }

    Id resolveOrCreateNode(const NodePat& node, Bindings& row) {
        if (!node.var.empty()) {
            auto it = row.find(node.var);
            if (it != row.end()) {
                if (it->second.type != ValueType::Node)
                    throw Error("'" + node.var + "' is not a node");
                if (!nodeHasPatternProps(node, it->second.ref))
                    throw Error("node '" + node.var + "' does not match the pattern");
                return it->second.ref;
            }
        }
        const Id id = db.addNode(node.has_label ? node.label : std::string(),
                                 mapToProperties(node.props, row));
        ++result.nodes_created;
        if (!node.var.empty()) row[node.var] = Value::node(id);
        return id;
    }

    void createPattern(const Pattern& pattern, Bindings& row) {
        Id left = resolveOrCreateNode(pattern[0].node, row);
        for (std::size_t i = 1; i + 1 < pattern.size(); i += 2) {
            const RelPat& rel = pattern[i].rel;
            const Id right = resolveOrCreateNode(pattern[i + 1].node, row);

            const Id src = (rel.dir == -1) ? right : left;
            const Id dst = (rel.dir == -1) ? left : right;
            const Id edge = db.addEdge(rel.has_type ? rel.type : std::string(), src, dst,
                                       mapToProperties(rel.props, row));
            ++result.edges_created;
            if (!rel.var.empty()) row[rel.var] = Value::rel(edge);
            left = right;
        }
    }

    void applyCreate(const Clause& clause) {
        for (Bindings& row : rows) {
            for (const Pattern& pattern : clause.patterns) createPattern(pattern, row);
        }
    }

    // MERGE (single node pattern): match it, or create it.
    Id mergeNode(const NodePat& node, Bindings& row) {
        if (!node.var.empty()) {
            auto it = row.find(node.var);
            if (it != row.end()) {
                if (it->second.type != ValueType::Node)
                    throw Error("'" + node.var + "' is not a node");
                if (!nodeMatches(node, it->second.ref))
                    throw Error("node '" + node.var + "' does not match the MERGE pattern");
                return it->second.ref;
            }
        }

        std::vector<Id> matches;
        for (Id id : candidates(node, row)) {
            if (db.nodeExists(id) && nodeMatches(node, id)) matches.push_back(id);
        }
        if (matches.size() > 1) {
            throw Error("MERGE matched " + std::to_string(matches.size()) + " nodes for " +
                        describe(node) +
                        ". MERGE needs a unique key; add one, or use MATCH with LIMIT 1 if any "
                        "match will do.");
        }
        if (matches.size() == 1) {
            if (!node.var.empty()) row[node.var] = Value::node(matches[0]);
            return matches[0];
        }
        return resolveOrCreateNode(node, row);
    }

    // MERGE (a)-[r:T]->(b) with both endpoints already bound.
    Id mergeEdge(const RelPat& rel, Id from, Id to, Bindings& row) {
        if (!rel.var.empty()) {
            auto it = row.find(rel.var);
            if (it != row.end()) {
                if (it->second.type != ValueType::Rel)
                    throw Error("'" + rel.var + "' is not a relationship");
                return it->second.ref;
            }
        }

        std::vector<Id> matches;
        for (Id edge : incidentEdges(from, rel.dir)) {
            const Id other =
                (db.edgeSource(edge) == from) ? db.edgeTarget(edge) : db.edgeSource(edge);
            if (other == to && relMatches(rel, edge)) matches.push_back(edge);
        }
        if (matches.size() > 1) {
            throw Error("MERGE matched " + std::to_string(matches.size()) +
                        " relationships between the two nodes; the pattern is ambiguous.");
        }
        if (matches.size() == 1) {
            if (!rel.var.empty()) row[rel.var] = Value::rel(matches[0]);
            return matches[0];
        }

        const Id src = (rel.dir == -1) ? to : from;
        const Id dst = (rel.dir == -1) ? from : to;
        const Id edge = db.addEdge(rel.has_type ? rel.type : std::string(), src, dst,
                                   mapToProperties(rel.props, row));
        ++result.edges_created;
        if (!rel.var.empty()) row[rel.var] = Value::rel(edge);
        return edge;
    }

    // Each part of the pattern is merged on its own: a node is matched by its
    // key and only created when genuinely absent, then the relationship is
    // matched and only created when absent.
    //
    // This deliberately differs from Neo4j, whose whole-path MERGE creates a
    // brand new copy of every node when only the relationship is missing -- the
    // documented footgun that duplicates people. Merging part by part can never
    // produce a duplicate, and behaves identically in every other case.
    void applyMerge(const Clause& clause) {
        for (Bindings& row : rows) {
            for (const Pattern& pattern : clause.patterns) {
                Id left = mergeNode(pattern[0].node, row);
                for (std::size_t i = 1; i + 1 < pattern.size(); i += 2) {
                    const Id right = mergeNode(pattern[i + 1].node, row);
                    mergeEdge(pattern[i].rel, left, right, row);
                    left = right;
                }
            }
        }
    }

    void applySet(const Clause& clause) {
        for (Bindings& row : rows) {
            for (const SetItem& item : clause.sets) {
                auto it = row.find(item.var);
                if (it == row.end()) throw Error("variable '" + item.var + "' is not defined here");
                const Value target = it->second;
                if (!target.isGraph()) throw Error("SET needs a node or a relationship");
                const bool onNode = target.type == ValueType::Node;

                if (item.mode == SetMode::Assign) {
                    const Value value = evalExpr(ctx, *item.value, row);
                    const bool ok = onNode ? db.setNodeProp(target.ref, item.key, value)
                                           : db.setEdgeProp(target.ref, item.key, value);
                    if (!ok) throw Error("cannot set a property on a deleted record");
                    ++result.props_set;
                    continue;
                }

                if (item.value->k != Expr::K::Map) {
                    throw Error("SET ... = needs a property map, for example SET n = {name: 'x'}");
                }
                if (item.mode == SetMode::Replace) {
                    const Properties existing = onNode ? db.nodeProps(target.ref)
                                                       : db.edgeProps(target.ref);
                    for (const Prop& p : existing) {
                        if (onNode) db.removeNodeProp(target.ref, p.key);
                        else db.removeEdgeProp(target.ref, p.key);
                    }
                }
                const Properties incoming = mapToProperties(item.value->entries, row);
                if (onNode) {
                    if (!db.nodeExists(target.ref)) throw Error("cannot set properties on a deleted node");
                } else if (!db.edgeExists(target.ref)) {
                    throw Error("cannot set properties on a deleted relationship");
                }
                for (const Prop& p : incoming) {
                    const bool ok = onNode ? db.setNodeProp(target.ref, p.key, p.value)
                                           : db.setEdgeProp(target.ref, p.key, p.value);
                    if (!ok) throw Error("cannot set a property on a deleted record");
                    ++result.props_set;
                }
            }
        }
    }

    void applyDelete(const Clause& clause) {
        for (const Bindings& row : rows) {
            for (const std::string& var : clause.vars) {
                auto it = row.find(var);
                if (it == row.end()) throw Error("variable '" + var + "' is not defined here");
                const Value v = it->second;
                if (v.type == ValueType::Node) {
                    const std::size_t before = db.edgeCount();
                    if (db.removeNode(v.ref)) {
                        ++result.nodes_deleted;
                        result.edges_deleted += static_cast<std::int64_t>(before - db.edgeCount());
                    }
                } else if (v.type == ValueType::Rel) {
                    if (db.removeEdge(v.ref)) ++result.edges_deleted;
                } else {
                    throw Error("DELETE needs a node or a relationship");
                }
            }
        }
    }
};

QueryResult finishReturn(Engine& engine, const Clause& clause) {
    QueryResult& out = engine.result;
    const bool star = clause.items.size() == 1 && clause.items[0].star;

    std::vector<std::string> columns;
    if (star) {
        std::set<std::string> names;
        for (const Bindings& row : engine.rows) {
            for (const auto& kv : row) names.insert(kv.first);
        }
        columns.assign(names.begin(), names.end());
    } else {
        for (const ReturnItem& item : clause.items) columns.push_back(item.label);
    }

    bool aggregated = false;
    if (!star) {
        for (const ReturnItem& item : clause.items) {
            if (hasCount(*item.expr)) {
                aggregated = true;
                break;
            }
        }
    }

    struct OutRow {
        std::vector<Value> values;
        const Bindings* source = nullptr;
    };
    std::vector<OutRow> projected;

    if (aggregated) {
        // count() collapses everything to a single row. An empty match still
        // yields that one row, so count(*) over nothing is 0 rather than absent.
        static const Bindings kEmpty;
        const Bindings& base = engine.rows.empty() ? kEmpty : engine.rows.front();
        EvalCtx ctx = engine.ctx;
        ctx.all = &engine.rows;
        OutRow row;
        row.source = &base;
        for (const ReturnItem& item : clause.items) {
            row.values.push_back(evalExpr(ctx, *item.expr, base));
        }
        projected.push_back(std::move(row));
    } else {
        for (const Bindings& bindings : engine.rows) {
            OutRow row;
            row.source = &bindings;
            if (star) {
                for (const std::string& name : columns) {
                    auto it = bindings.find(name);
                    row.values.push_back(it == bindings.end() ? Value::null() : it->second);
                }
            } else {
                for (const ReturnItem& item : clause.items) {
                    row.values.push_back(evalExpr(engine.ctx, *item.expr, bindings));
                }
            }
            projected.push_back(std::move(row));
        }
    }

    if (clause.distinct) {
        std::unordered_set<std::string> seen;
        std::vector<OutRow> unique;
        for (OutRow& row : projected) {
            std::string key;
            for (const Value& v : row.values) {
                key += v.toString();
                key.push_back('\x1f');
            }
            if (seen.insert(key).second) unique.push_back(std::move(row));
        }
        projected = std::move(unique);
    }

    if (!clause.order.empty()) {
        auto orderValue = [&](const OutRow& row, const OrderItem& item) -> Value {
            for (std::size_t c = 0; c < columns.size(); ++c) {
                if (columns[c] == item.label) return row.values[c];
            }
            return evalExpr(engine.ctx, *item.expr, *row.source);
        };
        std::stable_sort(projected.begin(), projected.end(),
                         [&](const OutRow& x, const OutRow& y) {
                             for (const OrderItem& item : clause.order) {
                                 const Value a = orderValue(x, item);
                                 const Value b = orderValue(y, item);
                                 if (a.isNull() || b.isNull()) {
                                     if (a.isNull() && b.isNull()) continue;
                                     return b.isNull();  // nulls sort last
                                 }
                                 const int cmp = compareValues(a, b);
                                 if (cmp == 0) continue;
                                 return item.desc ? cmp > 0 : cmp < 0;
                             }
                             return false;
                         });
    }

    std::size_t begin = 0;
    if (clause.has_skip) {
        begin = clause.skip < 0 ? 0 : static_cast<std::size_t>(clause.skip);
        if (begin > projected.size()) begin = projected.size();
    }
    std::size_t end = projected.size();
    if (clause.has_limit) {
        const std::size_t limit = clause.limit < 0 ? 0 : static_cast<std::size_t>(clause.limit);
        end = std::min(projected.size(), begin + limit);
    }

    out.columns = std::move(columns);
    for (std::size_t i = begin; i < end; ++i) {
        Row row;
        row.values = std::move(projected[i].values);
        out.rows.push_back(std::move(row));
    }
    return out;
}

}  // namespace

QueryResult Database::query(const std::string& cypher) {
    Parameters empty;
    return query(cypher, empty);
}

QueryResult Database::query(const std::string& cypher, const Parameters& params) {
    Parser parser(cypher);
    std::vector<Clause> clauses = parser.parseProgram();

    Engine engine(*this, params);
    engine.rowLimit = row_limit_;
    for (std::size_t i = 0; i < clauses.size(); ++i) {
        const Clause& clause = clauses[i];
        switch (clause.k) {
        case Clause::K::Match: {
            // Fuse a WHERE that immediately follows this MATCH into the
            // traversal. Keeping it a separate pass costs a full set of binding
            // maps for rows that are about to be discarded -- which is every row
            // but a handful on a scan.
            const Expr* predicate = nullptr;
            if (i + 1 < clauses.size() && clauses[i + 1].k == Clause::K::Where) {
                predicate = clauses[i + 1].expr.get();
                engine.applyMatch(clause, predicate);
                ++i;
            } else {
                engine.applyMatch(clause, nullptr);
            }
            break;
        }
        case Clause::K::Where: engine.applyWhere(clause); break;
        case Clause::K::Create: engine.applyCreate(clause); break;
        case Clause::K::Merge: engine.applyMerge(clause); break;
        case Clause::K::Set: engine.applySet(clause); break;
        case Clause::K::Delete: engine.applyDelete(clause); break;
        case Clause::K::Return: return finishReturn(engine, clause);
        }
    }
    return engine.result;
}

}  // namespace gdb
