format_x
========

Synopsis
--------

The function `format_x` produces output formatted according to a format string, in a style similar to the C function `sprintf`. It is an extension of the builtin function `format` as it allows the use of lookups, e.g.:

```sql
SELECT format_x('Hello %(location)s!', '{"location": "World"}'::JSONB);
   format_x
--------------
 Hello World!
(1 row)
```

It is (TODO almost) backward compatible with the builtin `format`. Lookups are supported against composite types, `JSONB`, and `HSTORE`.

Description
-----------

The `format_x` function behaves like `format` but allows format specifiers to include `keys` that perform a lookup against one or more arguments.

```sql
CREATE TABLE nation(name TEXT, code CHAR(2), population INT);
INSERT INTO nation VALUES
  ('United States', 'US', 1000),
  ('Canada', 'CA', 30),
  ('Mexico', 'MX', 40);

SELECT format_x('Hello %(name)s <%(code)s>', nation) FROM nation;
         format_x         
--------------------------
 Hello United States <US>
 Hello Canada <CA>
 Hello Mexico <MX>
(3 rows)
```

Lookups are supported against multiple arguments. Each argument may be specified by position (just as with `format`):

```sql
SELECT format_x('%2(name)s %1(name)s',
  (SELECT nation FROM nation WHERE code = 'US'),
  (SELECT nation FROM nation WHERE code = 'CA')
);
       format_x       
----------------------
 Canada United States
(1 row)
```

`HSTORE` is supported as well if the `hstore` extension is available:

```sql
SELECT format_x('Hello %(name)s', hstore(ARRAY[
  'name', 'United States', 'code', 'US', 'population', '1000'
]));
      format_x       
---------------------
 Hello United States
(1 row)
```

Lookups can be chained by specifying multiple keys. Each lookup is performed against the result of the previous lookup and types can be mixed.

```sql
CREATE TABLE description(name TEXT, data JSONB);
INSERT INTO description VALUES
  ('United States', '{"code": "US", "population": 1000}'::JSONB);

SELECT format_x('%(name)I: %(data.population)s', description)
  FROM description;
       format_x        
-----------------------
 "United States": 1000
(1 row)
```

Usage
-----

The function `format_x` produces output formatted according to a format string, in a style similar to the C function `sprintf`.

```
format_x(formatstr text [, formatarg "any" [, ...] ])
```

`formatstr` is a format string that specifies how the result should be formatted. Text in the format string is copied directly to the result, except where *format specifiers* are used. Format specifiers act as placeholders in the string, defining how subsequent function arguments should be formatted and inserted into the result. Each `formatarg` argument is converted to text according to the usual output rules for its data type, and then formatted and inserted into the result string according to the format specifier(s).

Format specifiers are introduced by a `%` character and have the form

```
%[position][(keys)][flags][width][.precision]type
```

where the component fields are:

#### `position` (optional)

A string of the form `n$` where `n` is the index of the argument to print. Index 1 means the first argument after `formatstr`. If the `position` is omitted, the default is to use the next argument in sequence.

#### `keys` (optional)

A string of one or more keys separated by `.`. Each key is looked up in sequence against the respective argument. If a key is present the argument must be of a dictionary-like type as follows:

  * If the argument is a composite type the lookup is performed with the same effect as the `.` operator.

  * If the argument type is `JSONB` or `HSTORE` the lookup is performed to the same effect as the `->` operator (unless the key does not exist in which case an error is generated).

  * Any other argument type results in an error.

If multiple keys are given then each key after the first is looked up against the result of the previous lookup. At any point if a lookup is requested against a `NULL` value an error is produced.

#### `flags` (optional)

Additional options controlling how the format specifier's output is formatted. Currently the only supported flag is a minus sign (`-`) which will cause the format specifier's output to be left-justified. This has no effect unless the `width` field is also specified.

#### `width` (optional)

Specifies the **minimum** number of characters to use to display the format specifier's output. The output is padded on the left or right (depending on the `-` flag) with spaces as needed to fill the width. A too-small width does not cause truncation of the output, but is simply ignored. The width is specified using a positive integer.

If the width argument is negative, the result is left aligned (as if the `-` flag had been specified) within a field of length `abs(width)`.

Using an asterisk `*` to indirectly specify the width is not supported at this time.

#### `precision` (optional)

If the argument type is a numeric type supporting a fractional component (`DECIMAL`, `NUMERIC`, `REAL`, or `DOUBLE PRECISION`) then this specifies the number of digits to appear after the radix character. Otherwise this specifies the maximum number of characters that should be output. The precision is specified using a positive integer.

Using an asterisk `*` to indirectly specify the precision is not supported at this time.

#### `type` (required)

The type of format conversion to use to produce the format specifier's output. The following types are supported:
  * `s` formats the argument value as a simple string. A null value is treated as an empty string.
  * `I` treats the argument value as an SQL identifier, double-quoting it if necessary. It is an error for the value to be null (equivalent to `quote_ident`).
  * `L` quotes the argument value as an SQL literal. A null value is displayed as the string NULL, without quotes (equivalent to `quote_nullable`).

In addition to the format specifiers described above, the special sequence `%%` may be used to output a literal `%` character.

Here are some examples of the basic format conversions:

```sql
SELECT format_x('Hello %s', 'World');
Result: Hello World

SELECT format_x('Testing %s, %s, %s, %%', 'one', 'two', 'three');
Result: Testing one, two, three, %

SELECT format_x('INSERT INTO %I VALUES(%L)', 'Foo bar', E'O\'Reilly');
Result: INSERT INTO "Foo bar" VALUES('O''Reilly')

SELECT format_x('INSERT INTO %I VALUES(%L)', 'locations', E'C:\\Program Files');
Result: INSERT INTO locations VALUES(E'C:\\Program Files')
```

Here are examples using `width` fields and the `-` flag:

```sql
SELECT format_x('|%10s|', 'foo');
Result: |       foo|

SELECT format_x('|%-10s|', 'foo');
Result: |foo       |

SELECT format_x('|%*s|', 10, 'foo');
Result: |       foo|

SELECT format_x('|%*s|', -10, 'foo');
Result: |foo       |

SELECT format_x('|%-*s|', 10, 'foo');
Result: |foo       |

SELECT format_x('|%-*s|', -10, 'foo');
Result: |foo       |
```

These examples show use of `position` fields:

```sql
SELECT format_x('Testing %3$s, %2$s, %1$s', 'one', 'two', 'three');
Result: Testing three, two, one

SELECT format_x('|%*2$s|', 'foo', 10, 'bar');
Result: |       bar|

SELECT format_x('|%1$*2$s|', 'foo', 10, 'bar');
Result: |       foo|
```

Unlike the standard C function `sprintf`, the `format_x` function allows format specifiers with and without `position` fields to be mixed in the same format string.

  * A format specifier with a `position` field always uses the argument in that position.
  * A format specifier without a `position` field and without any `keys` uses the next argument after the last argument consumed.
  * A format specifier without a `position` field and with `keys` uses the same argument as the last argument consumed. If no last argument is available it uses the first argument.

In addition, the `format_x` function does not require all function arguments to be used in the format string. For example:

```sql
SELECT format_x('Testing %3$s, %2$s, %s', 'one', 'two', 'three');
Result: Testing three, two, three
```

The `%I` and `%L` format specifiers are particularly useful for safely constructing dynamic SQL statements.

Support
-------

http://github.com/melanieplageman/format_x/issues

Author
------

Melanie Plageman <melanieplageman@gmail.com>

Copyright and License
---------------------

Copyright (c) 2016-2017 Melanie Plageman

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
