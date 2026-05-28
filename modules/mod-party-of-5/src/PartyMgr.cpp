/*
 * WowPs Party-of-5 mod — PartyMgr implementation
 */

#include "PartyMgr.h"
#include "PartyFollow.h"

#include "Chat.h"
#include "CharacterCache.h"
#include "CharmInfo.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LootMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Unit.h"
#include "WorldSession.h"

// mod-playerbots (AC's modules build adds every subdir of every module to the
// include path, so no `Bot/` prefix is needed)
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "AiObjectContext.h"
#include "Value.h"

#include "MotionMaster.h"
#include "Pet.h"  // PET_FOLLOW_DIST

#include "WorldSession.h"
#include "WorldPacket.h"
#include "Opcodes.h"

// Forward declarations of helpers defined in PartyAddonProtocol.cpp / PartyRotation.cpp
namespace WowPsParty
{
    void SendRosterTo(Player* player);
    void SendSwappedTo(Player* player, int oldSlot, int newSlot);
    void RotationCacheRefreshFromDB(uint32 guid);
    void PushControlledLoadoutTo(Player* requester, int slot);
}

namespace WowPsParty
{
    PartyMgr& PartyMgr::Instance()
    {
        static PartyMgr instance;
        return instance;
    }

    static uint32 FetchAccountForGuid(uint32 guid)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `account` FROM `characters` WHERE `guid` = {}", guid);
        if (!q)
            return 0;
        return q->Fetch()[0].Get<uint32>();
    }

    static std::vector<std::pair<uint8 /*slot*/, uint32 /*guid*/>> FetchPartyRows(uint32 accountId)
    {
        std::vector<std::pair<uint8, uint32>> rows;
        QueryResult q = CharacterDatabase.Query(
            "SELECT `slot`, `guid` FROM `account_party` WHERE `account` = {} ORDER BY `slot`", accountId);
        if (!q)
            return rows;
        do
        {
            Field* f = q->Fetch();
            rows.emplace_back(f[0].Get<uint8>(), f[1].Get<uint32>());
        } while (q->NextRow());
        return rows;
    }

    EnrollResult PartyMgr::Enroll(Player* requestor, uint32 targetGuid, std::string const& targetName)
    {
        if (!requestor || !requestor->GetSession())
            return EnrollResult::DatabaseError;

        uint32 const requestorAccount = requestor->GetSession()->GetAccountId();

        // Default: enroll the requestor's own currently-logged-in character.
        if (targetGuid == 0)
            targetGuid = requestor->GetGUID().GetCounter();

        // Verify the target character belongs to the requestor's account.
        uint32 const targetAccount = FetchAccountForGuid(targetGuid);
        if (targetAccount == 0)
            return EnrollResult::TargetNotFound;
        if (targetAccount != requestorAccount)
            return EnrollResult::TakenByAnotherAccount;

        // Already enrolled?
        QueryResult existing = CharacterDatabase.Query(
            "SELECT `account`, `slot` FROM `account_party` WHERE `guid` = {}", targetGuid);
        if (existing)
        {
            uint32 const ownerAccount = existing->Fetch()[0].Get<uint32>();
            if (ownerAccount == requestorAccount)
                return EnrollResult::AlreadyEnrolled;
            return EnrollResult::TakenByAnotherAccount;
        }

        // Find next free slot 0..4 in this account.
        auto const rows = FetchPartyRows(requestorAccount);
        if (rows.size() >= PARTY_SIZE)
            return EnrollResult::PartyFull;

        uint8 nextSlot = 0;
        for (uint8 candidate = 0; candidate < PARTY_SIZE; ++candidate)
        {
            bool taken = false;
            for (auto const& row : rows)
            {
                if (row.first == candidate) { taken = true; break; }
            }
            if (!taken) { nextSlot = candidate; break; }
        }

        // Transactional insert: account_party row + characters.party_slot column.
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append(
            "INSERT INTO `account_party` (`account`, `slot`, `guid`, `is_active_on_login`) "
            "VALUES ({}, {}, {}, {})",
            requestorAccount, nextSlot, targetGuid,
            (nextSlot == 0 ? 1u : 0u));
        tx->Append(
            "UPDATE `characters` SET `party_slot` = {} WHERE `guid` = {}",
            nextSlot, targetGuid);
        CharacterDatabase.CommitTransaction(tx);

        LOG_INFO("module",
                 "[WowPsParty] enroll: account={} guid={} name={} slot={}",
                 requestorAccount, targetGuid, targetName, nextSlot);

        return EnrollResult::Ok;
    }

    bool PartyMgr::Leave(Player* requestor)
    {
        if (!requestor || !requestor->GetSession())
            return false;

        uint32 const guid = requestor->GetGUID().GetCounter();
        uint32 const account = requestor->GetSession()->GetAccountId();

        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append(
            "DELETE FROM `account_party` WHERE `account` = {} AND `guid` = {}",
            account, guid);
        tx->Append(
            "UPDATE `characters` SET `party_slot` = NULL WHERE `guid` = {}",
            guid);
        // Also clear any loadout for this character.
        tx->Append(
            "DELETE FROM `party_loadout` WHERE `guid` = {}", guid);
        CharacterDatabase.CommitTransaction(tx);

        LOG_INFO("module", "[WowPsParty] leave: account={} guid={}", account, guid);
        return true;
    }

    std::vector<PartyMember> PartyMgr::GetParty(uint32 accountId)
    {
        std::vector<PartyMember> out;

        QueryResult q = CharacterDatabase.Query(
            "SELECT ap.`slot`, ap.`guid`, c.`name`, c.`class`, c.`level` "
            "FROM `account_party` ap "
            "JOIN `characters` c ON c.`guid` = ap.`guid` "
            "WHERE ap.`account` = {} ORDER BY ap.`slot`", accountId);
        if (!q)
            return out;

        do
        {
            Field* f = q->Fetch();
            PartyMember m;
            m.slot    = f[0].Get<uint8>();
            m.guid    = f[1].Get<uint32>();
            m.name    = f[2].Get<std::string>();
            m.classId = f[3].Get<uint8>();
            m.level   = f[4].Get<uint8>();
            m.online  = ObjectAccessor::FindPlayerByLowGUID(m.guid) != nullptr;
            out.push_back(std::move(m));
        } while (q->NextRow());

        return out;
    }

    std::optional<uint8> PartyMgr::GetSlotForGuid(uint32 guid)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `party_slot` FROM `characters` WHERE `guid` = {}", guid);
        if (!q)
            return std::nullopt;

        Field* f = q->Fetch();
        if (f[0].IsNull())
            return std::nullopt;
        return f[0].Get<uint8>();
    }

    void PartyMgr::OnActiveLogin(Player* active)
    {
        if (!active || !active->GetSession())
            return;

        uint32 const account = active->GetSession()->GetAccountId();
        uint32 const activeGuid = active->GetGUID().GetCounter();

        auto const rows = FetchPartyRows(account);
        if (rows.empty())
            return;  // not enrolled — nothing to spawn

        // mod-playerbots' PlayerScript::OnLogin creates the PlayerbotMgr. AC
        // dispatches PlayerScript hooks in REGISTRATION order, not
        // alphabetical (earlier comment was wrong) -- and our PartyHooks
        // script registers first, so on first login `botMgr` may be null
        // here. Defer the bot-spawn body by 1s via the player's event queue
        // so mod-playerbots has a chance to wire up its manager.
        ObjectGuid const activeObjGuid2 = active->GetGUID();
        active->m_Events.AddEventAtOffset([activeObjGuid2, account, activeGuid, rows]()
        {
            Player* p = ObjectAccessor::FindConnectedPlayer(activeObjGuid2);
            if (!p) return;
            PlayerbotMgr* botMgr = sPlayerbotsMgr.GetPlayerbotMgr(p);
            if (!botMgr)
            {
                LOG_WARN("module",
                         "[WowPsParty] OnActiveLogin (deferred): still no PlayerbotMgr "
                         "for guid={}; mod-playerbots is genuinely missing. Idle party "
                         "members will NOT spawn. Re-login should fix.", activeGuid);
                return;
            }
            // WoW party is capped at 5 members. Active player counts as 1,
            // so spawn at most 4 bots. Extra enrolled chars beyond that just
            // sit out this session.
            uint8 spawned = 0;
            for (auto const& row : rows)
            {
                uint32 const guid = row.second;
                RotationCacheRefreshFromDB(guid);
                if (guid == activeGuid) continue;
                if (spawned >= 4) break;
                ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
                botMgr->AddPlayerBot(og, account);
                ++spawned;
            }
            LOG_INFO("module",
                "[WowPsParty] OnActiveLogin (deferred): spawned {} idle bot(s)", spawned);

            // Install follow directives now that the bots are loaded. This
            // ALSO acts as the "is a party-of-5 bot" gate used by the
            // patched PlayerbotAI::UpdateAI — without it, the AI doesn't
            // know to suppress the default rotation and bots fall back to
            // their class strategy (priests Smite, mages Fireball, etc.).
            WowPsParty::SetActiveFollowers(account,
                ObjectGuid::Create<HighGuid::Player>(activeGuid));
        }, std::chrono::seconds(1));


        // (Previously: attached PlayerbotAI to the active session player so the
        // body the user leaves behind on swap could be driven by AI. But the
        // ResetStrategies invocation that RetargetBotFollow fires on every swap
        // appears to clobber the just-established charm state — the swap returns
        // Ok, the chat messages fire, but the visible control never transfers.
        // Removed until we can attach the AI in a way that doesn't ResetStrategies
        // immediately on the active body. The "vacated body has no AI" trade-off
        // is a regression; will revisit by either (a) using a thin custom follower
        // AI that doesn't share mod-playerbots' tick path, or (b) deferring the
        // AI attach until *after* the swap settles.)

        // NOTE: Earlier versions attached PlayerbotAI to the session player
        // here so the vacated body could be AI-driven (fight, cast, etc.)
        // while the user controlled a different body. Every implementation
        // attempt broke SetCharmedBy semantics for subsequent swaps -- the
        // first swap worked, the second showed pet bar but no control
        // transfer. The conflict appears fundamental between mod-playerbots'
        // AI lifecycle and a Player owning POSSESS charms repeatedly.
        //
        // Trade-off: vacated body no longer FIGHTS while you control someone
        // else. It still WALKS to follow the new leader via the MoveFollow
        // installed in RetargetBotFollow. The 4 other bots still have full
        // AI (they're spawned via mod-playerbots' normal path, not attached
        // post-hoc) so they fight + loot + cast as before.
        ObjectGuid const activeObjGuid = active->GetGUID();

        // Ensure all 5 are in one Group with the active as leader and FFA
        // loot. Run after a short delay so mod-playerbots' bot-spawn callbacks
        // have settled into the world.
        active->m_Events.AddEventAtOffset([activeObjGuid]()
        {
            Player* leader = ObjectAccessor::FindConnectedPlayer(activeObjGuid);
            if (!leader || !leader->GetSession()) return;
            uint32 const account = leader->GetSession()->GetAccountId();

            Group* group = leader->GetGroup();
            if (!group)
            {
                group = new Group();
                if (!group->Create(leader))
                {
                    delete group;
                    return;
                }
                sGroupMgr->AddGroup(group);
            }
            group->SetLootMethod(FREE_FOR_ALL);

            // Walk the party and invite every member that isn't already in.
            auto const rows = FetchPartyRows(account);
            for (auto const& row : rows)
            {
                ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(row.second);
                if (og == leader->GetGUID()) continue;
                Player* mem = ObjectAccessor::FindConnectedPlayer(og);
                if (!mem || mem->GetGroup() == group) continue;
                if (mem->GetGroup())
                    mem->RemoveFromGroup();
                group->AddMember(mem);
            }

            // Enable auto-loot on every bot in the party so kills get looted
            // without the user having to right-click every corpse. mod-playerbots'
            // "+loot" strategy in BOT_STATE_NON_COMBAT triggers LootAction on
            // nearby unlooted corpses; "+gather" picks up herbs/ore. Idempotent —
            // ChangeStrategy on an already-enabled strategy is a no-op.
            for (auto const& row : rows)
            {
                ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(row.second);
                Player* mem = ObjectAccessor::FindConnectedPlayer(og);
                if (!mem) continue;
                if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(mem))
                {
                    ai->ChangeStrategy("+loot", BOT_STATE_NON_COMBAT);
                    ai->ChangeStrategy("+gather", BOT_STATE_NON_COMBAT);
                }
            }
        }, std::chrono::seconds(6));

        LOG_INFO("module",
                 "[WowPsParty] OnActiveLogin: account={} active_guid={} -- bot spawn deferred by 1s",
                 account, activeGuid);
    }

    // Re-target every party-member bot's "master" pointer so they follow the
    // unit the player is *actually* driving right now. Without this, the bots
    // keep tailing the session's player even after the player has swapped to
    // a different body via possess — they stand around the empty husk.
    //
    // Walks the account's party_loadout rows, gets each loaded Player, finds
    // its PlayerbotAI (only set on bot characters — the active session's player
    // doesn't have one), and rewrites the master. Skips the target itself so
    // we don't ask the controlled bot to follow… itself.
    //
    // NOTE: we deliberately do NOT call ResetStrategies here. That method tears
    // down engine state and re-initialises movement/targeting, which has the
    // side effect of clobbering the just-established charm on the freshly-
    // possessed target (visible as "swap returns Ok but no control transfer").
    // SetMaster on its own is a pointer swap — safe.
    static void RetargetBotFollow(uint32 account, ObjectGuid newMasterGuid, Player* newMasterPlayer)
    {
        // ENTIRELY REWRITTEN. Old version manipulated mod-playerbots' AI
        // values (SetMaster, ChangeStrategy, AI value cache pokes) plus our
        // own MoveFollow. That whole stack was unreliable across rapid
        // swaps because mod-playerbots' CalculatedValue caches kept getting
        // back to stale state within ~1 tick of our changes, leaving bots
        // glued to the original session player.
        //
        // New approach: a dedicated tick-driven "always follow group leader"
        // re-asserter (PartyFollow.cpp). Every 1 second it walks the
        // current directives and calls MoveFollow on each follower targeting
        // the leader. mod-playerbots' AI can still run combat / cast / etc.
        // but its FollowAction's stale cache stops mattering -- our ticker
        // overwrites the follow target every second.
        //
        // RetargetBotFollow's only job now: update the directives table
        // with the new leader. Combat-state / charm-state skip-logic lives
        // in the ticker per-tick, not here per-swap.
        LOG_INFO("module",
            "[WowPsParty] RetargetBotFollow: account={} new_leader_guid={} name={}",
            account, newMasterGuid.GetCounter(),
            newMasterPlayer ? newMasterPlayer->GetName() : "<null>");
        WowPsParty::SetActiveFollowers(account, newMasterGuid);
        // No per-bot motion manipulation here -- the ticker handles it.
        // The auditor noted this was destabilising charms on the controller
        // when called inline with SetCharmedBy. Pushing it to a separate
        // thread context (well, WorldScript::OnUpdate is the same thread,
        // but at least it's a different stack frame and not interleaved
        // with the SetCharmedBy packet flush) avoids that interaction.
    }

    SwapResult PartyMgr::SwapTo(Player* requestor, uint8 targetSlot)
    {
        if (!requestor || !requestor->GetSession())
            return SwapResult::TargetNotInWorld;

        if (targetSlot >= PARTY_SIZE)
            return SwapResult::InvalidSlot;

        // No swaps in battlegrounds / arenas — too many edge cases (queue state,
        // team balance, score attribution). Phase 5 may revisit.
        if (requestor->InBattleground() || requestor->InArena())
            return SwapResult::InBattleground;

        uint32 const account = requestor->GetSession()->GetAccountId();

        // Find the target slot's guid for this account.
        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid` FROM `account_party` "
            "WHERE `account` = {} AND `slot` = {}",
            account, uint32(targetSlot));
        if (!q)
            return SwapResult::InvalidSlot;
        uint32 const targetGuidLow = q->Fetch()[0].Get<uint32>();

        ObjectGuid const targetGuid = ObjectGuid::Create<HighGuid::Player>(targetGuidLow);

        // Special case: swapping to the slot that IS the session player itself
        // means "drop any active possess and return to your real body" — same
        // semantics as .party unswap. Without this we'd try to SetCharmedBy on
        // ourselves, which AC explicitly rejects ("trying to charm itself").
        if (targetGuid == requestor->GetGUID())
        {
            Unit* currentCharm = requestor->GetCharm();
            if (currentCharm)
            {
                // Save the controlled body's position BEFORE releasing the
                // charm so we can teleport tank there. Gives the illusion
                // that tank traveled with the party (he didn't -- he was
                // standing still under POSSESS's UNIT_FLAG_DISABLE_MOVE --
                // but he appears at wherever you ended up).
                uint16 const reunionMap = currentCharm->GetMapId();
                float const reunionX = currentCharm->GetPositionX();
                float const reunionY = currentCharm->GetPositionY();
                float const reunionZ = currentCharm->GetPositionZ();
                float const reunionO = currentCharm->GetOrientation();

                currentCharm->RemoveCharmedBy(requestor);

                // Reunite with the party on swap-back. Skip if already
                // close (e.g., user didn't move the controlled body far).
                if (requestor->GetMapId() != reunionMap ||
                    requestor->GetDistance(reunionX, reunionY, reunionZ) > 5.0f)
                {
                    requestor->TeleportTo(reunionMap, reunionX, reunionY, reunionZ, reunionO);
                }
                // DO NOT remove the AI from the session player here. The
                // pause guard in PlayerbotAI's tick (`mgr && !charm`) already
                // keeps it dormant while the user is directly controlling
                // this body. Removing the AI breaks the NEXT swap-away --
                // the body is left without AI, just stares at the wall.
                uint32 const account = requestor->GetSession()->GetAccountId();
                CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
                tx->Append("UPDATE `account_party` SET `is_active_on_login` = 0 WHERE `account` = {}", account);
                tx->Append("UPDATE `account_party` SET `is_active_on_login` = 1 "
                           "WHERE `account` = {} AND `slot` = {}", account, uint32(targetSlot));
                CharacterDatabase.CommitTransaction(tx);
                RetargetBotFollow(account, requestor->GetGUID(), requestor);
                SendSwappedTo(requestor, -1, int(targetSlot));
                PushControlledLoadoutTo(requestor, int(targetSlot));
                LOG_INFO("module", "[WowPsParty] SwapTo: account={} returning to session player (slot={})",
                         account, uint32(targetSlot));
            }
            return SwapResult::Ok;
        }

        Player* target = ObjectAccessor::FindConnectedPlayer(targetGuid);
        if (!target || !target->IsInWorld())
            return SwapResult::TargetNotInWorld;

        if (!target->IsAlive())
            return SwapResult::TargetIsDead;

        // If the requestor is *already* controlling this slot, nothing to do.
        Unit* currentCharm = requestor->GetCharm();
        if (currentCharm && currentCharm->GetGUID() == targetGuid)
            return SwapResult::AlreadyControllingTarget;

        // Drop any existing possess first. RemoveCharmedBy reverses everything
        // SetCharmedBy did: camera back to charmer, charm bond broken, mover/
        // active-mover/client-control reset, UNIT_FLAG_POSSESSED cleared.
        // (StopCastingCharm alone only aborts the in-progress spell that
        // *initiated* the charm; for charms we created via the raw SetCharmedBy
        // API there is no spell to stop, so it was a no-op — which is why the
        // first swap stuck and subsequent swaps failed.)
        if (currentCharm)
            currentCharm->RemoveCharmedBy(requestor);

        // Establish new possess. AC's SetCharmedBy with CHARM_TYPE_POSSESS does:
        //   - Camera follows target  (SetView)
        //   - Charm bond              (SetCharm + SetCharmer)
        //   - Active mover            (SetMover)
        //   - Client-side control     (SetClientControl/UpdateClientControl)
        //   - UNIT_FLAG_POSSESSED on target
        // — i.e., exactly the per-frame state our F1-F5 swap needs.
        if (!target->SetCharmedBy(requestor, CHARM_TYPE_POSSESS))
        {
            LOG_WARN("module",
                     "[WowPsParty] SwapTo: SetCharmedBy failed account={} from_guid={} to_slot={} target_guid={}",
                     account, requestor->GetGUID().GetCounter(), uint32(targetSlot), targetGuidLow);
            return SwapResult::VehicleSetupFailed;
        }

        // (Previously tried RemoveUnitFlag(UNIT_FLAG_DISABLE_MOVE) here so
        // the charmer/vacated body could walk via PartyFollow. The follow
        // ticker's MoveFollow/NearTeleportTo on the charmer triggered AC
        // anti-cheat/position-sync paths that cleared UNIT_FLAG_POSSESSED
        // on the controlled body within a few seconds -- AI then took over
        // the controlled body. Reverted: tank stays stationary during
        // possess. Charm stability > vacated-body-follows-leader.)

        // Mark the new slot as the active-on-login default for next session.
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append("UPDATE `account_party` SET `is_active_on_login` = 0 WHERE `account` = {}", account);
        tx->Append("UPDATE `account_party` SET `is_active_on_login` = 1 "
                   "WHERE `account` = {} AND `slot` = {}", account, uint32(targetSlot));
        CharacterDatabase.CommitTransaction(tx);

        // Notify the client-side addon so the portrait bar can re-highlight
        // the active slot. oldSlot = -1 if requestor wasn't previously controlling
        // anything (i.e., this is the first swap from their session-bound body).
        int oldSlot = -1;
        if (currentCharm)
        {
            if (auto s = GetSlotForGuid(currentCharm->GetGUID().GetCounter()))
                oldSlot = int(*s);
        }
        SendSwappedTo(requestor, oldSlot, int(targetSlot));

        // Push the controlled body's spellbook + action bar layout so the
        // addon's custom 12-slot bar re-renders for the new active char.
        PushControlledLoadoutTo(requestor, int(targetSlot));

        // The other 3 bots were following `requestor`. Move their master pointer
        // to `target` so they fall in line behind the body the player is now driving.
        RetargetBotFollow(account, targetGuid, target);

        // (Earlier: attach AI to requestor with deferred ResetStrategies so
        // the vacated body could be AI-driven. Even with the 500ms delay this
        // re-introduced the "swap returns Ok but visual control never
        // transfers" regression. Removed for stability — the body the user
        // leaves behind now just stands still. We'll re-do this via a thin
        // custom follower AI (no PlayerbotAI/ResetStrategies) in a later pass.)

        // Promote target to group leader so loot, marking, and ready-checks key off
        // the body the user is actually driving. Group is shared across all 5; mod-
        // playerbots auto-invited the others into requestor's group on OnActiveLogin.
        if (Group* g = requestor->GetGroup())
        {
            bool const isMember = g->IsMember(targetGuid);
            bool const alreadyLeader = (g->GetLeaderGUID() == targetGuid);
            LOG_INFO("module",
                "[WowPsParty] SwapTo: group leader check -- current_leader={} target_guid={} is_member={} already_leader={}",
                g->GetLeaderGUID().GetCounter(), targetGuid.GetCounter(),
                isMember, alreadyLeader);
            if (!alreadyLeader && isMember)
            {
                g->ChangeLeader(targetGuid);
                g->SendUpdate();
                LOG_INFO("module",
                    "[WowPsParty] SwapTo: ChangeLeader fired -> {}  (post-leader_guid={})",
                    targetGuid.GetCounter(), g->GetLeaderGUID().GetCounter());
            }
        }
        else
        {
            LOG_WARN("module", "[WowPsParty] SwapTo: requestor has NO group -- bots can't follow via group leader");
        }

        // (Earlier I forced-saved both endpoints here as "Phase-6 hardening"
        // but `Player::SaveToDB` on a freshly-possessed unit re-serialises
        // movement/position state that undoes the charm in-memory, breaking
        // the swap visually even though our DB tracking is correct. Crash
        // recovery isn't worth the regression — removed.)

        LOG_INFO("module",
                 "[WowPsParty] SwapTo: account={} session_guid={} -> slot={} target_guid={} name={}",
                 account, requestor->GetGUID().GetCounter(), uint32(targetSlot),
                 targetGuidLow, target->GetName());

        return SwapResult::Ok;
    }

    // True character-switch via logout/login. The user's WorldSession actually
    // re-binds to a different Player object, sidestepping all the AC-charm
    // limitations that plagued the POSSESS-based SwapTo (charmer can't move,
    // possess breaks on charmer motion, AI takes over after a few seconds).
    //
    // Cost: ~2-3 second loading screen per swap. Win: the new active char
    // IS the session player, so movement / casting / loot all work without
    // any charm complications. Vacated char becomes a normal bot driven by
    // mod-playerbots.
    //
    // Sequence (all deferred so the in-flight chat command can return first):
    //   T+50ms : if target is currently a bot in world, LogoutPlayerBot it
    //   T+150ms: session->LogoutPlayer(true) on current char (saves state)
    //   T+150ms: HandlePlayerLoginOpcode with the new char's GUID
    //   ~T+2s  : client finishes loading screen, world view restored on
    //            the new char; OnActiveLogin fires + re-spawns missing bots
    SwapResult PartyMgr::SwapToViaRelogin(Player* requestor, uint8 targetSlot)
    {
        if (!requestor || !requestor->GetSession())
            return SwapResult::TargetNotInWorld;
        if (targetSlot >= PARTY_SIZE)
            return SwapResult::InvalidSlot;
        if (requestor->InBattleground() || requestor->InArena())
            return SwapResult::InBattleground;

        uint32 const account = requestor->GetSession()->GetAccountId();
        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}",
            account, uint32(targetSlot));
        if (!q) return SwapResult::InvalidSlot;
        uint32 const targetGuidLow = q->Fetch()[0].Get<uint32>();
        ObjectGuid const targetGuid = ObjectGuid::Create<HighGuid::Player>(targetGuidLow);

        if (targetGuid == requestor->GetGUID())
            return SwapResult::AlreadyControllingTarget;

        // Mark new slot as active-on-login default.
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        tx->Append("UPDATE `account_party` SET `is_active_on_login` = 0 WHERE `account` = {}", account);
        tx->Append("UPDATE `account_party` SET `is_active_on_login` = 1 "
                   "WHERE `account` = {} AND `slot` = {}", account, uint32(targetSlot));
        CharacterDatabase.CommitTransaction(tx);

        // Drop any stale POSSESS first.
        if (Unit* charm = requestor->GetCharm())
            charm->RemoveCharmedBy(requestor);

        // QUEUE the swap to be processed on the next world tick. We can't
        // do the quiet-logout inline because we'd delete the in-flight
        // session player from inside its own chat-handler call stack.
        // PartyFollow's WorldScript::OnUpdate drains the queue.
        WowPsParty::QueueQuietRelogin(account, targetGuid);
        return SwapResult::Ok;
    }

    bool PartyMgr::Unswap(Player* requestor)
    {
        if (!requestor || !requestor->GetSession())
            return false;
        Unit* charm = requestor->GetCharm();
        if (!charm)
            return false;
        ObjectGuid const formerCharmGuid = charm->GetGUID();
        charm->RemoveCharmedBy(requestor);

        // Drop the AI we attached when the user swapped *away* — user is now
        // driving this body directly again, the AI must not tick on it.
        if (sPlayerbotsMgr.GetPlayerbotAI(requestor))
            sPlayerbotsMgr.RemovePlayerBotData(requestor->GetGUID(), /*is_AI=*/true);

        // Restore bot followers to the session's real player.
        uint32 const account = requestor->GetSession()->GetAccountId();
        RetargetBotFollow(account, requestor->GetGUID(), requestor);

        LOG_INFO("module", "[WowPsParty] Unswap: guid={} released charm of guid={}",
                 requestor->GetGUID().GetCounter(), formerCharmGuid.GetCounter());
        return true;
    }
}
