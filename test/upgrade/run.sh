#!/usr/bin/env bash
#
# test/upgrade/run.sh: both 0.1.0 catalogs update to exactly 0.2.0, and
# their models mean what they meant.
#
# Usage: PG_GAPS_EXTDIR=DIR test/upgrade/run.sh
#        (with PGHOST, PGPORT and PGUSER set for a running server where
#        pg_gaps 0.2.0 is installable, test/upgrade/stage.sh has installed
#        the 0.1.0 starting points, and DIR is the extension directory the
#        server reads them from)
#
# For each starting point (the released v0.1.0 and the pre-release
# d35680c, extension versions 0.1.0-v0.1.0 and 0.1.0-d35680c, each with its
# own library, which its C functions must name before the update), builds
# that version's models, revokes PUBLIC's INSERT on
# gaps.factor, records marginals, CPTs, priors, parameters, members, edges
# and lineage, runs ALTER EXTENSION pg_gaps UPDATE TO '0.2.0', and requires:
# the same marginals, bit for bit, from the 0.2.0 library; the same catalog
# contents; the REVOKE still in place; and the extension's objects
# (test/upgrade/objects.sql) equal to a fresh 0.2.0 install's, with no
# function left naming an old library.  Every update script the server
# runs must be byte for byte the shipped pg_gaps--0.1.0--0.2.0.sql.  Then
# checks
# that the 0.2.0 library refuses to run under the 0.1.0 SQL, naming both
# versions, and that the planner still plans such a call.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fresh_db="pg_gaps_upgrade_fresh"
work="$(mktemp -d)"
trap 'rm -f "$work"/*; rmdir "$work"' EXIT

psql_q() { psql -X -q -v ON_ERROR_STOP=1 "$@"; }
fail() {
	echo "FAIL: $*" >&2
	exit 1
}
recreate() {
	psql_q -d postgres -c "DROP DATABASE IF EXISTS $1" -c "CREATE DATABASE $1"
}

# Every value is printed with 17 significant digits, so equal text means
# equal doubles.
marginals_q="SET extra_float_digits = 3;
	SELECT m.name, x.rel, x.attname, x.entity::text, x.value, x.probability, x.component,
		x.method, x.samples
	FROM gaps.model m, gaps.marginals(m.name) x ORDER BY 1, 2, 3, 4, 5"
# Each row as jsonb, without the columns 0.2.0 adds where they hold the
# value an existing row gets in the update (NULL, kind 'cpt', aggregate and
# frozen false), so that a 0.1.0 row and its updated self compare equal
# and a value the update changed does not.
catalog_q="SET extra_float_digits = 3;
	CREATE FUNCTION pg_temp.j(r jsonb) RETURNS jsonb LANGUAGE sql AS \$\$
		SELECT jsonb_strip_nulls(r - (CASE WHEN r->>'kind' = 'cpt' THEN 'kind' END)
			- (CASE WHEN r->>'frozen' = 'false' THEN 'frozen' END)
			- (CASE WHEN r->>'aggregate' = 'false' THEN 'aggregate' END))
	\$\$;
	SELECT 'factor', m.name, f.name, f.cpt::text,
		pg_temp.j(to_jsonb(f) - 'factor_id' - 'model_id' - 'child_stripe')
	FROM gaps.factor f JOIN gaps.model m USING (model_id)
	UNION ALL
	SELECT 'parent', f.name, p.ord::text, s.name,
		pg_temp.j(to_jsonb(p) - 'factor_id' - 'parent_stripe')
	FROM gaps.factor_parent p JOIN gaps.factor f USING (factor_id)
		JOIN gaps.stripe s ON s.stripe_id = p.parent_stripe
	UNION ALL
	SELECT 'attribute', m.name, a.attname, a.relname,
		pg_temp.j(to_jsonb(a) - 'attribute_id' - 'model_id' - 'rel')
	FROM gaps.attribute a JOIN gaps.model m USING (model_id)
	UNION ALL
	SELECT 'model', m.name, '', '', pg_temp.j(to_jsonb(m) - 'model_id') FROM gaps.model m
	ORDER BY 1, 2, 3"
# d35680c has no gaps.model_member.
members_q="SELECT c.name, mm.ord, m.name
	FROM gaps.model_member mm JOIN gaps.model c ON c.model_id = mm.model_id
		JOIN gaps.model m ON m.model_id = mm.member_id ORDER BY 1, 2"
grants_q="SELECT has_table_privilege('public', 'gaps.factor', 'INSERT'),
	has_table_privilege('public', 'gaps.factor', 'SELECT'),
	has_table_privilege('public', 'gaps.model', 'INSERT')"

