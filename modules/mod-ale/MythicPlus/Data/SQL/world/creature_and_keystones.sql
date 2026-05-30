DELETE FROM `creature_template` WHERE (`entry` = 900001);
INSERT INTO `creature_template` (`entry`, `difficulty_entry_1`, `difficulty_entry_2`, `difficulty_entry_3`, `KillCredit1`, `KillCredit2`, `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`, `speed_walk`, `speed_run`, `speed_swim`, `speed_flight`, `detection_range`, `rank`, `dmgschool`, `DamageModifier`, `BaseAttackTime`, `RangeAttackTime`, `BaseVariance`, `RangeVariance`, `unit_class`, `unit_flags`, `unit_flags2`, `dynamicflags`, `family`, `type`, `type_flags`, `lootid`, `pickpocketloot`, `skinloot`, `PetSpellDataId`, `VehicleId`, `mingold`, `maxgold`, `AIName`, `MovementType`, `HoverHeight`, `HealthModifier`, `ManaModifier`, `ArmorModifier`, `ExperienceModifier`, `RacialLeader`, `movementId`, `RegenHealth`, `flags_extra`, `ScriptName`, `VerifiedBuild`) VALUES
(900001, 0, 0, 0, 0, 0, 'Font of Power', '', 'Speak', 900001, 80, 80, 0, 35, 1, 1, 1.14286, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 1, 768, 32768, 0, 0, 0, 1048576, 0, 0, 0, 0, 0, 0, 0, '', 0, 1, 1, 1, 1, 1, 0, 0, 1, 0, '', 12340);

DELETE FROM `creature_template_model` WHERE (`CreatureID` = 900001);
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(900001, 1, 24868, 1, 1, 0);

DELETE FROM `item_template` WHERE (`entry` = 900100);
INSERT INTO `item_template` (`entry`, `class`, `subclass`, `SoundOverrideSubclass`, `name`, `displayid`, `Quality`, `Flags`, `FlagsExtra`, `BuyCount`, `BuyPrice`, `SellPrice`, `InventoryType`, `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`, `RequiredSkill`, `RequiredSkillRank`, `requiredspell`, `requiredhonorrank`, `RequiredCityRank`, `RequiredReputationFaction`, `RequiredReputationRank`, `maxcount`, `stackable`, `ContainerSlots`, `stat_type1`, `stat_value1`, `stat_type2`, `stat_value2`, `stat_type3`, `stat_value3`, `stat_type4`, `stat_value4`, `stat_type5`, `stat_value5`, `stat_type6`, `stat_value6`, `stat_type7`, `stat_value7`, `stat_type8`, `stat_value8`, `stat_type9`, `stat_value9`, `stat_type10`, `stat_value10`, `ScalingStatDistribution`, `ScalingStatValue`, `dmg_min1`, `dmg_max1`, `dmg_type1`, `dmg_min2`, `dmg_max2`, `dmg_type2`, `armor`, `holy_res`, `fire_res`, `nature_res`, `frost_res`, `shadow_res`, `arcane_res`, `delay`, `ammo_type`, `RangedModRange`, `spellid_1`, `spelltrigger_1`, `spellcharges_1`, `spellppmRate_1`, `spellcooldown_1`, `spellcategory_1`, `spellcategorycooldown_1`, `spellid_2`, `spelltrigger_2`, `spellcharges_2`, `spellppmRate_2`, `spellcooldown_2`, `spellcategory_2`, `spellcategorycooldown_2`, `spellid_3`, `spelltrigger_3`, `spellcharges_3`, `spellppmRate_3`, `spellcooldown_3`, `spellcategory_3`, `spellcategorycooldown_3`, `spellid_4`, `spelltrigger_4`, `spellcharges_4`, `spellppmRate_4`, `spellcooldown_4`, `spellcategory_4`, `spellcategorycooldown_4`, `spellid_5`, `spelltrigger_5`, `spellcharges_5`, `spellppmRate_5`, `spellcooldown_5`, `spellcategory_5`, `spellcategorycooldown_5`, `bonding`, `description`, `PageText`, `LanguageID`, `PageMaterial`, `startquest`, `lockid`, `Material`, `sheath`, `RandomProperty`, `RandomSuffix`, `block`, `itemset`, `MaxDurability`, `area`, `Map`, `BagFamily`, `TotemCategory`, `socketColor_1`, `socketContent_1`, `socketColor_2`, `socketContent_2`, `socketColor_3`, `socketContent_3`, `socketBonus`, `GemProperties`, `RequiredDisenchantSkill`, `ArmorDamageModifier`, `duration`, `ItemLimitCategory`, `HolidayId`, `ScriptName`, `DisenchantID`, `FoodType`, `minMoneyLoot`, `maxMoneyLoot`, `flagsCustom`, `VerifiedBuild`) VALUES
(900100, 12, 0, -1, 'Mythic Keystone', 62471, 4, 64, 0, 1, 0, 0, 0, -1, -1, 0, 80, 0, 0, 0, 0, 0, 0, 0, 1, 9999, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1000, 0, 0, 0, 0, 0, 0, -1, 0, -1, 0, 0, 0, 0, -1, 0, -1, 0, 0, 0, 0, -1, 0, -1, 0, 0, 0, 0, -1, 0, -1, 0, 0, 0, 0, -1, 0, -1, 1, 'Place within the Font of Power inside the dungeon on Heroic difficulty.', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, '', 0, 0, 0, 0, 0, 0);

