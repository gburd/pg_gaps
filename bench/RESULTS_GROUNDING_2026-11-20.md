# Grounding with typed entity keys (2026-11-20)

Two sets of numbers follow.  The first, "On the EC2 host", is the one to
quote: an idle host, the branch merged into current main, compared with
main, and including the small and wide models where per-call cost shows.
The second, "Local runs (floki)", is the first measurement of the key
change alone, on a loaded shared box; its ratio held up on EC2, its
absolute numbers do not carry.

## On the EC2 host

Host: c7i.4xlarge (16 vCPU), Fedora 44, PGDG PostgreSQL 18.7 built with
gcc 16.2.1; the one-minute load average was 0.6 to 1.1 before every arm.
"main" is 2d62ab1; "new" is wip/groundkeys at 9caa66a merged into
2d62ab1 (the merge conflicts only in ROADMAP.md and CHANGELOG.md).  Each
arm has its own cluster (shared_buffers 2GB, work_mem 256MB,
temp_buffers 1GB) loading pg_gaps from its own DESTDIR through
extension_control_path, and its own databases, made after installing it.
The script printed an md5 of every `gaps` function body in each arm's
databases, which differ between arms and agree within one (CREATE
EXTENSION copies function bodies into the catalog, so a reinstall alone
changes nothing; two earlier runs reused databases and are discarded).

Workloads: `bench/sharing.sql` at T = 125 (200,000 variables) and T =
1250 (2M), mratio 0.1; "small", a 2-attribute model over 2 rows (one
prior, one CPT); "wide", 900 attributes on one 2-row table, each with a
prior.  Correctness first: `gaps.marginals` output, sorted with
probabilities printed in full, is identical between the arms on all
four (39,846, 400,394, 4 and 3,600 rows).

Timings are `\timing` in one session per arm and workload: `_ground`
three times then `marginals` three times (hotel), `marginals` seven
times (small; the first call, which loads the library and caches plans,
is dropped) or three times (wide).  Medians, ms; the order was main,
new, main, new, so the second run of each arm is its A/A.

| workload | main | new | main (A/A) | new (A/A) | new / main |
|---|---|---|---|---|---|
| T=125 `_ground` | 2,564 | 1,130 | 2,262 | 1,054 | 0.44-0.47 |
| T=125 `marginals` | 2,586 | 1,203 | 2,266 | 1,114 | 0.47-0.49 |
| T=1250 `_ground` | 23,752 | 9,997 | 23,987 | 9,700 | 0.40-0.42 |
| T=1250 `marginals` | 26,575 | 10,347 | 24,468 | 10,350 | 0.39-0.42 |
| small `marginals` | 10.6 | 13.8 | 10.6 | 13.2 | 1.25-1.30 |
| wide `marginals` | 532 | 301 | 498 | 297 | 0.57-0.60 |

Main's A/A spread is 12% at T = 125 (2,564 against 2,262) and 8% on
`marginals` at T = 1250; the new arm's is under 8% everywhere.  The
ratio is outside both.  Grounding the hotel workload is 2.2x to 2.5x
faster, and the whole call 2.0x to 2.6x.  At T = 1250 `marginals` takes
10.3 s for 2M variables against 24.6 s in `RESULTS_SHARING_2026-11-19.md`
on the same instance type.

The small model is 3 ms (25-30%) slower per call.  The branch issues
more statements per grounding (77 against 61 in one call, from
pg_stat_statements), and the first call in each transaction creates the
key and entity tables and an index.  Within one transaction, after the
first call, both arms take about 3.5 to 4 ms (locally, unmeasured on
EC2).  Making the temp tables live for the session would remove the
creation cost, but they are created as the caller, and a later call by
another role in the same session (SET ROLE) cannot write them; I left
that alone.

The wide model was 3.6x slower than main in the first version of this
branch (the review's 2.40 s against 0.67 s, locally).  It is now 1.7x
faster.  Three changes did it, each found by pg_stat_statements on this
host: reusing the tables (no CREATE and DROP per attribute), naming all
attributes' tables in one indexed catalog query instead of 900 queries
that each scanned 900 pg_attribute rows, and taking an unqualified
stripe's child variables from its entity table instead of joining the
900-column table's rows to it (293 ms of that was planning).

Locks.  With max_locks_per_transaction 64 and max_connections 100, the
review's probes now pass where the first version failed: 3,000 calls in
one DO block (9.9 s, locally), 200 calls in one transaction hold 58
locks (6,032 before), one call over 4,000 attributes on four tables
returns its 8,000 rows.  `sql/infer.sql` asserts that 50 calls in one
transaction add no locks.

The S2 check (a LEFT JOIN on the parent's entities, so a parent row
grounding did not see as an entity is an error rather than dropped)
costs nothing measurable.  With the hotel's light factor mapped
explicitly on (t, room, sid), so the mapping joins rows, `_ground`
medians were 1,090 / 1,089 ms with the check and 1,081 / 1,089 ms with
the inner join it replaced at T = 125, and 10,414 / 10,300 against
10,411 / 10,394 ms at T = 1250 (interleaved, A/A); every arm returned
the same marginals (md5).

Unmeasured on EC2: PG 17 and 19, vertical factors and coarse entity
sets at scale, keys of types other than int.

## Local runs (floki)

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
arms.  "main" is 9beacd9; "new" is 512406f, the first commit of this
branch (before the table reuse and the later changes measured on EC2).

### Correctness first

At T = 125, `gaps.marginals('hotel')` returns 39,846 rows on both, and
the two outputs, sorted and with probabilities printed in full, are
byte-identical (`cmp`).  The regression suite, `test/dump`,
`test/engine` and `test/pgmpy` pass unchanged on PG 17, 18 and 19
(`nix flake check`); pgmpy compared 2,762 marginals to 1e-9.

### What the profile showed

auto_explain at T = 125 on main: of 3.3 s, the four steps that build or
join jsonb keys took 2.1 s (entities 0.55 s per attribute, child
instances 0.31 s per factor, the parent join 0.71 s).  In a probe on the
same data, joining 100,000 distinct keys to their entities took 285 ms as
jsonb, 150 ms as a typed composite, and 70 ms as plain typed columns;
grouping the mapping join by child variable instead of by child key cut
it from 590 ms to 160 ms (typed columns) or 255 ms (composite).

### The change

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
key has a NULL.  `sql/infer.sql` has a case for a NULL key, a NULL-keyed
child under the default mapping (which matches whole keys), and a coarse
parent mapped one-to-many.  Explicit mappings stay SQL `=`, so a NULL in
a mapped column maps to no parent (doc/DESIGN.md, "Mappings").

Variables are numbered in key-column order instead of jsonb order.  That
changes which entity the "maps to no parent" error names first (the
expected output changed in that one line) and the component numbers, but
not any probability.

### Results

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

### Where the time goes now

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

These local numbers predate the per-call fixed cost the review found
(temp tables re-created per attribute per call); the EC2 section measures
the fixed version, including small and wide models.
