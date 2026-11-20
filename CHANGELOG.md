# Changelog

## Unreleased

Project foundations: PGXS build, module stub, an empty install script for
0.1.0, a regression test that installs the extension and loads its
library, a Nix flake that builds and tests against PostgreSQL 17, 18 and
19, and a CI workflow.  No SQL objects yet.

`gaps.validate(model)`: the dependency-graph check of [W11 3.3.2], run
from the catalog without reading data.  It proves a model's ground
network acyclic for any table contents, or names the cycle it could not
rule out.  Its proofs use only quals on columns fixed per entity (entity
keys, or any column when an immediate primary key on a table without
inheritance children makes each entity one row) whose type's comparison
operators are a btree total order and whose input does not depend on
session settings.  A variable that is its own parent in the ground
network is now reported as a directed cycle rather than a malformed
network.
