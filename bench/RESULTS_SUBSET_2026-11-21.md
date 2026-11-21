# Restricted marginals at the nightly shape (2026-11-21)

Host: floki (local, 48 CPUs), shared: load average 7 to 15 during the
runs from other users' work, so absolute times are noisy.  PostgreSQL
18.7 (nixpkgs, clang), throwaway cluster from `dev/regress.sh`, default
settings.  Commit a132b0d.  Workload: `bench/subset.sql`, the pg.ddx.io
t6.sql shape: 318,000 emails, a latent 4-state author and 9 observed
5-valued children per email, one component per email, 318 emails
labelled.  `\timing` inside one session (AGENTS.md rule 4); each arm runs
twice (A/A), five restricted calls then three full calls, twice.

## Correctness first

For each n, the restricted call's rows equal the full call's rows for
the same emails with bit-identical probabilities:

| n (newest emails) | restricted rows | full rows for them | differ |
|---|---|---|---|
| 23 | 88 | 88 | 0 |
| 230 | 916 | 916 | 0 |
| 2,300 | 9,188 | 9,188 | 0 |

## Timings

Medians of each block, in ms.  Restricted is
`gaps.marginals('em9', 'em9', <n keys>)`; full is
`gaps.marginals('em9') WHERE attname = 'a'`.

| n | restricted A | restricted A' | full A | full A' |
|---|---|---|---|---|
| 23 | 71.5 | 37.7 | 39,568 | 33,851 |
| 230 | 112.7 | 109.2 | 33,391 | 35,087 |
| 230 (second run) | 110.0 | 109.0 | 33,374 | 33,434 |
| 2,300 | 904.0 | 916.6 | 34,118 | 35,614 |

At the nightly size of 230 emails the restricted call takes 110 ms against
33.4 s for the full call, about 300x, in two runs.  The A/A blocks agree
within 3% at 230 and 2,300.  At 23 they do not (71.5 against 37.7 ms):
at that size fixed per-call costs dominate (catalog reads, the temp
tables, one closure round per factor edge), and the first block ran
before the full calls had warmed the catalog caches; read it as "under
0.1 s", not as a number.  From 230 to 2,300 entities the restricted
time grows 8.3x for 10x the entities, so the cost follows the entities
requested, not the table.  The full call's own A/A spread (up to 17%)
is load noise on a shared host.

## After the review fixes (same day, the fix commit)

The nightly shape again, after the key-match and closure changes (load
average 8 to 10): 916 rows, 0 mismatched; restricted medians 108.2 and
105.8 ms (A/A), full 33.2 and 33.7 s.  No change.

## When it is not proportional

`bench/subset_keys.sql`: 100,000 rows, naive Bayes with three children,
230 entities requested, and a 2,000-row chain of vertical factors (y of
row i depends on y of row i - 1, `prev` indexed) with one entity
requested.  Every restricted call returned the full call's rows bit for
bit (460 rows for each key variant, 2 for the chain, 0 mismatched).
Medians of three, two passes, in ms; load average 9 to 11:

| arm | pass 1 | pass 2 |
|---|---|---|
| full call, 100,000 rows | 3,660 | 3,558 |
| restricted, nullable key with an index | 49.6 | 49.7 |
| restricted, nullable key, no index | 355.4 | 354.3 |
| restricted, primary key | 44.8 | 44.3 |
| restricted, NOT NULL key, no index | 207.5 | 204.8 |
| full call, 2,000-row chain | 31.1 | 30.8 |
| restricted, 2,000-row chain | 104.7 | 105.1 |

A nullable key column must also match two NULLs.  With an index the
planner probes it for both sides of that test; without one the key is
compared a second time as a record, which hashes.  Before that second
comparison the reviewer measured 24 to 27 s for the unindexed nullable
key, 7x the full call; it is now about 10x cheaper than the full call,
and an index makes it 70x cheaper.

A chain costs one closure round per step, and a round costs about one
statement per factor edge and direction, whatever its frontier: the
reviewer measured 1,772 ms restricted against 39 ms full on this chain,
and a 10,001-row chain hit the round cap after 9 s.  The closure now
gives up once its statements, less 100, times 25 exceed the model's
rows, and the call grounds the whole model and reports the requested
entities only.  The factor 25 is the measured ratio: the chain's one
edge makes 2 statements a round, so the reviewer's 1.77 s for its 2,000
rounds is about 0.44 ms a statement, and the full call's 31 to 39 ms for
2,000 rows about 0.02 ms a row, some 22 to 28 rows a statement.  Here
the closure gives up after 90 rounds (180 statements), then grounds
everything: 105 ms against 31 ms for the full call.  A 10,001-row chain took
320 ms against 134 ms (one run each, not A/A).  The restricted call can
still cost up to a few times the full call on such a model, but no
longer 45x, and it no longer fails.

## What makes it proportional

Each closure round, and each grounding step of a restricted call, joins
the frontier to the user's table on the entity key columns and the
mapping columns.  Here both are the primary key `num`, so every join is
an index probe.  Without an index on the mapping columns a round is a
hash join over the table: still correct, but it costs the table's size
per round.  The section above measures the other cases.

## Not measured

No clean (idle, EC2) host was used; the numbers above are from a shared
machine.  d-separation pruning was not implemented: every component here
is one email, so the closure is already the requested entities, and
pruning could not have helped this workload.
