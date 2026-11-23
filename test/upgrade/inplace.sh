#!/usr/bin/env bash
#
# test/upgrade/inplace.sh: the update as a package manager performs it.
#
# Usage: test/upgrade/inplace.sh LIBDIR EXTDIR
#        (with PGHOST, PGPORT and PGUSER set for a running server that
#        loads $libdir/pg_gaps from LIBDIR and reads extension scripts from
#        EXTDIR, where pg_gaps 0.2.0 is installed and test/upgrade/stage.sh
#        has staged the 0.1.0 starting points)
#
# test/upgrade/run.sh gives each 0.1.0 starting point its own version name
# and library, so both live beside 0.2.0.  A real server has one of each:
# extension version '0.1.0' whose C functions name $libdir/pg_gaps, and a
# package update that replaces that file and the scripts under the same
# names.  For each starting point this script installs its library as
# pg_gaps.so and its script as pg_gaps--0.1.0.sql, creates the extension
# and its models, and records their marginals.  Then it puts the 0.2.0
# library back in place, as a package update does, and requires: a new
# session's marginals fail with the version mismatch; ALTER EXTENSION
# pg_gaps UPDATE, with no target, reaches 0.2.0; and the marginals are
# then what the old library returned, bit for bit.
#
# It writes LIBDIR and EXTDIR, with sudo -n when they are not writable, so
# it suits a throwaway install only.  It leaves both as it found them.
# The flake check cannot run it: its library directory is in the read-only
# Nix store.
set -euo pipefail

libdir="${1:?usage: inplace.sh LIBDIR EXTDIR}"
extdir="${2:?usage: inplace.sh LIBDIR EXTDIR}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sudo=
if [ ! -w "$libdir" ] || [ ! -w "$extdir" ]; then
	sudo="sudo -n"
fi
work="$(mktemp -d)"

psql_q() { psql -X -q -v ON_ERROR_STOP=1 "$@"; }
fail() {
	echo "FAIL: $*" >&2
	exit 1
}

# Replace $2 by a copy of $1 through a rename, so that a backend that has
# the old file mapped keeps its own copy, as with a package update.
put() {
	$sudo cp "$1" "$2.inplace-new"
	$sudo chmod 644 "$2.inplace-new"
	$sudo mv -f "$2.inplace-new" "$2"
}

# The 0.2.0 library and the staged 0.1.0 script, restored on any exit.
for f in "$libdir/pg_gaps.so" "$extdir/pg_gaps--0.1.0.sql" \
	"$libdir/pg_gaps_v010.so" "$libdir/pg_gaps_d35680c.so" \
	"$extdir/pg_gaps--0.1.0-v0.1.0.sql" "$extdir/pg_gaps--0.1.0-d35680c.sql"; do
	[ -f "$f" ] || fail "$f is missing; run test/upgrade/stage.sh first"
done
cp "$libdir/pg_gaps.so" "$work/new.so"
cp "$extdir/pg_gaps--0.1.0.sql" "$work/staged.sql"
restore() {
	put "$work/new.so" "$libdir/pg_gaps.so"
	put "$work/staged.sql" "$extdir/pg_gaps--0.1.0.sql"
	rm -f "$work"/*
	rmdir "$work"
}
trap restore EXIT

marginals_q="SET extra_float_digits = 3;
	SELECT m.name, x.rel, x.attname, x.entity::text, x.value, x.probability, x.component,
		x.method, x.samples
	FROM gaps.model m, gaps.marginals(m.name) x ORDER BY 1, 2, 3, 4, 5"
version_q="SELECT extversion FROM pg_extension WHERE extname = 'pg_gaps'"

# Update one starting point in place.  $1 names its library, $2 its
# extension version in EXTDIR, $3.. are the model scripts to run under it.
check_inplace() {
	local lib="$1" ver="$2" db="pg_gaps_upgrade_inplace" out
	shift 2
	put "$libdir/$lib.so" "$libdir/pg_gaps.so"
	put "$extdir/pg_gaps--$ver.sql" "$extdir/pg_gaps--0.1.0.sql"
	psql_q -d postgres -c "DROP DATABASE IF EXISTS $db" -c "CREATE DATABASE $db"
	psql_q -d "$db" -c "CREATE EXTENSION pg_gaps VERSION '0.1.0'"
	for f in "$@"; do
		psql_q -d "$db" -f "$here/$f" >/dev/null
	done
	psql_q -d "$db" -At -c "$marginals_q" >"$work/before"
	[ -s "$work/before" ] || fail "$ver in place: no marginals before the update"

	# The package update: the 0.2.0 library replaces the old one.
	put "$work/new.so" "$libdir/pg_gaps.so"
	if out="$(psql -X -q -d "$db" -c "SELECT count(*) FROM gaps.marginals('given')" 2>&1)"; then
		fail "$ver in place: the 0.2.0 library ran under the 0.1.0 SQL"
	fi
	grep -q "pg_gaps library 0.2.0 is loaded but the extension's SQL is 0.1.0" <<<"$out" ||
		fail "$ver in place: unexpected error for a version mismatch: $out"

	psql_q -d "$db" -c "ALTER EXTENSION pg_gaps UPDATE"
	[ "$(psql_q -d "$db" -At -c "$version_q")" = 0.2.0 ] ||
		fail "$ver in place: ALTER EXTENSION pg_gaps UPDATE did not reach 0.2.0"
	psql_q -d "$db" -At -c "$marginals_q" >"$work/after"
	if ! cmp -s "$work/before" "$work/after"; then
		diff "$work/before" "$work/after" | head -20 >&2
		fail "$ver in place: marginals differ after the update"
	fi
	echo "upgrade $ver in place: mismatch refused, then updated;" \
		"$(wc -l <"$work/before") marginals identical, bit for bit"
	psql_q -d postgres -c "DROP DATABASE $db"
}

check_inplace pg_gaps_d35680c 0.1.0-d35680c d35680c.sql
check_inplace pg_gaps_v010 0.1.0-v0.1.0 d35680c.sql v0.1.0.sql
