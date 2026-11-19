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
