#!/usr/bin/env bash
#
# test/dump/run.sh: a model survives pg_dump and restore.
#
# Usage: test/dump/run.sh   (with PGHOST, PGPORT and PGUSER set for a
#                            running server where pg_gaps is installable,
#                            e.g. through dev/regress.sh -- test/dump/run.sh)
#
# Builds a model with vertical and horizontal factors, a coarse entity set
# and a learned CPT in one database, dumps it with pg_dump, restores it
# into a fresh database, and requires gaps.marginals to return the same
# rows with the same probabilities in both.  Then checks that renaming
# and dropping a column the model uses, and dropping its table, make
# inference fail with an error naming what changed.  The factors' CPTs and
# priors, and the attributes' bin edges, must also come back unchanged.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src_db="pg_gaps_dump_src"
dst_db="pg_gaps_dump_dst"
work="$(mktemp -d)"
trap 'rm -f "$work"/*; rmdir "$work"' EXIT

psql_q() { psql -X -q -v ON_ERROR_STOP=1 "$@"; }

for db in "$src_db" "$dst_db"; do
	psql_q -d postgres -c "DROP DATABASE IF EXISTS $db"
	psql_q -d postgres -c "CREATE DATABASE $db"
done

psql_q -d "$src_db" -f "$here/model.sql"
psql_q -d "$src_db" -At -c "SELECT entity::text, attname, value, probability
	FROM gaps.marginals('dump') ORDER BY 1, 2, 3" >"$work/before"
factors_q="SELECT name, cpt, prior FROM gaps.factor ORDER BY name"
edges_q="SELECT relname, attname, domain, edges FROM gaps.attribute ORDER BY 1, 2"
psql_q -d "$src_db" -At -c "$factors_q" >"$work/factors_before"
psql_q -d "$src_db" -At -c "$edges_q" >"$work/edges_before"

pg_dump -d "$src_db" -Fc -f "$work/dump"
pg_restore -d "$dst_db" "$work/dump"
psql_q -d "$dst_db" -At -c "SELECT entity::text, attname, value, probability
	FROM gaps.marginals('dump') ORDER BY 1, 2, 3" >"$work/after"
psql_q -d "$dst_db" -At -c "$factors_q" >"$work/factors_after"
psql_q -d "$dst_db" -At -c "$edges_q" >"$work/edges_after"

if [ ! -s "$work/before" ]; then
	echo "FAIL: the model returned no marginals before the dump" >&2
	exit 1
fi
if ! cmp -s "$work/before" "$work/after"; then
	echo "FAIL: marginals differ after pg_dump and restore" >&2
	diff "$work/before" "$work/after" | head -20 >&2
	exit 1
fi
echo "dump: $(wc -l <"$work/before") marginals identical after pg_dump and restore"
if ! grep -q '^light|.*|{2,0.5,0.25,3}$' "$work/factors_before"; then
	echo "FAIL: factor light has no prior before the dump" >&2
	exit 1
fi
if ! cmp -s "$work/factors_before" "$work/factors_after"; then
	echo "FAIL: CPTs or priors differ after pg_dump and restore" >&2
	diff "$work/factors_before" "$work/factors_after" >&2
	exit 1
fi
echo "dump: $(wc -l <"$work/factors_before") factors' CPTs and priors identical after restore"
if ! grep -q '^public.reading|lux|{dim,lit,bright}|{10,100}$' "$work/edges_before"; then
	echo "FAIL: attribute lux has no bin edges before the dump" >&2
	exit 1
fi
if ! grep -q '^lux|{0.3333333333333333,0.3333333333333333,0.3333333333333333}|$' \
	"$work/factors_before"; then
	echo "FAIL: factor lux did not learn one entity per bin" >&2
	exit 1
fi
if ! cmp -s "$work/edges_before" "$work/edges_after"; then
	echo "FAIL: attributes' domains or bin edges differ after pg_dump and restore" >&2
	diff "$work/edges_before" "$work/edges_after" >&2
	exit 1
fi
echo "dump: $(wc -l <"$work/edges_before") attributes' domains and bin edges identical after restore"

# The restored model must still find its table by name, and refuse when the
# table it was declared over has changed.
expect_error() {
	local what="$1" pattern="$2"
	shift 2
	if out="$(psql -X -q -d "$dst_db" -c "SELECT count(*) FROM gaps.marginals('dump')" 2>&1)"; then
		echo "FAIL: $what: inference succeeded" >&2
		exit 1
	fi
	if ! grep -q "$pattern" <<<"$out"; then
		echo "FAIL: $what: unexpected error: $out" >&2
		exit 1
	fi
	echo "dump: $what is refused: $(grep -o 'ERROR:.*' <<<"$out")"
}
psql_q -d "$dst_db" -c "ALTER TABLE sensor RENAME COLUMN light TO lux"
expect_error "a renamed column" 'column "light" of relation "public.sensor" no longer exists'
psql_q -d "$dst_db" -c "ALTER TABLE sensor RENAME COLUMN lux TO light"
psql_q -d "$dst_db" -c "ALTER TABLE sensor ALTER COLUMN light TYPE varchar(10)"
expect_error "a column type change" 'changed type from text to character varying(10)'
psql_q -d "$dst_db" -c "ALTER TABLE sensor ALTER COLUMN light TYPE text"
psql_q -d "$dst_db" -c "DROP TABLE sensor"
expect_error "a dropped table" 'relation "public.sensor" of model attribute'

for db in "$src_db" "$dst_db"; do
	psql_q -d postgres -c "DROP DATABASE $db"
done
