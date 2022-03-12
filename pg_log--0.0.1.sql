--
-- pg_log--0.0.1.sql
--
DROP FUNCTION IF EXISTS pg_log();
--
CREATE FUNCTION pg_log() RETURNS setof record
 AS 'pg_log.so', 'pg_log'
 LANGUAGE C STRICT;
--
