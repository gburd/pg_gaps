# pg_gaps design

This document says what BayesStore is, which parts of it pg_gaps builds,
and how each part maps onto a PostgreSQL extension.  ROADMAP.md says what
order they are built in and what is open.  Where the two disagree about
_status_, ROADMAP.md wins; where they disagree about _design_, this file
does, and the disagreement is a bug to fix.

## Sources

- [BW08] D. Z. Wang, E. Michelakis, M. Garofalakis, J. M. Hellerstein.
  "BayesStore: Managing Large, Uncertain Data Repositories with
  Probabilistic Graphical Models."  PVLDB 1(1), 2008.
  <https://www.vldb.org/pvldb/vol1/1453896.pdf> (the copy in the repo root,
  `1453896.pdf`, is that paper; it is gitignored and not redistributed).
- [W11] D. Z. Wang.  "Extracting and Querying Probabilistic Information in
  BayesStore."  PhD thesis, UC Berkeley, UCB/EECS-2011-131.
  <https://www2.eecs.berkeley.edu/Pubs/TechRpts/2011/EECS-2011-131.pdf>.
  The paper is chapters 3-4 of this; the thesis adds the CRF instance,
  in-database inference algorithms, query-driven inference, and the rules
  for choosing an algorithm (chapters 5-7).  Citations below are to the
  thesis unless marked [BW08].
- [W10] D. Z. Wang, M. J. Franklin, M. Garofalakis, J. M. Hellerstein.
  "Querying Probabilistic Information Extraction."  PVLDB 3(1), 2010.
  The source of the query-driven Viterbi algorithms in thesis chapter 6.
- [P03] D. Poole.  "First-order Probabilistic Inference."  IJCAI 2003.  The
  lifted-inference idea BayesStore's "first-order sharing" borrows.

Citations of the form `T2285-2287` are line numbers in the thesis text as
extracted by `pdftotext -layout EECS-2011-131.pdf` (poppler-utils 26.10,
6958 lines).  Regenerate the text that way before following one.

BayesStore itself was a modified PostgreSQL (8.2.4 for the paper's
prototype, 8.4 by the thesis), changing the data model, executor, and
rewriter; it was never an extension.  Apache MADlib later shipped
linear-chain CRF training and Viterbi inference in SQL, and was retired to
the Apache Attic in September 2026.  I found no maintained PostgreSQL
extension that provides correlated, model-based uncertainty over ordinary
tables, but that rests on one pg.ddx.io archive search (2026-11-18), not
a survey.  If it holds, that gap is this project's reason to exist.

## The idea in one paragraph

