-- WowPs Party-of-5 — per-tank "follow recorded path" toggle (idempotent).
--
-- When ON (the default), a lead tank walks the RECORDED dungeon path ahead of
-- the party (TankFollowPath). When OFF, it still LEADS but via ordinary
-- lead-ahead MoveFollow (mirrors the leader's route) instead of the recording
-- — the same fallback used for a wing that was never recorded. Stored per
-- character here. Tri-state, default ON for every tank (like safe_pull):
--   ''  = unset  -> default ON (follow the recorded path)
--   '1' = explicit follow
--   '0' = explicit off (lead via MoveFollow, ignore the recording)
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'party_loadout'
      AND COLUMN_NAME  = 'follow_path');
SET @ddl := IF(@col = 0,
    "ALTER TABLE `party_loadout` ADD COLUMN `follow_path` VARCHAR(1) NOT NULL DEFAULT ''",
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;
