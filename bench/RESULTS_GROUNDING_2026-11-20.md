# Grounding with typed entity keys (2026-11-20)

Host: floki, a shared dev box (48 vCPU Intel Xeon Platinum 8375C, 185 GB),
PostgreSQL 18.7 from the flake's dev shell, a bench cluster of its own
(shared_buffers 4GB, work_mem 256MB, temp_buffers 1GB).  Other users' jobs
ran throughout; the load average was 7 to 21 during the timed runs, so
every number below comes with A/A runs and interleaved arms.  Workload:
`bench/sharing.sql` at mratio 0.1, as in `RESULTS_SHARING_2026-11-19.md`:
100 rooms x 8 sensors x T time steps, two attributes, 2T * 800 variables.
Timings are `\timing` inside one session (AGENTS.md rule 4): `_ground`
alone three times, then `gaps.marginals` three times.  Arms ran in the
order main, new, main, new, rebuilding and restarting the server between
arms.  "main" is 9beacd9; "new" is this branch.

## Correctness first

At T = 125, `gaps.marginals('hotel')` returns 39,846 rows on both, and
the two outputs, sorted and with probabilities printed in full, are
byte-identical (`cmp`).  The regression suite, `test/dump`,
`test/engine` and `test/pgmpy` pass unchanged on PG 17, 18 and 19
(`nix flake check`); pgmpy compared 2,762 marginals to 1e-9.

## What the profile showed

auto_explain at T = 125 on main: of 3.3 s, the four steps that build or
join jsonb keys took 2.1 s (entities 0.55 s per attribute, child
instances 0.31 s per factor, the parent join 0.71 s).  In a probe on the
same data, joining 100,000 distinct keys to their entities took 285 ms as
jsonb, 150 ms as a typed composite, and 70 ms as plain typed columns;
grouping the mapping join by child variable instead of by child key cut
it from 590 ms to 160 ms (typed columns) or 255 ms (composite).

## The change

Each attribute's entities live in a temp table (v int, k key, ev int),
where the key is a composite of the attribute's key columns with their
own types and collations (the row type of an empty temp table made with
`CREATE TABLE AS ... LIMIT 0`).  Every later step joins user rows to that
table on `ROW(c.k1, ...)::key = e.k` once, and carries only variable
numbers.  The parent mapping groups by child variable and counts distinct
parent variables, so no key arrays are built.  jsonb is built only for
the variables a result reports (`to_jsonb(e.k)`) and for error messages.

The composite, rather than separate typed columns, is what keeps NULL
key values working: record equality treats two NULLs as equal, as GROUP
BY does, and it hashes.  A column-by-column `=` would drop every row whose
key has a NULL.  `sql/infer.sql` now has a case for a NULL key and one
for a coarse parent mapped one-to-many.

Variables are numbered in key-column order instead of jsonb order.  That
changes which entity the "maps to no parent" error names first (the
expected output changed in that one line) and the component numbers, but
not any probability.

## Results

Medians of three runs in one session, ms.  The second arm of each kind is
the A/A run for the first.

| T | variables | arm | `_ground` runs | median | `marginals` runs | median |
|---|---|---|---|---|---|---|
| 125 | 200,000 | main | 3,300, 3,372, 3,384 | 3,372 | 3,339, 3,348, 3,330 | 3,339 |
| 125 | 200,000 | new | 1,743, 1,668, 1,642 | 1,668 | 1,722, 1,751, 1,732 | 1,732 |
| 125 | 200,000 | main (A/A) | 3,338, 3,243, 3,216 | 3,243 | 3,351, 3,343, 3,377 | 3,351 |
| 125 | 200,000 | new (A/A) | 2,684, 2,637, 2,302 | 2,637 | 1,732, 1,781, 1,758 | 1,758 |
| 125 | 200,000 | main, repeat | 3,318, 3,226, 3,188 | 3,226 | 3,277, 3,282, 3,274 | 3,277 |
| 125 | 200,000 | new, repeat | 1,691, 1,604, 1,614 | 1,614 | 1,703, 1,684, 1,686 | 1,686 |
| 125 | 200,000 | main, repeat (A/A) | 3,296, 3,209, 3,202 | 3,209 | 3,318, 3,306, 3,325 | 3,318 |
| 125 | 200,000 | new, repeat (A/A) | 1,677, 1,632, 1,628 | 1,632 | 1,700, 1,700, 1,715 | 1,700 |
| 1250 | 2,000,000 | main | 44,633, 33,449, 33,411 | 33,449 | 34,564, 34,597, 34,223 | 34,564 |
| 1250 | 2,000,000 | new | 17,239, 16,853, 17,037 | 17,037 | 17,712, 17,772, 17,771 | 17,771 |
| 1250 | 2,000,000 | main (A/A) | 33,673, 33,345, 39,046 | 33,673 | 38,959, 36,275, 35,010 | 36,275 |
| 1250 | 2,000,000 | new (A/A) | 17,385, 17,754, 16,953 | 17,385 | 17,887, 17,795, 17,788 | 17,795 |

Grounding and the whole call are 1.9x to 2.0x faster at both scales.  The
A/A medians agree within 5% except one: the first T = 125 new A/A arm
grounded in 2.6 s while `marginals` in the same session took 1.76 s, which
does not fit a slower `_ground`; the load average was 13 at the time.  I
reran the whole T = 125 sequence at load 7, and both new arms came out at
1.61 and 1.63 s.  Main is 3.2 to 3.4 s on this box against 2.2 s on the
idle EC2 host of the earlier report, so absolute numbers do not carry
across hosts; the ratio is the result.  Scaling from T = 125 to 1250 is
x10.2 to x10.8 for the new code (x10 rows), still linear.

## Where the time goes now

auto_explain on the new code at T = 1250, one `_ground` of 17.2 s:
entity tables 3.6 s (1.5 s per attribute plus 0.3 s copying into
pg_gaps_rv), child instances 3.7 s, the parent join 3.5 s, the coverage
check 2.3 s, and assembling the arrays 3.3 s.  No step is dominated by
keys any more.  The coverage check and the assembly never touch keys
(5.6 s together, a third of the total): they are sorts and window
aggregates over 2M to 3M rows of integers, which PL/pgSQL issuing SQL
cannot make much cheaper.  `marginals` minus `_ground` is 0.4 to 0.7 s
at T = 1250, which covers inference and naming the 200,000 missing
values; inference alone was not timed on this host.

The next step for grounding is therefore not the key representation.
The candidates are to fold the coverage check into building the child
instances, and to build the engine's arrays in C over SPI (option (c) of
the task: the instance rows are already integers, so this is a scan and
counting sort, not a rewrite of grounding).  Both are unmeasured.

Unmeasured: PG 17 and 19, vertical factors and coarse entity sets at
scale, keys of types other than int, and an idle host.
