#pragma once
#include "duckdb.h"
#include <string>
#include <unordered_map>

void check(duckdb_state s, const char* msg);
std::string trim(const std::string& s);
int indentLevel(const std::string& line);
std::string replaceAll(std::string s, const std::unordered_map<std::string,std::string>& env);
std::string replaceEnvVars(const std::string& s);