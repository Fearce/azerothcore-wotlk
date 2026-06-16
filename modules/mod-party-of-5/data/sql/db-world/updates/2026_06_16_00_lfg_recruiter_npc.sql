-- WowPsParty: gossip NPC for the LFG "fill my party" offer.
-- Summoned on the player (visible to summoner only, 60s timed despawn) by
-- PartyLfgFill.cpp; its gossip carries the BoxMoney cost confirmation. Friendly
-- faction (35) + immune-to-pc/npc so it never enters combat; ScriptName binds
-- the gossip handler. Idempotent (DELETE + INSERT).
SET @Entry := 920050;

DELETE FROM `creature_template` WHERE `entry` = @Entry;
INSERT INTO `creature_template`
(`entry`, `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`,
 `npcflag`, `rank`, `dmgschool`, `BaseAttackTime`, `RangeAttackTime`, `unit_class`, `unit_flags`, `type`,
 `type_flags`, `lootid`, `pickpocketloot`, `skinloot`, `AIName`, `MovementType`, `HoverHeight`,
 `RacialLeader`, `movementId`, `RegenHealth`, `CreatureImmunitiesId`, `flags_extra`, `ScriptName`) VALUES
(@Entry, 'Party Recruiter', 'Looking For Group', NULL, 0, 80, 80, 2, 35,
 1, 0, 0, 2000, 0, 1, 768, 7,
 138936390, 0, 0, 0, '', 0, 1,
 0, 0, 1, 0, 0, 'npc_wowps_lfg_recruiter');

DELETE FROM `creature_template_model` WHERE `CreatureID` = @Entry;
INSERT INTO `creature_template_model`
(`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(@Entry, 0, 19646, 1, 1, 0);
