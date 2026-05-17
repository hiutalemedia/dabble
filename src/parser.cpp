#include "parser.h"
#include "utils.h"
#include <sstream>
#include <regex>
#include <iostream>

Parser::Parser(const std::string& src) {
    std::stringstream ss(src);
    std::string line;
    while (std::getline(ss, line)) {
        // Normalize leading tabs to 4 spaces so the rest of the parser
        // only ever sees space-based indentation.  Tabs inside strings
        // and comments are left untouched.
        std::string normalized;
        bool leading = true;
        for (char c : line) {
            if (leading && c == '\t') {
                normalized += "    ";
            } else {
                leading = (c == ' ') && leading ? true : false;
                normalized += c;
            }
        }
        lines.push_back(normalized);
    }
}


// Detect the indentation step used by the first indented line after the
// current position.  Handles 2-space, 4-space, and any other consistent
// indentation — the step is whatever the author actually typed.
// Falls back to 4 if no indented line is found (empty block).
int Parser::detectIndentStep(int baseIndent) {
    for (int i = pos; i < (int)lines.size(); i++) {
        int lvl = indentLevel(lines[i]);
        std::string t = trim(lines[i]);
        if (t.empty() || t.rfind("--", 0) == 0) continue;
        if (lvl > baseIndent) return lvl - baseIndent;
        if (lvl <= baseIndent) break;
    }
    return 4;
}

// ====================== MAIN PARSER =======================

std::vector<ASTPtr> Parser::parseBlock(int baseIndent) {
    std::vector<ASTPtr> block;

    while (pos < (int)lines.size()) {
        int lvl = indentLevel(lines[pos]);
        if (lvl < baseIndent) break;

        std::string t = trim(lines[pos]);
        if (t.empty() || t.rfind("--", 0) == 0) {
            pos++;
            continue;
        }

        if (lvl > baseIndent) {
            // Line is indented deeper than expected — likely a continuation
            // line that wasn't consumed by the previous parser (e.g. a column
            // definition line after a CREATE TABLE was only partially parsed).
            // Skip it rather than aborting — collectSQL with paren tracking
            // should handle most cases, but defensive skip avoids hard crashes.
            pos++;
            continue;
        }

        if (t.rfind("let ", 0) == 0)         block.push_back(parseLet(baseIndent));
        else if (t.rfind("table ", 0) == 0)  block.push_back(parseLet(baseIndent));
        else if (t.rfind("val ", 0) == 0)    block.push_back(parseVal(baseIndent));
        else if (t.rfind("scalar ", 0) == 0) block.push_back(parseVal(baseIndent));
        else if (t.rfind("for ", 0) == 0)    block.push_back(parseFor(baseIndent));
        else if (t.rfind("if ", 0) == 0)     block.push_back(parseIf(baseIndent));
        else if (t.rfind("while ", 0) == 0)  block.push_back(parseWhile(baseIndent));
        else if (t.rfind("expect ", 0) == 0) block.push_back(parseExpect(baseIndent));
        else if (t.rfind("check ", 0) == 0)  block.push_back(parseExpect(baseIndent));
        else if (t.rfind("fn ", 0) == 0)     block.push_back(parseFn(baseIndent));
        else if (t.rfind("print ", 0) == 0)  block.push_back(parsePrint());
        else if (t.rfind("log ", 0) == 0)    block.push_back(parseLog());
        else if (t.rfind("import ", 0) == 0)     block.push_back(parseImport());
        else if (t.rfind("projection ", 0) == 0) block.push_back(parseProjection(baseIndent));
        else if (t.rfind("proj ", 0) == 0)       block.push_back(parseProjection(baseIndent));
        else if (t.rfind("columns ", 0) == 0)    block.push_back(parseProjection(baseIndent));
        else if (t.rfind("cols ", 0) == 0)       block.push_back(parseProjection(baseIndent));
        else {
            // Detect array let append: "name += expr"
            // Must check before parseRawSQL so += is not mistaken for SQL.
            auto plus_eq = t.find("+=");
            bool is_arr_append = false;
            if (plus_eq != std::string::npos && plus_eq > 0) {
                std::string lhs = trim(t.substr(0, plus_eq));
                is_arr_append = !lhs.empty();
                for (char c : lhs)
                    if (!std::isalnum((unsigned char)c) && c != '_') { is_arr_append = false; break; }
            }
            if (is_arr_append)                  block.push_back(parseArrAppend(baseIndent));
            else if (t.rfind("break", 0) == 0)  { pos++; block.push_back(std::make_shared<ASTNode>(BreakStmt{}, pos)); }
            else if (t.rfind("continue", 0) == 0) { pos++; block.push_back(std::make_shared<ASTNode>(ContinueStmt{}, pos)); }
            else                                block.push_back(parseRawSQL(baseIndent));
        }
    }
    return block;
}

