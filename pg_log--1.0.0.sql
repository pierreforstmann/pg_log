--
-- pg_log--1.0.0.sql
--
-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_log" to load this file. \quit

--
-- function flog displays full log file contents
--

CREATE OR REPLACE FUNCTION flog()
RETURNS text
LANGUAGE plpgsql
SET search_path = pg_temp
AS $$
DECLARE
    log_size bigint;
    start_pos bigint;
    log_directory text;
    log_filename text;
    pgdata text;
    full_log_filename text;
BEGIN

    -- Retrieve PG log file name
   SELECT setting INTO log_directory FROM pg_settings WHERE name = 'log_directory';
   IF NOT FOUND THEN
      RAISE EXCEPTION 'log_directory not found in pg_settings';
   END IF;
   SELECT setting INTO pgdata FROM pg_settings WHERE name = 'data_directory';
   IF NOT FOUND THEN
      RAISE EXCEPTION 'data_directory not found in pg_settings';
   END IF;
   SELECT setting INTO log_filename FROM pg_settings WHERE name = 'log_filename';
   IF NOT FOUND THEN
      RAISE EXCEPTION 'log_filename not found in pg_settings';
   END IF;
   full_log_filename := pgdata || '/' || log_directory || '/' || log_filename;

    -- Get current size
    SELECT size INTO log_size
    FROM pg_stat_file(full_log_filename);

    -- Return all bytes
    RETURN pg_read_file(full_log_filename, 0, log_size);
END;
$$;
--

--
-- function tlog displays log file last bytes 
-- can be used with psql \watch command to display log file tail every n seconds
--

CREATE OR REPLACE FUNCTION tlog(
    bytes integer DEFAULT 5000
)
RETURNS text
LANGUAGE plpgsql
SET search_path = pg_temp
AS $$
DECLARE
    log_size bigint;
    start_pos bigint;
    log_directory text;
    log_filename text;
    pgdata text;
    full_log_filename text;
BEGIN

    -- Retrieve PG log file name
   SELECT setting INTO log_directory FROM pg_settings WHERE name = 'log_directory';
   IF NOT FOUND THEN
      RAISE EXCEPTION 'log_directory not found in pg_settings';
   END IF;
   SELECT setting INTO pgdata FROM pg_settings WHERE name = 'data_directory';
   IF NOT FOUND THEN
      RAISE EXCEPTION 'data_directory not found in pg_settings';
   END IF;
   SELECT setting INTO log_filename FROM pg_settings WHERE name = 'log_filename';
   IF NOT FOUND THEN
      RAISE EXCEPTION 'log_filename not found in pg_settings';
   END IF;
   full_log_filename := pgdata || '/' || log_directory || '/' || log_filename;

    -- Get current size
    SELECT size INTO log_size
    FROM pg_stat_file(full_log_filename);

    -- Compute start position
    start_pos := GREATEST(log_size - bytes, 0);

    -- Return last bytes
    RETURN pg_read_file(full_log_filename, start_pos, bytes);
END;
$$;
--

-- default privileges on function is EXECUTE (not displayed by pg_proc.proacl)
--
-- https://www.postgresql.org/docs/18/ddl-priv.html says
--
-- PostgreSQL grants privileges on some types of objects to PUBLIC by default when the objects are created. 
-- No privileges are granted to PUBLIC by default on tables, table columns, sequences, foreign data wrappers, foreign servers, large objects, schemas, tablespaces, or configuration parameters. 
-- For other types of objects, the default privileges granted to PUBLIC are as follows: 
-- CONNECT and TEMPORARY (create temporary tables) privileges for databases; 
-- EXECUTE privilege for functions and procedures; 
-- and USAGE privilege for languages and data types (including domains).

-- revoke privileges on public because log may display password for example
--
REVOKE EXECUTE ON FUNCTION FLOG FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION TLOG FROM PUBLIC;
