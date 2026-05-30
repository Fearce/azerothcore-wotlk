-- WowPs Party-of-5 — characters DB schema drift fix (idempotent).
--
-- The original base schema (2026_05_27_00_party_of_5_schema.sql) was missing
-- three things that had only ever been applied to the dev DB by hand:
--   * account_party.role        (per-member tank/healer/dps role)
--   * party_loadout.action_bar_csv
--   * the dungeon_path table     (recorded tank lead-path waypoints)
-- A fresh clone therefore failed with "Unknown column 'role' in 'field list'".
-- This migration adds them, guarded so it's safe to re-run / on existing DBs.

-- account_party.role -------------------------------------------------------
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'account_party'
      AND COLUMN_NAME  = 'role');
SET @ddl := IF(@col = 0,
    "ALTER TABLE `account_party` ADD COLUMN `role` VARCHAR(8) NOT NULL DEFAULT 'dps' AFTER `guid`",
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- party_loadout.action_bar_csv ---------------------------------------------
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'party_loadout'
      AND COLUMN_NAME  = 'action_bar_csv');
SET @ddl := IF(@col = 0,
    "ALTER TABLE `party_loadout` ADD COLUMN `action_bar_csv` VARCHAR(256) NOT NULL DEFAULT ''",
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- dungeon_path table (tank lead-path recording) ----------------------------
CREATE TABLE IF NOT EXISTS `dungeon_path` (
    `map_id`       INT UNSIGNED NOT NULL,
    `sequence`     INT UNSIGNED NOT NULL,
    `x`            FLOAT        NOT NULL,
    `y`            FLOAT        NOT NULL,
    `z`            FLOAT        NOT NULL,
    `orientation`  FLOAT        NOT NULL DEFAULT 0,
    `recorded_by`  INT UNSIGNED DEFAULT NULL,
    `recorded_at`  TIMESTAMP    NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`map_id`, `sequence`),
    KEY `map_idx` (`map_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci
  COMMENT='WowPsParty: recorded dungeon tank lead-path waypoints';
