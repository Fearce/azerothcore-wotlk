/*
 * WowPs Party-of-5 mod — Track-4 PlayerScript hooks
 *
 * Mirrors XP, reputation, money, loot and quest accepts across all 5 party
 * members, and auto-learns class spells on level-up. Each hook uses a
 * thread_local guard to avoid the obvious infinite recursion (each mirrored
 * grant would re-enter the hook and re-mirror forever).
 *
 * Also mirrors the leader's TEXT EMOTES onto their companions (/dance and the
 * whole party dances) — see MirrorTextEmoteToParty below.
 */

#include "PartyMgr.h"

#include "ArenaTeam.h"   // GetReqPlayersForType — arena auto-ready progress log
#include "Battleground.h"
#include "Chat.h"
#include "Creature.h"
#include "DBCStores.h"   // sLockStore — identify "key" items that open world objects
#include "Group.h"
#include "GroupMgr.h"
#include "GameTime.h"
#include "LFGMgr.h"      // clear a stale group LFG state after a BG (dungeon-finder unblock)
#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"     // SMSG_TEXT_EMOTE — replay the leader's emote line for a companion
#include "Player.h"
#include "QuestDef.h"
#include "Reputation/ReputationMgr.h"
#include "PartyFollow.h"
#include "PartyPath.h"
#include "ScriptMgr.h"
#include "GroupScript.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Spell.h"                    // Spell::GetSpellInfo — record the mount a human just rode
#include "CheckMountStateAction.h"    // CheckMountStateAction::RecordManualMount
#include "StringFormat.h"
#include "Trainer.h"
#include "World.h"        // sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE) — emote earshot
#include "WorldPacket.h"
#include "WorldSession.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "PartyRotation.h"   // RecordSpellDamageTaken (backs took_damage_from:<name>)

#include <algorithm>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#include <excpt.h>   // __try/__except — guard reagent scans against freed item pointers
#endif

namespace WowPsParty
{
    // Probe an item behind structured exception handling before a reagent scan trusts it.
    // The shared-inventory item mover can leave a freed item referenced in a bag slot;
    // reading it (GetEntry/GetCount/GetBagSize) would abort the worldserver with a null
    // value-array ACCESS_VIOLATION. A guarded read of its entry faults harmlessly so the
    // scan skips it. POD-only locals (C2712); MSVC-only, a no-op elsewhere.
#ifdef _WIN32
    static bool WowPsItemReadable(Item const* it)
    {
        __try { volatile uint32 e = it->GetEntry(); (void)e; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
#else
    static bool WowPsItemReadable(Item const* /*it*/) { return true; }
#endif

    // Re-entrance guard. Set by the originating hook, checked by the mirror
    // path before propagating to peers. thread_local is fine — AC dispatches
    // PlayerScript hooks on the world thread.
    static thread_local bool g_propagatingXP    = false;
    static thread_local bool g_propagatingRep   = false;
    static thread_local bool g_propagatingMoney = false;
    static thread_local bool g_propagatingQuest = false;
    static thread_local bool g_propagatingLoot  = false;
    // Set while a HERO is turning in a mirrored quest. The leader's quest XP /
    // money / rep were already mirrored to every hero by the leader's own
    // turn-in, so a hero's RewardQuest must NOT grant them again (it would double
    // the gain and the party would out-level itself). We keep the hero's reward
    // ITEMS (the whole point — each hero picks a different choice) but ZERO the
    // XP/money/rep in those hooks while this is set.
    static thread_local bool g_heroQuestTurnin  = false;

    bool IsEnabled();     // from PartyBootstrap.cpp
    bool IsLogVerbose();

    // Shared-progression toggle (XP / gold / loot / quest mirroring). Off =
    // each char keeps its own — normal solo play. Defaults ON.
    static bool ProgressionShared(Player* p)
    {
        if (!p || !p->GetSession()) return false;
        return GetAccountSettings(p->GetSession()->GetAccountId()).sharedProgression;
    }

    // Shared-inventory toggle (the merged party bag grid, B key). Off = each
    // char's bags are their own. Gates cross-member crafting reagents. ON default.
    static bool InventoryShared(Player* p)
    {
        if (!p || !p->GetSession()) return false;
        return GetAccountSettings(p->GetSession()->GetAccountId()).sharedInventory;
    }

    // Return a real person in `killer`'s group other than the person directing
    // that killer's own companion party. A playerbot may land the killing blow,
    // so simply finding any real player in the group would also find its own
    // captain and incorrectly disable solo party auto-loot.
    static Player* GetExternalHumanGroupMember(Player* killer)
    {
        if (!killer) return nullptr;
        Group* group = killer->GetGroup();
        if (!group) return nullptr;

        ObjectGuid const captain = GetLeaderFor(killer->GetGUID());
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == killer || !member->GetSession()) continue;
            if (captain && member->GetGUID() == captain) continue;

            PlayerbotAI* memberAI = sPlayerbotsMgr.GetPlayerbotAI(member);
            if (!memberAI || memberAI->IsRealPlayer()) return member;
        }
        return nullptr;
    }

    // The co-op bypass can run once per creature, so keep the diagnostic useful
    // without turning a trash pull into a wall of identical log entries.
    static void LogExternalHumanLootHandoff(Player* killer, Player* human,
                                            Creature* killed, Group* group)
    {
        if (!killer || !human || !killed || !group) return;

        static thread_local std::unordered_map<uint32, uint32> lastLogAt;
        uint32 const killerGuid = killer->GetGUID().GetCounter();
        uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());
        auto const previous = lastLogAt.find(killerGuid);
        if (previous != lastLogAt.end() && now - previous->second < 5) return;
        lastLogAt[killerGuid] = now;

        LOG_INFO("module",
            "[WowPsParty Loot] {} leaves corpse {} to group loot: external human "
            "{} present method={}",
            killer->GetName(), killed->GetGUID().GetCounter(), human->GetName(),
            static_cast<uint32>(group->GetLootMethod()));
    }

    // ---- Raid loot belongs to the raid ---------------------------------------
    // The auto-loot bypass in OnPlayerCreatureKill empties a body at the instant
    // of the kill. That is a single-player convenience, and it costs nothing while
    // the "group" is one person and their companions — but the core only starts a
    // group's loot rolls when somebody OPENS the corpse (Player::SendLoot), and
    // after the bypass has run nobody ever does. In a raid that swallowed the very
    // drops the group is entitled to roll for: the epics a boss dropped were in a
    // companion's bags before a roll window could exist. So in a raid the party
    // takes only what the group's own loot threshold calls trash, leaves the rest
    // on the body, starts the rolls the open would have started, and keeps the
    // companions out of them.

    // The raid whose loot rules own a drop, or nullptr when ordinary party sharing
    // applies. A battleground / battlefield group is a raid too, and nobody rolls
    // for what its NPCs drop, so those keep the old behaviour.
    static Group* RaidLootRules(Group* group)
    {
        if (!group || !group->isRaidGroup() || group->isBGGroup() || group->isBFGroup())
            return nullptr;
        return group;
    }

    // Only ever the group that TAPPED the body, and only when the killer is in it.
    // Rolling out a corpse another group is entitled to would block their items
    // (Roll sets is_blocked) on loot that was never ours to hand out.
    static Group* RaidLootRulesForCorpse(Player* killer, Creature* killed)
    {
        if (!killer || !killed) return nullptr;
        Group* const group = killed->GetLootRecipientGroup();
        if (!group || group != killer->GetGroup()) return nullptr;
        return RaidLootRules(group);
    }

    // The group's own threshold is the line: at or above it a drop is rolled (or
    // master-looted) instead of round-robined, exactly as it would be for a raid
    // of people. is_blocked means a roll is already running on it.
    static bool RaidRollsForItem(Group const* raid, LootItem const& li, ItemTemplate const& tmpl)
    {
        return li.is_blocked || tmpl.Quality >= uint32(raid->GetLootThreshold());
    }

    // The core starts a group's rolls the first time somebody OPENS the body
    // (Player::SendLoot). After the auto-loot pass has run nobody ever will, so
    // the drops we leave for the raid would sit there unoffered — start the rolls
    // here instead. Only the two methods whose whole interaction IS the roll:
    // master loot, round robin and free-for-all hand items out through the loot
    // WINDOW, and those packets belong with the open. Their above-threshold drops
    // are still left on the body by the pass below, for the player to take there.
    static void StartRaidLootRolls(Group* raid, Creature* killed)
    {
        Loot& loot = killed->loot;
        if (loot.loot_type != LOOT_NONE) return;   // already roll-processed

        switch (raid->GetLootMethod())
        {
            case GROUP_LOOT:        raid->GroupLoot(&loot, killed);      break;
            case NEED_BEFORE_GREED: raid->NeedBeforeGreed(&loot, killed); break;
            default: return;
        }
        // Claim the body as roll-processed exactly as opening it would, so a later
        // open can't build a second set of rolls over the same items.
        loot.loot_type = LOOT_CORPSE;
    }

    // One count for every ordinary drop still lying on the body, so that
    // Loot::isLooted() cannot claim the corpse is empty while a raid item is
    // unclaimed on it — that is what lets the next DoLootRelease run Loot::clear()
    // straight through an item nobody has won yet.
    //
    // A floor, never a ceiling: quest_items[] are counted in the same tally and
    // this pass does not walk them, so lowering it would destroy a quest drop the
    // player has not collected. And it must be a floor rather than a one-off "if
    // it reached zero, make it one" — Group::CountTheRoll spends one count per
    // item it awards (Group.cpp, unguarded --unlootedCount), so a single spare
    // count is gone the moment the rolls resolve and the over-spend re-emerges
    // 60 seconds later, against exactly the drops nobody won.
    static void RestoreUnlootedFloor(Loot& loot)
    {
        uint32 stillHere = 0;
        for (LootItem const& li : loot.items)
            if (!li.is_looted)
                ++stillHere;

        if (loot.unlootedCount < stillHere)
            loot.unlootedCount = uint8(stillHere);
    }

    // Retire one drop from the body — either into the party bags or, for a
    // blacklisted entry, into nothing. Both are "the party is done with this
    // slot", so both owe the tally the same bookkeeping.
    //
    // Keep the core's own count straight: this pass stores directly rather than
    // through Player::StoreLootItem, and an unlootedCount that never comes down
    // leaves Loot::isLooted() permanently false — so a body we only PARTLY emptied
    // (a raid drop left for the roll) keeps its sparkle forever, because the
    // teardown at the end of the roll (Group::CountTheRoll) is gated on isLooted().
    //
    // Only ever for an item the FILL actually counted. A condition-gated drop is
    // counted LAZILY, the moment somebody opens the loot window
    // (Loot::FillNonQuestNonFFAConditionalLoot sets is_counted) — and this pass
    // exists precisely so that nobody ever does. Decrementing for one of those
    // spends the count belonging to a drop still ON the body: unlootedCount reaches
    // 0 while a raid epic is mid-roll, isLooted() starts saying the corpse is empty,
    // and the next loot release runs Loot::clear() straight through the unwon item.
    // The predicate is the core's own (LootMgr.cpp::FillLoot, LootItemStorage.cpp).
    // Declared in PartyMgr.h — PartyFollow.cpp's henchman corpse loot retires a
    // blacklisted drop through this same helper, so the two paths share one
    // definition of "was this drop counted" instead of drifting apart.
    void MarkPartyTookLootItem(Loot& loot, LootItem& li, ItemTemplate const& tmpl)
    {
        li.is_looted = true;

        bool const wasCounted =
            li.is_counted ||
            (!li.needs_quest && li.conditions.empty() &&
             !tmpl.HasFlag(ITEM_FLAG_MULTI_DROP));
        if (wasCounted && loot.unlootedCount)
            --loot.unlootedCount;
    }

    // A body with nobody behind it. A hero the human is DRIVING (.party swap)
    // keeps a merely PAUSED PlayerbotAI, so it is a person right now.
    static bool IsBotBody(Player* member)
    {
        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(member);
        return ai && !ai->IsRealPlayer() && !member->HasUnitFlag(UNIT_FLAG_POSSESSED);
    }

    static bool GroupHasRealPerson(Group* group)
    {
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            if (Player* member = itr->GetSource())
                if (member->GetSession() && !IsBotBody(member))
                    return true;
        return false;
    }

    // ---- Party-wide KEY sharing ---------------------------------------------
    // Every item id that some lock accepts as its KEY (LOCK_KEY_ITEM). Built once
    // from Lock.dbc. Catches doors/chests that need the key sitting in the bags.
    static std::unordered_set<uint32> const& LockKeyItemIds()
    {
        static std::unordered_set<uint32> ids;
        static std::once_flag once;
        std::call_once(once, []
        {
            for (uint32 i = 1; i < sLockStore.GetNumRows(); ++i)
                if (LockEntry const* lock = sLockStore.LookupEntry(i))
                    for (uint8 j = 0; j < MAX_LOCK_CASE; ++j)
                        if (lock->Type[j] == LOCK_KEY_ITEM && lock->Index[j])
                            ids.insert(lock->Index[j]);
        });
        return ids;
    }

    // "Key-like" = an item the WHOLE party needs its own copy of because it OPENS
    // something in the world. Three signals, any of which qualifies:
    //   1. an on-use OPEN_LOCK spell (e.g. the ZF Executioner's Key, spell 10738,
    //      that opens the Troll Cage — note it is item-class CONSUMABLE, not KEY,
    //      so a class check alone would miss it),
    //   2. an explicit KEY-class item (most dungeon/door keys),
    //   3. it is the item-key of some lock (doors that want the key in the bags).
    static bool IsPartyKeyItem(ItemTemplate const* tmpl)
    {
        if (!tmpl) return false;
        if (tmpl->Class == ITEM_CLASS_KEY) return true;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            if (uint32 const sid = tmpl->Spells[i].SpellId)
                if (SpellInfo const* si = sSpellMgr->GetSpellInfo(sid))
                    for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                        if (si->Effects[e].Effect == SPELL_EFFECT_OPEN_LOCK)
                            return true;
        return LockKeyItemIds().count(tmpl->ItemId) != 0;
    }

    // When any party member loots a key item, give every OTHER member their own
    // copy — otherwise a bot that auto-looted the dungeon key (the ZF Executioner's
    // Key report) leaves the human unable to open the gate. HasItemCount skips a
    // member who already has it; CanStoreNewItem enforces unique / max-count / bag
    // space, so this can never dupe or over-supply. Caller must have confirmed
    // shared progression. Uses StoreNewItem (not StoreLootItem), so it does NOT
    // re-fire OnPlayerLootItem — no recursion.
    static void MirrorKeyItemToParty(Player* looter, uint32 itemId, uint32 count)
    {
        if (!looter) return;
        ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(itemId);
        if (!IsPartyKeyItem(tmpl)) return;
        if (count == 0) count = 1;

        std::vector<ObjectGuid> party;
        GetPartyGuidsFor(looter->GetGUID(), party);
        for (ObjectGuid const& g : party)
        {
            if (g == looter->GetGUID()) continue;
            Player* p = ObjectAccessor::FindConnectedPlayer(g);
            if (!p || !p->IsInWorld()) continue;
            if (p->HasItemCount(itemId, count, true)) continue;   // already has enough
            ItemPosCountVec dest;
            if (p->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) != EQUIP_ERR_OK) continue;
            p->StoreNewItem(dest, itemId, /*update=*/true);
            LOG_INFO("module", "[WowPsParty Key] gave {} (id={}) x{} to {} so the whole party can open it",
                     tmpl->Name1, itemId, count, p->GetName());
        }
    }

    // Teach every class-trainer spell `p` qualifies for at its level. Shared by
    // the .party learnall command and the on-ding hook below. CanTeachSpell
    // enforces level/skill/prereqs and the do/while picks up follow-up ranks a
    // freshly-learned spell unlocks.
    uint32 LearnAllClassSpells(Player* p)
    {
        if (!p) return 0;
        uint32 learned = 0;
        std::vector<Trainer::Trainer const*> const& trainers =
            sObjectMgr->GetClassTrainers(p->getClass());
        bool hadNew;
        do
        {
            hadNew = false;
            for (Trainer::Trainer const* trainer : trainers)
            {
                if (!trainer->IsTrainerValidForPlayer(p))
                    continue;
                for (Trainer::Spell const& ts : trainer->GetSpells())
                {
                    if (!trainer->CanTeachSpell(p, &ts))
                        continue;
                    if (ts.IsCastable())
                        p->CastSpell(p, ts.SpellId, true);
                    else
                        p->learnSpell(ts.SpellId, false);
                    ++learned;
                    hadNew = true;
                }
            }
        } while (hadNew);
        return learned;
    }

    // Every WEAPON-MASTER proficiency the class+race can use (Axes, Swords,
    // Daggers, Bows, Staves, …). Weapon skills are taught by weapon-master NPCs,
    // NOT the class trainer, so LearnAllClassSpells never grants them — a fresh
    // alt would have to visit a weapon master for each. The ids are the stable
    // 3.3.5a proficiency spells; IsSpellFitByClassAndRace is the SAME class/race
    // gate the weapon master itself applies (Trainer::GetSpellState), so e.g. a
    // mage never gets axes. Idempotent (HasSpell skip), safe on any build
    // (GetSpellInfo guard).
    static constexpr uint32 kWeaponProficiencies[] = {
        196,    // Axes (one-hand)
        197,    // Two-Handed Axes
        198,    // Maces (one-hand)
        199,    // Two-Handed Maces
        200,    // Polearms
        201,    // Swords (one-hand)
        202,    // Two-Handed Swords
        227,    // Staves
        264,    // Bows
        266,    // Guns
        1180,   // Daggers
        2567,   // Thrown
        5011,   // Crossbows
        15590,  // Fist Weapons
    };

    uint32 LearnAllWeaponSkills(Player* p)
    {
        if (!p) return 0;
        uint32 learned = 0;
        for (uint32 spell : kWeaponProficiencies)
        {
            if (p->HasSpell(spell)) continue;
            if (!sSpellMgr->GetSpellInfo(spell)) continue;
            if (!p->IsSpellFitByClassAndRace(spell)) continue;  // class/race gate
            p->learnSpell(spell, false);
            ++learned;
        }
        return learned;
    }

    // Class-quest abilities: things a class normally earns from a one-off class
    // QUEST rather than a trainer, so CanTeachSpell / LearnAllClassSpells never
    // grant them. Bots don't run quests — a bot druid would reach max level with
    // no Bear Form. Teach them by level instead. Trainer upgrades (e.g. Dire Bear
    // Form) are already covered by LearnAllClassSpells; only the quest-gated base
    // abilities live here. Spell ids are the stable 3.3.5a class-quest rewards.
    struct ClassQuestSkill { uint8 cls; uint32 spell; uint8 minLevel; };
    static ClassQuestSkill const kClassQuestSkills[] =
    {
        { CLASS_WARRIOR,    71, 10 },  // Defensive Stance (lvl-10 class quest)
        { CLASS_WARRIOR,  2458, 30 },  // Berserker Stance  (lvl-30 class quest)
        { CLASS_DRUID,    5487, 10 },  // Bear Form
        { CLASS_DRUID,    6807, 10 },  // Maul  — comes WITH Bear Form, NOT in the trainer list
        { CLASS_DRUID,    6795, 10 },  // Growl — bear taunt, same (verified absent from trainer_spell)
        { CLASS_DRUID,    1066, 16 },  // Aquatic Form
        { CLASS_DRUID,     783, 16 },  // Travel Form
        { CLASS_DRUID,     768, 20 },  // Cat Form
        // Shaman: the FIRST totem of each element is a "Call of …" class-quest
        // reward, not trainer-taught (the LATER ones — Strength of Earth,
        // Flametongue, Mana Spring — are). Verified absent from trainer_spell.
        { CLASS_SHAMAN,   8071,  4 },  // Stoneskin Totem (earth)
        { CLASS_SHAMAN,   3599, 10 },  // Searing Totem   (fire)
        { CLASS_SHAMAN,   5394, 20 },  // Healing Stream Totem (water)
        { CLASS_WARLOCK,   697, 10 },  // Summon Voidwalker
        { CLASS_WARLOCK,   712, 20 },  // Summon Succubus
        { CLASS_WARLOCK,   691, 30 },  // Summon Felhunter
        { CLASS_WARLOCK,  5784, 40 },  // Summon Felsteed — basic mount (Dreadsteed @60 is trainer-taught)
        { CLASS_PALADIN, 13819, 40 },  // Summon Warhorse
        { CLASS_PALADIN, 23214, 60 },  // Summon Charger
    };

    uint32 LearnClassQuestSkills(Player* p)
    {
        if (!p) return 0;
        uint8 const cls = p->getClass();
        uint8 const lvl = p->GetLevel();
        uint32 learned = 0;
        for (ClassQuestSkill const& q : kClassQuestSkills)
        {
            if (q.cls != cls || lvl < q.minLevel) continue;
            if (p->HasSpell(q.spell)) continue;
            if (!sSpellMgr->GetSpellInfo(q.spell)) continue;  // absent on this build
            p->learnSpell(q.spell, false);
            ++learned;
            LOG_INFO("module",
                "[WowPsParty] taught class-quest spell {} to {} (class {}, level {})",
                q.spell, p->GetName(), uint32(cls), uint32(lvl));
        }

        // Summon Felguard (30146) is a Demonology TALENT spell, not in the level table
        // above — so a demo-specced warlock henchman wouldn't have its signature pet and
        // the rotation fell back to the Imp. Teach it when Demonology is the primary tree
        // (PrimaryTalentTree==1) and the lock is deep enough (50+), so a "demo lock" runs a
        // Felguard. Idempotent (HasSpell skip); the rotation's pet rule then summons it.
        if (cls == CLASS_WARLOCK && lvl >= 50 && !p->HasSpell(30146)
            && WowPsParty::PrimaryTalentTree(p) == 1 && sSpellMgr->GetSpellInfo(30146))
        {
            p->learnSpell(30146, false);
            ++learned;
            LOG_INFO("module",
                "[WowPsParty] taught Summon Felguard to demo warlock {} (level {})",
                p->GetName(), uint32(lvl));
        }
        return learned;
    }
}

