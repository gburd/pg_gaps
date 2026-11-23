# Contributing to pg_gaps

## Building

pg_gaps builds with PGXS against an installed PostgreSQL 17 or later.  You
need a C compiler and the server headers (`postgresql-server-dev-*`, or a
source install with `pg_config`).

```sh
make PG_CONFIG=/path/to/pg_config
sudo make install PG_CONFIG=/path/to/pg_config
make installcheck PG_CONFIG=/path/to/pg_config   # against a running server
```

`make installcheck` writes its results into the directory it runs in,
so it fails with "could not create directory" when the source tree is
not writable by the user running it.  Use `dev/regress.sh`, or an
out-of-tree (VPATH) build, which keeps objects and test output out of the
source directory:

```sh
mkdir -p /path/to/build && cd /path/to/build
make -f /path/to/pg_gaps/Makefile VPATH=/path/to/pg_gaps
```

### With Nix

```sh
nix develop                  # PostgreSQL 18, clang, make, shellcheck
dev/regress.sh               # build out of tree, run the suite on a throwaway cluster
nix flake check              # the gate: build + regress on PG 17, 18 and 19
nix build .#pg17             # the extension package for one major
```

`dev/regress.sh` needs PostgreSQL 18 or later, because it points the
server at the build's install tree with `extension_control_path` instead
of writing into the server's own prefix.  `nix flake check` covers 17.

## Tests

- **Regression** (`sql/*.sql` against `expected/*.out`): the SQL surface.
  Generate expected output from a real run (`dev/regress.sh` leaves it in
  `results/` under the build directory), read it, then copy it into
  `expected/`.
- **Oracles**: every inference path is checked against brute-force
  enumeration of possible worlds on small models, and the exact paths
  against pgmpy.  These arrive with Phase 2 (`ROADMAP.md`).

- **Upgrade** (`test/upgrade/run.sh`): each earlier catalog updates to
  the current one.  `test/upgrade/stage.sh` builds the libraries of the
  starting points from their commits and installs them as their own
  extension versions; the test builds models under each, updates, and
  requires the same marginals bit for bit, the same catalog contents, a
  `REVOKE` kept, and the extension's objects equal to a fresh install's
  (`test/upgrade/objects.sql`).  The last check fails when the update
  script misses anything the install script has.  It also requires that
  a view over a dropped function stops the update whole.
  `test/upgrade/inplace.sh` then performs the update as a package
  manager does: version '0.1.0' with the one library `$libdir/pg_gaps`,
  that file replaced by the new library, a mismatch error from a new
  session, `ALTER EXTENSION pg_gaps UPDATE`, and the same marginals bit
  for bit.  It rewrites the library directory, so `ci/pgdg.sh`,
  `dev/qualify-pgdg.sh` and `dev/regress.sh` run it and the flake does
  not.  Run both with `dev/regress.sh -- upgrade`.

A change that affects the probabilities returned comes with a test that
would have failed before it.  A change to the install script comes with
the same change to the update script from the last release (the object
check fails until it does).

## Style

PostgreSQL's own C conventions: tabs, pgindent-compatible layout, C99
without declarations after statements, `snake_case`.  Read a neighbouring
file when in doubt.  No new warnings under the server's `CFLAGS`; CI
builds with `-Werror`.  ASCII only in sources and install scripts (`make
check-ascii`).

The inference core (factor algebra, variable elimination, sampling,
Viterbi) is plain C with no PostgreSQL headers, so it can be compiled and
tested on its own; keep it that way.

## Submitting changes

There is no CLA and no sign-off requirement.  By contributing you agree
that your contribution is licensed under the PostgreSQL License
(`LICENSE`).

## Continuous integration

Codeberg (the origin) runs `.woodpecker/ci.yaml` on Woodpecker
(ci.codeberg.org); GitHub, which mirrors it, runs
`.github/workflows/ci.yml`.  Both build and test against the PGDG packages
of PostgreSQL 17, 18 and 19 through the same script, `ci/pgdg.sh`, so the
two cannot drift; GitHub also runs `nix flake check`.  Run `ci/pgdg.sh
MAJOR` in a `debian:trixie` container to reproduce either locally.

## Releasing

1. Bump `default_version` in `pg_gaps.control` and both `"version"`
   fields in `META.json` (`make check-meta` fails until they agree), and
   rename the install script as AGENTS.md rule 25 says.  Add the update
   script `pg_gaps--<old>--<new>.sql` to `DATA` in the Makefile.  It
   brings every catalog in the wild named `<old>` to the new install
   script's objects: `ADD COLUMN IF NOT EXISTS`, `CREATE TABLE` plus
   `pg_extension_config_dump` for new tables, `DROP FUNCTION IF EXISTS`
   for every signature whose arguments or results changed, then every
   function, trigger and policy as the install script has them (the tail
   of 0.1.0--0.2.0 is the install script with `CREATE OR REPLACE`, each
   trigger and policy dropped first, and no `GRANT`s).  Grant only on
   objects the script creates.  Then make `<old>` a starting point of
   `test/upgrade`: its commit and hash in `test/upgrade/stage.sh` and
   `flake.nix`, and a model script using its features.
2. Move `CHANGELOG.md`'s "Unreleased" section under the new version.
   Change `META.json`'s `release_status` from `testing` to `stable` for a
   release that should be PGXN's default.
3. Tag `vX.Y.Z` and push the tag to GitHub.  `.github/workflows/release.yml`
   builds `make dist` (a PGXN-layout `pg_gaps-X.Y.Z.zip` from `git
   archive`; `.gitattributes` keeps development files out), builds and
   tests the zip itself, attaches it to a GitHub Release, and uploads it to
   PGXN with the `PGXN_USER` and `PGXN_PASSWORD` repository secrets
   (skipped with a warning when they are not set).
4. The Codeberg-to-GitHub mirror can lag a tag push by hours (its
   interval is 8 h); force it with `tea api -l codeberg.org/gregburd -X POST
   /repos/gregburd/pg_gaps/push_mirrors-sync`.  Afterwards attach the same
   zip and its `.sha256sum` to a Codeberg release (`tea releases create`,
   then `tea releases assets create`), and check that the GitHub, PGXN and
   Codeberg zips have one sha256.
5. To republish an existing tag, run the workflow by hand
   (`workflow_dispatch`) with the tag; do not re-tag.  PGXN answers 409 for
   a version it already has.
