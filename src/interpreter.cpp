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

Interpreter::Interpreter(duckdb_connection c, bool verbose, bool progress)
    : conn(c), verbose(verbose), show_progress(progress),
      progress_is_tty(progress && isatty(STDERR_FILENO)) {}


// ====================== PROGRESS ======================

void Interpreter::clearProgressLine() {
    if (!show_progress || !progress_is_tty || !progress_line_active) return;
    std::cerr << "\r\033[2K";  // carriage return + erase line
    progress_line_active = false;
}

void Interpreter::emitProgress(const std::string& label, int cur, int tot) {
    if (!show_progress) return;

    int s_cur = (cur >= 0) ? cur : current_stmt;
    int s_tot = (tot >= 0) ? tot : total_stmts;

    if (progress_is_tty) {
        // Human mode: animated bar that overwrites itself
        const int bar_width = 24;
        int filled = (s_tot > 0) ? (s_cur * bar_width / s_tot) : 0;
        std::string bar(filled, '=');
        if (filled < bar_width) { bar += '>'; bar += std::string(bar_width - filled - 1, ' '); }

        // Truncate label to fit terminal
        std::string lbl = label.size() > 36 ? label.substr(0, 35) + "…" : label;

        std::cerr << "\r\033[2K"   // erase line
                  << "\033[2m["    // dim
                  << bar << "] "
                  << s_cur << "/" << s_tot
                  << " " << lbl
                  << "\033[0m"
                  << std::flush;
        progress_line_active = true;
    } else {
        // Machine mode: structured lines on stderr, one per event
        std::cerr << "PROGRESS " << s_cur << "/" << s_tot;
        if (!label.empty()) std::cerr << " " << label;
        if (cur >= 0 && tot > 0) std::cerr << " (" << cur << "/" << tot << ")";
        std::cerr << "\n";
    }
}

// ====================== LOC + DBEXEC =======================

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
    total_stmts = (int)prog.size();
    current_stmt = 0;
    execBlock(prog, {}, {}, true);  // top_level=true
    if (show_progress) {
        clearProgressLine();
        if (!progress_is_tty) std::cerr << "PROGRESS DONE\n";
    }
    flushLog();
    if (!source_file.empty()) {
        file_stack.pop_back();
    }
}

void Interpreter::execBlock(const std::vector<ASTPtr>& block, Env env, RawEnv raw, bool top_level) {
    for (auto& n : block) {
        current_line = n->line_no;  // keep loc() current

        if (top_level && show_progress) {
            current_stmt++;
            std::string label = std::visit([](auto&& x) -> std::string {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, LetStmt>)        return "let " + x.name;
                if constexpr (std::is_same_v<T, ValStmt>)        return "val " + x.name;
                if constexpr (std::is_same_v<T, FnStmt>)         return "fn " + x.name + "()";
                if constexpr (std::is_same_v<T, ForStmt>)        return "for " + x.var;
                if constexpr (std::is_same_v<T, WhileStmt>)      return "while";
                if constexpr (std::is_same_v<T, IfStmt>)         return "if";
                if constexpr (std::is_same_v<T, ExpectStmt>)     return "check";
                if constexpr (std::is_same_v<T, PrintStmt>)      return "print";
                if constexpr (std::is_same_v<T, LogStmt>)        return "log";
                if constexpr (std::is_same_v<T, ImportStmt>)     return "import " + x.filename;
                if constexpr (std::is_same_v<T, ProjectionStmt>) return "projection " + x.name;
                if constexpr (std::is_same_v<T, ArrLetStmt>)         return x.name + " +=";
                if constexpr (std::is_same_v<T, BreakStmt>)          return "break";
                if constexpr (std::is_same_v<T, ContinueStmt>)       return "continue";

                if constexpr (std::is_same_v<T, SQLStmt>) {
                    std::string s = x.sql.substr(0, 40);
                    if (x.sql.size() > 40) s += "…";
                    return s;
                }
                return "";
            }, n->node);
            emitProgress(label);
        }

        std::visit([&](auto&& x){ exec(x, env, raw); }, n->node);
        // Stop processing this block if break or continue was signalled
        if (loop_signal != LoopSignal::None) break;
    }
}

// ====================== HELPERS ======================

