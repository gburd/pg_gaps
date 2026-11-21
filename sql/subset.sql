-- Restricted calls: gaps.marginals and gaps.impute given rel and a jsonb
-- array of entity keys ground only the components of those entities and
-- return only their variables, with the probabilities of the unrestricted
-- call (doc/DESIGN.md, "Restricted calls").
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
\set VERBOSITY terse

-- The rows of the unrestricted call for the requested entities, against
-- the restricted call: entities requested but absent on one side, and
-- probabilities that are not bit-identical, count as mismatches.  Also
-- reports how many rows the restricted call returned, so an empty
-- comparison cannot pass unnoticed (AGENTS.md rule 8), and how far the
-- component numbers fail to map one to one (0: two variables share a
-- restricted component exactly when they share an unrestricted one).
CREATE FUNCTION same(mname text, r regclass, keys jsonb,
	OUT rows bigint, OUT mismatched bigint, OUT split bigint)
LANGUAGE plpgsql AS $$
BEGIN
	CREATE TEMP TABLE full_m ON COMMIT DROP AS
		SELECT * FROM gaps.marginals(mname) m
		WHERE m.rel = r AND m.entity IN (SELECT jsonb_array_elements(keys));
	CREATE TEMP TABLE part_m ON COMMIT DROP AS
		SELECT * FROM gaps.marginals(mname, r, keys);
	SELECT count(*) INTO rows FROM part_m;
	SELECT count(*) INTO mismatched
	FROM full_m f FULL JOIN part_m p USING (rel, attname, entity, value)
	WHERE f.probability IS DISTINCT FROM p.probability
	   OR f.method IS DISTINCT FROM p.method;
	-- Component numbers map one to one: as many distinct (full, restricted)
	-- pairs as distinct full components and as distinct restricted ones.
	SELECT count(DISTINCT (f.component, p.component))
		   - least(count(DISTINCT f.component), count(DISTINCT p.component))
	INTO split
	FROM full_m f JOIN part_m p USING (rel, attname, entity, value);
	-- impute, likewise.
	SELECT rows + count(*) INTO rows FROM gaps.impute(mname, r, keys);
	SELECT mismatched + count(*) INTO mismatched
	FROM (SELECT * FROM gaps.impute(mname) i
		  WHERE i.rel = r AND i.entity IN (SELECT jsonb_array_elements(keys))) f
		FULL JOIN gaps.impute(mname, r, keys) p USING (rel, attname, entity)
	WHERE f.value IS DISTINCT FROM p.value OR f.probability IS DISTINCT FROM p.probability;
	DROP TABLE full_m, part_m;
END
$$;

-- Naive Bayes, one component per row: a latent class and two observed
-- children per email.
CREATE TABLE em (num bigint PRIMARY KEY, a text, o1 text, o2 text);
INSERT INTO em SELECT g, NULL, (ARRAY['x', 'y'])[1 + g % 2], (ARRAY['x', 'y'])[1 + g % 3 % 2]
FROM generate_series(1, 50) g;
UPDATE em SET o2 = NULL WHERE num % 7 = 0;
SELECT gaps.create_model('nb');
SELECT gaps.declare_attribute('nb', 'em', 'a', ARRAY['h', 'l']);
SELECT gaps.declare_attribute('nb', 'em', 'o1', ARRAY['x', 'y']);
SELECT gaps.declare_attribute('nb', 'em', 'o2', ARRAY['x', 'y']);
SELECT gaps.add_stripe('nb', 'a', 'em', 'a');
SELECT gaps.add_stripe('nb', 'o1', 'em', 'o1');
SELECT gaps.add_stripe('nb', 'o2', 'em', 'o2');
SELECT gaps.add_factor('nb', 'pa', 'a', '{}', ARRAY[0.7, 0.3]);
SELECT gaps.add_factor('nb', 'p1', 'o1', ARRAY['a'], ARRAY[0.8, 0.2, 0.3, 0.7]);
SELECT gaps.add_factor('nb', 'p2', 'o2', ARRAY['a'], ARRAY[0.6, 0.4, 0.1, 0.9]);
SELECT gaps.map_factor('nb', 'p2', 1, 'num', 'num');
SELECT * FROM same('nb', 'em',
	(SELECT jsonb_agg(jsonb_build_object('num', num)) FROM em WHERE num > 40));
