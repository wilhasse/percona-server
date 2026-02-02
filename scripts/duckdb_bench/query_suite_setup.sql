-- Expects: database selected, and @customers/@orders/@lineitems set.

DROP TABLE IF EXISTS bench_lineitems;
DROP TABLE IF EXISTS bench_orders;
DROP TABLE IF EXISTS bench_customers;

CREATE TABLE bench_customers (
  id INT PRIMARY KEY,
  region VARCHAR(16) NOT NULL,
  segment VARCHAR(16) NOT NULL
);

CREATE TABLE bench_orders (
  id INT PRIMARY KEY,
  customer_id INT NOT NULL,
  order_date DATE NOT NULL,
  amount DECIMAL(10,2) NOT NULL,
  status VARCHAR(8) NOT NULL,
  KEY customer_id_idx (customer_id)
);

CREATE TABLE bench_lineitems (
  id INT PRIMARY KEY,
  order_id INT NOT NULL,
  product_id INT NOT NULL,
  quantity INT NOT NULL,
  price DECIMAL(10,2) NOT NULL,
  KEY order_id_idx (order_id)
);

SET cte_max_recursion_depth = 1000000;

WITH RECURSIVE seq AS (
  SELECT 1 AS n
  UNION ALL
  SELECT n + 1 FROM seq WHERE n < @customers
)
INSERT INTO bench_customers (id, region, segment)
SELECT n,
       CASE (n % 4)
         WHEN 0 THEN 'APAC'
         WHEN 1 THEN 'EMEA'
         WHEN 2 THEN 'AMER'
         ELSE 'LATAM'
       END,
       CASE (n % 3)
         WHEN 0 THEN 'SMB'
         WHEN 1 THEN 'MID'
         ELSE 'ENT'
       END
FROM seq;

WITH RECURSIVE seq AS (
  SELECT 1 AS n
  UNION ALL
  SELECT n + 1 FROM seq WHERE n < @orders
)
INSERT INTO bench_orders (id, customer_id, order_date, amount, status)
SELECT n,
       (n % @customers) + 1,
       DATE_ADD('2024-01-01', INTERVAL (n % 365) DAY),
       CAST((n % 10000) / 100.0 AS DECIMAL(10,2)),
       CASE (n % 5)
         WHEN 0 THEN 'HOLD'
         WHEN 1 THEN 'NEW'
         WHEN 2 THEN 'PAID'
         WHEN 3 THEN 'SHIP'
         ELSE 'CLOSE'
       END
FROM seq;

WITH RECURSIVE seq AS (
  SELECT 1 AS n
  UNION ALL
  SELECT n + 1 FROM seq WHERE n < @lineitems
)
INSERT INTO bench_lineitems (id, order_id, product_id, quantity, price)
SELECT n,
       (n % @orders) + 1,
       (n % 1000) + 1,
       (n % 5) + 1,
       CAST((n % 10000) / 100.0 AS DECIMAL(10,2))
FROM seq;
