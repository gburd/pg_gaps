-- Models test/upgrade/run.sh builds under the released 0.1.0, with the
-- features it adds to d35680c's: a prior, a binned attribute, EM, a
-- composite model with an aggregating noisy-OR parent, and a logistic
-- factor.  d35680c.sql runs first, so the plain models are there too.
SET client_min_messages = warning;

CREATE TABLE reading (id int PRIMARY KEY, lux numeric);
INSERT INTO reading VALUES (1, 0.5), (2, 10), (3, NULL), (4, 250), (5, 'NaN');
SELECT gaps.create_model('binned');
SELECT gaps.declare_attribute('binned', 'reading', 'lux', ARRAY['dim', 'lit', 'bright'],
							  edges => ARRAY[10, 100]);
SELECT gaps.add_stripe('binned', 'lux', 'reading', 'lux');
SELECT gaps.add_factor('binned', 'lux', 'lux', '{}');
SELECT gaps.set_prior('learned', 'light', ARRAY[2, 0.5, 0.25, 3]);
SELECT * FROM gaps.learn_cpts('learned', method => 'em');
SELECT * FROM gaps.learn_cpts('binned');

CREATE TABLE building (b int PRIMARY KEY, alarm text, siren text);
INSERT INTO building VALUES (1, NULL, 'on'), (2, NULL, NULL);
ALTER TABLE room ADD COLUMN b int;
UPDATE room SET b = room;
SELECT gaps.create_model('site');
SELECT gaps.add_member('site', 'learned');
SELECT gaps.declare_attribute('site', 'building', 'alarm', ARRAY['off', 'on']);
SELECT gaps.declare_attribute('site', 'building', 'siren', ARRAY['off', 'on']);
SELECT gaps.add_stripe('site', 'alarm', 'building', 'alarm');
SELECT gaps.add_stripe('site', 'siren', 'building', 'siren');
SELECT gaps.add_factor('site', 'alarm', 'alarm', ARRAY['learned.occ'], kind => 'noisy_or',
					   params => ARRAY[0.05, 0.7], aggregate => ARRAY[true]);
SELECT gaps.map_factor('site', 'alarm', 1, 'b', 'b');
SELECT gaps.add_factor('site', 'siren', 'siren', ARRAY['alarm'], kind => 'logistic',
					   params => ARRAY[-2, 0, 4]);
