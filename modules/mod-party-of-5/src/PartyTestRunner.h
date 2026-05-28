/*
 * WowPs Party-of-5 — In-process handler test runner
 *
 * Purpose: drive every WPSP server-side handler against REAL Player objects
 * spawned via mod-playerbots, with NO WoW client needed. Triggered via the
 * `.wowps_admin run_handler_tests` GM command (SOAP-drivable).
 *
 * Why this exists: SOAP-based DB-assertion tests cover only handlers whose
 * effects show up purely in the DB (e.g. BOOTSTRAP_PARTY). PET_BAR_SET,
 * GOTO_DELTA, RetargetBotFollow, spellbook dedupe all need actual Player
 * objects in the world. So we spawn three autonomous bots (mod-playerbots
 * with masterAccountId=0), wait for them to be live, then call the
 * handlers directly and assert state changes.
 *
 * Results are streamed both to the worldserver log (LOG_INFO scope=module)
 * AND to a fixed file (handler-test-results.log next to worldserver.exe)
 * so the PowerShell test harness can read PASS/FAIL totals deterministically.
 */
#ifndef WOWPSPARTY_PARTYTESTRUNNER_H
#define WOWPSPARTY_PARTYTESTRUNNER_H

#include <string>

namespace WowPsParty
{
    // Kicks off the test run. Spawns/locates 3 bot Players in-world (deferred
    // until they're loaded), runs each test, writes pass/fail to log + file.
    // Safe to call repeatedly; idempotent. Logs go to module scope.
    void RunHandlerTests();
}

#endif
