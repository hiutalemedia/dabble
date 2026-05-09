#include "analyzer.h"
#include "utils.h"
#include <unistd.h>
#include <ctime>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <queue>
#include <regex>

// ── Helpers ───────────────────────────────────────────────────────────────────

std::string kindName(NodeKind k) {
    switch (k) {
        case NodeKind::Let:        return "let";
        case NodeKind::Val:        return "val";
        case NodeKind::Fn:         return "fn";
        case NodeKind::Projection: return "projection";
        case NodeKind::Export:     return "export";
        case NodeKind::Mutation:   return "mutation";
        case NodeKind::Source:     return "source";
    }
    return "?";
}

static std::string nodeId(NodeKind k, const std::string& name) {
    return kindName(k) + ":" + name;
}

static std::string shortFile(const std::string& path) {
    auto slash = path.rfind('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

static std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); });
    return s;
}

// ── Graph construction ────────────────────────────────────────────────────────

void DependencyGraph::addNode(DepNode node) {
    nodes_[node.id] = std::move(node);
}

void DependencyGraph::addEdge(const std::string& from_id, const std::string& to_id) {
    if (!nodes_.count(to_id) || from_id == to_id) return;
    auto& refs = nodes_[from_id].refs;
    if (std::find(refs.begin(), refs.end(), to_id) == refs.end())
        refs.push_back(to_id);
    auto& deps = dependents_[to_id];
    if (std::find(deps.begin(), deps.end(), from_id) == deps.end())
        deps.push_back(from_id);
}

// ── SQL reference extraction ──────────────────────────────────────────────────

// Walk DuckDB's json_serialize_sql output and collect all "table_name" values.
// The parse tree is a nested JSON object — we scan for the pattern
// "table_name":"identifier" which appears for every BASE_TABLE_REF node.
// This correctly handles subqueries, CTEs, JOINs, and nested SELECTs.
std::vector<std::string> DependencyGraph::refsFromParseTree(const std::string& json) const {
    std::vector<std::string> found;
    std::unordered_set<std::string> seen;

    // Check for error in serialization
    if (json.find("\"error\":true") != std::string::npos) return found;

    // Scan for "table_name":"value" patterns
    const std::string key = "\"table_name\":\"";
    size_t pos = 0;
    while ((pos = json.find(key, pos)) != std::string::npos) {
        pos += key.size();
        size_t end = json.find('"', pos);
        if (end == std::string::npos) break;
        std::string tbl = json.substr(pos, end - pos);
        pos = end + 1;
        if (tbl.empty()) continue;

        // Map to known Dabble node if it exists
        if (known_lets.count(tbl) && !seen.count(tbl)) {
            found.push_back(nodeId(NodeKind::Let, tbl));
            seen.insert(tbl);
        } else if (known_fns.count(tbl) && !seen.count(tbl)) {
            found.push_back(nodeId(NodeKind::Fn, tbl));
            seen.insert(tbl);
        } else if (!known_lets.count(tbl) && !known_vals.count(tbl) &&
                   !known_fns.count(tbl) && !known_projs.count(tbl) &&
                   !seen.count("source:" + tbl)) {
            // Unknown table → external source
            std::string src_id = "source:" + tbl;
            if (!nodes_.count(src_id)) {
                // We'll register it as a source node after scanning
                // For now just record the name
            }
            found.push_back(src_id);
            seen.insert("source:" + tbl);
        }
    }

    // Also scan for ...projection spreads in the original SQL (not in parse tree)
    return found;
}

