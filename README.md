# Dabble

> **If it dabbles like a duck, it's probably Dabble.**

Dabble is a lightweight scripting layer for DuckDB.

It adds:
- scalar and table variables
- loops
- functions
- validation with `expect` syntax
- control flow

…without leaving SQL.

DuckDB is still the engine you drive with. Dabble is just the gearbox that gives you the controls to sequence it. It aims to be lightweight, fast, and pleasant to read and write.

---

> ⚠️ **This is still an experimental project**, although already useful. Please do not use it in production, critical pipelines, or anywhere that matters. It will probably eat your data, steal the breadcrumbs, and leave wet duck tracks on the floor.

---

## Why

A real data pipeline in Python looks something like this:

```python
import duckdb
from datetime import date, timedelta

conn = duckdb.connect()

# Load and clean
conn.execute("CREATE TEMP TABLE raw AS SELECT * FROM 'orders.parquet'")
conn.execute("""
    CREATE TEMP TABLE clean AS
    SELECT * FROM raw
    WHERE amount > 0 AND customer_id IS NOT NULL AND status != 'test'
""")

# Compute thresholds dynamically
cutoff = date.today() - timedelta(days=30)
avg_row = conn.execute("SELECT AVG(amount) FROM clean").fetchone()
avg = avg_row[0]

# Branch on results
count_row = conn.execute(
    f"SELECT COUNT(*) FROM clean WHERE created_at > '{cutoff}'"
).fetchone()

if count_row[0] == 0:
    raise Exception("No recent orders — something is wrong")

# Per-customer report
customers = conn.execute("SELECT DISTINCT customer_id FROM clean").fetchall()
for (cid,) in customers:
    row = conn.execute(f"""
        SELECT COUNT(*), SUM(amount)
        FROM clean
        WHERE customer_id = '{cid}' AND amount > {avg}
    """).fetchone()
    if row[0] > 0:
        print(f"Customer {cid}: {row[0]} orders above average, total ${row[1]:.2f}")

# Validate and export
dup_row = conn.execute("""
    SELECT COUNT(*) FROM (
        SELECT order_id, COUNT(*) FROM clean GROUP BY order_id HAVING COUNT(*) > 1
    )
""").fetchone()
if dup_row[0] > 0:
    print(f"WARNING: {dup_row[0]} duplicate order IDs found")

conn.execute(f"""
    COPY (SELECT * FROM clean WHERE created_at > '{cutoff}' ORDER BY created_at DESC)
    TO 'recent_orders.csv' (FORMAT CSV, HEADER)
""")
```

The same pipeline in Dabble:

```sql
-- Load and clean
let raw   = SELECT * FROM 'orders.parquet'
let clean = SELECT * FROM raw
    WHERE amount > 0 AND customer_id IS NOT NULL AND status != 'test'

-- Compute thresholds
val cutoff = CURRENT_DATE - INTERVAL 30 DAYS
val avg    = SELECT AVG(amount) FROM clean

-- Validate recent data
expect (COUNT(*) > 0 FROM clean WHERE created_at > cutoff)
    else fail 'No recent orders — something is wrong'

-- Per-customer breakdown
for c in (SELECT DISTINCT customer_id FROM clean):
    let above = SELECT COUNT(*) AS cnt, SUM(amount) AS total
        FROM clean WHERE customer_id = {{c.customer_id}} AND amount > avg
    if (cnt > 0 FROM above):
        print c.customer_id || ': ' || cnt || ' orders above average, total $' || ROUND(total, 2)

-- Warn on duplicates
let dupes = SELECT order_id, COUNT(*) AS n FROM clean GROUP BY order_id HAVING n > 1
expect (COUNT(*) = 0 FROM dupes) else warn 'duplicate order IDs found'

-- Export
SELECT * FROM clean WHERE created_at > cutoff ORDER BY created_at DESC > recent_orders.csv
```

No dataframes. No string-interpolated SQL. No context switching. Just SQL with a script to run in.

---

## Install

Requires CMake 3.20+ and a C++20 compiler. DuckDB is downloaded automatically.

```bash
git clone https://github.com/yourname/dabble
cd dabble
cmake -B build
cmake --build build -j
```

Run a script:

```bash
./build/bin/dabble myscript.sql
./build/bin/dabble --verbose myscript.sql   # show execution trace
```

---

## Language Reference

Dabble scripts are plain `.sql` files — VS Code syntax highlighting works out of the box. Comments are `--`.

### Tables — `let` / `table`

Materialises a query into a DuckDB temp table. Lives for the duration of the script.

```sql
let paid  = SELECT * FROM orders WHERE status = 'paid'
let report =
    SELECT o.*, p.name, p.category
    FROM orders o
    JOIN products p ON p.id = o.product_id
    WHERE o.status = 'paid'
    ORDER BY o.id
```

`let` and `table` are aliases. Use whichever reads better.

