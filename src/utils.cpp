#include "utils.h"
#include <iostream>
#include <cctype>
#include <regex>

void check(duckdb_state s, const char* msg) {
    if (s == DuckDBError) {
        std::cerr << "ERROR: " << msg << "\n";
        std::exit(1);
    }
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

int indentLevel(const std::string& line) {
    // Tabs are normalized to spaces in the Parser constructor,
    // so we only need to count spaces here.
    int c = 0;
    for (char ch : line) {
        if (ch == ' ') c++;
        else break;
    }
    return c;
}

std::string replaceAll(std::string s,
    const std::unordered_map<std::string,std::string>& env,
    const std::unordered_map<std::string,std::string>& raw) {

    // {{name}} — raw string injection for SQL construction.
    // Fetches the actual string value so fragments like join clauses,
    // identifiers, and WHERE conditions are inlined verbatim.
    // Check raw first; fall back to env (which holds getvariable() refs).
    for (const auto& [k, v] : raw) {
        std::string placeholder = "{{" + k + "}}";
        size_t pos = 0;
        while ((pos = s.find(placeholder, pos)) != std::string::npos) {
            s.replace(pos, placeholder.size(), v);
            pos += v.size();
        }
    }
    // bare name — typed value reference, injects getvariable() fragment.
    // Only from env (loop vars and for-loop columns).
    // Skips matches inside {{...}} templates and inside SQL string literals
    // so bare substitution never clobbers quoted strings like 'c = ' or column names.
    for (const auto& [k, v] : env) {
        size_t pos = 0;
        while ((pos = s.find(k, pos)) != std::string::npos) {
            bool left_ok = (pos == 0) || (!std::isalnum(s[pos - 1]) && s[pos - 1] != '.' && s[pos - 1] != '_');
            bool right_ok = (pos + k.size() == s.size()) || (!std::isalnum(s[pos + k.size()]) && s[pos + k.size()] != '.' && s[pos + k.size()] != '_');

            // Skip if inside a {{...}} template
            bool in_template = false;
            if (pos >= 2 && s[pos - 1] == '{' && s[pos - 2] == '{') {
                size_t close = s.find("}}", pos + k.size());
                if (close != std::string::npos) in_template = true;
            }

            // Skip if inside a SQL string literal (single-quoted)
            // Count unescaped single quotes before this position — odd count means inside string
            bool in_string = false;
            {
                int quotes = 0;
                for (size_t i = 0; i < pos; i++) {
                    if (s[i] == '\'') {
                        // Skip escaped quotes ('')
                        if (i + 1 < pos && s[i + 1] == '\'') { i++; continue; }
                        quotes++;
                    }
                }
                in_string = (quotes % 2 != 0);
            }

            if (left_ok && right_ok && !in_template && !in_string) {
                s.replace(pos, k.size(), v);
                pos += v.size();
            } else {
                pos += k.size();
            }
        }
    }
    return s;
}

std::string replaceEnvVars(const std::string& input) {
    // Replace env.VAR_NAME with a SQL-safe single-quoted string literal.
    // This ensures paths, secrets, and other env var values with special
    // characters (/, :, spaces) don't break the surrounding SQL.
    // e.g. env.HOME → '/home/user'  (with escaped internal quotes)
    std::string s = input;
    std::regex env_pattern(R"(env\.([A-Za-z_][A-Za-z0-9_]*))");
    std::smatch match;
    size_t pos = 0;

    while (std::regex_search(s.cbegin() + pos, s.cend(), match, env_pattern)) {
        std::string var = match[1].str();
        const char* val = std::getenv(var.c_str());
        std::string raw = val ? val : "";
        // Escape single quotes in the value for SQL safety
        std::string escaped;
        for (char c : raw) {
            if (c == '\'') escaped += "\'\'";
            else escaped += c;
        }
        // Wrap in single quotes so it's a SQL string literal
        std::string replacement = "\'" + escaped + "\'";
        s.replace(pos + match.position(0), match.length(0), replacement);
        pos += match.position(0) + replacement.size();
    }
    return s;
}