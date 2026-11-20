-- Inference end to end: grounding a model over tables, marginals and MAP
-- completions, checked against a brute-force possible-worlds computation
-- written in plain SQL, which shares no code with the extension.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
\set VERBOSITY terse

-- Two sensors per room and time.  sensor 2's temperature depends on
-- sensor 1's in the same room and time (a vertical factor, phi3 of the
-- paper's Fig. 3.5); every light reading depends on its own temperature
-- (horizontal).
CREATE TABLE sensor (t int, room int, sid int, temp text, light text,
					 PRIMARY KEY (t, room, sid));
INSERT INTO sensor VALUES
	(1, 1, 1, NULL,  'dark'),
	(1, 1, 2, NULL,  NULL),
	(1, 2, 1, 'hot', NULL),
	(1, 2, 2, NULL,  'bright'),
	(2, 1, 1, NULL,  NULL),
	(2, 1, 2, 'cold', 'dark');

SELECT gaps.create_model('m');
SELECT gaps.declare_attribute('m', 'sensor', 'temp', ARRAY['cold', 'hot']);
SELECT gaps.declare_attribute('m', 'sensor', 'light', ARRAY['dark', 'bright']);
SELECT gaps.add_stripe('m', 'tp1', 'sensor', 'temp');
SELECT gaps.add_stripe_qual('m', 'tp1', 'sid', '=', '1');
SELECT gaps.add_stripe('m', 'tp2', 'sensor', 'temp');
SELECT gaps.add_stripe_qual('m', 'tp2', 'sid', '=', '2');
SELECT gaps.add_stripe('m', 'l', 'sensor', 'light');
SELECT gaps.add_stripe('m', 'tp', 'sensor', 'temp');
SELECT gaps.add_factor('m', 'prior1', 'tp1', '{}', ARRAY[0.3, 0.7]);
SELECT gaps.add_factor('m', 'copy', 'tp2', ARRAY['tp1'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT gaps.map_factor('m', 'copy', 1, 't', 't');
SELECT gaps.map_factor('m', 'copy', 1, 'room', 'room');
SELECT gaps.add_factor('m', 'light', 'l', ARRAY['tp'], ARRAY[0.75, 0.25, 0.4, 0.6]);

-- The posterior of every missing value.  Probabilities per value; within a
-- component (a room and time here) the values are correlated.
SELECT attname, entity, value, round(probability::numeric, 6) AS p, component
FROM gaps.marginals('m')
ORDER BY attname, entity::text, value;

-- Every distribution sums to 1 and is non-negative (AGENTS.md rule 14).
SELECT count(*) AS bad_groups FROM (
	SELECT attname, entity FROM gaps.marginals('m')
	GROUP BY attname, entity
	HAVING abs(sum(probability) - 1) > 1e-12 OR min(probability) < 0) x;

-- The oracle.  For room 1 at time 1: missing temp1, temp2, light2;
-- observed light1 = dark.  Enumerate the 8 worlds, multiply the CPT
-- entries, normalize, and compare with what the extension returned.
WITH w AS (
	SELECT t1, t2, l2,
		   (CASE t1 WHEN 'cold' THEN 0.3 ELSE 0.7 END)			-- prior1
		 * (CASE WHEN t1 = 'cold' THEN (CASE t2 WHEN 'cold' THEN 0.9 ELSE 0.1 END)
				 ELSE (CASE t2 WHEN 'cold' THEN 0.2 ELSE 0.8 END) END)	-- copy
		 * (CASE t1 WHEN 'cold' THEN 0.75 ELSE 0.4 END)			-- light1 = dark
		 * (CASE WHEN t2 = 'cold' THEN (CASE l2 WHEN 'dark' THEN 0.75 ELSE 0.25 END)
				 ELSE (CASE l2 WHEN 'dark' THEN 0.4 ELSE 0.6 END) END) AS p
	FROM unnest(ARRAY['cold', 'hot']) t1, unnest(ARRAY['cold', 'hot']) t2,
		 unnest(ARRAY['dark', 'bright']) l2),
oracle AS (
	SELECT 'temp' AS attname, 1 AS sid, t1 AS value, sum(p) / (SELECT sum(p) FROM w) AS p
	FROM w GROUP BY t1
	UNION ALL
	SELECT 'temp', 2, t2, sum(p) / (SELECT sum(p) FROM w) FROM w GROUP BY t2
	UNION ALL
	SELECT 'light', 2, l2, sum(p) / (SELECT sum(p) FROM w) FROM w GROUP BY l2)
SELECT o.attname, o.sid, o.value, count(m.*) AS found,
	   abs(o.p - m.probability) < 1e-12 AS agrees
FROM oracle o
	LEFT JOIN gaps.marginals('m') m
	  ON m.attname = o.attname AND m.value = o.value
	 AND m.entity = jsonb_build_object('t', 1, 'room', 1, 'sid', o.sid)
GROUP BY o.attname, o.sid, o.value, o.p, m.probability
ORDER BY 1, 2, 3;

-- MAP completion: one value per missing variable, jointly most probable
-- within its component, with that assignment's posterior probability.
SELECT attname, entity, value, round(probability::numeric, 6) AS p_component
FROM gaps.impute('m')
ORDER BY attname, entity::text;

-- Coarse entities: readings of one sensor at one time share an RV.
CREATE TABLE reading (sid int, t int, k int, status text, PRIMARY KEY (sid, t, k));
INSERT INTO reading VALUES (1, 1, 1, 'up'), (1, 1, 2, NULL), (1, 2, 1, NULL), (1, 2, 2, NULL);
SELECT gaps.create_model('c');
SELECT gaps.declare_attribute('c', 'reading', 'status', ARRAY['up', 'down'],
							  ARRAY['sid', 't']::name[]);
SELECT gaps.add_stripe('c', 's', 'reading', 'status');
SELECT gaps.add_factor('c', 'p', 's', '{}', ARRAY[0.9, 0.1]);
SELECT entity, value, round(probability::numeric, 12) AS p
FROM gaps.marginals('c') ORDER BY entity::text, value;
-- A coarse parent mapped one-to-many: room occupancy per (room, t), over
-- several rows each, is the parent of every sensor's temperature in that
-- room and time.  Room 1 at time 1 is observed occupied; room 2 at time 1
-- has one hot reading below it, room 1 at time 2 one cold reading.  The
-- oracle is Bayes' rule by hand: P(yes | hot) = 0.3 * 0.1 / (0.3 * 0.1 +
-- 0.7 * 0.8), and so on.
CREATE TABLE occ (room int, t int, k int, occupied text, PRIMARY KEY (room, t, k));
INSERT INTO occ VALUES (1, 1, 1, 'yes'), (1, 1, 2, NULL), (2, 1, 1, NULL), (2, 1, 2, NULL),
	(1, 2, 1, NULL);
SELECT gaps.create_model('o');
SELECT gaps.declare_attribute('o', 'occ', 'occupied', ARRAY['yes', 'no'],
							  ARRAY['room', 't']::name[]);
SELECT gaps.declare_attribute('o', 'sensor', 'temp', ARRAY['cold', 'hot']);
SELECT gaps.add_stripe('o', 'occ', 'occ', 'occupied');
SELECT gaps.add_stripe('o', 'tp', 'sensor', 'temp');
SELECT gaps.add_factor('o', 'occ', 'occ', '{}', ARRAY[0.3, 0.7]);
SELECT gaps.add_factor('o', 'tp', 'tp', ARRAY['occ'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT gaps.map_factor('o', 'tp', 1, 't', 't');
SELECT gaps.map_factor('o', 'tp', 1, 'room', 'room');
WITH oracle(attname, entity, value, p) AS (VALUES
	('occupied', '{"room": 2, "t": 1}', 'yes', 0.03 / 0.59),
	('occupied', '{"room": 1, "t": 2}', 'yes', 0.27 / 0.41),
	('temp', '{"t": 1, "room": 1, "sid": 1}', 'cold', 0.9),
	('temp', '{"t": 1, "room": 1, "sid": 2}', 'cold', 0.9),
	('temp', '{"t": 1, "room": 2, "sid": 2}', 'cold', 0.139 / 0.59),
	('temp', '{"t": 2, "room": 1, "sid": 1}', 'cold', 0.271 / 0.41))
SELECT o.attname, o.entity, count(m.*) AS found,
	   abs(o.p - m.probability) < 1e-12 AS agrees
FROM oracle o
	FULL JOIN (SELECT * FROM gaps.marginals('o') WHERE value IN ('yes', 'cold')) m
	  ON m.attname = o.attname AND m.entity = o.entity::jsonb
GROUP BY o.attname, o.entity, o.p, m.probability
ORDER BY 1, 2;
SELECT gaps.drop_model('o');
DROP TABLE occ;

-- An entity key may be NULL: rows with a NULL key are one entity, as in
-- GROUP BY.  Here that entity is observed through one of its two rows.
CREATE TABLE nk (id int PRIMARY KEY, g int, x text);
INSERT INTO nk VALUES (1, NULL, NULL), (2, NULL, 'b'), (3, 1, NULL);
SELECT gaps.create_model('nk');
SELECT gaps.declare_attribute('nk', 'nk', 'x', ARRAY['a', 'b'], ARRAY['g']::name[]);
SELECT gaps.add_stripe('nk', 'x', 'nk', 'x');
SELECT gaps.add_factor('nk', 'p', 'x', '{}', ARRAY[0.25, 0.75]);
SELECT entity, value, round(probability::numeric, 12) AS p FROM gaps.marginals('nk') ORDER BY value;
-- A NULL-keyed entity with a parent: y depends on x of the same entity by
-- the default mapping, which matches keys as entities do, NULLs equal.
-- The oracle for g NULL (x observed b): P(y = c | b) = 0.2.
ALTER TABLE nk ADD COLUMN y text;
SELECT gaps.declare_attribute('nk', 'nk', 'y', ARRAY['c', 'd'], ARRAY['g']::name[]);
SELECT gaps.add_stripe('nk', 'y', 'nk', 'y');
SELECT gaps.add_factor('nk', 'py', 'y', ARRAY['x'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT attname, entity, value, round(probability::numeric, 12) AS p
FROM gaps.marginals('nk') WHERE attname = 'y' ORDER BY entity::text, value;
-- The same through a parent stripe with a predicate, which joins rows.
SELECT gaps.add_stripe('nk', 'xq', 'nk', 'x');
SELECT gaps.add_stripe_qual('nk', 'xq', 'id', '<', '10');
UPDATE gaps.factor_parent SET parent_stripe = s.stripe_id
FROM gaps.stripe s WHERE s.name = 'xq'
	AND factor_id = (SELECT factor_id FROM gaps.factor WHERE name = 'py')
RETURNING s.name;
SELECT attname, entity, value, round(probability::numeric, 12) AS p
FROM gaps.marginals('nk') WHERE attname = 'y' ORDER BY entity::text, value;
-- An explicit mapping compares with =, so a NULL maps to no parent.
SELECT gaps.map_factor('nk', 'py', 1, 'g', 'g');
SELECT * FROM gaps.marginals('nk');                          -- fail: maps to no parent
SELECT gaps.drop_model('nk');
DROP TABLE nk;

-- Repeated groundings in one transaction reuse their temp tables, so the
-- locks the backend holds do not grow with the number of calls.
BEGIN;
SELECT count(*) > 0 AS works FROM gaps.marginals('c');
SELECT count(*) AS locks1 FROM pg_locks WHERE pid = pg_backend_pid() \gset
DO $$ BEGIN FOR i IN 1 .. 50 LOOP PERFORM count(*) FROM gaps.marginals('c'); END LOOP; END $$;
SELECT count(*) - :locks1 AS lock_growth FROM pg_locks WHERE pid = pg_backend_pid();
COMMIT;

-- Keys of the same types share one entity table whatever their columns
-- are called, and each entity is still named by its own columns.  A
-- model with more key shapes than the bound is refused, naming the count.
CREATE TABLE sk1 (a int PRIMARY KEY, x text);
CREATE TABLE sk2 (b int PRIMARY KEY, y text);
INSERT INTO sk1 VALUES (1, NULL);
INSERT INTO sk2 VALUES (2, NULL);
SELECT gaps.create_model('sk');
SELECT gaps.declare_attribute('sk', 'sk1', 'x', ARRAY['p', 'q']);
SELECT gaps.declare_attribute('sk', 'sk2', 'y', ARRAY['p', 'q']);
SELECT gaps.add_stripe('sk', 'x', 'sk1', 'x');
SELECT gaps.add_stripe('sk', 'y', 'sk2', 'y');
SELECT gaps.add_factor('sk', 'px', 'x', '{}', ARRAY[0.25, 0.75]);
SELECT gaps.add_factor('sk', 'py', 'y', '{}', ARRAY[0.5, 0.5]);
BEGIN;
SELECT attname, entity, value, round(probability::numeric, 12) AS p
FROM gaps.marginals('sk') ORDER BY 1, 3;
SELECT count(*) AS entity_tables FROM pg_class
WHERE relnamespace = pg_my_temp_schema() AND relname LIKE 'pg\_gaps\_e%' AND relkind = 'r';
COMMIT;
SELECT gaps.drop_model('sk');
DROP TABLE sk1, sk2;
SELECT gaps.create_model('many');
DO $$
BEGIN
	FOR i IN 1 .. 101 LOOP
		EXECUTE format('CREATE TABLE many%s (k numeric(%s) PRIMARY KEY, x text)', i, i);
		PERFORM gaps.declare_attribute('many', format('many%s', i)::regclass, 'x', ARRAY['a']);
	END LOOP;
END $$;
SELECT * FROM gaps.marginals('many');                        -- fail: 101 shapes
SELECT gaps.drop_model('many');
DO $$ BEGIN FOR i IN 1 .. 101 LOOP EXECUTE format('DROP TABLE many%s', i); END LOOP; END $$;

-- Entity keys must not repeat a column.
CREATE TABLE dk (id int PRIMARY KEY, g int, x text);
SELECT gaps.create_model('dk');
SELECT gaps.declare_attribute('dk', 'dk', 'x', ARRAY['a', 'b'],
							  ARRAY['g', 'g']::name[]);               -- fail: g twice
SELECT gaps.drop_model('dk');
DROP TABLE dk;

UPDATE reading SET status = 'down' WHERE (sid, t, k) = (1, 1, 2);
SELECT * FROM gaps.marginals('c');                          -- fail: conflicting evidence
UPDATE reading SET status = 'sideways' WHERE (sid, t) = (1, 1);
SELECT * FROM gaps.marginals('c');                          -- fail: value not in domain

-- Grounding errors.
SELECT gaps.create_model('bad');
SELECT gaps.declare_attribute('bad', 'sensor', 'temp', ARRAY['cold', 'hot']);
SELECT gaps.add_stripe('bad', 'tp', 'sensor', 'temp');
SELECT * FROM gaps.marginals('bad');                         -- fail: no CPT covers temp
SELECT gaps.add_factor('bad', 'p', 'tp', '{}');
SELECT * FROM gaps.marginals('bad');                         -- fail: NULL CPT
SELECT gaps.set_cpt('bad', 'p', ARRAY[0.5, 0.5]);
SELECT gaps.add_stripe('bad', 'tp_again', 'sensor', 'temp');
SELECT gaps.add_factor('bad', 'p2', 'tp_again', '{}', ARRAY[0.5, 0.5]);
SELECT * FROM gaps.marginals('bad');                         -- fail: two CPTs for one RV
SELECT gaps.drop_model('bad');
SELECT gaps.create_model('bad');
SELECT gaps.declare_attribute('bad', 'sensor', 'temp', ARRAY['cold', 'hot']);
SELECT gaps.add_stripe('bad', 'tp', 'sensor', 'temp');
SELECT gaps.add_stripe('bad', 'tp1', 'sensor', 'temp');
SELECT gaps.add_stripe_qual('bad', 'tp1', 'sid', '=', '1');
SELECT gaps.add_factor('bad', 'p', 'tp', ARRAY['tp1'], ARRAY[0.5, 0.5, 0.5, 0.5]);
SELECT gaps.map_factor('bad', 'p', 1, 't', 't');
SELECT * FROM gaps.marginals('bad');                         -- fail: maps to several parents
-- Also require room = room and the child's sid = the parent's room: the
-- parent stripe holds only sid 1, so a child in room 2 finds no parent.
SELECT gaps.map_factor('bad', 'p', 1, 'room', 'room');
SELECT gaps.map_factor('bad', 'p', 1, 'sid', 'room');
SELECT * FROM gaps.marginals('bad');                         -- fail: maps to no parent
SELECT gaps.drop_model('bad');

-- A cycle: temp of sensor 1 depends on sensor 2's and the other way round.
SELECT gaps.create_model('cyc');
SELECT gaps.declare_attribute('cyc', 'sensor', 'temp', ARRAY['cold', 'hot']);
SELECT gaps.add_stripe('cyc', 'a', 'sensor', 'temp');
SELECT gaps.add_stripe_qual('cyc', 'a', 'sid', '=', '1');
SELECT gaps.add_stripe('cyc', 'b', 'sensor', 'temp');
SELECT gaps.add_stripe_qual('cyc', 'b', 'sid', '=', '2');
SELECT gaps.add_factor('cyc', 'ab', 'a', ARRAY['b'], ARRAY[0.5, 0.5, 0.5, 0.5]);
SELECT gaps.map_factor('cyc', 'ab', 1, 't', 't');
SELECT gaps.map_factor('cyc', 'ab', 1, 'room', 'room');
SELECT gaps.add_factor('cyc', 'ba', 'b', ARRAY['a'], ARRAY[0.5, 0.5, 0.5, 0.5]);
SELECT gaps.map_factor('cyc', 'ba', 1, 't', 't');
SELECT gaps.map_factor('cyc', 'ba', 1, 'room', 'room');
SELECT * FROM gaps.marginals('cyc');                         -- fail: cycle
SELECT gaps.drop_model('cyc');

-- A CPT written directly with a length that fits the child but not the
-- parents is refused at grounding.
UPDATE gaps.factor SET cpt = ARRAY[0.5, 0.5] WHERE name = 'copy';
SELECT * FROM gaps.marginals('m');                           -- fail: CPT length
UPDATE gaps.factor SET cpt = ARRAY[0.9, 0.1, 0.2, 0.8] WHERE name = 'copy';

-- Evidence the model calls impossible.
SELECT gaps.create_model('z');
SELECT gaps.declare_attribute('z', 'sensor', 'temp', ARRAY['cold', 'hot']);
SELECT gaps.add_stripe('z', 'tp', 'sensor', 'temp');
SELECT gaps.add_factor('z', 'p', 'tp', '{}', ARRAY[1, 0]);
SELECT * FROM gaps.marginals('z');                           -- fail: hot has probability 0
SELECT gaps.drop_model('z');

-- The size bounds turn an oversized problem into an error, not an
-- allocation: a CPT larger than the bound is refused before inference, and
-- a component too large for the cells allowed is refused by the engine.
SET gaps.max_factor_cells = 2;
SELECT count(*) FROM gaps.marginals('m');                    -- fail: CPT too big
RESET gaps.max_factor_cells;
SET gaps.max_total_cells = 4;
SELECT count(*) FROM gaps.marginals('m');                    -- fail: component too big
RESET gaps.max_total_cells;

-- Gibbs sampling for a component too large to solve exactly: a 6 x 6
-- grid where each cell depends on the cell above and the cell to its
-- left.  Its CPTs have at most 8 cells, but exact inference on the grid
-- needs factors of about 2^7, so gaps.max_factor_cells = 8 stops it.
CREATE TABLE grid (i int, j int, im1 int, jm1 int, v text, PRIMARY KEY (i, j));
INSERT INTO grid
SELECT i, j, i - 1, j - 1,
	   CASE WHEN (i * 7 + j) % 5 = 0 THEN CASE WHEN (i + j) % 2 = 0 THEN 'a' ELSE 'b' END END
FROM generate_series(1, 6) i, generate_series(1, 6) j;
SELECT gaps.create_model('g');
SELECT gaps.declare_attribute('g', 'grid', 'v', ARRAY['a', 'b']);
SELECT gaps.add_stripe('g', 'corner', 'grid', 'v');
SELECT gaps.add_stripe_qual('g', 'corner', 'i', '=', '1');
SELECT gaps.add_stripe_qual('g', 'corner', 'j', '=', '1');
SELECT gaps.add_stripe('g', 'top', 'grid', 'v');
SELECT gaps.add_stripe_qual('g', 'top', 'i', '=', '1');
SELECT gaps.add_stripe_qual('g', 'top', 'j', '>', '1');
SELECT gaps.add_stripe('g', 'side', 'grid', 'v');
SELECT gaps.add_stripe_qual('g', 'side', 'i', '>', '1');
SELECT gaps.add_stripe_qual('g', 'side', 'j', '=', '1');
SELECT gaps.add_stripe('g', 'inner', 'grid', 'v');
SELECT gaps.add_stripe_qual('g', 'inner', 'i', '>', '1');
SELECT gaps.add_stripe_qual('g', 'inner', 'j', '>', '1');
SELECT gaps.add_stripe('g', 'any', 'grid', 'v');
SELECT gaps.add_factor('g', 'p0', 'corner', '{}', ARRAY[0.4, 0.6]);
SELECT gaps.add_factor('g', 'left', 'top', ARRAY['any'], ARRAY[0.8, 0.2, 0.3, 0.7]);
SELECT gaps.map_factor('g', 'left', 1, 'i', 'i');
SELECT gaps.map_factor('g', 'left', 1, 'jm1', 'j');
SELECT gaps.add_factor('g', 'up', 'side', ARRAY['any'], ARRAY[0.8, 0.2, 0.3, 0.7]);
SELECT gaps.map_factor('g', 'up', 1, 'im1', 'i');
SELECT gaps.map_factor('g', 'up', 1, 'j', 'j');
SELECT gaps.add_factor('g', 'both', 'inner', ARRAY['any', 'any'],
					   ARRAY[0.9, 0.1, 0.6, 0.4, 0.5, 0.5, 0.2, 0.8]);
SELECT gaps.map_factor('g', 'both', 1, 'im1', 'i');
SELECT gaps.map_factor('g', 'both', 1, 'j', 'j');
SELECT gaps.map_factor('g', 'both', 2, 'i', 'i');
SELECT gaps.map_factor('g', 'both', 2, 'jm1', 'j');
CREATE TEMP TABLE grid_exact AS SELECT * FROM gaps.marginals('g');
SELECT method, samples, count(*) FROM grid_exact GROUP BY 1, 2;
SET gaps.max_factor_cells = 8;
SELECT count(*) FROM gaps.marginals('g');                    -- fail: too large, samples 0
SET gaps.samples = 4000;
SET gaps.seed = 42;
CREATE TEMP TABLE grid_gibbs AS SELECT * FROM gaps.marginals('g');
-- Every missing value is answered by sampling, every distribution sums to
-- 1 (rule 14), and every probability is within 0.1 of exact (rule 13; the
-- tolerance is the engine test's, test/engine/test_engine.c GIBBS_TOL,
-- measured on small models like this one, not a property of sampling).
SELECT method, samples, count(*) FROM grid_gibbs GROUP BY 1, 2;
SELECT count(*) AS bad_groups FROM (
	SELECT entity FROM grid_gibbs GROUP BY entity
	HAVING abs(sum(probability) - 1) > 1e-12 OR min(probability) < 0) x;
SELECT count(*) AS compared, count(*) FILTER (WHERE abs(e.probability - s.probability) < 0.1)
	   AS within_tol, bool_or(e.probability <> s.probability) AS not_exact
FROM grid_exact e JOIN grid_gibbs s USING (entity, value);
-- The same seed repeats the answer bit for bit, with sharing on or off.
SELECT count(*) AS differ FROM grid_gibbs s JOIN gaps.marginals('g') r USING (entity, value)
WHERE r.probability <> s.probability;
SET gaps.share_components = off;
SELECT count(*) AS differ FROM grid_gibbs s JOIN gaps.marginals('g') r USING (entity, value)
WHERE r.probability <> s.probability;
RESET gaps.share_components;
-- impute on a sampled component takes each value's most probable sampled
-- value and says so; it gives no joint probability.
SELECT method, samples, count(*), count(probability) AS with_probability
FROM gaps.impute('g') GROUP BY 1, 2;
SELECT count(*) AS not_argmax FROM gaps.impute('g') i JOIN grid_gibbs s USING (entity)
WHERE s.value <> i.value AND s.probability > 0.5;
-- Sampling is cancellable: a billion samples would run for hours.
SET gaps.samples = 1000000000;
SET statement_timeout = '1s';
SELECT count(*) FROM gaps.marginals('g');                    -- fail: canceled
RESET statement_timeout;
SET gaps.samples = 4000;
-- A posterior the chains cannot cross: 50 readings of the corner cell,
-- each agreeing with it with probability 0.8, pin the corner to whichever
-- value a chain starts it at, so the chains (two of which start at
-- opposite values) disagree and the answer is refused rather than given.
CREATE TABLE grid_reading (k int PRIMARY KEY, i int, j int, v text);
INSERT INTO grid_reading SELECT k, 1, 1, NULL FROM generate_series(1, 50) k;
SELECT gaps.declare_attribute('g', 'grid_reading', 'v', ARRAY['a', 'b']);
SELECT gaps.add_stripe('g', 'reading', 'grid_reading', 'v');
SELECT gaps.add_factor('g', 'reads', 'reading', ARRAY['corner'], ARRAY[0.8, 0.2, 0.2, 0.8]);
SELECT gaps.map_factor('g', 'reads', 1, 'i', 'i');
SELECT gaps.map_factor('g', 'reads', 1, 'j', 'j');
SELECT count(*) FROM gaps.marginals('g');                    -- fail: chains disagree
-- A zero in a CPT the component uses rules out sampling.
SELECT gaps.set_cpt('g', 'both', ARRAY[0.9, 0.1, 0.6, 0.4, 0.5, 0.5, 0, 1]);
SELECT count(*) FROM gaps.marginals('g');                    -- fail: zero, cannot sample
RESET gaps.samples;
RESET gaps.seed;
RESET gaps.max_factor_cells;
DROP TABLE grid, grid_reading, grid_exact, grid_gibbs;
SELECT gaps.drop_model('g');

-- A role that cannot read the table cannot infer over it, even with the
-- model's owner role.
CREATE ROLE gaps_reader;
GRANT gaps_reader TO current_user;
UPDATE gaps.model SET owner = 'gaps_reader' WHERE name = 'm';
SET ROLE gaps_reader;
SELECT count(*) FROM gaps.marginals('m');                    -- fail: no SELECT on sensor
RESET ROLE;
GRANT SELECT ON sensor TO gaps_reader;
SET ROLE gaps_reader;
SELECT count(*) > 0 AS works FROM gaps.marginals('m');
RESET ROLE;

-- Row-level security on the user's table is honored: hide room 2.
ALTER TABLE sensor ENABLE ROW LEVEL SECURITY;
CREATE POLICY room1 ON sensor TO gaps_reader USING (room = 1);
SET ROLE gaps_reader;
SELECT DISTINCT entity->>'room' AS room FROM gaps.marginals('m');
RESET ROLE;

-- Grounding fails, rather than drop a row, when its statements see
-- different rows, as under READ COMMITTED with a concurrent insert.  A
-- policy evaluated once per statement simulates one: the parent's second
-- row shows only from the second statement that reads the table on, so
-- the mapping reaches a parent row the entity step never saw.
CREATE SEQUENCE public.reads;
GRANT USAGE ON SEQUENCE public.reads TO gaps_reader;
CREATE FUNCTION public.later() RETURNS bool LANGUAGE sql VOLATILE
	SET search_path = public AS $$ SELECT nextval('reads') > 1 $$;
CREATE TABLE par (id int PRIMARY KEY, x text);
INSERT INTO par VALUES (1, 'a'), (2, NULL);
ALTER TABLE par ENABLE ROW LEVEL SECURITY;
CREATE POLICY later ON par TO gaps_reader USING (id = 1 OR (SELECT public.later()));
CREATE TABLE ch (id int PRIMARY KEY, g int, pid int, y text);
INSERT INTO ch VALUES (1, 1, 1, NULL), (2, 1, 2, NULL);
GRANT SELECT ON par, ch TO gaps_reader;
SELECT gaps.create_model('snap');
UPDATE gaps.model SET owner = 'gaps_reader' WHERE name = 'snap';
SELECT gaps.declare_attribute('snap', 'par', 'x', ARRAY['a', 'b']);
SELECT gaps.declare_attribute('snap', 'ch', 'y', ARRAY['c', 'd'], ARRAY['g']::name[]);
SELECT gaps.add_stripe('snap', 'x', 'par', 'x');
SELECT gaps.add_stripe('snap', 'y', 'ch', 'y');
SELECT gaps.add_factor('snap', 'px', 'x', '{}', ARRAY[0.5, 0.5]);
SELECT gaps.add_factor('snap', 'py', 'y', ARRAY['x'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT gaps.map_factor('snap', 'py', 1, 'pid', 'id');
SET ROLE gaps_reader;
SELECT * FROM gaps.marginals('snap');                        -- fail: rows changed
RESET ROLE;
SELECT gaps.drop_model('snap');
DROP TABLE par, ch;
DROP FUNCTION public.later();
DROP SEQUENCE public.reads;

-- The internal entry point validates its arrays.
SELECT * FROM gaps._infer('{2}', '{0}', '{1}', '{1,1}', '{}', '{1}', '{0.5,0.5}');
SELECT * FROM gaps._infer('{2}', '{3}', '{1}', '{1,1}', '{}', '{1}', '{0.5,0.5}');  -- fail
SELECT * FROM gaps._infer('{2}', '{0}', '{1}', '{1,1}', '{}', '{1}', '{0.5}');        -- fail
SELECT * FROM gaps._infer('{2}', '{0}', '{2}', '{1,1}', '{}', '{1}', '{0.5,0.5}');  -- fail
SELECT * FROM gaps._infer('{2}', '{0}', '{1}', '{1,2}', '{1}', '{1}', '{0.5,0.5,0.5,0.5}');   -- fail

DROP TABLE sensor, reading;
SELECT gaps.drop_model('m');
SELECT gaps.drop_model('c');
DROP ROLE gaps_reader;
