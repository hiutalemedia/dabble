#pragma once
#include "ast.h"
#include "duckdb.h"
#include <unistd.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <set>

// ── Dependency node ───────────────────────────────────────────────────────────

enum class NodeKind {
    Let,        // let / table
    Val,        // val / scalar
    Fn,         // fn definition
    Projection, // projection / proj / cols / columns
    Export,     // -> or >> target file
    Mutation,   // UPDATE / INSERT / DELETE / CREATE — writes to a table
    Source,     // external data source: read_csv, read_parquet, raw table name
};

std::string kindName(NodeKind k);

struct DepNode {
    std::string              id;
    NodeKind                 kind = NodeKind::Source;
    std::string              name;
    std::string              file;
    int                      line = 0;
    bool                     is_mutation = false;  // writes rather than reads
    std::vector<std::string> refs;                 // node ids this depends on
};

// ── Dependency graph ──────────────────────────────────────────────────────────

class DependencyGraph {
public:
    // Analyze AST. Optionally pass a DuckDB connection for json_serialize_sql
    // on SELECT statements — gives accurate table extraction from complex SQL.
    // If conn is nullptr, falls back to identifier-based scanning.
    void analyze(const std::vector<ASTPtr>& ast,
                 const std::string& file,
                 duckdb_connection conn = nullptr);

    // Downstream: everything transitively affected by changing node_id
    std::vector<std::string> affected(const std::string& node_id) const;

    // Upstream: all nodes node_id transitively depends on
    std::vector<std::string> upstream(const std::string& node_id) const;

    // Leaf sources: upstream nodes with no dependencies (data origins)
    std::vector<std::string> sources(const std::string& node_id) const;

    // Export destinations: downstream nodes that are exports
    std::vector<std::string> destinations(const std::string& node_id) const;

    // Output modes
    void printText(const std::string& highlight = "") const;
    void printDot (const std::string& highlight = "") const;
    void printJson() const;

    // Focused query outputs
    void printUpstream(const std::string& node_id) const;
    void printSources(const std::string& node_id) const;
    void printDestinations(const std::string& node_id) const;

    const std::unordered_map<std::string, DepNode>& nodes() const { return nodes_; }

private:
    std::unordered_map<std::string, DepNode>              nodes_;
    std::unordered_map<std::string, std::vector<std::string>> dependents_;

    std::set<std::string> known_lets;
    std::set<std::string> known_vals;
    std::set<std::string> known_fns;
    std::set<std::string> known_projs;

    std::string       current_file_;
    duckdb_connection conn_ = nullptr;

    void addNode(DepNode node);
    void addEdge(const std::string& from_id, const std::string& to_id);
    void walkBlock(const std::vector<ASTPtr>& block);

    // Extract table references from SQL:
    //   SELECT/WITH → json_serialize_sql if conn available, else scanTokens
    //   UPDATE/INSERT/DELETE/CREATE → regex on first two words
    std::vector<std::string> refsFromSql(const std::string& sql) const;

    // json_serialize_sql path: recursively extract "table_name" values
    // from DuckDB's parse tree JSON, filtered to known Dabble entities
    std::vector<std::string> refsFromParseTree(const std::string& json) const;

    // Fallback: token-based scan for known entity names
    std::vector<std::string> scanTokens(const std::string& sql) const;

    // DML: extract the primary target table from UPDATE/INSERT/DELETE/CREATE
    std::string dmlTable(const std::string& upper_sql) const;
};