// Fallback token scanner when no DuckDB connection available
std::vector<std::string> DependencyGraph::scanTokens(const std::string& sql) const {
    std::vector<std::string> found;
    std::unordered_set<std::string> seen;

    auto add = [&](const std::string& id) {
        if (!seen.count(id)) { found.push_back(id); seen.insert(id); }
    };

    // ...projection spreads
    size_t p = 0;
    while ((p = sql.find("...", p)) != std::string::npos) {
        size_t start = p + 3, end = start;
        while (end < sql.size() &&
               (std::isalnum((unsigned char)sql[end]) || sql[end] == '_')) end++;
        if (end > start) {
            std::string name = sql.substr(start, end - start);
            if (known_projs.count(name)) add(nodeId(NodeKind::Projection, name));
        }
        p = end;
    }

    // Token scan — skip string literals
    std::string tok;
    bool in_single = false, in_double = false;
    for (size_t i = 0; i <= sql.size(); i++) {
        char c = (i < sql.size()) ? sql[i] : 0;
        if (c == '\'' && !in_double) { in_single = !in_single; tok.clear(); continue; }
        if (c == '"'  && !in_single) { in_double = !in_double; tok.clear(); continue; }
        if (in_single || in_double)  { tok.clear(); continue; }

        if (std::isalnum((unsigned char)c) || c == '_') {
            tok += c;
        } else {
            if (!tok.empty() && !std::isdigit((unsigned char)tok[0])) {
                bool is_call = (c == '(');
                if (is_call) {
                    if (known_fns.count(tok)) add(nodeId(NodeKind::Fn, tok));
                } else {
                    if (known_lets.count(tok))  add(nodeId(NodeKind::Let, tok));
                    if (known_vals.count(tok))  add(nodeId(NodeKind::Val, tok));
                    if (known_projs.count(tok)) add(nodeId(NodeKind::Projection, tok));
                }
            }
            tok.clear();
        }
    }
    return found;
}

// Extract target table name from DML statement (UPDATE/INSERT/DELETE/CREATE)
std::string DependencyGraph::dmlTable(const std::string& upper) const {
    // UPDATE table_name SET ...
    // INSERT INTO table_name ...
    // DELETE FROM table_name ...
    // CREATE [OR REPLACE] [TEMP] TABLE table_name ...
    std::regex dml_re(
        R"((?:UPDATE|INSERT\s+INTO|DELETE\s+FROM)\s+([A-Za-z_][A-Za-z0-9_]*))"
        R"||(CREATE\s+(?:OR\s+REPLACE\s+)?(?:TEMP\s+)?TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?([A-Za-z_][A-Za-z0-9_]*))||",
        std::regex::icase
    );
    std::smatch m;
    if (std::regex_search(upper, m, dml_re)) {
        // group 1 for UPDATE/INSERT/DELETE, group 2 for CREATE TABLE
        if (m[1].matched && !m[1].str().empty()) return m[1].str();
        if (m[2].matched && !m[2].str().empty()) return m[2].str();
    }
    return "";
}

std::vector<std::string> DependencyGraph::refsFromSql(const std::string& sql) const {
    if (sql.empty()) return {};

    std::string up = toUpper(trim(sql));

    // First always scan for ...projection and known-name tokens
    auto base_refs = scanTokens(sql);

    // DML: extract mutation target, don't try json_serialize_sql
    if (up.rfind("UPDATE", 0) == 0 || up.rfind("INSERT", 0) == 0 ||
        up.rfind("DELETE", 0) == 0) {
        // For DML we return the refs from token scan (which tables it reads)
        // The mutation target is handled separately in walkBlock
        return base_refs;
    }

    // SELECT/WITH: use json_serialize_sql for accurate table extraction
    if (conn_ && (up.rfind("SELECT", 0) == 0 || up.rfind("WITH", 0) == 0)) {
        // Escape single quotes in sql for the query string
        std::string escaped;
        for (char c : sql) {
            if (c == '\'') escaped += "''";
            else escaped += c;
        }
        std::string query = "SELECT json_serialize_sql('" + escaped + "')";
        duckdb_result res;
        duckdb_state state = duckdb_query(conn_, query.c_str(), &res);
        if (state == DuckDBSuccess && duckdb_row_count(&res) > 0) {
            char* val = duckdb_value_varchar(&res, 0, 0);
            if (val) {
                std::string json = val;
                duckdb_free(val);
                duckdb_destroy_result(&res);

                auto parse_refs = refsFromParseTree(json);
                // Merge: parse tree refs + projection spreads from token scan
                std::unordered_set<std::string> seen(parse_refs.begin(), parse_refs.end());
                for (auto& r : base_refs) {
                    if (seen.find(r) == seen.end()) {
                        parse_refs.push_back(r);
                        seen.insert(r);
                    }
                }
                return parse_refs;
            }
        }
        duckdb_destroy_result(&res);
    }

    return base_refs;
}

