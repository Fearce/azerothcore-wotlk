-- DB update 2026_05_15_00 -> 2026_07_03_00
-- Quest 10629 "Shizz Work" (Hellfire Peninsula): the Fel Guard Hound's "leavings"
-- (Felhound Poo, GO 184980, loot template 21311) rarely/never seemed to drop the
-- Shredder Keys (item 30794), and players reported many poos with NO loot at all.
--
-- Root cause: the three loot entries (Gnawed Bone 44.9, Acidic Slime 35.6,
-- Shredder Keys 20) have chances summing to ~100% -- clearly meant to be mutually
-- exclusive (exactly one drop) -- but were configured with GroupId = 0, i.e. three
-- INDEPENDENT rolls. That produced ~28% empty poos ((1-.449)(1-.356)(1-.20) = 0.284,
-- the "sometimes no loot in it" symptom) and diluted the quest key.
--
-- Fix: put the three entries into a single group (GroupId = 1) so each poo yields
-- exactly one item (never empty, Blizzlike), and raise the quest key's weight so the
-- slow feed-the-hound mechanic reliably completes. Shredder Keys keeps QuestRequired.
UPDATE `gameobject_loot_template` SET `GroupId` = 1, `Chance` = 35 WHERE `Entry` = 21311 AND `Item` = 5369;   -- Gnawed Bone (grey filler)
UPDATE `gameobject_loot_template` SET `GroupId` = 1, `Chance` = 25 WHERE `Entry` = 21311 AND `Item` = 6456;   -- Acidic Slime (grey filler)
UPDATE `gameobject_loot_template` SET `GroupId` = 1, `Chance` = 40 WHERE `Entry` = 21311 AND `Item` = 30794;  -- Shredder Keys (quest 10629)
