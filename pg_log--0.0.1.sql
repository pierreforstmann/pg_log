--
-- pg_log--0.0.1.sql
--
DROP FUNCTION IF EXISTS pg_log();
DROP FUNCTION IF EXISTS pg_read();
---
CREATE FUNCTION pg_read(cstring) RETURNS void 
 AS 'pg_log.so', 'pg_read'
 LANGUAGE C STRICT;
--
CREATE FUNCTION pg_log() RETURNS setof record 
 AS 'pg_log.so', 'pg_log'
 LANGUAGE C STRICT;
--
