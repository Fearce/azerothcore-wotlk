/*
 * WowPs Party-of-5 mod — LFG "fill my party" offer
 *
 * When a SOLO player queues the Dungeon Finder, offer to instantly fill their
 * party to a 1-tank / 1-healer / 3-dps composition with hired bots, for the
 * summed henchman cost discounted 15%. The player's own slot is set by the LFG
 * role they queued as (tank/healer/dps). Declining leaves them in the normal
 * random-player queue. No client addon required — the yes/no + gold-cost prompt
 * is a momentarily-summoned gossip NPC (visible only to the summoner), whose
 * "Fill" item carries a BoxMoney cost so the 3.3.5a client renders its native
 * gold-cost confirmation.
 *
 * Flow (Option A): the OnPlayerCanJoinLfg hook returns FALSE to abort the
 * immediate queue, summons the recruiter, and force-opens its gossip. The
 * player's choice then drives the real action:
 *   - Fill   : charge the discounted total ONCE, hire the needed role bots
 *              (HireHenchman with skipCharge), then re-drive JoinLfg for the
 *              now-full premade (which instant-pops; LFGMgr auto-answers the
 *              bots' role check).
 *   - Decline: re-drive JoinLfg unchanged → normal random queue.
 *   - Opt out: persist "don't ask again" then decline.
 * A per-player "passthrough" flag lets the re-driven JoinLfg through the hook
 * (guards against infinite recursion).
 */

#include "PartyMgr.h"

