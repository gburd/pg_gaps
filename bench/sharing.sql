-- bench/sharing.sql: first-order sharing on the hotel sensor workload
-- of the BayesStore thesis section 4.5.1, minus its two vertical factors
-- (which Phase 3 adds): 100 rooms x 8 sensors x T time steps, a prior on
-- temperature and a CPT of light given temperature shared by every
-- sensor, with mratio of the values missing.
--
-- Usage: psql -X -v T=500 -v mratio=0.1 -f bench/sharing.sql
--
-- Prints the number of variables and components, the cache hits, and the
-- time of gaps.marginals, run five times in one session (rule 4: timing
-- inside one session, take the median).  The engine's sharing cache is
-- per call, so every run pays for it again.
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
SELECT gaps.add_stripe('hotel', 'l', 'hotel', 'light');
SELECT gaps.add_factor('hotel', 'prior', 'tp', '{}', ARRAY[0.4, 0.6]);
SELECT gaps.add_factor('hotel', 'light', 'l', ARRAY['tp'], ARRAY[0.75, 0.25, 0.4, 0.6]);

SELECT count(*) AS rows, count(*) FILTER (WHERE temp IS NULL) + count(*) FILTER (WHERE light IS NULL) AS missing
FROM hotel;
\timing on
SELECT count(*) FROM gaps.marginals('hotel');
SELECT count(*) FROM gaps.marginals('hotel');
SELECT count(*) FROM gaps.marginals('hotel');
SELECT count(*) FROM gaps.marginals('hotel');
SELECT count(*) FROM gaps.marginals('hotel');
\timing off
