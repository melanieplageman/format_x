CREATE EXTENSION IF NOT EXISTS format_x;

-- JSONB --

SELECT format_x('Hello %(name)s',
  '{"name": "United States", "code": "US", "population": 1000}'::JSONB);
SELECT format_x('Hello %(name)s <%(code)s>',
  '{"name": "United States", "code": "US", "population": 1000}'::JSONB);
SELECT format_x('%(name)s: %(population)s',
  '{"name": "United States", "code": "US", "population": 1000}'::JSONB);
SELECT format_x('%2(name)s %1(name)s',
  '{"name": "United States", "code": "US", "population": 1000}'::JSONB,
  '{"name": "Canada", "code": "CA", "population": 30}'::JSONB
);
SELECT format_x('%2(name)s %1(name)s', VARIADIC ARRAY[
  '{"name": "United States", "code": "US", "population": 1000}'::JSONB,
  '{"name": "Canada", "code": "CA", "population": 30}'::JSONB
]);
SELECT format_x('SELECT * FROM nation WHERE code = %(code)I',
  '{"name": "United States", "code": "US", "population": 1000}'::JSONB);
SELECT format_x('SELECT * FROM nation WHERE code = %(code)L',
  '{"name": "United States", "code": "US", "population": 1000}'::JSONB);

-- JSONB with missing key --

SELECT format_x('%(size)s', '{"name": "United Kingdom"}'::JSONB);

-- JSONB with null value --

SELECT format_x('%(name)s %(code)I %(population)L',
  '{"name": "United Kingdom", "code": "UK", "population": 200}'::JSONB);
SELECT format_x('%(name)s %(code)I %(population)L',
  '{"name": null, "code": "UK", "population": 200}'::JSONB);
SELECT format_x('%(name)s %(code)I %(population)L',
  '{"name": "United Kingdom", "code": null, "population": 200}'::JSONB);
SELECT format_x('%(name)s %(code)I %(population)L',
  '{"name": "United Kingdom", "code": "UK", "population": null}'::JSONB);

-- JSONB with nested object --

SELECT format_x('%(n1.code)I, %(n2.name)s', '{
  "n1": {"name": "United States", "code": "US", "population": 1000},
  "n2": {"name": "Canada", "code": "CA", "population": 30}
}'::JSONB);

SELECT format_x('%(n1.code)I, %(n2.name)s', '{
  "n1": {"name": "United States", "code": "US", "population": 1000},
  "n2": null
}'::JSONB);

SELECT format_x('%(n1.code)I, %(n2)s', '{
  "n1": {"name": "United States", "code": "US", "population": 1000},
  "n2": {"name": "Canada", "code": "CA", "population": 30}
}'::JSONB);

SELECT format_x('%(n1.code)I, %(n2)L', '{
  "n1": {"name": "United States", "code": "US", "population": 1000},
  "n2": {"name": "Canada", "code": "CA", "population": 30}
}'::JSONB);

-- Composite type with nested JSONB attribute --

CREATE TABLE description(name TEXT, data JSONB);
INSERT INTO description VALUES
  ('United States', '{"code": "US", "population": 1000}'::JSONB);
SELECT format_x('%(name)I: %(data.population)s', description)
  FROM description;
DROP TABLE description;
