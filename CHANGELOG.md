# Changelog

## 0.2.0 (unreleased)

**Updating from 0.1.0.**  Install the 0.2.0 package.  Then close every
existing connection (restart the server, or at least restart connection
poolers and long-lived clients), because a backend that has loaded the
0.1.0 library keeps it until it exits: such a backend running 0.2.0's
SQL can fail with "could not find function gaps_param_size", and an
`ALTER EXTENSION` run from it fails with "could not find function
gaps_expected_counts" and changes nothing.  Then run, in every database
that has the extension, from a new connection:

    ALTER EXTENSION pg_gaps UPDATE;

A view or SQL function of yours over `gaps.marginals`, `gaps.impute`,
`gaps.learn_cpts`, `gaps.create_model`, `gaps.declare_attribute` or
`gaps.add_factor` stops the update, since 0.2.0 drops and recreates
those (their arguments or result columns changed).  The update fails
with, for example:

    ERROR:  cannot drop function marginals(text) because other objects depend on it
    DETAIL:  view public.scores depends on function marginals(text)

and changes nothing: the extension stays at 0.1.0.  Drop such views
before the update and create them again after it.  From d35680c,
`marginals` and `impute` return 8 columns where they returned 6 or 7,
so a `SELECT *` view gets new columns when you recreate it.

Models, CPTs, priors, parameters and composite members are kept, and
every model returns the same marginals, bit for bit, as before the update
(`test/upgrade`).  Existing factors become `kind` 'cpt', not frozen, with
no prior; attributes have no bin edges; models have no lineage.  Until a
database is updated, the 0.2.0 library refuses to run under its 0.1.0
SQL: every call that reaches the C library fails with "pg_gaps library
0.2.0 is loaded but the extension's SQL is 0.1.0" and a hint to run
`ALTER EXTENSION pg_gaps UPDATE`.  Under the released 0.1.0's SQL those
are `marginals`, `impute`, `learn_cpts` with method 'em', and
`cpt_rows` or grounding of a logistic or noisy-OR factor; under
d35680c's, `marginals` and `impute`.  `learn_cpts` with method 'counts'
(the default) and the functions that edit the model run in SQL alone
and keep working under the old SQL.  On PostgreSQL 17.0 to 17.7, which
lack a system cache on `pg_extension`, the library reads `extversion`
on each such call instead of once per backend; that costs one index
scan, too little to measure next to inference.  (Before, the new library called by the
old SQL failed with "gaps._infer arguments must not be NULL", or worse.)

The same update handles both catalogs that were installed as 0.1.0: the
release (tag v0.1.0) and the earlier pre-release d35680c, which pg.ddx.io
runs.  d35680c has no `gaps.model_member`; the update creates it.

Privileges: the update grants nothing on a table that already exists, so
a `REVOKE` on a catalog table survives it.  It grants PUBLIC the same
rights on `gaps.model_member` as on the other catalog tables only when it
creates that table (from d35680c); revoke them again if you revoked the
others.  Functions whose arguments or results changed are dropped and
created again, and so are executable by PUBLIC afterwards: from the
release, `gaps.create_model` and `gaps.learn_cpts`; from d35680c also
`gaps.declare_attribute`, `gaps.add_factor`, `gaps.marginals` and
`gaps.impute`.  Repeat any `REVOKE EXECUTE` you made on those.  Every
other function keeps its privileges.

