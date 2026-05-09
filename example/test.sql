-- ================================================================
-- test.dl — full feature test for datalang
-- ================================================================
-- Simulates a small sales pipeline:
--   raw orders → cleaned → enriched → reported
-- Covers: let, for, if/else, while, fn, expect, print, redirect
-- ================================================================


-- ----------------------------------------------------------------
-- 1. SEED DATA
-- ----------------------------------------------------------------

CREATE TABLE products (
    id      INTEGER,
    name    VARCHAR,
    category VARCHAR,
    price   DECIMAL(10,2)
);

INSERT INTO products VALUES
    (1,  'Keyboard',     'electronics', 79.99),
    (2,  'Mouse',        'electronics', 29.99),
    (3,  'Coffee',       'food',         9.99),
    (4,  'Notebook',     'stationery',   4.99),
    (5,  'Headphones',   'electronics', 149.99),
    (6,  'Tea',          'food',         7.99),
    (7,  'Pen',          'stationery',   1.99),
    (8,  'Monitor',      'electronics', 399.99);

CREATE TABLE orders (
    id          INTEGER,
    product_id  INTEGER,
    customer    VARCHAR,
    qty         INTEGER,
    status      VARCHAR   -- 'paid', 'refunded', 'pending'
);

INSERT INTO orders VALUES
    (1,  1, 'alice',   2, 'paid'),
    (2,  3, 'bob',     5, 'paid'),
    (3,  5, 'alice',   1, 'refunded'),
    (4,  2, 'carol',   3, 'paid'),
    (5,  8, 'bob',     1, 'pending'),
    (6,  4, 'carol',   10,'paid'),
    (7,  6, 'alice',   4, 'paid'),
    (8,  7, 'dave',    20,'paid'),
    (9,  1, 'dave',    1, 'paid'),
    (10, 3, 'carol',   2, 'refunded');

print '--- 1. seed data loaded ---'


-- ----------------------------------------------------------------
-- 2. BASIC LET + PRINT
-- ----------------------------------------------------------------

let paid_orders = 
    SELECT o.*, p.name AS product_name, p.category, p.price
    FROM orders o
    JOIN products p ON p.id = o.product_id
    WHERE o.status = 'paid'

print SELECT 'paid orders: ' || COUNT(*) FROM paid_orders
print '--- 2. basic let ok ---'


-- ----------------------------------------------------------------
-- 3. FUNCTIONS — CTE model
-- ----------------------------------------------------------------

-- Each function builds a self-contained WITH chain.
-- Calling fn inside another fn merges their CTEs.

fn electronics_revenue():
    let relevant = SELECT * FROM paid_orders WHERE category = 'electronics'
    SELECT
        product_name,
        SUM(qty * price) AS revenue
    FROM relevant
    GROUP BY product_name
    ORDER BY revenue DESC

fn food_revenue():
    let relevant = SELECT * FROM paid_orders WHERE category = 'food'
    SELECT
        product_name,
        SUM(qty * price) AS revenue
    FROM relevant
    GROUP BY product_name
    ORDER BY revenue DESC

fn top_customers():
    let with_value = SELECT customer, qty * price AS order_value FROM paid_orders
    SELECT
        customer,
        COUNT(*)            AS order_count,
        SUM(order_value)    AS total_spent
    FROM with_value
    GROUP BY customer
    ORDER BY total_spent DESC

-- Materialize into temp tables
let elec   = electronics_revenue()
let food   = food_revenue()
let customers = top_customers()

print '--- 3. functions ok ---'
print SELECT 'electronics rows: ' || COUNT(*) FROM elec
print SELECT 'food rows: ' || COUNT(*) FROM food


-- ----------------------------------------------------------------
-- 4. NESTED FUNCTIONS (CTE merging)
-- ----------------------------------------------------------------

fn category_summary():
    let with_value  = SELECT category, qty * price AS line_total FROM paid_orders
    SELECT
        category,
        COUNT(*)           AS lines,
        SUM(line_total)    AS revenue,
        AVG(line_total)    AS avg_line
    FROM with_value
    GROUP BY category
    ORDER BY revenue DESC

let summary = category_summary()

print '--- 4. category summary ---'
SELECT * FROM summary


-- ----------------------------------------------------------------
-- 5. FOR LOOP
-- ----------------------------------------------------------------

print '--- 5. per-customer breakdown ---'

for c in customers:
    print SELECT '  ' || '{{c.customer}}' || ' spent $' || ROUND({{c.total_spent}}, 2)


-- ----------------------------------------------------------------
-- 6. IF / ELSE
-- ----------------------------------------------------------------

