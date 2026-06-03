--
-- pg_log--0.0.1.sql
--
-- script must be run in database name corresponding to pg_log.datname 
--
DROP TABLE IF EXISTS log;
DROP VIEW IF EXISTS log;
DROP FUNCTION IF EXISTS pg_log();
DROP FUNCTION IF EXISTS pg_read();
DROP FUNCTION IF EXISTS pg_get_logname();
DROP FUNCTION IF EXISTS pg_log_refresh();
--
--
CREATE TABLE pglog(id numeric, message text);
--
CREATE VIEW log AS SELECT * FROM pglog;
---
CREATE FUNCTION pg_get_logname() RETURNS cstring 
 AS 'pg_log.so', 'pg_get_logname'
 LANGUAGE C STRICT;
---
CREATE FUNCTION pg_read(cstring) RETURNS void 
 AS 'pg_log.so', 'pg_read'
 LANGUAGE C STRICT;
--
CREATE FUNCTION pg_log(OUT line integer, OUT message text) RETURNS SETOF record 
 AS 'pg_log.so', 'pg_log'
 LANGUAGE C STRICT;
--
CREATE FUNCTION pg_log_refresh() RETURNS void 
 AS 'pg_log.so', 'pg_log_refresh'
 LANGUAGE C STRICT;
--
-- function taillog can be used independently to watch log with psql \watch
--
CREATE OR REPLACE FUNCTION taillog(
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

