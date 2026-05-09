#include "interpreter.h"
#include "utils.h"
#include "parser.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unistd.h>

// ====================== ANSI COLORS ======================
// Only emit color codes when stderr is a real terminal.
// Piped output (logs, CI) stays clean.
namespace {
    bool colors_on() {
        static bool v = isatty(STDERR_FILENO);
        return v;
    }
    const char* RED    = "\033[1;31m";
    const char* YELLOW = "\033[1;33m";
    const char* DIM    = "\033[2m";
    const char* RESET  = "\033[0m";

    std::string red   (const std::string& s) { return colors_on() ? RED    + s + RESET : s; }
    std::string yellow(const std::string& s) { return colors_on() ? YELLOW + s + RESET : s; }
    std::string dim   (const std::string& s) { return colors_on() ? DIM    + s + RESET : s; }
}

Interpreter::Interpreter(duckdb_connection c, bool verbose) : conn(c), verbose(verbose) {}

// ====================== LOC + DBEXEC ======================

std::string Interpreter::loc() const {
    std::string file = file_stack.empty() ? "<unknown>" : file_stack.back();
    // Shorten to just the filename for readability
    auto slash = file.rfind('/');
    if (slash != std::string::npos) file = file.substr(slash + 1);
    return file + ":" + std::to_string(current_line) + ": ";
}

// Run a query and return the DuckDB error string on failure, "" on success.
// If the caller passes a result pointer it gets populated (for SELECT etc.);
// if nullptr we use a local result just to capture the error then destroy it.
std::string Interpreter::dbExec(const std::string& sql, duckdb_result* out) {
    duckdb_result local;
    duckdb_result* res = out ? out : &local;

    duckdb_state state = duckdb_query(conn, sql.c_str(), res);
    std::string err;
    if (state == DuckDBError) {
        const char* msg = duckdb_result_error(res);
        err = msg ? msg : "unknown error";
    }
    if (!out) duckdb_destroy_result(&local);
    return err;
}

// ====================== RUN / EXECBLOCK ======================

void Interpreter::run(const std::vector<ASTPtr>& prog, const std::string& source_file) {
    if (!source_file.empty()) {
        file_stack.push_back(std::filesystem::absolute(source_file).string());
    }
    execBlock(prog, {});
    if (!source_file.empty()) {
        file_stack.pop_back();
    }
}

void Interpreter::execBlock(const std::vector<ASTPtr>& block, Env env) {
    for (auto& n : block) {
        current_line = n->line_no;  // keep loc() current
        std::visit([&](auto&& x){ exec(x, env); }, n->node);
    }
}

// ====================== HELPERS ======================

std::string Interpreter::resolve(const std::string& sql, const Env& env) {
    return replaceEnvVars(replaceAll(sql, env));
}

bool Interpreter::isFunctionCall(const std::string& sql, std::string& fn_name,
                                  std::vector<std::string>& args) {
    std::string t = trim(sql);
    if (t.empty()) return false;
    if (t.back() == ';') t.pop_back();
    t = trim(t);

    size_t lparen = t.find('(');
    if (lparen == std::string::npos) return false;

    size_t rparen = t.rfind(')');
    if (rparen == std::string::npos || rparen < lparen) return false;
    if (!trim(t.substr(rparen + 1)).empty()) return false;

    fn_name = trim(t.substr(0, lparen));
    if (fn_name.empty()) return false;
    for (char c : fn_name) {
        if (!std::isalnum((unsigned char)c) && c != '_') return false;
    }

    // Split arguments by top-level commas (respecting nested parens and quotes).
    std::string inner = t.substr(lparen + 1, rparen - lparen - 1);
    args.clear();
    if (!trim(inner).empty()) {
        int depth = 0;
        bool in_single = false, in_double = false;
        std::string current;
        for (char c : inner) {
            if (c == '\'' && !in_double) { in_single = !in_single; current += c; }
            else if (c == '"' && !in_single) { in_double = !in_double; current += c; }
            else if (!in_single && !in_double) {
                if (c == '(') { depth++; current += c; }
                else if (c == ')') { depth--; current += c; }
                else if (c == ',' && depth == 0) {
                    args.push_back(trim(current));
                    current.clear();
                } else { current += c; }
            } else { current += c; }
        }
        if (!trim(current).empty()) args.push_back(trim(current));
    }
    return true;
}