// Normalise a SQL expression to a full SELECT statement.
//
//   SELECT/WITH/DML  → pass through unchanged
//   (SELECT ...)     → SELECT * FROM (SELECT ...)  — subquery expression
//   FROM ...         → SELECT * FROM ...
//   expr FROM ...    → SELECT expr FROM ...
//   ident [clauses]  → SELECT * FROM ident [clauses]  (table shorthand)
//   bare_ident       → SELECT * FROM ident
//   literal/expr     → SELECT (expr)  — scalar
//
// SQL clause keywords (WHERE, ORDER, LIMIT, etc.) after a first identifier
// signal a table shorthand — the whole expression is a table source.
// scalar_context=true: bare idents wrap as SELECT (expr) — used for val/fn params
// scalar_context=false: bare idents become SELECT * FROM name — used for let/sql stmts
static std::string normaliseSQL(const std::string& sql, bool scalar_context = false) {
    if (sql.empty()) return sql;

    std::string trimmed = trim(sql);
    std::string up = trimmed;
    std::transform(up.begin(), up.end(), up.begin(),
                   [](unsigned char c){ return std::toupper(c); });

    // Pass through: already a full query
    if (up.rfind("SELECT", 0) == 0 || up.rfind("WITH", 0) == 0)
        return trimmed;

    // Pass through: DML — never wrap in SELECT
    if (up.rfind("CREATE", 0) == 0 || up.rfind("INSERT", 0) == 0 ||
        up.rfind("UPDATE", 0) == 0 || up.rfind("DELETE", 0) == 0 ||
        up.rfind("DROP",   0) == 0 || up.rfind("ALTER",  0) == 0 ||
        up.rfind("COPY",   0) == 0 || up.rfind("ATTACH", 0) == 0 ||
        up.rfind("DETACH", 0) == 0 || up.rfind("LOAD",   0) == 0 ||
        up.rfind("INSTALL",0) == 0 || up.rfind("SET ",   0) == 0)
        return trimmed;

    // Subquery expression: starts with ( — wrap as subquery source
    if (trimmed[0] == '(')
        return "SELECT * FROM " + trimmed;

    // Find top-level FROM (not inside parens or string literals)
    int depth = 0; bool in_s = false, in_d = false;
    size_t from_pos = std::string::npos;
    for (size_t i = 0; i < up.size(); i++) {
        char c = up[i];
        if (c == '\'' && !in_d) { in_s = !in_s; continue; }
        if (c == '"'  && !in_s) { in_d = !in_d; continue; }
        if (in_s || in_d) continue;
        if (c == '(') { depth++; continue; }
        if (c == ')') { depth--; continue; }
        if (depth == 0 && up.substr(i, 5) == "FROM " && (i == 0 || up[i-1] == ' ')) {
            from_pos = i; break;
        }
    }

    if (from_pos != std::string::npos) {
        if (from_pos == 0) return "SELECT * " + trimmed; // FROM first
        return "SELECT " + trimmed;                       // expr FROM table
    }

    // No FROM — determine if this is a table shorthand or a scalar expression.
    // SQL clause keywords after a first identifier signal a table shorthand:
    //   paid WHERE status = 'paid'  → SELECT * FROM paid WHERE status = 'paid'
    //   summary LIMIT 10            → SELECT * FROM summary LIMIT 10
    // Pure scalars/literals have no such keywords:
    //   100, CURRENT_DATE, total > 500  → SELECT (expr)
    static const char* table_clauses[] = {
        "WHERE ", "ORDER ", "GROUP ", "LIMIT ", "HAVING ",
        "JOIN ", "LEFT ", "RIGHT ", "INNER ", "OUTER ",
        "CROSS ", "FULL ", "UNION ", "EXCEPT ", "INTERSECT ",
        "OFFSET ", "QUALIFY ", "WINDOW ", nullptr
    };
    for (int i = 0; table_clauses[i]; i++) {
        // Must appear after at least one word (the table name)
        size_t kw_pos = up.find(table_clauses[i]);
        if (kw_pos != std::string::npos && kw_pos > 0 && up[kw_pos - 1] == ' ')
            return "SELECT * FROM " + trimmed;
    }

    // Known DuckDB table functions — always SELECT * FROM, never scalar.
    // These must be detected before the bare-ident check since they have parens.
    static const char* TABLE_FUNCS[] = {
        "read_csv(", "read_parquet(", "read_json(", "read_ndjson(",
        "range(", "generate_series(", "glob(", "scan_arrow(",
        "read_csv_auto(", "parquet_scan(", nullptr
    };
    for (int i = 0; TABLE_FUNCS[i]; i++) {
        std::string tf = TABLE_FUNCS[i];
        std::transform(tf.begin(), tf.end(), tf.begin(),
                       [](unsigned char c){ return std::toupper(c); });
        if (up.rfind(tf, 0) == 0) return "SELECT * FROM " + trimmed;
    }

    // Bare identifier: letters, digits, underscores only.
    // In scalar_context (val/fn params), treat as expression — SELECT (expr).
    // In table context (let/sql stmts), treat as table name — SELECT * FROM name.
    // Always scalar if starts with digit (numeric literal like 100, 3.14).
    bool is_bare = !trimmed.empty() && !std::isdigit((unsigned char)trimmed[0]);
    if (is_bare) {
        for (char c : trimmed)
            if (!std::isalnum((unsigned char)c) && c != '_') { is_bare = false; break; }
    }
    if (is_bare && !scalar_context) return "SELECT * FROM " + trimmed;

    // Scalar expression (or scalar_context with bare ident)
    return "SELECT (" + trimmed + ")";
}

