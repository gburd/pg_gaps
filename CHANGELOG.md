# Changelog

## Unreleased

Gibbs sampling for components too large for exact inference: set
`gaps.samples` (default 0, never) and `gaps.seed`.  `marginals` and
`impute` gain a `samples` column (NULL for exact) and report method
'gibbs' or 'gibbs-marginal-argmax'; `_infer` gains `samples` and returns
NULL `map_probability` for a sampled component.  A too-large component
with a zero CPT entry is refused.  Four chains run per sampled component
(`gaps.samples` counts sweeps per chain); when two chains' estimates of a
probability differ by more than 0.1, the query fails rather than return a
distribution that has not converged.  `bench/hotel.sql` runs the hotel
workload with its vertical factors.

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

`gaps.validate(model)`: the dependency-graph check of [W11 3.3.2], run
from the catalog without reading data.  It proves a model's ground
network acyclic for any table contents, or names the cycle it could not
rule out.  Its proofs use only quals on columns fixed per entity (entity
keys, or any column when an immediate primary key on a table without
inheritance children makes each entity one row) whose type's comparison
operators are a btree total order and whose input does not depend on
session settings.  A variable that is its own parent in the ground
network is now reported as a directed cycle rather than a malformed
network.  validate refuses a model with more than 100 factors or
20000 parent slots, or whose stripe tests exceed a fixed budget.