# Record database $1's marginals, catalog contents and privileges as
# $work/*_$2.
snapshot() {
	local db="$1" when="$2"
	psql_q -d "$db" -At -c "$marginals_q" >"$work/marginals_$when"
	psql_q -d "$db" -At -c "$catalog_q" >"$work/catalog_$when"
	if [ -n "$(psql_q -d "$db" -At -c "SELECT to_regclass('gaps.model_member')")" ]; then
		psql_q -d "$db" -At -c "$members_q" >>"$work/catalog_$when"
	fi
	psql_q -d "$db" -At -c "$grants_q" >"$work/grants_$when"
}

# The update scripts the server will run are the one this tree ships, and
# both starting points are installed: a missing fixture is a failure, never
# a skipped test.
extdir="${PG_GAPS_EXTDIR:?set PG_GAPS_EXTDIR to the extension directory of the server}"
shipped="$here/../../pg_gaps--0.1.0--0.2.0.sql"
for f in pg_gaps--0.1.0--0.2.0.sql pg_gaps--0.1.0-v0.1.0--0.2.0.sql \
	pg_gaps--0.1.0-d35680c--0.2.0.sql; do
	cmp "$shipped" "$extdir/$f" || fail "$extdir/$f is not the shipped update script"
done
for v in 0.1.0 0.1.0-v0.1.0 0.1.0-d35680c; do
	[ "$(psql_q -d postgres -At -c "SELECT count(*) FROM pg_available_extension_versions
		WHERE name = 'pg_gaps' AND version = '$v'")" = 1 ] ||
		fail "pg_gaps version $v is not installed; run test/upgrade/stage.sh first"
done
echo "upgrade: the server runs the shipped update script for each starting point"

# The C functions of the database, by the library they name.
modules_q="SELECT string_agg(DISTINCT probin, ',' ORDER BY probin) FROM pg_proc
	WHERE pronamespace = 'gaps'::regnamespace AND probin IS NOT NULL"

# The fresh 0.2.0 install every update must equal.
recreate "$fresh_db"
psql_q -d "$fresh_db" -c "CREATE EXTENSION pg_gaps VERSION '0.2.0'"
psql_q -d "$fresh_db" -At -f "$here/objects.sql" >"$work/objects_fresh"
if ! grep -q '^function gaps.marginals(text,regclass,jsonb) ' "$work/objects_fresh" ||
	! grep -q '^config gaps.model_member$' "$work/objects_fresh"; then
	fail "the object listing of a fresh 0.2.0 install is incomplete"
fi

