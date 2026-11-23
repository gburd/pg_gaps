#!/usr/bin/env bash
#
# dev/regress.sh: build pg_gaps out of tree and run its regression suite
# against a throwaway cluster.  The inner loop; `nix flake check` is the gate.
#
# Usage: dev/regress.sh [pg_regress test name ...]
#        dev/regress.sh -- command [args ...]
#
# With "--", runs the command instead of the regression suite, with PGHOST
# and PGUSER set for the throwaway cluster (where pg_gaps is installable).
# `dev/regress.sh -- upgrade` stages the 0.1.0 starting points next to the
# build's install (test/upgrade/stage.sh) and runs the upgrade tests
# (test/upgrade/run.sh, then the in-place update, test/upgrade/inplace.sh).
#
# Uses whatever pg_config and postgres are on PATH (the dev shell provides
# PostgreSQL 18).  Needs PostgreSQL 18 or later, because it points the server
# at the build's install tree with extension_control_path instead of
# installing into the server's own (read-only, under Nix) prefix.  The flake
# check covers 17.
#
# Build output goes to $PG_GAPS_BUILD_DIR, by default under /scratch when it
# exists, so nothing is written into the source tree.  A passing run moves
# its cluster to the trash when trash-cli is installed and otherwise leaves
# it in place; a failing one always keeps it and prints where.
set -euo pipefail

src="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
major="$(pg_config --version | sed -E 's/^PostgreSQL ([0-9]+).*/\1/')"
if [ "$major" -lt 18 ]; then
	echo "dev/regress.sh needs PostgreSQL 18+ (found $major); use nix flake check" >&2
	exit 1
fi

if [ -z "${PG_GAPS_BUILD_DIR:-}" ]; then
	if [ -d /scratch ] && [ -w /scratch ]; then
		PG_GAPS_BUILD_DIR="/scratch/pg_gaps/build-pg$major"
	else
		PG_GAPS_BUILD_DIR="${TMPDIR:-/tmp}/pg_gaps/build-pg$major"
	fi
fi
mkdir -p "$PG_GAPS_BUILD_DIR"
cd "$PG_GAPS_BUILD_DIR"

mk() { make -s -f "$src/Makefile" VPATH="$src" "$@"; }

# Build, then install into a per-run DESTDIR the server is pointed at.
mk
run="$(mktemp -d "$PG_GAPS_BUILD_DIR/run.XXXXXX")"
mk install DESTDIR="$run/inst" >/dev/null

initdb -D "$run/data" -U postgres --no-locale -E UTF8 >/dev/null 2>&1
cat >>"$run/data/postgresql.conf" <<EOF
listen_addresses = ''
unix_socket_directories = '$run'
extension_control_path = '$run/inst$(pg_config --sharedir):\$system'
dynamic_library_path = '$run/inst$(pg_config --pkglibdir):\$libdir'
EOF
pg_ctl -D "$run/data" -l "$run/server.log" -w start >/dev/null
trap 'pg_ctl -D "$run/data" -w -m fast stop >/dev/null' EXIT

if [ "${1:-}" = "--" ]; then
	shift
	if [ "${1:-}" = "upgrade" ]; then
		libdir="$run/inst$(pg_config --pkglibdir)"
		PG_GAPS_EXTDIR="$run/inst$(pg_config --sharedir)/extension"
		export PG_GAPS_EXTDIR
		"$src/test/upgrade/stage.sh" "$(command -v pg_config)" "$PG_GAPS_BUILD_DIR/upgrade" \
			"$libdir" "$PG_GAPS_EXTDIR"
		upgrade_tests() {
			bash "$src/test/upgrade/run.sh" &&
				bash "$src/test/upgrade/inplace.sh" "$libdir" "$PG_GAPS_EXTDIR"
		}
		set -- upgrade_tests
	fi
	run_test() { PGHOST="$run" PGUSER=postgres PGDATABASE=postgres "$@"; }
else
	run_test() {
		local regress=()
		if [ "$#" -gt 0 ]; then
			regress=("REGRESS=$*")
		fi
		mk installcheck PGHOST="$run" PGUSER=postgres "${regress[@]}"
	}
fi
if run_test "$@"; then
	pg_ctl -D "$run/data" -w -m fast stop >/dev/null
	trap - EXIT
	if command -v trash >/dev/null; then
		trash "$run"
	else
		echo "passed; cluster left in $run"
	fi
else
	echo "FAILED: diffs in $PG_GAPS_BUILD_DIR/regression.diffs," \
		"server log in $run/server.log" >&2
	exit 1
fi