---

### Scalars — `val` / `scalar`

Stores a single typed value. Under the hood it lives in a DuckDB temp table so the original type (`DATE`, `DECIMAL`, `INTERVAL`, etc.) is preserved — no string casting, no quoting surprises.

```sql
val threshold  = 500
val cutoff     = CURRENT_DATE - INTERVAL 30 DAYS
scalar label   = 'monthly report'
val total      = SELECT SUM(price * qty) FROM paid
```

`val` and `scalar` are aliases. Only write `SELECT` when you need a `FROM` clause.

Scalars substitute as properly-typed subquery fragments, so comparisons, date math, and arithmetic all work exactly as they would in native SQL:

```sql
val cutoff = CURRENT_DATE - INTERVAL 30 DAYS
val avg    = SELECT AVG(amount) FROM orders

SELECT * FROM orders WHERE created_at > cutoff AND amount > avg
```

Reference with `{{name}}` (explicit) or bare `name` surrounded by spaces or punctuation (implicit).

---

### Functions — `fn`

Functions build a self-contained CTE chain and return the last `SELECT`. They never touch the database until called — everything is lazy.

```sql
fn paid_summary():
    let enriched = SELECT o.*, p.category, p.price
        FROM orders o JOIN products p ON p.id = o.product_id
        WHERE o.status = 'paid'
    SELECT
        category,
        COUNT(*)         AS orders,
        SUM(qty * price) AS revenue
    FROM enriched
    GROUP BY category
    ORDER BY revenue DESC

-- Materialise into a temp table:
let summary = paid_summary()

-- Or run and print directly:
paid_summary()
```

`let` inside a function becomes a CTE — not a real temp table — so the whole function compiles down to a single `WITH` chain that DuckDB optimises as one query. `val` inside a function is scoped and dropped on return.

---

### For loops — `for`

Iterates over every row of a table or inline query.

```sql
for c in customers:
    print c.name || ' — $' || c.total_spent

-- Inline query as source:
for row in (SELECT category, SUM(revenue) AS rev FROM summary GROUP BY category):
    print row.category || ': $' || ROUND(row.rev, 2)

-- Large files work too:
for row in (SELECT * FROM read_parquet('events/*.parquet')):
    print row.event_type || ': ' || row.ts
```

---

### If / Else — `if`

Any SQL expression or query, evaluated by DuckDB. `SELECT` is optional when a `FROM` clause is present.

```sql
if (total > 1000):
    print 'big month'
else if (total > 500):
    print 'decent month'
else:
    print 'rough month'

-- These are equivalent:
if (COUNT(*) > 0 FROM orders WHERE status = 'pending'):
if (SELECT COUNT(*) > 0 FROM orders WHERE status = 'pending'):
```

---

### While — `while`

Condition follows the same rules as `if`. Scalars can drive the loop:

```sql
val batch_size = 1000
val offset     = 0

while (offset < (SELECT COUNT(*) FROM events)):
    SELECT * FROM events LIMIT batch_size OFFSET offset > batch_{{offset}}.csv
    val offset = offset + batch_size
```

---

### Expect — `expect`

Data quality assertions. `fail` exits immediately with a red error. `warn` prints a yellow warning and continues.

```sql
expect (COUNT(*) > 0 FROM paid)              else fail 'no paid orders found'
expect (COUNT(*) = 0 FROM dupes)             else fail 'duplicate order IDs detected'
expect (SUM(amount) > 0 FROM ledger)         else warn 'ledger total is zero'
expect (MAX(created_at) > CURRENT_DATE - INTERVAL 1 DAY FROM events)
    else warn 'no events in the last 24 hours'
```

---

### Statement termination

Multi-line SQL statements at the top level need a semicolon to signal the end. Inside `let`, `val`, or `fn` bodies, indentation handles this automatically — no semicolons needed.

```sql
-- Top level: semicolon required when followed by another raw statement
INSERT INTO log VALUES (now(), 'ran');
SELECT * FROM log;

-- Single statement followed by a Dabble keyword: semicolon optional
SELECT * FROM summary

let next = SELECT 1   -- Dabble keyword terminates the SELECT above

-- let/fn bodies: indentation terminates, no semicolons needed
let report =
    SELECT o.*, p.name
    FROM orders o
    JOIN products p ON p.id = o.product_id
    WHERE o.status = 'paid'
```

---

### Bare table name

A plain identifier on its own line prints the full table — shorthand for `SELECT * FROM name`:

```sql
let result = orders_by_status('paid', 3)
result       -- prints SELECT * FROM result
```

---

### Print — `print`

Prints a string, evaluates an expression, or renders a full result set. Multi-line works with indentation.

```sql
print 'hello world'
print total                              -- scalar
print 'revenue: $' || total             -- expression with scalar
print SELECT * FROM summary             -- full result set
print SELECT
    'orders: '  || COUNT(*)  ||
    ' revenue: $' || SUM(amount)
FROM paid
```