Most probabilistic databases attach a probability to each tuple or value
and treat them as independent.  BayesStore instead keeps the data and the
uncertainty model apart: a table may have _missing_ values in some
columns, and a separate probabilistic graphical model (PGM) says how those
values are distributed and how they correlate, with each other and with
the values that are present.  The model is first-order: one conditional
probability table (CPT) covers a whole population of rows ("every
sensor's light reading depends on its temperature reading the same way"),
so the model stays small however large the table grows.  Queries then run
over both halves at once, and the answer carries probabilities computed by
inference over the model, conditioned on the data actually present.

## Data model [W11 3.2, BW08 2.1]

A probabilistic database is `DB = <R, F>`:

- **Incomplete relations `R`.** An ordinary table whose columns split into
  _deterministic_ attributes `Ad` (never uncertain, though they may be SQL
  `NULL`; they include every key) and _probabilistic_ attributes `Ap`.  A
  present value of a probabilistic attribute is _evidence_.  A missing one
  is a random variable (RV) over the attribute's domain.
- **A distribution `F`** over all the missing-value RVs, encoded by a PGM.
  Each complete assignment of the RVs is one possible world.

Two kinds of correlation matter at query time: _horizontal_ (between
attributes of the same row) and _vertical_ (between different rows).  What
makes inference cheap is independence between _grounded components_, not
between rows: a coarse entity set makes several rows share one RV, and a
one-to-many mapping across relations ties a parent row to many child rows,
so rows can be correlated without any factor that looks vertical.
Independence, and everything built on it (per-component inference,
first-order sharing), is defined on the grounded component.

### First-order Bayesian networks [W11 3.3.2, BW08 2.3]

- **Entity set** `R.E = pi_K'(sigma_cond(K')(R))`: a selection and
  projection over deterministic attributes.  Its members are _entities_,
  the granularity of the underlying RVs.  Projecting on a key gives one
  entity per row (the maximal entity set); a coarser projection makes
  several rows share one RV.
- **Stripe** `S<R.Ap, E>`: one RV per entity in `E`, for probabilistic
  attribute `Ap`.  A stripe is a first-order RV.
- **Stripe mapping** `f[Sp, Sc]: Ec -> Ep`: a function from child entities
  to parent entities, defined by a formula over the two entity sets'
  attributes with `Kp` a subset of `Kc` (a key/foreign-key path), and
  required to be surjective, so every parent RV has a child.  Each child
  RV gets exactly one parent RV per parent stripe.  [W11 3.3.2] also
  requires the two entity sets' selection predicates to be the same
  (T2285-2287), but its own example violates that: in Fig. 3.5, `phi3`
  maps sensor 2's temperature (`Sid = 2`) to sensor 1's (`Sid = 1`) at
  the same room and time (T2474-2478), and the evaluation workload's
  `phi(Tp2|Tp1)` and `phi(Tp4|Tp3)` (T3268-3269) need the same shape.
  pg_gaps therefore allows a mapping of equalities on shared attributes
  plus a constant per side, and requires the equalities together with the
  parent side's constants to determine the parent's key `Kp` (for `phi3`:
  `T` and `R` by equality, `Sid = 1` by constant).  The property
  correctness needs is that the mapping is a total function on the child
  entities: every child RV has exactly one parent RV.  The thesis also
  requires surjectivity, but as minimality, so that every parent RV has a
  child (T2295-2297); a parent with no child in one factor is harmless,
  and pg_gaps does not require it.
- **First-order factor** `phi(Sc | Sp1..Spn, f1..fn, CPT)`: one CPT shared
  by every child RV of the stripe.
- **FOBN model** = a set of first-order factors.

**Validity.** Stripes in different factors may overlap, so a set of
factors that each look fine can ground to a cyclic Bayesian network.
[W11 3.3.2] gives a check on the first-order structure, without
grounding.  Overlap here means shared _RVs_: the same attribute over
shared entities.  Two stripes on different attributes of the same rows
(`Sensor1.L` and `Sensor1.Tp` in `phi2` of Fig. 3.5, T2429-2432) share
entities but no RVs.  The thesis writes "entity sets" and "share
entities"; read literally that would reject every horizontal factor,
including its own `phi2`.  Its precondition is that within each factor
the child stripe shares no RV with any of its parents (T2423-2424).
Build the dependency graph: every stripe is a node, directed edges run
from parent to child within a factor, and undirected edges join a parent
stripe and a child stripe of _different_ factors when they share RVs
(T2417-2421).  An acyclic graph proves the model valid.  A cycle of
alternating edges makes it invalid only if tracing an overlapping RV's
lineage around the cycle returns to that RV.

`gaps.validate(model)` runs this check from the catalog alone, for any
table contents, and returns one row per test, `(test, ok, detail)`:
`within-factor` (the precondition) and `dependency graph` (no cycle
through two or more factors).  Both `ok` proves the ground network
acyclic.  A row that is not `ok` proves nothing, and validate does not
trace lineage: the cycle may not exist in the ground network (a chain
through time, where stripe `t > 1` has parent stripe `t < 2`, fails the
precondition and is fine), so `detail` names the factors and stripes of
the cycle it found and grounding decides.  It works on a graph with a
node per factor and an edge `f -> g` when `f`'s child stripe may share an
RV with a parent stripe of `g`; that is the thesis's graph with each
child stripe and its factor merged, and a self-loop is the precondition
failing.  Every ground edge runs from an RV that is the child of one
factor (coverage) and a parent in the next, so every ground cycle follows
a cycle of this graph.

Whether two stripes may share an RV is decided from their quals.  They
cannot when they are on different attributes, or when two quals (from
either stripe) contradict on a column fixed per entity: an entity key, or
any column when the entity keys include the primary key, so each entity
is one row.  The primary key counts only when it is not deferrable and
the relation has no inheritance children or partitions: grounding reads
`FROM rel`, children included, and neither a parent's key nor a deferred
one keeps two rows of one entity apart.  A qual on another column selects
rows, and rows of one coarse entity can fall on both sides.  Two quals
contradict when one is `= c` and `c` fails the other, or a lower bound
(`>`, `>=`) lies above an upper bound (`<`, `<=`), or they meet at one
constant and either is strict, with constants compared as grounding
compares them (cast to the column's type, in its collation).  These rules
need `=`, `<` and `>` to be a total order consistent with equality, so a
column counts only when its type's `<`, `<=`, `=`, `>=` and `>` are the
strategies of one btree operator family and `<>` negates that `=` (box
has all six operators, but its `=` compares areas within a tolerance and
is not transitive).  Under those conditions the test is sound; anything
else counts as may share.  It is not complete: `<>` contradicts only
`=`, it does not see the gaps of a discrete type (`t > 1` and `t < 2`
over `int`), and a type with no comparison operators of its own
(`varchar`, a domain) is not reasoned about.

The constants are text, cast to the column type each time a model is
used, so for types whose input depends on the session (`date`,
`timestamp`, `timestamptz`, `time`, `timetz` and `interval` on TimeZone
and DateStyle, `interval` also on IntervalStyle, `money` on
lc_monetary, the `reg*` types on search_path) the model itself means
different things in different sessions.  validate does not reason about
a column whose type's input function is not immutable, which excludes
all of these, so its verdict does not depend on the session; grounding
still casts those constants under the caller's settings.  Storing the
constants in canonical form is an open ROADMAP item.  A proof also
assumes the catalogs as they were when validate ran: DDL afterwards (a
child table added, a key made deferrable) can void it.  validate
resolves the model's tables and columns first, as grounding does, and
fails with the same messages.

Grounding keeps the exact check (gx_check) and does not run validate.
`sql/validate.sql` checks soundness against grounding over 500 random
models: 300 whose entities are rows, and 200 over coarse entities with
quals on a column outside the entity keys, which must never separate
entities.  validate tests each distinct pair of child and parent stripe
once, looking only at the columns the pair's quals name, and refuses,
before testing any, a model with more than 100 factors or 20000 parent
slots, or whose pair tests and qual comparisons would exceed a fixed
budget (200000 units; see the function's comment).  The worst models
measured within those limits take under 7 s on the development host.

Two more conditions are needed for `F` to be a distribution at all, and
the thesis does not state them (my inference): every RV of a declared
probabilistic attribute, present or missing, is covered by exactly one
child stripe, so each has exactly one CPT, and every mapping is total.
A present value needs its CPT too: under `phi(Tp2|Tp1)`, an observed
`Tp2` is evidence about a missing `Tp1`, and without the factor that
evidence would be silently dropped.  Entity sets are queries over the
data, so all of these depend on the data: they are checked when a model
is declared and again whenever it is grounded.

**Tuple existence.** A probabilistic boolean attribute `Exist` models
whether a row is in the relation at all.  A user may declare one on a base
table (T2078-2081); query processing adds one (see Selection).

## Query semantics [W11 4, BW08 3]

Every operator acts on both halves: it transforms the data _and_ the
model, so that the result is again an incomplete relation plus a valid
distribution for it.

### Selection [W11 4.2]

- **Over the model.** A predicate on a deterministic attribute restricts
  the entity set.  A predicate on a probabilistic attribute makes row
  existence uncertain: selection adds an `Exist` stripe whose
  deterministic CPT is `Exist = 1 iff Ap theta c`, and a second factor
  fixing `Exist = 0` for entities outside the predicate's entity set.
  Plain conditioning would be wrong, because it cannot produce worlds
  with different numbers of rows [BW08 3.1.1].
- **Over the data.** A row must be kept if it can satisfy the predicate
  _or_ if it is correlated with a row that can, because dropping its
  evidence would change the posterior of rows that remain.  The base
  algorithm keeps rows that satisfy the predicate or have the attribute
  missing, then takes their transitive closure over the tuple correlation
  graph.  Two optimisations shrink the result without changing the answer:
  - _evidence-based early stopping_: stop the closure at rows that are
    conditionally independent given the evidence (Bayes Ball [W11 4.2.2]);
  - _factor-based filtering_: for a row whose RVs are involved _only_ in
    horizontal correlations, drop it if its missing value has zero
    probability of satisfying the predicate under its own horizontal
    factor's CPT, given the evidence in the same row (T2976-2990).  The
    restriction matters: a row with vertical correlations can have a
    nonzero posterior even when its own CPT row says zero.

  The point of both is that inference cost scales with the selection's
  output, so smaller output is faster answers [W11 4.5].

### Projection [W11 4.3]

Projection without duplicate elimination, on a projection list that
contains the primary key (T2995-2997).  It keeps an attribute outside the
list if a factor correlates it with one inside it, and drops factors whose
attributes are all projected away.  The thesis's Figure 4.5 makes one pass
over the factors, so its answer depends on their order; pg_gaps computes
the closure to a fixpoint.  Duplicate elimination is not first-order and
is out of scope, as it was for BayesStore.

### Join [W11 4.4]

A join on probabilistic attributes is a probabilistic selection over the
cross product: the result gets an `Exist` factor whose parents are the two
join attributes and whose CPT is `Exist = 1 iff R1.A theta R2.B`, stripes
are renamed into the result schema, and the data side keeps the same
evidence closure as selection.

### Inference [W11 2.2, 4.5.6, 5, 7.5]

Queries ask for one of:

- **Marginal**: the distribution of a missing value (or of `Exist`).
- **Top-k / MAP**: the most probable completions.
- **Joint**: the joint distribution of the missing values that satisfy a
  query (the paper's `INFER joint-distr`, [BW08 4.5]), bounded in size.

The two halves of BayesStore chose algorithms differently.  The FOBN
half did no choosing: it grounded the selected model to a flat Bayesian
network and ran variable elimination from Murphy's BNT toolbox, outside
the database (T3537-3541).  The CRF half has rules (Table 7.1 and
[W11 7.5.3], T5683-5691, T5786-5814), which choose on three things: model
shape, correlation strength, and label space size.  Viterbi handles
linear-chain top-k; sum-product handles chain and tree marginals; only
MCMC handles cyclic models, because the thesis does not study junction
trees or loopy belief propagation (T1402-1404).  Gibbs is preferred over
MCMC-MH unless the cycles come from query constraints, and is chosen
outright for a large label space with weak correlations.

pg_gaps policy (not BayesStore's) for FOBN models is:

| grounded component | top-k / MAP | marginal |
|---|---|---|
| small enough to solve exactly (bounded induced width) | variable elimination | variable elimination |
| linear chain (CRF) | Viterbi | forward-backward (sum-product) |
| too large, every sampled variable's CPT strictly positive | Gibbs | Gibbs |
| too large, zeros in a sampled variable's CPT | `ERROR` (see below) | `ERROR` |

Gibbs sampling cannot cross zero-probability regions: it handles
constraints that "do not break the distribution into disconnected
regions" (T5736-5739).  The `Exist` factors that selection and join add
are deterministic, so `Exist` is never sampled; it is evaluated as a
function of each sample of its parents.  A model whose own CPTs contain
zeros can still trap a Gibbs chain.  Until there is an MCMC-MH with moves
that change several variables at once, such a component is solved exactly
or refused.

**First-order sharing** [W11 4.5.6, P03]: components that are identical,
meaning the same factors, the same structure, and the same evidence, have
the same posterior, so it is computed once per distinct component rather
than once per row.  The sharing key is the grounded component, not the
row: two rows with equal evidence share a posterior only if their
components are equal too.  BayesStore called its measurement a best case.
Its text says 5-10x (T3551), and its Table 4.11 gives 5-14x
(T3504-3508).  It also notes that the overhead can outweigh the gain
(T3555-3557), which is why it wanted an optimizer to choose.

**Restricted calls.**  `gaps.marginals(model, rel, entities)` and
`gaps.impute(model, rel, entities)` answer for some entities of one
relation rather than the whole model.  `entities` is a jsonb array of
entity keys in the form the `entity` column returns, so a caller can pass
back what it got; `rel` names their relation.  For each object the call
returns the rows of the attributes of `rel` whose entity keys are the
object's keys (a `{"id": ...}` object gets the attributes keyed by `id`,
not a coarse attribute of the same rows keyed by `grp`), and nothing of
the rest of their components.  The interface is two optional
arguments rather than a predicate, a view or a new function: a key list
is what the pg.ddx.io nightly job has (the ids of new rows), it needs no
predicate language (Phase 4 brings one for selection, which is a
different operation: it conditions on `Exist`, where this only chooses
which posteriors to report), and both NULL is the old call.

An entity's posterior depends on its whole ground component (Principle
1; vertical factors, coarse entities and one-to-many mappings correlate
rows), so the call grounds exactly the components that hold a requested
variable.  `gaps._ground_closure` computes them over the ground graph in
rounds of set-at-a-time SQL: from the requested entities, each round
adds, along every factor's every parent, the parents of the entities the
last round added and their children, through the same mapping and
stripe predicates grounding uses, until a round adds nothing.  Grounding
then runs on that entity set: entities, factor instances, coverage,
mapping totality and evidence checks are the same code restricted to
it, and because the set is closed (every factor instance has all of its
variables in it or none), each grounded component equals the
unrestricted one.  The probabilities are therefore the unrestricted
call's, bit for bit; component numbers differ.  `sql/subset.sql` checks
this on fixed models of each shape and 150 random ones.

The closure ignores evidence, so it can hold more than a component in
the engine's sense (observed variables separate components there).  That
over-grounds, never under-grounds.  Pruning by d-separation (observed
separators, barren children) could shrink it; it is not done, because
the measured workload has one entity per component, where it cannot
help.

What a round costs depends on indexes.  Each round, and each grounding
step of a restricted call, joins the frontier to the user's table on the
entity key columns and the mapping columns.  A key column that holds no
NULL compares with `=`.  One that may hold a NULL must also match two
NULLs: an `OR` that an index on the column answers with a probe per side
and that otherwise leaves only a nested loop over the table, so without
such an index the key is compared a second time as a record, which
hashes.  A column counts as holding no NULL only when it is NOT NULL in
`rel` and in every inheritance child and partition (a scan reads them
too) and no not-null constraint of theirs is NOT VALID: on PostgreSQL 18
a NOT VALID constraint sets `attnotnull` over existing NULLs, and a
parent made NOT NULL with `ALTER TABLE ONLY` or `NO INHERIT` can have
children with NULLs.  Measured on 100,000 rows, 230 entities requested,
against 3.6 s for the full call (`bench/RESULTS_SUBSET_2026-11-21.md`):
50 ms with an index on a nullable key, 360 ms with neither an index nor
NOT NULL, about 50 ms on a NOT NULL primary key.  The mapping columns
are joined with `=`, so an index on them is what keeps a round in
proportion to its frontier; without one a round is a hash join over the
table.

A round costs about one statement per factor edge and direction, whatever
its frontier, so a long chain of vertical factors pays one round per
step: a 2,000-row chain took 1.8 s restricted against 39 ms for the
whole model.  The closure therefore gives up when its statements so far,
less an allowance of 100, times 25 (the measured cost of a statement in
rows of a whole-model grounding) exceed the model's size (the sum of its
attributes' relations' `reltuples`; never without statistics).  The call
then grounds the whole model and reports the requested entities only,
which gives the same probabilities by construction; the 2,000-row chain
then takes 105 ms and a 10,001-row one 320 ms (the full call: 31 and
134 ms).  The fallback is a cost rule, not a proof: a model can still
make a restricted call a few times slower than the full one, never the
45x of a closure that runs to the end.  The closure is bounded at
10,000,000 entities, the requested ones included and each insert limited
to the entities left, and at 10,000 rounds (AGENTS.md rule 16), with an
`ERROR` naming the limit that was hit and a hint to call without
entities; the inference bounds (`gaps.max_total_cells`) still apply to
each component.

A restricted call checks validity (coverage, one parent per child per
parent stripe, evidence agreement, CPTs set) only for the entities it
grounds.  The answer it gives is the right posterior for those
components under the model, but a clean restricted call does not show
that the model is valid: a defect in a component it does not touch (a
child with no parent row, rows that disagree) is reported only by a
call that grounds it, such as the unrestricted one or a restricted call
that gave up its closure.  Running the checks over the whole model would
cost the full grounding this exists to avoid; `gaps.validate` is the
place for a model-wide check.

At the nightly shape (318,000 emails, 230 requested) a restricted call
takes 110 ms against 33 s.  `learn_cpts` is not restricted: it learns
from all the data.

### The CRF instance [W11 3.4, 5, 6]

For information extraction, BayesStore stores text as a token table
`TokenTbl(strID, pos, token, label^p)` and a linear-chain CRF as a factor
table `MR(token, score[])` holding `phi[y_t, y_(t-1) | x_t]` as a
`|Y| x |Y|` array.  Inference is Viterbi (top-1 and incremental top-k),
forward-backward (marginals), and MCMC for skip-chain models.  The
query-driven variants push predicates into inference: constrained Viterbi
for `label = 'x'` selections, early-stopping `SelViterbi` for selections
over the ML world, rank-join over incremental Viterbi for probabilistic
joins, a hash filter on join keys before inference for joins over the
most-likely world, a Viterbi/marginal hybrid for projection [W11 6], and
per-document hybrid plans that pick Viterbi, sum-product or MCMC by each
document's grounded shape, including aggregate constraints over a
document's labels [W11 7.3, 7.6].  An entity table is a pivot of the
token table's most-likely labels [W11 6.2].

## Mapping onto a PostgreSQL extension

BayesStore changed the server.  An extension cannot add grammar, so there
is no `INFER` clause and no `SELECT Marginal(...)`.  Everything is a
function, an operator, or a planner hook.

### Principles

1. **Results are relations, but composing them is not inference.**
   Inference returns rows (`key, attribute, value, probability`) that SQL
   can join, filter and aggregate.  Doing so treats the returned values as
   independent, which is wrong whenever they are correlated: multiplying
   two marginals from one component is not their joint probability.
   BayesStore operators return a new relation _and_ a new model: they
   modify the model first, then filter the data against it (T2617-2622),
   and a later operator works on the result, as when a second selection
   intersects the entity sets of the first one's `Exist` factors
   (T2712-2713, T2762-2765) or a join conjoins both inputs' `Exist`
   conditions with the join condition (T3127-3129).  pg_gaps does the
   same inside one call: a call takes the whole query (a model, a
   relation or a join of two, and a predicate), builds the derived model
   internally, and only then runs inference.  Chaining calls, where the
   output of one is the input of another, is out of scope until a
   derived-model handle is designed (ROADMAP Q8).  The documentation for
   every function says which of its outputs may be combined in SQL.
2. **Correctness is checkable.** Every inference path has a brute-force
   possible-worlds oracle for small inputs, and the exact paths are
   cross-checked against an independent implementation (pgmpy) in tests.
   An approximate path (Gibbs, MCMC) reports how many samples it used and
   is tested against the exact path on models both can handle.
3. **Discrete, finite domains only**, as in BayesStore.  Continuous
   attributes are out of scope until there is a concrete need.
4. **No on-disk format of our own.** The model lives in ordinary catalog
   tables in schema `gaps`, so it is transactional, dumpable, and
   replicated with no WAL code.  Grounded models and posteriors are
   per-query memory, never persisted.  This removes the entire class of
   storage bugs pg_fts spends most of its rules on, and it is the reason
   not to add an access method until a measurement demands one.
5. **Planner honesty.** Set-returning functions declare real costs and
   row estimates.  A `SupportRequestRows` support function sees only
   constant arguments, so it improves plans with literal arguments; with
   parameters (generic plans) the planner falls back to the declared
   `ROWS`, which must therefore be a sensible default too.

### Model catalog

The model lives in ordinary tables in schema `gaps`, so it is
transactional, dumpable (each table and sequence is registered with
`pg_extension_config_dump`), and replicated with no WAL code of ours.

```
gaps.model          (model_id, name, owner regrole)
gaps.attribute      (attribute_id, model_id, rel regclass, relname, attname,
                     typname, entity_keys name[], domain text[])
gaps.stripe         (stripe_id, model_id, name, attribute_id)
gaps.stripe_qual    (stripe_id, ord, attname, op, value)   -- ANDed, on Ad
gaps.factor         (factor_id, model_id, name, child_stripe, cpt float8[])
gaps.factor_parent  (factor_id, ord, parent_stripe)
gaps.factor_map     (factor_id, ord, child_att, parent_att) -- equalities
```

**Identity of user objects.** `regclass` survives pg_upgrade (pg_class
OIDs are preserved, `pg_upgrade/check.c`) and pg_dump writes it as a
name, but it dangles after a `DROP`, and a later relation can reuse the
OID.  So each attribute row also stores the relation's qualified name,
the column's name and its type's name, and every use re-resolves them
and fails, naming what changed, if the OID's current name differs, the
column is gone, or its type changed.  Attribute numbers and type OIDs are
not stored at all.

**Entities.** A probabilistic attribute's random variables are its
entities: the distinct values of its `entity_keys` (by default the
table's primary key, giving one RV per row; a subset of it gives a
coarser entity set, several rows sharing one RV).  Every stripe over an
attribute uses that attribute's entity keys, so an RV is identified by
(attribute, entity key values) and stripes over one attribute that
overlap share RVs rather than conflicting.  The thesis allows stripes of
one attribute at different granularities; nothing in it needs that, and
it would make "which RV is this row" ambiguous.  The rows of one entity
must agree on an observed value; disagreeing evidence is an error naming
the entity.

Entity identity follows the key columns' own equality, as `GROUP BY`
does: each column's equality operator and collation decide which rows
are one entity (under a nondeterministic collation, `'A'` and `'a'` can
be one entity, named by either), and rows whose keys are NULL in the same
columns, and equal elsewhere, are one entity.  Grounding carries a key as
a composite of the key columns' types, whose record equality has exactly
these semantics.

**Key shapes.** Grounding keeps the entities of every attribute whose key
columns have the same types, typmods and collations, in order, in one
temp table for the transaction, with the key's fields named k1, k2, ...;
the column names come back from each attribute's `entity_keys` when a
result or an error names an entity.  So `customer(id int)` and
`orders(order_id int)` share one table.  Each distinct shape costs a key
table, an entity table, an index, and their types: about 16 locks, held
until commit, in the lock table that every backend shares.  That table
is sized for `max_locks_per_transaction` (default 64) times about
`max_connections` (default 100) entries, 6,400 with the defaults, and one
backend that fills it makes the others fail too.  (The table can borrow
spare shared memory past that size, which is why a single test session
can sometimes hold more, but nothing guarantees it.)  Grounding therefore
refuses a model with more than 100 distinct key shapes, an `ERROR`
naming the count, before it makes any table: at the limit one call holds
about 1,650 locks, a quarter of the default table, which leaves the rest
to other sessions.  A real model rarely has more than a handful of key
types; the limit is a guard against an unbounded failure, not a tuning
knob, so it is not a GUC.

**CPTs** are dense `float8[]`, indexed from 1, in row-major order over
(parents..., child), the child varying fastest.  Each variable's values are in
`gaps.attribute.domain` order, which is the order passed to
`declare_attribute`, not sorted: `ARRAY['1', '2', '10']` stays
`{1,2,10}`.  Note that a domain built with `array_agg(DISTINCT col)` comes
back sorted (`{1,10,2}` for text), so a CPT built in some other order
puts its probabilities on the wrong values without any error.
`gaps.cpt_rows(model, factor)` returns a factor's cells as rows (parent
values, child value, probability, prior) in array order, so nobody needs
to index the flat array by hand; `parents` is a JSON array of one
`{"<stripe>": value}` object per parent, in parent order, keyed by stripe
name because two parent stripes may share an attribute, and an array
because one stripe may be two parents of a factor.  The trigger on
`gaps.factor` checks every write: every entry finite and non-negative,
the length a multiple of the child's domain size, and every conditional
distribution summing to 1 within 1e-9.  The exact length for the
parents' domains needs `gaps.factor_parent`, which `pg_restore` loads
after `gaps.factor`, so the declaring functions and every grounding check
it instead.  A CPT may be NULL until `learn_cpts` fills it; inference
refuses a model with a NULL CPT.

**Priors.**  `gaps.factor.prior`, set with `gaps.set_prior(model, factor,
prior)`, holds a Dirichlet prior for `learn_cpts`: one non-negative
pseudocount per CPT cell, in the CPT's layout, checked like the CPT
except that its rows need not sum to anything.  `learn_cpts` sets each
parent row to its posterior mean, (count + prior) / (row total +
sum(prior row)) per cell, and a row whose counts and pseudocounts are all
zero to the uniform distribution.  NULL, the default, means `alpha` in
every cell, the symmetric Dirichlet(alpha); a factor with a prior ignores
`alpha`.  The prior is part of the model, so it is dumped with the
catalog, and inference never reads it.  A column rather than a
`learn_cpts` argument, because the prior belongs to the factor (it lives
as long as the factor, survives dump and restore, and is visible through
`cpt_rows`), and an argument would make every caller pass the same
arrays again.

**Entity-set predicates** are a conjunction of `column op constant` terms
stored as rows, with `op` one of `=`, `<>`, `<`, `<=`, `>`, `>=`.  Each use
builds the query from catalog-resolved, quoted identifiers and passes the
constants as parameters, cast to the column's type by its input function.
Predicates are never stored as SQL text to execute, and never as
`pg_node_tree`: that type has no input function, so SQL and SPI cannot
write it, the rows would not restore from a dump, and a node tree written
by one major version is not guaranteed to read in the next.  Terms may
reference only deterministic columns.  Disjunction and negation are a
later addition (ROADMAP Phase 4b).

**Mappings.** A factor's mapping to each parent is a set of equalities
`child_att = parent_att` plus the parent stripe's own predicate (its
constants).  With no equalities given, a parent stripe on the same
relation and entity keys as the child maps entity to itself (a horizontal
factor).  Grounding checks that every child entity maps to exactly one
parent entity in the parent stripe, and fails naming a child entity that
maps to none or to several.

