# contrib/pg_track_optimizer/Makefile

MODULE_big = pg_track_optimizer
OBJS = \
	$(WIN32RES) \
	pg_track_optimizer.o
PGFILEDESC = "pg_track_optimizer - track planning decisions"

EXTENSION = pg_track_optimizer
EXTVERSION = 0.1

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
