#pragma once
#include "ast.h"
#include "duckdb.h"
#include <unordered_map>
#include <set>
#include <vector>
#include <string>

// Returned by execFn — a self-contained CTE chain + final SELECT.
struct FnResult {
    std::vector<std::pair<std::string,std::string>> ctes;
    std::string select;
    std::vector<std::string> vars_to_reset;

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

    // ── progress ──────────────────────────────────────────────
    bool show_progress  = false;
    bool progress_is_tty = false;   // true → human bar, false → machine lines
    int  total_stmts    = 0;        // set in run() from AST size
    int  current_stmt   = 0;        // incremented in execBlock (top-level only)
    bool progress_line_active = false;  // true if \r line is on stderr, needs clearing

    // Emit a progress event.
    // Human (tty):    \r[=====>   ] 12/47 label
    // Machine (!tty): PROGRESS 12/47 label\n
    void emitProgress(const std::string& label, int cur = -1, int tot = -1);

    // Clear the current \r progress line before writing errors/warnings.
    // No-op in machine mode or when no line is active.
    void clearProgressLine();

    // ── state ─────────────────────────────────────────────────
    std::unordered_map<std::string, FnStmt> functions;
    std::unordered_map<std::string, std::string> projections;
    std::vector<std::string> file_stack;
    int current_line = 0;
    int fn_depth     = 0;
    std::set<std::string> known_tables;
    std::vector<std::vector<std::string>> val_scopes = {{}};

public:
    Interpreter(duckdb_connection c, bool verbose = false, bool progress = false);
    void run(const std::vector<ASTPtr>& prog, const std::string& source_file = "");

private:
    using Env    = std::unordered_map<std::string,std::string>;
    using RawEnv = std::unordered_map<std::string,std::string>;

    void execBlock(const std::vector<ASTPtr>& block, Env env, RawEnv raw = {},
                   bool top_level = false);
    FnResult execFn(const FnStmt& fn, Env env,
                    const std::vector<std::string>& args = {}, RawEnv raw = {});

    void exec(const LetStmt& s,        Env& env, RawEnv& raw);
    void exec(const ValStmt& s,        Env& env, RawEnv& raw);
    void exec(const ForStmt& s,        Env& env, RawEnv& raw);
    void exec(const IfStmt& s,         Env& env, RawEnv& raw);
    void exec(const WhileStmt& s,      Env& env, RawEnv& raw);
    void exec(const ExpectStmt& s,     Env& env, RawEnv& raw);
    void exec(const FnStmt& s,         Env& env, RawEnv& raw);
    void exec(const SQLStmt& s,        Env& env, RawEnv& raw);
    void exec(const PrintStmt& s,      Env& env, RawEnv& raw);
    void exec(const ImportStmt& s,     Env& env, RawEnv& raw);
    void exec(const ProjectionStmt& s, Env& env, RawEnv& raw);

    bool evalCond(std::string cond, Env& env);
    bool isFunctionCall(const std::string& sql, std::string& fn_name,
                        std::vector<std::string>& args);
    void printResult(duckdb_result* res);
    std::string resolve(const std::string& sql, const Env& env, const RawEnv& raw = {});
    std::string inferFrom(const std::string& expr);
    std::string loc() const;
    std::string dbExec(const std::string& sql, duckdb_result* res = nullptr);
};