print '--- 6. business health check ---'

let total_revenue = SELECT SUM(qty * price) AS rev FROM paid_orders
let pending_count = SELECT COUNT(*) AS cnt FROM orders WHERE status = 'pending'

if (rev > 500 FROM total_revenue):
    print 'Revenue target hit'
else:
    print 'Revenue below target'

if (cnt > 0 FROM pending_count):
    print SELECT 'WARNING: ' || cnt || ' pending orders need attention' FROM pending_count
else:
    print 'No pending orders'


-- ----------------------------------------------------------------
-- 7. WHILE LOOP — compute a running rank manually
-- ----------------------------------------------------------------

print '--- 7. while loop + val/scalar ---'

-- val: literal scalar
val greeting   = 'hello'
scalar version = 42

print greeting || ' from datalang v' || version

-- val: scalar from SELECT (first cell)
val order_count = COUNT(*) FROM orders
val max_spend   = MAX(total_spent) FROM customers

print 'total orders : ' || order_count
print 'top customer : $' || max_spend

-- while: count down using a single-row table + UPDATE
CREATE TEMP TABLE tick (n INTEGER);
INSERT INTO tick VALUES (3);

while (SELECT n > 0 FROM tick):
    print SELECT '  tick: ' || n FROM tick
    UPDATE tick SET n = n - 1;

print 'done counting'

-- val inside a for loop (scoped to each iteration)
for c in customers:
    val label = '{{c.customer}}' || ' (' || UPPER('{{c.customer}}') || ')'
    print '  ' || label


-- ----------------------------------------------------------------
-- 8. EXPECT
-- ----------------------------------------------------------------

print '--- 8. data quality checks ---'

-- These should pass
expect (SELECT COUNT(*) > 0 FROM paid_orders)           else fail 'No paid orders found'
expect (SELECT COUNT(*) = 4 FROM customers)              else fail 'Expected 4 customers'
expect (SELECT rev > 0 FROM total_revenue)               else fail 'Revenue must be positive'

-- Warn but continue if any product has no orders
let orphan_products = SELECT COUNT(*) AS cnt FROM products p
    WHERE NOT EXISTS (SELECT 1 FROM orders o WHERE o.product_id = p.id)

expect (SELECT cnt = 0 FROM orphan_products) else warn 'Some products have no orders'

print 'all checks passed'


-- ----------------------------------------------------------------
-- 9. REDIRECT TO CSV
-- ----------------------------------------------------------------

print '--- 9. export ---'

SELECT
    c.customer,
    c.order_count,
    ROUND(c.total_spent, 2) AS total_spent
FROM customers c
ORDER BY total_spent DESC > /tmp/customer_report.csv

SELECT * FROM summary > /tmp/category_summary.csv

print 'exported customer_report.csv and category_summary.csv to /tmp'


-- ----------------------------------------------------------------
-- 10. FINAL SUMMARY PRINT
-- ----------------------------------------------------------------

print '--- done ---'
print SELECT
    'orders: '     || (SELECT COUNT(*) FROM orders)      ||
    '  paid: '     || (SELECT COUNT(*) FROM paid_orders)  ||
    '  revenue: $' || ROUND((SELECT rev FROM total_revenue), 2)


-- ----------------------------------------------------------------
-- 10. FUNCTION PARAMETERS
-- ----------------------------------------------------------------

print '--- 10. function parameters ---'

fn orders_by_status(status, min_qty):
    let filtered = SELECT o.*, p.name AS product_name, p.price
        FROM orders o JOIN products p ON p.id = o.product_id
        WHERE o.status = status AND o.qty >= min_qty
    SELECT product_name, qty, qty * price AS total
    FROM filtered
    ORDER BY total DESC

-- Materialise with args
let big_paid = orders_by_status('paid', 3)
print SELECT 'big paid orders: ' || COUNT(*) FROM big_paid
big_paid;

-- Call with different args, print directly
orders_by_status('refunded', 1)

-- Args can be expressions or outer-scope scalars
val min_qty = 2
let medium_paid = orders_by_status('paid', min_qty)
print SELECT 'medium paid orders: ' || COUNT(*) FROM medium_paid

-- Arity error
-- orders_by_status('paid')


-- ----------------------------------------------------------------
-- 11. COLOR SMOKE TEST
-- This section deliberately fires a warn and then a fail.
-- Remove once you have verified colors look right.
-- ----------------------------------------------------------------

expect (1 = 2) else warn 'this is what a warning looks like'
expect (1 = 2) else fail 'this is what a failure looks like'