-- Stranglethorn Fishing Extravaganza never appeared even though the client
-- calendar (Holidays.dbc) showed it "Sundays 14:00-16:00". Root cause is a
-- timezone mismatch, NOT a missing event.
--
-- The worldserver reads game_event.start_time via UNIX_TIMESTAMP() on a MySQL
-- connection whose session timezone is UTC, and compares it against GameTime
-- (real epoch). So the stock anchor "2016-10-30 14:00:00" is treated as
-- 14:00 UTC = 16:00 Danish local (CEST, UTC+2) -- the pools only spawned at
-- 16:00, two hours after players (and the calendar) expected them at 14:00.
-- The STV cluster (14 Announce / 15 Pools / 62 The Crew / 90 Turn-ins) was the
-- only holiday still on the raw UTC anchor; the server's own "Diurnal fishing
-- event" (id 79) already uses the 12:00-UTC = 14:00-CEST convention.
--
-- Shift every STV anchor back 2h so the schedule's wall-clock hour lands on
-- Danish local time, preserving the relative offsets between the four events:
--   15 Pools    -> Sun 12:00 UTC = 14:00 CEST, 120 min -> 14:00-16:00 local
--   90 Turn-ins -> Sun 12:00 UTC = 14:00 CEST, 180 min -> 14:00-17:00 local
--   62 The Crew -> Sun 11:00 UTC = 13:00 CEST, 240 min -> 13:00-17:00 local
--   14 Announce -> Sat 22:00 UTC = 00:00 CEST, 24 h    -> all Saturday local
--
-- CAVEAT (engine limitation): the game_event weekly window is a fixed 604800s
-- epoch modulo and cannot track DST. These anchors are correct during CEST
-- (summer). Under CET (winter, UTC+1) every window shifts 1h earlier in local
-- terms (pools 13:00-15:00). A DST-exact schedule is impossible with the stock
-- epoch-modulo event engine; the 1h winter drift is the accepted trade-off.
--
-- Idempotent.

UPDATE `game_event` SET `start_time` = '2016-10-28 22:00:00' WHERE `eventEntry` = 14; -- Announce
UPDATE `game_event` SET `start_time` = '2016-10-30 12:00:00' WHERE `eventEntry` = 15; -- Fishing Pools
UPDATE `game_event` SET `start_time` = '2016-10-30 11:00:00' WHERE `eventEntry` = 62; -- The Crew
UPDATE `game_event` SET `start_time` = '2016-10-30 12:00:00' WHERE `eventEntry` = 90; -- Turn-ins