std::string Interpreter::resolve(const std::string& sql, const Env& env, const RawEnv& raw) {
    // Pre-pass 1: expand ...projection_name into its column list.
    std::string s = sql;
    size_t p = 0;
    while ((p = s.find("...", p)) != std::string::npos) {
        size_t start = p + 3, end = start;
        while (end < s.size() && (std::isalnum((unsigned char)s[end]) || s[end] == '_')) end++;
        if (end > start) {
            std::string name = s.substr(start, end - start);
            auto it = projections.find(name);
            if (it != projections.end()) {
                s.replace(p, end - p, it->second);
                p += it->second.size();
                continue;
            }
        }
        p++;
    }

    // Pre-pass 2: expand array[index] references.
    // Handles: data[1], data[-1], data[some_scalar_val]
    // Resolves to the internal temp table name __arr_{name}_{n}.
    // Done before replaceAll so the result is a plain identifier DuckDB can use.
    for (const auto& [arr_name, items] : array_lets) {
        std::string pattern = arr_name + "[";
        size_t pos = 0;
        while ((pos = s.find(pattern, pos)) != std::string::npos) {
            // Verify left boundary — not part of a longer identifier
            bool left_ok = (pos == 0) ||
                           (!std::isalnum((unsigned char)s[pos-1]) && s[pos-1] != '_');
            if (!left_ok) { pos++; continue; }

            // Find matching closing bracket
            size_t bracket_start = pos + pattern.size();  // after "name["
            size_t bracket_end   = s.find(']', bracket_start);
            if (bracket_end == std::string::npos) { pos++; continue; }

            std::string index_expr = trim(s.substr(bracket_start, bracket_end - bracket_start));
            if (index_expr.empty()) { pos++; continue; }

            // Evaluate the index expression to an integer.
            // It may be a literal (-1, 2), a val name, or any scalar SQL expression.
            int idx = 0;
            {
                // Resolve env vars in the index expression first
                std::string resolved_idx = replaceEnvVars(replaceAll(index_expr, env, raw));
                // Execute as scalar SQL
                std::string idx_sql = "SELECT CAST((" + resolved_idx + ") AS INTEGER)";
                duckdb_result res;
                if (dbExec(idx_sql, &res).empty() && duckdb_row_count(&res) > 0) {
                    char* v = duckdb_value_varchar(&res, 0, 0);
                    if (v) { idx = std::stoi(v); duckdb_free(v); }
                }
                duckdb_destroy_result(&res);
            }

            // Convert to 1-based positive index
            int n = (int)items.size();
            if (idx < 0) idx = n + idx + 1;  // -1 → n, -2 → n-1, etc.

            std::string replacement;
            if (idx < 1 || idx > n) {
                clearProgressLine();
                std::cerr << red("error: ") << loc()
                          << arr_name << "[" << index_expr << "] out of range "
                          << "(have " << n << " item(s))\n";
                replacement = arr_name;  // leave as-is, DuckDB will error
            } else {
                replacement = items[idx - 1];  // items is 0-based internally
            }

            s.replace(pos, bracket_end - pos + 1, replacement);
            pos += replacement.size();
        }
    }

    return replaceEnvVars(replaceAll(s, env, raw));
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

// ====================== INFER FROM ======================
// If the expression has no FROM clause and no SELECT, look for the columns
// it references in the known_tables set. If all referenced columns resolve
// to exactly one table, append "FROM that_table" automatically.
// If ambiguous or nothing matches, return the expression unchanged —
// DuckDB will produce a clear error if it can't resolve it.

std::string Interpreter::inferFrom(const std::string& expr) {
    std::string upper = expr;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c){ return std::toupper(c); });

    // Only act on expressions with no FROM/SELECT already
    if (upper.find(" FROM ") != std::string::npos ||
        upper.rfind("SELECT", 0) == 0 ||
        upper.rfind("WITH", 0) == 0 ||
        known_tables.empty()) {
        return expr;
    }

    // Query information_schema for all columns across known tables.
    // Build: WHERE table_name IN ('t1','t2',...) AND table_schema = 'temp'
    std::string in_list;
    for (const auto& t : known_tables) {
        if (!in_list.empty()) in_list += ",";
        in_list += "'" + t + "'";
    }
    std::string schema_q =
        "SELECT table_name, column_name "
        "FROM information_schema.columns "
        "WHERE table_schema = 'temp' AND table_name IN (" + in_list + ") "
        "ORDER BY table_name, column_name";

    duckdb_result res;
    if (!dbExec(schema_q, &res).empty()) {
        duckdb_destroy_result(&res);
        return expr;
    }

    // Build map: column_name → list of tables that have it
    std::unordered_map<std::string, std::vector<std::string>> col_tables;
    idx_t rows = duckdb_row_count(&res);
    for (idx_t r = 0; r < rows; r++) {
        char* tbl = duckdb_value_varchar(&res, 0, r);
        char* col = duckdb_value_varchar(&res, 1, r);
        if (tbl && col) col_tables[col].push_back(tbl);
        if (tbl) duckdb_free(tbl);
        if (col) duckdb_free(col);
    }
    duckdb_destroy_result(&res);

    // Extract word tokens from the expression that could be column names.
    // For each recognised column, collect which known tables have it.
    // The candidate table set is the INTERSECTION across all found columns —
    // we want tables that have ALL the referenced columns, not any of them.
    std::set<std::string> candidate_tables;
    bool first_col = true;
    std::string tok;
    for (size_t i = 0; i <= expr.size(); i++) {
        char c = (i < expr.size()) ? expr[i] : 0;
        if (std::isalnum((unsigned char)c) || c == '_') {
            tok += c;
        } else {
            if (!tok.empty()) {
                std::string tu = tok;
                std::transform(tu.begin(), tu.end(), tu.begin(),
                               [](unsigned char c){ return std::toupper(c); });
                static const std::set<std::string> kws = {
                    "SELECT","FROM","WHERE","AND","OR","NOT","AS","IN","IS",
                    "NULL","TRUE","FALSE","BY","GROUP","ORDER","HAVING",
                    "LIMIT","OFFSET","CASE","WHEN","THEN","ELSE","END",
                    "COUNT","SUM","AVG","MIN","MAX","ROUND","CAST","OVER",
                    "DISTINCT","ALL","EXISTS","BETWEEN","LIKE","ILIKE"
                };
                if (kws.find(tu) == kws.end() && !std::isdigit((unsigned char)tok[0])) {
                    auto it = col_tables.find(tok);
                    if (it != col_tables.end()) {
                        std::set<std::string> col_set(it->second.begin(), it->second.end());
                        if (first_col) {
                            candidate_tables = col_set;
                            first_col = false;
                        } else {
                            // Intersect: keep only tables present in both sets
                            std::set<std::string> intersected;
                            for (const auto& t : candidate_tables) {
                                if (col_set.count(t)) intersected.insert(t);
                            }
                            candidate_tables = intersected;
                        }
                    }
                }
                tok.clear();
            }
        }
    }

    if (candidate_tables.size() == 1) {
        if (verbose) std::cout << dim("  inferred FROM " + *candidate_tables.begin()) << "\n";
        return expr + " FROM " + *candidate_tables.begin();
    }
    // Ambiguous or no match — return unchanged, let DuckDB error naturally
    return expr;
}

bool Interpreter::evalCond(std::string cond, Env& env, RawEnv& raw) {
    cond = resolve(cond, env, raw);

    // Normalise to a full SELECT (handles bare expressions, FROM-only, and full queries).
    // Then wrap in a subquery so the condition is evaluated exactly once.
    std::string normalised = normaliseSQL(inferFrom(cond));
    std::string query = "SELECT 1 FROM (SELECT (" + normalised + ") AS _cond) WHERE _cond IS TRUE LIMIT 1";

    duckdb_result res;
    std::string err = dbExec(query, &res);
    if (!err.empty()) {
        clearProgressLine(); std::cerr << red("error: ") << loc() << "condition: " << err << "\n";
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

    // duckdb_value_varchar returns NULL for complex types (arrays, structs, maps).
    // For those columns, fall back to a CAST ... AS VARCHAR re-execution so
    // arrays like INTEGER[] display as [1, 2, 3] instead of NULL.
    // We detect the need by trying the first row first.
    // Build a cast-select to re-execute if needed — used as a fallback per column.

    // Column headers
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
            if (val) {
                printf("%-20s", val);
                duckdb_free(val);
            } else {
                // duckdb_value_varchar returned NULL — complex type (array, struct, map).
                // Try duckdb_value_varchar with an explicit internal cast.
                // DuckDB C API: use duckdb_value_varchar which may work for some types,
                // or fall back to a type label so the user knows to CAST explicitly.
                duckdb_type col_type = duckdb_column_type(res, c);
                // Check if null vs complex type
                bool is_null = duckdb_value_is_null(res, c, r);
                if (is_null) {
                    printf("%-20s", "NULL");
                } else if (col_type == DUCKDB_TYPE_LIST || col_type == DUCKDB_TYPE_ARRAY) {
                    printf("%-20s", "<list>");
                } else if (col_type == DUCKDB_TYPE_STRUCT) {
                    printf("%-20s", "<struct>");
                } else if (col_type == DUCKDB_TYPE_MAP) {
                    printf("%-20s", "<map>");
                } else {
                    printf("%-20s", "?");
                }
            }
            if (c < cols-1) printf(" | ");
        }
        printf("\n");
    }
    printf("\n");
}



// ====================== BREAK / CONTINUE ======================

void Interpreter::exec(const BreakStmt&, Env&, RawEnv&) {
    // Signal the enclosing for/while to stop iterating.
    // execBlock checks this after each statement and stops processing.
    loop_signal = LoopSignal::Break;
}

