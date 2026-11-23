#!/usr/bin/env bash
#
# dev/qualify-pgdg.sh: build pg_gaps against installed PostgreSQL majors and
# run its regression suite on a throwaway cluster for each, the way a
# distribution builds it: the system compiler and the server's own CFLAGS,
# with warnings as errors.
#
# Usage: dev/qualify-pgdg.sh BINDIR...
#   e.g. dev/qualify-pgdg.sh /usr/pgsql-17/bin /usr/pgsql-18/bin /usr/pgsql-19/bin
#
# Each BINDIR holds pg_config, initdb, pg_ctl, postgres, psql, pg_dump and
# pg_restore for one major.  Runs the regression suite, test/dump and
# test/upgrade/run.sh and test/upgrade/inplace.sh (whose 0.1.0 starting
# points test/upgrade/stage.sh builds from this repository's history).
# The extension is installed into a per-run DESTDIR that the server is
# pointed at, so nothing is written into the server's prefix.  That needs
# extension_control_path (PostgreSQL 18+); for 17 the script falls back to
# `sudo make install` into the server's prefix, which a disposable test
# host can afford.
#
# Build output goes under $PG_GAPS_BUILD_DIR (default /scratch/pg_gaps when
# it exists, else $TMPDIR).  Prints one PASS or FAIL line per major and
# exits non-zero if any failed.
set -euo pipefail

src="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
root="${PG_GAPS_BUILD_DIR:-}"
if [ -z "$root" ]; then
	if [ -d /scratch ] && [ -w /scratch ]; then
		root=/scratch/pg_gaps
	else
		root="${TMPDIR:-/tmp}/pg_gaps"
	fi
fi

# Qualify one major.  Runs in a subshell so a failure ends only this major.
qualify() {
	local bindir="$1" pgc major build run
	pgc="$bindir/pg_config"
	major="$("$pgc" --version | sed -E 's/^PostgreSQL ([0-9]+).*/\1/')"
	build="$root/qual-pg$major"
	mkdir -p "$build"
	run="$(mktemp -d "$build/run.XXXXXX")"
	cd "$build"

	mk() { make -s -f "$src/Makefile" VPATH="$src" PG_CONFIG="$pgc" "$@"; }

	# Start clean so a stale object from another compiler cannot mask a
	# warning.
	mk clean >/dev/null
	mk COPT=-Werror
	mk check-ascii >/dev/null

	local conf="" extdir libdir stage=(bash "$src/test/upgrade/stage.sh" "$pgc" "$build/upgrade")
	if [ "$major" -ge 18 ]; then
		mk install DESTDIR="$run/inst" >/dev/null
		extdir="$run/inst$("$pgc" --sharedir)/extension"
		libdir="$run/inst$("$pgc" --pkglibdir)"
		"${stage[@]}" "$libdir" "$extdir" >/dev/null
		conf="extension_control_path = '$run/inst$("$pgc" --sharedir):\$system'
dynamic_library_path = '$run/inst$("$pgc" --pkglibdir):\$libdir'"
	else
		sudo -n make -s -f "$src/Makefile" VPATH="$src" PG_CONFIG="$pgc" install >/dev/null
		extdir="$("$pgc" --sharedir)/extension"
		libdir="$("$pgc" --pkglibdir)"
		sudo -n "${stage[@]}" "$libdir" "$extdir" >/dev/null
	fi

	"$bindir/initdb" -D "$run/data" -U postgres --no-locale -E UTF8 >/dev/null 2>&1
	cat >>"$run/data/postgresql.conf" <<EOF
listen_addresses = ''
unix_socket_directories = '$run'
$conf
EOF
	"$bindir/pg_ctl" -D "$run/data" -l "$run/server.log" -w start >/dev/null
	local rc=0
	mk installcheck PGHOST="$run" PGUSER=postgres || rc=$?
	if [ "$rc" -eq 0 ]; then
		PATH="$bindir:$PATH" PGHOST="$run" PGUSER=postgres PGDATABASE=postgres \
			bash "$src/test/dump/run.sh" >/dev/null || rc=$?
	fi
	if [ "$rc" -eq 0 ]; then
		PATH="$bindir:$PATH" PGHOST="$run" PGUSER=postgres PGDATABASE=postgres \
			PG_GAPS_EXTDIR="$extdir" bash "$src/test/upgrade/run.sh" >/dev/null || rc=$?
	fi
	if [ "$rc" -eq 0 ]; then
		PATH="$bindir:$PATH" PGHOST="$run" PGUSER=postgres PGDATABASE=postgres \
			bash "$src/test/upgrade/inplace.sh" "$libdir" "$extdir" >/dev/null || rc=$?
	fi
	"$bindir/pg_ctl" -D "$run/data" -w -m fast stop >/dev/null
	if [ "$rc" -ne 0 ]; then
		echo "  diffs: $build/regression.diffs  log: $run/server.log" >&2
		return 1
	fi
}

status=0
for bindir in "$@"; do
	major="$("$bindir/pg_config" --version)"
	if (qualify "$bindir"); then
		echo "PASS $major ($(cc --version | head -1))"
	else
		echo "FAIL $major"
		status=1
	fi
done
exit "$status"
