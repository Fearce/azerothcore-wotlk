/*
 * WowPs Party-of-5 mod — Track-4 PlayerScript hooks
 *
 * Mirrors XP and reputation gains across all 5 party members and triggers
 * auto-swap when the currently-controlled body dies. Each hook uses a
 * thread_local guard to avoid the obvious infinite recursion (each mirrored
 * XP/rep grant would re-enter the hook and re-mirror forever).
 */

#include "PartyMgr.h"

#include "Chat.h"
#include "Creature.h"
#include "Group.h"
#include "GroupMgr.h"
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
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Trainer.h"
#include "WorldSession.h"

#include <unordered_set>

namespace WowPsParty
{
    // Re-entrance guard. Set by the originating hook, checked by the mirror
    // path before propagating to peers. thread_local is fine — AC dispatches
    // PlayerScript hooks on the world thread.
    static thread_local bool g_propagatingXP    = false;
    static thread_local bool g_propagatingRep   = false;
    static thread_local bool g_autoSwapping     = false;
    static thread_local bool g_propagatingMoney = false;
    static thread_local bool g_propagatingQuest = false;
    static thread_local bool g_propagatingLoot  = false;

    bool IsEnabled();     // from PartyBootstrap.cpp
    bool IsLogVerbose();

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
}

class PartyHooksPlayerScript : public PlayerScript
{
public:
    PartyHooksPlayerScript() : PlayerScript("PartyHooksPlayerScript", {
        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_GIVE_REPUTATION,
        PLAYERHOOK_ON_PLAYER_JUST_DIED,
        PLAYERHOOK_ON_MONEY_CHANGED,
        PLAYERHOOK_ON_CREATURE_KILL,
        PLAYERHOOK_ON_STORE_NEW_ITEM,
        PLAYERHOOK_ON_LEVEL_CHANGED
    }) { }

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
                    ChatHandler(killer->GetSession()).SendSysMessage(lootMsg);
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
        if (WowPsParty::g_propagatingMoney)
            return;
        if (!WowPsParty::IsEnabled() || !player || !player->GetSession() || amount == 0)
            return;

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

    // Mirror kill/quest XP to every party member so the 5 level together.
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
    {
        if (WowPsParty::g_propagatingXP)
            return;
        if (!WowPsParty::IsEnabled() || !player || !player->GetSession() || amount == 0)
            return;

        std::vector<Player*> const peers =
            LoadedPartyPeers(player->GetSession()->GetAccountId(), player);
        if (peers.empty())
            return;

        WowPsParty::g_propagatingXP = true;
        for (Player* p : peers)
        {
            // GiveXP(uint32 xp, Unit* victim, float group_rate, bool isLFGReward).
            // group_rate=1.0 means no party-share dilution (we already share
            // the full per-kill amount; AC's GiveXP applies its own rest/racial
            // modifiers per peer).
            (void)xpSource;
            p->GiveXP(amount, victim, /*group_rate=*/1.0f);
        }
        WowPsParty::g_propagatingXP = false;
    }

    // Mirror reputation gains so all 5 stay at the same standing with each faction.
    void OnPlayerGiveReputation(Player* player, int32 factionID, float& amount, ReputationSource repSource) override
    {
        if (WowPsParty::g_propagatingRep)
            return;
        if (!WowPsParty::IsEnabled() || !player || !player->GetSession() || amount == 0.0f || factionID <= 0)
            return;

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

    // Auto-swap on death: if the body the user is currently driving dies,
    // jump control to the next alive party member. Without this the user is
    // stuck staring at their corpse waiting to release. Also: if the dead
    // player is one of the bots (not the body the user is currently driving),
    // schedule a battlefield-rez 6s after death so wipes are recoverable
    // without trekking to a spirit healer. (Standard GW1 hero behaviour:
    // your party isn't permanently down just because one member died.)
    void OnPlayerJustDied(Player* /*deceased*/) override
    {
        // Auto-rez removed. Death is handled by the priest's rotation —
        // they can run a `Resurrect dead member` rule out of combat. If
        // the whole party wipes, everyone respawns at the spirit healer
        // via the normal release-corpse flow.
    }
};

void AddPartyHooksScripts()
{
    new PartyHooksPlayerScript();
}

// Trampoline called from the [WowPsParty PATCH] in PlayerQuest.cpp::AddQuest.
// Mirrors the accepted quest to every other party member that can take it.
void WowPsParty_OnQuestAccepted_Trampoline(Player* who, Quest const* quest)
{
    if (WowPsParty::g_propagatingQuest) return;
    if (!WowPsParty::IsEnabled() || !who || !who->GetSession() || !quest) return;

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
