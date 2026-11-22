# Changelog

## Unreleased

Composite models: `gaps.add_member(model, member)` makes a model hold
others, and its factors may name a member's stripes as `member.stripe`.
One call grounds and solves the union as one network, so a person-level
combiner over three networks conditions everything on everything instead
of multiplying the members' marginals in SQL.  A member in use cannot be
dropped; membership cycles and members the caller cannot see are
refused; composites survive pg_dump and restore.

Parameterised factors: `gaps.add_factor(..., kind => 'logistic' |
'noisy_or', params => ...)` declares a factor whose table is computed
from weights at grounding instead of stored, over each instance's missing
parents only, so a logistic over 15 observed five-valued inputs costs one
row per instance rather than 5^15.  `learn_cpts` keeps them fixed (method
'fixed (<kind>)'), EM learns the other factors around them, and
`cpt_rows` shows their computed table.  A parent of such a factor may
aggregate (`aggregate => ARRAY[true, ...]`): a child then maps to any
number of parent entities there, which share the slot's parameters, and
grounding expands them into an OR chain or count chains of internal
auxiliary variables, so inference stays exact.

Binned attributes: `gaps.declare_attribute(..., edges => float8[])`
declares a numeric column (smallint, integer, bigint, numeric, real or
double precision) that pg_gaps discretises on read.  n strictly
increasing finite edges make n + 1 bins, (-inf, e1), [e1, e2), ...,
[en, inf), labelled in order by the `domain`, which must have n + 1
entries; a value on an edge goes to the bin above, and NaN counts as
missing.  Learning, EM, restricted calls, `cpt_rows` and the results all
use the labels.  `gaps.attribute.edges` holds the edges and survives
pg_dump and restore.

A model may be declared over a view or a foreign table, not only a table
or a materialized view: grounding no longer reads `ctid`, which failed
on the first `marginals` call over a view.  `declare_attribute` refuses
a relation that is none of these (a sequence, an index, a composite
type), and a view needs its `entity_keys` given.  Rows of a view whose
key is not unique are one entity, and when they disagree the error says
the key is not unique.  A view over volatile functions must return the
same rows to every statement of a grounding; DESIGN.md "Snapshots" says
when to use REPEATABLE READ or a materialized view instead.

`gaps.marginals` and `gaps.impute` take optional `rel` and `entities`
arguments: given a jsonb array of entity keys of `rel` (the form the
`entity` column returns), they ground only the components holding those
entities and return only their variables, with the probabilities of the
whole-model call.  Cost follows the entities requested when their
components are small and the mapping columns are indexed (a nullable
entity key wants an index too): 230 of 318,000 emails in 110 ms against
33 s.  A long chain of vertical factors costs one closure round per step,
so past a size measured against the model the call grounds the whole
model instead, with the same answers.  A restricted call checks the
model's validity only for the components it grounds.  Malformed keys, a
relation not in the model, or one argument without the other are errors;
a key matching no row returns nothing.

Dirichlet priors per CPT cell: `gaps.factor.prior` (NULL by default) and
`gaps.set_prior(model, factor, prior)`.  `learn_cpts` sets each parent row
to (count + prior) / (row total + sum(prior row)), so a Beta(8.70, 191.30)
prior with 9 events in 200 gives 0.04425; a factor without a prior still
uses `alpha` in every cell.  Priors are validated like CPTs and survive
pg_dump and restore (`test/dump` compares them).  `gaps.cpt_rows(model,
factor)` returns a factor's cells as rows with their parent values, child
value, probability and prior, in array order.  DESIGN.md states that CPT
entries follow `gaps.attribute.domain` in declaration order.

EM for CPTs with missing values: `gaps.learn_cpts(model, alpha, method
=> 'em', max_iterations => 50, tol => 1e-6)` learns from entities whose
child or parents are missing, starting from the complete-data estimate
and using the factor's prior as in the default method; it is EM for the
posterior mode under Dirichlet(pseudocount + 1), stopped when an
iteration changes the log posterior by at most `tol` times its size.
When the complete-data estimate gives probability 0 to an observed value
(alpha 0), EM starts from the add-one estimate instead.  `learn_cpts`
gains the columns `method`, `iterations` (0 for 'counts'), `loglik` and
`converged` (NULL for 'counts').  Exact inference only.
`gaps._expected_counts` is its E-step.

`learn_cpts` no longer takes time quadratic in the number of entities:
its counting query self-joined an unanalyzed temp table, which the
planner ran as a nested loop.  40,800 rows took 7.8 minutes and take
0.84 s (`bench/RESULTS_LEARN_2026-11-21.md`).  `learn_cpts` no longer
rounds a cell with a tiny positive pseudocount to 0.

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

PGXN packaging: `META.json` (validated with `pgxn_meta`), `make dist`
producing the PGXN source zip from `git archive`, `make check-meta`
(run by the flake and CI) holding `META.json` to the control file's
version, and a tag-triggered release workflow that tests the zip and
uploads it to PGXN.

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
