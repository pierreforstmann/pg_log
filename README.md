# pg_log
PostgreSQL extension to display log from SQL


# Installation
## Compiling

This module can be built using the standard PGXS infrastructure. For this to work, the `pg_config` program must be available in your $PATH:
  
`git clone https://github.com/pierreforstmann/pg_log.git` <br>
`cd pg_log` <br>
`make` <br>
`make install` <br>

This extension has been validated with PostgreSQL 10, 11, 12, 13, 14 and 15.

## PostgreSQL setup

Extension must loaded at server level with `shared_preload_libraries` parameter.

`shared_preload_libraries = 'pg_log'`

Following SQL statement should be run in the database that will store `pg_log` objects:

`create extension pg_log;`

# Usage
`pg_log` has 3 specific GUC settings:
1. `pg_log.fraction` which is the log fraction that is displayed between 0 and 1. To display 10% of log contents starting from the end, use `pg_log.fraction=0.1`. Default value is 0.01 (1%).
2. `pg_log.naptime` is the duration between each log refresh in the database. Default value is 30 seconds.
3. `pg_log.datname` is the database name where `pglog` table and `log` view are created. This database must be created before installing the extension. Default database name is `pg_log`.

## Example

Add in `postgresql.conf`:

`shared_preload_libraries = 'pg_log'` <br>

Create database `pg_log`:

`create database pg_log;`

Run in database `pg_log`: <br>
`create extension pg_log`;

To display 1% of log contents connect to database `pg_log` and query the `log` view:<br>
`\c pg_log` <br>
`select * from log;`<br>