bool Interpreter::evalCond(std::string cond, Env& env) {
    cond = resolve(cond, env);

    // Build a normalised SELECT from the condition expression.
    // Three cases:
    //   1. Already a full query: "SELECT COUNT(*) > 0 FROM orders" → use as-is
    //   2. Has a FROM clause but no SELECT: "COUNT(*) > 0 FROM orders" → prepend SELECT
    //   3. Pure expression: "total > 500" → wrap in SELECT (...)
    std::string upper = cond;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c){ return std::toupper(c); });

    std::string normalised;
    if (upper.rfind("SELECT", 0) == 0 || upper.rfind("WITH", 0) == 0) {
        normalised = cond;                      // already a full query
    } else if (upper.find(" FROM ") != std::string::npos) {
        normalised = "SELECT " + cond;          // has FROM, just needs SELECT
    } else {
        normalised = "SELECT (" + cond + ")";   // pure expression
    }

    std::string query = "SELECT 1 FROM (" + normalised + ") AS _cond_src WHERE (" +
                        normalised + ") IS TRUE LIMIT 1";

    // Simpler single-evaluation form using the normalised query as a subquery:
    query = "SELECT 1 FROM (SELECT (" + normalised + ") AS _cond) WHERE _cond IS TRUE LIMIT 1";

    duckdb_result res;
    std::string err = dbExec(query, &res);
    if (!err.empty()) {
        std::cerr << red("error: ") << loc() << "condition: " << err << "\n";
        duckdb_destroy_result(&res);
        return false;
    }
    bool ok = (duckdb_row_count(&res) > 0);
    duckdb_destroy_result(&res);
    return ok;
}

void Interpreter::printResult(duckdb_result* res) {
    idx_t cols = duckdb_column_count(res);
    idx_t rows = duckdb_row_count(res);
    if (cols == 0) return;

    for (idx_t c = 0; c < cols; c++) {
        printf("%-20s", duckdb_column_name(res, c));
        if (c < cols-1) printf(" | ");
    }
    printf("\n");
    for (idx_t c = 0; c < cols; c++) {
        printf("--------------------");
        if (c < cols-1) printf("-+-");
    }
    printf("\n");
    for (idx_t r = 0; r < rows; r++) {
        for (idx_t c = 0; c < cols; c++) {
            char* val = duckdb_value_varchar(res, c, r);
            printf("%-20s", val ? val : "NULL");
            if (val) duckdb_free(val);
            if (c < cols-1) printf(" | ");
        }
        printf("\n");
    }
    printf("\n");
}

// ====================== FUNCTION DEFINITION ======================

void Interpreter::exec(const FnStmt& s, Env& env) {
    functions[s.name] = s;
    if (verbose) std::cout << dim("✓ defined fn " + s.name + "()") << "\n";
}

// ====================== FUNCTION EXECUTION ======================
//
// Functions never touch the database during execution.
// - let x = ...   → accumulated as CTE (lazy, no temp table)
// - SELECT ...     → captured as the return value (last one wins)
// - everything else (INSERT, UPDATE, CREATE...) → side effect, runs immediately
//
// Nested function calls (let x = inner()) have their CTEs merged in,
// producing a flat single WITH chain in the returned FnResult.