The default mapping matches keys as entities are identified, so a
NULL-keyed child finds the parent entity with the same NULL key.  Explicit
equalities are SQL `=`, so a NULL in a mapped column matches nothing, and
a child whose mapped column is NULL maps to no parent: an error naming
it, not a guess.  A user who wants NULLs to match can map from a column
that encodes them.

**Snapshots.** Grounding reads the user's tables in several statements
(one per attribute for entities and evidence, then per factor for the
instances and per parent for the mapping).  Under READ COMMITTED each
statement sees its own snapshot, so a concurrent commit can make steps
disagree.  Grounding detects the disagreement it can see (a row reached
through a mapping whose entity the entity step did not see) and fails
with a hint; it does not detect evidence or rows changed between steps
in ways that leave every row an entity.  For one consistent view, run the
call in a REPEATABLE READ (or SERIALIZABLE) transaction, where every
statement shares the transaction's snapshot.  Grounding under one snapshot
whatever the isolation level needs C (Phase 6).

A missing value is SQL `NULL` in a declared probabilistic column.  `NULL`
in a deterministic column is ordinary SQL `NULL`, as in [W11 3.2].

### SQL surface (proposed; each phase settles its part)

```sql
-- declare and build
SELECT gaps.create_model('sensors');
SELECT gaps.declare_attribute('sensors', 'sensor1', 'temp', ARRAY['cold','hot']);
SELECT gaps.add_stripe('sensors', ...);   -- attribute + entity set
SELECT gaps.add_factor('sensors', child => ..., parents => ..., cpt => ...);
SELECT gaps.set_prior('sensors', ...);    -- optional Dirichlet pseudocounts per cell
SELECT gaps.learn_cpts('sensors');        -- posterior mean from complete rows
SELECT * FROM gaps.validate('sensors');   -- first-order acyclicity, no data read

-- query (built: the whole model, or the components of some entities)
SELECT * FROM gaps.marginals('sensors');
SELECT * FROM gaps.marginals('sensors', 'sensor1'::regclass,
                             '[{"t": 1, "room": 2, "sid": 1}]');
SELECT * FROM gaps.impute('sensors');                        -- MAP completion
-- proposed
SELECT * FROM gaps.select_rows('sensors', 'sensor1'::regclass,
                               $$ light = 'dark' $$);        -- rows + P(exist)
-- Phase 5 adds the join form: two relations and a join predicate.
```

