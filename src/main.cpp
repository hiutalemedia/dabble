#include "parser.h"
#include "interpreter.h"
#include "utils.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>

int main(int argc, char** argv) {
    bool verbose  = false;
    bool progress = false;
    const char* file = nullptr;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--verbose" || arg == "-v") {
            verbose = true;
            continue;
        }
        if (arg == "--progress" || arg == "-p") {
            progress = true;
            continue;
        }

        // key=value pairs become environment variables accessible via env.KEY
        auto eq = arg.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string key = arg.substr(0, eq);
            std::string val = arg.substr(eq + 1);
            // Only treat as key=value if key is a plain identifier
            bool is_ident = true;
            for (char c : key) {
                if (!std::isalnum((unsigned char)c) && c != '_') {
                    is_ident = false; break;
                }
            }
            if (is_ident) {
                setenv(key.c_str(), val.c_str(), 1);  // 1 = overwrite
                continue;
            }
        }

        if (!file) {
            file = argv[i];
        }
    }

    if (!file) {
        std::cout << "Usage: ./dabble [--verbose] [--progress] <file.dabble> [key=value ...]\n";
        std::cout << "  key=value pairs are available inside scripts as env.key\n";
        return 1;
    }

    std::ifstream f(file);
    if (!f) {
        std::cerr << "Cannot open file: " << file << "\n";
        return 1;
    }

    std::stringstream buf;
    buf << f.rdbuf();

    duckdb_database db;
    duckdb_connection conn;
    check(duckdb_open(nullptr, &db), "open db");
    check(duckdb_connect(db, &conn), "connect");

    Parser parser(buf.str());
    auto ast = parser.parseBlock();

    Interpreter interp(conn, verbose, progress);
    interp.run(ast, file);

    duckdb_disconnect(&conn);
    duckdb_close(&db);

    return 0;
}