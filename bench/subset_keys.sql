-- Restricted marginals where the closure is not one probe per entity: the
-- entity key's NULLs and indexes (100,000 rows, naive Bayes with three
-- children, 230 entities requested), and a chain of vertical factors,
-- where each step of the chain is a closure round.  Each restricted call
-- is checked against the full call first (AGENTS.md rule 2), then timed
-- with the full call, A/A, in one session (rules 3 and 4); take the
-- medians.  Run with psql -X -f bench/subset_keys.sql against a database
-- with pg_gaps.
\set ON_ERROR_STOP 1
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;

-- One model per key variant, over a table of its own: nn_idx a nullable
-- indexed key, nn_none nullable and unindexed, pk a primary key, nn_nn
-- NOT NULL and unindexed.
CREATE FUNCTION pg_temp.keyed(t text, ddl text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE %I (num bigint %s, a text, o1 text, o2 text, o3 text)', t, ddl);
	EXECUTE format('INSERT INTO %I SELECT g, NULL, (ARRAY[''x'', ''y''])[1 + g %% 2], '
				   '(ARRAY[''x'', ''y''])[1 + g %% 3 %% 2], (ARRAY[''x'', ''y''])[1 + g %% 5 %% 2] '
				   'FROM generate_series(1, 100000) g', t);
	IF t = 'nn_idx' THEN
		EXECUTE 'CREATE INDEX ON nn_idx (num)';
	END IF;
	EXECUTE format('ANALYZE %I', t);
	PERFORM gaps.create_model(t);
	PERFORM gaps.declare_attribute(t, t, 'a', ARRAY['h', 'l'], ARRAY['num']::name[]);
	PERFORM gaps.add_stripe(t, 'a', t, 'a');
	PERFORM gaps.add_factor(t, 'pa', 'a', '{}', ARRAY[0.7, 0.3]);
	FOR i IN 1 .. 3 LOOP
		PERFORM gaps.declare_attribute(t, t, 'o' || i, ARRAY['x', 'y'], ARRAY['num']::name[]);
		PERFORM gaps.add_stripe(t, 's' || i, t, 'o' || i);
		PERFORM gaps.add_factor(t, 'f' || i, 's' || i, ARRAY['a'], ARRAY[0.8, 0.2, 0.3, 0.7]);
	END LOOP;
END
$$;
SELECT pg_temp.keyed('nn_idx', ''), pg_temp.keyed('nn_none', ''),
	   pg_temp.keyed('pk', 'PRIMARY KEY'), pg_temp.keyed('nn_nn', 'NOT NULL');
SELECT jsonb_agg(jsonb_build_object('num', g)) AS new
FROM generate_series(100000 - 229, 100000) g \gset

-- A chain: y of row i depends on y of row i - 1.
CREATE TABLE ch (id int PRIMARY KEY, prev int, s int, y text);
INSERT INTO ch SELECT i, i - 1, least(i - 1, 1), CASE WHEN i % 50 = 0 THEN 'p' END
FROM generate_series(1, 2000) i;
CREATE INDEX ON ch (prev);
ANALYZE ch;
SELECT gaps.create_model('ch');
SELECT gaps.declare_attribute('ch', 'ch', 'y', ARRAY['p', 'q']);
SELECT gaps.add_stripe('ch', 'y', 'ch', 'y');
SELECT gaps.add_stripe('ch', 'y0', 'ch', 'y');
SELECT gaps.add_stripe_qual('ch', 'y0', 's', '=', '0');
SELECT gaps.add_stripe('ch', 'y1', 'ch', 'y');
SELECT gaps.add_stripe_qual('ch', 'y1', 's', '=', '1');
SELECT gaps.add_factor('ch', 'y0', 'y0', '{}', ARRAY[0.4, 0.6]);
SELECT gaps.add_factor('ch', 'y1', 'y1', ARRAY['y'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT gaps.map_factor('ch', 'y1', 1, 'prev', 'id');

-- Correctness: each restricted call's rows equal the full call's, bit for
-- bit, and are not nothing (rule 8).
CREATE FUNCTION pg_temp.check(t text, keys jsonb, OUT model text, OUT rows bigint,
							  OUT mismatched bigint) LANGUAGE plpgsql AS $$
BEGIN
	model := t;
	SELECT count(*) INTO rows FROM gaps.marginals(t, t::regclass, keys);
	SELECT count(*) INTO mismatched
	FROM (SELECT * FROM gaps.marginals(t) m
		  WHERE m.entity IN (SELECT jsonb_array_elements(keys))) f
		FULL JOIN gaps.marginals(t, t::regclass, keys) p USING (attname, entity, value)
	WHERE f.probability IS DISTINCT FROM p.probability;
END
$$;
SELECT * FROM pg_temp.check('nn_idx', :'new')
UNION ALL SELECT * FROM pg_temp.check('nn_none', :'new')
UNION ALL SELECT * FROM pg_temp.check('pk', :'new')
UNION ALL SELECT * FROM pg_temp.check('nn_nn', :'new')
UNION ALL SELECT * FROM pg_temp.check('ch', '[{"id": 1}, {"id": 1000}]');

-- Timings: three of each, twice over (A/A).
\timing on
\o /dev/null
\echo full nn_idx
SELECT count(*) FROM gaps.marginals('nn_idx');
SELECT count(*) FROM gaps.marginals('nn_idx');
SELECT count(*) FROM gaps.marginals('nn_idx');
\echo full ch
SELECT count(*) FROM gaps.marginals('ch');
SELECT count(*) FROM gaps.marginals('ch');
SELECT count(*) FROM gaps.marginals('ch');
\echo pass 1
\echo restricted nn_idx
SELECT count(*) FROM gaps.marginals('nn_idx', 'nn_idx', :'new');
SELECT count(*) FROM gaps.marginals('nn_idx', 'nn_idx', :'new');
SELECT count(*) FROM gaps.marginals('nn_idx', 'nn_idx', :'new');
\echo restricted nn_none
SELECT count(*) FROM gaps.marginals('nn_none', 'nn_none', :'new');
SELECT count(*) FROM gaps.marginals('nn_none', 'nn_none', :'new');
SELECT count(*) FROM gaps.marginals('nn_none', 'nn_none', :'new');
\echo restricted pk
SELECT count(*) FROM gaps.marginals('pk', 'pk', :'new');
SELECT count(*) FROM gaps.marginals('pk', 'pk', :'new');
SELECT count(*) FROM gaps.marginals('pk', 'pk', :'new');
\echo restricted nn_nn
SELECT count(*) FROM gaps.marginals('nn_nn', 'nn_nn', :'new');
SELECT count(*) FROM gaps.marginals('nn_nn', 'nn_nn', :'new');
SELECT count(*) FROM gaps.marginals('nn_nn', 'nn_nn', :'new');
\echo restricted ch
SELECT count(*) FROM gaps.marginals('ch', 'ch', '[{"id": 1000}]');
SELECT count(*) FROM gaps.marginals('ch', 'ch', '[{"id": 1000}]');
SELECT count(*) FROM gaps.marginals('ch', 'ch', '[{"id": 1000}]');
\echo pass 2
\echo restricted nn_idx
SELECT count(*) FROM gaps.marginals('nn_idx', 'nn_idx', :'new');
SELECT count(*) FROM gaps.marginals('nn_idx', 'nn_idx', :'new');
SELECT count(*) FROM gaps.marginals('nn_idx', 'nn_idx', :'new');
\echo restricted nn_none
SELECT count(*) FROM gaps.marginals('nn_none', 'nn_none', :'new');
SELECT count(*) FROM gaps.marginals('nn_none', 'nn_none', :'new');
SELECT count(*) FROM gaps.marginals('nn_none', 'nn_none', :'new');
\echo restricted pk
SELECT count(*) FROM gaps.marginals('pk', 'pk', :'new');
SELECT count(*) FROM gaps.marginals('pk', 'pk', :'new');
SELECT count(*) FROM gaps.marginals('pk', 'pk', :'new');
\echo restricted nn_nn
SELECT count(*) FROM gaps.marginals('nn_nn', 'nn_nn', :'new');
SELECT count(*) FROM gaps.marginals('nn_nn', 'nn_nn', :'new');
SELECT count(*) FROM gaps.marginals('nn_nn', 'nn_nn', :'new');
\echo restricted ch
SELECT count(*) FROM gaps.marginals('ch', 'ch', '[{"id": 1000}]');
SELECT count(*) FROM gaps.marginals('ch', 'ch', '[{"id": 1000}]');
SELECT count(*) FROM gaps.marginals('ch', 'ch', '[{"id": 1000}]');
\echo full nn_idx
SELECT count(*) FROM gaps.marginals('nn_idx');
SELECT count(*) FROM gaps.marginals('nn_idx');
SELECT count(*) FROM gaps.marginals('nn_idx');
\echo full ch
SELECT count(*) FROM gaps.marginals('ch');
SELECT count(*) FROM gaps.marginals('ch');
SELECT count(*) FROM gaps.marginals('ch');
\o
\timing off
SELECT gaps.drop_model(name) FROM gaps.model
WHERE name IN ('nn_idx', 'nn_none', 'pk', 'nn_nn', 'ch');
DROP TABLE nn_idx, nn_none, pk, nn_nn, ch;
