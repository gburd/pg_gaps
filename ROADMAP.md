# pg_gaps roadmap

The single plan file.  Each phase has a goal, what it delivers, and what
must be true before it counts as done.  Status lives here and nowhere else:
when an item closes, collapse it to one line and put the detail in
CHANGELOG.md.  Do not start a second plan file.

doc/DESIGN.md holds the design these phases build; section references
like [W11 4.2] point at the BayesStore thesis it cites.

---

## Decisions needed before Phase 1

Each has a recommendation.  None blocks Phase 0, which is done.

| # | question | recommendation | why |
|---|---|---|---|
| Q1 | Build the CRF / information-extraction half of BayesStore (thesis ch. 5-7), or only the first-order Bayesian network half (the VLDB'08 paper)? | **Both, FOBN first.** CRF is Phase 7. | The two share the data model and catalog, and the CRF half has the query-driven inference that made BayesStore fast. Doing FOBN first gets a useful extension (imputation with correlations) before the larger CRF work. |
| Q2 | Declare models through SQL functions, or a small text DSL parsed into the catalog? | **Functions** (`gaps.add_factor(...)`). | No parser to write, fuzz and maintain; arguments are typed and validated by the server; pg_dump of the catalog tables is the export format. A DSL can come later as sugar over the same functions. |
| Q3 | How to store a probabilistic attribute's domain? | **`text[]` of the type's output form, plus the type as `regtype`**, converted with the type's input function once per query. | A user table cannot have an `anyarray` column. Requiring an enum type is cleaner but forces a schema change on the user's table, which defeats "point it at the table you have". Type OIDs do not survive dump and restore; a type name does. |
| Q4 | Supported PostgreSQL majors? | **17, 18, 19** (as pg_fts). | 17 is the oldest still widely deployed with the APIs we need. `dev/regress.sh` needs 18 (`extension_control_path`); the flake gates 17. |
| Q5 | Use pgmpy (Python) as an independent oracle in tests? | **Yes, as its own flake check**, never in `make installcheck`. | An oracle we wrote ourselves shares our misreadings of the math. pgmpy 1.1.2 is in nixpkgs. Keeping it out of PGXS keeps distro builds Python-free. |
| Q6 | Where is the canonical repository? | **Ask.** pg_fts uses Codeberg with a GitHub mirror and runs CI only on GitHub (Codeberg has no shared runners). | Needed before the first push; nothing is pushed yet. |
| Q7 | Name, schema, license. | `pg_gaps`, schema `gaps`, PostgreSQL License. | Matches pg_fts. Schema is fixed (`relocatable = false`) because the catalog tables reference each other. |
| Q8 | One call per whole query, or a derived-model handle so calls chain (`select_rows(...)` feeding a later `marginals(...)`)? | **One call per query for now.** | BayesStore operators return a relation _and_ a model, and the next operator needs both (DESIGN.md, Principle 1). Composing marginals in SQL silently assumes independence. A handle needs a lifetime, an owner and invalidation rules; design it when a user needs chaining, not before. |

---

## Phase 0: foundations (done 2026-11-18)

- PGXS `Makefile` with the control file as the single source of the
  version, VPATH (out-of-tree) builds, and `make check-ascii`.
- Module stub `src/pg_gaps.c` (magic block; `PG_MODULE_MAGIC_EXT` on 18+).
- Empty install script, one regression test that installs the extension
  and loads the library.  Verified it fails when the magic block is removed.
- Nix flake: packages and `installcheck-pg{17,18,19}` checks, built with
  nixpkgs' `postgresqlBuildExtension`; dev shell; `ascii` check.
  `nix flake check` green on 17.12, 18.7 and 19.0 (19.0 built from the
  REL_19_0 tag, since nixpkgs still packages 19rc2), zero compiler
  warnings.  PGDG 17.12, 18.7 and 19.0 on Fedora 44 with gcc 16 and
  `-Werror` pass `dev/qualify-pgdg.sh`.
- `dev/regress.sh`: out-of-tree build plus a throwaway cluster, under
  `/scratch`, nothing written to the source tree.
- GitHub Actions: pgdg gcc build with `-Werror` + regress on 17/18/19,
  and the flake check.  Written and linted (actionlint); **never run**,
  because the repository has no remote yet.
- `AGENTS.md`, this file, `doc/DESIGN.md`, `CONTRIBUTING.md`, `README.md`.

## Status, 2026-11-19

Phases 1 and 2 are built, with three items each still open (below).
Engine and SQL interface gated on PG 17.12, 18.7 and 19.0 (nixpkgs with
clang; PGDG on Fedora 44 with gcc 16 and `-Werror`).  Exact inference
already handles vertical correlations (any component the size bounds
allow), which was planned for Phase 3; Phase 3 is left with sampling for
components too large to solve exactly.

| phase | item | status |
|---|---|---|
| 1 | catalog, triggers, row-level security, declaration functions | done (`sql/catalog.sql`) |
| 1 | data-dependent validity: coverage, mappings total, evidence consistent, cycles | done (`sql/infer.sql`) |
| 1 | the dependency-graph check before grounding [W11 3.3.2] | **open**: cycles are found on the ground network instead (gx_check), which is exact but needs the data |
| 1 | `pg_dump` / restore test | done (`test/dump`; in the flake, CI and `dev/qualify-pgdg.sh`; fails if one config table is not registered) |
| 1 | dropped or altered table names the change | done: rename, type change and drop each refused with the name (`test/dump`) |
| 2 | engine: exact inference, bucket tree, log space, bounds, sharing | done; brute-force oracle, closed forms, allocation failure (`test/engine`); two independent reviews |
| 2 | `marginals`, `impute`, `learn_cpts` | done (`sql/infer.sql`, `sql/learn.sql`) |
| 2 | pgmpy cross-check | done: 2,647 to 2,762 marginals per run agree to 1e-9 (`test/pgmpy`, flake check `pgmpy`) |
| 2 | sharing measured with A/A | done: 1.6-2.2x on inference (`bench/RESULTS_SHARING_2026-11-19.md`) |
| 2 | `SupportRequestRows` for the SRFs | **open** |
| 2 | grounding cost | **open**: about 100x the inference cost (23 s for 2M variables); jsonb entity keys are next (Phase 6) |
| 3 | Gibbs sampling for components too large for exact | done (branch wip/gibbs): `gaps.samples` (per chain), `gaps.seed`; 4 chains from dispersed starts, refused (`ERROR`) when two chains' estimates differ by more than 0.1.  On models of at most 12 variables (20,000 random strictly positive cases) and the 6 x 6 grid, Rao-Blackwellized marginals are within 0.1 of exact at 4,000 samples (largest error 0.025) with no refusals; the review's multimodal grid and hub models are refused.  The check cannot see chains all stuck in one mode, so a sampled answer can still be wrong without an error (DESIGN.md "Gibbs sampling, as built").  Refusal on zeros, cancellable (`test/engine`, `sql/infer.sql`) |
| 3 | full hotel workload with its vertical factors | done: runs exact, components of at most 4 variables (`bench/hotel.sql`, `bench/RESULTS_HOTEL_2026-11-20.md`) |

## Phase 1: model catalog and validation

**Goal:** a user can declare a first-order Bayesian network over an
existing table, and the extension refuses any model that does not define a
valid distribution.

Delivers:
- Catalog tables in schema `gaps` (DESIGN.md "Model catalog"), each
  registered with `pg_extension_config_dump`, and each serial sequence
  registered too, so pg_dump keeps user models.  User objects are named by
  `regclass` plus their name, columns by name, never by attribute number
  or type OID, and every use re-checks the names.
- `create_model`, `drop_model`, `declare_attribute`, `add_stripe`,
  `add_stripe_qual`, `add_factor`, `map_factor`, `validate`.  Every
  function runs as its caller (DESIGN.md "Security"); triggers validate
  every catalog row, and row-level security limits each role to the models
  its role owns, so direct DML on `gaps.*` meets the same checks.
- Validation at the trust boundary: domain non-empty and duplicate-free;
  CPT length equals the product of the domain sizes; every conditional
  distribution non-negative and summing to 1 within 1e-9; mapping columns
  exist and are deterministic, and the equalities plus the parent side's
  constants determine the parent's key `Kp`; entity-set terms type-check
  against the relation, touch only deterministic columns, and use only
  immutable operators.
- Data-dependent validity (DESIGN.md "Validity"): within each factor the
  child stripe shares no RV with a parent; every mapping is total on the
  child entities; every value of a declared attribute, present or
  missing, is covered by exactly one child stripe; the dependency-graph
  check [W11 3.3.2] with the lineage trace on a cycle.  Run at `validate`
  and again at every grounding.
- Privilege checks on every referenced column, as the caller.

Done when: regression tests cover each rejection with a `-- fail` case and
each acceptance with a model that grounds; the model of [W11 fig. 3.5]
(T2465-2478) is accepted, and a hand-built model whose grounding has a
directed cycle is rejected (the thesis gives no verdict for fig. 3.5 and
fig. 3.4 is an image, so the cyclic case is ours, checked by grounding
it); the catalog survives `pg_dump` / restore into a fresh database (TAP
test); a model over a table that was dropped or had a column dropped
fails with an error naming it.

## Phase 2: exact inference for independent rows, and CPT learning

**Goal:** the first useful feature.  For models whose grounded
components are single rows (only horizontal factors, maximal entity sets,
no one-to-many mappings), return the posterior of every missing value
given the evidence in its own row.

Delivers:
- The standalone C inference core (no PostgreSQL headers): discrete
  factors, product, marginalise, reduce by evidence, variable elimination.
- Grounding, and a check that refuses (for now) any model whose
  components are larger than one row; Phase 3 lifts that.
- First-order sharing: cache posteriors keyed by the canonical form of
  the grounded component for the duration of the query [W11 4.5.6].
- `gaps.marginals(model, rel)` -> `(key, attribute, value, probability)`;
  `gaps.impute(model, rel)` -> MAP completion per row.
- `gaps.learn_cpts(model [, alpha])`: maximum-likelihood CPTs from rows
  complete for each factor's variables, with additive (Laplace) smoothing;
  the data is read as the caller and the CPTs written by a definer writer.
  The BayesStore system implemented inference only; its thesis lists
  in-database learning as future work [W11 ch. 8], and a workshop paper
  discussed learning for hierarchical FOBNs (T2496-2499).  Without it the
  extension is unusable on real data.
- `SupportRequestRows` support functions for the SRFs.

Done when: a brute-force possible-worlds oracle agrees with every
marginal to 1e-12 on generated small models; the pgmpy check (Q5) agrees
on the same models to 1e-9; probabilities in every output group sum to 1
within 1e-12; a row with no missing values produces no output rows;
first-order sharing is measured on a horizontal-only sensor workload (the
hotel workload of [W11 4.5.1] minus its two vertical factors, which need
Phase 3) and the speedup is reported with an A/A run beside it, not
projected.

## Phase 3: vertical correlations

**Goal:** correct posteriors when rows are correlated with each other
(sensor 2's reading depends on sensor 1's at the same time).

Delivers:
- Tuple correlation graph and its connected components; each component is
  grounded and solved on its own.
- Exact inference (variable elimination with a min-fill order) when the
  component's induced width is under a bound; above it, Gibbs sampling
  with `gaps.seed` and `gaps.samples`, deterministic given both, only for
  components whose CPTs are all strictly positive (DESIGN.md "Inference":
  Gibbs cannot cross zero-probability regions).  A large component with
  zeros is an `ERROR` naming the reason.  Four chains run; when they disagree by more than 0.1 on any
  probability, the answer is an `ERROR`, not a guess.
- `gaps.max_component_vars`: declined.  `gaps.max_factor_cells` and
  `gaps.max_total_cells` already refuse an exponential factor before it
  is built, and they measure what costs memory, which a variable count
  does not.  Sampling holds memory linear in the component (charged to
  `gaps.max_total_cells`) and takes time proportional to 4.4 x
  `gaps.samples` times the component's factor scopes, which the caller chooses and can
  cancel.
- Output columns say which method answered and, for sampling, how many
  samples it used: `method` ('exact', 'gibbs', or for `impute`
  'gibbs-marginal-argmax') and `samples` (NULL for exact).

Open: start two of the four chains from forward samples of the prior
(evidence respected), which in a prototype caught the ternary-hub case on
4 of 5 seeds and cut the alternating hub from 6 to 4 of 40 accepted
wrong answers (DESIGN.md "Gibbs sampling, as built").  It narrows, it
does not close, the same-mode gap.

Done when: exact and oracle agree on correlated models, including
coarse entity sets and one-to-many mappings; Gibbs is within a stated
tolerance of exact on every model both can solve, at the documented
sample count; the full hotel workload of [W11 4.5.1] runs; a model that
exceeds the bound errors instead of hanging; the query is cancellable
during sampling (`CHECK_FOR_INTERRUPTS`).

## Phase 4: probabilistic selection

**Goal:** `SELECT ...  WHERE` over missing values, with each result row's
probability of existing [W11 4.2].

Delivers:
- Selection over the model: `Exist` stripe and factors.  `Exist` is
  deterministic and is computed from each world, never sampled.
- Selection over the data: PlainSel (closure over the correlation graph),
  FactorSel (zero-probability filtering, only for rows with no vertical
  correlations), EvidenceSel (Bayes Ball early stopping), and FullSel
  (both).
- `gaps.select_rows(model, rel, predicate)` -> rows plus `P(exist)`;
  predicates are conjunctions of `attribute op constant`, stored and
  checked as in Phase 1.  A user-declared `Exist` attribute on a base
  table is honoured.

Done when: for every row that PlainSel and a variant both return, the
probabilities agree with each other and with the oracle to 1e-12; every
row a variant drops has `P(exist) = 0` under the oracle or is independent
of every returned row given the evidence; and on the synthetic hotel
workload, output size and run time are measured against table size,
missing ratio and connectivity, the four variants ordered as in
[W11 fig. 4.8-4.11], each with an A/A noise floor.

## Phase 4b: predicate language

Disjunction and negation in selection predicates [BW08 3.1] (the paper
mentions both and gives no algorithm), and joint-distribution output for
a selection (the paper's `INFER joint-distr`, [BW08 4.5]) bounded by
`gaps.max_component_vars`.  Done when both agree with the oracle.

## Phase 5: projection and join

Delivers projection that keeps correlated attributes (closure to a
fixpoint) and drops dead factors, with the primary key required in the
projection list [W11 4.3], and joins on probabilistic attributes as
selection over the cross product with an `Exist` factor [W11 4.4], with
the join's evidence closure.  Top-k completions for k > 1 over FOBN
models land here, with the join they are most useful for.  Done when both agree with the oracle on small models and
with a manual SQL formulation of the same possible-worlds computation.

## Phase 6: planner integration and grounding cost

Grounding dominates: about 100 times the inference cost at 2M variables
(`bench/RESULTS_SHARING_2026-11-19.md`), from building and joining jsonb
entity keys in every step.  First, entity keys as typed composite or
integer surrogates, measured against the current form on the hotel
workload at two scales with A/A runs.  Then costs for every SRF from model
statistics (component sizes, distinct evidence signatures), so plans are
reasonable without hints.  A custom scan only if a measurement shows the
SRF form costs something real; otherwise this phase closes with that
measurement.

## Phase 7: CRF instance for information extraction

**Goal:** BayesStore's second instance [W11 3.4, 5, 6, 7].

Delivers: token table and factor-table (`MR`) conventions; entity tables
as a pivot of the most-likely labels [W11 6.2]; Viterbi top-1 and
incremental top-k; forward-backward marginals; constrained Viterbi and
SelViterbi for selections; the join-key hash filter for joins over the
most-likely world and rank-join for probabilistic joins; the projection
hybrid [W11 6]; Gibbs and query-driven MCMC-MH for skip-chain models and
aggregate constraints, and the per-document hybrid plans and
algorithm-choice rules of [W11 7.3-7.6].  Model weights are loaded, not
trained (CRF training is out of scope, as it was for BayesStore).

Done when: Viterbi equals brute force on short sequences; forward-backward
marginals sum to 1 and equal brute force; top-k comes out in
non-increasing probability with no duplicates; the query-driven variants
return the same answers as the naive plan.

## Phase 8: release engineering

Before 1.0: `make dist` from `git archive` with `.gitattributes`
export-ignore, `META.json` for PGXN, a tag-triggered release workflow,
SGML docs, `SECURITY.md`, the distro packaging the workspace release
checklist requires, sanitizer and fuzz jobs for the C core, and a
coverage gate.  Decide `trusted` after a security review (default stays
untrusted).

---

## Declined

Nothing yet.  A declined idea goes here with the measurement or argument
that declined it.

## Closed

Phase 0 (2026-11-18).