FnResult Interpreter::execFn(const FnStmt& fn, Env env,
                              const std::vector<std::string>& args) {
    FnResult result;

    fn_depth++;
    val_scopes.push_back({});

    // Validate arity up front
    if (args.size() != fn.params.size()) {
        std::cerr << red("error: ") << loc() << "fn " << fn.name
                  << " expects " << fn.params.size()
                  << " args, got " << args.size() << "\n";
        if (args.size() < fn.params.size()) {
            val_scopes.pop_back(); fn_depth--;
            return result;
        }
    }

    // Bind arguments to parameter names via SET VARIABLE.
    // Two important subtleties:
    //
    // 1. DEFERRED RESET: param variables are added to result.vars_to_reset
    //    rather than val_scopes, so they survive until the caller has finished
    //    materializing the returned CTE chain. The caller resets them after.
    //
    // 2. NO DOUBLE-SUBSTITUTION: args[i] may already be a getvariable() call
    //    (when an outer-scope val is passed as an arg). We detect this and pass
    //    it through directly instead of running resolve() which would match the
    //    variable name inside the string literal and double-wrap it.
    for (size_t i = 0; i < fn.params.size() && i < args.size(); i++) {
        const std::string& param = fn.params[i];
        const std::string& raw_arg = args[i];

        // If the arg is already a getvariable() reference from an outer val,
        // use it directly — no resolve() to avoid matching inside the string.
        std::string expr;
        std::string trimmed = trim(raw_arg);
        if (trimmed.rfind("getvariable(", 0) == 0) {
            expr = trimmed;  // already resolved, pass through as-is
        } else {
            expr = resolve(raw_arg, env);  // substitute outer-scope vars
        }

        std::string upper = expr;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c){ return std::toupper(c); });
        std::string value_expr;
        if (upper.rfind("SELECT", 0) == 0 || upper.rfind("WITH", 0) == 0) {
            value_expr = "(" + expr + ")";
        } else if (upper.find(" FROM ") != std::string::npos) {
            value_expr = "(SELECT " + expr + ")";
        } else if (upper.rfind("GETVARIABLE(", 0) == 0) {
            value_expr = "(" + expr + ")";  // already a scalar call
        } else {
            value_expr = "(SELECT (" + expr + "))";
        }

        std::string varname = "__val_" + std::to_string(fn_depth) + "_" + param;
        std::string err = dbExec("SET VARIABLE " + varname + " = " + value_expr);
        if (!err.empty()) {
            std::cerr << red("error: ") << loc()
                      << "fn " << fn.name << " param " << param << ": " << err << "\n";
        } else {
            result.vars_to_reset.push_back(varname);  // defer, not val_scopes
            env[param] = "getvariable('" + varname + "')";
            if (verbose) std::cout << dim("  param " + param + " = " + raw_arg) << "\n";
        }
    }

    for (auto& n : fn.body) {
        current_line = n->line_no;

        if (auto* let = std::get_if<LetStmt>(&n->node)) {
            std::string sql = trim(resolve(let->sql, env));

            std::string inner_fn; std::vector<std::string> inner_args;
            if (isFunctionCall(sql, inner_fn, inner_args) && functions.count(inner_fn)) {
                FnResult inner = execFn(functions[inner_fn], env, inner_args);
                for (auto& cte : inner.ctes) result.ctes.push_back(cte);
                if (!inner.select.empty())
                    result.ctes.push_back({let->name, inner.select});
            } else {
                result.ctes.push_back({let->name, sql});
            }

        } else if (auto* val = std::get_if<ValStmt>(&n->node)) {
            // val inside a function: evaluate immediately and inject into env
            // so subsequent CTEs in this function can reference it via {{name}}
            exec(*val, env);

        } else if (auto* stmt = std::get_if<SQLStmt>(&n->node)) {
            std::string sql = trim(resolve(stmt->sql, env));
            std::string upper = sql;
            std::transform(upper.begin(), upper.end(), upper.begin(),
                           [](unsigned char c){ return std::toupper(c); });

            if (upper.rfind("SELECT", 0) == 0) {
                result.select = sql;
            } else {
                exec(*stmt, env);
            }
        } else {
            std::visit([&](auto&& x){ exec(x, env); }, n->node);
        }
    }

    // Reset all DuckDB variables created in this function scope.
    for (auto& v : val_scopes.back()) {
        dbExec("RESET VARIABLE " + v);
        if (verbose) std::cout << dim("✓ reset " + v) << "\n";
    }
    val_scopes.pop_back();
    fn_depth--;

    return result;
}

// ====================== LET ======================

