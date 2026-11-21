-- Models over relations without ctid: a plain view, beside a table, a
-- partitioned table and a materialized view with the same rows, and the
-- relations declare_attribute refuses.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
\set VERBOSITY terse
\set SHOW_CONTEXT never

-- pg.ddx.io's m1 shape: a latent author a per email, a markdown flag md
-- observed.  Hand posterior P(a = llm | md): no .04/.895 = .044693, yes
-- .06/.105 = .571429.
CREATE TABLE em_t (num bigint, md boolean);
INSERT INTO em_t VALUES (1, false), (2, true), (3, false);
CREATE VIEW em_v AS
	SELECT num, NULL::text AS a, CASE WHEN md THEN 'yes' ELSE 'no' END AS md FROM em_t;
CREATE TABLE em_r AS SELECT * FROM em_v;
CREATE TABLE em_p (num bigint, a text, md text) PARTITION BY RANGE (num);
CREATE TABLE em_p1 PARTITION OF em_p FOR VALUES FROM (1) TO (3);
CREATE TABLE em_p2 PARTITION OF em_p FOR VALUES FROM (3) TO (10);
INSERT INTO em_p SELECT * FROM em_v;
CREATE MATERIALIZED VIEW em_mv AS SELECT * FROM em_v;

-- The same model over each relation; mapped by map_factor as pg.ddx.io's
-- was, or by the default mapping (same relation and keys).
CREATE FUNCTION pg_temp.m1(name text, rel regclass, mapped boolean DEFAULT true) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM gaps.create_model(name);
	PERFORM gaps.declare_attribute(name, rel, 'a', ARRAY['human', 'llm'], ARRAY['num']::name[]);
	PERFORM gaps.declare_attribute(name, rel, 'md', ARRAY['no', 'yes'], ARRAY['num']::name[]);
	PERFORM gaps.add_stripe(name, 'a', rel, 'a');
	PERFORM gaps.add_stripe(name, 'm', rel, 'md');
	PERFORM gaps.add_factor(name, 'pa', 'a', '{}', ARRAY[0.9, 0.1]);
	PERFORM gaps.add_factor(name, 'pm', 'm', ARRAY['a'], ARRAY[0.95, 0.05, 0.4, 0.6]);
	IF mapped THEN
		PERFORM gaps.map_factor(name, 'pm', 1, 'num', 'num');
	END IF;
END
$$;
SELECT pg_temp.m1('r', 'em_r'), pg_temp.m1('p', 'em_p'), pg_temp.m1('m', 'em_mv'),
	   pg_temp.m1('v', 'em_v'), pg_temp.m1('vd', 'em_v', false);
SELECT ok FROM gaps.validate('v');
SELECT (entity->>'num')::int AS num, round(probability::numeric, 6) AS p_llm, method
FROM gaps.marginals('v') WHERE attname = 'a' AND value = 'llm' ORDER BY 1;
-- Every relkind gives the view's rows exactly, unrestricted and restricted.
SELECT x.model, count(*) AS rows,
	   bool_and(x.probability = v.probability) AS equal
FROM (SELECT 'r' AS model, * FROM gaps.marginals('r')
	  UNION ALL SELECT 'p', * FROM gaps.marginals('p')
	  UNION ALL SELECT 'm', * FROM gaps.marginals('m')
	  UNION ALL SELECT 'v (restricted)', * FROM gaps.marginals('v', 'em_v', '[{"num": 1}, {"num": 2}, {"num": 3}]')) x
	JOIN gaps.marginals('v') v USING (attname, entity, value)
GROUP BY x.model ORDER BY 1;
SELECT (entity->>'num')::int AS num, value, probability
FROM gaps.impute('v', 'em_v', '[{"num": 2}]');

-- A view's columns carry no NOT NULL, so a restricted call treats its
-- keys as nullable: a NULL-keyed entity is found like any other under the
-- default mapping, which matches keys as entities are identified.  (A
-- map_factor equality is SQL =, so there it maps to no parent, an error.)
INSERT INTO em_t VALUES (NULL, true);
SELECT entity, round(probability::numeric, 6) AS p_llm
FROM gaps.marginals('vd', 'em_v', '[{"num": null}]') WHERE value = 'llm';
SELECT count(*) FROM gaps.marginals('v', 'em_v', '[{"num": null}]');  -- fail: no parent

-- A view's key need not be unique: rows with equal keys are one entity,
-- and when they disagree the error says the key is not unique.
INSERT INTO em_t VALUES (2, false);
SELECT * FROM gaps.marginals('v');                                 -- fail: key not unique
\set VERBOSITY default
SELECT * FROM gaps.marginals('v');                                 -- fail: same, in full
\set VERBOSITY terse

-- Rejections at declare time.
SELECT gaps.create_model('bad');
\set VERBOSITY default
SELECT gaps.declare_attribute('bad', 'em_v', 'a', ARRAY['human']);   -- fail: no key
SELECT gaps.declare_attribute('bad', 'em_mv', 'a', ARRAY['human']);  -- fail: no key
\set VERBOSITY terse
CREATE SEQUENCE em_seq;
CREATE INDEX em_r_num ON em_r (num);
CREATE TYPE em_ct AS (num int, a text);
SELECT gaps.declare_attribute('bad', 'em_seq', 'last_value', ARRAY['1'], ARRAY['log_cnt']::name[]); -- fail
SELECT gaps.declare_attribute('bad', 'em_r_num', 'num', ARRAY['1'], ARRAY['num']::name[]);       -- fail
SELECT gaps.declare_attribute('bad', 'em_ct', 'a', ARRAY['1'], ARRAY['num']::name[]);            -- fail
SELECT count(*) FROM gaps.attribute a JOIN gaps.model m USING (model_id) WHERE m.name = 'bad';

SELECT gaps.drop_model(name) FROM gaps.model WHERE name IN ('r', 'p', 'm', 'v', 'vd', 'bad');
