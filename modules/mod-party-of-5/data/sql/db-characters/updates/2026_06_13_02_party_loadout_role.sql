-- WowPs Party-of-5 — per-character role for SOLO / un-enrolled characters (idempotent).
--
-- A party role (tank/healer/dps) normally lives in account_party.role, which only
-- exists for ENROLLED characters. In solo mode the player runs whatever character
-- they like — often a 6th char that can't enroll (party full) — yet still needs a
-- role so hired henchmen behave (e.g. the human tanks → henchmen wait for threat).
-- Store that per-character role here; the leader-role lookup falls back to it when
-- the character has no account_party row. '' = unset (treated as dps).
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'party_loadout'
      AND COLUMN_NAME  = 'role');
SET @ddl := IF(@col = 0,
    "ALTER TABLE `party_loadout` ADD COLUMN `role` VARCHAR(8) NOT NULL DEFAULT ''",
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;