void Interpreter::exec(const LetStmt& s, Env& env) {
    std::string sql = trim(resolve(s.sql, env));

    std::string fn_name; std::vector<std::string> fn_args;
    if (isFunctionCall(sql, fn_name, fn_args) && functions.count(fn_name)) {
        FnResult r = execFn(functions[fn_name], env, fn_args);
        std::string built = r.build();
        if (!built.empty()) {
            std::string full = "CREATE OR REPLACE TEMP TABLE " + s.name + " AS (" + built + ")";
            std::string err = dbExec(full);
            for (auto& v : r.vars_to_reset) dbExec("RESET VARIABLE " + v);
            if (!err.empty()) {
                std::cerr << red("error: ") << loc() << "let " << s.name << " = " << fn_name << "(): " << err << "\n";
                return;
            }
            if (verbose) std::cout << dim("✓ let " + s.name + " = " + fn_name + "()") << "\n";
        }
        return;
    }

    std::string full = "CREATE OR REPLACE TEMP TABLE " + s.name + " AS " + sql;
    std::string err = dbExec(full);
    if (!err.empty()) {
        std::cerr << red("error: ") << loc() << "let " << s.name << ": " << err << "\n";
        return;
    }
    if (verbose) std::cout << dim("✓ let " + s.name) << "\n";
}


// ====================== VAL / SCALAR ======================
// Evaluates the expression via DuckDB (wrapping in SELECT if needed),
// extracts the first cell, and stores the result as a plain string in env.
// Subsequent statements in the same block see it via {{name}} or bare name.

void Interpreter::exec(const ValStmt& s, Env& env) {
    std::string expr = trim(resolve(s.expr, env));

    // Use DuckDB SET VARIABLE so the value is stored with its original type
    // (DATE, DECIMAL, INTERVAL, etc.) and can be referenced anywhere via
    // getvariable('name') — including table macro parameters and extension
    // function arguments where subqueries are not allowed.
    //
    // Bare expressions (42, CURRENT_DATE - INTERVAL 30 DAYS) are wrapped in
    // SELECT so DuckDB evaluates them before storing.
    std::string upper = expr;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    // Three cases — mirrors evalCond normalisation:
    //   "SELECT ..." / "WITH ..."   → already a full query, use as-is
    //   "expr FROM table"           → has FROM, just prepend SELECT
    //   "42" / "CURRENT_DATE - ..." → pure expression, wrap in SELECT (...)
    std::string value_expr;
    if (upper.rfind("SELECT", 0) == 0 || upper.rfind("WITH", 0) == 0) {
        value_expr = "(" + expr + ")";
    } else if (upper.find(" FROM ") != std::string::npos) {
        value_expr = "(SELECT " + expr + ")";
    } else {
        value_expr = "(SELECT (" + expr + "))";
    }

    // Scope-prefix the variable name so function-local vals never collide
    // with same-named vals at outer scopes.
    std::string varname = "__val_" + std::to_string(fn_depth) + "_" + s.name;
    std::string set_sql = "SET VARIABLE " + varname + " = " + value_expr;
    std::string err = dbExec(set_sql);
    if (!err.empty()) {
        std::cerr << red("error: ") << loc() << "val " << s.name << ": " << err << "\n";
        return;
    }

    // Register so scope exit can NULL it out (DuckDB has no DROP VARIABLE).
    val_scopes.back().push_back(varname);

    // Inject as getvariable() — a plain scalar function call that works
    // everywhere, including as a macro/extension parameter.
    env[s.name] = "getvariable('" + varname + "')";

    if (verbose) {
        // Use CAST to VARCHAR so any type — including arrays, structs, maps —
        // renders correctly. duckdb_value_varchar only handles scalar types.
        duckdb_result res;
        dbExec("SELECT CAST(getvariable('" + varname + "') AS VARCHAR)", &res);
        char* v = duckdb_value_varchar(&res, 0, 0);
        std::cout << dim("✓ val " + s.name + " = " + (v ? v : "NULL")) << "\n";
        if (v) duckdb_free(v);
        duckdb_destroy_result(&res);
    }
}

