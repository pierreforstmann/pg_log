# pg_log
PostgreSQL extension to display instance log from SQL


# Installation
## Compiling

This module can be built using the standard PGXS infrastructure. For this to work, the `pg_config` program must be available in your $PATH:
  
`git clone https://github.com/pierreforstmann/pg_log.git` <br>
`cd pg_log` <br>
`make` <br>
`make install` <br>

This extension has been validated with PostgreSQL 14, 15, 16, 17 and 18.

## PostgreSQL setup

You must first set `logging_collector` to `on` and valid value for `log_directory` and `log_filename`.
Note that `log_filename` must be set to constant name : %-escape parameters are not supported.

There is no GUC setting and there is no need to modify `shared_preload_libraries` parameter.

Related PL/pgSQL function code reads log file contents for any `log_line_prefix` configuration without parsing log line (`file_fdw` foreign data wrapper is not used).

Following SQL statement must be run in each database by superuser:

`create extension pg_log;`

For security reason, only supersuer can execute `flog()` 'nad `tlog()` function: https://commitfest.postgresql.org/patch/5597/.

# Usage

## Example

Run: <br>
`create extension pg_log`;

To display last 5000 bytes of log contents:<br>
`select * from tlog();`<br>

To display last 1000 bytes of log contents:<br>
`select * from tlog(1000);`<br>

To display full log file contents:<br>
`select * from flog();`<br>

To use psql to monitor log file in "real-time":<br>
`select * from tlog();`<br>
`\watch`<br>