Grounding capacity (pg.ddx.io N1).  A model whose ground network passed
1 GB in total failed after minutes with "tuple is too big", because
`gaps._ground` returned its nine arrays as one record; it now returns a
named composite that is never formed into one tuple, so `learn_cpts`,
`marginals`, `impute` and EM ground pg.ddx.io's 5.25M-row,
7-attribute model (1.1 GB of arrays).  Parameterised factors' tables
travel the same way.  The limit that remains, 2^26 variables (auxiliary
variables included) or parent links per call, 134M table cells, and
1 GB per array, is checked before any table is built, the parameterised
factors' tables sized but not built, an `ERROR` naming the model with a
hint to split the model.  4 noisy-OR factors over 1M rows failed
after 182 s with an unnamed "array size exceeds the maximum allowed" and
now get that `ERROR` after 167 s, before any table is built; over 980k
rows (1.07 GB of cells) they ground (doc/DESIGN.md, "Grounding
capacity").

EM's stopping rule changed meaning.  `learn_cpts(..., 'em', tol)` used to
stop when an iteration changed the objective by at most tol times
|objective|, which at pg.ddx.io's scale (|loglik| about 5e4) stopped
after a few iterations with CPT cells 2e-3 to 4e-3 from EM's fixed point
and `converged` true.  tol is now an absolute tolerance on probabilities:
EM stops when every CPT cell is estimated to be within tol of the fixed
point (per factor, from the rate of its last steps; doc/DESIGN.md "When
EM stops").  The default tol stays 1e-6 and now means that; `max_iterations`
defaults to 500 (was 50), since the stricter rule runs 70 to 80
iterations on that shape and a model with little labelled data can need
hundreds.  A new output column, `last_change`, is the objective's change
over the last M-step in nats.

`gaps.set_frozen(model, factor, frozen)` and `gaps.factor.frozen`:
`learn_cpts` keeps a frozen factor's CPT and reports it as
'fixed (frozen)'; EM conditions on it, and a frozen CPT that rules out
observed values is an error naming it.  `gaps.drop_factor(model,
factor)` removes a factor and leaves its attribute and stripes, so a
replacement can be added after it; with `cascade => true` it also
removes the attribute (or, for a split attribute, the factor's stripe)
when nothing else uses it.  `gaps.model.parent` and `gaps.model.note` record a model's
lineage, set by `create_model(name, parent, note)` or
`gaps.set_lineage`.  `gaps.factor_parent.parent_stripe` no longer
cascades on delete (constraint `factor_parent_parent_stripe_fkey`,
deferred), so a stripe that some factor still uses as a parent cannot be
removed under it.

## 0.1.0 (2026-11-22)

The first release.  pg_gaps declares first-order Bayesian networks over
ordinary tables with missing values (BayesStore, VLDB 2008) and answers
with exact posteriors.  Supports PostgreSQL 17, 18 and 19.

Models.  The catalog lives in ordinary tables in schema `gaps`, dumped
with pg_dump and protected by row-level security per owner role.
`create_model`, `declare_attribute`, `add_stripe`, `add_stripe_qual`,
`add_factor`, `map_factor`, `set_cpt` and `drop_model` declare a model;
triggers validate every row (CPT shapes and sums, domains, operators).
A model may sit over tables, partitioned tables, materialized views,
views or foreign tables.  `declare_attribute(..., edges => ...)` bins a
numeric column on read.  `gaps.validate(model)` proves a model acyclic
from its declaration alone (the dependency-graph check of the thesis,
sound, not complete), and every grounding checks coverage, mappings and
cycles exactly.

Inference.  `gaps.marginals(model)` returns the posterior distribution
of every missing value and `gaps.impute(model)` the most likely joint
completion, both exact: the network splits into independent components,
each solved by variable elimination over a bucket tree in log space,
with identical components solved once.  Inference is bounded by
`gaps.max_factor_cells` and `gaps.max_total_cells` and cancellable.
Given `rel` and `entities`, both ground only the components holding
those entities, with the same answers as the whole-model call.  A
planner support function estimates their rows.

Learning.  `gaps.learn_cpts(model, alpha)` sets each CPT to its posterior
mean from the complete rows, under a symmetric Dirichlet(alpha) or the
per-cell pseudocounts of `gaps.set_prior`; `method => 'em'` also learns
from rows with missing values, by EM with the engine's exact E-step.
`gaps.cpt_rows(model, factor)` shows a CPT as rows.

Parameterised and composite models.  `add_factor(..., kind =>
'logistic' | 'noisy_or', params => ...)` computes a factor's CPT from
weights for each grounded instance, and an aggregating parent slot takes
any number of parent rows with shared weights, still exactly.
`gaps.add_member` builds a composite model whose members ground and
solve as one network.  Learning keeps parameterised factors fixed;
fitting their weights is not yet available.

Sampling (experimental).  With `gaps.samples` set, a component too
large for exact inference is sampled by Gibbs, four chains from
dispersed starts, refused when they disagree, seeded by `gaps.seed`.
Chains that all settle in the same wrong mode give a wrong answer
without an error (doc/DESIGN.md, "Gibbs sampling, as built").

Tested against brute-force enumeration and pgmpy (marginals and EM),
under AddressSanitizer and UndefinedBehaviorSanitizer, and through
pg_dump and restore; built with clang (Nix) and gcc -Werror (PGDG on
Debian and Fedora).

Not yet: probabilistic selection, projection and joins (`select_rows`),
the CRF half of BayesStore, fitting logistic and noisy-OR weights, EM for
components too large for exact inference, and running on a hot standby.
