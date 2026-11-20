-- bench/hotel.sql: the full hotel sensor workload of the BayesStore thesis
-- section 4.5.1 (T3259-3269), with its two vertical factors: 100 rooms x 8
-- sensors x T time steps; a temperature prior for sensors 1, 3 and 5-8;
-- sensor 2's temperature given sensor 1's, and sensor 4's given sensor
-- 3's, in the same room at the same time; and light given temperature for
-- every sensor.  mratio of the values are missing.
--
-- Usage: psql -X -v T=125 -v mratio=0.1 -f bench/hotel.sql
--
-- Prints the variables, the methods that answered and the component
-- sizes, then times gaps.marginals five times in one session (rule 4:
-- timing inside one session, take the median).
\set ON_ERROR_STOP 1
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_gaps;
DROP TABLE IF EXISTS hotel;
SELECT gaps.drop_model(name) FROM gaps.model WHERE name = 'hotel';
CREATE TABLE hotel (t int, room int, sid int, temp text, light text,
					PRIMARY KEY (t, room, sid));
SELECT setseed(0.42);
INSERT INTO hotel
SELECT t, room, sid,
	   CASE WHEN random() < :mratio THEN NULL
			WHEN random() < 0.4 THEN 'cold' ELSE 'hot' END,
	   CASE WHEN random() < :mratio THEN NULL
			WHEN random() < 0.5 THEN 'dark' ELSE 'bright' END
FROM generate_series(1, :T) t, generate_series(1, 100) room, generate_series(1, 8) sid;
ANALYZE hotel;

SELECT gaps.create_model('hotel');
SELECT gaps.declare_attribute('hotel', 'hotel', 'temp', ARRAY['cold', 'hot']);
SELECT gaps.declare_attribute('hotel', 'hotel', 'light', ARRAY['dark', 'bright']);
SELECT gaps.add_stripe('hotel', 'tp', 'hotel', 'temp');
SELECT gaps.add_stripe_qual('hotel', 'tp', 'sid', '<>', '2');
SELECT gaps.add_stripe_qual('hotel', 'tp', 'sid', '<>', '4');
SELECT gaps.add_stripe('hotel', 'tp1', 'hotel', 'temp');
SELECT gaps.add_stripe_qual('hotel', 'tp1', 'sid', '=', '1');
SELECT gaps.add_stripe('hotel', 'tp2', 'hotel', 'temp');
SELECT gaps.add_stripe_qual('hotel', 'tp2', 'sid', '=', '2');
SELECT gaps.add_stripe('hotel', 'tp3', 'hotel', 'temp');
SELECT gaps.add_stripe_qual('hotel', 'tp3', 'sid', '=', '3');
SELECT gaps.add_stripe('hotel', 'tp4', 'hotel', 'temp');
SELECT gaps.add_stripe_qual('hotel', 'tp4', 'sid', '=', '4');
SELECT gaps.add_stripe('hotel', 'tpall', 'hotel', 'temp');
SELECT gaps.add_stripe('hotel', 'l', 'hotel', 'light');
SELECT gaps.add_factor('hotel', 'prior', 'tp', '{}', ARRAY[0.4, 0.6]);
SELECT gaps.add_factor('hotel', 'tp2_tp1', 'tp2', ARRAY['tp1'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT gaps.map_factor('hotel', 'tp2_tp1', 1, 't', 't');
SELECT gaps.map_factor('hotel', 'tp2_tp1', 1, 'room', 'room');
SELECT gaps.add_factor('hotel', 'tp4_tp3', 'tp4', ARRAY['tp3'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT gaps.map_factor('hotel', 'tp4_tp3', 1, 't', 't');
SELECT gaps.map_factor('hotel', 'tp4_tp3', 1, 'room', 'room');
SELECT gaps.add_factor('hotel', 'light', 'l', ARRAY['tpall'], ARRAY[0.75, 0.25, 0.4, 0.6]);

SELECT count(*) AS rows, count(*) FILTER (WHERE temp IS NULL) + count(*) FILTER (WHERE light IS NULL) AS missing
FROM hotel;
SELECT method, max(n) AS largest_component, count(*) AS components
FROM (SELECT method, component, count(DISTINCT (attname, entity)) AS n
	  FROM gaps.marginals('hotel') GROUP BY 1, 2) c
GROUP BY method;
\timing on
SELECT count(*) FROM gaps.marginals('hotel');
SELECT count(*) FROM gaps.marginals('hotel');
SELECT count(*) FROM gaps.marginals('hotel');
SELECT count(*) FROM gaps.marginals('hotel');
SELECT count(*) FROM gaps.marginals('hotel');
\timing off