// ── Source node registration ──────────────────────────────────────────────────
// Extract read_csv/read_parquet/read_json calls from SQL as source nodes

static std::vector<std::string> extractSources(const std::string& sql) {
    std::vector<std::string> sources;
    std::regex src_re(R"(read_(?:csv|parquet|json|ndjson)\s*\(\s*'([^']+)')",
                      std::regex::icase);
    auto begin = std::sregex_iterator(sql.begin(), sql.end(), src_re);
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        sources.push_back((*it)[1].str());
    }
    return sources;
}

// ── AST walking ───────────────────────────────────────────────────────────────

void DependencyGraph::analyze(const std::vector<ASTPtr>& ast,
                               const std::string& file,
                               duckdb_connection conn) {
    conn_         = conn;
    current_file_ = shortFile(file);
    walkBlock(ast);
}

void DependencyGraph::walkBlock(const std::vector<ASTPtr>& block) {
    for (auto& n : block) {
        std::visit([&](auto&& stmt) {
            using T = std::decay_t<decltype(stmt)>;

            // ── let ──────────────────────────────────────────────────────────
            if constexpr (std::is_same_v<T, LetStmt>) {
                std::string id = nodeId(NodeKind::Let, stmt.name);
                DepNode node;
                node.id = id; node.kind = NodeKind::Let;
                node.name = stmt.name; node.file = current_file_; node.line = n->line_no;
                addNode(node);
                known_lets.insert(stmt.name);

                // Register file sources inside the SQL
                for (auto& src : extractSources(stmt.sql)) {
                    std::string src_id = "source:" + src;
                    if (!nodes_.count(src_id)) {
                        DepNode sn; sn.id = src_id; sn.kind = NodeKind::Source;
                        sn.name = src; sn.file = current_file_; sn.line = n->line_no;
                        addNode(sn);
                    }
                    nodes_[id].refs.push_back(src_id);
                    addEdge(id, src_id);
                }

                // SQL refs
                for (auto& ref : refsFromSql(stmt.sql)) {
                    // Register unknown source nodes on the fly
                    if (ref.rfind("source:", 0) == 0 && !nodes_.count(ref)) {
                        DepNode sn; sn.id = ref; sn.kind = NodeKind::Source;
                        sn.name = ref.substr(7); sn.file = current_file_; sn.line = n->line_no;
                        addNode(sn);
                    }
                    addEdge(id, ref);
                }

            // ── val ──────────────────────────────────────────────────────────
            } else if constexpr (std::is_same_v<T, ValStmt>) {
                std::string id = nodeId(NodeKind::Val, stmt.name);
                DepNode node;
                node.id = id; node.kind = NodeKind::Val;
                node.name = stmt.name; node.file = current_file_; node.line = n->line_no;
                addNode(node);
                known_vals.insert(stmt.name);
                for (auto& ref : refsFromSql(stmt.expr)) addEdge(id, ref);

            // ── fn ───────────────────────────────────────────────────────────
            } else if constexpr (std::is_same_v<T, FnStmt>) {
                std::string id = nodeId(NodeKind::Fn, stmt.name);
                DepNode node;
                node.id = id; node.kind = NodeKind::Fn;
                node.name = stmt.name; node.file = current_file_; node.line = n->line_no;
                addNode(node);
                known_fns.insert(stmt.name);

                // Scan all SQL inside the fn body
                for (auto& bn : stmt.body) {
                    std::visit([&](auto&& bs) {
                        using BT = std::decay_t<decltype(bs)>;
                        std::string sql;
                        if constexpr (std::is_same_v<BT, LetStmt>) sql = bs.sql;
                        else if constexpr (std::is_same_v<BT, ValStmt>) sql = bs.expr;
                        else if constexpr (std::is_same_v<BT, SQLStmt>) sql = bs.sql;
                        if (!sql.empty()) {
                            for (auto& src : extractSources(sql)) {
                                std::string src_id = "source:" + src;
                                if (!nodes_.count(src_id)) {
                                    DepNode sn; sn.id = src_id; sn.kind = NodeKind::Source;
                                    sn.name = src; sn.file = current_file_; sn.line = n->line_no;
                                    addNode(sn);
                                }
                                addEdge(id, src_id);
                            }
                            for (auto& ref : refsFromSql(sql)) {
                                if (ref.rfind("source:", 0) == 0 && !nodes_.count(ref)) {
                                    DepNode sn; sn.id = ref; sn.kind = NodeKind::Source;
                                    sn.name = ref.substr(7); sn.file = current_file_; sn.line = n->line_no;
                                    addNode(sn);
                                }
                                addEdge(id, ref);
                            }
                        }
                    }, bn->node);
                }

            // ── projection ───────────────────────────────────────────────────
            } else if constexpr (std::is_same_v<T, ProjectionStmt>) {
                std::string id = nodeId(NodeKind::Projection, stmt.name);
                DepNode node;
                node.id = id; node.kind = NodeKind::Projection;
                node.name = stmt.name; node.file = current_file_; node.line = n->line_no;
                addNode(node);
                known_projs.insert(stmt.name);
                for (auto& ref : scanTokens(stmt.cols)) addEdge(id, ref);

            // ── raw SQL: exports and mutations ───────────────────────────────
            } else if constexpr (std::is_same_v<T, SQLStmt>) {
                std::string up = toUpper(trim(stmt.sql));

                if (!stmt.redirect_file.empty()) {
                    // Export node
                    std::string id = "export:" + stmt.redirect_file;
                    if (!nodes_.count(id)) {
                        DepNode node; node.id = id; node.kind = NodeKind::Export;
                        node.name = stmt.redirect_file; node.file = current_file_; node.line = n->line_no;
                        addNode(node);
                    }
                    for (auto& ref : refsFromSql(stmt.sql)) addEdge(id, ref);

                } else if (up.rfind("UPDATE", 0) == 0 || up.rfind("INSERT", 0) == 0 ||
                           up.rfind("DELETE", 0) == 0) {
                    // Mutation node
                    std::string tbl = dmlTable(up);
                    if (!tbl.empty()) {
                        std::string id = "mutation:" + tbl + "_L" + std::to_string(n->line_no);
                        DepNode node; node.id = id; node.kind = NodeKind::Mutation;
                        node.name = tbl + " (L" + std::to_string(n->line_no) + ")";
                        node.file = current_file_; node.line = n->line_no;
                        node.is_mutation = true;
                        addNode(node);

                        // The mutated table is a dependency (it must exist)
                        if (known_lets.count(tbl)) addEdge(id, nodeId(NodeKind::Let, tbl));

                        // Other tables read in the statement
                        for (auto& ref : refsFromSql(stmt.sql)) addEdge(id, ref);

                        // Anything that reads the mutated let table now depends on this mutation
                        if (known_lets.count(tbl)) {
                            addEdge(nodeId(NodeKind::Let, tbl), id);
                        }
                    }
                }

            // ── control flow: recurse ─────────────────────────────────────────
            } else if constexpr (std::is_same_v<T, ForStmt>) {
                walkBlock(stmt.body);
            } else if constexpr (std::is_same_v<T, IfStmt>) {
                walkBlock(stmt.thenb);
                walkBlock(stmt.elseb);
            } else if constexpr (std::is_same_v<T, WhileStmt>) {
                walkBlock(stmt.body);
            }
            // PrintStmt, ImportStmt, ExpectStmt, ValStmt inside blocks: skip

        }, n->node);
    }
}

