CREATE EXTENSION IF NOT EXISTS format_x;

select format_x(NULL);
select format_x('Hello');
select format_x('Hello %s', 'World');
select format_x('Hello %%');
select format_x('Hello %%%%');
-- should fail
select format_x('Hello %s %s', 'World');
select format_x('Hello %s');
select format_x('Hello %x', 20);
-- check literal and sql identifiers
select format_x('INSERT INTO %I VALUES(%L,%L)', 'mytab', 10, 'Hello');
select format_x('%s%s%s','Hello', NULL,'World');
select format_x('INSERT INTO %I VALUES(%L,%L)', 'mytab', 10, NULL);
select format_x('INSERT INTO %I VALUES(%L,%L)', 'mytab', NULL, 'Hello');
-- should fail, sql identifier cannot be NULL
select format_x('INSERT INTO %I VALUES(%L,%L)', NULL, 10, 'Hello');
-- check positional placeholders
select format_x('%1$s %3$s', 1, 2, 3);
select format_x('%1$s %12$s', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
-- should fail
select format_x('%1$s %4$s', 1, 2, 3);
select format_x('%1$s %13$s', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
select format_x('%0$s', 'Hello');
/* select format_x('%*0$s', 'Hello'); */
select format_x('%1$', 1);
select format_x('%1$1', 1);
-- check mix of positional and ordered placeholders
select format_x('Hello %s %1$s %s', 'World', 'Hello again');
select format_x('Hello %s %s, %2$s %2$s', 'World', 'Hello again');
-- check variadic labeled arguments
select format_x('%s, %s', variadic array['Hello','World']);
select format_x('%s, %s', variadic array[1, 2]);
select format_x('%s, %s', variadic array[true, false]);
select format_x('%s, %s', variadic array[true, false]::text[]);
-- check variadic with positional placeholders
select format_x('%2$s, %1$s', variadic array['first', 'second']);
select format_x('%2$s, %1$s', variadic array[1, 2]);
-- variadic argument can be array type NULL, but should not be referenced
select format_x('Hello', variadic NULL::int[]);
-- variadic argument allows simulating more than FUNC_MAX_ARGS parameters
select format_x(string_agg('%s',','), variadic array_agg(i))
from generate_series(1,200) g(i);
-- check field widths and left, right alignment
select format_x('>>%10s<<', 'Hello');
select format_x('>>%10s<<', NULL);
select format_x('>>%10s<<', '');
select format_x('>>%-10s<<', '');
select format_x('>>%-10s<<', 'Hello');
select format_x('>>%-10s<<', NULL);
select format_x('>>%1$10s<<', 'Hello');
select format_x('>>%1$-10I<<', 'Hello');
/* select format_x('>>%2$*1$L<<', 10, 'Hello'); */
/* select format_x('>>%2$*1$L<<', 10, NULL); */
/* select format_x('>>%2$*1$L<<', -10, NULL); */
/* select format_x('>>%*s<<', 10, 'Hello'); */
/* select format_x('>>%*1$s<<', 10, 'Hello'); */
select format_x('>>%-s<<', 'Hello');
select format_x('>>%10L<<', NULL);
/* select format_x('>>%2$*1$L<<', NULL, 'Hello'); */
/* select format_x('>>%2$*1$L<<', 0, 'Hello'); */