void Interpreter::exec(const ContinueStmt&, Env&, RawEnv&) {
    // Signal the enclosing for/while to skip to the next iteration.
    loop_signal = LoopSignal::Continue;
}

// ====================== ARRAY LET =======================
//
// Array lets store an ordered list of temp tables named __arr_{name}_{n}.
// A TEMP VIEW named {name} is recreated on every append as:
//   SELECT * FROM __arr_{name}_1 UNION BY NAME
//   SELECT * FROM __arr_{name}_2 UNION BY NAME ...
//
// This means:
//   data += expr          creates __arr_data_N, rebuilds view
//   data[1]               → SELECT * FROM __arr_data_1
//   data[-1]              → SELECT * FROM __arr_data_{last}
//   data[scalar_val]      → resolves scalar, picks __arr_data_N
//   data                  → view — UNION BY NAME of all items
//   SELECT * FROM data    → works natively, no rewriting needed
//
// ArrLetStmt is emitted for both:
//   let data[] = []                  → name="data", expr=""  (init empty)
//   let data[] = [source1, source2]  → name="data", expr="source1, source2"
//   data += expr                     → name="data", expr="expr"  (append)

void Interpreter::exec(const ArrLetStmt& s, Env& env, RawEnv& raw) {
    // Split the expression: may be a comma-separated init list or a single expr.
    // We treat each comma-separated item as one source to append.
    std::vector<std::string> items;
    if (!s.expr.empty()) {
        // Simple split on top-level commas (respecting parens/quotes)
        int depth = 0; bool in_s = false, in_d = false;
        std::string cur;
        for (char c : s.expr) {
            if (c == '\'' && !in_d) { in_s = !in_s; cur += c; continue; }
            if (c == '"' && !in_s)  { in_d = !in_d; cur += c; continue; }
            if (in_s || in_d)        { cur += c; continue; }
            if (c == '(') { depth++; cur += c; continue; }
            if (c == ')') { depth--; cur += c; continue; }
            if (c == ',' && depth == 0) {
                std::string t = trim(cur);
                if (!t.empty()) items.push_back(t);
                cur.clear();
            } else { cur += c; }
        }
        std::string t = trim(cur);
        if (!t.empty()) items.push_back(t);
    }

    // Initialise the array entry if this is first use
    if (!array_lets.count(s.name)) {
        array_lets[s.name] = {};
        if (verbose) std::cout << dim("✓ array let " + s.name + " []") << "\n";
    }

    // Append each item
    for (const auto& item : items) {
        auto& arr = array_lets[s.name];
        int   idx = (int)arr.size() + 1;  // 1-based
        std::string tbl  = "__arr_" + s.name + "_" + std::to_string(idx);
        std::string expr = trim(resolve(item, env, raw));
        std::string norm = normaliseSQL(expr);

        std::string err = dbExec("CREATE OR REPLACE TEMP TABLE " + tbl + " AS " + norm);
        if (!err.empty()) {
            clearProgressLine();
            std::cerr << red("error: ") << loc()
                      << s.name << " += : " << err << "\n";
            return;
        }

        arr.push_back(tbl);
        known_tables.insert(tbl);
        if (verbose) std::cout << dim("✓ " + s.name + "[" + std::to_string(idx) + "] = " + item) << "\n";

        // Rebuild the TEMP VIEW so "SELECT * FROM data" always sees all items
        std::string view_sql = "CREATE OR REPLACE TEMP VIEW " + s.name + " AS ";
        for (size_t i = 0; i < arr.size(); i++) {
            if (i > 0) view_sql += "\n    UNION BY NAME ";
            view_sql += "SELECT * FROM " + arr[i];
        }
        view_sql += ";";
        err = dbExec(view_sql);
        if (!err.empty()) {
            clearProgressLine();
            std::cerr << red("error: ") << loc()
                      << "rebuilding view for " << s.name << ": " << err << "\n";
        }
    }
    if (verbose && items.empty())
        std::cout << dim("✓ " + s.name + " initialised (empty)") << "\n";
}

// ====================== FUNCTION DEFINITION =======================

void Interpreter::exec(const FnStmt& s, Env& env, RawEnv& raw) {
    functions[s.name] = s;
    if (verbose) std::cout << dim("✓ defined fn " + s.name + "()") << "\n";
}


// ====================== PROJECTION ======================

void Interpreter::exec(const ProjectionStmt& s, Env& env, RawEnv& raw) {
    // Resolve any {{vars}} in the column list at definition time.
    std::string cols = resolve(s.cols, env, raw);
    projections[s.name] = cols;
    if (verbose) std::cout << dim("✓ projection " + s.name) << "\n";
}

// ====================== FUNCTION EXECUTION =======================
//
// Functions never touch the database during execution.
// - let x = ...   → accumulated as CTE (lazy, no temp table)
// - SELECT ...     → captured as the return value (last one wins)
// - everything else (INSERT, UPDATE, CREATE...) → side effect, runs immediately
//
// Nested function calls (let x = inner()) have their CTEs merged in,
// producing a flat single WITH chain in the returned FnResult.