namespace
{
    // Look up every loaded party member for the given account (excluding the
    // originator). Used by the XP and reputation mirrors.
    static std::vector<Player*> LoadedPartyPeers(uint32 accountId, Player* originator)
    {
        std::vector<Player*> peers;
        auto const party = sPartyMgr.GetParty(accountId);
        for (auto const& m : party)
        {
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(m.guid);
            Player* p = ObjectAccessor::FindConnectedPlayer(guid);
            if (!p || p == originator || !p->IsInWorld())
                continue;
            peers.push_back(p);
        }
        return peers;
    }

    // Count of reagent `itemId` a member can actually contribute to a craft:
    // backpack + equipped + keyring + bags, EXCLUDING items locked in an open
    // trade window. Deliberately mirrors Player::HasItemCount's predicate (and
    // excludes bank, like the vanilla reagent check) so this count and the
    // Player::DestroyItemCount used to consume — which also skips IsInTrade()
    // items — stay symmetric: never count a copy we then can't destroy.
    static uint32 UsableReagentCount(Player* p, uint32 itemId)
    {
        if (!p) return 0;
        uint32 count = 0;
        for (uint8 i = EQUIPMENT_SLOT_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (Item* it = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                if (WowPsParty::WowPsItemReadable(it) && it->GetEntry() == itemId && !it->IsInTrade())
                    count += it->GetCount();
        for (uint8 i = KEYRING_SLOT_START; i < CURRENCYTOKEN_SLOT_END; ++i)
            if (Item* it = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                if (WowPsParty::WowPsItemReadable(it) && it->GetEntry() == itemId && !it->IsInTrade())
                    count += it->GetCount();
        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = p->GetBagByPos(i))
            {
                if (!WowPsParty::WowPsItemReadable(bag))   // a freed bag in the slot would crash GetBagSize()
                    continue;
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (Item* it = bag->GetItemByPos(j))
                        if (WowPsParty::WowPsItemReadable(it) && it->GetEntry() == itemId && !it->IsInTrade())
                            count += it->GetCount();
            }
        return count;
    }

    // Whether member `p` holds a TOOL matching totem-category `cat` (blacksmith
    // hammer, mining pick, arclight spanner, runed rod, ...). Mirrors
    // Player::HasItemTotemCategory (equipment + keyring + bags) but SEH-guards
    // every slot read with WowPsItemReadable: the shared-inventory item mover can
    // leave a freed Item* in a party-mate's bag slot, and the vanilla method would
    // dereference it (the recurring dangling-item bag UAF that crashes the world
    // thread). Same guard discipline as UsableReagentCount above — use this, not
    // the vanilla method, for any read of a PEER's bags.
    static bool UsableToolCategory(Player* p, uint32 cat)
    {
        if (!p) return false;
        auto matches = [p, cat](Item* it)
        {
            if (!WowPsParty::WowPsItemReadable(it)) return false;
            // A freed-but-still-mapped bag slot can yield a plausible Item pointer
            // with a garbage entry. Never pass a null template to
            // IsTotemCategoryCompatiableWith: that helper dereferences it.
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(it->GetEntry());
            return proto && p->IsTotemCategoryCompatiableWith(proto, cat);
        };
        for (uint8 i = EQUIPMENT_SLOT_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (Item* it = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                if (matches(it))
                    return true;
        for (uint8 i = KEYRING_SLOT_START; i < CURRENCYTOKEN_SLOT_END; ++i)
            if (Item* it = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                if (matches(it))
                    return true;
        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = p->GetBagByPos(i))
            {
                if (!WowPsParty::WowPsItemReadable(bag))   // a freed bag in the slot would crash GetBagSize()
                    continue;
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (Item* it = bag->GetItemByPos(j))
                        if (matches(it))
                            return true;
            }
        return false;
    }

    // Finish a quest turn-in on a hero WITHOUT granting the reward item: consume
    // the required/dropped quest items, free the log slot, and mark it rewarded.
    // Used when the hero's bags are too full for the reward (CanRewardQuest
    // false) and by the login reconciler. Without it, a full-bagged hero is left
    // stuck COMPLETE-but-not-rewarded — an orphan "?" turn-in marker, quest items
    // clogging its bags, and the NEXT turn-in's CanRewardQuest then failing too
    // (the desync Viv hit). The leader already received the actual reward into
    // the shared inventory, so the hero only needs lockstep + a clean bag.
    static void ForceCompleteTurnIn(Player* p, Quest const* quest)
    {
        if (!p || !quest) return;
        uint32 const questId = quest->GetQuestId();

        // Consume required + dropped quest items (mirrors Player::RewardQuest).
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            if (ItemTemplate const* it = sObjectMgr->GetItemTemplate(quest->RequiredItemId[i]))
            {
                if (quest->RequiredItemCount[i] > 0 && it->Bonding == BIND_QUEST_ITEM
                    && !quest->IsRepeatable()
                    && !p->HasQuestForItem(quest->RequiredItemId[i], questId, true))
                    p->DestroyItemCount(quest->RequiredItemId[i], 9999, true);
                else if (quest->RequiredItemCount[i] > 0)
                    p->DestroyItemCount(quest->RequiredItemId[i], quest->RequiredItemCount[i], true);
            }
        }
        for (uint8 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
        {
            if (ItemTemplate const* it = sObjectMgr->GetItemTemplate(quest->ItemDrop[i]))
            {
                if (quest->ItemDropQuantity[i] > 0 && it->Bonding == BIND_QUEST_ITEM
                    && !quest->IsRepeatable()
                    && !p->HasQuestForItem(quest->ItemDrop[i], questId))
                    p->DestroyItemCount(quest->ItemDrop[i], 9999, true);
                else if (quest->ItemDropQuantity[i] > 0)
                    p->DestroyItemCount(quest->ItemDrop[i], quest->ItemDropQuantity[i], true);
            }
        }
        p->TakeQuestSourceItem(questId, false);
        p->RemoveTimedQuest(questId);
        uint16 const slot = p->FindQuestSlot(questId);
        if (slot < MAX_QUEST_LOG_SIZE)
            p->SetQuestSlot(slot, 0);
        p->RemoveActiveQuest(questId);
        p->SetRewardedQuest(questId);          // lockstep on quest chains
        p->learnQuestRewardedSpells(quest);    // any spell/recipe the turn-in grants
    }
}

namespace WowPsParty
{
    // ---- Emote mirroring -------------------------------------------------
    // "/dance and my heroes dance too." A companion has no game client, so
    // nothing turns the leader's CMSG_TEXT_EMOTE into an animation for it —
    // this does for the whole party what WorldSession::HandleTextEmoteOpcode
    // does for the one real player who typed it.
    //
    // Scope is exactly the emotes the core itself drives server-side: the
    // looping dance state and every one-shot. /sit, /sleep and /kneel reach a
    // real player as a client-side stand state and are deliberately left alone
    // — a seated companion is stood back up by the follow ticker's next 1 Hz
    // pass (PartyFollow.cpp), and one carrying any party power-regen buff
    // (Blessing of Wisdom, Mana Spring) reads as mid-drink to BotIsConsuming
    // (PartyRotation.cpp), which would park the party in a fake drink hold.

    // Companions holding a mirrored dance -> the leader they copied it from.
    // Guid counters, not Player*: either side can log out between passes.
    // CMSG_TEXT_EMOTE is PROCESS_THREADSAFE (Opcodes.cpp), so the hook runs on
    // a map-update thread while the clearing pass runs on the world thread.
    static std::mutex g_danceMutex;
    static std::unordered_map<uint32, ObjectGuid> g_mirroredDances;

    static float EmoteListenRange()
    {
        return sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE);
    }

    // The "<Name> dances with <target>." line nearby clients print. The core
    // builds this per-receiver locale (Acore::EmoteChatBuilder), but that
    // builder is private to ChatHandler.cpp and the target's name is its only
    // localised part — one packet at the default locale is the same text on
    // this realm.
    static void SendMirroredEmoteText(Player* bot, uint32 textEmote, uint32 emoteNum, Unit const* target)
    {
        std::string const name = target ? target->GetName() : "";
        WorldPacket data(SMSG_TEXT_EMOTE, 20 + name.size());
        data << bot->GetGUID();
        data << uint32(textEmote);
        data << uint32(emoteNum);
        data << uint32(name.size());
        if (name.size() > 1)
            data << name;
        else
            data << uint8(0);
        // self = false: the receiver is every real client watching the bot; the
        // bot's own fake session has nothing to render.
        bot->SendMessageToSetInRange(&data, EmoteListenRange(), false);
    }

    // Play the animation, following the core's switch. False = this emote does
    // nothing for a clientless companion, so it must not claim the emote in
    // chat either — a hero that prints "sits down." while still standing is
    // worse than one that stays out of it.
    static bool ApplyMirroredEmote(Player* bot, uint32 emoteAnim, ObjectGuid leaderGuid)
    {
        switch (emoteAnim)
        {
            case EMOTE_STATE_SLEEP:
            case EMOTE_STATE_SIT:
            case EMOTE_STATE_KNEEL:
                return false;                           // stand states — see the note above
            case EMOTE_ONESHOT_NONE:
                return true;                            // text-only emote, nothing to animate
            case EMOTE_STATE_DANCE:
            {
                bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_DANCE);
                std::lock_guard<std::mutex> lock(g_danceMutex);
                g_mirroredDances[bot->GetGUID().GetCounter()] = leaderGuid;
                return true;
            }
            default:
                bot->HandleEmoteCommand(emoteAnim);     // one-shot, no state left behind
                return true;
        }
    }

    // A companion is eligible to copy the leader only if the emote would read as
    // part of the same scene: alive, idle, and close enough that the mirrored
    // chat line reaches the same people the leader's did.
    static bool CanMirrorEmoteTo(Player* bot, Player* leader)
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
            return false;
        if (bot->HasUnitState(UNIT_STATE_DIED))         // feign death — the core skips it too
            return false;
        if (!sPlayerbotsMgr.GetPlayerbotAI(bot))        // managed companion only
            return false;
        if (bot->IsInCombat())                          // fighting beats dancing
            return false;
        if (bot->IsInFlight() || bot->GetVehicle())     // strapped in; nothing to animate
            return false;
        if (bot->IsBeingTeleported())
            return false;
        // InMap, not the bare IsWithinDist: that one skips the map / instance /
        // phase test, so a companion stranded in another copy of the same map id
        // would compare as standing right here.
        return bot->IsWithinDistInMap(leader, EmoteListenRange(), false);
    }

    // Copy `leader`'s text emote onto every companion it is running. Heroes,
    // hired alts and henchmen alike — GetPartyGuidsFor is the follow-directive
    // roster, so a second human's bots are correctly out of scope.
    // Deliberately does NOT re-fire the emote's achievement credit or the
    // CreatureAI::ReceiveEmote script call: the leader's own emote already did,
    // and four echoes would re-trigger emote-gated quest scripts.
    static void MirrorTextEmoteToParty(Player* leader, uint32 textEmote, uint32 emoteNum, ObjectGuid targetGuid)
    {
        if (!IsEnabled() || !leader || !leader->GetSession() || !leader->IsInWorld())
            return;
        if (sPlayerbotsMgr.GetPlayerbotAI(leader))      // only a human drives the party
            return;

        EmotesTextEntry const* em = sEmotesTextStore.LookupEntry(textEmote);
        if (!em)
            return;

        std::vector<ObjectGuid> guids;
        GetPartyGuidsFor(leader->GetGUID(), guids);

        uint32 mirrored = 0;
        for (ObjectGuid const& g : guids)
        {
            if (g == leader->GetGUID())
                continue;
            Player* bot = ObjectAccessor::FindConnectedPlayer(g);
            if (!CanMirrorEmoteTo(bot, leader))
                continue;
            if (!ApplyMirroredEmote(bot, em->textid, leader->GetGUID()))
                continue;

            SendMirroredEmoteText(bot, textEmote, emoteNum,
                                  targetGuid ? ObjectAccessor::GetUnit(*bot, targetGuid) : nullptr);
            ++mirrored;
        }

        if (mirrored && IsLogVerbose())
            LOG_INFO("module", "[WowPsParty] emote {} from {} mirrored to {} companion(s)",
                     textEmote, leader->GetName(), mirrored);
    }

    // A mirrored dance lasts exactly as long as the leader's own. The core drops
    // the leader's UNIT_NPC_EMOTESTATE the moment their client reports movement
    // (MovementHandler.cpp); a companion has no client to send that, so this
    // reads the leader's field as the authority rather than inventing a timer —
    // which also survives the follow ticker's idle wander, where watching the
    // companion's own feet would end the dance after a few seconds of standing
    // together. Combat and death end it early, as they do for a player.
    static void ClearFinishedDances()
    {
        std::lock_guard<std::mutex> lock(g_danceMutex);
        for (auto it = g_mirroredDances.begin(); it != g_mirroredDances.end(); )
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(it->first));
            // Gone, or something else owns the field now (death clears it) —
            // drop the record without touching anyone.
            if (!bot || !bot->IsInWorld()
                || bot->GetUInt32Value(UNIT_NPC_EMOTESTATE) != EMOTE_STATE_DANCE)
            {
                it = g_mirroredDances.erase(it);
                continue;
            }
            Player const* leader = ObjectAccessor::FindConnectedPlayer(it->second);
            bool const leaderDancing = leader && leader->IsInWorld()
                && leader->GetUInt32Value(UNIT_NPC_EMOTESTATE) == EMOTE_STATE_DANCE;
            if (leaderDancing && !bot->IsInCombat())
            {
                ++it;
                continue;
            }
            bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_ONESHOT_NONE);
            if (IsLogVerbose())
                LOG_INFO("module", "[WowPsParty] {} stopped dancing ({})",
                         bot->GetName(), leaderDancing ? "in combat" : "leader stopped");
            it = g_mirroredDances.erase(it);
        }
    }
}

