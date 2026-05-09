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
    int c = 0;
    for (char ch : line) {
        if (ch == ' ') c++;
        else if (ch == '\t') c += 4;
        else break;
    }
    return c;
}

std::string replaceAll(std::string s, const std::unordered_map<std::string,std::string>& env) {
    // Explicit {{var.col}} style
    for (const auto& [k, v] : env) {
        std::string placeholder = "{{" + k + "}}";
        size_t pos = 0;
        while ((pos = s.find(placeholder, pos)) != std::string::npos) {
            s.replace(pos, placeholder.size(), v);
            pos += v.size();
        }
    }

    // Shorthand f.name with boundaries
    for (const auto& [k, v] : env) {
        size_t pos = 0;
        while ((pos = s.find(k, pos)) != std::string::npos) {
            bool left_ok = (pos == 0) || (!std::isalnum(s[pos - 1]) && s[pos - 1] != '.');
            bool right_ok = (pos + k.size() == s.size()) || !std::isalnum(s[pos + k.size()]);

            if (left_ok && right_ok) {
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
    std::string s = input;
    std::regex env_pattern(R"(env\.([A-Za-z_][A-Za-z0-9_]*))");
    std::smatch match;
    size_t pos = 0;

    while (std::regex_search(s.cbegin() + pos, s.cend(), match, env_pattern)) {
        std::string var = match[1].str();
        const char* val = std::getenv(var.c_str());
        std::string replacement = val ? val : "";
        s.replace(pos + match.position(0), match.length(0), replacement);
        pos += match.position(0) + replacement.size();
    }
    return s;
}