---

### Export — `>` and `>>`

Redirect any query to a CSV file.

```sql
SELECT * FROM summary ORDER BY revenue DESC > report.csv
SELECT * FROM errors >> error_log.csv       -- append
```

---

### Import — `import`

Runs another `.sql` file in the current context. Paths resolve relative to the importing file.

```sql
import "lib/helpers.sql"
import "setup.sql"
```

---

### Persistent Data

Just add attach. Dabble support what Duckdb spports.

```sql
ATTACH 'test.duckdb' as my_db;
USE my_db;
```

---

## How it works

Dabble compiles your script to an AST, then walks it. Each statement is either:

- **Handed directly to DuckDB** — raw SQL, `let`, `val`, redirects
- **Used to drive iteration** — `for` fetches rows and loops, `while` re-evaluates a condition each pass
- **Built into a CTE chain** — `let` and `val` inside `fn` bodies are lazy, never hitting the database until the caller materialises them

There is no expression evaluator, no type system, no query planner. DuckDB handles all of that. Dabble's entire job is sequencing.

The result: cold start overhead is minimal — almost everything becomes a single DuckDB query.

---

## What Dabble actually sends to DuckDB

Dabble is transparent by design. Here is a realistic script and the exact SQL it generates.

**Dabble script:**

```sql
val cutoff  = CURRENT_DATE - INTERVAL 30 DAYS
val avg     = SELECT AVG(amount) FROM orders WHERE status = 'paid'

fn recent_summary():
    let paid    = SELECT * FROM orders WHERE status = 'paid' AND created_at > cutoff
    let ranked  = SELECT customer_id, SUM(amount) AS total FROM paid GROUP BY customer_id
    SELECT * FROM ranked WHERE total > avg ORDER BY total DESC

let summary = recent_summary()
expect (COUNT(*) > 0 FROM summary) else fail 'no customers above average'

for row in summary:
    print row.customer_id || ' — $' || ROUND(row.total, 2)
```

**What DuckDB actually receives, statement by statement:**

```sql
-- val cutoff
SET VARIABLE __val_0_cutoff = (SELECT (CURRENT_DATE - INTERVAL 30 DAYS));

-- val avg
SET VARIABLE __val_0_avg = (SELECT AVG(amount) FROM orders WHERE status = 'paid');

-- let summary = recent_summary()
-- The function never ran a query. Its lets became CTEs.
-- Scalars inject as getvariable() — a plain scalar function call.
-- The whole function compiles down to a single WITH chain:
CREATE OR REPLACE TEMP TABLE summary AS (
    WITH paid AS (
        SELECT * FROM orders
        WHERE status = 'paid'
        AND created_at > getvariable('__val_0_cutoff')
    ),
    ranked AS (
        SELECT customer_id, SUM(amount) AS total
        FROM paid
        GROUP BY customer_id
    )
    SELECT * FROM ranked
    WHERE total > getvariable('__val_0_avg')
    ORDER BY total DESC
);

-- expect
SELECT 1 FROM (
    SELECT (COUNT(*)) AS _cond FROM summary
) WHERE _cond IS TRUE LIMIT 1;

-- for row in summary: print row.customer_id || ' — $' || ROUND(row.total, 2)
-- (one query per row, column values substituted as literals)
SELECT ('alice' || ' — $' || ROUND(312.50, 2));
SELECT ('bob'   || ' — $' || ROUND(274.00, 2));
-- ...
```

A few things worth noticing:

- The function body produced **zero queries during definition**. It only ran when `let summary = recent_summary()` was reached, and even then as a single `WITH` statement.
- Scalars inject as `getvariable('__val_0_cutoff')` — a plain scalar function call, not a subquery. This means scalars work anywhere in DuckDB: table macro parameters, extension function arguments, `COPY` options, anywhere a subquery would be rejected.
- The `expect` condition `COUNT(*) > 0 FROM summary` was normalised to a full `SELECT` and wrapped in a subquery. DuckDB evaluates it; Dabble just checks whether any row came back.
- Dabble itself never evaluates a single expression. Every number, date, string, and comparison is computed by DuckDB.


## What Dabble intentionally is not

- **Not a general scripting language.** No file I/O beyond CSV, no HTTP, no string manipulation outside SQL.
- **Not a Python replacement.** If you need pandas, numpy, or ML — use Python. Dabble is for the part of your pipeline that is already pure SQL logic.
- **Not production-ready.** Error messages are improving. The language will change. Things will break.
- **Not an ORM.** Dabble doesn't know what a model is.

---

## Roadmap / known gaps

- [ ] Better indentation handling (currently hardcoded 4 spaces)
- [ ] `return` keyword for early exit from functions (return is currently last select statement)
- [ ] Package/module system beyond `import`

---

## License

MIT. Do whatever you want, just don't blame the duck.