// ── Graph queries ─────────────────────────────────────────────────────────────

std::vector<std::string> DependencyGraph::affected(const std::string& node_id) const {
    std::vector<std::string> result;
    std::unordered_set<std::string> visited;
    std::queue<std::string> q;
    q.push(node_id);
    visited.insert(node_id);
    while (!q.empty()) {
        std::string cur = q.front(); q.pop();
        if (cur != node_id) result.push_back(cur);
        auto it = dependents_.find(cur);
        if (it != dependents_.end()) {
            for (const auto& dep : it->second) {
                if (!visited.count(dep)) { visited.insert(dep); q.push(dep); }
            }
        }
    }
    return result;
}


// ── Upstream traversal ────────────────────────────────────────────────────────
// BFS through refs_ (forward edges) — everything node_id depends on.

std::vector<std::string> DependencyGraph::upstream(const std::string& node_id) const {
    std::vector<std::string> result;
    std::unordered_set<std::string> visited;
    std::queue<std::string> q;
    q.push(node_id);
    visited.insert(node_id);
    while (!q.empty()) {
        std::string cur = q.front(); q.pop();
        if (cur != node_id) result.push_back(cur);
        auto it = nodes_.find(cur);
        if (it == nodes_.end()) continue;
        for (const auto& ref : it->second.refs) {
            if (!visited.count(ref)) { visited.insert(ref); q.push(ref); }
        }
    }
    return result;
}