-- Only the requested entities' variables come back, all of rel's
-- attributes: a and the missing o2 of 42 and 49.
SELECT attname, entity, count(*) AS nvalues
FROM gaps.marginals('nb', 'em', '[{"num": 42}, {"num": 43}, {"num": 49}]')
GROUP BY 1, 2 ORDER BY 1, entity::text;
-- A well-formed key that matches no row is no entity: no rows, no error.
SELECT count(*) FROM gaps.marginals('nb', 'em', '[{"num": 1000}]');
SELECT count(*) FROM gaps.marginals('nb', 'em', '[]');
-- Errors at the boundary.
SELECT count(*) FROM gaps.marginals('nb', NULL, '[{"num": 1}]');     -- fail: no rel
SELECT count(*) FROM gaps.marginals('nb', 'em');                     -- fail: no entities
SELECT count(*) FROM gaps.marginals('nb', 'pg_class', '[{"num": 1}]'); -- fail: rel not in model
SELECT count(*) FROM gaps.marginals('nb', 'em', '[{"id": 1}]');      -- fail: wrong key
SELECT count(*) FROM gaps.marginals('nb', 'em', '[{"num": 1, "x": 2}]'); -- fail: extra key
SELECT count(*) FROM gaps.marginals('nb', 'em', '[{"num": "one"}]'); -- fail: does not cast
SELECT count(*) FROM gaps.marginals('nb', 'em', '[1]');              -- fail: not an object
SELECT count(*) FROM gaps.marginals('nb', 'em', '{"num": 1}');       -- fail: not an array

-- Vertical factors: sensor 2's temperature depends on sensor 1's in the
-- same room and time, so requesting a sensor 1 row pulls in its sensor 2
-- partner (and the reverse), and nothing else.
CREATE TABLE sensor (t int, room int, sid int, temp text, light text,
					 PRIMARY KEY (t, room, sid));
INSERT INTO sensor
SELECT t, room, sid,
	   CASE WHEN (t + room + sid) % 3 = 0 THEN 'hot' END,
	   CASE WHEN (t * room + sid) % 4 = 0 THEN 'dark' WHEN sid = 2 THEN 'bright' END
FROM generate_series(1, 5) t, generate_series(1, 3) room, generate_series(1, 2) sid;
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
SELECT * FROM same('m', 'sensor', '[{"t": 1, "room": 1, "sid": 1}, {"t": 4, "room": 2, "sid": 2}]');
-- The restricted grounding held the two requested entities' components,
-- two rows with two attributes each, not the table's sixty variables.
BEGIN;
SELECT count(*) FROM gaps.marginals('m', 'sensor', '[{"t": 1, "room": 1, "sid": 1}]');
SELECT count(*) AS grounded_rvs FROM pg_gaps_rv;
COMMIT;

-- A coarse parent mapped one-to-many: room occupancy per (room, t), over
-- several rows of occ, is the parent of every sensor's temperature in that
-- room and time.  Requesting one sensor row pulls in its occupancy and,
-- through it, every other sensor in the room at that time; requesting an
-- occupancy entity, its rows' shared variable, pulls in the same.
CREATE TABLE occ (room int, t int, k int, occupied text, PRIMARY KEY (room, t, k));
INSERT INTO occ SELECT room, t, k, CASE WHEN (room + t + k) % 5 = 0 THEN 'yes' END
FROM generate_series(1, 3) room, generate_series(1, 5) t, generate_series(1, 2) k;
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
SELECT * FROM same('o', 'sensor', '[{"t": 2, "room": 2, "sid": 1}, {"t": 5, "room": 3, "sid": 2}]');
SELECT * FROM same('o', 'occ', '[{"room": 1, "t": 1}, {"room": 3, "t": 4}]');
-- occ's entity key is (room, t): the row key (room, t, k) names no entity.
SELECT count(*) FROM gaps.marginals('o', 'occ', '[{"room": 1, "t": 1, "k": 1}]'); -- fail
-- One sensor row grounds its room-time: one occupancy and two temperatures.
BEGIN;
SELECT count(*) FROM gaps.marginals('o', 'sensor', '[{"t": 2, "room": 2, "sid": 1}]');
SELECT count(*) AS grounded_rvs FROM pg_gaps_rv;
COMMIT;

