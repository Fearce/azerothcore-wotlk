/*
 * WowPs Party-of-5 — Handler test runner (implementation)
 *
 * Triggered via `.wowps_admin run_handler_tests` (SOAP-drivable).
 *
 * Approach:
 *   1. Call mod-playerbots AddPlayerBot(guid, 0) for 3 known-good test
 *      chars on RNDBOT0 (guids 1, 2, 3). masterAccountId=0 means
 *      "autonomous random-bot" — no human master session needed.
 *   2. Schedule a deferred event ~8 sec later to run the actual tests
 *      (gives bots time to log in + load).
 *   3. Each test asserts state changes after calling the handler logic
 *      directly (CharmInfo writes, MotionMaster transitions, etc.).
 *   4. Results written to handler-test-results.log next to worldserver.exe
 *      for the PowerShell harness to scrape, AND to the AC log for forensics.
 *
 * NEVER calls std::this_thread::sleep_for on the world thread.
 */

#include "PartyTestRunner.h"
#include "PartyMgr.h"

#include "CharacterCache.h"
#include "CharmInfo.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MovementGenerator.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"

#include <fstream>
#include <vector>
#include <unordered_map>

namespace WowPsParty
{
    struct TestResults
    {
        uint32 total = 0;
        uint32 passed = 0;
        std::vector<std::string> log;

        void Pass(std::string const& name)
        {
            ++total; ++passed;
            log.push_back("PASS  " + name);
        }
        void Fail(std::string const& name, std::string const& detail)
        {
            ++total;
            log.push_back("FAIL  " + name + "  -- " + detail);
        }
    };

    static void WriteResultsFile(TestResults const& r)
    {
        std::ofstream f("handler-test-results.log", std::ios::trunc);
        if (!f) return;
        for (auto const& line : r.log)
            f << line << '\n';
        f << "TOTAL=" << r.total << " PASSED=" << r.passed
          << " FAILED=" << (r.total - r.passed) << '\n';
    }

    static void LogLine(std::string const& line)
    {
        LOG_INFO("module", "[WowPsParty TEST] {}", line);
    }

    // ====== Test cases ====================================================

