-- Restricted marginals at the pg.ddx.io nightly shape (their t6.sql): an
-- email table of 318,000 rows, a latent 4-state author per email and 9
-- observed children, one component per email.  Times the full call and a
-- call restricted to the 230 newest emails, A/A for each arm, all in one
-- session (AGENTS.md rules 1-4); take the medians.  Correctness first:
-- the restricted rows must equal the full call's rows for those emails.
-- Run with psql -X -f bench/subset.sql against a database with pg_gaps;
-- -v n=2300 requests the newest n emails instead of 230.
\set ON_ERROR_STOP 1
\if :{?n}
\else
\set n 230
\endif
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
DROP TABLE IF EXISTS em9;
SELECT gaps.drop_model(name) FROM gaps.model WHERE name = 'em9';
CREATE TABLE em9 (num bigint PRIMARY KEY, a text, o1 text, o2 text, o3 text, o4 text,
				  o5 text, o6 text, o7 text, o8 text, o9 text);
INSERT INTO em9
SELECT g, NULL, b[1 + (g * 7) % 5], b[1 + (g * 11) % 5], b[1 + (g * 13) % 5],
	   b[1 + (g * 17) % 5], b[1 + (g * 19) % 5], b[1 + (g * 23) % 5],
	   b[1 + (g * 29) % 5], b[1 + (g * 31) % 5], b[1 + (g * 37) % 5]
FROM generate_series(1, 318000) g, (SELECT ARRAY['b1', 'b2', 'b3', 'b4', 'b5'] AS b) x;
-- A few labelled emails, as in the real job.
UPDATE em9 SET a = 'human' WHERE num % 1000 = 0;
ANALYZE em9;
SELECT gaps.create_model('em9');
SELECT gaps.declare_attribute('em9', 'em9', 'a',
	ARRAY['human', 'human_nonnative', 'nonnative_llm_assisted', 'llm_generated']);
SELECT gaps.add_stripe('em9', 'a', 'em9', 'a');
SELECT gaps.add_factor('em9', 'pa', 'a', '{}', ARRAY[0.6, 0.3, 0.05, 0.05]);
DO $$ BEGIN FOR i IN 1..9 LOOP
	PERFORM gaps.declare_attribute('em9', 'em9', 'o' || i, ARRAY['b1', 'b2', 'b3', 'b4', 'b5']);
	PERFORM gaps.add_stripe('em9', 's' || i, 'em9', 'o' || i);
	PERFORM gaps.add_factor('em9', 'f' || i, 's' || i, ARRAY['a'],
		ARRAY[0.3, 0.25, 0.2, 0.15, 0.1, 0.3, 0.25, 0.2, 0.15, 0.1,
			  0.2, 0.2, 0.2, 0.2, 0.2, 0.1, 0.15, 0.2, 0.25, 0.3]);
	PERFORM gaps.map_factor('em9', 'f' || i, 1, 'num', 'num');
END LOOP; END $$;
SELECT jsonb_agg(jsonb_build_object('num', num)) AS new
FROM em9 WHERE num > 318000 - :n \gset

-- Correctness: same rows, bit for bit, and not nothing (rule 8).
CREATE TEMP TABLE part AS SELECT * FROM gaps.marginals('em9', 'em9', :'new');
CREATE TEMP TABLE whole AS SELECT * FROM gaps.marginals('em9')
WHERE (entity ->> 'num')::bigint > 318000 - :n;
SELECT (SELECT count(*) FROM part) AS part_rows, (SELECT count(*) FROM whole) AS whole_rows,
	   (SELECT count(*) FROM part p FULL JOIN whole w USING (attname, entity, value)
		WHERE p.probability IS DISTINCT FROM w.probability) AS mismatched;
DROP TABLE part, whole;

\timing on
SELECT count(*) FROM gaps.marginals('em9', 'em9', :'new');
SELECT count(*) FROM gaps.marginals('em9', 'em9', :'new');
SELECT count(*) FROM gaps.marginals('em9', 'em9', :'new');
SELECT count(*) FROM gaps.marginals('em9', 'em9', :'new');
SELECT count(*) FROM gaps.marginals('em9', 'em9', :'new');
SELECT count(*) FROM gaps.marginals('em9') WHERE attname = 'a';
SELECT count(*) FROM gaps.marginals('em9') WHERE attname = 'a';
SELECT count(*) FROM gaps.marginals('em9') WHERE attname = 'a';
SELECT count(*) FROM gaps.marginals('em9', 'em9', :'new');
SELECT count(*) FROM gaps.marginals('em9', 'em9', :'new');
SELECT count(*) FROM gaps.marginals('em9', 'em9', :'new');
SELECT count(*) FROM gaps.marginals('em9', 'em9', :'new');
SELECT count(*) FROM gaps.marginals('em9', 'em9', :'new');
SELECT count(*) FROM gaps.marginals('em9') WHERE attname = 'a';
SELECT count(*) FROM gaps.marginals('em9') WHERE attname = 'a';
SELECT count(*) FROM gaps.marginals('em9') WHERE attname = 'a';
\timing off
SELECT gaps.drop_model('em9');
DROP TABLE em9;
