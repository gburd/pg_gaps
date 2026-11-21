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

## What makes it proportional

Each closure round, and each grounding step of a restricted call, joins
the frontier to the user's table on the entity key columns and the
mapping columns.  Here both are the primary key `num`, so every join is
an index probe.  Without an index on the mapping columns a round falls
back to a scan of the table, which is still correct but costs the
table's size per round.

## Not measured

No clean (idle, EC2) host was used; the numbers above are from a shared
machine.  d-separation pruning was not implemented: every component here
is one email, so the closure is already the requested entities, and
pruning could not have helped this workload.