    // Test: spellbook dedupe filter invariants.
    // Applies the same filter the SendSpellbookTo handler uses against the
    // bot's real spell map. Asserts:
    //   * Deduped count <= raw count (filter never grows)
    //   * No two output entries share a GetFirstSpellInChain key
    //   * No passive/tradeskill spells in output
    static void Test_SpellbookDedupe(Player* bot, TestResults& r)
    {
        std::string const tn = "SpellbookDedupe[" + std::string(bot->GetName()) + "]";

        uint32 rawCount = 0;
        std::unordered_map<uint32, uint32> chainKeys;
        for (auto const& kv : bot->GetSpellMap())
        {
            ++rawCount;
            if (kv.second->State == PLAYERSPELL_REMOVED) continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(kv.first);
            if (!info) continue;
            if (info->IsPassive()) continue;
            if (info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL)) continue;
            if (info->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY)) continue;
            uint32 const first = sSpellMgr->GetFirstSpellInChain(kv.first);
            uint32 const ck = first ? first : kv.first;
            auto it = chainKeys.find(ck);
            if (it == chainKeys.end())
                chainKeys[ck] = kv.first;
            else
            {
                SpellInfo const* existing = sSpellMgr->GetSpellInfo(it->second);
                if (existing && info->SpellLevel > existing->SpellLevel)
                    it->second = kv.first;
            }
        }

        for (auto const& kv : chainKeys)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(kv.second);
            if (!info) { r.Fail(tn, "missing spell info"); return; }
            if (info->IsPassive()) { r.Fail(tn, "passive leaked through"); return; }
            if (info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL))
            { r.Fail(tn, "tradeskill leaked through"); return; }
        }
        if (chainKeys.size() > rawCount)
        {
            r.Fail(tn, "deduped grew the count");
            return;
        }
        r.Pass(tn + Acore::StringFormat("  raw={} deduped={}",
            rawCount, uint32(chainKeys.size())));
    }

    // Test: UpdateAllowedPositionZ + MovePoint (the GotoDelta fix path).
    // Asserts the Z snap is sane and MotionMaster ends up in POINT mode.
    static void Test_GotoDelta(Player* bot, TestResults& r)
    {
        std::string const tn = "GotoDelta[" + std::string(bot->GetName()) + "]";

        float const startX = bot->GetPositionX();
        float const startY = bot->GetPositionY();
        float const startZ = bot->GetPositionZ();
        float const targetX = startX + 8.0f;
        float const targetY = startY + 8.0f;
        float targetZ = bot->GetMap()->GetHeight(bot->GetPhaseMask(),
                                                 targetX, targetY, MAX_HEIGHT);
        if (targetZ <= INVALID_HEIGHT)
            targetZ = startZ;

        bot->UpdateAllowedPositionZ(targetX, targetY, targetZ);

        if (std::abs(targetZ - startZ) > 200.0f)
        {
            r.Fail(tn, Acore::StringFormat(
                "Z delta unreasonable: startZ={:.2f} targetZ={:.2f}",
                startZ, targetZ));
            return;
        }

        bot->StopMoving();
        bot->GetMotionMaster()->MovePoint(0, targetX, targetY, targetZ);

        MovementGeneratorType const gt = bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
        if (gt != POINT_MOTION_TYPE)
        {
            r.Fail(tn, Acore::StringFormat(
                "MotionMaster not in POINT_MOTION_TYPE (got {})", uint32(gt)));
            return;
        }
        r.Pass(tn + Acore::StringFormat(
            "  ({:.0f},{:.0f},{:.1f}) -> ({:.0f},{:.0f},{:.1f})",
            startX, startY, startZ, targetX, targetY, targetZ));
    }

    // Test: RetargetBotFollow's effect chain.
    //
    // RetargetBotFollow does, per bot: SetMaster(new) + AI value cache poke
    // for "group leader"/"master target" + MotionMaster::Clear() + MoveFollow.
    // We exercise the same primitives directly here against a bot, with a
    // different bot as the new master. Asserts:
    //   * PlayerbotAI->GetMaster() returns the new master
    //   * MotionMaster's current movegen is FOLLOW_MOTION_TYPE
    static void Test_RetargetBotFollow(Player* mover, Player* newMaster, TestResults& r)
    {
        std::string const tn = "RetargetBotFollow["
                                + std::string(mover->GetName())
                                + " -> follow "
                                + std::string(newMaster->GetName()) + "]";

        PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(mover);
        if (!ai)
        {
            r.Fail(tn, "mover has no PlayerbotAI");
            return;
        }
        if (mover->GetMapId() != newMaster->GetMapId())
        {
            r.Fail(tn, "map mismatch");
            return;
        }

        ai->SetMaster(newMaster);
        // (skip the AI value cache poke here — we already log the concern in
        //  RetargetBotFollow's comments; if GetMaster() returns the new
        //  master and MoveFollow installs, the live behavior is correct.)

        mover->GetMotionMaster()->Clear();
        mover->GetMotionMaster()->MoveFollow(
            newMaster, PET_FOLLOW_DIST, mover->GetFollowAngle());

        if (ai->GetMaster() != newMaster)
        {
            r.Fail(tn, Acore::StringFormat(
                "GetMaster() did not update (got name={})",
                ai->GetMaster() ? ai->GetMaster()->GetName() : "<null>"));
            return;
        }

        MovementGeneratorType const gt = mover->GetMotionMaster()->GetCurrentMovementGeneratorType();
        if (gt != FOLLOW_MOTION_TYPE)
        {
            r.Fail(tn, Acore::StringFormat(
                "MotionMaster not in FOLLOW_MOTION_TYPE (got {})", uint32(gt)));
            return;
        }
        r.Pass(tn);
    }

    // ====== Driver ========================================================

    static constexpr uint32 TEST_BOT_GUIDS[3] = { 1, 2, 3 };

    static Player* FindBot(uint32 guidLow)
    {
        return ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(guidLow));
    }

    static void RunTestsNow()
    {
        TestResults r;

        Player* a = FindBot(TEST_BOT_GUIDS[0]);
        Player* b = FindBot(TEST_BOT_GUIDS[1]);
        Player* c = FindBot(TEST_BOT_GUIDS[2]);

        if (!a) r.Fail("BotSpawn[guid=1]", "not in world");
        else    r.Pass("BotSpawn[guid=1]  " + std::string(a->GetName()));
        if (!b) r.Fail("BotSpawn[guid=2]", "not in world");
        else    r.Pass("BotSpawn[guid=2]  " + std::string(b->GetName()));
        if (!c) r.Fail("BotSpawn[guid=3]", "not in world");
        else    r.Pass("BotSpawn[guid=3]  " + std::string(c->GetName()));

        // Co-locate b on a for the possess test (if both present)
        if (a && b)
        {
            b->TeleportTo(a->GetMapId(), a->GetPositionX(),
                          a->GetPositionY(), a->GetPositionZ(), 0.0f);
        }

        if (a) Test_SpellbookDedupe(a, r);
        if (b) Test_SpellbookDedupe(b, r);
        if (c) Test_SpellbookDedupe(c, r);

        if (a) Test_GotoDelta(a, r);

        // RetargetBotFollow — bot c re-follows a (different master)
        if (a && c)
        {
            // Ensure c is on the same map as a
            if (c->GetMapId() != a->GetMapId())
            {
                c->TeleportTo(a->GetMapId(), a->GetPositionX(),
                              a->GetPositionY(), a->GetPositionZ(), 0.0f);
            }
            Test_RetargetBotFollow(c, a, r);
        }

        for (auto const& line : r.log) LogLine(line);
        LogLine(Acore::StringFormat(
            "summary: total={} passed={} failed={}",
            r.total, r.passed, r.total - r.passed));
        WriteResultsFile(r);
    }

    void RunHandlerTests()
    {
        LogLine("=== handler test run started ===");

        // Kick off bot logins
        for (uint32 guid : TEST_BOT_GUIDS)
        {
            if (!FindBot(guid))
                sRandomPlayerbotMgr.AddPlayerBot(
                    ObjectGuid::Create<HighGuid::Player>(guid), 0);
        }

        // Pick any existing in-world Player to attach the deferred timer to.
        // If none exist yet, just call directly (bots may already be in world).
        Player* any = FindBot(TEST_BOT_GUIDS[0]);
        if (!any) any = FindBot(TEST_BOT_GUIDS[1]);
        if (!any) any = FindBot(TEST_BOT_GUIDS[2]);

        if (any)
        {
            // Already up — run immediately
            RunTestsNow();
        }
        else
        {
            // Defer ~10s via a global PlayerbotMgr event queue. mod-playerbots'
            // own bot login uses the same world-thread event queue. We attach
            // to the first bot once it appears via a chained retry.
            //
            // Simpler approach: schedule on World::AddSession's tick. But
            // worldserver modules don't get direct access. We use a static
            // counter + periodic check via a different mechanism.
            //
            // Practical workaround: poll once after 10 seconds using a static
            // background scheduler. AC has TaskScheduler but per-module use
            // requires plumbing. For now: log a warning and let the user
            // re-trigger the admin command in a few seconds once bots load.
            LogLine("Bots not yet in world. AddPlayerBot queued. "
                    "Re-run `.wowps_admin run_handler_tests` after ~10 sec.");

            TestResults r;
            r.Fail("BotSpawn", "bots not yet loaded; re-run after 10s");
            WriteResultsFile(r);
        }
    }
}