namespace
{
    // Which per-player list carries this entry's looted state, if any. The shared
    // LootItem::is_looted flag is only the truth for a plain entry: the core keeps
    // per-player state for quest, free-for-all and condition-gated drops, and
    // Player::StoreLootItem never even sets the shared flag for a free-for-all
    // one. Membership of that list is also what makes the entry ADDRESSABLE —
    // LootItemInSlot / GetMaxSlotInLootFor size a player's slot space from it, and
    // the lists are built once at kill time for group members within loot-reward
    // distance. So a player with no entry cannot take the item at all, now or
    // later, and must not be treated as someone still owed it.
    QuestItemMap const* PerPlayerLootStateFor(Loot const& loot, LootItem const& li, bool questSlot)
    {
        if (questSlot)              return &loot.GetPlayerQuestItems();
        if (li.freeforall)          return &loot.GetPlayerFFAItems();
        if (!li.conditions.empty()) return &loot.GetPlayerNonQuestNonFFAConditionalItems();
        return nullptr;
    }

    bool EntryStillTakeableBy(Loot const& loot, LootItem const& li, uint8 index,
                              Player* p, bool questSlot)
    {
        // The shared flag is authoritative when SET: the item is physically off
        // the body, so nobody can take it — including via a per-player list that
        // the taker (our auto-loot pass, which stores directly rather than through
        // StoreLootItem) never updated.
        if (li.is_looted) return false;

        QuestItemMap const* perPlayer = PerPlayerLootStateFor(loot, li, questSlot);
        if (!perPlayer) return true;

        auto const itr = perPlayer->find(p->GetGUID());
        if (itr == perPlayer->end() || !itr->second) return false;
        for (QuestItem const& qi : *itr->second)
            if (qi.index == index)
                return !qi.is_looted;
        return false;
    }

    // Every non-bot player whose loot rights a skin of `corpse` must respect.
    // Both groups are consulted because they differ: a bot can tap the kill from
    // outside the human's own Group object. A hero the human is currently DRIVING
    // (.party swap) carries a PlayerbotAI that is merely paused, so it must count
    // as a human here — quest credit is per character, and it may be the one on
    // the quest. Ghosts are kept too: a player who died on the pull is running
    // back for exactly that corpse.
    std::vector<Player*> HumanLootClaimants(Player* member, Creature* corpse)
    {
        std::vector<Player*> humans;

        auto add = [&humans](Player* p)
        {
            if (!p || !p->IsInWorld()) return;
            if (sPlayerbotsMgr.GetPlayerbotAI(p) && !p->HasUnitFlag(UNIT_FLAG_POSSESSED)) return;
            if (std::find(humans.begin(), humans.end(), p) == humans.end())
                humans.push_back(p);
        };
        auto addGroup = [&add](Group* g)
        {
            if (!g) return;
            for (GroupReference* itr = g->GetFirstMember(); itr != nullptr; itr = itr->next())
                add(itr->GetSource());
        };

        add(member);
        if (member) addGroup(member->GetGroup());
        add(corpse->GetLootRecipient());
        addGroup(corpse->GetLootRecipientGroup());
        return humans;
    }

    // The human who can still take this normal-slot entry AS A QUEST OBJECTIVE.
    // Quest drops reach items[] as condition-gated (CONDITION_QUESTTAKEN) rows —
    // the dedicated quest_items[] list is walked separately. HasQuestForItem is
    // the same predicate the core's own HaveQuestLootForPlayer uses, and it
    // self-clears the instant the player has collected enough of the item, so
    // nothing stays reserved after the objective is met. Cheapest test first: on
    // a corpse the party already vacuumed, every entry is looted and we stop
    // before the condition and quest-log walks.
    Player* QuestDropClaimant(Loot const& loot, LootItem const& li, uint8 index,
                              ObjectGuid src, std::vector<Player*> const& humans)
    {
        for (Player* p : humans)
            if (EntryStillTakeableBy(loot, li, index, p, false)
                && li.AllowedForPlayer(p, src)
                && p->HasQuestForItem(li.itemid))
                return p;
        return nullptr;
    }

    // The human who still has a QUEST drop waiting on this corpse. Deliberately
    // distance-independent, and deliberately indifferent to whether they are
    // alive: quest loot is irreplaceable and a player who died on the pull is
    // running back for exactly that corpse, so the skin waits until it is looted
    // or the body despawns. AllowedForPlayer plus the per-player list check are
    // what keep this from over-blocking — a drop nobody in the party can take
    // leaves the corpse skinnable.
    Player* PendingQuestLootOwner(Creature* corpse, std::vector<Player*> const& humans)
    {
        Loot const& loot = corpse->loot;
        ObjectGuid const src = corpse->GetGUID();

        for (uint8 i = 0; i < loot.quest_items.size(); ++i)
        {
            LootItem const& li = loot.quest_items[i];
            for (Player* p : humans)
                if (EntryStillTakeableBy(loot, li, i, p, true) && li.AllowedForPlayer(p, src))
                    return p;
        }
        for (uint8 i = 0; i < loot.items.size(); ++i)
            if (Player* p = QuestDropClaimant(loot, loot.items[i], i, src, humans))
                return p;
        return nullptr;
    }

    // Tell the player about a quest drop the party deliberately did NOT pick up.
    // The auto-loot pass empties bodies so thoroughly that nobody opens a corpse
    // any more, so without this the remaining sparkle is a mystery rather than an
    // instruction — and the skinner visibly stalling on that corpse reads as a bug.
    void AnnounceQuestDropLeftOnCorpse(Player* owner, ItemTemplate const& tmpl, LootItem const& li)
    {
        if (!owner || !owner->GetSession()) return;
        uint32 const itemId = li.itemid;
        std::string const itemName = tmpl.Name1;
        std::string const msg = Acore::StringFormat(
            "|cff66ccff[Loot]|r quest drop |cffffffff|Hitem:{}::::::::1::::|h[{}]|h|r left on the corpse for you",
            itemId, itemName);
        ChatHandler(owner->GetSession()).SendSysMessage(msg);
    }

    // Tell the player about a drop the party left for the raid that is NOT being
    // rolled for (master loot, or a roll nobody was eligible for) — a roll puts up
    // its own window and says it better. The auto-loot pass empties bodies so
    // thoroughly that nobody opens a corpse any more, so without this the body
    // keeps a sparkle for no visible reason and the drop is quietly missed.
    void AnnounceRaidLootLeftOnCorpse(Player* human, ItemTemplate const& tmpl, LootItem const& li)
    {
        if (!human || !human->GetSession()) return;
        uint32 const itemId = li.itemid;
        std::string const itemName = tmpl.Name1;
        std::string const msg = Acore::StringFormat(
            "|cff66ccff[Loot]|r |cffffffff|Hitem:{}::::::::1::::|h[{}]|h|r is the raid's — "
            "left on the body, loot it to take it",
            itemId, itemName);
        ChatHandler(human->GetSession()).SendSysMessage(msg);
    }

    // Tell the player about a drop the party refused because they blacklisted it.
    // Nothing else on the corpse would show it: the body is emptied at the kill, so
    // a silent skip is indistinguishable from the item never having dropped — and a
    // blacklist you can't see working is one you stop trusting. Greyed, because it
    // is an item the player has already said they don't want.
    //
    // Says DISCARDED, never "left behind". The two sibling announcements either side
    // of this one ("left on the corpse for you", "left on the body, loot it to take
    // it") mean exactly that — the item is still there to walk back for. A declined
    // blacklist drop is destroyed (MarkPartyTookLootItem), so borrowing their verb
    // sends the player to an empty body for an item that no longer exists. It is the
    // one line that has to be unambiguous, because it is the only record the item
    // ever dropped.
    //
    // Once a minute per item, because a blacklist earns its keep on exactly the
    // things that drop off every second mob — unthrottled, the line confirming the
    // feature works IS the spam the feature was added to stop.
    constexpr uint32 BLACKLIST_ANNOUNCE_INTERVAL_MS = 60 * IN_MILLISECONDS;

    void AnnounceBlacklistedDropSkipped(Player* human, ItemTemplate const& tmpl, LootItem const& li)
    {
        if (!human || !human->GetSession()) return;

        static std::unordered_map<uint64, uint32> lastAnnounced;   // (account<<32|entry) -> ms
        static std::mutex announceMutex;
        uint64 const key = (uint64(human->GetSession()->GetAccountId()) << 32) | li.itemid;
        uint32 const now = getMSTime();
        {
            std::lock_guard<std::mutex> lock(announceMutex);
            auto it = lastAnnounced.find(key);
            if (it != lastAnnounced.end() &&
                getMSTimeDiff(it->second, now) < BLACKLIST_ANNOUNCE_INTERVAL_MS)
                return;
            lastAnnounced[key] = now;
        }

        uint32 const itemId = li.itemid;
        std::string const itemName = tmpl.Name1;
        std::string const msg = Acore::StringFormat(
            "|cff888888[Loot] discarded |Hitem:{}::::::::1::::|h[{}]|h — blacklisted, "
            "so it was not left on the body.|r",
            itemId, itemName);
        ChatHandler(human->GetSession()).SendSysMessage(msg);
    }

    // Master-loot priority: a living human standing close enough to still take
    // ordinary loot off the corpse. Unlike quest loot this is forfeited once they
    // walk away, so it only defers the skin while they are actually here. Range is
    // wide enough that a human walking up to the kill isn't beaten to it by a fast
    // skinner.
    constexpr float MASTER_LOOT_PRIORITY_RANGE = 40.0f;

    Player* NearbyOrdinaryLootOwner(Creature* corpse, std::vector<Player*> const& humans)
    {
        Loot const& loot = corpse->loot;
        ObjectGuid const src = corpse->GetGUID();
        for (Player* p : humans)
        {
            if (!p->IsAlive() || !p->IsWithinDistInMap(corpse, MASTER_LOOT_PRIORITY_RANGE))
                continue;
            if (loot.gold > 0) return p;
            for (uint8 i = 0; i < loot.items.size(); ++i)
            {
                LootItem const& li = loot.items[i];
                if (EntryStillTakeableBy(loot, li, i, p, false) && li.AllowedForPlayer(p, src))
                    return p;
            }
        }
        return nullptr;
    }
}

