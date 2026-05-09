#pragma once
#include "ast.h"
#include <vector>
#include <string>

class Parser {
    std::vector<std::string> lines;
    int pos = 0;

public:
    Parser(const std::string& src);
    std::vector<ASTPtr> parseBlock(int baseIndent = 0);

private:
    std::string collectSQL(int minIndent, std::string& redirect_file, bool& append);
    ASTPtr parseLet(int baseIndent);    // let / table  → temp table
    ASTPtr parseVal(int baseIndent);    // val / scalar → env string
    ASTPtr parseFor(int baseIndent);
    ASTPtr parseIf(int baseIndent);
    ASTPtr parseWhile(int baseIndent);
    ASTPtr parseExpect(int baseIndent);
    ASTPtr parseFn(int baseIndent);
    ASTPtr parsePrint();
    ASTPtr parseImport();
    ASTPtr parseRawSQL(int baseIndent);
};