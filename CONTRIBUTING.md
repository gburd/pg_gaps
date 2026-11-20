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

The build also works out of tree, which keeps objects and test output out
of the source directory:

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

A change that affects the probabilities returned comes with a test that
would have failed before it.

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

## Releasing

1. Bump `default_version` in `pg_gaps.control` and both `"version"`
   fields in `META.json` (`make check-meta` fails until they agree), and
   rename the install script as AGENTS.md rule 25 says.
2. Move `CHANGELOG.md`'s "Unreleased" section under the new version.
   Change `META.json`'s `release_status` from `testing` to `stable` for a
   release that should be PGXN's default.
3. Tag `vX.Y.Z` and push the tag to GitHub.  `.github/workflows/release.yml`
   builds `make dist` (a PGXN-layout `pg_gaps-X.Y.Z.zip` from `git
   archive`; `.gitattributes` keeps development files out), builds and
   tests the zip itself, attaches it to a GitHub Release, and uploads it to
   PGXN with the `PGXN_USER` and `PGXN_PASSWORD` repository secrets
   (skipped with a warning when they are not set).
4. To republish an existing tag, run the workflow by hand
   (`workflow_dispatch`) with the tag; do not re-tag.  PGXN answers 409 for
   a version it already has.
