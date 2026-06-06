--
-- pg_log--1.0.0.sql
--


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

    -- Return last bytes
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