#include "Chat.h"
#include "Creature.h"
#include "GossipDef.h"
#include "Group.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"           // urand — randomise + diversify the LFG-fill hire picks
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "TemporarySummon.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using lfg::LfgDungeonSet;

    // Custom recruiter NPC (creature_template shipped in this module's db-world SQL).
    constexpr uint32 NPC_LFG_RECRUITER  = 920050;
    constexpr uint32 OFFER_TIMEOUT_MS   = 60000;   // recruiter despawn / offer expiry
    constexpr uint32 REDRIVE_DELAY_SEC  = 9;       // > the 8s henchman group-up delay

    // Gossip action ids.
    enum : uint32 { ACTION_FILL = 1, ACTION_DECLINE = 2, ACTION_OPTOUT = 3 };

    struct PendingFill
    {
        LfgDungeonSet dungeons;
        uint8  roles       = 0;     // original bitmask the player queued with (decline path)
        uint8  fillRole    = 0;     // narrowed leader+single-role for the filled-premade re-queue
        uint8  needTank    = 0;
        uint8  needHeal    = 0;
        uint8  needDps     = 0;
        uint32 totalCost   = 0;     // copper, already discounted
        ObjectGuid npc;
        bool   committed   = false; // a choice was taken → offer-expiry cleanup skips
        bool   passthrough = false; // next OnPlayerCanJoinLfg for this guid returns true
    };

    std::unordered_map<uint32, PendingFill> g_pending;   // keyed by player guid low
    std::mutex                              g_mutex;

    std::string FormatGold(uint32 copper)
    {
        uint32 const g = copper / 10000;
        uint32 const s = (copper % 10000) / 100;
        uint32 const c = copper % 100;
        std::ostringstream o;
        if (g) o << g << "g ";
        if (g || s) o << s << "s ";
        o << c << "c";
        return o.str();
    }

    void SendOfferMenu(Player* player, Creature* npc, PendingFill const& pf)
    {
        std::string const cost = FormatGold(pf.totalCost);
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG,
            "Fill my party: 1 tank, 1 healer, 3 dps  (" + cost + ", 15% off)",
            GOSSIP_SENDER_MAIN, ACTION_FILL,
            "Recruit a balanced party to fill your group and enter the dungeon "
            "immediately?\n\nCost: " + cost + " (already 15% off).", pf.totalCost, false);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "No thanks — queue me with random players",
            GOSSIP_SENDER_MAIN, ACTION_DECLINE);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "Don't ask again  (re-enable later with: .party lfgfill on)",
            GOSSIP_SENDER_MAIN, ACTION_OPTOUT);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, npc->GetGUID());
    }

    void Say(Player* p, std::string const& msg)
    {
        if (p && p->GetSession())
            ChatHandler(p->GetSession()).PSendSysMessage("{}", msg);
    }

    // Re-issue the original queue unchanged (the random-player path). A
    // passthrough flag makes our own hook let this re-entry through.
    void RequeueNormally(Player* player, PendingFill const& pf)
    {
        uint32 const low = player->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            PendingFill& e = g_pending[low];
            e.committed = true;
            e.passthrough = true;
        }
        LfgDungeonSet dungeons = pf.dungeons;   // JoinLfg takes a non-const ref
        uint8 const roles = pf.roles;
        sLFGMgr->JoinLfg(player, roles, dungeons, "");
    }

    void DoLfgAutoFill(Player* player, PendingFill const& pf)
    {
        uint32 const low = player->GetGUID().GetCounter();

        if (player->GetGroup())   // no longer solo (raced into a group)
        {
            Say(player, "|cffff5555[WowPsParty]|r You're already in a group — fill cancelled.");
            std::lock_guard<std::mutex> lk(g_mutex);
            g_pending.erase(low);
            return;
        }
        if (player->GetMoney() < pf.totalCost)
        {
            Say(player, "|cffff5555[WowPsParty]|r Not enough gold to fill your party — queuing you normally.");
            RequeueNormally(player, pf);
            return;
        }

        // Bucket candidates by role (keep class so we can diversify the picks).
        std::vector<WowPsParty::HenchmanCandidate> const cands = WowPsParty::BuildHenchmanCandidates(player);
        std::vector<WowPsParty::HenchmanCandidate> tanks, heals, dps;
        for (auto const& c : cands)
        {
            if (c.role == "tank")        tanks.push_back(c);
            else if (c.role == "healer") heals.push_back(c);
            else                         dps.push_back(c);
        }
        if (tanks.size() < pf.needTank || heals.size() < pf.needHeal || dps.size() < pf.needDps)
        {
            Say(player, "|cffff5555[WowPsParty]|r Couldn't find enough adventurers to fill — queuing you normally.");
            RequeueNormally(player, pf);
            return;
        }

        // Charge the discounted total ONCE; the per-bot hires skip their own charge.
        player->ModifyMoney(-int32(pf.totalCost));

        // Order a role's pool for the hire: SHUFFLE (so the same fill doesn't recur), then
        // float ONE candidate of each distinct class to the front, then the rest. hireFrom
        // takes the first N that hire — so 3 dps come out as 3 DIFFERENT classes when the
        // pool allows, picked at random (Kevin got 3 warriors + a hunter; now it varies).
        auto orderForVariety = [](std::vector<WowPsParty::HenchmanCandidate> bucket) -> std::vector<uint32>
        {
            for (size_t i = bucket.size(); i > 1; --i)
                std::swap(bucket[i - 1], bucket[urand(0, uint32(i - 1))]);   // Fisher-Yates
            std::vector<uint32> out;
            std::set<uint8>  usedCls;
            std::set<uint32> picked;
            for (auto const& c : bucket)                       // pass 1: one per class
                if (usedCls.insert(c.cls).second) { out.push_back(c.guid); picked.insert(c.guid); }
            for (auto const& c : bucket)                       // pass 2: remaining (spare specs/classes)
                if (!picked.count(c.guid)) out.push_back(c.guid);
            return out;
        };

        auto hireFrom = [&](std::vector<uint32> const& pool, char const* role, uint8 count) -> uint8
        {
            uint8 done = 0;
            for (uint32 g : pool)
            {
                if (done >= count) break;
                std::string msg;
                if (WowPsParty::HireHenchman(player, g, role, msg, /*skipCharge=*/true))
                    ++done;
            }
            return done;
        };
        uint8 const ht = hireFrom(orderForVariety(tanks), "tank",   pf.needTank);
        uint8 const hh = hireFrom(orderForVariety(heals), "healer", pf.needHeal);
        uint8 const hd = hireFrom(orderForVariety(dps),   "dps",    pf.needDps);

        if (ht < pf.needTank || hh < pf.needHeal || hd < pf.needDps)
        {
            // Rare online race between candidate-build and hire — fully unwind.
            player->ModifyMoney(int32(pf.totalCost));
            WowPsParty::DismissAllHenchmen(player);   // solo player had none before
            Say(player, "|cffff5555[WowPsParty]|r Some adventurers became unavailable — refunded, queuing you normally.");
            LOG_WARN("module",
                "[WowPsParty LfgFill] partial fill guid={} (t={}/{} h={}/{} d={}/{}) — refunded {} + requeued normally",
                low, ht, pf.needTank, hh, pf.needHeal, hd, pf.needDps, pf.totalCost);
            RequeueNormally(player, pf);
            return;
        }

        Say(player, "|cff66ccff[WowPsParty]|r Recruiting your party — you'll be queued the moment they arrive.");
        LOG_INFO("module",
            "[WowPsParty LfgFill] FILL guid={} cost={} comp=1T/1H/3D (player role-filled)", low, pf.totalCost);

        // Re-drive the queue once the hired bots have grouped (henchman grouping
        // fires at +8s). The full premade instant-pops and LFGMgr auto-answers
        // the bots' role check via its SetPartyBotLfgRoles trampoline.
        ObjectGuid const g = player->GetGUID();
        LfgDungeonSet const dungeons = pf.dungeons;
        uint8 const roles = pf.fillRole;   // human pinned to their single slot for the premade
        player->m_Events.AddEventAtOffset([g, dungeons, roles]()
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(g);
            if (!p)
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_pending.erase(g.GetCounter());
                return;
            }
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                PendingFill& e = g_pending[g.GetCounter()];
                e.committed = true;
                e.passthrough = true;
            }
            LfgDungeonSet d = dungeons;
            sLFGMgr->JoinLfg(p, roles, d, "");
        }, std::chrono::seconds(REDRIVE_DELAY_SEC));
    }
}

