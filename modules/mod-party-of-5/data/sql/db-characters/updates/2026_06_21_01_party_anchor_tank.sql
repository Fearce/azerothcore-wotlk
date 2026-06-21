-- WowPs Party-of-5 — per-bot "anchor on tank" toggle (idempotent).
--
-- When ON for a NON-tank bot, that bot formation-FOLLOWS the party's TANK
-- instead of the human leader (only while the leader isn't the tank) — so melee
-- reach the front fast when the leader is ranged. Non-combat positioning only.
-- Stored per-character here. Tri-state column, but unlike safe_pull there is NO
-- default-on: unset means OFF everywhere.
--   ''  = unset  -> OFF (follow the leader, normal behaviour)
--   '1' = explicit anchor-on-tank
--   '0' = explicit off
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'party_loadout'
      AND COLUMN_NAME  = 'anchor_tank');
SET @ddl := IF(@col = 0,
    "ALTER TABLE `party_loadout` ADD COLUMN `anchor_tank` VARCHAR(1) NOT NULL DEFAULT ''",
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;