FnResult Interpreter::execFn(const FnStmt& fn, Env env,
                              const std::vector<std::string>& args, RawEnv raw) {
    FnResult result;

    fn_depth++;
    val_scopes.push_back({});

    // Validate arity up front
    if (args.size() != fn.params.size()) {
        clearProgressLine(); std::cerr << red("error: ") << loc() << "fn " << fn.name
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
            expr = resolve(raw_arg, env, raw);  // substitute outer-scope vars
        }

        // scalar_context=true: fn params are always scalar values, never table names
        std::string value_expr = "(" + normaliseSQL(expr, true) + ")";

        std::string varname = "__val_" + std::to_string(fn_depth) + "_" + param;
        std::string err = dbExec("SET VARIABLE " + varname + " = " + value_expr);
        if (!err.empty()) {
            clearProgressLine(); std::cerr << red("error: ") << loc()
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
            std::string sql = trim(resolve(let->sql, env, raw));

            std::string inner_fn; std::vector<std::string> inner_args;
            if (isFunctionCall(sql, inner_fn, inner_args) && functions.count(inner_fn)) {
                FnResult inner = execFn(functions[inner_fn], env, inner_args, raw);
                for (auto& cte : inner.ctes) result.ctes.push_back(cte);
                if (!inner.select.empty())
                    result.ctes.push_back({let->name, inner.select});
            } else {
                result.ctes.push_back({let->name, sql});
            }

        } else if (auto* val = std::get_if<ValStmt>(&n->node)) {
            // val inside a function: evaluate immediately and inject into env
            // so subsequent CTEs in this function can reference it via {{name}}
            exec(*val, env, raw);

        } else if (auto* stmt = std::get_if<SQLStmt>(&n->node)) {
            std::string sql = trim(resolve(stmt->sql, env, raw));
            std::string upper = sql;
            std::transform(upper.begin(), upper.end(), upper.begin(),
                           [](unsigned char c){ return std::toupper(c); });

            if (upper.rfind("SELECT", 0) == 0) {
                result.select = sql;
            } else {
                exec(*stmt, env, raw);
            }
        } else {
            std::visit([&](auto&& x){ exec(x, env, raw); }, n->node);
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

void Interpreter::exec(const LetStmt& s, Env& env, RawEnv& raw) {
    std::string sql = trim(resolve(s.sql, env, raw));

    std::string fn_name; std::vector<std::string> fn_args;
    if (isFunctionCall(sql, fn_name, fn_args) && functions.count(fn_name)) {
        FnResult r = execFn(functions[fn_name], env, fn_args, raw);
        std::string built = r.build();
        if (!built.empty()) {
            std::string full = "CREATE OR REPLACE TEMP TABLE " + s.name + " AS (" + built + ")";
            std::string err = dbExec(full);
            for (auto& v : r.vars_to_reset) dbExec("RESET VARIABLE " + v);
            if (!err.empty()) {
                clearProgressLine(); std::cerr << red("error: ") << loc() << "let " << s.name << " = " << fn_name << "(): " << err << "\n";
                return;
            }
            known_tables.insert(s.name);
            if (verbose) std::cout << dim("✓ let " + s.name + " = " + fn_name + "()") << "\n";
        }
        return;
    }

    sql = normaliseSQL(sql);
    std::string full = "CREATE OR REPLACE TEMP TABLE " + s.name + " AS " + sql;
    std::string err = dbExec(full);
    if (!err.empty()) {
        clearProgressLine(); std::cerr << red("error: ") << loc() << "let " << s.name << ": " << err << "\n";
        return;
    }
    known_tables.insert(s.name);
    if (verbose) std::cout << dim("✓ let " + s.name) << "\n";
}


// ====================== VAL / SCALAR ======================
// Evaluates the expression via DuckDB (wrapping in SELECT if needed),
// extracts the first cell, and stores the result as a plain string in env.
// Subsequent statements in the same block see it via {{name}} or bare name.

void Interpreter::exec(const ValStmt& s, Env& env, RawEnv& raw) {
    // Guard: val name must not clash with any column name in known tables.
    // A val named "amount" would silently shadow a column called "amount"
    // in every subsequent query — an error here is much safer.
    // Skip check on reassignment (env.count > 0) — already validated at first definition.
    if (!known_tables.empty() && !env.count(s.name)) {
        std::string in_list;
        for (const auto& t : known_tables) {
            if (!in_list.empty()) in_list += ",";
            in_list += "'" + t + "'";
        }
        std::string col_q =
            "SELECT table_name, column_name FROM information_schema.columns "
            "WHERE table_schema = 'temp' AND table_name IN (" + in_list + ") "
            "AND column_name = '" + s.name + "' LIMIT 1";
        duckdb_result col_res;
        if (dbExec(col_q, &col_res).empty() && duckdb_row_count(&col_res) > 0) {
            char* tbl = duckdb_value_varchar(&col_res, 0, 0);
            std::string tbl_name = tbl ? tbl : "?";
            if (tbl) duckdb_free(tbl);
            duckdb_destroy_result(&col_res);
            clearProgressLine();
            std::cerr << red("error: ") << loc()
                      << "val '" << s.name << "' conflicts with column '"
                      << s.name << "' in table '" << tbl_name
                      << "' — choose a different name\n";
            return;
        }
        duckdb_destroy_result(&col_res);
    }

    std::string expr = trim(resolve(s.expr, env, raw));

    // Use DuckDB SET VARIABLE so the value is stored with its original type
    // (DATE, DECIMAL, INTERVAL, etc.) and can be referenced anywhere via
    // getvariable('name') — including table macro parameters and extension
    // function arguments where subqueries are not allowed.
    //
    // Bare expressions (42, CURRENT_DATE - INTERVAL 30 DAYS) are wrapped in
    // SELECT so DuckDB evaluates them before storing.
    // Normalise to a full SELECT — handles bare expressions, FROM-only, and full queries.
    // scalar_context=true: never treat bare ident as table name for val
    std::string normalised = normaliseSQL(inferFrom(expr), true);
    std::string value_expr = "(" + normalised + ")";

    // Scope-prefix the variable name so function-local vals never collide
    // with same-named vals at outer scopes.
    std::string varname = "__val_" + std::to_string(fn_depth) + "_" + s.name;

    // Fetch raw string value by executing the expression directly — before SET VARIABLE
    // so raw[s.name] is always set even if SET VARIABLE fails.
    // Execute value_expr directly for reliability; avoids SET VARIABLE → getvariable round-trip.
    {
        duckdb_result raw_res;
        std::string raw_err = dbExec("SELECT CAST(" + value_expr + " AS VARCHAR)", &raw_res);
        if (raw_err.empty() && duckdb_row_count(&raw_res) > 0) {
            char* v = duckdb_value_varchar(&raw_res, 0, 0);
            raw[s.name] = v ? v : "";
            if (v) duckdb_free(v);
        } else {
            raw[s.name] = "";
        }
        duckdb_destroy_result(&raw_res);
    }

    std::string set_sql = "SET VARIABLE " + varname + " = " + value_expr;
    std::string err = dbExec(set_sql);
    if (!err.empty()) {
        clearProgressLine(); std::cerr << red("error: ") << loc() << "val " << s.name << ": " << err << "\n";
        return;
    }

    // Register so scope exit can NULL it out (DuckDB has no DROP VARIABLE).
    val_scopes.back().push_back(varname);

    // Inject as getvariable() — typed runtime reference for SQL expressions.
    env[s.name] = "getvariable('" + varname + "')";

    if (verbose) std::cout << dim("✓ val " + s.name + " = " + raw[s.name]) << "\n";
}

// ====================== FOR =======================

void Interpreter::exec(const ForStmt& s, Env& env, RawEnv& raw) {
    // Normalise source — bare table name, inline query, or full SELECT all work.
    // The resolve() is done here so {{vars}} in the source are expanded.
    std::string source = trim(resolve(s.source, env, raw));
    std::string q = normaliseSQL(source);

    duckdb_result res;
    std::string err = dbExec(q, &res);
    if (!err.empty()) {
        clearProgressLine(); std::cerr << red("error: ") << loc() << "for " << s.var << " in " << s.source << ": " << err << "\n";
        return;
    }

    idx_t rows = duckdb_row_count(&res);
    idx_t cols = duckdb_column_count(&res);
    if (verbose) std::cout << dim("→ for " + s.var) << "\n";

    for (idx_t r = 0; r < rows; r++) {
        if (show_progress && rows > 1) {
            emitProgress("for " + s.var, (int)r + 1, (int)rows);
        }
        Env    local     = env;
        RawEnv local_raw = raw;

        // Bind each column of the current row into local env/raw.
        // env  → getvariable() ref so values are SQL-typed (no quoting issues)
        // raw  → actual string for {{col}} template injection
        std::vector<std::string> row_vars;
        for (idx_t c = 0; c < cols; c++) {
            char* val = duckdb_value_varchar(&res, c, r);
            std::string col   = duckdb_column_name(&res, c);
            std::string value = val ? val : "";
            if (val) duckdb_free(val);

            std::string varname = "__val_" + std::to_string(fn_depth + 1) + "_" + s.var + "_" + col;
            std::string escaped;
            for (char ch : value) {
                if (ch == '\'') escaped += "\'\'";
                else escaped += ch;
            }
            dbExec("SET VARIABLE " + varname + " = '" + escaped + "'");
            row_vars.push_back(varname);

            local[s.var + "." + col]     = "getvariable('" + varname + "')";
            local_raw[s.var + "." + col] = value;
        }

        // Single-column shorthand: also bind the bare var name
        if (cols == 1) {
            char* sv = duckdb_value_varchar(&res, 0, r);
            std::string sv_val = sv ? sv : "";
            if (sv) duckdb_free(sv);
            local[s.var]     = "getvariable('" + row_vars[0] + "')";
            local_raw[s.var] = sv_val;
        }

        // Index variable: "for i, row in table:" binds i as 1-based integer
        std::string idx_varname;
        if (!s.index_var.empty()) {
            idx_varname = "__val_" + std::to_string(fn_depth + 1) + "_" + s.index_var;
            dbExec("SET VARIABLE " + idx_varname + " = " + std::to_string(r + 1));
            row_vars.push_back(idx_varname);
            local[s.index_var]     = "getvariable('" + idx_varname + "')";
            local_raw[s.index_var] = std::to_string(r + 1);
        }

        execBlock(s.body, local, local_raw);

        // Clean up row variables after each iteration
        for (auto& v : row_vars) dbExec("RESET VARIABLE " + v);

        // Handle break/continue signals from the body
        if (loop_signal == LoopSignal::Break) {
            loop_signal = LoopSignal::None;
            break;
        }
        if (loop_signal == LoopSignal::Continue) {
            loop_signal = LoopSignal::None;
            // continue to next iteration naturally
        }
    }
    if (show_progress && rows > 1) clearProgressLine();
    duckdb_destroy_result(&res);
}

// ====================== IF / WHILE =======================

void Interpreter::exec(const IfStmt& s, Env& env, RawEnv& raw) {
    if (evalCond(s.cond, env, raw)) {
        execBlock(s.thenb, env, raw);
    } else {
        execBlock(s.elseb, env, raw);
    }
}

void Interpreter::exec(const WhileStmt& s, Env& env, RawEnv& raw) {
    if (verbose) std::cout << dim("→ while") << "\n";
    // Execute body with env/raw passed by reference so val assignments
    // inside the loop body are visible on the next condition check.
    // This is intentionally different from for loops — while is imperative,
    // mutations are expected to propagate (val c = c + 1 drives the loop).
    while (evalCond(s.cond, env, raw)) {
        for (auto& n : s.body) {
            current_line = n->line_no;
            std::visit([&](auto&& x){ exec(x, env, raw); }, n->node);
            if (loop_signal != LoopSignal::None) break;
        }
        if (loop_signal == LoopSignal::Break) {
            loop_signal = LoopSignal::None;
            break;
        }
        if (loop_signal == LoopSignal::Continue) {
            loop_signal = LoopSignal::None;
            // continue to next while iteration
        }
    }
}

// ====================== EXPECT ======================

void Interpreter::exec(const ExpectStmt& s, Env& env, RawEnv& raw) {
    if (!evalCond(s.condition, env, raw)) {
        std::string msg = s.message.empty()
            ? "expectation failed: " + s.condition
            : s.message;

        if (s.action == "fail") {
            clearProgressLine(); std::cerr << red("❌ fail: ") << loc() << msg << "\n";
            flushLog();
            std::exit(1);
        } else {
            clearProgressLine(); std::cerr << yellow("⚠️  warn: ") << loc() << msg << "\n";
        }
    }
}

// ====================== PRINT ======================

// ====================== SHARED TEXT EVALUATION ======================
// evalText is the common engine for both print and log.
// It evaluates a text expression and returns a string result.
//
// Three cases:
//   SELECT/WITH  → execute; if multi-row: set is_multirow=true, populate out_res
//                  if single cell: return the value as string
//   expression   → SELECT CAST((expr) AS VARCHAR) — handles vals, arithmetic,
//                  string concat, arrays, structs
//   plain text   → returned as-is if DuckDB can't evaluate it
//
// The caller decides what to do with multi-row results:
//   print  → calls printResult() to display as a table
//   log    → logs metadata: "tablename: N rows"

std::string Interpreter::evalText(const std::string& text, bool& is_multirow,
                                   duckdb_result* out_res) {
    is_multirow = false;

    std::string upper = text;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c){ return std::toupper(c); });

    // SELECT/WITH: execute and check result shape
    if (upper.rfind("SELECT", 0) == 0 || upper.rfind("WITH", 0) == 0) {
        duckdb_result res;
        std::string err = dbExec(text, &res);
        if (!err.empty()) {
            clearProgressLine();
            std::cerr << red("error: ") << loc() << "print/log: " << err << "\n";
            duckdb_destroy_result(&res);
            return "";
        }
        idx_t rows = duckdb_row_count(&res);
        idx_t cols = duckdb_column_count(&res);
        if (rows == 1 && cols == 1) {
            // Single cell — return as plain string
            char* val = duckdb_value_varchar(&res, 0, 0);
            std::string result = val ? val : "";
            if (val) duckdb_free(val);
            duckdb_destroy_result(&res);
            return result;
        } else if (rows > 0) {
            // Multi-row — apply COLUMNS(*)::VARCHAR cast via CTE so complex
            // types (arrays, structs, maps) render correctly for the caller.
            // The CTE puts COLUMNS(*) at the outermost SELECT where DuckDB
            // can expand it properly.
            is_multirow = true;
            if (out_res) {
                std::string cast_sql = "WITH __dabble_r AS (" + text +
                                       ") SELECT COLUMNS(*)::VARCHAR FROM __dabble_r";
                duckdb_result cast_res;
                if (dbExec(cast_sql, &cast_res).empty()) {
                    duckdb_destroy_result(&res);
                    *out_res = cast_res;  // caller owns cast result
                } else {
                    *out_res = res;       // fallback: caller owns original result
                }
            } else {
                duckdb_destroy_result(&res);
            }
            return "";
        }
        duckdb_destroy_result(&res);
        return "";
    }

    // Expression — wrap in CAST so any type renders correctly
    {
        duckdb_result res;
        std::string err = dbExec("SELECT CAST((" + text + ") AS VARCHAR)", &res);
        if (!err.empty()) {
            // Not a SQL expression — return raw text
            return text;
        }
        std::string result = text;  // fallback
        if (duckdb_row_count(&res) > 0 && duckdb_column_count(&res) > 0) {
            char* val = duckdb_value_varchar(&res, 0, 0);
            if (val) { result = val; duckdb_free(val); }
        }
        duckdb_destroy_result(&res);
        return result;
    }
}

