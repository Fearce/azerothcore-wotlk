-- WowPs Party-of-5 — account-wide COMMON shared rotation (idempotent).
--
-- A single pre-rotation per ACCOUNT that runs (highest priority, first) on EVERY
-- party bot — heroes AND henchmen — so universal rules (interrupt/stun a known
-- dangerous cast, eat/drink, ...) live in one place instead of being copy-pasted
-- into all 9 individual rotations. Prepended to each bot's own rules at eval time
-- (TickRotation); the bot's rules still run after, so the common rotation is a
-- pre-rotation, not a replacement. Stored as the same priority_actions_json DSL the
-- per-bot rotations use.
CREATE TABLE IF NOT EXISTS `party_shared_rotation` (
    `account`               INT UNSIGNED NOT NULL,
    `priority_actions_json` LONGTEXT     NOT NULL,
    PRIMARY KEY (`account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
