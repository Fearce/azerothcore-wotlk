-- Remove "Minigob Manabonk" (creature_template entry 32838), the Dalaran prank gnome
-- that periodically polymorphs everyone nearby into random critters and mails them
-- junk wands. His single spawn (guid 44457) is driven by game_event 33 ("Dalaran:
-- Minigob", 30-min occurrence / 5-min length). Event 33 controls ONLY this creature
-- (0 gameobjects, no other creatures), so deleting the spawn plus its event link
-- removes him for good without disturbing anything else. Idempotent.
DELETE FROM `game_event_creature` WHERE `eventEntry` = 33 AND `guid` = 44457;
DELETE FROM `creature_addon` WHERE `guid` = 44457;
DELETE FROM `creature` WHERE `guid` = 44457 AND `id1` = 32838;