// ====================== HELPERS ======================

std::string Parser::collectSQL(int minIndent, std::string& redirect_file, bool& append) {
    // Simple rule: collect lines until a semicolon, a Dabble keyword,
    // or indentation drops below minIndent — BUT only break on indent drop
    // when parens are balanced. This handles multi-line SQL like:
    //
    //   CREATE TABLE foo (      ← opens paren
    //       col1 INTEGER,       ← indented, collected
    //       col2 VARCHAR        ← indented, collected
    //   );                      ← back to base indent, but ) closes paren → collected
    //
    // Without paren tracking, the ); line would be missed and cause a parse error.
    auto openParens = [](const std::string& s) {
        int depth = 0; bool in_s = false;
        for (char c : s) {
            if (c == '\'') { in_s = !in_s; continue; }
            if (in_s) continue;
            if (c == '(') depth++;
            else if (c == ')') depth--;
        }
        return depth;
    };

    std::string sql;
    int paren_depth = 0;
    while (pos < (int)lines.size()) {
        int lvl = indentLevel(lines[pos]);
        // Only break on indent drop when all parens are closed
        if (lvl < minIndent && paren_depth <= 0) break;

        std::string t = trim(lines[pos]);
        if (t.empty() || t.rfind("--", 0) == 0) { pos++; continue; }

        // Dabble keywords always start a new statement
        if (t.rfind("let ", 0) == 0 || t.rfind("table ", 0) == 0 ||
            t.rfind("val ", 0) == 0 || t.rfind("scalar ", 0) == 0 ||
            t.rfind("for ", 0) == 0 || t.rfind("if ", 0) == 0 ||
            t.rfind("while ", 0) == 0 || t.rfind("expect ", 0) == 0 ||
            t.rfind("check ", 0) == 0 || t.rfind("fn ", 0) == 0 ||
            t.rfind("print ", 0) == 0 || t.rfind("log ", 0) == 0 || t.rfind("import ", 0) == 0 ||
            t.rfind("else", 0) == 0 || t.rfind("projection ", 0) == 0 ||
            t.rfind("proj ", 0) == 0 || t.rfind("columns ", 0) == 0 ||
            t.rfind("cols ", 0) == 0 ||
            t == "break" || t == "break;" ||
            t == "continue" || t == "continue;") {
            break;
        }

        // Redirect operator ends the statement
        // Note: bare > is intentionally excluded — too ambiguous with SQL comparison operators.
        // Only -> (export) and >> (append) are redirect operators.
        std::regex redir(R"(^(.*?)\s*(->|>>)\s*([^\s>]+)\s*$)");
        std::smatch match;
        if (std::regex_match(t, match, redir)) {
            std::string sql_part = match[1].str();
            if (!sql_part.empty() && sql_part.back() == ';') sql_part.pop_back();
            sql += (sql.empty() ? "" : "\n") + sql_part;
            redirect_file = match[3].str();
            // Strip trailing semicolon from filename if present
            if (!redirect_file.empty() && redirect_file.back() == ';')
                redirect_file.pop_back();
            append = (match[2].str() == ">>");
            pos++;
            break;
        }

        if (!sql.empty()) sql += "\n";
        sql += lines[pos];
        paren_depth += openParens(lines[pos]);
        pos++;

        // Semicolon terminates — only when parens are balanced
        std::string trimmed = trim(sql);
        if (!trimmed.empty() && trimmed.back() == ';' && paren_depth <= 0) {
            sql = trimmed.substr(0, trimmed.size() - 1);
            break;
        }
    }
    return trim(sql);
}