// Would treating `creature` as skinnable right now destroy loot a HUMAN party
// member can still take? Skinning clears the corpse's loot outright and despawns
// the body next update, so this is the one gate standing between the party's
// skinner and the player's quest drops. `reason` (optional) receives a log-ready
// explanation of the block.
//
// Every skin path must consult it — the force path in WowPsParty_ForceSkinReady
// AND the path where the engine already set UNIT_FLAG_SKINNABLE. That flag is no
// proof the body is finished: the party auto-loot pass below raises it as soon as
// the ORDINARY items are in the party bags, which is the instant of the kill.
bool WowPsParty_SkinWouldDestroyPartyLoot(Player* member, Creature* creature, std::string* reason)
{
    if (!creature || !WowPsParty::IsEnabled()) return false;

    // A group loot roll is still running on this body. Skinning calls
    // Loot::clear(), which invalidates the Roll's loot reference — the item
    // nobody has won yet would simply cease to exist mid-roll. The core arms and
    // clears this timer itself around the roll, so it is exactly the question.
    if (creature->m_groupLootTimer)
    {
        if (reason)
            *reason = "a group loot roll is still running on this corpse";
        return true;
    }

    std::vector<Player*> const humans = HumanLootClaimants(member, creature);
    if (humans.empty()) return false;

    if (Player* owner = PendingQuestLootOwner(creature, humans))
    {
        if (reason)
        {
            std::string const name = owner->GetName();
            *reason = Acore::StringFormat("unlooted quest drop {} can still take", name);
        }
        return true;
    }
    if (Player* owner = NearbyOrdinaryLootOwner(creature, humans))
    {
        if (reason)
        {
            std::string const name = owner->GetName();
            float const range = MASTER_LOOT_PRIORITY_RANGE;
            *reason = Acore::StringFormat("human {} within {}y of still-lootable corpse",
                                          name, range);
        }
        return true;
    }
    return false;
}

class PartyHooksPlayerScript : public PlayerScript
{
public:
    PartyHooksPlayerScript() : PlayerScript("PartyHooksPlayerScript", {
        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_GIVE_REPUTATION,
        PLAYERHOOK_ON_MONEY_CHANGED,
        PLAYERHOOK_ON_CREATURE_KILL,
        PLAYERHOOK_ON_STORE_NEW_ITEM,
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_QUEST_ABANDON,
        PLAYERHOOK_ON_LEARN_TAXI_NODE,
        PLAYERHOOK_ON_MAP_CHANGED,
        PLAYERHOOK_ON_REMOVE_FROM_BATTLEGROUND,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE,
        PLAYERHOOK_ON_SPELL_CAST,
        PLAYERHOOK_ON_TEXT_EMOTE
    }) { }

