-- WowPs Party-of-5 mod — characters DB schema (idempotent)
-- Tables: account_party (account -> 5 char slot map), party_loadout (per-char AI config)
-- Column: characters.party_slot (denormalized for fast per-character slot lookup on login)

CREATE TABLE IF NOT EXISTS `account_party` (
    `account`             INT UNSIGNED      NOT NULL,
    `slot`                TINYINT UNSIGNED  NOT NULL,
    `guid`                INT UNSIGNED      NOT NULL,
    `is_active_on_login`  TINYINT UNSIGNED  NOT NULL DEFAULT 0,
    `created_at`          TIMESTAMP         NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`account`, `slot`),
    UNIQUE KEY `account_party_guid_unique` (`guid`),
    KEY `account_party_account_idx` (`account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci
  COMMENT='WowPsParty: maps an account to its 5-character party (slots 0..4)';

CREATE TABLE IF NOT EXISTS `party_loadout` (
    `guid`                   INT UNSIGNED  NOT NULL,
    `strategies_csv`         TEXT          NOT NULL,
    `talents_hex`            VARCHAR(128)  NOT NULL DEFAULT '',
    `glyphs_csv`             VARCHAR(256)  NOT NULL DEFAULT '',
    `gear_lock_json`         TEXT          NOT NULL,
    `priority_actions_json`  TEXT          NOT NULL,
    `updated_at`             TIMESTAMP     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci
  COMMENT='WowPsParty: per-character AI loadout (strategies, talents, glyphs, gear locks, rotation overrides)';

-- Add party_slot column to characters table. Nullable for backward-compat with pre-mod characters
-- (which are treated as solo, not yet enrolled in the party-of-5 system).
SET @col_exists := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'characters'
      AND COLUMN_NAME  = 'party_slot');
SET @ddl := IF(@col_exists = 0,
    'ALTER TABLE `characters` ADD COLUMN `party_slot` TINYINT UNSIGNED DEFAULT NULL, ADD KEY `characters_party_slot_idx` (`account`, `party_slot`)',
    'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
