#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// utils.h — Shared utilities for the Dabble interpreter
//
// replaceAll is the core substitution engine.  It runs in two passes:
//
//   Pass 1 — {{name}} (raw map)
//     Inlines the actual string value verbatim.  Used for dynamic SQL
//     construction — column aliases, WHERE clause fragments, table names.
//     Example: {{join_part}} → LEFT JOIN products p ON p.id = o.product_id
//
//   Pass 2 — bare name (env map)
//     Injects a getvariable('__val_N_name') call so DuckDB resolves the value
//     with its original type at query time.  Safe for dates, decimals, arrays.
//     Example: cutoff → getvariable('__val_0_cutoff')
//     Skips matches inside {{...}} to avoid clobbering unresolved templates.
//
// replaceEnvVars handles env.VAR_NAME → getenv("VAR_NAME") for CLI/OS env vars.
// ─────────────────────────────────────────────────────────────────────────────
#include "duckdb.h"
#include <string>
#include <unordered_map>

void check(duckdb_state s, const char* msg);
std::string trim(const std::string& s);
int indentLevel(const std::string& line);
// env: bare name substitution (getvariable() fragments)
// raw: {{name}} substitution (actual string values for SQL construction)
std::string replaceAll(std::string s,
    const std::unordered_map<std::string,std::string>& env,
    const std::unordered_map<std::string,std::string>& raw = {});
std::string replaceEnvVars(const std::string& s);