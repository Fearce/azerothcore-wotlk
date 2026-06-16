/*
 * WowPs Party-of-5 mod — script loader
 *
 * AzerothCore globs modules/<name>/src/*.cpp and links them into worldserver.
 * Each module exposes one Add<folder_with_underscores>Scripts() function that
 * fans out to the module's individual AddXxxScripts() entry points.
 *
 * Folder name `mod-party-of-5` → function name `Addmod_party_of_5Scripts`.
 */

void AddPartyBootstrapScripts();
void AddPartyCommandScripts();
void AddPartyHooksScripts();
void AddPartyAddonProtocolScripts();
void AddPartyFollowScripts();
void AddPartyLfgFillScripts();
void AddPartyBgFillScripts();

void Addmod_party_of_5Scripts()
{
    AddPartyBootstrapScripts();
    AddPartyCommandScripts();
    AddPartyHooksScripts();
    AddPartyAddonProtocolScripts();
    AddPartyFollowScripts();
    AddPartyLfgFillScripts();
    AddPartyBgFillScripts();
}
