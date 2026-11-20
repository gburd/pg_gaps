-- The structural validity check of [W11 3.3.2]: gaps.validate decides from
-- the catalog alone whether a model's ground network is acyclic, and
-- grounding (gx_check) stays the exact authority.  Every model below is
-- also grounded, to show what validate's verdict means.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
\set VERBOSITY terse

-- The sensor relation of Fig. 3.3, with a deterministic column loc that
-- is not part of the key, and tm1 = t - 1 for mapping to the previous time.
CREATE TABLE sensor1 (t int, r int, sid int, loc text, tp text, l text,
					  tm1 int GENERATED ALWAYS AS (t - 1) STORED,
					  PRIMARY KEY (t, r, sid));
INSERT INTO sensor1 (t, r, sid, loc, tp, l) VALUES
	(1, 1, 1, 'a', NULL,  'dark'),
	(1, 1, 2, 'b', NULL,  NULL),
	(2, 1, 1, 'a', 'hot', NULL),
	(2, 1, 2, 'b', NULL,  'bright');

-- Fig. 3.5 (T2427-2478): phi1(S1) over Tp where Sid <> 2, phi2(S3 | S2)
-- from Tp to L on every row, phi3(S5 | S4) from Sid 1's Tp to Sid 2's at
-- the same time and room.  S5 and S4 are on the same attribute but cannot
-- meet, so the precondition holds, and the graph is acyclic: valid.
SELECT gaps.create_model('fig35');
SELECT gaps.declare_attribute('fig35', 'sensor1', 'tp', ARRAY['cold', 'hot']);
SELECT gaps.declare_attribute('fig35', 'sensor1', 'l', ARRAY['dark', 'bright']);
SELECT gaps.add_stripe('fig35', 's1', 'sensor1', 'tp');
SELECT gaps.add_stripe_qual('fig35', 's1', 'sid', '<>', '2');
SELECT gaps.add_stripe('fig35', 's2', 'sensor1', 'tp');
SELECT gaps.add_stripe('fig35', 's3', 'sensor1', 'l');
SELECT gaps.add_stripe('fig35', 's4', 'sensor1', 'tp');
SELECT gaps.add_stripe_qual('fig35', 's4', 'sid', '=', '1');
SELECT gaps.add_stripe('fig35', 's5', 'sensor1', 'tp');
SELECT gaps.add_stripe_qual('fig35', 's5', 'sid', '=', '2');
SELECT gaps.add_factor('fig35', 'phi1', 's1', '{}', ARRAY[0.3, 0.7]);
SELECT gaps.add_factor('fig35', 'phi2', 's3', ARRAY['s2'], ARRAY[0.75, 0.25, 0.4, 0.6]);
SELECT gaps.add_factor('fig35', 'phi3', 's5', ARRAY['s4'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT gaps.map_factor('fig35', 'phi3', 1, 't', 't');
SELECT gaps.map_factor('fig35', 'phi3', 1, 'r', 'r');
SELECT * FROM gaps.validate('fig35');
SELECT count(*) AS marginals FROM gaps.marginals('fig35');

-- A structural cycle that grounds fine: Sid 1 at time t depends on Sid 2
-- at time t - 1, and Sid 2 at time t on Sid 1 at time t.  The two factors
-- feed each other in the graph, but the lineage moves forward in time.
SELECT gaps.create_model('zigzag');
SELECT gaps.declare_attribute('zigzag', 'sensor1', 'tp', ARRAY['cold', 'hot']);
SELECT gaps.add_stripe('zigzag', 'a1', 'sensor1', 'tp');
SELECT gaps.add_stripe_qual('zigzag', 'a1', 'sid', '=', '1');
SELECT gaps.add_stripe_qual('zigzag', 'a1', 't', '=', '1');
SELECT gaps.add_stripe('zigzag', 'a', 'sensor1', 'tp');
SELECT gaps.add_stripe_qual('zigzag', 'a', 'sid', '=', '1');
SELECT gaps.add_stripe('zigzag', 'later', 'sensor1', 'tp');
SELECT gaps.add_stripe_qual('zigzag', 'later', 'sid', '=', '1');
SELECT gaps.add_stripe_qual('zigzag', 'later', 't', '>', '1');
SELECT gaps.add_stripe('zigzag', 'b', 'sensor1', 'tp');
SELECT gaps.add_stripe_qual('zigzag', 'b', 'sid', '=', '2');
SELECT gaps.add_factor('zigzag', 'first', 'a1', '{}', ARRAY[0.5, 0.5]);
SELECT gaps.add_factor('zigzag', 'ab', 'later', ARRAY['b'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT gaps.map_factor('zigzag', 'ab', 1, 'tm1', 't');
SELECT gaps.map_factor('zigzag', 'ab', 1, 'r', 'r');
SELECT gaps.add_factor('zigzag', 'ba', 'b', ARRAY['a'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT gaps.map_factor('zigzag', 'ba', 1, 't', 't');
SELECT gaps.map_factor('zigzag', 'ba', 1, 'r', 'r');
SELECT * FROM gaps.validate('zigzag');
SELECT count(*) AS marginals FROM gaps.marginals('zigzag');

-- A structural cycle that is a ground cycle: Sid 1 and Sid 2 at the same
-- time and room are each other's parent.
SELECT gaps.create_model('cyc');
SELECT gaps.declare_attribute('cyc', 'sensor1', 'tp', ARRAY['cold', 'hot']);
SELECT gaps.add_stripe('cyc', 'a', 'sensor1', 'tp');
SELECT gaps.add_stripe_qual('cyc', 'a', 'sid', '=', '1');
SELECT gaps.add_stripe('cyc', 'b', 'sensor1', 'tp');
SELECT gaps.add_stripe_qual('cyc', 'b', 'sid', '=', '2');
SELECT gaps.add_factor('cyc', 'ab', 'a', ARRAY['b'], ARRAY[0.5, 0.5, 0.5, 0.5]);
SELECT gaps.map_factor('cyc', 'ab', 1, 't', 't');
SELECT gaps.map_factor('cyc', 'ab', 1, 'r', 'r');
SELECT gaps.add_factor('cyc', 'ba', 'b', ARRAY['a'], ARRAY[0.5, 0.5, 0.5, 0.5]);
SELECT gaps.map_factor('cyc', 'ba', 1, 't', 't');
SELECT gaps.map_factor('cyc', 'ba', 1, 'r', 'r');
SELECT * FROM gaps.validate('cyc');
SELECT * FROM gaps.marginals('cyc');                         -- fail: cycle

-- The precondition fails, harmlessly: Tp at time t > 1 depends on Tp at
-- time t - 1, and the child and parent stripes overlap at the times
-- between.  validate cannot prove the model; it grounds fine.
SELECT gaps.create_model('chain');
SELECT gaps.declare_attribute('chain', 'sensor1', 'tp', ARRAY['cold', 'hot']);
SELECT gaps.add_stripe('chain', 'start', 'sensor1', 'tp');
SELECT gaps.add_stripe_qual('chain', 'start', 't', '=', '1');
SELECT gaps.add_stripe('chain', 'rest', 'sensor1', 'tp');
SELECT gaps.add_stripe_qual('chain', 'rest', 't', '>', '1');
SELECT gaps.add_stripe('chain', 'prev', 'sensor1', 'tp');
SELECT gaps.add_stripe_qual('chain', 'prev', 't', '<', '2');
SELECT gaps.add_factor('chain', 'p0', 'start', '{}', ARRAY[0.5, 0.5]);
SELECT gaps.add_factor('chain', 'step', 'rest', ARRAY['prev'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT gaps.map_factor('chain', 'step', 1, 'tm1', 't');
SELECT gaps.map_factor('chain', 'step', 1, 'r', 'r');
SELECT gaps.map_factor('chain', 'step', 1, 'sid', 'sid');
SELECT * FROM gaps.validate('chain');
SELECT count(*) AS marginals FROM gaps.marginals('chain');
-- prev is only t = 1 here, but the test does not see the gap between
-- t > 1 and t < 2 over int.  Written as t <= 1 it does, and the model is
-- proved.
DELETE FROM gaps.stripe_qual q USING gaps.stripe s
WHERE s.stripe_id = q.stripe_id AND s.name = 'prev';
SELECT gaps.add_stripe_qual('chain', 'prev', 't', '<=', '1');
SELECT * FROM gaps.validate('chain');
SELECT count(*) AS marginals FROM gaps.marginals('chain');

-- The precondition fails, and it is a ground cycle: every Tp is its own
-- parent through a second stripe over the same entities.
SELECT gaps.create_model('self');
SELECT gaps.declare_attribute('self', 'sensor1', 'tp', ARRAY['cold', 'hot']);
SELECT gaps.add_stripe('self', 'tp', 'sensor1', 'tp');
SELECT gaps.add_stripe('self', 'tp_too', 'sensor1', 'tp');
SELECT gaps.add_factor('self', 'loop', 'tp', ARRAY['tp_too'], ARRAY[0.5, 0.5, 0.5, 0.5]);
SELECT * FROM gaps.validate('self');
SELECT * FROM gaps.marginals('self');                        -- fail: cycle

-- The overlap test, one stripe pair per row: whether the predicates alone
-- leave room for a shared RV.  A false is a proof; a true is "may".
SELECT gaps.create_model('ops');
SELECT gaps.declare_attribute('ops', 'sensor1', 'tp', ARRAY['cold', 'hot']);
SELECT gaps.declare_attribute('ops', 'sensor1', 'l', ARRAY['dark', 'bright']);
CREATE FUNCTION pg_temp.stripe(name text, att name, quals text[]) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
	PERFORM gaps.add_stripe('ops', name, 'sensor1', att);
	FOR i IN 1 .. coalesce(array_length(quals, 2), 0) LOOP
		PERFORM gaps.add_stripe_qual('ops', name, quals[1][i], quals[2][i], quals[3][i]);
	END LOOP;
END
$$;
SELECT pg_temp.stripe(n, a::name, q::text[]) FROM (VALUES
	('eq1', 'tp', '{{sid},{=},{1}}'), ('eq1b', 'tp', '{{sid},{=},{1}}'),
	('eq2', 'tp', '{{sid},{=},{2}}'), ('eq3', 'tp', '{{sid},{=},{3}}'),
	('ne1', 'tp', '{{sid},{<>},{1}}'), ('ne2', 'tp', '{{sid},{<>},{2}}'),
	('lt1', 'tp', '{{sid},{<},{1}}'), ('le1', 'tp', '{{sid},{<=},{1}}'),
	('le2', 'tp', '{{sid},{<=},{2}}'), ('gt1', 'tp', '{{sid},{>},{1}}'),
	('ge1', 'tp', '{{sid},{>=},{1}}'), ('gap', 'tp', '{{sid,sid},{>,<},{1,2}}'),
	('empty', 'tp', '{{t,t},{>,<},{5,3}}'), ('all', 'tp', '{}'),
	('loc_b', 'tp', '{{loc},{=},{b}}'), ('loc_a', 'tp', '{{loc},{<},{a}}'),
	('loc_ab', 'tp', '{{loc},{>},{ab}}'), ('t1', 'tp', '{{t},{=},{1}}'),
	('sid1_not_t1', 'tp', '{{sid,t},{=,<>},{1,1}}'), ('l_eq1', 'l', '{{sid},{=},{1}}'))
	v(n, a, q);
SELECT a, b, gaps._may_share_rvs(gaps._stripe_id('ops', a), gaps._stripe_id('ops', b))
	AS may_share
FROM (VALUES ('eq1', 'eq2'), ('eq1', 'eq1b'), ('eq1', 'ne1'), ('ne1', 'ne2'),
			 ('lt1', 'ge1'), ('le1', 'ge1'), ('le1', 'gt1'), ('lt1', 'gt1'),
			 ('eq3', 'le2'), ('eq2', 'le2'), ('eq1', 'gap'), ('gap', 'all'),
			 ('empty', 'all'), ('loc_b', 'loc_a'), ('loc_b', 'loc_ab'),
			 ('t1', 'sid1_not_t1'), ('eq1', 'l_eq1')) v(a, b);

-- A coarse entity is several rows, so a qual on a column outside its keys
-- does not split it: rows k = 1 and k = 2 of one (sid, t) are one RV.
CREATE TABLE reading (sid int, t int, k int, status text, PRIMARY KEY (sid, t, k));
SELECT gaps.create_model('coarse');
SELECT gaps.declare_attribute('coarse', 'reading', 'status', ARRAY['up', 'down'],
							  ARRAY['sid', 't']::name[]);
SELECT gaps.add_stripe('coarse', 'k1', 'reading', 'status');
SELECT gaps.add_stripe_qual('coarse', 'k1', 'k', '=', '1');
SELECT gaps.add_stripe_qual('coarse', 'k1', 'sid', '=', '1');
SELECT gaps.add_stripe('coarse', 'k2', 'reading', 'status');
SELECT gaps.add_stripe_qual('coarse', 'k2', 'k', '=', '2');
SELECT gaps.add_stripe('coarse', 'k2s2', 'reading', 'status');
SELECT gaps.add_stripe_qual('coarse', 'k2s2', 'k', '=', '2');
SELECT gaps.add_stripe_qual('coarse', 'k2s2', 'sid', '=', '2');
SELECT gaps._may_share_rvs(gaps._stripe_id('coarse', 'k1'), gaps._stripe_id('coarse', 'k2'))
	AS k1_k2,
	   gaps._may_share_rvs(gaps._stripe_id('coarse', 'k1'), gaps._stripe_id('coarse', 'k2s2'))
	AS k1_k2s2;

-- Soundness against grounding, over random models (AGENTS.md rule 13):
-- whenever validate proves a model, grounding finds no cycle.  Each model
-- has a Tp factor per sid, with random parents among the sids at the same
-- time; each stripe's sid is written one of several ways, some of which
-- the overlap test cannot see through, so all three outcomes occur.
CREATE TABLE soundness (proved boolean, ground_cycle boolean);
DO $$
DECLARE
	forms text[] := ARRAY['=', '><', '>=<=', '=<>'];
	proved boolean;
	cyc boolean;
	np int;
	ps text[];
	qual text;
BEGIN
	PERFORM setseed(0.5);
	DELETE FROM sensor1;
	INSERT INTO sensor1 (t, r, sid, loc) SELECT t, 1, s, 'x'
	FROM generate_series(1, 2) t, generate_series(1, 3) s;
	FOR iter IN 1 .. 300 LOOP
		PERFORM gaps.create_model('rnd');
		PERFORM gaps.declare_attribute('rnd', 'sensor1', 'tp', ARRAY['cold', 'hot']);
		-- Stripe "c<k>" is the child for sid k, "p<k>" a parent stripe for it.
		FOR k IN 1 .. 3 LOOP
			FOREACH qual IN ARRAY ARRAY['c', 'p'] LOOP
				PERFORM gaps.add_stripe('rnd', qual || k, 'sensor1', 'tp');
				CASE forms[1 + floor(random() * 4)::int]
					WHEN '=' THEN
						PERFORM gaps.add_stripe_qual('rnd', qual || k, 'sid', '=', k::text);
					WHEN '><' THEN
						PERFORM gaps.add_stripe_qual('rnd', qual || k, 'sid', '>', (k - 1)::text);
						PERFORM gaps.add_stripe_qual('rnd', qual || k, 'sid', '<', (k + 1)::text);
					WHEN '>=<=' THEN
						PERFORM gaps.add_stripe_qual('rnd', qual || k, 'sid', '>=', k::text);
						PERFORM gaps.add_stripe_qual('rnd', qual || k, 'sid', '<=', k::text);
					ELSE
						PERFORM gaps.add_stripe_qual('rnd', qual || k, 'sid', '=', k::text);
						PERFORM gaps.add_stripe_qual('rnd', qual || k, 'sid', '<>',
													 (1 + (k + floor(random() * 2)::int) % 3)::text);
				END CASE;
			END LOOP;
		END LOOP;
		FOR k IN 1 .. 3 LOOP
			np := floor(random() * 3)::int;
			ps := ARRAY(SELECT 'p' || x FROM generate_series(1, 3) x
						ORDER BY random() LIMIT np);
			PERFORM gaps.add_factor('rnd', 'f' || k, 'c' || k, ps,
									array_fill(0.5::float8, ARRAY[2 << np]));
			FOR o IN 1 .. np LOOP
				PERFORM gaps.map_factor('rnd', 'f' || k, o, 't', 't');
			END LOOP;
		END LOOP;
		SELECT bool_and(v.ok) INTO proved FROM gaps.validate('rnd') v;
		BEGIN
			PERFORM count(*) FROM gaps.marginals('rnd');
			cyc := false;
		EXCEPTION WHEN invalid_parameter_value THEN
			IF SQLERRM NOT LIKE 'the ground network has a directed cycle%' THEN
				RAISE;
			END IF;
			cyc := true;
		END;
		INSERT INTO soundness VALUES (proved, cyc);
		PERFORM gaps.drop_model('rnd');
	END LOOP;
END
$$;
SELECT proved, ground_cycle, count(*) > 0 AS seen FROM soundness
GROUP BY 1, 2 ORDER BY 1, 2;
SELECT count(*) AS unsound FROM soundness WHERE proved AND ground_cycle;

-- validate is bounded.
DO $$
BEGIN
	PERFORM gaps.create_model('big');
	PERFORM gaps.declare_attribute('big', 'sensor1', 'tp', ARRAY['cold', 'hot']);
	PERFORM gaps.add_stripe('big', 's', 'sensor1', 'tp');
	FOR i IN 1 .. 101 LOOP
		PERFORM gaps.add_factor('big', 'f' || i, 's', '{}', ARRAY[0.5, 0.5]);
	END LOOP;
END
$$;
SELECT * FROM gaps.validate('big');                          -- fail: too many factors
SELECT * FROM gaps.validate('nosuch');                       -- fail: no model

DROP TABLE sensor1, reading, soundness;
SELECT gaps.drop_model(name) FROM gaps.model ORDER BY name;
