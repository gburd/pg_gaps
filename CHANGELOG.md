# Changelog

## Unreleased

Project foundations: PGXS build, module stub, an empty install script for
0.1.0, a regression test that installs the extension and loads its
library, a Nix flake that builds and tests against PostgreSQL 17, 18 and
19, and a CI workflow.  No SQL objects yet.

Row estimates for `gaps.marginals`, `gaps.impute` and `gaps.learn_cpts`: a
planner support function estimates their rows from the model catalog and
the column statistics when the model name is a constant, and each declares
a default `ROWS` for the generic-plan case.  Planning never waits for a
lock on the catalog, gives no estimate on a hot standby or under
`SERIALIZABLE`, and a cached plan replans when a data table is analyzed.