INSERT INTO `item_template_locale` (`ID`, `locale`, `Name`, `Description`, `VerifiedBuild`) VALUES (900100, 'deDE', 'Mythischer Schlüsselstein', 'Setzt ihn im Dungeon auf dem Schwierigkeitsgrad ''Heroisch'' im Born der Macht ein.', 15050);
INSERT INTO `item_template_locale` (`ID`, `locale`, `Name`, `Description`, `VerifiedBuild`) VALUES (900100, 'esES', 'Piedra angular mítica', 'Colócala dentro de la fuente de poder que hay en la mazmorra en dificultad heroica.', 15050);
INSERT INTO `item_template_locale` (`ID`, `locale`, `Name`, `Description`, `VerifiedBuild`) VALUES (900100, 'esMX', 'Piedra angular mítica', 'Colócala dentro de la fuente de poder que hay en la mazmorra en dificultad heroica.', 15050);
INSERT INTO `item_template_locale` (`ID`, `locale`, `Name`, `Description`, `VerifiedBuild`) VALUES (900100, 'frFR', 'Clé mythique', 'À insérer dans la fontaine de puissance à l''intérieur d''un donjon en mode héroïque.', 15050);
INSERT INTO `item_template_locale` (`ID`, `locale`, `Name`, `Description`, `VerifiedBuild`) VALUES (900100, 'koKR', '신화 쐐기돌', '영웅 난이도 던전 안에 있는 마력의 샘에 넣으십시오.', 15050);
INSERT INTO `item_template_locale` (`ID`, `locale`, `Name`, `Description`, `VerifiedBuild`) VALUES (900100, 'ruRU', 'Эпохальный ключ', 'Положите ключ в Чашу силы в подземелье в героическом режиме.', 15050);
INSERT INTO `item_template_locale` (`ID`, `locale`, `Name`, `Description`, `VerifiedBuild`) VALUES (900100, 'zhCN', '[Mythic Keystone]', '[Place within the Font of Power inside the dungeon on Heroic difficulty.]', 15050);
INSERT INTO `item_template_locale` (`ID`, `locale`, `Name`, `Description`, `VerifiedBuild`) VALUES (900100, 'zhTW', '傳奇鑰石', '在英雄難度中，放置在地城裡的能量之泉內。', 15050);

