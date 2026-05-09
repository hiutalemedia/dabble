#pragma once
#include "ast.h"
#include "duckdb.h"
#include <unordered_map>
#include <vector>
#include <string>

// Returned by execFn — a self-contained CTE chain + final SELECT.
// Callers either materialize it into a temp table (let x = fn())
// or execute and print it (standalone fn()).
struct FnResult {
    std::vector<std::pair<std::string,std::string>> ctes;  // name → sql
    std::string select;
    std::vector<std::string> vars_to_reset;  // param variables — reset AFTER caller materializes

    // Build a single self-contained SQL string: WITH ... SELECT ...
    std::string build() const {
        if (select.empty()) return "";
        if (ctes.empty()) return select;
        std::string sql = "WITH ";
        for (size_t i = 0; i < ctes.size(); i++) {
            if (i > 0) sql += ",\n     ";
            sql += ctes[i].first + " AS (" + ctes[i].second + ")";
        }
        return sql + "\n" + select;
    }
};

class Interpreter {
    duckdb_connection conn;
    bool verbose;
    std::unordered_map<std::string, FnStmt> functions;
    std::vector<std::string> file_stack;   // for relative import resolution
    int current_line = 0;                  // updated in execBlock, used in errors
    int fn_depth = 0;                          // incremented on each execFn entry
    std::vector<std::vector<std::string>> val_scopes = {{}};  // per-scope list of __val_ tables to drop on exit

public:
    Interpreter(duckdb_connection c, bool verbose = false);
    // source_file is used to resolve relative imports; pass the script path from main.
    void run(const std::vector<ASTPtr>& prog, const std::string& source_file = "");

private:
    using Env = std::unordered_map<std::string,std::string>;

    void execBlock(const std::vector<ASTPtr>& block, Env env);
    FnResult execFn(const FnStmt& fn, Env env, const std::vector<std::string>& args = {});

    void exec(const LetStmt& s, Env& env);
    void exec(const ValStmt& s, Env& env);
    void exec(const ForStmt& s, Env& env);
    void exec(const IfStmt& s, Env& env);
    void exec(const WhileStmt& s, Env& env);
    void exec(const ExpectStmt& s, Env& env);
    void exec(const FnStmt& s, Env& env);
    void exec(const SQLStmt& s, Env& env);
    void exec(const PrintStmt& s, Env& env);
    void exec(const ImportStmt& s, Env& env);

    bool evalCond(std::string cond, Env& env);
    bool isFunctionCall(const std::string& sql, std::string& fn_name, std::vector<std::string>& args);
    void printResult(duckdb_result* res);

    // Apply env substitution + env var expansion in one call
    std::string resolve(const std::string& sql, const Env& env);

    // Format "filename:line: " prefix for error messages.
    // Works for both the root script and imported files.
    std::string loc() const;

    // Run a DuckDB query and return the error string on failure, "" on success.
    // Always pass a result so we can retrieve the error message.
    std::string dbExec(const std::string& sql, duckdb_result* res = nullptr);
};