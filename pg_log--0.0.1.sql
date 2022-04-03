--
-- pg_log--0.0.1.sql
--
-- script must be run in database named pg_log
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