    // /dance, /cheer, /sit — whatever the human just did, the companions they
    // are running do too. Fires on the leader's own CMSG_TEXT_EMOTE, before the
    // core plays it for them, so the whole party moves as one.
    void OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 emoteNum, ObjectGuid guid) override
    {
        WowPsParty::MirrorTextEmoteToParty(player, textEmote, emoteNum, guid);
    }

    // Remember the last mount a human rode manually on a hero char, so that character
    // prefers the same mount when it's later AI-driven — "ride Winterspring Frostsaber
    // once and your henchman keeps riding it." Ground and flying mounts are tracked
    // independently; a char never ridden manually keeps the default auto-mount behavior.
    // Fires on every human spell cast, so reject non-mounts before any lookup. Only real
    // (human-controlled) casts count: an AI bot has a PlayerbotAI and is skipped, so a
    // bot's random fallback mount can never overwrite the stored preference.
    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!WowPsParty::IsEnabled() || !player || !spell)
            return;
        SpellInfo const* info = spell->GetSpellInfo();
        if (!info || info->Effects[0].ApplyAuraName != SPELL_AURA_MOUNTED)
            return;
        if (sPlayerbotsMgr.GetPlayerbotAI(player))                       // AI auto-mount, not manual
            return;
        if (!sPartyMgr.GetSlotForGuid(player->GetGUID().GetCounter()))   // enrolled hero only
            return;
        CheckMountStateAction::RecordManualMount(player, info);
    }

    // On-demand "Cast <spell>" party command: a human leader typing e.g.
    // "Cast Portal: Ironforge" or "Cast Heroism" in party/raid chat makes whichever
    // of their party bots knows that spell cast it. Fires on the outgoing message
    // (before it broadcasts, so the chat line still shows normally). Restricted to
    // the GROUP channels — henchmen and heroes are always in the WoW group, so party/
    // raid always reaches them, and /say is left out so a conversational "cast a wide
    // net" can't false-trigger. LANG_ADDON is skipped so WPSP protocol traffic (which
    // also rides these hooks) never touches the handler. All remaining guarding
    // (human leader, managed party, spell resolution) lives in HandleOnDemandCast.
    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override
    {
        if (!WowPsParty::IsEnabled() || !player || lang == LANG_ADDON) return;
        switch (type)
        {
            case CHAT_MSG_PARTY:
            case CHAT_MSG_PARTY_LEADER:
            case CHAT_MSG_RAID:
            case CHAT_MSG_RAID_LEADER:
                WowPsParty::HandleOnDemandCast(player, msg);
                break;
            default:
                break;
        }
    }

    // Stop managed bots from whisper-spamming humans (Desouza got whispered by the Nisse
    // heroes in Naxx). Player::Whisper drops the whisper when this returns false.
    //  - HENCHMEN: never whisper anyone.
    //  - HEROES (enrolled alts): only ever whisper their OWN leader (the human captain).
    // Real players, pool/fill bots (no leader), and ADDON traffic (the WPSP protocol rides
    // CHAT_MSG_WHISPER with LANG_ADDON) are all left untouched.
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& /*msg*/, Player* receiver) override
    {
        if (!player || !receiver) return true;
        if (type != CHAT_MSG_WHISPER || language == LANG_ADDON) return true;   // only real whispers
        if (!sPlayerbotsMgr.GetPlayerbotAI(player)) return true;               // a human whispers freely
        if (WowPsParty::IsHenchman(player->GetGUID())) return false;           // henchman -> never whisper
        ObjectGuid const leader = WowPsParty::GetLeaderFor(player->GetGUID());
        if (!leader) return true;                                             // not a managed hero -> leave it
        return receiver->GetGUID() == leader;                                // hero -> only its own leader
    }

    // After a managed party leaves a BG, the dungeon finder can be wrongly disabled
    // ("Join as Party" greyed, can't queue anything "like I'm not the leader") even
    // though the DB group is fine and the leader icon shows — a runtime state stuck on
    // the GROUP that survives relog (the in-memory group persists via the hero bots) and
    // is only cleared by kick+reinvite (a fresh group) or a server restart.
    // Log the exact post-BG group state to pin the cause, and clear a stale LFG state
    // (the most likely culprit: the group reads as "in the dungeon system"). Deferred so
    // it runs AFTER the BG-group teardown + original-group restore.
    void OnPlayerRemoveFromBattleground(Player* player, Battleground* /*bg*/) override
    {
        if (!WowPsParty::IsEnabled() || !player || !player->GetSession()) return;
        if (sPlayerbotsMgr.GetPlayerbotAI(player)) return;            // human leader only
        if (WowPsParty::CountFollowersFor(player->GetGUID()) == 0) return;   // managed party only
        ObjectGuid const guid = player->GetGUID();
        player->m_Events.AddEventAtOffset([guid]()
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(guid);
            if (!p) return;
            Group* grp = p->GetGroup();
            ObjectGuid const gg = grp ? grp->GetGUID() : ObjectGuid::Empty;
            lfg::LfgState const pState = sLFGMgr->GetState(guid);
            lfg::LfgState const gState = gg ? sLFGMgr->GetState(gg) : lfg::LFG_STATE_NONE;
            LOG_INFO("module",
                "[WowPsParty BGLeave] {} post-BG: leader={} grp={} isLFGGroup={} isBGGroup={} isRaid={} playerLfg={} groupLfg={}",
                p->GetName(), grp && grp->IsLeader(guid),
                gg.GetCounter(), grp && grp->isLFGGroup(), grp && grp->isBGGroup(),
                grp && grp->isRaidGroup(), int(pState), int(gState));
            // Clear a stale LFG state when we're NOT actually in an LFG dungeon group, so
            // the dungeon finder stops treating the party as "in the LFG system". Real
            // LFG_STATE_QUEUED (still in the finder) is left alone. LeaveLfg also pushes an
            // LFG update to the client, which re-enables the greyed "Join as Party".
            bool const inLfgGroup = grp && grp->isLFGGroup();
            if (!inLfgGroup)
            {
                if (pState != lfg::LFG_STATE_NONE && pState != lfg::LFG_STATE_QUEUED)
                    sLFGMgr->LeaveLfg(guid);
                if (gg && gState != lfg::LFG_STATE_NONE && gState != lfg::LFG_STATE_QUEUED)
                    sLFGMgr->LeaveLfg(gg);
            }
        }, std::chrono::seconds(2));
    }

    // On entering a dungeon, tell the party leader how the tank will get around here:
    // a recorded route, an auto route to the bosses still standing, or neither. Only
    // the human owner (has followers; bots never do) sees it.
    void OnPlayerMapChanged(Player* player) override
    {
        if (!player || !player->GetMap() || !player->GetMap()->IsDungeon())
            return;
        if (WowPsParty::CountFollowersFor(player->GetGUID()) == 0)
            return;   // not leading a party here — nothing to say about a tank route

        std::string const summary = WowPsParty::DescribeTankRoute(player);
        if (!summary.empty())
            ChatHandler(player->GetSession()).PSendSysMessage(summary);
    }

    // Mirror a freshly-discovered FLIGHT PATH (taxi node) to every other LOADED
    // hero on the account, so flight paths are effectively ACCOUNT-BOUND: discover
    // once on the active char and all your heroes know it. Only the active char
    // physically visits flight masters (the rest follow as bots and never talk to
    // the NPC), so without this they'd never learn a node and couldn't fly when you
    // later play them. Henchmen (hired pool bots) are excluded — not your heroes.
    // The engine only calls this for a genuinely NEW node, so no spam. The peers
    // are bots (no real client), so SetTaximaskNode on their in-memory mask is all
    // that's needed — it persists on their next character save.
    void OnPlayerLearnTaxiNode(Player const* player, uint32 nodeId) override
    {
        using namespace WowPsParty;
        if (!IsEnabled() || !player || !player->GetSession() || !nodeId) return;
        Player* learner = const_cast<Player*>(player);   // hook is const; we only read it
        if (!ProgressionShared(learner)) return;         // solo / companions off: don't mirror
        if (!sPartyMgr.GetSlotForGuid(learner->GetGUID().GetCounter()))
            return;   // discoverer isn't one of THIS account's enrolled heroes

        for (Player* peer : LoadedPartyPeers(learner->GetSession()->GetAccountId(), learner))
            if (sPartyMgr.GetSlotForGuid(peer->GetGUID().GetCounter()))   // enrolled hero, never a henchman
                peer->m_taxi.SetTaximaskNode(nodeId);
    }

    // Mirror a quest ABANDON across the party: when one party character drops a
    // quest, every other LOADED hero that still has it in their log drops it too
    // (the counterpart to the accept/turn-in mirrors). Heroes without the quest
    // are left alone. Fires from sScriptMgr->OnPlayerQuestAbandon in the engine's
    // abandon handler, so it only triggers on a real player-initiated abandon —
    // not on turn-in or auto-removal.
    void OnPlayerQuestAbandon(Player* player, uint32 questId) override
    {
        using namespace WowPsParty;
        if (g_propagatingQuest) return;
        if (!IsEnabled() || !player || !player->GetSession() || !questId) return;

        // A playerbot can invoke the normal abandon handler while cleaning its
        // own quest log. That is not a player's shared-progression decision and
        // must never fan out to the other real characters on the account.
        // (The managed-bot quest guards stop the originating removal too; this
        // is the containment layer for any future playerbot abandon path.)
        if (sPlayerbotsMgr.GetPlayerbotAI(player))
        {
            LOG_INFO("module", "[WowPsParty QuestGuard] ignored bot abandon: {} quest={}",
                     player->GetName(), questId);
            return;
        }
        if (!ProgressionShared(player)) return;   // solo: don't mirror quests
        if (!sPartyMgr.GetSlotForGuid(player->GetGUID().GetCounter()))
            return;  // abandoner isn't one of this account's party characters

        std::vector<Player*> const peers =
            LoadedPartyPeers(player->GetSession()->GetAccountId(), player);
        if (peers.empty()) return;

        // g_propagatingQuest guards the accept/reward trampolines from re-entering;
        // the engine doesn't re-fire OnPlayerQuestAbandon for these RemoveActiveQuest
        // calls, but set it anyway for parity and belt-and-braces.
        g_propagatingQuest = true;
        uint32 dropped = 0;
        for (Player* p : peers)
        {
            uint16 const slot = p->FindQuestSlot(questId);
            if (slot >= MAX_QUEST_LOG_SIZE) continue;   // hero doesn't have it

            // Mirror the engine's abandon sequence (QuestHandler.cpp). The first
            // step is its gate: if the provided source item can't be returned
            // (e.g. an equipped non-empty quest bag), the engine refuses the
            // abandon — so skip this hero too rather than half-removing it.
            if (!p->TakeQuestSourceItem(questId, true)) continue;
            p->RemoveTimedQuest(questId);
            p->AbandonQuest(questId);           // destroy quest-received items
            p->RemoveActiveQuest(questId);      // drop the active status (+ DB)
            p->SetQuestSlot(slot, 0);           // clear the visible log slot
            ++dropped;
        }
        g_propagatingQuest = false;

        if (dropped)
            LOG_INFO("module",
                "[WowPsParty] {} abandoned quest {} — mirrored to {} hero(es).",
                player->GetName(), questId, dropped);
    }

    // Auto-learn on ding: when a party member levels up, immediately teach
    // every class spell now available — so a 5-char party never has to trek
    // to a trainer. Fires for the controlled char and every follower bot
    // (they level via the mirrored XP hook).
    void OnPlayerLevelChanged(Player* player, uint8 /*oldlevel*/) override
    {
        using namespace WowPsParty;
        if (!IsEnabled() || !player || !player->GetSession()) return;
        // Auto-train an ENROLLED party character (the user's hero alts, controlled or
        // bot) OR the human's CONTROLLED character even when it isn't enrolled — a
        // fresh 6th+ character levelled past a full party-of-5 still trains itself,
        // matching the login backfill (which already learns for any human). Only a
        // NON-enrolled BOT (a random-pool companion that isn't ours) is skipped, so
        // we never touch foreign bots. (The bug: the old enrollment-only gate left a
        // new, un-enrollable main missing every spell it dinged into.)
        if (!sPartyMgr.GetSlotForGuid(player->GetGUID().GetCounter())
            && sPlayerbotsMgr.GetPlayerbotAI(player))
            return;

        // Snapshot the spell-chains already known so we can report only the
        // genuinely NEW abilities (not higher ranks of spells already in use —
        // those don't need a rotation change since rules auto-pick top rank).
        std::unordered_set<uint32> knownChains;
        for (auto const& kv : player->GetSpellMap())
        {
            if (kv.second->State == PLAYERSPELL_REMOVED) continue;
            uint32 const root = sSpellMgr->GetFirstSpellInChain(kv.first);
            knownChains.insert(root ? root : kv.first);
        }

        LearnAllWeaponSkills(player);   // weapon proficiencies — kept OUT of `n` so they
                                        // don't trigger the "new abilities" report (not rotation-relevant)
        uint32 const n = LearnAllClassSpells(player) + LearnClassQuestSkills(player);
        if (!n) return;

        // Collect names of the new abilities (new chain-roots, active + shown).
        std::vector<std::string> newNames;
        std::unordered_set<uint32> seen;
        for (auto const& kv : player->GetSpellMap())
        {
            if (kv.second->State == PLAYERSPELL_REMOVED) continue;
            uint32 const root = sSpellMgr->GetFirstSpellInChain(kv.first);
            uint32 const key  = root ? root : kv.first;
            if (knownChains.count(key)) continue;       // already had it
            if (!seen.insert(key).second) continue;     // dedupe ranks
            SpellInfo const* si = sSpellMgr->GetSpellInfo(kv.first);
            if (!si) continue;
            if (si->IsPassive()) continue;              // not rotation-relevant
            if (si->HasAttribute(SPELL_ATTR0_IS_TRADESKILL)) continue;
            if (si->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY)) continue;
            char const* nm = si->SpellName[0];
            if (nm && *nm) newNames.push_back(nm);
        }

        LOG_INFO("module",
            "[WowPsParty] {} reached level {} — auto-learned {} spell(s), {} new",
            player->GetName(), uint32(player->GetLevel()), n, uint32(newNames.size()));
        if (newNames.empty()) return;

        // Auto-learn above ALWAYS runs (every player levels their full kit). The
        // chat reminder below, though, is a party-of-5 nudge to configure the
        // bots' rotation — skip it in SOLO mode (companions disabled): there are
        // no bots to slot the new spells into.
        if (!GetAccountSettings(player->GetSession()->GetAccountId()).spawnCompanions)
            return;

        std::string list;
        uint32 shown = 0;
        for (auto const& nm : newNames)
        {
            if (shown >= 6) { list += ", …"; break; }
            if (!list.empty()) list += ", ";
            list += nm;
            ++shown;
        }

        // Send the reminder to the char the player is actually watching: the
        // leader for a follower bot, or the leveling char itself if controlled.
        Player* recipient = player;
        if (ObjectGuid const lg = GetLeaderFor(player->GetGUID()))
            if (Player* leader = ObjectAccessor::FindConnectedPlayer(lg))
                recipient = leader;
        if (recipient->GetSession())
            ChatHandler(recipient->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r {} (L{}) learned: |cffffff00{}|r — open the "
                "rotation editor (Y) to slot them in.",
                player->GetName(), uint32(player->GetLevel()), list);
    }

    // When any party member receives an item that's required for one of
    // their OWN active quests OR any peer's active quest, mirror the
    // item into every other party member's bags. Each mirrored AddItem
    // calls Player::ItemAddedQuestCheck internally, so quest progress
    // bumps naturally — no separate credit propagation needed.
    //
    // Gating: we only mirror if at least one *other* member has the
    // item as a quest requirement. This prevents 5× duplication of
    // every grey vendor drop while still solving the wolf-pelt case.
    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 count) override
    {
        using namespace WowPsParty;
        if (!IsEnabled() || g_propagatingLoot) return;
        if (!player || !item) return;
        NotifyPartyInventoryChanged(player);
        if (!player->GetSession()) return;

        // A HIRED ALT is the player's real character, parked exactly as they left
        // it — nothing may mutate its bags. Skip BOTH the soul-shard trim and the
        // shared quest-item mirror below; its loot stays its own, hidden until the
        // player logs into it.
        if (WowPsParty::IsHiredAlt(player->GetGUID())) return;

        // Cap soul shards on a bot warlock (hero or henchman) at one. The
        // playerbot AI casts Drain Soul on every kill and never spends the
        // shards, and Soul Shard is non-stacking (one item per bag slot), so
        // they pile up and flood the bags. A human-played warlock (no
        // PlayerbotAI) manages their own shards and needs several on hand, so
        // it is left untouched. Every shard-creation path funnels through
        // StoreNewItem -> this hook, so this is the single choke point. Spare
        // the just-stored `item` (destroy the OLDER shards) — DoCreateItem and
        // the loot path still use that pointer after this hook returns.
        constexpr uint32 SOUL_SHARD_ITEM_ID = 6265;
        if (item->GetEntry() == SOUL_SHARD_ITEM_ID && sPlayerbotsMgr.GetPlayerbotAI(player))
        {
            WowPsParty::TrimSoulShardsToOne(player, item);
            return;   // shards are never quest items — nothing to mirror below
        }

        if (!ProgressionShared(player)) return;   // solo: keep own loot

        ItemTemplate const* tmpl = item->GetTemplate();
        if (!tmpl) return;
        uint32 const itemId = tmpl->ItemId;
        if (!itemId || !count) return;

        uint32 const account = player->GetSession()->GetAccountId();
        auto peers = LoadedPartyPeers(account, player);
        if (peers.empty()) return;

        std::vector<Player*> wanters;
        wanters.reserve(peers.size());
        for (Player* peer : peers)
        {
            for (uint8 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
            {
                uint32 const questId = peer->GetQuestSlotQuestId(i);
                if (!questId) continue;
                Quest const* qInfo = sObjectMgr->GetQuestTemplate(questId);
                if (!qInfo) continue;
                if (peer->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE) continue;
                bool wants = false;
                for (uint8 j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
                {
                    if (qInfo->RequiredItemId[j] == itemId &&
                        peer->GetItemCount(itemId, false) < qInfo->RequiredItemCount[j])
                    {
                        wants = true;
                        break;
                    }
                }
                if (wants) { wanters.push_back(peer); break; }
            }
        }
        if (wanters.empty()) return;

        g_propagatingLoot = true;
        for (Player* peer : wanters)
        {
            peer->AddItem(itemId, count);
        }
        g_propagatingLoot = false;
    }

    // Auto-loot every creature a party member kills. mod-playerbots' own loot
    // strategy was unreliable for our setup (per Kevin's "bots dont loot at
    // all"). Simpler and more reliable to handle it here:
    //   * Iterate the creature's loot items
    //   * Drop each into the first party member that has bag space
    //   * Auto-pickup the gold to the killer (FFA semantics)
    // Gear shared across the whole party — Cuid2-style "the party is one wallet
    // and one inventory." A RAID is the exception: anything at or above the
    // group's loot threshold is left on the body for the group's own rules to
    // hand out (see RaidLootRules).
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (!WowPsParty::IsEnabled() || !killer || !killed) return;
        if (!killer->GetSession()) return;
        if (!WowPsParty::ProgressionShared(killer)) return;  // solo: normal loot

        // Only auto-loot if the killer is in a WowPsParty party (avoid
        // stealing loot from non-party players who happen to be on a
        // bot-shared server).
        uint32 const account = killer->GetSession()->GetAccountId();
        auto const party = sPartyMgr.GetParty(account);
        if (party.empty()) return;

        // Our shared-inventory shortcut is for a single human and their own
        // companions. Once another person joins the group/raid it must not
        // preempt the core's loot method: Group Loot and Need Before Greed need
        // the corpse intact to create their roll windows, while other methods
        // retain their normal Blizzard semantics. A bot can land the kill, so
        // GetExternalHumanGroupMember excludes that bot's own captain.
        if (Player* externalHuman = WowPsParty::GetExternalHumanGroupMember(killer))
        {
            WowPsParty::LogExternalHumanLootHandoff(killer, externalHuman, killed,
                                                     killer->GetGroup());
            return;
        }

        Loot* loot = &killed->loot;
        if (loot->isLooted()) return;

        // Build candidate list of party HEROES in the same map who can take
        // items. GetParty returns only enrolled heroes (slot 0..4), never
        // hired henchmen, so a henchman never receives shared loot even if it
        // landed the kill. The killer is deliberately NOT pushed first: loot
        // is spread evenly across all heroes regardless of who got the kill
        // (the per-item ordering below does the balancing).
        std::vector<Player*> takers;
        for (auto const& m : party)
        {
            ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(m.guid);
            Player* p = ObjectAccessor::FindConnectedPlayer(og);
            if (!p || !p->IsInWorld()) continue;
            // Same-Map* only (not just same map id): storing into a peer on
            // another map/instance races that peer's save thread and can free
            // an item still queued in it (the use-after-free seen in the shared
            // reagent path — see WowPsParty_TakeReagent). Compare Map* because
            // two instances of one map are distinct update contexts.
            if (p->GetMap() != killer->GetMap()) continue;
            takers.push_back(p);
        }
        if (takers.empty()) return;

        // Gold first — to the killer, then mirrored across party by the
        // existing OnPlayerMoneyChanged hook.
        if (loot->gold > 0)
        {
            killer->ModifyMoney(int32(loot->gold));
            loot->gold = 0;
        }

        // Announce every hero pickup to the HUMAN at the keyboard (the party
        // leader), not to whichever hero bot actually grabbed it — otherwise a
        // bot-landed kill announced the loot to the BOT's session and the player
        // never saw it. `killer` is either the human (landed the kill) or a
        // managed bot whose directive leader IS the human; GetLeaderFor returns
        // empty for the leader itself, so fall back to `killer`. Only enrolled
        // heroes are ever takers here (GetParty excludes henchmen), so henchman
        // pickups are never announced — exactly what we want.
        ObjectGuid const leaderGuid = WowPsParty::GetLeaderFor(killer->GetGUID());
        Player* human = leaderGuid ? ObjectAccessor::FindConnectedPlayer(leaderGuid) : killer;
        if (!human || !human->GetSession()) human = killer;

        // We are about to empty this body, so nobody will ever open it — start the
        // raid's rolls now, before the pass below decides what it may take (it reads
        // the is_blocked the roll sets).
        Group* const raid = WowPsParty::RaidLootRulesForCorpse(killer, killed);
        if (raid)
            WowPsParty::StartRaidLootRolls(raid, killed);

        // Items: iterate, for each non-FFA non-looted item, find a taker
        // with bag space, store it. Announce each pickup in chat so the
        // user sees what the party scooped up.
        std::vector<Player*> const humans = HumanLootClaimants(human, killed);
        for (size_t i = 0; i < loot->items.size(); ++i)
        {
            LootItem& li = loot->items[i];
            if (li.is_looted || li.freeforall) continue;

            ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(li.itemid);
            if (!tmpl) continue;

            // A QUEST drop stays on the body. Quest credit is per character, so
            // hoovering it into whichever hero had bag room costs the player the
            // objective and leaves them fishing it back out of a companion's bags.
            // Only quest objectives are held back — profession recipes and other
            // condition-gated drops still flow into the shared party bags, since a
            // hero may be the one who qualifies for them and this pass is a bot's
            // only way to pick anything up. The skin guard keeps the party's
            // skinner off the corpse until the player has taken it.
            if (Player* owner = QuestDropClaimant(*loot, li, uint8(i), killed->GetGUID(), humans))
            {
                AnnounceQuestDropLeftOnCorpse(owner, *tmpl, li);
                continue;
            }

            // In a raid the group's loot rules outrank the shared party bags for
            // anything at or above its threshold: it stays on the body, either
            // because a roll is now running on it or because the raid's method
            // hands it out through the loot window.
            if (raid && WowPsParty::RaidRollsForItem(raid, li, *tmpl))
            {
                if (!li.is_blocked)
                    AnnounceRaidLootLeftOnCorpse(human, *tmpl, li);
                LOG_INFO("module",
                    "[WowPsParty Loot] left {} (quality {}) on corpse {} for the raid — "
                    "method={} threshold={} rolling={}",
                    tmpl->Name1, uint32(tmpl->Quality), killed->GetGUID().GetCounter(),
                    uint32(raid->GetLootMethod()), uint32(raid->GetLootThreshold()),
                    li.is_blocked ? 1 : 0);
                continue;
            }

            // The player's own blacklist (Interface -> AddOns -> WowPsParty -> Loot
            // blacklist). Deliberately LAST of the three carve-outs: a quest drop is
            // still the player's to collect and the raid's rules still outrank the
            // shared bags, so blacklisting an item must not swallow a drop that was
            // about to be rolled for. Retired rather than left lying: an item left on
            // the body keeps it lootable, which both denies the corpse to the party's
            // skinner (NearbyOrdinaryLootOwner) and leaves a sparkle on every kill.
            if (WowPsParty::LootBlacklisted(account, li.itemid))
            {
                AnnounceBlacklistedDropSkipped(human, *tmpl, li);
                // Named lvalues: Acore::StringFormat forwards through
                // fmt::make_format_args, which binds non-const references only.
                std::string const skippedName = tmpl->Name1;
                uint32 const skippedId = li.itemid;
                uint32 const skippedCount = li.count;
                uint32 const corpse = killed->GetGUID().GetCounter();
                // Verbose-only: Server.log already runs to ~350 MB, and a blacklisted
                // common drop would add a line to it on every second kill.
                if (WowPsParty::IsLogVerbose())
                    LOG_INFO("module",
                        "[WowPsParty Loot] blacklisted {} (entry {}) x{} discarded from corpse {} "
                        "for account {}",
                        skippedName, skippedId, skippedCount, corpse, account);
                WowPsParty::MarkPartyTookLootItem(*loot, li, *tmpl);
                continue;
            }

            // Spread loot evenly: for each item prefer the hero with the most
            // free bag space. When bags are equally full this naturally
            // round-robins (each pickup drops that hero's free count by one, so
            // the next item lands on a different hero), and a hero whose bags
            // are full sorts last and is skipped -- the item overflows to a
            // hero that still has room. The human's own character is just
            // another hero in this list, so a full human inventory overflows to
            // the heroes exactly the same way. Re-sorted per item because the
            // previous StoreNewItem changed the free-space counts.
            std::stable_sort(takers.begin(), takers.end(),
                [](Player const* a, Player const* b)
                {
                    return a->GetFreeInventorySpace() > b->GetFreeInventorySpace();
                });

            for (Player* taker : takers)
            {
                ItemPosCountVec dest;
                InventoryResult const res =
                    taker->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, li.itemid, li.count);
                if (res != EQUIP_ERR_OK) continue;
                // Preserve the random-property roll the loot already had
                // (li.randomPropertyId). Earlier version regenerated, giving
                // players a DIFFERENT enchant/suffix than what was on the
                // corpse -- silently worse RNG outcome for the player.
                Item* newItem = taker->StoreNewItem(dest, li.itemid, /*update=*/true,
                                                    li.randomPropertyId);
                if (newItem)
                {
                    WowPsParty::MarkPartyTookLootItem(*loot, li, *tmpl);
                    // A KEY (e.g. the ZF Executioner's Key) that lands on one hero
                    // leaves the human unable to open the gate — give the whole
                    // party a copy. No-op for normal loot.
                    WowPsParty::MirrorKeyItemToParty(taker, li.itemid, li.count);
                    // Loot announcement. Use named lvalues for the fmt args
                    // because Acore::StringFormat forwards via
                    // fmt::make_format_args which requires lvalue references.
                    std::string const takerName = taker->GetName();
                    std::string const itemName = tmpl->Name1;
                    uint32 const itemId = li.itemid;
                    uint32 const itemCount = li.count;
                    std::string const lootMsg = Acore::StringFormat(
                        "|cff66ccff[Loot]|r {} picked up |cffffffff|Hitem:{}::::::::1::::|h[{}]|h|r x{}",
                        takerName, itemId, itemName, itemCount);
                    if (human && human->GetSession())
                        ChatHandler(human->GetSession()).SendSysMessage(lootMsg);
                    break;
                }
            }
        }

        // Backstop for the wasCounted guess above. That predicate is only HALF of
        // what Loot::FillLoot does: the fill also skips its ++unlootedCount for any
        // drop that no group member was AllowedForPlayer at the time — an unusable
        // or already-known recipe (ITEM_FLAG_HIDE_UNUSABLE_RECIPE / a BoP recipe),
        // a faction-flagged item, a quest starter everybody has already done. Those
        // clear needs_quest/conditions/MULTI_DROP, so the predicate calls them
        // counted and taking one spends a count belonging to a drop still ON the
        // body — the same over-spend the predicate was tightened to stop, by the
        // other door. Re-deriving that visibility here would be a second guess (the
        // answer can have changed since the fill), so restore the floor by counting
        // what is demonstrably still lying here instead.
        //
        // Unconditional, not just for the raid carve-out: the over-spend belongs to
        // the decrement, not to the raid, and an ordinary party leaves drops behind
        // too (every party bag full). Same destruction, no roll involved — the tally
        // hits 0 with that drop still on the body, the player opens the corpse and
        // closes it without taking it, and DoLootRelease clears straight through it.
        // A no-op on a body this pass emptied completely (nothing left to count), so
        // it cannot cost a fully-looted corpse its skinning.
        WowPsParty::RestoreUnlootedFloor(*loot);

        // Mark loot fully consumed if all items got taken. "All" only ever means
        // the ordinary items this pass moves into the party bags: quest_items[]
        // is not even walked above, and a quest drop still on the body must not
        // reach AllLootRemovedFromCorpse — that flags the corpse SKINNABLE, and
        // the party's skinner then clears the loot and despawns the body before
        // the player can walk over and take it.
        bool allTaken = (loot->gold == 0);
        for (auto const& li : loot->items)
        {
            if (!li.is_looted) { allTaken = false; break; }
        }
        if (allTaken && !WowPsParty_SkinWouldDestroyPartyLoot(human, killed, nullptr))
            killed->AllLootRemovedFromCorpse();
    }

    // KEY sharing for EVERY other loot path — a member looting through the normal
    // loot window: the human manually looting a corpse/chest, a henchman's engine
    // loot, a gameobject. The creature auto-loot above stores items directly (no
    // OnPlayerLootItem), so the two paths are disjoint — no double-grant. Mirrors
    // any key item to the whole party so nobody is ever locked out of a gate.
    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid /*lootguid*/) override
    {
        if (!WowPsParty::IsEnabled() || !player || !item) return;
        if (!WowPsParty::ProgressionShared(player)) return;   // solo: own loot
        WowPsParty::MirrorKeyItemToParty(player, item->GetEntry(), count);
    }

    // Shared gold pool: every delta applied to one party member is mirrored
    // to all peers, so all 5 always show the same total. Vendor purchases
    // and loot both go through Player::ModifyMoney → fires this hook.
    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        // Hero turning in a mirrored quest: the quest gold already came from the
        // leader's mirrored turn-in, so zero this hero's copy (don't double it).
        if (WowPsParty::g_heroQuestTurnin)
        {
            amount = 0;
            return;
        }
        if (WowPsParty::g_propagatingMoney)
            return;
        if (!WowPsParty::IsEnabled() || !player || !player->GetSession() || amount == 0)
            return;
        if (!WowPsParty::ProgressionShared(player)) return;  // solo: own gold

        std::vector<Player*> const peers =
            LoadedPartyPeers(player->GetSession()->GetAccountId(), player);
        if (peers.empty())
            return;

        WowPsParty::g_propagatingMoney = true;
        for (Player* p : peers)
        {
            // ModifyMoney returns false if the resulting balance would underflow
            // or exceed MAX_MONEY_AMOUNT. We accept the asymmetry — peers that
            // are already at the cap simply stay capped.
            (void)p->ModifyMoney(amount, /*sendError=*/false);
        }
        WowPsParty::g_propagatingMoney = false;
    }

    // Mirror QUEST turn-in XP to every party member. Kill / explore / BG XP is
    // deliberately NOT mirrored: the party-of-5 is a real group, so the engine
    // already splits those across all members the normal way (each gets its
    // ~1/5 share once). Mirroring them stacked all five shares onto everyone —
    // the ~5x over-leveling bug. Quest XP is the exception, because the heroes
    // never visit the quest giver, so the leader's turn-in is the only GiveXP
    // that fires and the mirror is the only way they get quest XP at all.
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
    {
        // Henchmen are fixed at their hire level — they never level up, so they
        // never gain XP (from group kills or anything else). Zero it out before
        // anything else, including the propagation guard.
        if (player && WowPsParty::IsHenchman(player->GetGUID()))
        {
            amount = 0;
            return;
        }
        // A hero turning in a mirrored quest already received this quest's XP from
        // the leader's turn-in (mirrored below). Zero it so it isn't counted twice.
        if (WowPsParty::g_heroQuestTurnin)
        {
            amount = 0;
            return;
        }
        if (WowPsParty::g_propagatingXP)
            return;
        if (!WowPsParty::IsEnabled() || !player || !player->GetSession() || amount == 0)
            return;

        // Apply this account's per-source XP rate (set via .party xp, default
        // 100%). Scaling the ORIGINAL gain means the originator AND every peer
        // get the boosted amount: the mirror loop below passes this already-
        // scaled value to GiveXP, which doesn't re-fire OnPlayerGiveXP, so the
        // rate lands exactly once. Runs in solo mode too (the rate is account-
        // wide), so it sits before the shared-progression gate. Only kill and
        // quest XP are configurable — explore / BG stay at 100%.
        {
            WowPsParty::PartySettings const s =
                WowPsParty::GetAccountSettings(player->GetSession()->GetAccountId());
            uint32 rate = 100;
            if (xpSource == XPSOURCE_KILL)
                rate = s.killXpRate;
            else if (xpSource == XPSOURCE_QUEST || xpSource == XPSOURCE_QUEST_DF)
                rate = s.questXpRate;
            if (rate != 100)
                amount = static_cast<uint32>(uint64(amount) * rate / 100);
        }

        // Only QUEST turn-in XP is mirrored. Kill / explore / BG XP is already
        // handed to each group member by the engine's normal party-XP split, so
        // mirroring it stacked 5 shares onto everyone (~5x). This member still
        // keeps its own scaled gain above — we just don't copy it to the peers.
        if (xpSource != XPSOURCE_QUEST && xpSource != XPSOURCE_QUEST_DF)
            return;

        if (!WowPsParty::ProgressionShared(player)) return;  // solo: own quest XP, no mirror

        std::vector<Player*> const peers =
            LoadedPartyPeers(player->GetSession()->GetAccountId(), player);
        if (peers.empty())
            return;

        // Quest XP isn't group-split by the engine (turn-in is a personal
        // reward), and the heroes can't turn the quest in themselves, so mirror
        // the leader's full (scaled) quest XP to each of them — like five solo
        // players each handing the quest in.
        WowPsParty::g_propagatingXP = true;
        for (Player* p : peers)
            p->GiveXP(amount, victim, /*group_rate=*/1.0f);
        WowPsParty::g_propagatingXP = false;
    }

    // Mirror reputation gains so all 5 stay at the same standing with each faction.
    void OnPlayerGiveReputation(Player* player, int32 factionID, float& amount, ReputationSource repSource) override
    {
        // Hero turning in a mirrored quest: the rep already came from the leader's
        // mirrored turn-in, so zero this hero's copy (don't double it).
        if (WowPsParty::g_heroQuestTurnin)
        {
            amount = 0.0f;
            return;
        }
        if (WowPsParty::g_propagatingRep)
            return;
        if (!WowPsParty::IsEnabled() || !player || !player->GetSession() || amount == 0.0f || factionID <= 0)
            return;
        if (!WowPsParty::ProgressionShared(player)) return;  // solo: own reputation

        FactionEntry const* faction = sFactionStore.LookupEntry(uint32(factionID));
        if (!faction)
            return;

        std::vector<Player*> const peers =
            LoadedPartyPeers(player->GetSession()->GetAccountId(), player);
        if (peers.empty())
            return;

        WowPsParty::g_propagatingRep = true;
        for (Player* p : peers)
        {
            // ModifyReputation honours faction prerequisites, race/faction
            // restrictions, and rep caps. If a peer can't gain rep with this
            // faction (wrong race/faction), it's a no-op for that peer.
            (void)repSource;
            p->GetReputationMgr().ModifyReputation(faction, amount);
        }
        WowPsParty::g_propagatingRep = false;
    }

};

