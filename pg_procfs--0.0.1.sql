--
-- pg_procfs--0.0.1.sql
--
DROP FUNCTION IF EXISTS pg_read();
DROP FUNCTION IF EXISTS pg_procfs();
---
CREATE FUNCTION pg_read(cstring) RETURNS void 
 AS 'pg_procfs.so', 'pg_read'
 LANGUAGE C STRICT;
---
CREATE FUNCTION pg_procfs(IN filename cstring, OUT line integer, OUT message text) RETURNS SETOF record 
 AS 'pg_procfs.so', 'pg_procfs'
 LANGUAGE C STRICT;
--
