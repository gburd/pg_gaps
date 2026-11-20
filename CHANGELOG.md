# Changelog

## Unreleased

Gibbs sampling for components too large for exact inference: set
`gaps.samples` (default 0, never) and `gaps.seed`.  `marginals` and
`impute` gain a `samples` column (NULL for exact) and report method
'gibbs' or 'gibbs-marginal-argmax'; `_infer` gains `samples` and returns
NULL `map_probability` for a sampled component.  A too-large component
with a zero CPT entry is refused.  `bench/hotel.sql` runs the hotel
workload with its vertical factors.

Project foundations: PGXS build, module stub, an empty install script for
0.1.0, a regression test that installs the extension and loads its
library, a Nix flake that builds and tests against PostgreSQL 17, 18 and
19, and a CI workflow.  No SQL objects yet.
