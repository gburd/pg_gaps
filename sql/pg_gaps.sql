-- The extension installs into its fixed schema and its library loads into
-- this server (a wrong magic block fails the LOAD).
CREATE EXTENSION pg_gaps;
SELECT extversion, extnamespace::regnamespace AS schema, extrelocatable
  FROM pg_extension WHERE extname = 'pg_gaps';
LOAD 'pg_gaps';
