# contrib/pg_track_optimizer/Makefile

MODULE_big = pg_track_optimizer
OBJS = \
	$(WIN32RES) \
	pg_track_optimizer.o plan_error.o
PGFILEDESC = "pg_track_optimizer - track planning decisions"

EXTENSION = pg_track_optimizer
EXTVERSION = 0.1

DATA = pg_track_optimizer--0.1.sql

REGRESS = pg_track_optimizer interface
EXTRA_REGRESS_OPTS=--temp-config=$(top_srcdir)/$(subdir)/pg_track_optimizer.conf

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_track_optimizer
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
