# Changelog

## Unreleased

EM's stopping rule changed meaning.  `learn_cpts(..., 'em', tol)` used to
stop when an iteration changed the objective by at most tol times
|objective|, which at pg.ddx.io's scale (|loglik| about 5e4) stopped
after a few iterations with CPT cells 2e-3 to 4e-3 from EM's fixed point
and `converged` true.  tol is now an absolute tolerance on probabilities:
EM stops when every CPT cell is estimated to be within tol of the fixed
point.  The default tol stays 1e-6 and now means that; `max_iterations`
defaults to 500 (was 50), since the stricter rule runs 70 to 80
iterations on that shape and a model with little labelled data can need
hundreds.  A new output column, `last_change`, is the objective's change
over the last M-step in nats.

`gaps.set_frozen(model, factor, frozen)` and `gaps.factor.frozen`:
`learn_cpts` keeps a frozen factor's CPT and reports it as
'fixed (frozen)'; EM conditions on it.  `gaps.drop_factor(model,
factor)` removes a factor, and its attribute when no factor covers it
any more.  `gaps.model.parent` and `gaps.model.note` record a model's
lineage, set by `create_model(name, parent, note)` or
`gaps.set_lineage`.  `gaps.factor_parent.parent_stripe` no longer
cascades on delete, so a stripe that some factor still uses as a parent
cannot be removed under it.

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
