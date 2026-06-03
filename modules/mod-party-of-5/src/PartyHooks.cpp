/*
 * WowPs Party-of-5 mod — Track-4 PlayerScript hooks
 *
 * Mirrors XP, reputation, money, loot and quest accepts across all 5 party
 * members, and auto-learns class spells on level-up. Each hook uses a
 * thread_local guard to avoid the obvious infinite recursion (each mirrored
 * grant would re-enter the hook and re-mirror forever).
 */

#include "PartyMgr.h"

#include "Chat.h"
#include "Creature.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "Reputation/ReputationMgr.h"
#include "PartyFollow.h"
#include "ScriptMgr.h"
#include "GroupScript.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Trainer.h"
#include "WorldSession.h"

#include <algorithm>
#include <unordered_set>

namespace WowPsParty
{
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
                if (it->GetEntry() == itemId && !it->IsInTrade())
                    count += it->GetCount();
        for (uint8 i = KEYRING_SLOT_START; i < CURRENCYTOKEN_SLOT_END; ++i)
            if (Item* it = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                if (it->GetEntry() == itemId && !it->IsInTrade())
                    count += it->GetCount();
        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = p->GetBagByPos(i))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (Item* it = bag->GetItemByPos(j))
                        if (it->GetEntry() == itemId && !it->IsInTrade())
                            count += it->GetCount();
        return count;
    }
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
        PLAYERHOOK_ON_QUEST_ABANDON
    }) { }

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
        if (!sPartyMgr.GetSlotForGuid(player->GetGUID().GetCounter()))
            return;  // not one of this account's party characters
        // SOLO MODE: auto-learning the whole class kit on ding and nudging the
        // player to "open the rotation editor" are party-of-5 conveniences (so
        // the bots have their full kit + a rotation). With companions disabled
        // there are no bots — the player trains normally and gets no reminder.
        if (!GetAccountSettings(player->GetSession()->GetAccountId()).spawnCompanions)
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

        uint32 const n = LearnAllClassSpells(player);
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
        if (!player || !player->GetSession() || !item) return;
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
    // and one inventory."
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

        Loot* loot = &killed->loot;
        if (loot->isLooted()) return;

        // Build candidate list of party members in same map who can take items.
        std::vector<Player*> takers;
        takers.push_back(killer);
        for (auto const& m : party)
        {
            ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(m.guid);
            Player* p = ObjectAccessor::FindConnectedPlayer(og);
            if (!p || p == killer || !p->IsInWorld()) continue;
            if (p->GetMapId() != killer->GetMapId()) continue;
            takers.push_back(p);
        }

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

        // Items: iterate, for each non-FFA non-looted item, find a taker
        // with bag space, store it. Announce each pickup in chat so the
        // user sees what the party scooped up.
        for (size_t i = 0; i < loot->items.size(); ++i)
        {
            LootItem& li = loot->items[i];
            if (li.is_looted || li.freeforall) continue;

            ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(li.itemid);
            if (!tmpl) continue;

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
                    li.is_looted = true;
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

        // Mark loot fully consumed if all items got taken.
        bool allTaken = (loot->gold == 0);
        for (auto const& li : loot->items)
        {
            if (!li.is_looted) { allTaken = false; break; }
        }
        if (allTaken)
            killed->AllLootRemovedFromCorpse();
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

    void OnRemoveMember(Group* /*group*/, ObjectGuid guid, RemoveMethod /*method*/,
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
        // The henchman OWNER (the player who hired them) left / was removed from
        // the group — e.g. leaving an LFG dungeon. Dismiss their henchmen rather
        // than let them teleport out and keep orphan-following the ex-leader.
        // (If the group fully DISBANDS, each henchman's own removal hits the
        // branch above; this covers the case where the group survives without
        // the owner — e.g. matched LFG players stay behind.)
        if (WowPsParty::CountHenchmenFor(guid) > 0)
        {
            std::vector<ObjectGuid> members;
            WowPsParty::GetPartyGuidsFor(guid, members);
            for (ObjectGuid const& m : members)
                if (WowPsParty::IsHenchman(m))
                    WowPsParty::DismissHenchmanByGuid(m);
        }
    }
};

void AddPartyHooksScripts()
{
    new PartyHooksPlayerScript();
    new PartyHenchmanGroupScript();
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
        if (!p->CanRewardQuest(quest, choice, false)) continue;
        p->RewardQuest(quest, choice, nullptr, false);
        ++rewarded;
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
        total += UsableReagentCount(p, itemId);
    return total;
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
        uint32 const take = std::min(UsableReagentCount(p, itemId), remaining);
        if (take)
        {
            p->DestroyItemCount(itemId, take, true);
            remaining -= take;
        }
    }
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
