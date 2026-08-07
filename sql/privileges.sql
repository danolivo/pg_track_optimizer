-- Access control for the tracked data.
--
-- The hash table stores query texts of all users and all databases, so by
-- default only superusers may read or manage it.  Everything sensitive must
-- be revoked from PUBLIC; the DBA can delegate access with plain GRANTs.
CREATE EXTENSION pg_track_optimizer;

CREATE ROLE regress_pgto_user;

SET ROLE regress_pgto_user;

-- Must fail: exposes query texts of every user and database
SELECT count(*) FROM pg_track_optimizer();
SELECT count(*) FROM pg_track_optimizer;

-- Must fail: management functions are not for ordinary users
SELECT pg_track_optimizer_flush();
SELECT pg_track_optimizer_reset();

-- Status exposes no query texts and stays generally readable
SELECT mode IS NOT NULL AS has_mode FROM pg_track_optimizer_status;

RESET ROLE;

-- The DBA can delegate access.  Note that SELECT on the view alone is not
-- enough: EXECUTE on a function inside a view is checked as the calling
-- user, not the view owner, so the underlying function must be granted too.
GRANT SELECT ON pg_track_optimizer TO regress_pgto_user;

SET ROLE regress_pgto_user;
SELECT count(*) >= 0 AS readable FROM pg_track_optimizer;
SELECT count(*) FROM pg_track_optimizer();
RESET ROLE;

GRANT EXECUTE ON FUNCTION pg_track_optimizer() TO regress_pgto_user;
SET ROLE regress_pgto_user;
SELECT count(*) >= 0 AS readable FROM pg_track_optimizer;
SELECT count(*) >= 0 AS callable FROM pg_track_optimizer();
RESET ROLE;

-- Management functions become delegable as well (no hardcoded superuser
-- check): a GRANT is enough.
GRANT EXECUTE ON FUNCTION pg_track_optimizer_reset() TO regress_pgto_user;
SET ROLE regress_pgto_user;
SELECT pg_track_optimizer_reset() >= 0 AS reset_allowed;
RESET ROLE;

-- Clean up
DROP OWNED BY regress_pgto_user;
DROP ROLE regress_pgto_user;
SELECT pg_track_optimizer_reset() >= 0;
DROP EXTENSION pg_track_optimizer;