// ====================== FOR =======================

void Interpreter::exec(const ForStmt& s, Env& env) {
    // Support both bare table names and inline queries as source.
    std::string source = trim(s.source);
    std::string upper = source;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    std::string q = (upper.rfind("SELECT", 0) == 0 || upper.rfind("WITH", 0) == 0)
        ? source
        : "SELECT * FROM " + source;

    duckdb_result res;
    std::string err = dbExec(q, &res);
    if (!err.empty()) {
        std::cerr << red("error: ") << loc() << "for " << s.var << " in " << s.source << ": " << err << "\n";
        return;
    }

    idx_t rows = duckdb_row_count(&res);
    idx_t cols = duckdb_column_count(&res);
    if (verbose) std::cout << dim("→ for " + s.var) << "\n";

    for (idx_t r = 0; r < rows; r++) {
        Env local = env;
        for (idx_t c = 0; c < cols; c++) {
            char* val = duckdb_value_varchar(&res, c, r);
            local[s.var + "." + duckdb_column_name(&res, c)] = val ? val : "NULL";
            if (val) duckdb_free(val);
        }
        execBlock(s.body, local);
    }
    duckdb_destroy_result(&res);
}

// ====================== IF / WHILE ======================

void Interpreter::exec(const IfStmt& s, Env& env) {
    if (evalCond(s.cond, env)) {
        execBlock(s.thenb, env);
    } else {
        execBlock(s.elseb, env);
    }
}

void Interpreter::exec(const WhileStmt& s, Env& env) {
    if (verbose) std::cout << dim("→ while") << "\n";
    while (evalCond(s.cond, env)) {
        execBlock(s.body, env);
    }
}

// ====================== EXPECT ======================

void Interpreter::exec(const ExpectStmt& s, Env& env) {
    if (!evalCond(s.condition, env)) {
        std::string msg = s.message.empty()
            ? "expectation failed: " + s.condition
            : s.message;

        if (s.action == "fail") {
            std::cerr << red("❌ fail: ") << loc() << msg << "\n";
            std::exit(1);
        } else {
            std::cerr << yellow("⚠️  warn: ") << loc() << msg << "\n";
        }
    }
}

// ====================== PRINT ======================

void Interpreter::exec(const PrintStmt& s, Env& env) {
    std::string text = trim(resolve(s.text, env));

    std::string upper = text;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c){ return std::toupper(c); });

    // Multi-row SELECT: print as a table.
    if (upper.rfind("SELECT", 0) == 0 || upper.rfind("WITH", 0) == 0) {
        duckdb_result res;
        std::string err = dbExec(text, &res);
        if (!err.empty()) {
            std::cerr << red("error: ") << loc() << "print: " << err << "\n";
            duckdb_destroy_result(&res);
            return;
        }
        idx_t rows = duckdb_row_count(&res);
        idx_t cols = duckdb_column_count(&res);
        if (rows == 1 && cols == 1) {
            // Single cell: print as plain value, not a table.
            char* val = duckdb_value_varchar(&res, 0, 0);
            if (val) { std::cout << val << "\n"; duckdb_free(val); }
        } else if (rows > 0) {
            printResult(&res);
        }
        duckdb_destroy_result(&res);
        return;
    }

    // Expression (may contain scalar subquery fragments from val substitution):
    // wrap in SELECT (...) so DuckDB evaluates it with proper types.
    // e.g. print 'total: ' || order_count
    //   →  SELECT ('total: ' || getvariable('__val_0_order_count'))
    {
        duckdb_result res;
        // Wrap in CAST(... AS VARCHAR) so arrays, structs, maps all render.
        std::string err = dbExec("SELECT CAST((" + text + ") AS VARCHAR)", &res);
        if (!err.empty()) {
            // Not a SQL expression — just print the raw text.
            std::cout << text << "\n";
            return;
        }
        if (duckdb_row_count(&res) > 0 && duckdb_column_count(&res) > 0) {
            char* val = duckdb_value_varchar(&res, 0, 0);
            if (val) { std::cout << val << "\n"; duckdb_free(val); }
        }
        duckdb_destroy_result(&res);
    }
}

