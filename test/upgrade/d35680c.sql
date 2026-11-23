-- Models test/upgrade/run.sh builds under the pre-release d35680c, with the
-- features it has: plain CPT models, stripe predicates, a vertical factor
-- with a mapping, a coarse entity set, given and learned CPTs.
SET client_min_messages = warning;

CREATE TABLE sensor (t int, room int, sid int, temp text, light text,
					 PRIMARY KEY (t, room, sid));
INSERT INTO sensor VALUES
	(1, 1, 1, NULL, 'dark'), (1, 1, 2, NULL, NULL), (1, 2, 1, 'hot', NULL),
	(1, 2, 2, NULL, 'bright'), (2, 1, 1, NULL, NULL), (2, 1, 2, 'cold', 'dark'),
	(2, 2, 1, 'cold', 'dark'), (2, 2, 2, 'cold', NULL);
CREATE TABLE room (room int, t int, k int, occupied text, PRIMARY KEY (room, t, k));
INSERT INTO room VALUES (1, 1, 1, 'yes'), (1, 1, 2, NULL), (2, 1, 1, NULL), (2, 2, 1, 'no');

SELECT gaps.create_model('given');
SELECT gaps.declare_attribute('given', 'sensor', 'temp', ARRAY['cold', 'hot']);
SELECT gaps.declare_attribute('given', 'sensor', 'light', ARRAY['dark', 'bright']);
SELECT gaps.add_stripe('given', 'tp1', 'sensor', 'temp');
SELECT gaps.add_stripe_qual('given', 'tp1', 'sid', '=', '1');
SELECT gaps.add_stripe('given', 'tp2', 'sensor', 'temp');
SELECT gaps.add_stripe_qual('given', 'tp2', 'sid', '<>', '1');
SELECT gaps.add_stripe('given', 'tp', 'sensor', 'temp');
SELECT gaps.add_stripe('given', 'l', 'sensor', 'light');
SELECT gaps.add_factor('given', 'prior1', 'tp1', '{}', ARRAY[0.3, 0.7]);
SELECT gaps.add_factor('given', 'copy', 'tp2', ARRAY['tp1'], ARRAY[0.9, 0.1, 0.2, 0.8]);
SELECT gaps.map_factor('given', 'copy', 1, 't', 't');
SELECT gaps.map_factor('given', 'copy', 1, 'room', 'room');
SELECT gaps.add_factor('given', 'light', 'l', ARRAY['tp'], ARRAY[0.75, 0.25, 0.4, 0.6]);

SELECT gaps.create_model('learned');
SELECT gaps.declare_attribute('learned', 'sensor', 'temp', ARRAY['cold', 'hot']);
SELECT gaps.declare_attribute('learned', 'sensor', 'light', ARRAY['dark', 'bright']);
SELECT gaps.declare_attribute('learned', 'room', 'occupied', ARRAY['yes', 'no'],
							  ARRAY['room', 't']::name[]);
SELECT gaps.add_stripe('learned', 'tp', 'sensor', 'temp');
SELECT gaps.add_stripe('learned', 'l', 'sensor', 'light');
SELECT gaps.add_stripe('learned', 'occ', 'room', 'occupied');
SELECT gaps.add_factor('learned', 'temp', 'tp', '{}');
SELECT gaps.add_factor('learned', 'light', 'l', ARRAY['tp']);
SELECT gaps.add_factor('learned', 'occ', 'occ', '{}');
SELECT * FROM gaps.learn_cpts('learned');