# Update one starting point and check it.  $1 names it, $2.. are the model
# scripts to run under it.
check_update() {
	local tag="$1" db="pg_gaps_upgrade_${1//./_}" out
	shift
	recreate "$db"
	psql_q -d "$db" -c "CREATE EXTENSION pg_gaps VERSION '0.1.0-$tag'"
	out="$(psql_q -d "$db" -At -c "$modules_q")"
	[ "$out" = "\$libdir/pg_gaps_${tag//./}" ] ||
		fail "$tag: its C functions name $out, not its own library"
	echo "upgrade $tag: before the update its C functions load $out"
	for f in "$@"; do
		psql_q -d "$db" -f "$here/$f" >/dev/null
	done
	psql_q -d "$db" -c "REVOKE INSERT ON gaps.factor FROM PUBLIC"

	snapshot "$db" before
	[ -s "$work/marginals_before" ] || fail "$tag: no marginals before the update"
	if awk -F'|' '$6 > 0 && $6 < 1 { n++ } END { exit !(n == 0) }' "$work/marginals_before"; then
		fail "$tag: every probability is 0 or 1 before the update"
	fi
	[ "$(cat "$work/grants_before")" = "f|t|t" ] || fail "$tag: the REVOKE did not take"

	# A user view over a function the update drops blocks the update.  It
	# must fail whole, leaving the extension at its old version with
	# nothing created, and succeed once the view is dropped.
	if [ "$tag" = d35680c ]; then
		psql_q -d "$db" -c "CREATE VIEW public.scores AS SELECT * FROM gaps.marginals('given')"
		if out="$(psql -X -q -d "$db" -c "ALTER EXTENSION pg_gaps UPDATE TO '0.2.0'" 2>&1)"; then
			fail "$tag: the update dropped gaps.marginals(text) under a dependent view"
		fi
		grep -q 'cannot drop function marginals(text) because other objects depend on it' \
			<<<"$out" || fail "$tag: unexpected error for a dependent view: $out"
		[ "$(psql_q -d "$db" -At -c "SELECT extversion FROM pg_extension
			WHERE extname = 'pg_gaps'")|$(psql_q -d "$db" -At -c "SELECT
			to_regclass('gaps.model_member') IS NULL")" = "0.1.0-$tag|t" ] ||
			fail "$tag: the failed update left changes behind"
		echo "upgrade $tag: a dependent view stops the update whole: $(grep -o 'ERROR:.*' <<<"$out")"
		psql_q -d "$db" -c "DROP VIEW public.scores"
	fi

	psql_q -d "$db" -c "ALTER EXTENSION pg_gaps UPDATE TO '0.2.0'"

	snapshot "$db" after
	psql_q -d "$db" -At -f "$here/objects.sql" >"$work/objects_after"
	out="$(psql_q -d "$db" -At -c "$modules_q")"
	[ "$out" = "\$libdir/pg_gaps" ] || fail "$tag: after the update, C functions name $out"
	echo "upgrade $tag: after the update every C function loads $out"

	if ! cmp -s "$work/marginals_before" "$work/marginals_after"; then
		diff "$work/marginals_before" "$work/marginals_after" | head -20 >&2
		fail "$tag: marginals differ after the update"
	fi
	echo "upgrade $tag: $(wc -l <"$work/marginals_before") marginals identical, bit for bit"
	if ! cmp -s "$work/catalog_before" "$work/catalog_after"; then
		diff "$work/catalog_before" "$work/catalog_after" >&2
		fail "$tag: the catalog's contents differ after the update"
	fi
	echo "upgrade $tag: $(wc -l <"$work/catalog_before") catalog rows identical"
	[ "$(cat "$work/grants_after")" = "f|t|t" ] ||
		fail "$tag: privileges changed in the update: $(cat "$work/grants_after")"
	echo "upgrade $tag: PUBLIC's revoked INSERT on gaps.factor stays revoked"
	if ! cmp -s "$work/objects_fresh" "$work/objects_after"; then
		diff "$work/objects_fresh" "$work/objects_after" | cut -c1-200 >&2
		fail "$tag: the updated extension's objects differ from a fresh 0.2.0 install's"
	fi
	echo "upgrade $tag: $(wc -l <"$work/objects_after") objects equal a fresh 0.2.0 install's"

	# The updated catalog keeps working: its triggers check a write, and
	# learning and the new functions run.
	if out="$(psql -X -q -d "$db" -c "UPDATE gaps.factor SET cpt = '{0.5,0.6}'
			WHERE name = 'prior1'" 2>&1)"; then
		fail "$tag: an updated catalog accepted a CPT that does not sum to 1"
	fi
	grep -q 'sum' <<<"$out" || fail "$tag: unexpected error for a bad CPT: $out"
	psql_q -d "$db" -c "SELECT gaps.set_frozen('learned', 'occ', true)" \
		-c "SELECT count(*) FROM gaps.learn_cpts('learned', method => 'em')" >/dev/null
	echo "upgrade $tag: an updated catalog checks writes and learns"
	psql_q -d postgres -c "DROP DATABASE $db"
}

check_update d35680c d35680c.sql
check_update v0.1.0 d35680c.sql v0.1.0.sql

# The 0.2.0 library under the 0.1.0 SQL: what a server runs after its
# package was replaced and before ALTER EXTENSION UPDATE.  Inference must
# refuse with both versions named; planning the call must not fail.
db="pg_gaps_upgrade_mismatch"
recreate "$db"
psql_q -d "$db" -c "CREATE EXTENSION pg_gaps VERSION '0.1.0'"
psql_q -d "$db" -f "$here/d35680c.sql" >/dev/null
psql_q -d "$db" -c "EXPLAIN SELECT * FROM gaps.marginals('given')" >/dev/null
if out="$(psql -X -q -d "$db" -c "SELECT count(*) FROM gaps.marginals('given')" 2>&1)"; then
	fail "the 0.2.0 library ran under the 0.1.0 SQL"
fi
grep -q "pg_gaps library 0.2.0 is loaded but the extension's SQL is 0.1.0" <<<"$out" ||
	fail "unexpected error for a version mismatch: $out"
grep -q 'ALTER EXTENSION pg_gaps UPDATE' <<<"$out" ||
	fail "the mismatch error has no hint: $out"
echo "upgrade: the 0.2.0 library refuses the 0.1.0 SQL: $(grep -o 'ERROR:.*' <<<"$out")"
psql_q -d postgres -c "DROP DATABASE $db" -c "DROP DATABASE $fresh_db"
