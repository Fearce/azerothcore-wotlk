-- WowPs Party-of-5 — per-character roster display order (idempotent).
--
-- The Party Roster panel lets the player hand-sort ALL their characters (not just
-- the 5 enrolled party members) with up/down arrows. The chosen position persists
-- here as a per-account display rank; NULL means "not yet placed" and sorts after
-- the placed characters in the legacy enrolled-first / name order. Mirrors the
-- runtime EnsureRosterOrderColumn() so existing DBs get it without a migration.
-- MySQL 8.0 has no ADD COLUMN IF NOT EXISTS, so probe information_schema first.
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'characters'
      AND COLUMN_NAME  = 'roster_order');
SET @ddl := IF(@col = 0,
    'ALTER TABLE `characters` ADD COLUMN `roster_order` SMALLINT UNSIGNED DEFAULT NULL',
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;
