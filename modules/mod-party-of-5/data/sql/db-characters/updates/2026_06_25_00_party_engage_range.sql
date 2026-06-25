-- WowPs Party-of-5 — per-tank "initial engage range" setting (idempotent).
--
-- How far (yards) around the LEAD TANK the auto-pull opener scans for the nearest
-- hostile to open a pull on. Per-character, stored here so it's set from the
-- rotation editor slider instead of a hard-coded constant:
--   ''        = unset -> default 22
--   '10'..'40' = explicit engage range
-- The server clamps to [10,40] (WowPsParty::BotEngageRange). Two-digit, so
-- VARCHAR(2) holds the full range.
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'party_loadout'
      AND COLUMN_NAME  = 'engage_range');
SET @ddl := IF(@col = 0,
    "ALTER TABLE `party_loadout` ADD COLUMN `engage_range` VARCHAR(2) NOT NULL DEFAULT ''",
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;
