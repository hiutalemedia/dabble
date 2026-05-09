# Dabble for VS Code

Syntax highlighting for [Dabble](https://github.com/yourname/dabble) — a lightweight scripting layer for DuckDB.

## Install

Copy this folder to your VS Code extensions directory:

```bash
# macOS / Linux
cp -r vscode-dabble ~/.vscode/extensions/

# Windows
cp -r vscode-dabble %USERPROFILE%\.vscode\extensions\
```

Restart VS Code. Files ending in `.dabble` will automatically use Dabble highlighting.

## What gets highlighted

- **Dabble keywords** — `let`, `val`, `scalar`, `table`, `for`, `while`, `if`, `else`, `fn`, `expect`, `check`, `print`, `import`
- **`{{variables}}`** — template interpolation in a distinct color
- **`->`  and `>>`** — export operators
- **SQL keywords** — `SELECT`, `FROM`, `WHERE`, `JOIN`, etc.
- **SQL functions** — `COUNT`, `SUM`, `ROUND`, `READ_PARQUET`, `GETVARIABLE`, etc.
- **Strings** with `{{interpolation}}` highlighted inside them
- **Comments** — `--`
- **Auto-indent** after `:` (block starters)
- **Auto-close** for `(`, `'`, `{{`