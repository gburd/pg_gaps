# Changelog

## Unreleased

Project foundations: PGXS build, module stub, an empty install script for
0.1.0, a regression test that installs the extension and loads its
library, a Nix flake that builds and tests against PostgreSQL 17, 18 and
19, and a CI workflow.  No SQL objects yet.

Grounding identifies entities by a typed composite of their key columns
instead of a jsonb object, and builds jsonb names only for the variables
a result reports: `gaps._ground` and `gaps.marginals` are about twice as
fast on the hotel workload (`bench/RESULTS_GROUNDING_2026-11-20.md`).
`gaps._ground` no longer returns `var_entity`; `gaps._missing_names`
names a grounding's missing variables.  Grounding keeps one entity table
per key shape (the key columns' types, typmods and collations, not their
names) for the transaction and truncates it on reuse, so repeated calls
in one transaction hold a fixed number of locks, and models with
thousands of attributes ground without running out of lock table space.
A model with more than 100 distinct key shapes is refused with an error
naming the count.
The default (same-key) mapping now matches NULL keys as entity identity
does; explicit mappings stay SQL `=`.  `declare_attribute` refuses entity
keys that name a column twice.  A mapped parent row that grounding did
not see as an entity, which only a concurrent change can cause, is an
error that recommends REPEATABLE READ.
