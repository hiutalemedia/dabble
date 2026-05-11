#include "parser.h"
#include "interpreter.h"
#include "analyzer.h"
#include "utils.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>

int main(int argc, char** argv) {
    bool verbose   = false;
    bool progress  = false;
    bool deps_mode = false;
    std::string deps_format  = "text";   // text | dot | json
    std::string deps_changed = "";       // node_id for --changed
    std::string deps_upstream = "";     // node_id for --upstream
    std::string deps_sources = "";      // node_id for --sources
    std::string deps_dests = "";        // node_id for --destinations
    std::string log_dest;                   // --log=file destination
    const char* file = nullptr;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--verbose" || arg == "-v") { verbose = true; continue; }
        if (arg == "--progress"|| arg == "-p") { progress = true; continue; }
        if (arg == "--deps"    || arg == "-d") { deps_mode = true; continue; }

        if (arg == "--format" && i + 1 < argc) { deps_format = argv[++i]; continue; }
        if (arg.rfind("--format=", 0) == 0)    { deps_format = arg.substr(9); continue; }

        if (arg == "--changed"      && i + 1 < argc) { deps_changed  = argv[++i]; continue; }
        if (arg.rfind("--changed=", 0) == 0)          { deps_changed  = arg.substr(10); continue; }
        if (arg == "--upstream"     && i + 1 < argc) { deps_upstream = argv[++i]; continue; }
        if (arg.rfind("--upstream=", 0) == 0)         { deps_upstream = arg.substr(11); continue; }
        if (arg == "--sources"      && i + 1 < argc) { deps_sources  = argv[++i]; continue; }
        if (arg.rfind("--sources=", 0) == 0)          { deps_sources  = arg.substr(10); continue; }
        if (arg == "--destinations" && i + 1 < argc) { deps_dests    = argv[++i]; continue; }
        if (arg.rfind("--destinations=", 0) == 0)     { deps_dests    = arg.substr(15); continue; }

        // key=value → environment variable
        auto eq = arg.find('=');
        if (eq != std::string::npos && eq > 0) {
            std::string key = arg.substr(0, eq);
            std::string val = arg.substr(eq + 1);
            bool is_ident = true;
            for (char c : key)
                if (!std::isalnum((unsigned char)c) && c != '_') { is_ident = false; break; }
            if (is_ident) { setenv(key.c_str(), val.c_str(), 1); continue; }
        }

        if (!file) file = argv[i];
    }

    if (!file) {
        std::cout << "Usage: ./dabble [options] <file.dabble> [key=value ...]\n\n"
                  << "Options:\n"
                  << "  --verbose,  -v          show execution trace\n"
                  << "  --progress, -p          show progress bar\n"
                  << "  --log=<file>            write log to file (.db, .csv, .json)\n"
                  << "  --deps,     -d          analyze dependencies (no execution)\n"
                  << "  --format=text|dot|json      deps output format (default: text)\n"
                  << "  --changed=kind:name         show what changing this affects (downstream)\n"
                  << "  --upstream=kind:name        show what this depends on\n"
                  << "  --sources=kind:name         show external data sources for this\n"
                  << "  --destinations=kind:name    show which exports this flows into\n\n"
                  << "Examples:\n"
                  << "  dabble script.dabble month=2026-04\n"
                  << "  dabble --deps script.dabble\n"
                  << "  dabble --deps --changed=projection:money_fmt script.dabble\n"
                  << "  dabble --deps --format=dot script.dabble | dot -Tsvg > graph.svg\n";
        return 1;
    }

    std::ifstream f(file);
    if (!f) { std::cerr << "Cannot open file: " << file << "\n"; return 1; }
    std::stringstream buf;
    buf << f.rdbuf();

    Parser parser(buf.str());
    auto ast = parser.parseBlock();

    // ── Dependency analysis mode — opens DuckDB only for json_serialize_sql ──
    if (deps_mode) {
        duckdb_database db; duckdb_connection conn;
        duckdb_open(nullptr, &db);
        duckdb_connect(db, &conn);

        DependencyGraph graph;
        graph.analyze(ast, file, conn);

        duckdb_disconnect(&conn);
        duckdb_close(&db);

        // Focused queries take priority over full graph output
        if (!deps_upstream.empty())    { graph.printUpstream(deps_upstream); }
        else if (!deps_sources.empty()) { graph.printSources(deps_sources); }
        else if (!deps_dests.empty())   { graph.printDestinations(deps_dests); }
        else if (deps_format == "dot")  { graph.printDot(deps_changed); }
        else if (deps_format == "json") { graph.printJson(); }
        else                            { graph.printText(deps_changed); }
        return 0;
    }

    // ── Normal execution ──────────────────────────────────────────────────────
    duckdb_database db;
    duckdb_connection conn;
    check(duckdb_open(nullptr, &db), "open db");
    check(duckdb_connect(db, &conn), "connect");

    Interpreter interp(conn, verbose, progress);
    if (!log_dest.empty()) interp.setLogDestination(log_dest);
    interp.run(ast, file);

    duckdb_disconnect(&conn);
    duckdb_close(&db);
    return 0;
}