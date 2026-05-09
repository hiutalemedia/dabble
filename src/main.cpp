#include "parser.h"
#include "interpreter.h"
#include "utils.h"
#include <iostream>
#include <fstream>
#include <sstream>

int main(int argc, char** argv) {
    bool verbose = false;
    const char* file = nullptr;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (!file) {
            file = argv[i];
        }
    }

    if (!file) {
        std::cout << "Usage: ./dabble [--verbose] <file.sql>\n";
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

    Interpreter interp(conn, verbose);
    interp.run(ast, file);

    duckdb_disconnect(&conn);
    duckdb_close(&db);

    return 0;
}