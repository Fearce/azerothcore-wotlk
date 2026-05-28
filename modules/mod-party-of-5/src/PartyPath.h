/*
 * WowPs Party-of-5 — Dungeon path recording + tank playback.
 *
 * Idea: walk through each dungeon once in a GM-mode/superspeed ghost while
 * the server samples your position. Subsequent runs, the assigned tank
 * walks that path automatically — so they actually navigate the corridor
 * the way you taught them, not just stand 12y in front of you.
 *
 * Storage:  dungeon_path(map_id, sequence, x, y, z, orientation)
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
 *   - When the assigned tank is in a dungeon with a stored path AND the
 *     leader is not currently fighting, the tank walks toward the
 *     "leader-cursor + 3" waypoint. Leader cursor = nearest waypoint to
 *     the leader's current position.
 *   - 30-yard leash from the leader — if exceeded, tank stops and waits.
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

    // Sample-tick hook called from PartyFollow's 1Hz ticker for every
    // recording player. Handles GM-mode reapplication, speed, and the
    // position-sample buffer.
    void TickPathRecording(uint32 diffMs);

    // Drop all stored waypoints for the given map. Used by the addon's
    // "Clear path" action when the user wants to re-record from scratch.
    void ClearPath(uint32 mapId);

    // Tank-side playback. Called each AI tick (from the patched
    // PlayerbotAI::UpdateAI). Walks the assigned tank toward the
    // next path waypoint when not engaged.
    void TankFollowPath(Player* bot);

    // Count of stored waypoints for the given map. Used by the addon
    // to show "Deadmines — 47 waypoints" status.
    uint32 GetPathWaypointCount(uint32 mapId);
}

#endif
