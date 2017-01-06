-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION format_x" to load this file. \quit

CREATE OR REPLACE FUNCTION format_x(string TEXT)
  RETURNS TEXT AS
'format_x', 'format_x'
LANGUAGE C IMMUTABLE;

CREATE OR REPLACE FUNCTION format_x(string TEXT, VARIADIC "any")
  RETURNS TEXT AS
'format_x', 'format_x'
LANGUAGE C IMMUTABLE;