// Leaf sources: upstream nodes with no refs (data origins — files, external tables)
std::vector<std::string> DependencyGraph::sources(const std::string& node_id) const {
    auto up = upstream(node_id);
    std::vector<std::string> result;
    for (const auto& id : up) {
        auto it = nodes_.find(id);
        if (it == nodes_.end()) continue;
        // A source is either: NodeKind::Source, OR any node with no outgoing refs
        if (it->second.kind == NodeKind::Source || it->second.refs.empty()) {
            result.push_back(id);
        }
    }
    return result;
}

// Export destinations: downstream nodes that are exports
std::vector<std::string> DependencyGraph::destinations(const std::string& node_id) const {
    auto down = affected(node_id);
    std::vector<std::string> result;
    for (const auto& id : down) {
        auto it = nodes_.find(id);
        if (it == nodes_.end()) continue;
        if (it->second.kind == NodeKind::Export) result.push_back(id);
    }
    return result;
}

// ── Focused print methods ─────────────────────────────────────────────────────

void DependencyGraph::printUpstream(const std::string& node_id) const {
    bool colors = isatty(STDOUT_FILENO);
    const char* BOLD  = colors ? "\033[1m"    : "";
    const char* DIM   = colors ? "\033[2m"    : "";
    const char* CYAN  = colors ? "\033[1;36m" : "";
    const char* GREEN = colors ? "\033[1;32m" : "";
    const char* RESET = colors ? "\033[0m"    : "";

    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        std::cerr << "Unknown node: " << node_id << "\n"; return;
    }

    std::cout << BOLD << "Upstream of " << node_id << RESET << ":\n";
    auto up = upstream(node_id);
    if (up.empty()) { std::cout << DIM << "  (no dependencies)\n" << RESET; return; }

    for (const auto& id : up) {
        auto nit = nodes_.find(id);
        if (nit == nodes_.end()) continue;
        const char* color = (nit->second.kind == NodeKind::Source) ? GREEN : CYAN;
        std::cout << color << "  " << kindName(nit->second.kind) << ":" << nit->second.name
                  << RESET << DIM << "  " << nit->second.file << ":" << nit->second.line
                  << RESET << "\n";
    }
}

