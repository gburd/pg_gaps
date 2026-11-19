# pg_gaps

Probabilistic graphical models over incomplete relations, for PostgreSQL.

pg_gaps brings the ideas of BayesStore (Wang, Michelakis, Garofalakis and
Hellerstein, VLDB 2008) to ordinary PostgreSQL tables.  Some columns of a
table may be missing values.  A separately stored first-order Bayesian
network says how those values are distributed and how they correlate,
with each other and with the values that are present.  Queries then return
the distribution of each missing value, the most likely completion of
each row, or the rows that satisfy a predicate together with the
probability that each one does, all computed by inference conditioned on
the data you have.

**Status: early development.** The extension installs and does nothing
yet.  `ROADMAP.md` lists the phases; `doc/DESIGN.md` describes the data
model and how it maps onto PostgreSQL.

## Requirements

PostgreSQL 17, 18 or 19, a C compiler, and the server headers.

## Build and install

```sh
make PG_CONFIG=/path/to/pg_config
sudo make install PG_CONFIG=/path/to/pg_config
```

```sql
CREATE EXTENSION pg_gaps;   -- installs into schema "gaps"
```

With Nix, `nix flake check` builds and tests against all three majors;
see `CONTRIBUTING.md`.

## License

PostgreSQL License; see `LICENSE`.
