# pg_gaps: PGXS build
#
# Build against an installed PostgreSQL (17 or later):
#     make PG_CONFIG=/path/to/pg_config
#     make install PG_CONFIG=/path/to/pg_config
#     make installcheck PG_CONFIG=/path/to/pg_config
#
# PG_CONFIG defaults to whatever pg_config is on PATH.  The build also works
# out of tree (a VPATH build), which keeps objects and test output out of the
# source directory:
#     mkdir -p /path/to/build && cd /path/to/build
#     make -f /path/to/pg_gaps/Makefile VPATH=/path/to/pg_gaps

EXTENSION = pg_gaps

# The control file is the single source of the version.  Recursive (=)
# assignment on purpose: srcdir is only defined once PGXS is included.
EXTVERSION = $(shell sed -n "s/^default_version *= *'\(.*\)'/\1/p" $(srcdir)/$(EXTENSION).control)

MODULE_big = pg_gaps
OBJS = src/pg_gaps.o src/gaps_infer.o src/gaps_support.o src/engine/gx.o
DATA = $(EXTENSION)--$(EXTVERSION).sql
PGFILEDESC = "pg_gaps - probabilistic graphical models over incomplete relations"

PG_CPPFLAGS = -DPG_GAPS_VERSION=\"$(EXTVERSION)\" -I$(srcdir)/src

REGRESS = pg_gaps catalog infer learn em validate rows subset views

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# A VPATH build compiles src/*.c into src/*.o under the build directory,
# which make does not create on its own.
$(OBJS): | src-dir
.PHONY: src-dir
src-dir:
	@$(MKDIR_P) src/engine

# A non-ASCII byte in an install script makes CREATE EXTENSION fail on a
# database whose encoding is not UTF-8.  The C sources and the control file
# are held to the same rule so that nothing non-ASCII can leak into them.
.PHONY: check-ascii
check-ascii:
	@bad=$$(LC_ALL=C grep -l '[^[:print:][:space:]]' \
		$(srcdir)/$(EXTENSION).control $(srcdir)/$(EXTENSION)--*.sql \
		$(srcdir)/src/*.c $(srcdir)/src/*.h $(srcdir)/src/engine/*.[ch] \
		$(srcdir)/test/engine/*.[ch] 2>/dev/null); \
	if [ -n "$$bad" ]; then \
		echo "non-ASCII bytes in:" $$bad >&2; exit 1; \
	fi; \
	echo "control file, install scripts and C sources are ASCII"

# A source distribution in PGXN layout: pg_gaps-<version>.zip, every file
# under a pg_gaps-<version>/ prefix, from the committed tree (git archive),
# so it matches the tag and holds no build output.  .gitattributes keeps
# development files out of it.
DISTNAME = $(EXTENSION)-$(EXTVERSION)

.PHONY: dist
dist: check-meta
	git -C $(srcdir) archive --format=zip --prefix=$(DISTNAME)/ -o $(CURDIR)/$(DISTNAME).zip HEAD
	@echo "created $(DISTNAME).zip"

# META.json repeats the version for PGXN; it must match the control file,
# in both the distribution and the extension it provides.
.PHONY: check-meta
check-meta:
	@for v in $$(sed '/"meta-spec"/,/}/d' $(srcdir)/META.json | \
			sed -n 's/^ *"version": *"\([^"]*\)".*/\1/p'); do \
		if [ "$$v" != "$(EXTVERSION)" ]; then \
			echo "META.json says version $$v, $(EXTENSION).control says $(EXTVERSION)" >&2; \
			exit 1; \
		fi; \
	done; \
	grep -q '"file": *"$(EXTENSION)--$(EXTVERSION).sql"' $(srcdir)/META.json || { \
		echo "META.json does not provide $(EXTENSION)--$(EXTVERSION).sql" >&2; exit 1; }; \
	echo "META.json matches version $(EXTVERSION)"