void DependencyGraph::printSources(const std::string& node_id) const {
    bool colors = isatty(STDOUT_FILENO);
    const char* BOLD  = colors ? "\033[1m"    : "";
    const char* DIM   = colors ? "\033[2m"    : "";
    const char* GREEN = colors ? "\033[1;32m" : "";
    const char* RESET = colors ? "\033[0m"    : "";

    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        std::cerr << "Unknown node: " << node_id << "\n"; return;
    }

    std::cout << BOLD << "Data sources for " << node_id << RESET << ":\n";
    auto srcs = sources(node_id);
    if (srcs.empty()) { std::cout << DIM << "  (no external sources found)\n" << RESET; return; }

    for (const auto& id : srcs) {
        auto nit = nodes_.find(id);
        if (nit == nodes_.end()) continue;
        std::cout << GREEN << "  " << kindName(nit->second.kind) << ":" << nit->second.name
                  << RESET << DIM << "  " << nit->second.file << ":" << nit->second.line
                  << RESET << "\n";
    }
}

void DependencyGraph::printDestinations(const std::string& node_id) const {
    bool colors = isatty(STDOUT_FILENO);
    const char* BOLD   = colors ? "\033[1m"    : "";
    const char* DIM    = colors ? "\033[2m"    : "";
    const char* YELLOW = colors ? "\033[1;33m" : "";
    const char* RESET  = colors ? "\033[0m"    : "";

    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        std::cerr << "Unknown node: " << node_id << "\n"; return;
    }

    std::cout << BOLD << "Export destinations for " << node_id << RESET << ":\n";
    auto dests = destinations(node_id);
    if (dests.empty()) {
        std::cout << DIM << "  (not exported to any file)\n" << RESET; return;
    }

    for (const auto& id : dests) {
        auto nit = nodes_.find(id);
        if (nit == nodes_.end()) continue;
        std::cout << YELLOW << "  " << nit->second.name
                  << RESET << DIM << "  " << nit->second.file << ":" << nit->second.line
                  << RESET << "\n";
    }
}

// ── Output: Text ──────────────────────────────────────────────────────────────

void DependencyGraph::printText(const std::string& highlight) const {
    bool colors = isatty(STDOUT_FILENO);
    const char* BOLD  = colors ? "\033[1m"    : "";
    const char* DIM   = colors ? "\033[2m"    : "";
    const char* RED   = colors ? "\033[1;31m" : "";
    const char* CYAN  = colors ? "\033[1;36m" : "";
    const char* GREEN = colors ? "\033[1;32m" : "";
    const char* RESET = colors ? "\033[0m"    : "";

    std::unordered_set<std::string> highlighted;
    if (!highlight.empty()) {
        auto aff = affected(highlight);
        highlighted.insert(aff.begin(), aff.end());
        highlighted.insert(highlight);
    }

    // Group nodes by kind for cleaner output
    std::vector<NodeKind> order = {
        NodeKind::Source, NodeKind::Projection, NodeKind::Val,
        NodeKind::Fn, NodeKind::Let, NodeKind::Mutation, NodeKind::Export
    };

    for (auto kind : order) {
        bool printed_header = false;
        for (const auto& [id, node] : nodes_) {
            if (node.kind != kind) continue;
            if (!printed_header) {
                std::cout << "\n" << BOLD << kindName(kind) << "s:" << RESET << "\n";
                printed_header = true;
            }
            bool is_changed  = (id == highlight);
            bool is_affected = highlighted.count(id) && !is_changed;

            const char* color = is_changed ? RED : (is_affected ? CYAN : "");
            std::string marker = is_changed ? " ← changed" : (is_affected ? " ← affected" : "");

            std::cout << color << "  " << BOLD << node.name << RESET
                      << DIM << "  " << node.file << ":" << node.line << RESET
                      << (is_changed ? RED : CYAN) << marker << RESET << "\n";

            for (const auto& ref : node.refs) {
                auto it = nodes_.find(ref);
                if (it == nodes_.end()) continue;
                bool ref_hl = highlighted.count(ref);
                std::cout << (ref_hl ? GREEN : DIM)
                          << "    └── " << kindName(it->second.kind) << ":" << it->second.name
                          << RESET << "\n";
            }
        }
    }

    if (!highlight.empty()) {
        auto aff = affected(highlight);
        std::cout << "\n" << BOLD << "Changing " << highlight
                  << " affects " << aff.size() << " node(s):" << RESET << "\n";
        for (const auto& id : aff) {
            auto it = nodes_.find(id);
            if (it == nodes_.end()) continue;
            std::cout << CYAN << "  " << kindName(it->second.kind)
                      << ":" << it->second.name
                      << DIM << "  " << it->second.file << ":" << it->second.line
                      << RESET << "\n";
        }
    }
}

