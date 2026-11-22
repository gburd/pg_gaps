# pg_gaps

Probabilistic graphical models over incomplete relations, for PostgreSQL.

pg_gaps brings the ideas of BayesStore (Wang, Michelakis, Garofalakis and
Hellerstein, VLDB 2008) to ordinary PostgreSQL tables.  Some columns of a
table may be missing values.  A separately stored first-order Bayesian
network says how those values are distributed and how they correlate,
with each other and with the values that are present.  Queries return the
distribution of each missing value, or the most likely completion of
each row, computed by exact inference conditioned on the data you have.

**Status: 0.1.0, the first release.**  The model catalog, exact
inference, learning (counts with Dirichlet priors, and EM), restricted
queries, parameterised and composite models are built and tested.
Probabilistic selection, projection and joins (`select_rows` and
friends) are not yet; `ROADMAP.md` lists what is next and
`doc/DESIGN.md` describes the data model and how it maps onto
PostgreSQL.

## Requirements

PostgreSQL 17, 18 or 19, a C compiler, and the server headers.

## Build and install

From PGXN:

```sh
pgxn install pg_gaps
```

From a source tree or a release zip:

```sh
make PG_CONFIG=/path/to/pg_config
sudo make install PG_CONFIG=/path/to/pg_config
```

```sql
CREATE EXTENSION pg_gaps;   -- installs into schema "gaps"; needs a superuser
```

## A first model

A survey where some people did not answer "smoker" or "cough", and
smoking makes coughing more likely:

```sql
CREATE TABLE survey (id int PRIMARY KEY, smoker text, cough text);
-- ... rows, some with NULLs ...

SELECT gaps.create_model('health');
SELECT gaps.declare_attribute('health', 'survey', 'smoker', ARRAY['no', 'yes']);
SELECT gaps.declare_attribute('health', 'survey', 'cough',  ARRAY['no', 'yes']);
SELECT gaps.add_stripe('health', 's', 'survey', 'smoker');
SELECT gaps.add_stripe('health', 'c', 'survey', 'cough');
SELECT gaps.add_factor('health', 'p_smoker', 's');                 -- P(smoker)
SELECT gaps.add_factor('health', 'p_cough',  'c', ARRAY['s']);     -- P(cough | smoker)

SELECT * FROM gaps.learn_cpts('health');          -- from the data
SELECT * FROM gaps.marginals('health');           -- every missing value's distribution
SELECT * FROM gaps.impute('health');              -- the most likely completion
```

A row that coughs and did not say whether it smokes comes back more
likely a smoker than the base rate, and one that does not cough less
likely; every distribution sums to 1 and says which method computed it.

What else is there:

- **Correlated rows.**  A factor's parent may be another row's value
  (sensor 2 depends on sensor 1 at the same time and room), through
  `gaps.map_factor`; inference solves each connected group of rows exactly.
- **Learning.**  `learn_cpts(model, alpha)` takes the posterior mean
  under a symmetric Dirichlet(alpha), or per-cell pseudocounts set with
  `gaps.set_prior`; `method => 'em'` also learns from rows with missing
  values.
- **Some entities only.**  `gaps.marginals(model, rel, entities)` grounds
  only the components that hold the given entities, with the same
  answers as the whole-model call.
- **Views, binned numbers, weighted factors.**  A model may sit over a
  view; `declare_attribute(..., edges => ...)` bins a numeric column;
  `add_factor(..., kind => 'logistic' | 'noisy_or', params => ...)`
  computes a CPT from weights, and an aggregating parent takes any number
  of rows (all of a person's e-mails).
- **Several networks as one.**  `gaps.add_member` builds a composite
  model whose members are grounded and solved together, so combining
  them never multiplies dependent probabilities in SQL.
- **Checks.**  `gaps.validate(model)` proves a model acyclic from its
  declaration alone; `gaps.cpt_rows(model, factor)` shows a CPT as rows.

## Limits worth knowing

- Domains are discrete and finite; bin continuous values.
- Exact inference is bounded by `gaps.max_factor_cells` and
  `gaps.max_total_cells`; a model past them is an error, not a hang.
  Setting `gaps.samples` lets a too-large component be sampled (Gibbs,
  four chains, refused when they disagree).  Sampling is
  **experimental**: chains that all settle in the same wrong mode give a
  wrong answer without an error; see `doc/DESIGN.md`.
- Do not join two `marginals` results and multiply: that assumes the
  values are independent, which the model does not.  Ask for one call.
- `marginals` and friends create temporary tables, so they do not run on
  a hot standby.
- Functions whose names start with an underscore are internal and may
  change between releases.

## Testing

`make installcheck` runs the regression suite.  With Nix, `nix flake
check` builds and tests against all three majors, plus the standalone
engine tests and the pgmpy cross-check; see `CONTRIBUTING.md`.

## License

PostgreSQL License; see `LICENSE`.