void Interpreter::exec(const PrintStmt& s, Env& env, RawEnv& raw) {
    std::string text = trim(resolve(s.text, env, raw));
    bool is_multirow = false;
    duckdb_result res;
    std::string result = evalText(text, is_multirow, &res);

    if (is_multirow) {
        printResult(&res);
        duckdb_destroy_result(&res);
    } else if (!result.empty()) {
        std::cout << result << "\n";
    }
}


// ====================== LOGGING ======================
//
// All log entries go to __dabble_log (an in-memory temp table).
// If log_destination is set (via "SET log = 'file.db'" or CLI),
// entries are also flushed to that file at script end.
//
// __dabble_log schema:
//   ts       TIMESTAMP   — when the entry was written
//   level    VARCHAR     — debug / info / warn / error
//   message  VARCHAR     — the evaluated message string
//   script   VARCHAR     — source filename
//   line     INTEGER     — source line number

void Interpreter::initLogTable() {
    if (log_table_ready) return;
    dbExec(
        "CREATE TEMP TABLE IF NOT EXISTS __dabble_log ("
        "  ts      TIMESTAMP DEFAULT NOW(),"
        "  level   VARCHAR,"
        "  message VARCHAR,"
        "  script  VARCHAR,"
        "  line    INTEGER"
        ")"
    );
    log_table_ready = true;
}

