# Project instructions: pg_gaps

Read this file first.  It says what the project is, where things live, and
the rules for working on it.  Everything else is linked, not repeated.

## What this is

A PostgreSQL extension (C, PGXS) that brings BayesStore to ordinary
PostgreSQL tables: some columns may be missing values, a separately stored
first-order probabilistic graphical model describes how those values are
distributed and correlated, and queries return answers with probabilities
computed by inference over the model, conditioned on the data present.
BayesStore was a research fork of PostgreSQL 8.2-8.4 (Wang, Michelakis,
Garofalakis, Hellerstein, VLDB 2008; Wang's 2011 thesis); pg_gaps rebuilds
its ideas as an extension, with functions where BayesStore changed the
grammar.

**It optimises for correct probabilities first.** A fast wrong
distribution is worse than no answer, because nothing downstream can tell.

Status: Phase 0 (foundations).  The extension installs and does nothing yet.

## Where things live

| need | file |
|---|---|
| what is built, what is next, open decisions | `ROADMAP.md` (the ONLY plan file) |
| the data model, query semantics, and how they map to an extension | `doc/DESIGN.md` |
| build, test, style, and how to submit | `CONTRIBUTING.md` |
| what changed per release | `CHANGELOG.md` |
| the papers | `1453896.pdf` in the root (gitignored), thesis URL in `doc/DESIGN.md` |

## Rules

Rules 1-12 come from pg_fts (`~/ws/pg_fts/AGENTS.md`), where each one cost
a wasted run, a wrong published number, or a shipped bug.  Rules 13 and up
are specific to probabilistic inference.

### Measurement

1. **A projected ratio is not a measurement.** Report what was measured;
   if it was not measured, say "unmeasured".
2. **Assert correctness before looking at any timing.** Both arms must
   return the same probabilities first.
3. **Re-run an arm against itself before believing a difference** between
   arms (A/A noise floor), and hold a number until it reproduces at a
   second scale.
4. **Never wrap `psql` in `/usr/bin/time`.** Use `\timing` inside one
   session and take the median.
5. **`psql -c` is a new backend every time.** Per-backend state reads as
   empty from a second invocation.
6. **`elog(LOG)` goes to the server log**, and `log_min_messages =
   warning` hides it.  Read the log of the server you started.
7. **Verify that what you are testing contains your change.** `grep` the
   built artifact or the installed file for the new symbol before
   believing a result.
8. **A test that compares nothing to nothing proves nothing.** Assert that
   the rows you compare exist and the probabilities are not all zero.

### Code

9. **Bug fix = root cause, in the shared function, for every caller.**
   Grep the siblings before declaring a class of bug fixed.
10. **Validate at the trust boundary before using a value.** CPT arrays,
    domains, stripe mappings and predicates come from SQL callers; check
    lengths, ranges and sums before indexing with them.
11. **ASCII only** in install SQL, C, specs and commit messages
    (`make check-ascii`). A non-ASCII byte in an install script breaks
    `CREATE EXTENSION` on non-UTF-8 databases.
12. **Every new TAP test goes into both `flake.nix` and
    `.github/workflows/ci.yml`.** A workflow that has never produced a run
    is not a gate: check that it ran before trusting a green badge.

### Inference

13. **Every inference path has an oracle.** Exact algorithms are checked
    against brute-force enumeration of possible worlds on small models,
    and against pgmpy (ROADMAP Q5). An approximate algorithm is checked
    against an exact one on every model both can solve, at a stated
    tolerance and sample count.
14. **Probabilities are an invariant, not an output.** Every distribution
    returned sums to 1 within the documented tolerance and has no negative
    entries; tests assert it on every result set, not one example.
15. **Randomness is seeded.** Sampling takes its seed and sample count from
    the call or a GUC and is deterministic given them; test output is
    stable across runs and platforms.
16. **Bound every grounding.** No code path builds a factor or a component
    whose size is not checked against a limit first; exceeding it is an
    `ERROR` naming the size, never an allocation failure or a hang. Long
    loops call `CHECK_FOR_INTERRUPTS()`.
17. **No persisted derived state** (grounded models, posteriors, caches)
    without a design note in `doc/DESIGN.md` that says what invalidates
    it. The model catalog is ordinary tables; keep it that way until a
    measurement says otherwise.
18. **Name the method in the result.** When an answer came from sampling,
    the output says so and says how many samples.
19. **Independence is a property of grounded components, not rows.** A
    coarse entity set or a one-to-many mapping correlates rows that no
    vertical factor mentions.  Per-component inference and the sharing
    cache are keyed on the component.
20. **Never sample a deterministic variable.** `Exist` is computed from
    each world.  Gibbs runs only on strictly positive components; a
    component with zeros is solved exactly or refused.
21. **One call per query; never compose probabilities in SQL.** Joining
    two marginal result sets assumes their values are independent.  A
    whole select-project-join query is one call, and each function's
    documentation says which outputs may be combined (DESIGN.md
    Principle 1, ROADMAP Q8).

### Repository

22. **One plan file.** `ROADMAP.md`.  No `NOTES_*.md`, `HANDOFF.md`, or
    `TODO.md`.
23. **Build out of tree.** `dev/regress.sh` builds under
    `/scratch/pg_gaps/`. This tree is synced between hosts, and a build
    directory left in it gets copied everywhere and resurrected when
    deleted.
24. **`expected/*.out` is source.** The user-level gitignore hides `*.out`;
    `.gitignore` re-includes `expected/`. Generate expected output from a
    real run and read it before committing it.
25. **A release renames the base install script** (`git mv
    pg_gaps--<old>.sql pg_gaps--<new>.sql`) and adds the
    `pg_gaps--<old>--<new>.sql` upgrade edge. The version lives in
    `pg_gaps.control` only; the Makefile and flake read it from there.
26. **Do not commit the papers.** They are the publishers' copyright;
    cite them by URL.

## Build and test

```sh
nix develop -c dev/regress.sh     # inner loop: PG 18, out of tree, throwaway cluster
nix flake check                   # the gate: build + regress on PG 17, 18, 19, ascii
```

Without Nix, PGXS works as usual (`make PG_CONFIG=... && make install &&
make installcheck`); see `CONTRIBUTING.md`.

See .agent-steering-domains.md for domain-specific steering (local).
