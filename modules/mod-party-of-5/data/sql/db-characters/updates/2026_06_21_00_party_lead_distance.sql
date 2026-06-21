-- WowPs Party-of-5 — per-tank "lead distance" setting (idempotent).
--
-- How far ahead (yards) a lead tank leads the party in dungeons. Per-character,
-- stored here so it's set from the rotation editor instead of a rotation rule:
--   ''        = unset -> default 15
--   '5'..'40' = explicit lead distance
-- The server clamps to [5,40] (WowPsParty::BotLeadDistance). Two-digit, so
-- VARCHAR(2) holds the full range.
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'party_loadout'
      AND COLUMN_NAME  = 'lead_distance');
SET @ddl := IF(@col = 0,
    "ALTER TABLE `party_loadout` ADD COLUMN `lead_distance` VARCHAR(2) NOT NULL DEFAULT ''",
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;