void Interpreter::writeLog(const std::string& level, const std::string& message) {
    initLogTable();

    // Escape single quotes in message for the INSERT
    std::string msg_escaped;
    for (char c : message) {
        if (c == '\'') msg_escaped += "\'\'";
        else msg_escaped += c;
    }

    std::string script = file_stack.empty() ? "" : file_stack.back();
    auto slash = script.rfind('/');
    if (slash != std::string::npos) script = script.substr(slash + 1);

    std::string insert =
        "INSERT INTO __dabble_log (level, message, script, line) VALUES ('"
        + level + "', '" + msg_escaped + "', '" + script + "', "
        + std::to_string(current_line) + ")";
    dbExec(insert);
}

void Interpreter::flushLog() {
    if (!log_table_ready || log_destination.empty()) return;

    // Determine format from extension
    std::string dest = log_destination;
    std::string ext;
    auto dot = dest.rfind('.');
    if (dot != std::string::npos) ext = dest.substr(dot + 1);

    if (ext == "csv") {
        dbExec("COPY __dabble_log TO '" + dest + "' (FORMAT CSV, HEADER, APPEND)");
    } else if (ext == "json" || ext == "ndjson") {
        dbExec("COPY __dabble_log TO '" + dest + "' (FORMAT JSON, ARRAY false)");
    } else {
        // Treat as DuckDB file — ATTACH and INSERT
        dbExec("ATTACH IF NOT EXISTS '" + dest + "' AS __log_dest");
        dbExec(
            "CREATE TABLE IF NOT EXISTS __log_dest.dabble_log AS "
            "SELECT * FROM __dabble_log WHERE false"
        );
        dbExec("INSERT INTO __log_dest.dabble_log SELECT * FROM __dabble_log");
        dbExec("DETACH __log_dest");
    }
    if (verbose) std::cout << dim("✓ log flushed to " + dest) << "\n";
}

void Interpreter::exec(const LogStmt& s, Env& env, RawEnv& raw) {
    std::string text = trim(resolve(s.text, env, raw));
    std::string level = s.level;

    // Skip debug entries unless verbose
    if (level == "debug" && !verbose) return;

    std::string message;

    // Check if text is a bare known let/array table — log metadata, not data.
    // Must check AFTER resolve() so val substitution has run, but the text
    // should still be a plain identifier (no spaces) to match a table name.
    bool is_bare_table = known_tables.count(text) > 0 ||
                         array_lets.count(text) > 0;

    if (is_bare_table) {
        // Log row count as metadata
        std::string tbl = known_tables.count(text) ? text : text; // view name for array let
        duckdb_result res;
        if (dbExec("SELECT COUNT(*) FROM " + tbl, &res).empty() &&
            duckdb_row_count(&res) > 0) {
            char* v = duckdb_value_varchar(&res, 0, 0);
            message = (array_lets.count(text) ? "array:" : "let:") + text +
                      " — " + (v ? v : "?") + " rows";
            if (v) duckdb_free(v);
        }
        duckdb_destroy_result(&res);
    } else {
        // Evaluate via the shared evalText engine — same path as print.
        // normaliseSQL converts "COUNT(*) FROM t" → "SELECT COUNT(*) FROM t",
        // and leaves quoted literals 'hello' and full SELECTs unchanged.
        std::string norm = normaliseSQL(text);
        bool is_multirow = false;
        duckdb_result res;
        message = evalText(norm, is_multirow, &res);

        if (is_multirow) {
            // Multi-row: log metadata only (data itself stays in the table)
            idx_t rows = duckdb_row_count(&res);
            idx_t cols = duckdb_column_count(&res);
            message = std::to_string(rows) + " rows, " +
                      std::to_string(cols) + " cols";
            duckdb_destroy_result(&res);
        }
    }

    // Write to log table
    writeLog(level, message);

    // Print to stderr with level color and timestamp
    bool colors = isatty(STDERR_FILENO);
    const char* col =
        (level == "error") ? (colors ? "\033[1;31m" : "") :
        (level == "warn")  ? (colors ? "\033[1;33m" : "") :
        (level == "debug") ? (colors ? "\033[2m"    : "") :
                             (colors ? "\033[0;36m"  : "");  // info: cyan
    const char* RST = colors ? "\033[0m" : "";

    std::cerr << col << "[" << level << "] " << RST << message << "\n";
}