class PartyLfgFillPlayerScript : public PlayerScript
{
public:
    PartyLfgFillPlayerScript() : PlayerScript("PartyLfgFillPlayerScript", {
        PLAYERHOOK_CAN_JOIN_LFG
    }) { }

    bool OnPlayerCanJoinLfg(Player* player, uint8 roles, std::set<uint32>& dungeons, std::string const& /*comment*/) override
    {
        if (!WowPsParty::IsEnabled() || !WowPsParty::IsLfgAutofillEnabled())
            return true;
        if (!player || !player->GetSession())
            return true;

        uint32 const low = player->GetGUID().GetCounter();

        // Re-entrancy guard: a re-driven JoinLfg (our own Fill/Decline path) must
        // pass through; any other re-entry while an offer/fill is in progress is
        // blocked so a manual re-click can't double-queue.
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_pending.find(low);
            if (it != g_pending.end())
            {
                if (it->second.passthrough) { g_pending.erase(it); return true; }
                return false;
            }
        }

        // Solo only (v1). A real group falls through to normal LFG.
        if (player->GetGroup())
            return true;
        // Must have queued as a real role.
        if (!(roles & (lfg::PLAYER_ROLE_TANK | lfg::PLAYER_ROLE_HEALER | lfg::PLAYER_ROLE_DAMAGE)))
            return true;
        if (dungeons.empty())
            return true;
        if (WowPsParty::GetAccountSettings(player->GetSession()->GetAccountId()).lfgAutofillOptOut)
            return true;

