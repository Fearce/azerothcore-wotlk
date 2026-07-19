-- WowPs Party-of-5 — per-mob COMMON rule sections (idempotent).
--
-- The Common tab's account-wide rotation (party_shared_rotation) grew unwieldy once
-- bosses were scripted one-by-one into it, every rule carrying its own
-- `target_name:<boss>` clause. This splits those out into named sections: one row per
-- (account, mob), each holding just that boss's rules in the same priority_actions_json
-- DSL the shared/per-bot rotations use. The section NAME is the gate — at eval time the
-- server prepends `target_name:<mob>&` to each rule (GetSharedAndMobRotation), so the
-- stored rules stay clean and round-trip to the editor untouched. Runs as a
-- pre-rotation ahead of the general Common list AND the bot's own rules, so a boss
-- section overrides the generic Common rules while that boss is the target.
CREATE TABLE IF NOT EXISTS `party_mob_rotation` (
    `account`               INT UNSIGNED NOT NULL,
    `mob_name`              VARCHAR(100) NOT NULL,
    `priority_actions_json` LONGTEXT     NOT NULL,
    PRIMARY KEY (`account`, `mob_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