// ── Output: DOT ───────────────────────────────────────────────────────────────

void DependencyGraph::printDot(const std::string& highlight) const {
    std::unordered_set<std::string> highlighted;
    if (!highlight.empty()) {
        auto aff = affected(highlight);
        highlighted.insert(aff.begin(), aff.end());
        highlighted.insert(highlight);
    }

    auto kindColor = [](NodeKind k) -> std::string {
        switch (k) {
            case NodeKind::Let:        return "#dbeafe";  // blue
            case NodeKind::Val:        return "#dcfce7";  // green
            case NodeKind::Fn:         return "#fef9c3";  // yellow
            case NodeKind::Projection: return "#fce7f3";  // pink
            case NodeKind::Export:     return "#f3f4f6";  // grey
            case NodeKind::Mutation:   return "#fee2e2";  // red-light
            case NodeKind::Source:     return "#e0f2fe";  // sky
        }
        return "#ffffff";
    };
    auto kindShape = [](NodeKind k) -> std::string {
        if (k == NodeKind::Export)   return "folder";
        if (k == NodeKind::Source)   return "cylinder";
        if (k == NodeKind::Mutation) return "diamond";
        return "box";
    };

    std::cout << "digraph dabble {\n  rankdir=LR;\n"
              << "  node [fontname=\"monospace\" style=filled];\n\n";

    for (const auto& [id, node] : nodes_) {
        bool hl = highlighted.count(id);
        std::string label = kindName(node.kind) + ":\\n" + node.name;
        std::cout << "  \"" << id << "\" [label=\"" << label << "\""
                  << " shape=" << kindShape(node.kind)
                  << " fillcolor=\"" << kindColor(node.kind) << "\""
                  << (hl ? " penwidth=2 color=\"#dc2626\"" : "") << "];\n";
    }
    std::cout << "\n";
    for (const auto& [id, node] : nodes_) {
        for (const auto& ref : node.refs) {
            if (!nodes_.count(ref)) continue;
            bool hl = highlighted.count(id) && highlighted.count(ref);
            bool mutation = node.kind == NodeKind::Mutation;
            std::cout << "  \"" << id << "\" -> \"" << ref << "\""
                      << (mutation ? " [style=dashed]" : "")
                      << (hl ? " [color=\"#dc2626\" penwidth=2]" : "") << ";\n";
        }
    }
    std::cout << "}\n";
}

// ── Output: JSON ──────────────────────────────────────────────────────────────

void DependencyGraph::printJson() const {
    auto esc = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if      (c == '"')  { out += '\\'; out += '"';  }
            else if (c == '\\') { out += '\\'; out += '\\'; }
            else out += c;
        }
        return out;
    };

    std::time_t now = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));

    std::cout << "{\n"
              << "  \"script\": \"" << esc(current_file_) << "\",\n"
              << "  \"analyzed_at\": \"" << ts << "\",\n"
              << "  \"nodes\": [\n";

    bool first = true;
    for (const auto& [id, node] : nodes_) {
        if (!first) std::cout << ",\n";
        first = false;
        std::cout << "    {\"id\":\"" << esc(id) << "\""
                  << ",\"kind\":\"" << kindName(node.kind) << "\""
                  << ",\"name\":\"" << esc(node.name) << "\""
                  << ",\"file\":\"" << esc(node.file) << "\""
                  << ",\"line\":" << node.line
                  << ",\"is_mutation\":" << (node.is_mutation ? "true" : "false")
                  << ",\"refs\":[";
        for (size_t i = 0; i < node.refs.size(); i++) {
            if (i) std::cout << ",";
            std::cout << "\"" << esc(node.refs[i]) << "\"";
        }
        std::cout << "]}";
    }
    std::cout << "\n  ]\n}\n";
}