// ====================== INDIVIDUAL PARSERS ======================

ASTPtr Parser::parseProjection(int baseIndent) {
    int ln = pos + 1;
    std::string line = trim(lines[pos]);

    // Detect prefix length: "projection "(11), "proj "(5), "columns "(8), "cols "(5)
    int prefix = 5;  // default: proj / cols
    if (line.rfind("projection ", 0) == 0) prefix = 11;
    else if (line.rfind("columns ", 0) == 0) prefix = 8;

    // Name is between the keyword and '=' or end of line
    auto eq = line.find('=');
    std::string name = (eq != std::string::npos)
        ? trim(line.substr(prefix, eq - prefix))
        : trim(line.substr(prefix));

    // Column list: either on same line after '=', or on indented lines below
    std::string cols;
    if (eq != std::string::npos) {
        cols = trim(line.substr(eq + 1));
    }
    pos++;

    // Collect continuation lines (indented deeper than the projection keyword)
    int proj_step = detectIndentStep(baseIndent);
    while (pos < (int)lines.size()) {
        int lvl = indentLevel(lines[pos]);
        if (lvl < baseIndent + proj_step) break;
        std::string t = trim(lines[pos]);
        if (t.empty() || t.rfind("--", 0) == 0) { pos++; continue; }
        if (!cols.empty()) cols += ",\n    ";
        cols += t;
        // Strip trailing comma if present — we add our own
        if (!cols.empty() && cols.back() == ',') cols.pop_back();
        pos++;
    }

    // Normalise: ensure commas between items if multi-line
    return std::make_shared<ASTNode>(ProjectionStmt{name, trim(cols)}, ln);
}

ASTPtr Parser::parseArrAppend(int baseIndent) {
    int ln = pos + 1;
    std::string line = trim(lines[pos]);

    auto plus_eq = line.find("+=");
    std::string name = trim(line.substr(0, plus_eq));
    std::string expr_head = trim(line.substr(plus_eq + 2));

    pos++;

    // Collect multi-line expression
    std::string redirect; bool app = false;
    int step = detectIndentStep(baseIndent);
    std::string more = collectSQL(baseIndent + step, redirect, app);
    if (!more.empty()) {
        if (!expr_head.empty()) expr_head += "\n";
        expr_head += more;
    }
    expr_head = trim(expr_head);
    // Strip trailing semicolon
    if (!expr_head.empty() && expr_head.back() == ';') expr_head.pop_back();

    return std::make_shared<ASTNode>(ArrLetStmt{name, trim(expr_head)}, ln);
}


ASTPtr Parser::parseRawSQL(int baseIndent) {
    int ln = pos + 1;
    std::string redirect_file;
    bool append = false;
    std::string sql = collectSQL(baseIndent, redirect_file, append);
    return std::make_shared<ASTNode>(SQLStmt{sql, redirect_file, append}, ln);
}

