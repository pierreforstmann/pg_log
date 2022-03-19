# pg_log
PostgreSQL extension to display log 


# Installation
## Compiling

This module can be built using the standard PGXS infrastructure. For this to work, the `pg_config` program must be available in your $PATH:
  
`git clone https://github.com/pierreforstmann/pg_log.git` <br>
`cd pg_log` <br>
`make` <br>
`make install` <br>

This extension has been validated with PostgreSQL  14.

## PostgreSQL setup

Extension can be loaded:

1. in local session with `LOAD pg_log`; <br>
2. using `session_preload_libraries` parameter in a specific connection <br>
3. at server level with `shared_preload_libraries` parameter. <br> 

# Usage
`pg_log` has no specific GUC.

## Example

In postgresql.conf:

`shared_preload_libraries = 'pg_log'` <br>

To display full log contents, run:

` select pg_log();`

