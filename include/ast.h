#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ast.h — Abstract Syntax Tree node types for Dabble
//
// Every Dabble statement is one of these structs, wrapped in ASTNode and held
// as a shared_ptr (ASTPtr).  The variant in ASTNode lets the interpreter use
// std::visit for exhaustive dispatch without virtual dispatch overhead.
//
// Naming convention:
//   LetStmt       let / table  — materialise a query into a temp table
//   ValStmt       val / scalar — store a scalar value via DuckDB SET VARIABLE
//   FnStmt        fn           — define a lazy function (builds CTE chain)
//   ProjectionStmt projection  — define a named column list for ...spread
//   ForStmt       for          — iterate rows of a table or query
//   IfStmt        if / else    — conditional branch
//   WhileStmt     while        — loop while condition holds
//   ExpectStmt    expect/check — data quality assertion (fail or warn)
//   SQLStmt       (raw)        — any SQL not matching a keyword above
//   PrintStmt     print        — print a value or query result
//   ImportStmt    import       — run another .dabble file in current context
// ─────────────────────────────────────────────────────────────────────────────
#include <vector>
#include <memory>
#include <variant>
#include <string>

struct ASTNode;
using ASTPtr = std::shared_ptr<ASTNode>;

struct LetStmt    { std::string name, sql; };
struct ValStmt    { std::string name, expr; };  // scalar: stores first cell in env
struct ForStmt    { std::string var, source; std::vector<ASTPtr> body; };
struct IfStmt     { std::string cond; std::vector<ASTPtr> thenb, elseb; };
struct WhileStmt  { std::string cond; std::vector<ASTPtr> body; };
struct ExpectStmt { std::string condition; std::string action; std::string message; };
struct FnStmt     { std::string name; std::vector<std::string> params; std::vector<ASTPtr> body; };
struct SQLStmt    { std::string sql; std::string redirect_file; bool append = false; };
struct PrintStmt  { std::string text; };
struct ImportStmt { std::string filename; };
struct ProjectionStmt { std::string name, cols; };  // named column list for ...spread

struct ASTNode {
    std::variant<LetStmt, ValStmt, ForStmt, IfStmt, WhileStmt, ExpectStmt,
                 FnStmt, SQLStmt, PrintStmt, ImportStmt, ProjectionStmt> node;
    int line_no = 0;

    template<typename T>
    ASTNode(T&& n, int line = 0) : node(std::forward<T>(n)), line_no(line) {}
};