ASTPtr Parser::parseLet(int baseIndent) {
    int ln = pos + 1;
    std::string line = trim(lines[pos]);
    auto eq = line.find('=');
    // "let name = ..." prefix is 4 chars; "table name = ..." prefix is 6 chars
    int prefix = (line.rfind("table ", 0) == 0) ? 6 : 4;
    std::string name = trim(line.substr(prefix, eq - prefix));
    std::string sql  = trim(line.substr(eq + 1));
    // Strip trailing semicolon from single-line RHS
    if (!sql.empty() && sql.back() == ';') sql.pop_back();
    sql = trim(sql);

    // Detect array let declaration: "let name[] = []" or "let name[] = [a, b, c]"
    // Strip trailing [] from name to get the base name.
    bool is_array = (name.size() >= 2 &&
                     name[name.size()-2] == '[' &&
                     name[name.size()-1] == ']');
    if (is_array) {
        name = trim(name.substr(0, name.size() - 2));
        // Parse the RHS list: [] = empty init, [a, b, c] = init with items
        // Strip trailing semicolon
        if (!sql.empty() && sql.back() == ';') sql.pop_back();
        // Strip outer brackets
        std::string inner = trim(sql);
        if (!inner.empty() && inner.front() == '[' && inner.back() == ']')
            inner = trim(inner.substr(1, inner.size() - 2));
        // Each comma-separated item becomes an ArrLetStmt append
        // We emit one ArrLetStmt with name and the full item list (comma-separated)
        // The interpreter handles splitting and creating individual tables.
        pos++;
        return std::make_shared<ASTNode>(ArrLetStmt{name, inner}, ln);
    }

    pos++;
    std::string redirect; bool app = false;
    int let_step = detectIndentStep(baseIndent);
    std::string more = collectSQL(baseIndent + let_step, redirect, app);
    if (!more.empty()) {
        if (!sql.empty()) sql += "\n";
        sql += more;
    }
    return std::make_shared<ASTNode>(LetStmt{name, sql}, ln);
}
ASTPtr Parser::parseVal(int baseIndent) {
    int ln = pos + 1;
    std::string line = trim(lines[pos]);
    auto eq = line.find('=');
    // "val name = ..." prefix is 4; "scalar name = ..." prefix is 7
    int prefix = (line.rfind("scalar ", 0) == 0) ? 7 : 4;
    std::string name = trim(line.substr(prefix, eq - prefix));
    std::string expr = trim(line.substr(eq + 1));
    // Strip trailing semicolon from single-line expression
    if (!expr.empty() && expr.back() == ';') expr.pop_back();
    expr = trim(expr);

    pos++;
    // Collect multi-line continuation (collectSQL strips its own semicolon)
    std::string redirect; bool app = false;
    int val_step = detectIndentStep(baseIndent);
    std::string more = collectSQL(baseIndent + val_step, redirect, app);
    if (!more.empty()) {
        if (!expr.empty()) expr += "\n";
        expr += more;
    }
    return std::make_shared<ASTNode>(ValStmt{name, expr}, ln);
}

ASTPtr Parser::parseFor(int baseIndent) {
    int ln = pos + 1;
    std::string line = trim(lines[pos]);
    auto inpos = line.find(" in ");
    std::string vars   = trim(line.substr(4, inpos - 4));
    std::string source = trim(line.substr(inpos + 4, line.find(':') - (inpos + 4)));

    // Detect enumeration: "for i, row in table:" → index_var="i", var="row"
    std::string index_var, var;
    auto comma = vars.find(',');
    if (comma != std::string::npos) {
        index_var = trim(vars.substr(0, comma));
        var       = trim(vars.substr(comma + 1));
    } else {
        var = vars;
    }

    // table.column shorthand: "for name in customers.name:"
    // → source becomes "(SELECT name FROM customers)"
    auto dot = source.find('.');
    if (dot != std::string::npos &&
        source.find(' ') == std::string::npos &&
        source.find('(') == std::string::npos) {
        std::string tbl = source.substr(0, dot);
        std::string col = source.substr(dot + 1);
        source = "(SELECT " + col + " FROM " + tbl + ")";
    }

    pos++;
    int for_step = detectIndentStep(baseIndent);
    auto body = parseBlock(baseIndent + for_step);
    return std::make_shared<ASTNode>(ForStmt{index_var, var, source, body}, ln);
}
ASTPtr Parser::parseIf(int baseIndent) {
    int ln = pos + 1;
    std::string line = trim(lines[pos]);
    auto l = line.find('(');
    auto r = line.rfind(')');
    std::string cond = (l != std::string::npos && r != std::string::npos)
                     ? line.substr(l + 1, r - l - 1) : "";

    pos++;
    int if_step = detectIndentStep(baseIndent);
    auto thenb = parseBlock(baseIndent + if_step);

    std::vector<ASTPtr> elseb;
    if (pos < (int)lines.size()) {
        std::string nxt = trim(lines[pos]);
        if (nxt.rfind("else if", 0) == 0) {
            elseb.push_back(parseIf(baseIndent));
        } else if (nxt.rfind("else:", 0) == 0) {
            pos++;
            elseb = parseBlock(baseIndent + if_step);
        }
    }
    return std::make_shared<ASTNode>(IfStmt{cond, thenb, elseb}, ln);
}

