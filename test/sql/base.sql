CREATE EXTENSION IF NOT EXISTS format_x;

-- Non-dictionary types should not support lookups --

SELECT format_x('Hello %(name)s', 27);
