-- One query per line; terminate each query with a semicolon.
-- Use "-- name: <label>" to label a query in reports.

-- name: orders_count
SELECT COUNT(*) FROM bench_orders;

-- name: orders_by_status
SELECT status, COUNT(*) AS cnt FROM bench_orders GROUP BY status ORDER BY cnt DESC;

-- name: customer_totals
SELECT o.customer_id, SUM(o.amount) AS total FROM bench_orders o GROUP BY o.customer_id ORDER BY total DESC LIMIT 10;

-- name: region_spend
SELECT c.region, SUM(o.amount) AS total FROM bench_customers c JOIN bench_orders o ON c.id = o.customer_id GROUP BY c.region ORDER BY total DESC;

-- name: daily_revenue
SELECT o.order_date, SUM(li.quantity * li.price) AS revenue FROM bench_orders o JOIN bench_lineitems li ON o.id = li.order_id GROUP BY o.order_date ORDER BY revenue DESC LIMIT 10;

-- name: customer_left_join
SELECT c.id, COALESCE(SUM(o.amount), 0) AS total FROM bench_customers c LEFT JOIN bench_orders o ON c.id = o.customer_id GROUP BY c.id ORDER BY c.id LIMIT 20;

-- name: avg_price_by_status
SELECT o.status, AVG(li.price) AS avg_price FROM bench_orders o JOIN bench_lineitems li ON o.id = li.order_id GROUP BY o.status ORDER BY avg_price DESC;