ASTPtr Parser::parseWhile(int baseIndent) {
    int ln = pos + 1;
    std::string line = trim(lines[pos]);
    auto l = line.find('(');
    auto r = line.rfind(')');
    std::string cond = (l != std::string::npos && r != std::string::npos)
                     ? line.substr(l + 1, r - l - 1) : "";

    pos++;
    int while_step = detectIndentStep(baseIndent);
    auto body = parseBlock(baseIndent + while_step);
    return std::make_shared<ASTNode>(WhileStmt{cond, body}, ln);
}

ASTPtr Parser::parseExpect(int baseIndent) {
    int ln = pos + 1;
    std::string line = trim(lines[pos]);
    auto l = line.find('(');
    auto r = line.rfind(')');
    std::string cond = (l != std::string::npos && r != std::string::npos)
                     ? line.substr(l + 1, r - l - 1) : "";

    std::string action = "fail";
    std::string msg = "";

    size_t else_pos = line.find("else ");
    if (else_pos != std::string::npos) {
        std::string rest = line.substr(else_pos + 5);
        if (rest.find("warn") != std::string::npos) action = "warn";
        if (rest.find("fail") != std::string::npos) action = "fail";

        size_t q1 = rest.find('\'');
        size_t q2 = rest.rfind('\'');
        if (q1 != std::string::npos && q2 > q1) {
            msg = rest.substr(q1 + 1, q2 - q1 - 1);
        }
    }

    pos++;
    return std::make_shared<ASTNode>(ExpectStmt{cond, action, msg}, ln);
}

ASTPtr Parser::parseFn(int baseIndent) {
    int ln = pos + 1;
    std::string line = trim(lines[pos]);
    size_t l = line.find('(');
    size_t r = line.rfind(')');
    std::string name = trim(line.substr(3, l - 3));
    std::string params_str = (l != std::string::npos && r != std::string::npos)
                           ? line.substr(l + 1, r - l - 1) : "";

    std::vector<std::string> params;
    std::stringstream ss(params_str);
    std::string p;
    while (std::getline(ss, p, ',')) {
        params.push_back(trim(p));
    }

    pos++;
    int fn_step = detectIndentStep(baseIndent);
    auto body = parseBlock(baseIndent + fn_step);

    return std::make_shared<ASTNode>(FnStmt{name, params, body}, ln);
}

