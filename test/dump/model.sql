-- The model test/dump/run.sh dumps and restores.  It exercises every
-- catalog table: stripe predicates, a vertical factor with a mapping, a
-- horizontal factor, a coarse entity set, a prior and learned CPTs.
SET client_min_messages = warning;
CREATE EXTENSION pg_gaps;

CREATE TABLE sensor (t int, room int, sid int, temp text, light text,
					 PRIMARY KEY (t, room, sid));
INSERT INTO sensor VALUES
	(1, 1, 1, NULL, 'dark'), (1, 1, 2, NULL, NULL), (1, 2, 1, 'hot', NULL),
	(1, 2, 2, NULL, 'bright'), (2, 1, 1, NULL, NULL), (2, 1, 2, 'cold', 'dark'),
	(2, 2, 1, 'cold', 'dark'), (2, 2, 2, 'cold', NULL);
CREATE TABLE room (room int, t int, k int, occupied text, PRIMARY KEY (room, t, k));
INSERT INTO room VALUES (1, 1, 1, 'yes'), (1, 1, 2, NULL), (2, 1, 1, NULL), (2, 2, 1, 'no');

SELECT gaps.create_model('dump');
SELECT gaps.declare_attribute('dump', 'sensor', 'temp', ARRAY['cold', 'hot']);
SELECT gaps.declare_attribute('dump', 'sensor', 'light', ARRAY['dark', 'bright']);
SELECT gaps.declare_attribute('dump', 'room', 'occupied', ARRAY['yes', 'no'],
							  ARRAY['room', 't']::name[]);
SELECT gaps.add_stripe('dump', 'tp1', 'sensor', 'temp');
SELECT gaps.add_stripe_qual('dump', 'tp1', 'sid', '=', '1');
SELECT gaps.add_stripe('dump', 'tp2', 'sensor', 'temp');
SELECT gaps.add_stripe_qual('dump', 'tp2', 'sid', '<>', '1');
SELECT gaps.add_stripe('dump', 'tp', 'sensor', 'temp');
SELECT gaps.add_stripe('dump', 'l', 'sensor', 'light');
SELECT gaps.add_stripe('dump', 'occ', 'room', 'occupied');
SELECT gaps.add_factor('dump', 'prior1', 'tp1', '{}', ARRAY[0.3, 0.7]);
SELECT gaps.add_factor('dump', 'copy', 'tp2', ARRAY['tp1'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT gaps.map_factor('dump', 'copy', 1, 't', 't');
SELECT gaps.map_factor('dump', 'copy', 1, 'room', 'room');
SELECT gaps.add_factor('dump', 'light', 'l', ARRAY['tp']);
SELECT gaps.add_factor('dump', 'occ', 'occ', '{}');
SELECT gaps.set_prior('dump', 'light', ARRAY[2, 0.5, 0.25, 3]);
SELECT * FROM gaps.learn_cpts('dump');