        PendingFill pf;
        pf.dungeons = dungeons;
        pf.roles    = roles;
        // Player's slot: prefer tank, then healer, else dps.
        uint8 const playerRole = (roles & lfg::PLAYER_ROLE_TANK)   ? 0
                               : (roles & lfg::PLAYER_ROLE_HEALER) ? 1 : 2;
        pf.needTank = (playerRole == 0) ? 0 : 1;
        pf.needHeal = (playerRole == 1) ? 0 : 1;
        pf.needDps  = (playerRole == 2) ? 2 : 3;     // total fills always 4
        // For the filled-premade re-queue, pin the human to exactly their slot
        // (keep the leader bit) so LFGMgr can't reassign them off the 1T/1H/3D comp.
        uint8 const slotBit = (playerRole == 0) ? lfg::PLAYER_ROLE_TANK
                            : (playerRole == 1) ? lfg::PLAYER_ROLE_HEALER
                                                : lfg::PLAYER_ROLE_DAMAGE;
        pf.fillRole = uint8((roles & lfg::PLAYER_ROLE_LEADER) | slotBit);
        uint8  const fills = pf.needTank + pf.needHeal + pf.needDps;
        uint32 const base  = WowPsParty::HenchmanHireCost(player->GetLevel());
        pf.totalCost = uint32((uint64(base) * fills * 85 + 50) / 100);   // 15% off, round half-up

        // Summon the recruiter on the player, visible to the summoner only.
        TempSummon* npc = player->SummonCreature(NPC_LFG_RECRUITER,
            player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
            player->GetOrientation(), TEMPSUMMON_TIMED_DESPAWN, OFFER_TIMEOUT_MS, nullptr, true);
        if (!npc)
            return true;   // couldn't summon → just let them queue normally
        pf.npc = npc->GetGUID();

        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_pending[low] = pf;
        }
        SendOfferMenu(player, npc, pf);

        // Offer-expiry cleanup: if the player ignores the popup, drop the pending
        // entry so a later queue offers afresh.
        ObjectGuid const g = player->GetGUID();
        player->m_Events.AddEventAtOffset([g]()
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_pending.find(g.GetCounter());
            if (it != g_pending.end() && !it->second.committed && !it->second.passthrough)
                g_pending.erase(it);
        }, std::chrono::milliseconds(OFFER_TIMEOUT_MS + 2000));

        return false;   // abort the immediate queue; the gossip choice drives it
    }
};

class PartyLfgFillRecruiterScript : public CreatureScript
{
public:
    PartyLfgFillRecruiterScript() : CreatureScript("npc_wowps_lfg_recruiter") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!player) return true;
        PendingFill pf; bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_pending.find(player->GetGUID().GetCounter());
            if (it != g_pending.end()) { pf = it->second; found = true; }
        }
        if (!found) { CloseGossipMenuFor(player); return true; }
        SendOfferMenu(player, creature, pf);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        CloseGossipMenuFor(player);
        if (creature)
            creature->DespawnOrUnsummon();
        if (!player || !player->GetSession())
            return true;

        uint32 const low = player->GetGUID().GetCounter();
        PendingFill pf; bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_pending.find(low);
            if (it != g_pending.end()) { pf = it->second; found = true; it->second.committed = true; }
        }
        if (!found)
            return true;

        switch (action)
        {
            case ACTION_FILL:
                DoLfgAutoFill(player, pf);
                break;
            case ACTION_OPTOUT:
                WowPsParty::SetAccountSetting(player->GetSession()->GetAccountId(), "lfg_autofill_optout", true);
                Say(player, "|cff66ccff[WowPsParty]|r Party-fill offers disabled. Re-enable with: |cffffff00.party lfgfill on|r");
                RequeueNormally(player, pf);
                break;
            case ACTION_DECLINE:
            default:
                RequeueNormally(player, pf);
                break;
        }
        return true;
    }
};

namespace WowPsParty
{
    void LfgFill_OnLogout(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_pending.erase(guid.GetCounter());
        // The recruiter is summoner-only with a timed despawn, so it disappears on
        // its own once the summoner is gone — no explicit despawn needed here.
    }
}

void AddPartyLfgFillScripts()
{
    new PartyLfgFillPlayerScript();
    new PartyLfgFillRecruiterScript();
}
