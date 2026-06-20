-- WowPs Party-of-5 — per-tank "multi-pull count" setting (idempotent).
--
-- How many mobs a lead tank gathers on its INITIAL multi-pull. Per-character,
-- stored here so it's set from the rotation editor instead of a rotation rule:
--   ''       = unset -> default 3 (the standard multi-pull)
--   '1'      = classic single-mob pull
--   '2'..'8' = explicit cluster size
-- The server clamps to [1,8] (WowPsParty::BotInitialPullCount). Single-digit, so
-- VARCHAR(1) matches the safe_pull / wait_tank_threat columns.
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'party_loadout'
      AND COLUMN_NAME  = 'pull_count');
SET @ddl := IF(@col = 0,
    "ALTER TABLE `party_loadout` ADD COLUMN `pull_count` VARCHAR(1) NOT NULL DEFAULT ''",
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;
