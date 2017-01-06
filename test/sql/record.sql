CREATE EXTENSION IF NOT EXISTS format_x;

-- Composite type representing a table --

CREATE TABLE nation(name TEXT, code CHAR(2), population INT);
INSERT INTO nation VALUES
  ('United States', 'US', 1000),
  ('Canada', 'CA', 30),
  ('Mexico', 'MX', 40);
SELECT format_x('Hello %(name)s', nation) FROM nation;
SELECT format_x('Hello %(name)s <%(code)s>', nation) FROM nation;
SELECT format_x('%(name)s: %(population)s', nation) FROM nation;
SELECT format_x('%2(name)s %1(name)s',
  (SELECT nation FROM nation WHERE code = 'US'),
  (SELECT nation FROM nation WHERE code = 'CA')
);
SELECT format_x('%2(name)s %1(name)s', VARIADIC array_agg(nation)) FROM nation;
SELECT format_x('SELECT * FROM nation WHERE code = %(code)I',
  nation) FROM nation;
SELECT format_x('SELECT * FROM nation WHERE code = %(code)L',
  nation) FROM nation;

-- Composite type with missing attribute --

SELECT format_x('%(size)s', ROW('United Kingdom', 'UK', 200)::nation);

-- Composite type literal with NULL attribute --

SELECT format_x('%(name)s %(code)I %(population)L',
  ROW('United Kingdom', 'UK', 200)::nation);
SELECT format_x('%(name)s %(code)I %(population)L',
  ROW(NULL, 'UK', 200)::nation);
SELECT format_x('%(name)s %(code)I %(population)L',
  ROW('United Kingdom', NULL, 200)::nation);
SELECT format_x('%(name)s %(code)I %(population)L',
  ROW('United Kingdom', 'UK', NULL)::nation);

-- Composite type with composite type attribute --

CREATE TYPE pair AS (n1 nation, n2 nation);

SELECT format_x('%(n1.code)I, %(n2.name)s', ROW(
  (SELECT nation FROM nation WHERE code = 'US'),
  (SELECT nation FROM nation WHERE code = 'CA')
)::pair);

SELECT format_x('%(n1.code)I, %(n2.name)s', ROW(
  (SELECT nation FROM nation WHERE code = 'US'), NULL
)::pair);

SELECT format_x('%(n1.code)I, %(n2)s', ROW(
  (SELECT nation FROM nation WHERE code = 'US'),
  (SELECT nation FROM nation WHERE code = 'CA')
)::pair);

SELECT format_x('%(n1.code)I, %(n2)L', ROW(
  (SELECT nation FROM nation WHERE code = 'US'),
  (SELECT nation FROM nation WHERE code = 'CA')
)::pair);