### Inference engine

Plain C, with no PostgreSQL headers in the core (factor algebra, variable
elimination, Gibbs, Viterbi, forward-backward), so it compiles standalone
for property tests, fuzzing, and sanitizers, as pg_fts does for its codec.
A thin layer converts between catalog rows and engine structures.  The
engine works on grounded connected components and, within a query, caches
posteriors keyed by a canonical form of the whole component (its factors,
structure and evidence), which is first-order sharing done safely.

Randomized algorithms take a seed and a sample count from the call or
from GUCs (`gaps.seed`, `gaps.samples`) and are deterministic given them,
so test output is stable.

**Gibbs sampling, as built.**  A component is sampled only when exact
inference would exceed `gaps.max_factor_cells` or `gaps.max_total_cells`
and `gaps.samples` > 0 (the default, 0, keeps exact-or-error).  Each
chain clamps the evidence, sweeps the missing variables in a fixed
order, and redraws each from its full conditional (its own CPT row and
its children's, summed in logs).  After a burn-in of samples / 10 sweeps,
each of the samples sweeps adds every variable's full conditional to an
accumulator; the average is the Rao-Blackwellized marginal, which has
lower variance than counting draws and sums to 1.  The sampler requires
every reduced factor entry (every CPT entry consistent with the
evidence) to be positive.  That is sufficient for the chain to reach
every world, and a component that fails it is an `ERROR`.

Four chains run, and `gaps.samples` counts sweeps per chain, so a
component costs 4.4 x `gaps.samples` sweeps.  Per chain, not in total,
because a chain needs its own burn-in and a run long enough to mix:
splitting 4,000 sweeps four ways made the engine test's 6 x 6 grid
refuse on 34 of 200 seeds, against none at 4,000 per chain.  The first
chain starts with every variable at its first value, the second at its
last, the others at uniform draws.  The answer is the four chains'
pooled average.

**The convergence check, and what it cannot see.**  Single-site Gibbs
can stay in one mode of a multimodal posterior far longer than any
affordable run.  The review of the first version (one chain) found a
20 x 20 grid whose corner has a parent with 40 more children, with CPTs
of 0.8/0.2, where every exact marginal is 0.5 and the sampler returned
0.0 or 1.0 at 4,000 and at 100,000 samples, labelled 'gibbs' with
nothing to say it was a coin flip.  Now each chain's own estimates are
compared, and if two chains estimate any probability more than 0.1
apart (`GX_MIX_SPREAD`), the query fails with an `ERROR` naming the
component's size, `gaps.samples`, the spread and the limit, with a hint
to raise `gaps.samples` or the exact-inference bounds.  Measured at the
default threshold and 4,000 samples per chain:

| model | refused |
|---|---|
| 20,000 random strictly positive models of at most 12 variables (`test/engine`) | 0 (largest spread 0.078; largest pooled error 0.025) |
| the 6 x 6 regression grid, 200 seeds | 0 (largest spread 0.080) |
| the review's grid + hub (20 x 20, 40 children, p = 0.8), 5 seeds | 5 (spread 1.0) |
| 20 x 20 grid at p = 0.95, 5 seeds | 5 (spread 0.32-0.45; at 80,000 samples 0.055-0.073 and error 0.021-0.024) |
| 30 x 30 grid at p = 0.97, 3 seeds | 3 (spread 0.56-0.71) |
| a hub with 30 or 50 agreeing children, 3 seeds each | 6 (spread 1.0) |
| **not refused:** a binary hub with 50 ternary children, CPT {0.1, 0.8, 0.1 \| 0.4, 0.2, 0.4}, exact P(hub) = 0.5 | 0 of every seed tried; error 0.5 |
| **not refused:** a binary hub whose children alternately agree and disagree with it (p = 0.8), 40 seeds | 34 (6 accepted with error 0.5; 5 of 40 still at 100,000 samples) |

A multi-chain check narrows silent failure but cannot rule it out, and
the last two rows show it failing on ordinary CPTs (no entry below 0.1).
All four chains can sit in the same mode: in the ternary hub, the
all-first and all-last starts put every child at an end value and a
uniform start puts two thirds there, which all pull the hub the same
way.  Chains can also agree by accident while each is wrong; a spread
under 0.1 is evidence the chains mixed, not proof.  The 0.1 tolerance the tests use (`GIBBS_TOL`) was
measured on models of at most 12 variables and on the 6 x 6 grid, where
any chain mixes in a few sweeps; it is not a property of the method.
For an answer that must be right, raise the bounds and solve exactly.

**Determinism.**  The PRNG is xoshiro256** seeded from `gaps.seed` and
the hash of the component's canonical form; the four chains take
successive streams from it.  Equal components therefore get the same
answer with sharing on or off.  The answer depends only on the
component and the relative order of its variables' numbers, which fixes
the local numbering, the key and the sweep order, and not on where the
component sits in the network.  The key hashes native `int` and
`double` bytes, and draws go through `exp()`, which C does not require
to be correctly rounded, so bit-identical results are promised on one
platform, not across byte orders or C libraries.

`impute` on a sampled component returns each variable's most probable
pooled value (method 'gibbs-marginal-argmax') with a NULL probability.
That is not the joint MAP, and sampling gives no estimate of a joint
probability, so the rows of a sampled component must not be read as one
joint completion.

### Row estimates

`gaps.marginals`, `gaps.impute` and `gaps.learn_cpts` have a planner
support function, `gaps._rows_support` (`src/gaps_support.c`), that
answers `SupportRequestRows` when the model name is a constant (a literal,
or a bound parameter in a custom plan).  It reads the catalog and the
planner statistics, never user data:

- `marginals`: the sum over the model's attributes of
  `reltuples * null_frac * |domain|`, one row per (missing value, domain
  value);
- `impute`: the same sum without `|domain|`, one row per missing value;
- `learn_cpts`: the number of factors;
- a restricted `marginals` or `impute` (rel and entities constants): the
  number of elements of the entities array (duplicates and coarse
  entities counted as one row's worth each) times the same sum over `rel`'s
  attributes alone divided by `rel`'s `reltuples`, the rows an average
  entity of `rel` returns.  With `entities` not a constant (a subquery),
  or only one of the two given, there is no estimate.

`null_frac` is the column's `pg_statistic.stanullfrac`.  On a table that
ANALYZE samples completely the estimate is exact; otherwise it is as good
as the column statistics.  It ignores stripe predicates, because every
random variable of an attribute is one of its entities whatever its
stripes, and it ignores coarse entity keys (several rows sharing one
variable), so for those it overestimates by the rows-per-entity ratio.

The lookup runs as the caller through read-only SPI, so the catalog's
row-level security hides a model the caller cannot read, and the statistics
are used only under the rule of the `pg_stats` view: SELECT on the column
and no row-level security active on the table for the caller.  Because
row-level security makes the estimate depend on the role, the plan is
marked role-dependent.  There is no estimate, and the planner uses the
declared `ROWS` (1000 for `marginals` and `impute`, 10 for
`learn_cpts`), when the name is not a constant, the model is missing or
hidden, a relation has never been analyzed or has inheritance children, the
caller may not see a column's statistics, or anything in the lookup raises
an error, which a subtransaction catches so that planning never fails
because of it (a query cancel still propagates).  With `row_security = off`
the lookup's query on the catalog raises an error for any role that the
catalog's policies would filter, so such a role gets no estimate.

Planning never waits for a lock.  Before the lookup the support function
takes `AccessShareLock` on `gaps.model` and on `gaps.attribute` (or
`gaps.factor` for `learn_cpts`), their TOAST tables and all their indexes
with `ConditionalLockRelationOid`, the locks the lookup's queries take
anyway, and gives no estimate if any of them is not free, for instance
during someone's `LOCK` or `REINDEX` of the catalog.  The locks it gets are
held to the end of the transaction, as a query's would be.  The data
tables are not locked: their `pg_class`, `pg_attribute` and `pg_statistic`
rows are read through the syscache, which needs no lock on the table, and
a table dropped meanwhile simply has no rows there.

There is no estimate during recovery.  The lookup runs in a
subtransaction, and on a hot standby a recovery conflict (a replayed
`ACCESS EXCLUSIVE` lock, or cleanup of rows the snapshot can still see)
that arrives inside a subtransaction terminates the session rather than
cancelling the query (`postgres.c`, `ProcessRecoveryConflictInterrupt` in
17 and 18, `report_recovery_conflict` in 19).  The conditional locks
remove the lock wait but not the snapshot case, and a row estimate is not
worth a session.  There is no estimate under `SERIALIZABLE` either, where
the lookup's reads would take predicate locks on the catalog, so that
planning alone (an `EXPLAIN`, a branch never executed) could make the
transaction fail to serialize against a concurrent model edit.  Nor in
parallel mode, where a worker has no use for an estimate.

A cached plan depends on every data table it estimated and on the catalog
tables the lookup read, so `ANALYZE`, `GRANT`, `REVOKE` or DDL on any of
them replans it, and the estimate follows.  Ordinary inserts, updates and
deletes send no invalidation for a user table, the catalog included, so a
cached plan's estimate can lag an edit of the model (a new attribute, a
dropped factor) until something else replans it.  Only the estimate lags:
the plan calls the function, which reads the model as it is when it runs.
The estimate also uses `reltuples` as `ANALYZE` or `VACUUM` stored it,
without the planner's scaling by the table's current size, so after a bulk
load without `ANALYZE` it lags the planner's own row count for the table.

### Security

The extension is **not trusted** (`trusted = false`, the default) until a
review says otherwise.  The rules:

- **Every function runs as its caller.**  None is `SECURITY DEFINER`.  A
  definer function cannot tell who called it (`current_user` is the
  definer inside it), its reads of user tables would run as the
  extension's superuser owner and bypass row-level security, and its
  writes would bypass the catalog's own row-level security.
- **User data is read through SPI as the caller**, so row-level security
  and column privileges apply exactly as they would to the caller's own
  `SELECT`.
- **The catalog protects itself.**  Callers may write `gaps.*` directly or
  through the functions, and both paths meet the same checks: triggers
  validate every row (CPT shape and sums, operators, attribute types), and
  row-level security limits each role to the models it owns or is a
  member of the owner of.  Superusers and the catalog's owner bypass
  row-level security, as everywhere in PostgreSQL.  A consequence: pg_dump
  by a role that is neither superuser nor the catalog owner fails on these
  tables, because pg_dump runs with `row_security = off`.
- **Learning publishes aggregates.**  `learn_cpts` writes statistics of
  the rows the caller could see into the model, which every role that can
  read the model can read.  A model is no more private than its owner
  role.
- **Inputs at the trust boundary** (CPT arrays, domains, predicates, and
  the arrays the internal inference function takes) are validated before
  use, and inference is bounded by `gaps.max_factor_cells` (the largest
  factor) and `gaps.max_total_cells` (everything held at once for one
  component), so a query cannot ask for an exponential factor product; it
  gets an `ERROR` naming the size instead.  GUCs live under the `gaps.`
  prefix, reserved with `MarkGUCPrefixReserved("gaps")`.

## Out of scope

- Grammar changes (no `INFER` clause); everything is functions.
- Chaining calls so that one call's output relation and model feed the
  next, until a derived-model handle is designed (ROADMAP Q8).  One call
  can still express a whole select-project-join query.
- Continuous distributions.
- Duplicate-eliminating projection (not first-order; BayesStore deferred
  it too).
- Structure learning, and CRF training.  CPT learning from complete data
  is in scope; learning which factors exist is not.
- Junction trees and loopy belief propagation (the thesis did not study
  them either); exact inference is variable elimination with a size bound.
- A custom access method or any on-disk format, until measured need.