// ====================== IMPORT =======================

void Interpreter::exec(const ImportStmt& s, Env& env, RawEnv& raw) {
    std::filesystem::path base = file_stack.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(file_stack.back()).parent_path();

    std::filesystem::path filepath = std::filesystem::weakly_canonical(base / s.filename);

    if (!std::filesystem::exists(filepath)) {
        clearProgressLine(); std::cerr << red("error: ") << loc() << "cannot open import '" << filepath.string() << "'\n";
        return;
    }

    std::ifstream f(filepath);
    std::stringstream buf;
    buf << f.rdbuf();

    file_stack.push_back(filepath.string());

    Parser subparser(buf.str());
    auto subast = subparser.parseBlock();
    execBlock(subast, env, raw);

    file_stack.pop_back();
    if (verbose) std::cout << dim("✓ imported " + filepath.string()) << "\n";
}

// ====================== SQL ======================

void Interpreter::exec(const SQLStmt& s, Env& env, RawEnv& raw) {
    // Check bare assignment on the ORIGINAL sql BEFORE resolve().
    // After resolve(), "c" becomes "getvariable('__val_0_c')" — no longer
    // a plain identifier — so the assignment check must see the raw source.
    if (s.redirect_file.empty()) {
        std::string orig = trim(s.sql);
        auto eq = orig.find('=');
        if (eq != std::string::npos && eq > 0) {
            char before = orig[eq - 1];
            std::string full_upper = orig;
            std::transform(full_upper.begin(), full_upper.end(), full_upper.begin(),
                           [](unsigned char c){ return std::toupper(c); });
            bool has_from = full_upper.find(" FROM ") != std::string::npos;

            if (before != '!' && before != '<' && before != '>' && before != '='
                && before != ':' && !has_from) {  // : guards against named params (mode := 'AUTO')
                std::string lhs = trim(orig.substr(0, eq));
                bool is_ident = !lhs.empty();
                for (char c : lhs)
                    if (!std::isalnum((unsigned char)c) && c != '_') { is_ident = false; break; }

                if (is_ident && env.count(lhs)) {
                    // Resolve only the RHS, then dispatch as val reassignment
                    std::string rhs = trim(resolve(orig.substr(eq + 1), env, raw));
                    ValStmt vs{lhs, rhs};
                    exec(vs, env, raw);
                    return;
                }
            }
        }
    }

    std::string sql = trim(resolve(s.sql, env, raw));
    if (sql.empty()) return;

    // Check for Dabble function call BEFORE normaliseSQL — otherwise
    // fn_name() becomes SELECT (fn_name()) which DuckDB tries to execute
    // as a SQL scalar function.
    {
        std::string fn_name; std::vector<std::string> fn_args;
        if (isFunctionCall(sql, fn_name, fn_args) && functions.count(fn_name)) {
            if (verbose) std::cout << dim("→ calling " + fn_name + "()") << "\n";
            FnResult r = execFn(functions[fn_name], env, fn_args, raw);
            std::string built = r.build();
            if (!built.empty()) {
                if (!s.redirect_file.empty()) {
                    std::string copy_sql = "COPY (" + built + ") TO '" + s.redirect_file +
                                           "' (FORMAT CSV" + (s.append ? ", APPEND" : ", HEADER") + ")";
                    std::string err = dbExec(copy_sql);
                    for (auto& v : r.vars_to_reset) dbExec("RESET VARIABLE " + v);
                    if (!err.empty())
                        clearProgressLine(), std::cerr << red("error: ") << loc() << "export " << fn_name << "(): " << err << "\n";
                    else if (verbose)
                        std::cout << dim("→ exported to " + s.redirect_file) << "\n";
                } else {
                    duckdb_result res;
                    std::string err = dbExec(built, &res);
                    for (auto& v : r.vars_to_reset) dbExec("RESET VARIABLE " + v);
                    if (!err.empty())
                        clearProgressLine(), std::cerr << red("error: ") << loc() << fn_name << "(): " << err << "\n";
                    else
                        printResult(&res);
                    duckdb_destroy_result(&res);
                }
            }
            return;
        }
    }

    // Normalise: bare table names, FROM expressions, or full SELECTs all work uniformly
    sql = normaliseSQL(sql);



    // Redirect to file
    if (!s.redirect_file.empty()) {
        std::string copy_sql = "COPY (" + sql + ") TO '" + s.redirect_file +
                               "' (FORMAT CSV" + (s.append ? ", APPEND" : ", HEADER") + ")";
        std::string err = dbExec(copy_sql);
        if (!err.empty()) {
            clearProgressLine(); std::cerr << red("error: ") << loc() << "export: " << err << "\n";
        } else if (verbose)
            std::cout << dim("→ exported to " + s.redirect_file) << "\n";
        return;
    }

    // Normal SQL
    duckdb_result res;
    std::string err = dbExec(sql, &res);
    if (!err.empty()) {
        clearProgressLine(); std::cerr << red("error: ") << loc() << err << "\n";
        duckdb_destroy_result(&res);
        return;
    }

    // Print result only for SELECT/WITH — DML returns a "Count" column we suppress.
    {
        std::string up2 = sql;
        std::transform(up2.begin(), up2.end(), up2.begin(),
                       [](unsigned char c){ return std::toupper(c); });
        bool is_select = up2.rfind("SELECT", 0) == 0 || up2.rfind("WITH", 0) == 0;
        if (is_select && duckdb_column_count(&res) > 0) {
            // Re-execute using a CTE so COLUMNS(*)::VARCHAR applies at the
            // outermost SELECT level — DuckDB requires this for the macro to work.
            // Using a CTE avoids the subquery wrapping issue:
            //   WITH __r AS (original_sql) SELECT COLUMNS(*)::VARCHAR FROM __r
            std::string typed_sql = "WITH __dabble_r AS (" + sql +
                                    ") SELECT COLUMNS(*)::VARCHAR FROM __dabble_r";
            duckdb_result typed_res;
            if (dbExec(typed_sql, &typed_res).empty()) {
                printResult(&typed_res);
                duckdb_destroy_result(&typed_res);
            } else {
                // Fallback — print original result as-is
                printResult(&res);
            }
        }
    }
    duckdb_destroy_result(&res);
}