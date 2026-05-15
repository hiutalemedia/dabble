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
struct ForStmt    { std::string index_var, var, source; std::vector<ASTPtr> body; };
//   index_var: set if "for i, row in table:" — empty otherwise
struct BreakStmt    {};  // break; — exit the enclosing for/while
struct ContinueStmt {};  // continue; — skip to next iteration
struct IfStmt     { std::string cond; std::vector<ASTPtr> thenb, elseb; };
struct WhileStmt  { std::string cond; std::vector<ASTPtr> body; };
struct ExpectStmt { std::string condition; std::string action; std::string message; };
struct FnStmt     { std::string name; std::vector<std::string> params; std::vector<ASTPtr> body; };
struct SQLStmt    { std::string sql; std::string redirect_file; bool append = false; };
struct PrintStmt  { std::string text; };
struct LogStmt    { std::string text; std::string level; };  // like print but writes to __dabble_log + stderr. level: debug/info/warn/error
struct ImportStmt { std::string filename; };
struct ProjectionStmt { std::string name, cols; };  // named column list for ...spread

// Array let — a named, ordered collection of temp tables.
// Each item is stored as __arr_{name}_{index} (1-based).
// A TEMP VIEW named {name} is kept up-to-date as a UNION BY NAME of all items.
// Supports: data += expr, data[1], data[-1], data[scalar_val], data (full union via view).
struct ArrLetStmt { std::string name; std::string expr; };  // += append statement

struct ASTNode {
    std::variant<LetStmt, ValStmt, ForStmt, IfStmt, WhileStmt, ExpectStmt,
                 FnStmt, SQLStmt, PrintStmt, LogStmt, ImportStmt, ProjectionStmt,
                 ArrLetStmt, BreakStmt, ContinueStmt> node;
    int line_no = 0;

    template<typename T>
    ASTNode(T&& n, int line = 0) : node(std::forward<T>(n)), line_no(line) {}
};