INSERT INTO `creature_template_locale` (`entry`, `locale`, `Name`, `Title`, `VerifiedBuild`) VALUES (900001, 'deDE', 'Born der Macht', '', 15050);
INSERT INTO `creature_template_locale` (`entry`, `locale`, `Name`, `Title`, `VerifiedBuild`) VALUES (900001, 'esES', 'Fuente de poder', '', 15050);
INSERT INTO `creature_template_locale` (`entry`, `locale`, `Name`, `Title`, `VerifiedBuild`) VALUES (900001, 'esMX', 'Fuente de poder', '', 15050);
INSERT INTO `creature_template_locale` (`entry`, `locale`, `Name`, `Title`, `VerifiedBuild`) VALUES (900001, 'frFR', 'Fontaine de puissance', '', 15050);
INSERT INTO `creature_template_locale` (`entry`, `locale`, `Name`, `Title`, `VerifiedBuild`) VALUES (900001, 'koKR', '마력의 샘', '', 15050);
INSERT INTO `creature_template_locale` (`entry`, `locale`, `Name`, `Title`, `VerifiedBuild`) VALUES (900001, 'ruRU', 'Чаша силы', '', 15050);
INSERT INTO `creature_template_locale` (`entry`, `locale`, `Name`, `Title`, `VerifiedBuild`) VALUES (900001, 'zhCN', 'Font of Power', '', 15050);
INSERT INTO `creature_template_locale` (`entry`, `locale`, `Name`, `Title`, `VerifiedBuild`) VALUES (900001, 'zhTW', '能量之泉', '', 15050);

INSERT INTO `creature` (`id1`, `id2`, `id3`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `equipment_id`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`, `MovementType`, `npcflag`, `unit_flags`, `dynamicflags`, `ScriptName`, `VerifiedBuild`, `CreateObject`, `Comment`) VALUES
(900001, 0, 0, 574, 0, 0, 2, 1, 0, 173.299, -92.1371, 12.5535, 2.75347, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 575, 0, 0, 2, 1, 0, 571.32, -333.858, 110.14, 0.844163, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 576, 0, 0, 2, 1, 0, 172.834, -10.6004, -16.636, 1.61453, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 578, 0, 0, 2, 1, 0, 1061.64, 995.343, 361.072, 4.26374, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 595, 0, 0, 2, 1, 0, 1430.53, 549.692, 35.8522, 2.32704, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 599, 0, 0, 2, 1, 0, 1133.13, 811.837, 195.835, 0.0394203, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 600, 0, 0, 2, 1, 0, -502.675, -513.078, 11.0454, 3.12431, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 601, 0, 0, 2, 1, 0, 423.48, 802.589, 827.911, 4.61237, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 602, 0, 0, 2, 1, 0, 1311.83, 268.017, 53.2948, 0.450539, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 604, 0, 0, 2, 1, 0, 1889.09, 629.347, 176.694, 2.4522, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 604, 0, 0, 2, 1, 0, 1886.73, 855.669, 176.694, 3.92115, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 608, 0, 0, 2, 1, 0, 1819.33, 809.071, 44.3639, 4.73869, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 619, 0, 0, 2, 1, 0, 390.893, -1089.21, 47.3606, 2.55577, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 632, 0, 0, 2, 1, 0, 4910.44, 2180.07, 638.734, 0.161806, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 650, 0, 0, 2, 1, 0, 799.102, 609.297, 412.364, 3.01206, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 658, 0, 0, 2, 1, 0, 435.35, 202.332, 528.718, 1.50715, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL),
(900001, 0, 0, 668, 0, 0, 2, 1, 0, 5231.32, 1945.65, 707.695, 5.5679, 300, 0, 0, 5342, 0, 0, 0, 0, 0, '', NULL, 0, NULL);

DELETE FROM `gameobject_template` WHERE `entry`=900000;
INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`, `size`, `Data0`, `Data1`, `Data2`, `Data3`, `Data4`, `Data5`, `Data6`, `Data7`, `Data8`, `Data9`, `Data10`, `Data11`, `Data12`, `Data13`, `Data14`, `Data15`, `Data16`, `Data17`, `Data18`, `Data19`, `Data20`, `Data21`, `Data22`, `Data23`, `AIName`, `ScriptName`, `VerifiedBuild`) VALUES 
(900000, 3, 8685, 'Mythic Vault', '', '', '', 2.5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '', '', 0);

INSERT INTO `gameobject` (`id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `position_x`, `position_y`, `position_z`, `orientation`, `rotation0`, `rotation1`, `rotation2`, `rotation3`, `spawntimesecs`, `animprogress`, `state`, `ScriptName`, `VerifiedBuild`, `Comment`) VALUES 
(900000, 571, 0, 0, 1, 1, 5732.36, 515.232, 647.452, 0.358227, 0, 0, 0.178157, 0.984002, 300, 0, 1, '', NULL, NULL);