// When a henchman leaves the party by ANY means (the addon Dismiss button,
// or the stock WoW group UI "Remove from group"), make it stop following and
// despawn — Kevin's "pretend he just hearthstoned" behaviour. Enrolled alts
// and the leader are ignored (IsHenchman gate).
class PartyHenchmanGroupScript : public GroupScript
{
public:
    PartyHenchmanGroupScript() : GroupScript("PartyHenchmanGroupScript", {
        GROUPHOOK_ON_REMOVE_MEMBER
    }) { }

    void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod method,
                        ObjectGuid /*kicker*/, char const* /*reason*/) override
    {
        if (!WowPsParty::IsEnabled()) return;
        if (WowPsParty::IsHenchman(guid))
        {
            // A (re-)hire is just MOVING this henchman out of a stale group into
            // the player's party — don't dismiss it (the "hello / see you later"
            // instant-leave after an LFG dungeon).
            if (WowPsParty::IsHenchmanRegrouping(guid)) return;
            WowPsParty::DismissHenchmanByGuid(guid);
            return;
        }
        if (WowPsParty::IsHiredAlt(guid))
        {
            // Same "pretend it hearthstoned" behaviour for a hired alt leaving the
            // party by any means — but it's logged out SAVED AS-IS (no bag clear).
            if (WowPsParty::IsHenchmanRegrouping(guid)) return;   // mid-(re)hire regroup
            WowPsParty::DismissHiredAltByGuid(guid);
            return;
        }
        // The henchman OWNER (the player who hired them) left / was removed from
        // the group — e.g. leaving an LFG dungeon. Dismiss their henchmen rather
        // than let them teleport out and keep orphan-following the ex-leader.
        // (If the group fully DISBANDS, each henchman's own removal hits the
        // branch above; this covers the case where the group survives without
        // the owner — e.g. matched LFG players stay behind.)
        //
        // Dismiss the henchmen when the owner leaves in either of two cases:
        //  1. a FOREIGN group (LFG dungeon / BG / raid) where matched players stay
        //     behind — the henchmen would otherwise orphan-follow the ex-leader; or
        //  2. the owner DELIBERATELY left (GROUP_REMOVEMETHOD_LEAVE — the "Leave Party"
        //     click, HandleGroupDisbandOpcode): leaving a party of henchmen must
        //     actually dismiss them "like normal".
        // It must NOT fire for an INTERNAL removal of the owner's OWN open-world party
        // (method DEFAULT — e.g. the follow ticker's transient regroup, a WG/LFG
        // reshuffle): those keep the henchmen so the self-heal re-forms the roster
        // around the human (Kevin: "never leave your roster" — the regression this gate
        // exists for). The deliberate-leave click is LEAVE, the transient churn is
        // DEFAULT, so RemoveMethod cleanly separates them.
        if ((WowPsParty::CountHenchmenFor(guid) > 0 || WowPsParty::CountHiredAltsFor(guid) > 0)
            && group
            && (group->isLFGGroup() || group->isBGGroup()
                || method == GROUP_REMOVEMETHOD_LEAVE))
        {
            std::vector<ObjectGuid> members;
            WowPsParty::GetPartyGuidsFor(guid, members);
            for (ObjectGuid const& m : members)
            {
                if (WowPsParty::IsHenchman(m))
                    WowPsParty::DismissHenchmanByGuid(m);
                else if (WowPsParty::IsHiredAlt(m))
                    WowPsParty::DismissHiredAltByGuid(m);
            }
        }
    }
};

// Records every SPELL that damages a player so the took_damage_from:<name> rotation
// condition can react to a specific source (e.g. Ingvar's spinning axe) — a damage
// event with no debuff aura to test for. Read-only on the damage; only victim+spell
// are captured.
class PartyDamageTrackScript : public UnitScript
{
public:
    PartyDamageTrackScript() : UnitScript("PartyDamageTrackScript", true,
        { UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK }) { }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& /*damage*/, SpellInfo const* spellInfo) override
    {
        if (!target || !spellInfo) return;
        if (Player* p = target->ToPlayer())
            WowPsParty::RecordSpellDamageTaken(p->GetGUID().GetCounter(), spellInfo->Id,
                attacker ? attacker->GetPositionX() : 0.0f,
                attacker ? attacker->GetPositionY() : 0.0f,
                attacker ? attacker->GetPositionZ() : 0.0f,
                attacker != nullptr);
    }

    // PERIODIC (DOT / ground-effect aura) ticks go through a SEPARATE damage path the
    // direct hook above never sees — so without this took_damage_from missed every
    // "stand in the fire" mechanic (Coldflame ticks via SPELL_COLDFLAME_PASSIVE, Defile,
    // Consecration, …), which is the most common thing a reposition rule reacts to
    // (Kevin: healer stood in Coldflame, took ticks, never moved). Record these too.
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& /*damage*/, SpellInfo const* spellInfo) override
    {
        if (!target || !spellInfo) return;
        if (Player* p = target->ToPlayer())
            WowPsParty::RecordSpellDamageTaken(p->GetGUID().GetCounter(), spellInfo->Id,
                attacker ? attacker->GetPositionX() : 0.0f,
                attacker ? attacker->GetPositionY() : 0.0f,
                attacker ? attacker->GetPositionZ() : 0.0f,
                attacker != nullptr);
    }
};

// Every arena spawns a "Ready Marker" gameobject (entry 301337, go_arena_ready_marker);
// once ALL participants click it the pre-match countdown collapses to 15s. Bots never
// click, so any match containing henchmen or fill bots always sat out the full wait —
// leaving the human clicking "ready" but still stuck at 1/N because the bots never
// register. Auto-mark every bot for them, so only the human clicks gate the skip.
//
// Do this on the per-tick BG update, NOT at AddPlayer: when a player is added the
// prep timer is still 0 (Battleground::_ProcessJoin only sets it on the first tick a
// player is on the map), so ReadyMarkerClicked's `GetStartDelayTime() <= 15s` guard
// rejected every entry-time mark — the original AddPlayer version raced the timer and
// lost, which is why the fix never took. The update hook re-attempts each tick and
// only fires once the prep window is genuinely open, so the marks always land.
class PartyArenaReadyBgScript : public AllBattlegroundScript
{
public:
    PartyArenaReadyBgScript() : AllBattlegroundScript("PartyArenaReadyBgScript", {
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_UPDATE
    }) { }

