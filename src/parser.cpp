#include "parser.h"
#include "utils.h"
#include <sstream>
#include <regex>
#include <iostream>

Parser::Parser(const std::string& src) {
    std::stringstream ss(src);
    std::string line;
    while (std::getline(ss, line)) {
        lines.push_back(line);
    }
}

// ====================== MAIN PARSER ======================

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
            std::cerr << "Indentation error at line " << (pos + 1) << "\n";
            std::exit(1);
        }

        if (t.rfind("let ", 0) == 0)         block.push_back(parseLet(baseIndent));
        else if (t.rfind("table ", 0) == 0)  block.push_back(parseLet(baseIndent));
        else if (t.rfind("val ", 0) == 0)    block.push_back(parseVal(baseIndent));
        else if (t.rfind("scalar ", 0) == 0) block.push_back(parseVal(baseIndent));
        else if (t.rfind("for ", 0) == 0)    block.push_back(parseFor(baseIndent));
        else if (t.rfind("if ", 0) == 0)     block.push_back(parseIf(baseIndent));
        else if (t.rfind("while ", 0) == 0)  block.push_back(parseWhile(baseIndent));
        else if (t.rfind("expect ", 0) == 0) block.push_back(parseExpect(baseIndent));
        else if (t.rfind("fn ", 0) == 0)     block.push_back(parseFn(baseIndent));
        else if (t.rfind("print ", 0) == 0)  block.push_back(parsePrint());
        else if (t.rfind("import ", 0) == 0) block.push_back(parseImport());
        else                                 block.push_back(parseRawSQL(baseIndent));
    }
    return block;
}

// ====================== HELPERS ======================

std::string Parser::collectSQL(int minIndent, std::string& redirect_file, bool& append) {
    // A SQL statement spans multiple lines until:
    //   a) a line ends with a semicolon  → that line ends the statement
    //   b) a Dabble keyword is encountered at the base indent → new statement
    //   c) indentation drops below minIndent → end of block
    // Single-line statements don't need a semicolon.
    // Multi-line statements do — this is unambiguous and requires no heuristics.
    std::string sql;
    while (pos < (int)lines.size()) {
        int lvl = indentLevel(lines[pos]);
        if (lvl < minIndent) break;

        std::string t = trim(lines[pos]);
        if (t.empty() || t.rfind("--", 0) == 0) {
            pos++;
            continue;
        }

        // Dabble keywords always start a new statement
        if (t.rfind("let ", 0) == 0 || t.rfind("table ", 0) == 0 ||
            t.rfind("val ", 0) == 0 || t.rfind("scalar ", 0) == 0 ||
            t.rfind("for ", 0) == 0 || t.rfind("if ", 0) == 0 ||
            t.rfind("while ", 0) == 0 || t.rfind("expect ", 0) == 0 ||
            t.rfind("fn ", 0) == 0 || t.rfind("print ", 0) == 0 ||
            t.rfind("import ", 0) == 0 || t.rfind("else", 0) == 0) {
            break;
        }

        // Redirect operator ends the statement
        std::regex redir(R"(^(.*)\s*(>>|>)\s*([^\s>]+)\s*$)");
        std::smatch match;
        if (std::regex_match(t, match, redir)) {
            // Strip trailing semicolon from the SQL part if present
            std::string sql_part = match[1].str();
            if (!sql_part.empty() && sql_part.back() == ';') sql_part.pop_back();
            sql += (sql.empty() ? "" : "\n") + sql_part;
            redirect_file = match[3].str();
            append = (match[2].str() == ">>");
            pos++;
            break;
        }

        // If this is not the first line and we don't already have an open
        // statement (no semicolon yet collected), only continue if we ARE
        // already mid-statement (sql is non-empty and no semicolon ended it).
        // A new same-indent line after a complete single-line statement
        // is a new statement — we stop here and let parseBlock handle it.
        if (!sql.empty()) {
            std::string prev = trim(sql);
            if (!prev.empty() && prev.back() != ';') {
                // Still mid-statement — keep collecting.
                // Nothing to do, fall through.
            }
        }

        if (!sql.empty()) sql += "\n";
        sql += lines[pos];
        pos++;

        // Semicolon at end of line terminates this statement.
        // Strip it — DuckDB doesn't need it and it avoids confusion.
        std::string trimmed = trim(sql);
        if (!trimmed.empty() && trimmed.back() == ';') {
            sql = trimmed.substr(0, trimmed.size() - 1);
            break;
        }
    }
    return trim(sql);
}

// ====================== INDIVIDUAL PARSERS ======================

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
    std::string sql = trim(line.substr(eq + 1));

    pos++;
    std::string redirect; bool app = false;
    std::string more = collectSQL(baseIndent + 4, redirect, app);
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

    pos++;
    // Collect multi-line continuation (same rule as let)
    std::string redirect; bool app = false;
    std::string more = collectSQL(baseIndent + 4, redirect, app);
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
    std::string var = trim(line.substr(4, inpos - 4));
    std::string src = trim(line.substr(inpos + 4, line.find(':') - (inpos + 4)));

    pos++;
    auto body = parseBlock(baseIndent + 4);
    return std::make_shared<ASTNode>(ForStmt{var, src, body}, ln);
}

ASTPtr Parser::parseIf(int baseIndent) {
    int ln = pos + 1;
    std::string line = trim(lines[pos]);
    auto l = line.find('(');
    auto r = line.rfind(')');
    std::string cond = (l != std::string::npos && r != std::string::npos)
                     ? line.substr(l + 1, r - l - 1) : "";

    pos++;
    auto thenb = parseBlock(baseIndent + 4);

    std::vector<ASTPtr> elseb;
    if (pos < (int)lines.size()) {
        std::string nxt = trim(lines[pos]);
        if (nxt.rfind("else if", 0) == 0) {
            elseb.push_back(parseIf(baseIndent));
        } else if (nxt.rfind("else:", 0) == 0) {
            pos++;
            elseb = parseBlock(baseIndent + 4);
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
    auto body = parseBlock(baseIndent + 4);
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
    auto body = parseBlock(baseIndent + 4);

    return std::make_shared<ASTNode>(FnStmt{name, params, body, ""}, ln);
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

    // Collect continuation lines that are indented deeper than the print keyword.
    // This lets SQL queries (and string concatenations) span multiple lines naturally:
    //
    //   print SELECT 'total: ' || COUNT(*)
    //       FROM orders
    //       WHERE status = 'paid'
    //
    // Lines at the same or shallower indent end the print expression.
    while (pos < (int)lines.size()) {
        int lvl = indentLevel(lines[pos]);
        if (lvl <= base_indent) break;
        std::string t = trim(lines[pos]);
        if (!t.empty() && t.rfind("--", 0) != 0) {
            text += " " + t;
        }
        pos++;
    }

    text = trim(text);
    if (!text.empty() && text.back() == ';') text.pop_back();
    text = trim(text);

    // Strip surrounding quotes for plain string literals.
    // Don't strip when it's a SQL expression — quotes there are part of the SQL.
    if (text.length() >= 2 &&
        ((text.front() == '\'' && text.back() == '\'') ||
         (text.front() == '"' && text.back() == '"'))) {
        text = text.substr(1, text.length() - 2);
    }

    return std::make_shared<ASTNode>(PrintStmt{text}, ln);
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