-- A coarse child: readings of one sensor at one time share a status, and
-- the status depends on the sensor's (row) health.  A requested health
-- reaches every status of that sensor across times, and one status its
-- health and then the sensor's other statuses.
CREATE TABLE health (sid int PRIMARY KEY, ok text);
INSERT INTO health VALUES (1, NULL), (2, 'good'), (3, NULL);
CREATE TABLE reading (sid int, t int, k int, status text, PRIMARY KEY (sid, t, k));
INSERT INTO reading SELECT sid, t, k, CASE WHEN (sid * t + k) % 4 = 0 THEN 'up' END
FROM generate_series(1, 3) sid, generate_series(1, 4) t, generate_series(1, 2) k;
SELECT gaps.create_model('c');
SELECT gaps.declare_attribute('c', 'health', 'ok', ARRAY['good', 'bad']);
SELECT gaps.declare_attribute('c', 'reading', 'status', ARRAY['up', 'down'],
							  ARRAY['sid', 't']::name[]);
SELECT gaps.add_stripe('c', 'ok', 'health', 'ok');
SELECT gaps.add_stripe('c', 'st', 'reading', 'status');
SELECT gaps.add_factor('c', 'ok', 'ok', '{}', ARRAY[0.8, 0.2]);
SELECT gaps.add_factor('c', 'st', 'st', ARRAY['ok'], ARRAY[0.9, 0.1, 0.3, 0.7]);
SELECT gaps.map_factor('c', 'st', 1, 'sid', 'sid');
SELECT * FROM same('c', 'health', '[{"sid": 1}, {"sid": 2}]');
SELECT * FROM same('c', 'reading', '[{"sid": 3, "t": 3}, {"sid": 1, "t": 4}]');
BEGIN;
SELECT count(*) FROM gaps.marginals('c', 'reading', '[{"sid": 3, "t": 3}]');
SELECT count(*) AS grounded_rvs FROM pg_gaps_rv;
COMMIT;

-- Random models (AGENTS.md rule 13): restricted equals unrestricted on
-- every one.  Over table rnd, x is keyed by id or, coarse, by grp; y by id
-- with parent x (by grp when x is coarse) and maybe z; z by id, either
-- with a prior or as chains of three rows (pos 0 a prior, pos 1 and 2 a
-- CPT on the row before), so components span rows through vertical
-- factors, coarse entities and one-to-many mappings.  Each model is asked
-- about a random subset of row and group entities.
CREATE TABLE rnd (id int PRIMARY KEY, grp int, pos int, prev int, x text, y text, z text);
CREATE TABLE rnd_out (rows bigint, mismatched bigint, split bigint);
CREATE FUNCTION pg_temp.cpt(n int) RETURNS float8[] LANGUAGE sql AS $$
	SELECT array_agg(v ORDER BY r, c) FROM (
		SELECT r, c, w / sum(w) OVER (PARTITION BY r) AS v
		FROM (SELECT r, c, 0.05 + random() AS w
			  FROM generate_series(1, n) r, generate_series(1, 2) c) a) b
$$;
CREATE FUNCTION pg_temp.random_models(n int) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
	coarse boolean;
	yz boolean;
	keys jsonb;