    void OnBattlegroundUpdate(Battleground* bg, uint32 /*diff*/) override
    {
        if (!WowPsParty::IsEnabled() || !bg || !bg->isArena())
            return;
        // Only during the pre-match countdown, and only once the prep timer is set
        // (delay > 15s) so ReadyMarkerClicked accepts the mark and the countdown-skip
        // maths (m_StartTime += delay - 15s) stays correct.
        if (bg->GetStatus() != STATUS_WAIT_JOIN || bg->GetStartDelayTime() <= BG_START_DELAY_15S)
            return;
        for (auto const& [guid, player] : bg->GetPlayers())
        {
            if (!player || player->IsSpectator())
                continue;
            if (!player->GetSession() || !player->GetSession()->IsBot())
                continue;
            if (bg->readyMarkerClickedSet.find(guid) != bg->readyMarkerClickedSet.end())
                continue;
            bg->ReadyMarkerClicked(player);
            LOG_INFO("module", "[WowPsParty] arena auto-ready: bot {} marked ({}/{} ready)",
                player->GetName(), bg->readyMarkerClickedSet.size(),
                ArenaTeam::GetReqPlayersForType(bg->GetArenaType()));
        }
    }
};

// Ends mirrored dances. Runs on its own short beat rather than the 1 Hz follow
// ticker so the party stops dancing as the leader walks off, not a second
// later. The pass only ever walks companions actually mid-dance, so it costs
// nothing the rest of the time.
class PartyEmoteWorldScript : public WorldScript
{
public:
    PartyEmoteWorldScript() : WorldScript("PartyEmoteWorldScript", {
        WORLDHOOK_ON_UPDATE
    }) { }

    void OnUpdate(uint32 diff) override
    {
        _accum += diff;
        if (_accum < CLEAR_INTERVAL_MS)
            return;
        _accum -= CLEAR_INTERVAL_MS;
        WowPsParty::ClearFinishedDances();
    }

private:
    static constexpr uint32 CLEAR_INTERVAL_MS = 500;
    uint32 _accum = 0;
};

void AddPartyHooksScripts()
{
    new PartyHooksPlayerScript();
    new PartyHenchmanGroupScript();
    new PartyDamageTrackScript();
    new PartyArenaReadyBgScript();
    new PartyEmoteWorldScript();
}

// Trampoline called from the [WowPsParty PATCH] in PlayerQuest.cpp::AddQuest.
// Mirrors the accepted quest to every other party member that can take it.
void WowPsParty_OnQuestAccepted_Trampoline(Player* who, Quest const* quest)
{
    if (WowPsParty::g_propagatingQuest) return;
    if (!WowPsParty::IsEnabled() || !who || !who->GetSession() || !quest) return;
    if (!WowPsParty::ProgressionShared(who)) return;   // solo: don't mirror quests

    std::vector<Player*> const peers =
        LoadedPartyPeers(who->GetSession()->GetAccountId(), who);
    if (peers.empty()) return;

    WowPsParty::g_propagatingQuest = true;
    for (Player* p : peers)
    {
        if (!p->CanTakeQuest(quest, false)) continue;
        if (p->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_NONE) continue;
        p->AddQuestAndCheckCompletion(quest, nullptr);
    }
    WowPsParty::g_propagatingQuest = false;
}

// Trampoline called from the [WowPsParty PATCH] in PlayerQuest.cpp::RewardQuest.
// Mirrors the quest TURN-IN to every party hero that has the quest, force-
// completing it first so they stay in lockstep on quest CHAINS (RewardQuest
// marks the quest rewarded, which satisfies the next quest's prerequisite). When
// the quest offers a CHOICE of rewards the heroes spread those choices so the
// account collects a variety instead of five copies of the same item. The heroes
// don't need to be at the quest giver — this is a full turn-in, not just loot.
void WowPsParty_OnQuestRewarded_Trampoline(Player* who, Quest const* quest, uint32 rewardChoice)
{
    if (WowPsParty::g_propagatingQuest) return;
    if (!WowPsParty::IsEnabled() || !who || !who->GetSession() || !quest) return;
    if (!WowPsParty::ProgressionShared(who)) return;   // solo: nothing to mirror

    std::vector<Player*> const peers =
        LoadedPartyPeers(who->GetSession()->GetAccountId(), who);
    if (peers.empty()) return;

    uint32 const questId     = quest->GetQuestId();
    uint32 const choiceCount = quest->GetRewChoiceItemsCount();

    // g_propagatingQuest: stop the accept/reward trampolines re-entering on the
    // heroes' own RewardQuest. g_heroQuestTurnin: zero the heroes' quest XP/money/
    // rep (already mirrored from the leader) while keeping their reward items.
    WowPsParty::g_propagatingQuest = true;
    WowPsParty::g_heroQuestTurnin  = true;
    uint32 rewarded = 0;   // counts heroes actually turned in, to keep choices distinct
    for (Player* p : peers)
    {
        QuestStatus const st = p->GetQuestStatus(questId);
        // Hero never had this quest, or already turned it in — nothing to do.
        if (st == QUEST_STATUS_NONE || st == QUEST_STATUS_REWARDED) continue;

        // The heroes are bots shadowing the leader's questing, and the leader
        // (which just handed this in) is the source of truth. Force the hero's
        // objectives complete if they aren't already, so it turns in alongside
        // the leader and stays in lockstep on the quest CHAIN — including
        // objective types we can't mirror (explore / escort / use-object /
        // talk-to-NPC). Without this, such a hero never reaches COMPLETE, is
        // skipped, is never marked rewarded, and then fails the prerequisite for
        // the next quest in the chain — a desync that never self-heals.
        if (st != QUEST_STATUS_COMPLETE)
            p->CompleteQuest(questId);

        // Spread the choice rewards: each hero takes a DIFFERENT index, starting
        // just past the leader's pick and wrapping when there are more heroes than
        // choices. A quest with 0/1 choice items always uses index 0.
        uint32 const choice = (choiceCount > 1)
            ? (rewardChoice + 1 + rewarded) % choiceCount
            : 0u;

        // Verifies the (now-complete) quest is rewardable and the chosen reward
        // fits the hero's bags. For a rare auto-reward tracking quest CompleteQuest
        // already rewarded it, so this returns false and we don't double-reward.
        if (p->CanRewardQuest(quest, choice, false))
        {
            p->RewardQuest(quest, choice, nullptr, false);
            ++rewarded;
        }
        else if (p->GetQuestStatus(questId) != QUEST_STATUS_REWARDED)
        {
            // Bags too full (or otherwise un-rewardable) — finish the turn-in
            // WITHOUT the reward item rather than leaving the quest stuck. See
            // ForceCompleteTurnIn: this is what stops the orphan-"?" + clogged-bag
            // desync that snowballs (a full bag fails every later turn-in too).
            ForceCompleteTurnIn(p, quest);
        }
    }
    WowPsParty::g_heroQuestTurnin  = false;
    WowPsParty::g_propagatingQuest = false;
}

// Trampoline called from the [WowPsParty PATCH] in Spell.cpp::CheckItems.
// Total count of crafting reagent `itemId` available to the crafter across the
// WHOLE shared party — the crafter's own bags plus every loaded party member's.
// Lets you craft (tailoring, etc.) using reagents sitting in a partner's bags
// shown in the merged Party Inventory. For a solo / shared-inventory-off caster
// this is just the crafter's own count, so vanilla behaviour is preserved.
uint32 WowPsParty_PartyReagentCount(Player* crafter, uint32 itemId)
{
    if (!crafter) return 0;
    uint32 total = UsableReagentCount(crafter, itemId);
    if (!WowPsParty::IsEnabled() || !WowPsParty::InventoryShared(crafter))
        return total;   // solo: own bags only == vanilla HasItemCount predicate
    for (Player* p : LoadedPartyPeers(crafter->GetSession()->GetAccountId(), crafter))
    {
        // Only peers sharing the crafter's Map* are eligible. A Map is updated by a
        // single MapUpdater worker thread, so consuming a same-map peer's reagent
        // (in WowPsParty_TakeReagent) is serialized with that peer's own save. A
        // peer on another map updates on another thread — mutating its inventory
        // from the crafter's thread races its save and can free an Item still queued
        // in it, the use-after-free seen as the _SaveInventory ACCESS_VIOLATION.
        // Counting must use the SAME predicate as consuming so the two stay
        // symmetric (never allow a craft we can't fully pay for). Compare Map* not
        // map id: two instances of one map are distinct update contexts.
        if (p->GetMap() != crafter->GetMap())
        {
            if (uint32 const elsewhere = UsableReagentCount(p, itemId))
                LOG_DEBUG("module", "[WowPsParty Reagent] {}x reagent {} held by party-mate {} is on another map ({} vs {}); not counted for {}'s craft.",
                          elsewhere, itemId, p->GetName(), p->GetMapId(), crafter->GetMapId(), crafter->GetName());
            continue;
        }
        total += UsableReagentCount(p, itemId);
    }
    return total;
}

// Trampoline called from the [WowPsParty PATCH] in Spell.cpp::CheckItems.
// A profession recipe can require a category TOOL in the crafter's bags — a
// blacksmith hammer, mining pick, arclight spanner, runed rod, etc. (spell
// TotemCategory, surfaced client-side as "Requires Blacksmith Hammer"). In the
// shared-inventory party that tool may sit in a party-mate's bags instead; the
// merged Party Inventory already shows it as "yours", so demanding it on the
// crafter specifically is the friction this removes. TRUE if the crafter OR a
// loaded SAME-MAP party peer holds a matching tool. The same-map boundary is
// required even for this read: a peer on another MapUpdater thread can mutate or
// save its bags while this check walks them. Solo / shared-inventory-off falls
// back to the crafter's own bags == the vanilla Player::HasItemTotemCategory
// predicate it augments.
bool WowPsParty_PartyHasTotemCategory(Player* crafter, uint32 totemCategory)
{
    if (!crafter) return false;
    if (crafter->HasItemTotemCategory(totemCategory))
        return true;
    if (!WowPsParty::IsEnabled() || !WowPsParty::InventoryShared(crafter))
    {
        LOG_INFO("module", "[WowPsParty Tool] {} craft tool-category={} result=missing reason=shared-inventory-disabled",
                 crafter->GetName(), totemCategory);
        return false;   // solo: own bags only == vanilla predicate
    }

    std::ostringstream peers;
    bool first = true;
    for (Player* p : LoadedPartyPeers(crafter->GetSession()->GetAccountId(), crafter))
    {
        if (!first) peers << ',';
        first = false;
        peers << p->GetName() << ':';
        if (p->GetMap() != crafter->GetMap())
        {
            peers << "other-map(" << p->GetMapId() << ')';
            continue;
        }
        if (UsableToolCategory(p, totemCategory))
        {
            LOG_INFO("module", "[WowPsParty Tool] {} craft tool-category={} holder={} holderMap={} crafterMap={} source=party-inventory",
                     crafter->GetName(), totemCategory, p->GetName(), p->GetMapId(), crafter->GetMapId());
            return true;
        }
        peers << "missing";
    }
    LOG_INFO("module", "[WowPsParty Tool] {} craft tool-category={} result=missing peers=[{}] reason=no-same-map-holder",
             crafter->GetName(), totemCategory, peers.str());
    return false;
}

// Trampoline called from the [WowPsParty PATCH] in Spell.cpp::CheckItems.
// A profession recipe can also require a SPECIFIC tool ITEM by id in the
// crafter's bags — Simple Grinder (jewelcrafting cuts), a Runed rod (enchanting),
// Philosopher's/Alchemist's Stone (alchemy transmute), Gnomish Army Knife, etc.
// (spell Totem[], surfaced client-side as "Requires Simple Grinder"). Unlike a
// TotemCategory tool this demands one exact item id, so it is a separate check
// from WowPsParty_PartyHasTotemCategory above. In the shared-inventory party the
// tool may sit in a party-mate's bags instead of the crafter's — the merged Party
// Inventory already shows it as "yours", so demanding it on the crafter is the
// friction this removes. TRUE if the crafter OR a loaded SAME-MAP party peer
// holds the item. An off-map peer's bag storage can mutate on another MapUpdater
// thread while this check reads it, so it is deliberately not inspected. Solo /
// shared-inventory-off falls back to the crafter's own bags == the vanilla
// Player::HasItemCount predicate.
bool WowPsParty_PartyHasTotemItem(Player* crafter, uint32 itemId)
{
    if (!crafter) return false;
    if (crafter->HasItemCount(itemId))
        return true;
    if (!WowPsParty::IsEnabled() || !WowPsParty::InventoryShared(crafter))
    {
        LOG_INFO("module", "[WowPsParty Tool] {} craft tool-item={} result=missing reason=shared-inventory-disabled",
                 crafter->GetName(), itemId);
        return false;   // solo: own bags only == vanilla predicate
    }

    std::ostringstream peers;
    bool first = true;
    for (Player* p : LoadedPartyPeers(crafter->GetSession()->GetAccountId(), crafter))
    {
        if (!first) peers << ',';
        first = false;
        peers << p->GetName() << ':';
        if (p->GetMap() != crafter->GetMap())
        {
            peers << "other-map(" << p->GetMapId() << ')';
            continue;
        }
        if (UsableReagentCount(p, itemId))
        {
            LOG_INFO("module", "[WowPsParty Tool] {} craft tool-item={} holder={} holderMap={} crafterMap={} source=party-inventory",
                     crafter->GetName(), itemId, p->GetName(), p->GetMapId(), crafter->GetMapId());
            return true;
        }
        peers << "missing";
    }
    LOG_INFO("module", "[WowPsParty Tool] {} craft tool-item={} result=missing peers=[{}] reason=no-same-map-holder",
             crafter->GetName(), itemId, peers.str());
    return false;
}

// Trampoline called from the [WowPsParty PATCH] in Spell.cpp::TakeReagents.
// Consume `count` of reagent `itemId`: the crafter's own bags first, then loaded
// party members for the shortfall (the counterpart to the count above). For a
// solo / shared-inventory-off caster this destroys only from the crafter — i.e.
// exactly what the vanilla DestroyItemCount it replaces did.
void WowPsParty_TakeReagent(Player* crafter, uint32 itemId, uint32 count)
{
    if (!crafter || count == 0) return;

    uint32 const own = UsableReagentCount(crafter, itemId);
    uint32 const fromCrafter = std::min(own, count);
    if (fromCrafter)
        crafter->DestroyItemCount(itemId, fromCrafter, true);

    uint32 remaining = count - fromCrafter;
    if (remaining == 0) return;
    if (!WowPsParty::IsEnabled() || !WowPsParty::InventoryShared(crafter)) return;

    for (Player* p : LoadedPartyPeers(crafter->GetSession()->GetAccountId(), crafter))
    {
        if (remaining == 0) break;
        // Same-Map* peers only — mutating an off-map peer's inventory from the
        // crafter's thread races that peer's save thread (use-after-free crash).
        // Matches the predicate in WowPsParty_PartyReagentCount; see it for detail.
        if (p->GetMap() != crafter->GetMap())
            continue;
        uint32 const take = std::min(UsableReagentCount(p, itemId), remaining);
        if (take)
        {
            p->DestroyItemCount(itemId, take, true);
            remaining -= take;
            LOG_INFO("module", "[WowPsParty Reagent] {} used {}x reagent {} from party-mate {}'s bags (shared inventory).",
                     crafter->GetName(), take, itemId, p->GetName());
        }
    }

    // CheckItems already approved this craft against the same same-map predicate, so
    // a shortfall here means a peer moved maps or dropped the reagent mid-cast. Log
    // it (rare) so a craft that silently consumed less than expected is diagnosable.
    if (remaining)
        LOG_WARN("module", "[WowPsParty Reagent] {}'s craft is still {}x short of reagent {} after the shared inventory — a holder likely left the crafter's map mid-cast.",
                 crafter->GetName(), remaining, itemId);
}

