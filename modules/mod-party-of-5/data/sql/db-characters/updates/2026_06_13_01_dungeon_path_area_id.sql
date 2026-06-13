-- WowPs Party-of-5 — per-WING dungeon paths (idempotent).
--
-- Multi-wing dungeons (Scarlet Monastery x4, Dire Maul x3) put every wing on ONE
-- map id, so a single map-keyed path made the recorded SM Library route play in
-- SM Graveyard (the tank walked toward the other wing's coordinates). Key paths
-- by the recorder's area too, so each wing stores its own route; playback picks
-- the wing whose waypoints are nearest the leader. Existing rows default to
-- area_id 0 and still work (selected by proximity).
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'dungeon_path'
      AND COLUMN_NAME  = 'area_id');
SET @ddl := IF(@col = 0,
    "ALTER TABLE `dungeon_path` "
    "ADD COLUMN `area_id` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `map_id`, "
    "DROP PRIMARY KEY, "
    "ADD PRIMARY KEY (`map_id`, `area_id`, `sequence`)",
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;
