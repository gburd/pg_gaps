-- bench/capacity.sql: grounding capacity at pg.ddx.io's reported shape
-- (their N1, 2026-11-22): one table of n rows and 7 attributes, six
-- priors and a child y with six parents whose parent space is 4 * 4 * 4 *
-- 4 * 15 * 16 = 61,440 rows.  About 1% of each parent and 5% of y are
-- missing.
--
-- Usage: psql -X -v n=5250000 -v learn=1 -v marginals=1 -f bench/capacity.sql
--
-- Prints an md5 of the gaps function bodies (which code ran, AGENTS.md
-- rule 7), then times learn_cpts (method 'counts') and a count of
-- marginals' rows, each in this one session (rule 4).  Errors do not stop
-- the script, so an arm that fails reports its time to the error.
SET client_min_messages = warning;
DROP EXTENSION IF EXISTS pg_gaps CASCADE;
CREATE EXTENSION pg_gaps;
SELECT md5(string_agg(p.prosrc, '' ORDER BY p.proname, p.oid)) AS code
FROM pg_proc p WHERE p.pronamespace = 'gaps'::regnamespace;
DROP TABLE IF EXISTS cap;

SELECT setseed(0.25);
CREATE TABLE cap (id int PRIMARY KEY, a int, b int, c int, d int, e int, f int, y int);
INSERT INTO cap
SELECT g, a, b, c, d, e, f,
	   CASE WHEN random() < 0.05 THEN NULL
			WHEN random() < 0.05 + 0.9 * ((a + b + c + d + e + f) % 10) / 9.0 THEN 2
			ELSE 1 END
FROM (SELECT g, 1 + floor(random() * 4)::int AS a, 1 + floor(random() * 4)::int AS b,
			 1 + floor(random() * 4)::int AS c, 1 + floor(random() * 4)::int AS d,
			 1 + floor(random() * 15)::int AS e, 1 + floor(random() * 16)::int AS f
	  FROM generate_series(1, :n) g) x;
UPDATE cap SET a = CASE WHEN random() < 0.01 THEN NULL ELSE a END,
			   b = CASE WHEN random() < 0.01 THEN NULL ELSE b END,
			   c = CASE WHEN random() < 0.01 THEN NULL ELSE c END,
			   d = CASE WHEN random() < 0.01 THEN NULL ELSE d END,
			   e = CASE WHEN random() < 0.01 THEN NULL ELSE e END,
			   f = CASE WHEN random() < 0.01 THEN NULL ELSE f END;
VACUUM ANALYZE cap;

SELECT gaps.create_model('cap');
SELECT gaps.declare_attribute('cap', 'cap', x,
	ARRAY(SELECT i::text FROM generate_series(1, k) i))
FROM (VALUES ('a', 4), ('b', 4), ('c', 4), ('d', 4), ('e', 15), ('f', 16), ('y', 2)) v(x, k);
SELECT gaps.add_stripe('cap', x, 'cap', x)
FROM unnest(ARRAY['a', 'b', 'c', 'd', 'e', 'f', 'y']) x;
SELECT gaps.add_factor('cap', 'p' || x, x) FROM unnest(ARRAY['a', 'b', 'c', 'd', 'e', 'f']) x;
SELECT gaps.add_factor('cap', 'py', 'y', ARRAY['a', 'b', 'c', 'd', 'e', 'f']);

\if :learn
\timing on
SELECT factor, complete_entities FROM gaps.learn_cpts('cap', 1);
\timing off
\endif
\if :marginals
\timing on
SELECT count(*) AS rows, count(DISTINCT component) AS components,
	   min(probability) >= 0 AS nonneg
FROM gaps.marginals('cap');
\timing off
\endif
