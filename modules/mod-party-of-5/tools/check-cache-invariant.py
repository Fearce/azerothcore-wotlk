#!/usr/bin/env python3
"""Static guard for the party-member loadout-cache invariant.

Why this exists: a party member's rotation and behaviour toggles PERSIST in
`party_loadout`, but combat and the follow ticker read ONLY the in-memory
caches. The DB row is where they survive a restart; the cache is what actually
plays the game. Nothing in the language ties the two together, so the failure
mode is silent and total -- a member whose caches were never filled fights with
zero rules while its rules sit safely on disk, and re-saving in the editor
"fixes" it because the editor writes the cache directly.

That is exactly the 2026-07-31 bug: `PartyMgr::Enroll` (the roster invite) spawned
the hero without filling its caches, so swapping to a party you had not already
loaded this worldserver run left every hero ruleless -- including in a
battleground. It went unnoticed for months because a ruleless bot just
auto-attacks; it never errors.

The fix consolidated the eleven refreshers into `RefreshMemberLoadoutCaches` and
called it from all three paths. That fix is one comment away from rotting: the
next per-member cache added to the module, or the next place a party member is
spawned, re-opens the same hole. A comment saying "remember to add it here too"
has a compliance rate near zero across fresh agent sessions, so this is the
enforcement point instead -- run by server-ac's pre-commit hook whenever
mod-party-of-5 C++ is staged (install-cache-invariant-hook.ps1).

Two checks, both purely textual -- no DB, no build, no worldserver:

  1. COVERAGE. Every per-member `*RefreshFromDB` (one that reads `party_loadout`
     WHERE `guid`) must be called from `RefreshMemberLoadoutCaches`. Adding a
     twelfth cached column and forgetting the helper fails here.

  2. SPAWN TRIPWIRE. Every call that spawns a party member as a bot is pinned in
     KNOWN_SPAWN_SITES below, each with a note on how it fills the caches. A new
     spawn path fails the check until someone states which of the two legal
     answers applies -- it loads the saved loadout, or it deliberately installs
     class defaults (the henchman rule). It cannot be added silently.

Usage:
    python check-cache-invariant.py           # check; exit 1 on violation
    python check-cache-invariant.py --list    # dump what it currently sees
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src"

# Where per-member RefreshFromDB helpers are defined.
REFRESHER_FILES = ["PartyRotation.cpp", "PartyFollow.cpp"]

# The single helper every per-member refresher must be reachable from.
AGGREGATOR_FILE = "PartyMgr.cpp"
AGGREGATOR = "RefreshMemberLoadoutCaches"

# Refreshers that read party_loadout but are NOT per-member state, so they do not
# belong in the aggregator. Keep this list short and justified.
NOT_PER_MEMBER = {
    # role is resolved on demand from the follow directive, never cached per member.
    "RoleRefreshFromDB",
}

# Every call that spawns a PARTY MEMBER (master = the player's account). Keyed by
# file -> {normalized call text: why it is safe}. Spawns with master 0 are
# mod-playerbots random/BG fills, not party members, and are ignored.
KNOWN_SPAWN_SITES = {
    "PartyMgr.cpp": {
        "mgr->AddPlayerBot(henchGuid, account);":
            "HireHenchman - deliberately installs the class DEFAULT rotation "
            "(henchmen never keep a saved one) and loads the toggles by hand.",
        "mgr->AddPlayerBot(altObjGuid, account);":
            "HireAlt - calls RefreshMemberLoadoutCaches(altGuid) first.",
        "botMgr->AddPlayerBot(og, account);":
            "OnActiveLogin spawn loop - calls RefreshMemberLoadoutCaches(guid) first.",
    },
    "PartyAddonProtocol.cpp": {
        "mgr->AddPlayerBot(":
            "MGMT_INVITE - PartyMgr::Enroll calls RefreshMemberLoadoutCaches("
            "targetGuid) before returning Ok.",
    },
}

# Files whose AddPlayerBot calls are random/BG bot fills, never party members.
SPAWN_SCAN_SKIP = {"PartyBgFill.cpp", "PartyTestRunner.cpp"}


def strip_noise(text):
    """Blank out // comments, /* */ comments and string literals.

    Brace matching and call detection both misfire on a brace or a call-shaped
    token inside a comment or an SQL string, and this module is full of both.
    Replaces with spaces so every byte offset still lines up with the original.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if text[k] != "\n":
                    out[k] = " "
            i = j
        elif text[i] in "\"'":
            quote, j = text[i], i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            for k in range(i + 1, j - 1):
                if text[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def function_span(clean, name):
    """Brace-matched (start, end) of `name`'s body, or None if it has no definition.

    Returns a SPAN rather than the text because callers need both views of it:
    strip_noise blanks string bodies, so the SQL that decides whether a refresher
    is per-member only survives in the raw source -- and strip_noise preserves
    every byte offset precisely so the same span indexes both.
    """
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\([^;{)]*\)\s*\{", clean):
        start = clean.index("{", m.start())
        depth, i = 0, start
        while i < len(clean):
            if clean[i] == "{":
                depth += 1
            elif clean[i] == "}":
                depth -= 1
                if depth == 0:
                    return start + 1, i
            i += 1
    return None


def find_per_member_refreshers():
    """Every `*RefreshFromDB` whose body reads party_loadout keyed on a guid."""
    found = {}
    for fname in REFRESHER_FILES:
        raw = (SRC / fname).read_text(encoding="utf-8", errors="replace")
        clean = strip_noise(raw)
        for m in re.finditer(r"\bvoid\s+(\w+RefreshFromDB)\s*\(\s*uint32\s+\w+\s*\)\s*\{",
                             clean):
            name = m.group(1)
            if name in NOT_PER_MEMBER:
                continue
            span = function_span(clean, name)
            if span is None:
                continue
            # The SQL only survives in the raw text; an account-scoped refresher
            # (the COMMON rotation, account settings) reads a different table or
            # keys on `account`, and must not be dragged into the per-member helper.
            sql = raw[span[0]:span[1]]
            if "party_loadout" in sql and re.search(r"`guid`\s*=", sql):
                found[name] = fname
    return found


def find_spawn_sites():
    """Party-member AddPlayerBot calls, as file -> {normalized call: [lines]}."""
    sites = {}
    for path in sorted(SRC.glob("*.cpp")):
        if path.name in SPAWN_SCAN_SKIP:
            continue
        raw = path.read_text(encoding="utf-8", errors="replace").splitlines()
        clean = strip_noise("\n".join(raw)).splitlines()
        for lineno, line in enumerate(clean, 1):
            if "AddPlayerBot(" not in line:
                continue
            call = line.strip()
            # master 0 == mod-playerbots random/BG fill, not one of our members.
            if re.search(r"AddPlayerBot\([^)]*,\s*0\s*\)", call):
                continue
            sites.setdefault(path.name, {}).setdefault(call, []).append(lineno)
    return sites


def check_coverage(problems):
    refreshers = find_per_member_refreshers()
    clean = strip_noise((SRC / AGGREGATOR_FILE).read_text(encoding="utf-8",
                                                          errors="replace"))
    span = function_span(clean, AGGREGATOR)
    if span is None:
        problems.append(
            f"{AGGREGATOR_FILE}: {AGGREGATOR}() is gone. Every path that brings a "
            f"saved member into the party relies on it; restore it or update this "
            f"check to name its replacement.")
        return refreshers, set()

    body = clean[span[0]:span[1]]
    called = {n for n in refreshers if re.search(r"\b" + n + r"\s*\(", body)}
    for name in sorted(set(refreshers) - called):
        problems.append(
            f"{refreshers[name]}: {name}() reads party_loadout per member but is "
            f"NOT called from {AGGREGATOR}(). A member invited or hired mid-session "
            f"would run with that cache unset. Add `{name}(guidLow);` to "
            f"{AGGREGATOR_FILE}::{AGGREGATOR}, or list it in NOT_PER_MEMBER here "
            f"with a reason.")
    return refreshers, called


def check_spawn_sites(problems):
    sites = find_spawn_sites()
    for fname, calls in sorted(sites.items()):
        known = KNOWN_SPAWN_SITES.get(fname, {})
        for call, linenos in sorted(calls.items()):
            match = next((k for k in known if call.startswith(k)), None)
            if match is None:
                problems.append(
                    f"{fname}:{linenos[0]}: new party-member spawn `{call}` is not "
                    f"in KNOWN_SPAWN_SITES. Before pinning it, make it either call "
                    f"{AGGREGATOR}() (a hero keeps its saved loadout) or install "
                    f"class defaults on purpose (the henchman rule); a spawn that "
                    f"does neither fights with no rules at all.")
            elif len(linenos) > 1:
                problems.append(
                    f"{fname}: `{call}` now appears {len(linenos)} times "
                    f"(lines {', '.join(map(str, linenos))}). One pinned entry can "
                    f"only vouch for one call site; give the new one a distinct "
                    f"variable name and pin it separately.")
    for fname, known in KNOWN_SPAWN_SITES.items():
        for call in known:
            if not any(c.startswith(call) for c in sites.get(fname, {})):
                problems.append(
                    f"{fname}: pinned spawn site `{call}` no longer exists. Drop it "
                    f"from KNOWN_SPAWN_SITES so the tripwire keeps matching reality.")
    return sites


def main():
    if not SRC.is_dir():
        print(f"check-cache-invariant: no source dir at {SRC}", file=sys.stderr)
        return 2

    problems = []
    refreshers, called = check_coverage(problems)
    sites = check_spawn_sites(problems)

    if "--list" in sys.argv:
        print(f"per-member refreshers ({len(refreshers)}):")
        for name in sorted(refreshers):
            mark = "ok " if name in called else "MISSING"
            print(f"  [{mark}] {name}  ({refreshers[name]})")
        print("\nparty-member spawn sites:")
        for fname, calls in sorted(sites.items()):
            for call, linenos in sorted(calls.items()):
                print(f"  {fname}:{linenos[0]}  {call}")

    if problems:
        print("check-cache-invariant: loadout-cache invariant broken\n")
        for p in problems:
            print(f"  * {p}\n")
        print("Background: modules/mod-party-of-5/tools/check-cache-invariant.py")
        return 1

    print(f"check-cache-invariant: ok - {len(refreshers)} per-member cache(s) all "
          f"loaded by {AGGREGATOR}, "
          f"{sum(len(c) for c in sites.values())} spawn site(s) accounted for.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
