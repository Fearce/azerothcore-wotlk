/*
 * WowPs Party-of-5 — Dungeon tank routing: recorded playback + auto-routing.
 *
 * There are TWO route sources, and the tank prefers them in this order:
 *
 *   1) A RECORDED route for the wing (see below) — a route a human walked, so
 *      it takes the corners the way you wanted them taken.
 *   2) An AUTO route, generated live from the navmesh (no recording needed).
 *      This is what makes an un-recorded dungeon work: the module reads the
 *      dungeon's boss list out of DungeonEncounter.dbc, walks the tank to the
 *      nearest one that is still alive, and re-picks when it dies. See the
 *      "Auto-routing" block in PartyPath.cpp.
 *
 *      Not every objective is a boss in a room. The same block also drives the
 *      SET-PIECES a dungeon opens with a conversation — an escort to start and
 *      walk with, a wave fight that begins when somebody says they are ready, a
 *      boss standing there waiting to be spoken to. Only options the DATABASE
 *      proves are safe to click are taken; see the "Events" block for the rule
 *      and for why a raid is treated differently.
 *
 * Both produce the same shape — a dense polyline — so the playback machinery
 * (steer node, lead/halt hysteresis, stall blink) is shared.
 *
 * Idea: walk through each dungeon once in a GM-mode/superspeed ghost while
 * the server samples your position. Subsequent runs, the assigned tank
 * walks that path automatically — so they actually navigate the corridor
 * the way you taught them, not just stand 12y in front of you.
 *
 * Storage:  dungeon_path(map_id, area_id, sequence, x, y, z, orientation)
 *   area_id keys the recording to a WING: multi-wing dungeons (Scarlet Monastery
 *   x4, Dire Maul x3) share one map id, so each wing stores its own path under
 *   the recorder's area, and playback picks the wing nearest the leader.
 *
 * Recording lifecycle:
 *   1) User presses WOWPS_PARTY_RECORD_PATH while inside a dungeon.
 *   2) Addon sends WPSP "RECORD_PATH_TOGGLE".
 *   3) Server flips the player to GM-mode + 5x speed, starts sampling
 *      their position every ~1500 ms into an in-memory buffer.
 *   4) User flies through the layout, untargetable by hostiles.
 *   5) User toggles again. Server stops sampling, dedupes points
 *      closer than 2y to the previous, persists to the table, drops
 *      GM-mode and resets speed.
 *
 * Playback (PartyFollow already calls into this every second):
 *   - When the assigned tank is in a dungeon with a stored path for the wing the
 *     leader is in AND the leader is not currently fighting, the tank walks a
 *     lookahead point LEAD_DISTANCE further ALONG that path than the leader's
 *     nearest waypoint (the cursor).
 *   - The wing is chosen by proximity (SelectPathForLeader); if no recorded path
 *     is near the leader, the tank just MoveFollows instead of leading.
 *   - 45-yard leash from the leader — if exceeded, tank stops and waits.
 *   - In combat the tank engages normally (AssistTarget / rotation rules).
 */

#ifndef WOWPSPARTY_PARTYPATH_H
#define WOWPSPARTY_PARTYPATH_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>

class Player;

namespace WowPsParty
{
    // Toggle recording for this player. Returns true if recording is now
    // ON, false if it was just turned off (and persisted).
    bool TogglePathRecording(Player* player);

    // Returns true if the given player is currently recording a path.
    bool IsRecordingPath(ObjectGuid playerGuid);

    // Abort an in-progress recording without persisting it, resetting the
    // player's movement state (ghost mode off). Call on logout so a player
    // who logs out mid-record doesn't relog stuck flying.
    void CancelPathRecording(Player* player);

    // Login self-heal: strip a stuck ghost-mode "Transparency" aura (37800) from a
    // player who isn't recording (logged out / crashed mid-record). No-op otherwise.
    void ClearStuckGhostMode(Player* player);

    // Sample-tick hook called from PartyFollow's 1Hz ticker for every
    // recording player. Handles GM-mode reapplication, speed, and the
    // position-sample buffer.
    void TickPathRecording(uint32 diffMs);

    // Drop the stored path for the WING the clearer is standing in (the recording
    // nearest them), leaving the other wings of a shared-map dungeon intact.
    // Returns waypoints removed (0 = nothing recorded near them).
    uint32 ClearPath(uint32 mapId, Player* clearer);

    // Tank-side playback. Called each AI tick (from the patched
    // PlayerbotAI::UpdateAI). Walks the assigned tank toward the
    // next path waypoint when not engaged.
    void TankFollowPath(Player* bot);

    // Total stored waypoints for the map (all wings). Coarse "anything recorded
    // here" gate + the addon's "Deadmines — 47 waypoints" status display.
    uint32 GetPathWaypointCount(uint32 mapId);

    // True if a recorded path exists for the WING the leader is in (proximity, the
    // same test playback uses) — so the lead-tank gate matches TankFollowPath.
    bool HasPathForLeader(uint32 mapId, Player* leader);

    // True if the tank has ANY route to drive here — a recorded wing route, or an
    // auto route to a boss that is still standing. This is the gate PartyFollow's
    // follow ticker uses to yield the lead tank's feet, so it must stay cheap: it
    // reads cached state only and never builds a route.
    bool TankRouteAvailable(uint32 mapId, Player* leader);

    // One-line summary of what the tank will do in this dungeon, for the message the
    // party leader gets on entering (PartyHooks::OnPlayerMapChanged). Names the
    // recorded route, or the auto route's next boss, or says why there's neither.
    std::string DescribeTankRoute(Player* leader);
}

#endif