BEGIN
	FOR iter IN 1 .. n LOOP
		TRUNCATE rnd;
		INSERT INTO rnd
		SELECT i, i / (2 + floor(random() * 3)::int), i % 3, i - 1,
			   CASE WHEN random() < 0.4 THEN 'p' END,
			   CASE WHEN random() < 0.5 THEN (ARRAY['p', 'q'])[1 + floor(random() * 2)::int] END,
			   CASE WHEN random() < 0.5 THEN (ARRAY['p', 'q'])[1 + floor(random() * 2)::int] END
		FROM generate_series(0, 5 + floor(random() * 15)::int) i;
		coarse := random() < 0.5;
		yz := random() < 0.5;
		PERFORM gaps.create_model('rnd');
		PERFORM gaps.declare_attribute('rnd', 'rnd', 'x', ARRAY['p', 'q'],
									   CASE WHEN coarse THEN ARRAY['grp']::name[] END);
		PERFORM gaps.declare_attribute('rnd', 'rnd', 'y', ARRAY['p', 'q']);
		PERFORM gaps.declare_attribute('rnd', 'rnd', 'z', ARRAY['p', 'q']);
		PERFORM gaps.add_stripe('rnd', 'x', 'rnd', 'x');
		PERFORM gaps.add_stripe('rnd', 'y', 'rnd', 'y');
		PERFORM gaps.add_stripe('rnd', 'z', 'rnd', 'z');
		PERFORM gaps.add_factor('rnd', 'x', 'x', '{}', pg_temp.cpt(1));
		IF yz THEN
			PERFORM gaps.add_factor('rnd', 'y', 'y', ARRAY['x', 'z'], pg_temp.cpt(4));
		ELSE
			PERFORM gaps.add_factor('rnd', 'y', 'y', ARRAY['x'], pg_temp.cpt(2));
		END IF;
		IF coarse THEN
			PERFORM gaps.map_factor('rnd', 'y', 1, 'grp', 'grp');
		END IF;
		IF random() < 0.5 THEN
			PERFORM gaps.add_factor('rnd', 'z', 'z', '{}', pg_temp.cpt(1));
		ELSE
			PERFORM gaps.add_stripe('rnd', 'z0', 'rnd', 'z');
			PERFORM gaps.add_stripe_qual('rnd', 'z0', 'pos', '=', '0');
			PERFORM gaps.add_stripe('rnd', 'zr', 'rnd', 'z');
			PERFORM gaps.add_stripe_qual('rnd', 'zr', 'pos', '>', '0');
			PERFORM gaps.add_factor('rnd', 'z0', 'z0', '{}', pg_temp.cpt(1));
			PERFORM gaps.add_factor('rnd', 'zr', 'zr', ARRAY['z'], pg_temp.cpt(2));
			PERFORM gaps.map_factor('rnd', 'zr', 1, 'prev', 'id');
		END IF;
		SELECT jsonb_agg(e) INTO keys FROM (
			SELECT jsonb_build_object('id', id) AS e FROM rnd WHERE random() < 0.15
			UNION ALL
			SELECT DISTINCT jsonb_build_object('grp', grp) FROM rnd
			WHERE coarse AND random() < 0.15) k;
		INSERT INTO rnd_out SELECT * FROM same('rnd', 'rnd', coalesce(keys, '[]'));
		PERFORM gaps.drop_model('rnd');
	END LOOP;
END
$$;
SELECT setseed(0.25);
SELECT pg_temp.random_models(150);
SELECT count(*) AS models, count(*) FILTER (WHERE rows > 0) AS compared,
	   sum(mismatched) AS mismatched, sum(split) AS split FROM rnd_out;
DROP TABLE rnd, rnd_out;

-- learn_cpts is not restricted: it takes no entities.
SELECT count(*) FROM gaps.learn_cpts('c');

SELECT gaps.drop_model('nb');
SELECT gaps.drop_model('m');
SELECT gaps.drop_model('o');
SELECT gaps.drop_model('c');
DROP TABLE em, sensor, occ, health, reading;
DROP FUNCTION same;