// ====================== IMPORT ======================

void Interpreter::exec(const ImportStmt& s, Env& env) {
    std::filesystem::path base = file_stack.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(file_stack.back()).parent_path();

    std::filesystem::path filepath = std::filesystem::weakly_canonical(base / s.filename);

    if (!std::filesystem::exists(filepath)) {
        std::cerr << red("error: ") << loc() << "cannot open import '" << filepath.string() << "'\n";
        return;
    }

    std::ifstream f(filepath);
    std::stringstream buf;
    buf << f.rdbuf();

    file_stack.push_back(filepath.string());

    Parser subparser(buf.str());
    auto subast = subparser.parseBlock();
    execBlock(subast, env);

    file_stack.pop_back();
    if (verbose) std::cout << dim("✓ imported " + filepath.string()) << "\n";
}

// ====================== SQL ======================

void Interpreter::exec(const SQLStmt& s, Env& env) {
    std::string sql = trim(resolve(s.sql, env));
    if (sql.empty()) return;

    // Bare table name shorthand: a single plain identifier (letters, digits, _)
    // with no spaces is treated as SELECT * FROM name. Lets you write:
    //   let result = fn()
    //   result          ← prints the table
    {
        std::string t = sql;
        if (!t.empty() && t.back() == ';') t.pop_back();
        t = trim(t);
        bool is_ident = !t.empty();
        for (char c : t) {
            if (!std::isalnum((unsigned char)c) && c != '_') { is_ident = false; break; }
        }
        if (is_ident) sql = "SELECT * FROM " + t;
    }

    // Standalone function call
    std::string fn_name; std::vector<std::string> fn_args;
    if (isFunctionCall(sql, fn_name, fn_args) && functions.count(fn_name)) {
        if (verbose) std::cout << dim("→ calling " + fn_name + "()") << "\n";
        FnResult r = execFn(functions[fn_name], env, fn_args);
        std::string built = r.build();
        if (built.empty()) return;

        if (!s.redirect_file.empty()) {
            std::string copy_sql = "COPY (" + built + ") TO '" + s.redirect_file +
                                   "' (FORMAT CSV, HEADER" + (s.append ? ", APPEND" : "") + ")";
            std::string err = dbExec(copy_sql);
            for (auto& v : r.vars_to_reset) dbExec("RESET VARIABLE " + v);
            if (!err.empty())
                std::cerr << red("error: ") << loc() << "export " << fn_name << "(): " << err << "\n";
            else if (verbose)
                std::cout << dim("→ exported to " + s.redirect_file) << "\n";
        } else {
            duckdb_result res;
            std::string err = dbExec(built, &res);
            for (auto& v : r.vars_to_reset) dbExec("RESET VARIABLE " + v);
            if (!err.empty())
                std::cerr << red("error: ") << loc() << fn_name << "(): " << err << "\n";
            else
                printResult(&res);
            duckdb_destroy_result(&res);
        }
        return;
    }

    // Redirect to file
    if (!s.redirect_file.empty()) {
        std::string copy_sql = "COPY (" + sql + ") TO '" + s.redirect_file +
                               "' (FORMAT CSV, HEADER" + (s.append ? ", APPEND" : "") + ")";
        std::string err = dbExec(copy_sql);
        if (!err.empty())
            std::cerr << red("error: ") << loc() << "export: " << err << "\n";
        else if (verbose)
            std::cout << dim("→ exported to " + s.redirect_file) << "\n";
        return;
    }

    // Normal SQL
    duckdb_result res;
    std::string err = dbExec(sql, &res);
    if (!err.empty()) {
        std::cerr << red("error: ") << loc() << err << "\n";
        duckdb_destroy_result(&res);
        return;
    }

    std::string up = sql;
    std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c){ return std::toupper(c); });
    bool is_query = up.rfind("SELECT", 0) == 0 || up.rfind("WITH", 0) == 0;

    if (is_query && duckdb_column_count(&res) > 0) {
        printResult(&res);
    }
    duckdb_destroy_result(&res);
}