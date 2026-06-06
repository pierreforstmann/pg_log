EXTENSION = pg_log  # the extension's name
DATA = pg_log--1.0.0.sql    # script file to install

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

#
pgxn:
	git archive --format zip  --output ../pgxn/pg_log/pg_log-1.0.0.zip main
