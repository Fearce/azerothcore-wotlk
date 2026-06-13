/*
 * WowPs Party-of-5 — Dungeon path recording + tank playback.
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
}

#endif