ASTPtr Parser::parsePrint() {
    int ln = pos + 1;
    std::string line = lines[pos];
    int base_indent = indentLevel(line);
    size_t print_pos = line.find("print ");
    std::string text = (print_pos != std::string::npos)
                     ? trim(line.substr(print_pos + 6))
                     : "";
    pos++;

    // Collect continuation lines. Two rules:
    //   1. Lines indented deeper than print always continue.
    //   2. Lines at the same indent continue while parens are unbalanced —
    //      this handles multiline table function calls:
    //
    //      print SELECT * FROM dlx_count(
    //          getvariable('n')::INTEGER,   ← deeper, collected
    //          getvariable('rows')::INTEGER[][]
    //      );                                ← same indent, but ) closes open paren
    //
    // Count open parens in text collected so far to know when we're "inside" a call.
    auto countOpenParens = [](const std::string& s) {
        int depth = 0; bool in_s = false;
        for (char c : s) {
            if (c == '\'') { in_s = !in_s; continue; }
            if (in_s) continue;
            if (c == '(') depth++;
            else if (c == ')') depth--;
        }
        return depth;
    };

    while (pos < (int)lines.size()) {
        int lvl = indentLevel(lines[pos]);
        std::string t = trim(lines[pos]);
        if (t.empty() || t.rfind("--", 0) == 0) { pos++; continue; }

        bool deeper    = lvl > base_indent;
        bool unbalanced = countOpenParens(text) > 0;

        if (!deeper && !unbalanced) break;

        text += " " + t;
        pos++;

        // Semicolon at end while balanced — done
        std::string trimmed = trim(text);
        if (!trimmed.empty() && trimmed.back() == ';' && countOpenParens(text) <= 0) {
            text = trimmed.substr(0, trimmed.size() - 1);
            break;
        }
    }

    text = trim(text);
    if (!text.empty() && text.back() == ';') text.pop_back();
    text = trim(text);

    // Strip surrounding quotes for plain string literals.
    if (text.length() >= 2 &&
        ((text.front() == '\'' && text.back() == '\'') ||
         (text.front() == '"' && text.back() == '"'))) {
        text = text.substr(1, text.length() - 2);
    }

    return std::make_shared<ASTNode>(PrintStmt{text}, ln);
}


ASTPtr Parser::parseLog() {
    // Mirrors parsePrint exactly — same multi-line text collection.
    // Optional level keyword after "log": debug, info, warn, error.
    // Examples:
    //   log 'message'
    //   log warn 'something looks off'
    //   log total_revenue
    //   log COUNT(*) FROM paid
    int ln = pos + 1;
    std::string line = lines[pos];
    int base_indent = indentLevel(line);
    size_t log_pos = line.find("log ");
    std::string rest = (log_pos != std::string::npos)
                     ? trim(line.substr(log_pos + 4)) : "";
    pos++;

    // Collect continuation lines indented deeper
    while (pos < (int)lines.size()) {
        int lvl = indentLevel(lines[pos]);
        if (lvl <= base_indent) break;
        std::string t = trim(lines[pos]);
        if (!t.empty() && t.rfind("--", 0) != 0) rest += " " + t;
        pos++;
    }

    rest = trim(rest);
    if (!rest.empty() && rest.back() == ';') rest.pop_back();
    rest = trim(rest);

    // Strip inline SQL comments (-- ...) that may follow the expression
    {
        size_t cmt = rest.find(" --");
        if (cmt != std::string::npos) rest = trim(rest.substr(0, cmt));
    }

    // Extract optional level prefix: "warn message", "error message", etc.
    // Note: do NOT strip surrounding quotes — evalText handles quoted strings
    // the same way print does: 'hello' → SELECT ('hello') → "hello"
    std::string level = "info";
    static const char* levels[] = {"debug ", "info ", "warn ", "error ", nullptr};
    for (int i = 0; levels[i]; i++) {
        if (rest.rfind(levels[i], 0) == 0) {
            level = trim(std::string(levels[i]));
            rest  = trim(rest.substr(strlen(levels[i])));
            break;
        }
    }

    return std::make_shared<ASTNode>(LogStmt{rest, level}, ln);
}

ASTPtr Parser::parseImport() {
    int ln = pos + 1;
    std::string line = trim(lines[pos]);
    size_t start = line.find('"');
    size_t end = line.rfind('"');
    std::string filename = (start != std::string::npos && end > start)
                         ? line.substr(start + 1, end - start - 1) : "";
    pos++;
    return std::make_shared<ASTNode>(ImportStmt{filename}, ln);
}