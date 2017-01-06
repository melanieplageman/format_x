CREATE EXTENSION IF NOT EXISTS format_x;
CREATE EXTENSION IF NOT EXISTS hstore;

-- Hstore --

SELECT format_x('Hello %(name)s', hstore(ARRAY[
  'name', 'United States', 'code', 'US', 'population', '1000'
]));
SELECT format_x('Hello %(name)s <%(code)s>', hstore(ARRAY[
  'name', 'United States', 'code', 'US', 'population', '1000'
]));
SELECT format_x('%(name)s: %(population)s', hstore(ARRAY[
  'name', 'United States', 'code', 'US', 'population', '1000'
]));
SELECT format_x('%2(name)s %1(name)s', hstore(ARRAY[
  'name', 'United States', 'code', 'US', 'population', '1000'
]), hstore(ARRAY[
  'name', 'Canada', 'code', 'CA', 'population', '30'
]));
SELECT format_x('%2(name)s %1(name)s', VARIADIC ARRAY[
  hstore(ARRAY[
    'name', 'United States', 'code', 'US', 'population', '1000'
  ]),
  hstore(ARRAY[
    'name', 'Canada', 'code', 'CA', 'population', '30'
  ])
]);
SELECT format_x('SELECT * FROM nation WHERE code = %(code)I', hstore(ARRAY[
  'name', 'United States', 'code', 'US', 'population', '1000'
]));
SELECT format_x('SELECT * FROM nation WHERE code = %(code)L', hstore(ARRAY[
  'name', 'United States', 'code', 'US', 'population', '1000'
]));

-- Hstore with missing key --

SELECT format_x('%(size)s', hstore(ARRAY['name', 'United Kingdom']));