// Trampoline called from the [WowPsParty PATCH] in Group::GroupLoot and
// Group::NeedBeforeGreed, once per member per item they are about to build a roll
// over. True keeps that member OUT of the roll.
//
// A companion is not somebody you roll against. Both kinds of bot had to go: a
// managed party bot hard-returns past mod-playerbots' UpdateAI and never answers
// a roll at all — with 39 of them in the roll the player's Need would sit out the
// whole 60-second timer before resolving — and a pool bot that DOES answer
// (LootRollAction) rolls Need on any upgrade, which is a bot taking an epic off a
// person. Opting them out with Player::SetPassOnGroupLoot was the other option and
// is worse: the core counts that vote as a roller anyway, and broadcasts a "passed
// on" line for each one to the whole raid.
//
// A group with no person in it is left alone, so bot-only groups still roll among
// themselves exactly as before. Deliberately realm-wide rather than party-of-5
// only: a pool bot filling ANY player's group shouldn't roll against them either.
bool WowPsParty_BotStandsDownFromRoll(Player* member, Group* group)
{
    if (!WowPsParty::IsEnabled() || !member || !group) return false;
    if (!WowPsParty::IsBotBody(member)) return false;
    return WowPsParty::GroupHasRealPerson(group);
}

// Trampoline called from the [WowPsParty PATCH] in Player::StoreLootItem (the
// loot-window path: gathering nodes, chests, fishing, manual corpse loot). The
// shared-inventory party is ONE pooled inventory across the human's HEROES, so
// normal loot is spread evenly and a member with full bags overflows into a
// hero that still has room — an individual member is never "full" until the
// WHOLE party is. Picks the hero (the looter included) with the most free bag
// space that can actually hold the item; returns the looter unchanged for
// solo / shared-inventory-off / henchman looters, or when nobody can take it
// (whole party full -> vanilla EQUIP_ERR). Henchmen are deliberately excluded
// as recipients (GetParty/LoadedPartyPeers list heroes only) AND as looters:
// a hired henchman's own loot is not redistributed.
Player* WowPsParty_PickLootReceiver(Player* looter, uint32 itemid, uint32 count)
{
    if (!looter || !looter->GetSession()) return looter;
    if (!WowPsParty::IsEnabled() || !WowPsParty::InventoryShared(looter))
        return looter;                                   // solo: own bag, vanilla
    if (WowPsParty::IsHenchman(looter->GetGUID()))
        return looter;                                   // henchman loot stays its own

    // A blacklisted item the player hand-looted anyway stays with whoever took it.
    // The list says "no companion of mine picks this up", and spreading it into a
    // hero's bags is exactly that — while an explicit click is not something to
    // second-guess, so it is kept rather than refused.
    if (WowPsParty::LootBlacklisted(looter->GetSession()->GetAccountId(), itemid))
        return looter;

    // In a raid the group's loot rules decide where a drop goes, so anything at
    // or above its threshold stays with whoever the raid gave it to. Otherwise
    // the epic the player just won or picked off the body would be shuffled into
    // whichever hero had the emptier bags — the same "a bot took my epic" the
    // kill-hook carve-out exists to stop. This trampoline also serves
    // Player::AutoStoreLoot (prospect / mill / disenchant / pickpocket) and the
    // two item-creation sites in Spell.cpp, so while in a raid those outputs stay
    // with their maker too — deliberate: the shared panel shows them either way.
    if (Group* raid = WowPsParty::RaidLootRules(looter->GetGroup()))
        if (ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(itemid))
            if (tmpl->Quality >= uint32(raid->GetLootThreshold()))
                return looter;

    if (count == 0) count = 1;

    // Candidates: the looter + every loaded HERO peer sharing the looter's Map*.
    // Same-Map* only (not just same map id): mutating an off-map peer's
    // inventory from this thread races that peer's save thread (use-after-free;
    // see WowPsParty_TakeReagent). Two instances of one map are distinct
    // update contexts, so compare Map* not map id.
    std::vector<Player*> heroes;
    heroes.push_back(looter);
    for (Player* p : LoadedPartyPeers(looter->GetSession()->GetAccountId(), looter))
        if (p->GetMap() == looter->GetMap())
            heroes.push_back(p);

    // Most free space first; equal bags round-robin naturally as each pickup
    // drops that hero's free count. A hero whose bags are full sorts last and
    // is skipped by the CanStoreNewItem check below.
    std::stable_sort(heroes.begin(), heroes.end(),
        [](Player const* a, Player const* b)
        { return a->GetFreeInventorySpace() > b->GetFreeInventorySpace(); });

    for (Player* h : heroes)
    {
        ItemPosCountVec dest;
        if (h->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemid, count) == EQUIP_ERR_OK)
            return h;
    }
    return looter;   // whole party is full -> let the vanilla path raise the error
}

// Trampoline called from the [WowPsParty PATCH] in Spell.cpp::CheckItems.
// Shaman totem spells require a totem RELIC item of the matching category in the
// caster's bags (the relics handed out by the early totem quests). Our party
// shamans — the player's hero alts AND hired henchmen — are spun up without ever
// running those quests, so the relic check would block every totem they cast.
// Skip it for them: TRUE means "don't enforce Totem/TotemCategory for this cast".
//
// Scoped to caster-is-a-shaman so the bypass only ever touches totem spells (the
// only ones a shaman casts that carry these fields) and never a profession's
// TotemCategory tool requirement (mining pick, blacksmith hammer, …). Scoped to
// managed party members so random-pool / AH bots and unrelated players are
// unaffected: an enrolled hero (GetSlotForGuid, includes the active char) or any
// follower bot (BotHasActiveFollowDirective, includes henchmen with no party_slot).
bool WowPsParty_ShouldBypassTotemReq(Player* caster)
{
    if (!caster || caster->getClass() != CLASS_SHAMAN) return false;
    if (WowPsParty::BotHasActiveFollowDirective(caster->GetGUID())) return true;
    return sPartyMgr.GetSlotForGuid(caster->GetGUID().GetCounter()).has_value();
}

// Trampoline called from the [WowPsParty PATCH] in PlayerStorage.cpp::ApplyEnchantment.
// Profession-restricted enchants (Leatherworking Fur Lining, Enchanting ring enchants,
// Blacksmithing prismatic sockets, …) can be applied to any party member's gear through
// the Party UI, but the core skill gate then grants NO stats to a wearer who lacks the
// profession — so the enchant shows on the item yet does nothing. Exempt managed party
// members from that skill requirement so cross-profession enchants actually work: an
// enrolled hero (GetSlotForGuid, includes the active/leader char) or any follower
// henchman (BotHasActiveFollowDirective, includes henchmen with no party_slot). Random-
// pool / AH bots and unrelated players keep the normal requirement.
bool WowPsParty_ShouldBypassEnchantSkillReq(Player* player)
{
    if (!player || !WowPsParty::IsEnabled()) return false;
    if (WowPsParty::BotHasActiveFollowDirective(player->GetGUID())) return true;
    return sPartyMgr.GetSlotForGuid(player->GetGUID().GetCounter()).has_value();
}

// Trampoline called from the [WowPsParty PATCH] in Spell.cpp::CheckCast for
// SPELL_EFFECT_SKINNING. The engine refuses to skin a beast whose NORMAL loot
// isn't fully gone (UNIT_FLAG_SKINNABLE unset and/or loot not looted). In a
// party-of-5 the bots loot only what they want/can and leave the rest — and with
// full bags they leave everything — so a skinnable corpse can stay "unlooted"
// forever and nobody in the group can skin it. If THIS skinner shares the
// corpse's kill (its loot-recipient GROUP — the real 5-man, so both Kevin and
// Mill qualify for their party's kills), finish the corpse's normal loot and
// mark it skinnable so the skin proceeds. The bots already pulled what they
// wanted into the shared inventory; the small remainder is dropped (skinning the
// corpse directly is choosing the skin over re-looting it). Returns true once
// the corpse is skinnable.
bool WowPsParty_ForceSkinReady(Player* skinner, Creature* creature)
{
    if (!skinner || !creature) return false;
    if (!WowPsParty::IsEnabled()) return false;

    // Skinnable beasts only — never disturb a non-skinnable corpse's loot.
    CreatureTemplate const* tmpl = creature->GetCreatureTemplate();
    if (!tmpl || tmpl->SkinLootId == 0) return false;

    Player* recipient = creature->GetLootRecipient();
    Group*  rgroup    = creature->GetLootRecipientGroup();

    // Diagnostic: shows exactly why a skin attempt is/ isn't allowed and the
    // corpse state, so a still-failing case can be read straight from the log.
    LOG_INFO("module",
        "[WowPsParty Skin] {} skins entry={} guid={}: recip={} rgroup={} skinnerGroup={} skinnableFlag={} isLooted={} skinLootId={}",
        skinner->GetName(), creature->GetEntry(), creature->GetGUID().GetCounter(),
        recipient ? recipient->GetName() : "<none>",
        rgroup ? "set" : "null",
        skinner->GetGroup() ? "set" : "null",
        creature->HasUnitFlag(UNIT_FLAG_SKINNABLE) ? 1 : 0,
        creature->loot.isLooted() ? 1 : 0,
        tmpl->SkinLootId);

    // Co-op private server: any party member may finish a corpse's leftover loot
    // to skin it. We require only that SOMEONE tapped it (a real kill, so there's
    // a recipient for AllLootRemovedFromCorpse) — the leftover is FORFEITED, not
    // stolen, so there's nothing to grief. The earlier strict "skinner shares the
    // recipient's WoW group" gate failed because the human isn't necessarily in
    // the same Group object as the bot that tapped the kill.
    if (!recipient && !rgroup) return false;

    // The player's loot comes first — quest drops are irreplaceable and the
    // loot.clear() below destroys whatever is left on the body.
    std::string blocked;
    if (WowPsParty_SkinWouldDestroyPartyLoot(skinner, creature, &blocked))
    {
        LOG_INFO("module", "[WowPsParty Skin] defer skin of guid={}: {}",
                 creature->GetGUID().GetCounter(), blocked);
        return false;
    }

    creature->loot.clear();                              // empty the normal loot
    creature->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);  // clear the sparkle/lootable state
    creature->AllLootRemovedFromCorpse();                // sets UNIT_FLAG_SKINNABLE (needs a recipient)
    // Belt-and-braces: if AllLootRemovedFromCorpse's internal guards didn't fire
    // (e.g. recipient cleared), set the flag directly — the SkinLootId check above
    // already confirmed this creature is genuinely skinnable.
    if (!creature->HasUnitFlag(UNIT_FLAG_SKINNABLE))
        creature->SetUnitFlag(UNIT_FLAG_SKINNABLE);
    return true;
}

namespace WowPsParty
{
    // Put every hero and henchman following `human` onto the SAME taxi route the
    // human just boarded, so the party flies the flight path together instead of
    // ground-running beneath it. Bots fly FREE: the human already paid, so we
    // grant each bot a temporary float to clear ActivateTaxiPathTo's fare check,
    // then restore it — with the party money-mirror suppressed so the temp sum
    // never propagates to the rest of the wallet. Gated to the human leader, so a
    // bot's own taxi activation (which re-enters the trampoline) can't recurse.
    void EscortPartyOnTaxi(Player* human, std::vector<uint32> const& nodes)
    {
        if (!IsEnabled() || !human || !human->GetSession()) return;
        if (nodes.size() < 2) return;
        if (sPlayerbotsMgr.GetPlayerbotAI(human)) return;   // only the human escorts

        // Escort is pure travel convenience, so gate it on followers ACTUALLY
        // following (g_directives), not on shared-progression — the follow and
        // mount-sync systems key off the same directives, so a player with
        // companions on but shared XP/gold off still gets a consistent party
        // that flies together. GetPartyGuidsFor returns empty (just the leader,
        // or nothing) when no companions follow, so the size check is the gate.
        std::vector<ObjectGuid> guids;
        GetPartyGuidsFor(human->GetGUID(), guids);
        if (guids.size() < 2) return;                       // no followers to escort

        uint32 const humanMap = human->GetMapId();
        uint32 escorted = 0;
        for (ObjectGuid const& g : guids)
        {
            if (g == human->GetGUID()) continue;
            Player* bot = ObjectAccessor::FindConnectedPlayer(g);
            if (!bot || !bot->IsInWorld() || !bot->IsAlive()) continue;
            if (!sPlayerbotsMgr.GetPlayerbotAI(bot)) continue;  // bots only (safety)
            if (bot->IsInFlight()) continue;                    // already flying
            if (bot->GetMapId() != humanMap) continue;          // can't board cross-map

            if (bot->IsInCombat()) bot->CombatStop();           // taxi rejects in-combat

            // Fly free: temporarily top the bot up so the fare check passes, run
            // the activation (it deducts firstcost), then restore — net zero, with
            // the money-mirror suppressed so neither change escapes to the party.
            // Set PLAYER_FIELD_COINAGE directly rather than via SetMoney(): SetMoney
            // also drives MoneyChanged(), whose "gather N gold" quest scan would see
            // the temporary top-up and could auto-complete such a quest (the restore
            // wouldn't revert it). The raw field set skips that path; the fare
            // deduction inside ActivateTaxiPathTo is a decrease and can't complete a
            // gather-gold quest.
            constexpr uint32 kTaxiEscortFloat = 5000000;        // 500g, ample for any fare
            bool const prevSuppress = g_propagatingMoney;
            g_propagatingMoney = true;
            uint32 const moneyBefore = bot->GetMoney();
            bot->SetUInt32Value(PLAYER_FIELD_COINAGE, moneyBefore + kTaxiEscortFloat);
            bool const ok = bot->ActivateTaxiPathTo(nodes, nullptr, 1);
            bot->SetUInt32Value(PLAYER_FIELD_COINAGE, moneyBefore);
            g_propagatingMoney = prevSuppress;

            if (ok) ++escorted;
            else
                LOG_INFO("module",
                    "[WowPsParty Taxi] {} could not board {}'s flight ({} nodes)",
                    bot->GetName(), human->GetName(), uint32(nodes.size()));
        }
        if (escorted)
            LOG_INFO("module",
                "[WowPsParty Taxi] escorted {} member(s) onto {}'s flight path ({} nodes)",
                escorted, human->GetName(), uint32(nodes.size()));
    }
}

// Global trampoline called from core Player::ActivateTaxiPathTo. Defined in this
// TU (not PartyFollow.cpp) because the fly-free path needs g_propagatingMoney,
// which lives here.
void WowPsParty_OnTaxiFlightStart_Trampoline(Player* human, std::vector<uint32> const& nodes)
{
    WowPsParty::EscortPartyOnTaxi(human, nodes);
}
