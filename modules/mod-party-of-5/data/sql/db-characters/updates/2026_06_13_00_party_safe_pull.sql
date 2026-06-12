-- WowPs Party-of-5 — per-tank "safe pull" toggle (idempotent).
--
-- A lead tank can be told to OPEN a pack with the ranged tag-and-step-back pull
-- (let the pack close in open space) or to just barge straight into melee. The
-- choice is per-character, stored here. Tri-state, but unlike wait_tank_threat
-- there's no per-type split — the safe pull is the default for EVERY tank:
--   ''  = unset  -> default ON (safe pull)
--   '1' = explicit safe pull
--   '0' = explicit barge
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'party_loadout'
      AND COLUMN_NAME  = 'safe_pull');
SET @ddl := IF(@col = 0,
    "ALTER TABLE `party_loadout` ADD COLUMN `safe_pull` VARCHAR(1) NOT NULL DEFAULT ''",
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;
