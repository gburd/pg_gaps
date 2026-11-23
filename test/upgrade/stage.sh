#!/usr/bin/env bash
#
# test/upgrade/stage.sh: install the two 0.1.0 starting points that
# test/upgrade/run.sh updates from, next to an installed pg_gaps 0.2.0.
#
# Usage: test/upgrade/stage.sh PG_CONFIG BUILD_DIR LIBDIR EXTDIR [V010_TREE D35680C_TREE]
#
# "0.1.0" names two catalogs: the release (tag v0.1.0, db04064) and a
# pre-release in production use (d35680c).  Each is built from its own
# source tree, its library renamed pg_gaps_<tag> so that both can sit
# beside the 0.2.0 library, and installed as its own extension version,
# 0.1.0-<tag>, whose secondary control file points at that library.  Each
# gets a copy of the real pg_gaps--0.1.0--0.2.0.sql as its update edge.
# d35680c's script is also installed as plain 0.1.0, which runs on the
# 0.2.0 library: the state of a server whose library was replaced and whose
# extension was not updated, as pg.ddx.io found it.
#
# The source trees default to `git archive` of the two commits from the
# repository this script is in (safe.directory set, since CI runs this
# with sudo in a checkout another user owns).  Objects go under BUILD_DIR; the libraries
# go to LIBDIR and the scripts and control files to EXTDIR (normally
# `pg_config --pkglibdir` and `pg_config --sharedir`/extension, possibly
# under a DESTDIR).
set -euo pipefail

pgc="${1:?usage: stage.sh PG_CONFIG BUILD_DIR LIBDIR EXTDIR [V010_TREE D35680C_TREE]}"
build="${2:?build directory}"
libdir="${3:?library directory}"
extdir="${4:?extension directory}"
src="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
declare -A commit=([v010]=db040647e79b2354d4dfbef06fd09e92a112101c
	[d35680c]=d35680cc3303e9791465256bfa9f5d8feb5615c2)
declare -A tree=([v010]="${5:-}" [d35680c]="${6:-}")
declare -A version=([v010]=0.1.0-v0.1.0 [d35680c]=0.1.0-d35680c)

mkdir -p "$build" "$libdir" "$extdir"
for tag in v010 d35680c; do
	t="${tree[$tag]}"
	if [ -z "$t" ]; then
		t="$build/src-$tag"
		if [ ! -f "$t/Makefile" ]; then
			if ! git -c safe.directory="$src" -C "$src" cat-file -e "${commit[$tag]}^{commit}" 2>/dev/null; then
				echo "stage.sh: commit ${commit[$tag]} is not in $src;" \
					"fetch the full history (git fetch --unshallow)" >&2
				exit 1
			fi
			mkdir -p "$t"
			git -c safe.directory="$src" -C "$src" archive "${commit[$tag]}" | tar -x -C "$t"
		fi
	fi

	# Build out of tree, as the module pg_gaps_<tag>.
	mkdir -p "$build/obj-$tag"
	make -s -C "$build/obj-$tag" -f "$t/Makefile" VPATH="$t" PG_CONFIG="$pgc" \
		MODULE_big="pg_gaps_$tag" >/dev/null
	install -m 755 "$build/obj-$tag/pg_gaps_$tag.so" "$libdir/"

	v="${version[$tag]}"
	install -m 644 "$t/pg_gaps--0.1.0.sql" "$extdir/pg_gaps--$v.sql"
	install -m 644 "$src/pg_gaps--0.1.0--0.2.0.sql" "$extdir/pg_gaps--$v--0.2.0.sql"
	echo "module_pathname = '\$libdir/pg_gaps_$tag'" >"$extdir/pg_gaps--$v.control"
	chmod 644 "$extdir/pg_gaps--$v.control"
done
install -m 644 "${tree[d35680c]:-$build/src-d35680c}/pg_gaps--0.1.0.sql" "$extdir/pg_gaps--0.1.0.sql"
