-- WowPs Party-of-5 — per-tank "pull gray (trivial) mobs" toggle (idempotent).
--
-- When ON, the lead tank will engage + pull mobs that are GRAY (trivial, too low
-- level to grant XP) for it — the case where a random dungeon rolls low-level
-- mobs and the tank otherwise refuses to pull anything at all. When OFF (the
-- default), gray mobs are ignored by the auto-pull as before (WontAutoAggro).
-- Stored per character here. Tri-state, default OFF for every tank (like safe_pull):
--   ''  = unset  -> default OFF (ignore gray mobs)
--   '1' = explicit on  (pull gray mobs too)
--   '0' = explicit off
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'party_loadout'
      AND COLUMN_NAME  = 'pull_grays');
SET @ddl := IF(@col = 0,
    "ALTER TABLE `party_loadout` ADD COLUMN `pull_grays` VARCHAR(1) NOT NULL DEFAULT ''",
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;
