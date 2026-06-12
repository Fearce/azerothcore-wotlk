-- WowPs Party-of-5 — per-bot "wait for tank threat" toggle (idempotent).
--
-- DPS bots can be told to hold/throttle until the human tank has built threat
-- (so they don't rip aggro) or to blast instantly like they used to. The choice
-- is per-character, stored here. Tri-state so the DEFAULT can differ by bot type
-- without a row per bot:
--   ''  = unset  -> use the per-type default (henchmen WAIT, heroes BLAST)
--   '1' = explicit WAIT
--   '0' = explicit BLAST
SET @col := (SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'party_loadout'
      AND COLUMN_NAME  = 'wait_tank_threat');
SET @ddl := IF(@col = 0,
    "ALTER TABLE `party_loadout` ADD COLUMN `wait_tank_threat` VARCHAR(1) NOT NULL DEFAULT ''",
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;
