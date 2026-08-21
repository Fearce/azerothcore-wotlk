/*
 * WowPs Party-of-5 mod — addon protocol over LANG_ADDON whispers
 *
 * Client side (Interface/AddOns/WowPsParty/WowPsParty.lua) sends WPSP messages
 * to itself via SendAddonMessage("WPSP", body, "WHISPER", UnitName("player")).
 * AC delivers those to OnPlayerBeforeSendChatMessage with lang=LANG_ADDON, so
 * we intercept here before they hit the chat dispatcher.
 *
 * Server → client messages are SMSG_MESSAGECHAT with CHAT_MSG_WHISPER/LANG_ADDON
 * carrying "WPSP\tCOMMAND\tPAYLOAD". The client's addon comm layer parses the
 * first tab-separated token as the prefix.
 *
 * Wire protocol (matches the client addon):
 *   Client → server:  REQ_ROSTER  |  PING  |  (future) LOADOUT, STRAT
 *   Server → client:  ROSTER\t<recordsep-pipe of records>
 *                     SWAPPED\t<oldSlot>\t<newSlot>
 *                     PONG
 *   Each ROSTER record is: slot\tguid\tname\tclass\tlevel\tactive(0/1)
 *   Records are pipe-separated within the single payload string.
 */

#include "PartyMgr.h"
#include "PartyRotation.h"
#include "PartyFollow.h"
#include "PartyPath.h"

#include "Bag.h"
#include "Chat.h"
#include "CharmInfo.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "ArenaTeam.h"    // GetSlotByType / MAX_ARENA_SLOT — arena-charter eligibility
#include "Opcodes.h"      // CMSG_PETITION_SIGN / CMSG_TURN_IN_PETITION — hero charter signing
#include "PetitionMgr.h"  // sPetitionMgr / Petition / Signatures — charter detection
#include "Pet.h"
#include "World.h"        // sWorld->getIntConfig — petition-sign / max-level configs
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Spell.h"
#include "StringFormat.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "AiObjectContext.h"
#include "Value.h"
#include "AuctionHouseMgr.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Cell.h"                // grid search for the object an aimed on-use item
#include "CellImpl.h"            // (a key, a charge) should be pointed at
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LootMgr.h"   // Loot / LootItem — open lootable satchels from the shared bags
#include "Mail.h"      // MailDraft — mail a Party Inventory item to a player
#include "CharacterCache.h"   // sCharacterCache — resolve a recipient name -> guid/account
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <excpt.h>   // SEH (__try/__except) guard for the item-field reads below
#endif

// mod-ah-bot-plus is a gitignored clone that may be absent on a fresh
// checkout, so the priced-to-sell AH_SELL clamp only compiles in when the
// header is present and degrades to listing at the requested price otherwise.
#include "Config.h"
#if __has_include("../../mod-ah-bot-plus/src/AuctionHouseBot.h")
#include "../../mod-ah-bot-plus/src/AuctionHouseBot.h"
#define WOWPS_HAS_AHBOT 1
#endif

// mod-transmog is likewise a gitignored clone that may be absent on a fresh
// checkout. TransmogPop's REQ_XMOG / XMOG_APPLY bridge only compiles in when the
// module's header is present; without it the opcodes reply "unavailable" so the
// tree still builds on a box that has no mod-transmog.
#if __has_include("../../mod-transmog/src/Transmogrification.h")
#include "../../mod-transmog/src/Transmogrification.h"
#define WOWPS_HAS_TRANSMOG 1
#endif

namespace WowPsParty
{
    bool IsEnabled();     // PartyBootstrap.cpp
    bool IsLogVerbose();
}

static constexpr char const* WPSP_PREFIX = "WPSP\t";
static constexpr std::size_t WPSP_PREFIX_LEN = 5;

namespace
{
    static void SendWPSP(Player* target, std::string const& body)
    {
        if (!target || !target->GetSession())
            return;
        WorldPacket data;
        std::string full;
        full.reserve(WPSP_PREFIX_LEN + body.size());
        full.append("WPSP\t");
        full.append(body);
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON,
                                     target->GetGUID(), target->GetGUID(),
                                     full, /*chatTag=*/0);
        target->GetSession()->SendPacket(&data);
    }

    // Stream a rotation DSL to the client as a chunked BEGIN / CHUNK / END group so a
    // large rotation can't overflow the ~255-byte addon-message cap and arrive empty
    // (what made a fully-scripted Common list vanish from the editor). Mirrors the
    // per-mob MOB_ROT_* framing: `beginMsg`/`endMsg` carry any key/token, each CHUNK is
    // a <=200-byte raw DSL fragment the client concatenates verbatim before parsing. The
    // '|' field separator is converted to '~' for the editor's import parser first.
    static void SendChunkedRotation(Player* target, std::string const& beginMsg,
                                    char const* chunkCmd, std::string dsl,
                                    std::string const& endMsg)
    {
        std::replace(dsl.begin(), dsl.end(), '|', '~');
        SendWPSP(target, beginMsg);
        for (size_t i = 0; i < dsl.size(); i += 200)
            SendWPSP(target, std::string(chunkCmd) + "\t" + dsl.substr(i, 200));
        SendWPSP(target, endMsg);
    }

    // --- TransmogPop bridge (Path B) --------------------------------------
    // The transmog NPC's own gossip/vendor flow can't drive a multi-slot custom
    // window (the client drops the gossip the moment the fake vendor opens), so
    // TransmogPop talks to us instead: REQ_XMOG streams the account's COLLECTED
    // appearances that are valid for each equipped slot; XMOG_APPLY commits one
    // pick through mod-transmog. We never involve the NPC.

    // Slots mod-transmog can reskin, ascending EQUIPMENT_SLOT_* (server, 0-based;
    // the addon maps these back to its rail as invSlot = serverSlot + 1).
    static uint8 const kXmogSlots[] = {
        EQUIPMENT_SLOT_HEAD, EQUIPMENT_SLOT_SHOULDERS, EQUIPMENT_SLOT_BODY,
        EQUIPMENT_SLOT_CHEST, EQUIPMENT_SLOT_WAIST, EQUIPMENT_SLOT_LEGS,
        EQUIPMENT_SLOT_FEET, EQUIPMENT_SLOT_WRISTS, EQUIPMENT_SLOT_HANDS,
        EQUIPMENT_SLOT_BACK, EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND,
        EQUIPMENT_SLOT_RANGED, EQUIPMENT_SLOT_TABARD
    };

    static void SendXmogCollectionTo(Player* player)
    {
        if (!player || !player->GetSession())
            return;
#ifndef WOWPS_HAS_TRANSMOG
        SendWPSP(player, "XMOG_UNAVAIL\tmodule");
#else
        if (!sTransmogrification->GetUseCollectionSystem())
        {
            SendWPSP(player, "XMOG_UNAVAIL\tcollection");
            return;
        }

        uint32 const accountId = player->GetSession()->GetAccountId();
        auto const it = sTransmogrification->collectionCache.find(accountId);

        // Which transmoggable slots the player actually has an item equipped in
        // (so the addon shows a slot even when it has no collected appearance).
        std::string slotsCsv;
        for (uint8 slot : kXmogSlots)
        {
            Item* equipped = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (equipped && equipped->GetTemplate())
            {
                if (!slotsCsv.empty())
                    slotsCsv += ',';
                slotsCsv += std::to_string(uint32(slot));
            }
        }

        SendWPSP(player, "XMOG_BEGIN");
        SendWPSP(player, "XMOG_SLOTS\t" + slotsCsv);

        std::string chunk;
        chunk.reserve(240);
        auto flush = [&]()
        {
            if (!chunk.empty())
            {
                SendWPSP(player, "XMOG\t" + chunk);
                chunk.clear();
            }
        };

        if (it != sTransmogrification->collectionCache.end())
        {
            for (uint8 slot : kXmogSlots)
            {
                Item* equipped = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!equipped)
                    continue;
                ItemTemplate const* dst = equipped->GetTemplate();
                if (!dst)
                    continue;
                for (uint32 itemId : it->second)
                {
                    ItemTemplate const* src = sObjectMgr->GetItemTemplate(itemId);
                    if (!src)
                        continue;
                    if (!sTransmogrification->SuitableForTransmogrification(player, src))
                        continue;
                    if (!sTransmogrification->CanTransmogrifyItemWithItem(player, dst, src))
                        continue;
                    std::string rec = std::to_string(uint32(slot)) + ":" + std::to_string(itemId);
                    if (!chunk.empty() && chunk.size() + 1 + rec.size() > 220)
                        flush();
                    if (!chunk.empty())
                        chunk += ';';
                    chunk += rec;
                }
            }
        }
        flush();
        SendWPSP(player, "XMOG_END");
#endif
    }

    // payload: "<serverSlot> <itemId>"  (itemId 0 == remove the slot's transmog).
    static void HandleXmogApply(Player* player, std::string_view payload)
    {
        if (!player || !player->GetSession())
            return;
#ifndef WOWPS_HAS_TRANSMOG
        SendWPSP(player, "XMOG_RESULT\t255\t0");
#else
        uint32 slot = 255, itemId = 0;
        {
            std::string p(payload);
            std::istringstream ss(p);
            ss >> slot >> itemId;
        }

        std::string res = "0";
        if (slot < EQUIPMENT_SLOT_END)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(slot)))
            {
                if (itemId == 0)
                {
                    sTransmogrification->DeleteFakeEntry(player, uint8(slot), item);
                    res = "1";
                }
                else
                {
                    // The uint32-entry Transmogrify overload does NOT verify the
                    // appearance is collected, so gate it here before applying.
                    uint32 const accountId = player->GetSession()->GetAccountId();
                    auto const it = sTransmogrification->collectionCache.find(accountId);
                    bool const collected = it != sTransmogrification->collectionCache.end()
                                        && it->second.count(itemId) > 0;
                    if (collected
                        && sTransmogrification->Transmogrify(player, itemId, uint8(slot), /*no_cost=*/false) == LANG_TRANSMOG_OK)
                        res = "1";
                }
            }
        }
        SendWPSP(player, "XMOG_RESULT\t" + std::to_string(slot) + "\t" + res);
#endif
    }
}

namespace WowPsParty
{
    void NotifyPartyInventoryChanged(Player* changed)
    {
        if (!changed)
            return;

        Player* recipient = changed;
        if (ObjectGuid const leaderGuid = WowPsParty::GetLeaderFor(changed->GetGUID()))
            if (Player* leader = ObjectAccessor::FindConnectedPlayer(leaderGuid))
                recipient = leader;

        if (!recipient || !recipient->GetSession())
            return;

        SendWPSP(recipient, "INV_DIRTY");
    }
}

namespace
{
    // Build + send the roster the party-management panel renders: one record per
    // character on the account, "<guid>:<name>:<race>:<class>:<level>:<slot|-1>:<role>".
    // Role precedence: account_party.role (enrolled) → party_loadout.role (solo) → dps.
    static void SendMgmtList(Player* player)
    {
        if (!player || !player->GetSession())
            return;
        uint32 const accountId = player->GetSession()->GetAccountId();
        QueryResult q = CharacterDatabase.Query(
            "SELECT c.guid, c.name, c.race, c.class, c.level, "
            "       COALESCE(ap.slot, 255) AS slot, "
            "       COALESCE(ap.role, NULLIF(pl.role, ''), 'dps') AS role "
            "FROM `characters` c "
            "LEFT JOIN `account_party` ap ON ap.guid = c.guid AND ap.account = c.account "
            "LEFT JOIN `party_loadout` pl ON pl.guid = c.guid "
            "WHERE c.account = {} AND (c.deleteInfos_Account IS NULL OR c.deleteInfos_Account = 0) "
            // The player's hand-sorted roster order wins; characters not yet placed
            // (roster_order NULL) fall to the bottom in the legacy enrolled-first /
            // name order, which also seeds the very first sort before any move.
            // KEEP THIS SORT IDENTICAL to the MGMT_MOVE read, or arrows swap the wrong row.
            "ORDER BY (c.roster_order IS NULL) ASC, c.roster_order ASC, "
            "         COALESCE(ap.slot, 255) ASC, c.name ASC",
            accountId);

        std::ostringstream out;
        out << "MGMT_LIST\t";
        bool first = true;
        if (q)
        {
            do
            {
                Field* f = q->Fetch();
                uint32 const g = f[0].Get<uint32>();
                std::string nm = f[1].Get<std::string>();
                uint32 const race = f[2].Get<uint8>();
                uint32 const cls  = f[3].Get<uint8>();
                uint32 const lvl  = f[4].Get<uint8>();
                uint8 const slot  = f[5].Get<uint8>();
                std::string role  = f[6].Get<std::string>();
                int const slotOut = (slot == 255) ? -1 : int(slot);

                if (!first) out << ';';
                first = false;
                out << g << ':' << nm << ':' << race << ':' << cls << ':'
                    << lvl << ':' << slotOut << ':' << role;
            } while (q->NextRow());
        }
        SendWPSP(player, out.str());
    }

    static std::string BuildRosterPayload(uint32 accountId)
    {
        auto const party = sPartyMgr.GetParty(accountId);
        std::ostringstream out;
        bool first = true;
        for (auto const& m : party)
        {
            if (!first) out << '|';
            first = false;
            out << uint32(m.slot) << '\t'
                << m.guid          << '\t'
                << m.name          << '\t'
                << uint32(m.classId) << '\t'
                << uint32(m.level) << '\t'
                << (m.online ? '1' : '0');
        }
        return out.str();
    }
}

namespace WowPsParty
{
    void SendRosterTo(Player* player)
    {
        if (!player || !player->GetSession())
            return;
        uint32 const accountId = player->GetSession()->GetAccountId();
        std::string const payload = BuildRosterPayload(accountId);
        SendWPSP(player, "ROSTER\t" + payload);

        // The account's henchman guids, so the client can distinguish a managed
        // henchman from a SECOND HUMAN in the group (without this the addon tags
        // the other player as a henchman — shows them in the rotation editor).
        std::vector<uint32> hench;
        WowPsParty::GetHenchmanGuidsForAccount(accountId, hench);
        std::ostringstream hp;
        for (size_t i = 0; i < hench.size(); ++i) { if (i) hp << ':'; hp << hench[i]; }
        SendWPSP(player, "HENCHGUIDS\t" + hp.str());
    }

    // SETTINGS\t<spawn>\t<inv>\t<gear>\t<prog>  (each 0/1) — the account's
    // solo/Po5 feature toggles, so the client panel reflects them and the
    // client-side UI hijacks (B/C) honour them.
    void SendSettingsTo(Player* player)
    {
        if (!player || !player->GetSession()) return;
        PartySettings const s = GetAccountSettings(player->GetSession()->GetAccountId());
        std::ostringstream out;
        out << "SETTINGS\t" << (s.spawnCompanions ? 1 : 0)
            << '\t' << (s.sharedInventory ? 1 : 0)
            << '\t' << (s.sharedGear ? 1 : 0)
            << '\t' << (s.sharedProgression ? 1 : 0)
            << '\t' << s.questXpRate
            << '\t' << s.killXpRate;
        SendWPSP(player, out.str());
    }

    // BL_BEGIN / BL\t<entry>:<quality>:<name>\t... / BL_END — the account's loot
    // blacklist, streamed so a long list can't overflow one addon message and arrive
    // truncated. The NAME comes from the server rather than the client's GetItemInfo
    // because the panel must be able to show an item this client has never seen.
    void SendLootBlacklistTo(Player* player)
    {
        if (!player || !player->GetSession()) return;
        std::vector<uint32> const entries =
            GetLootBlacklist(player->GetSession()->GetAccountId());

        SendWPSP(player, "BL_BEGIN");

        std::string chunk;
        chunk.reserve(240);
        auto flush = [&]()
        {
            if (chunk.empty()) return;
            SendWPSP(player, "BL\t" + chunk);
            chunk.clear();
        };
        for (uint32 entry : entries)
        {
            ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(entry);
            // A blacklisted entry whose item no longer exists in the DB still has to
            // be listed, or the player can never remove it.
            std::string rec = std::to_string(entry) + ':'
                            + std::to_string(tmpl ? uint32(tmpl->Quality) : 1u) + ':'
                            + (tmpl ? tmpl->Name1 : "item " + std::to_string(entry));
            if (chunk.size() + rec.size() + 1 > 240)
                flush();
            if (!chunk.empty()) chunk += '\t';
            chunk += rec;
        }
        flush();

        SendWPSP(player, "BL_END");
    }

    // Resolve what the player typed into an item entry. The addon already turns a
    // shift-clicked link into its numeric id, so this is either that id or a name
    // typed by hand. Names match case-insensitively and exactly — a substring match
    // would silently blacklist "Pattern: Bag of Many Hides" when asked for "Bag".
    //
    // 2040 lowercased names in item_template are shared by more than one entry
    // ("Bloodstained Fortune" by 20 of them), so `shared` reports how many matched
    // and the LOWEST entry is taken: unordered_map iteration order is not stable, so
    // returning the first hit would blacklist a different item run to run. Returns 0
    // for no match.
    uint32 ResolveItemEntryForBlacklist(std::string const& text, uint32* shared = nullptr)
    {
        if (shared) *shared = 0;
        if (text.empty()) return 0;

        if (text.find_first_not_of("0123456789") == std::string::npos)
        {
            uint32 const entry = uint32(std::strtoul(text.c_str(), nullptr, 10));
            return sObjectMgr->GetItemTemplate(entry) ? entry : 0;
        }

        auto lower = [](unsigned char c) { return char(std::tolower(c)); };
        std::string wanted = text;
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), lower);

        uint32 best = 0, matches = 0;
        for (auto const& [entry, tmpl] : *sObjectMgr->GetItemTemplateStore())
        {
            // Length first: the store holds ~50k rows and this runs on the map thread
            // that handled the click, so the common case must not allocate.
            if (tmpl.Name1.size() != wanted.size()) continue;
            if (!std::equal(wanted.begin(), wanted.end(), tmpl.Name1.begin(),
                            [&lower](char a, char b) { return a == lower(b); }))
                continue;
            ++matches;
            if (!best || entry < best) best = entry;
        }
        if (shared) *shared = matches;
        return best;
    }

    // HENCHMEN\t<rec>;<rec>;...   rec = guid:name:cls:level:role:cost:spec
    // The hireable random-pool candidates near the player's level. (spec is a short
    // abbrev, possibly empty for a talentless low-level char.)
    void SendHenchmenTo(Player* player)
    {
        if (!player) return;
        auto const cands = BuildHenchmanCandidates(player);
        std::ostringstream out;
        out << "HENCHMEN\t";
        bool first = true;
        for (auto const& c : cands)
        {
            if (!first) out << ';';
            first = false;
            out << c.guid << ':' << c.name << ':' << uint32(c.cls) << ':'
                << uint32(c.level) << ':' << c.role << ':'
                << HenchmanHireCost(c.level) << ':' << c.spec;
        }
        std::string const body = out.str();
        LOG_INFO("module",
            "[WowPsParty Henchmen] SendHenchmenTo guid={} level={} team={} "
            "candidates={} payload_bytes={}",
            player->GetGUID().GetCounter(), uint32(player->GetLevel()),
            uint32(player->GetTeamId()), uint32(cands.size()), uint32(body.size()));
        SendWPSP(player, body);
    }

    // ALTS\t<rec>;<rec>;...   rec = guid:name:cls:level:role:spec:hired
    // The player's own non-enrolled characters for the Hire-Alts window: an offline
    // row (hired=0) gets a Hire button, a currently-hired row (hired=1) a Dismiss.
    void SendAltsTo(Player* player)
    {
        if (!player) return;
        auto const cands = BuildAltCandidates(player);
        std::ostringstream out;
        out << "ALTS\t";
        bool first = true;
        for (auto const& c : cands)
        {
            if (!first) out << ';';
            first = false;
            out << c.guid << ':' << c.name << ':' << uint32(c.cls) << ':'
                << uint32(c.level) << ':' << c.role << ':' << c.spec << ':'
                << (c.hired ? 1 : 0);
        }
        std::string const body = out.str();
        LOG_INFO("module",
            "[WowPsParty Alts] SendAltsTo guid={} candidates={} payload_bytes={}",
            player->GetGUID().GetCounter(), uint32(cands.size()), uint32(body.size()));
        SendWPSP(player, body);
    }


    // Resolve the slot's character guid for the account.
    static uint32 GuidForAccountSlot(uint32 account, uint32 slot)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}",
            account, slot);
        return q ? q->Fetch()[0].Get<uint32>() : 0;
    }

    // Resolve a rotation-editor address token to a character guid.
    //   "h<guidLow>"  -> a henchman (validated: must be a henchman currently
    //                    led by this player, so one account can't poke another's
    //                    bot loadout by guessing guids).
    //   "<n>"         -> account_party slot n (the heroes), as before.
    // Returns 0 when the token doesn't resolve to something this player may edit.
    static uint32 ResolveLoadoutToken(Player* player, std::string const& token)
    {
        if (!player || !player->GetSession() || token.empty()) return 0;
        if (token[0] == 'h' || token[0] == 'H')
        {
            uint32 const g = std::strtoul(token.c_str() + 1, nullptr, 10);
            if (!g) return 0;
            ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(g);
            if (WowPsParty::IsHenchman(og) &&
                WowPsParty::GetLeaderFor(og) == player->GetGUID())
                return g;
            return 0;
        }
        uint32 const slot = std::strtoul(token.c_str(), nullptr, 10);
        if (slot >= WowPsParty::PARTY_SIZE) return 0;
        return GuidForAccountSlot(player->GetSession()->GetAccountId(), slot);
    }

    // Pull the first tab-separated field (the address token) off a payload,
    // leaving `rest` pointing at whatever follows the tab (empty if none).
    static std::string SplitToken(std::string const& payload, std::string& rest)
    {
        auto t = payload.find('\t');
        if (t == std::string::npos) { rest.clear(); return payload; }
        rest = payload.substr(t + 1);
        return payload.substr(0, t);
    }

    static Player* ResolveControlledBody(Player* session)
    {
        if (!session) return nullptr;
        if (Unit* charm = session->GetCharm())
            if (charm->IsPlayer())
                return charm->ToPlayer();
        return session;  // not possessing anyone — your own body
    }

    // SPELLBOOK\t<echo>\t<spellId1,spellId2,...>
    // `echo` is the address token the client sent (a slot number for heroes, or
    // "h<guid>" for henchmen) so the client routes the reply to the right tab.
    static void SendSpellbookForGuid(Player* requester, uint32 guid,
                                     std::string const& echo)
    {
        if (!requester || !requester->GetSession() || !guid) return;
        ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
        Player* target = ObjectAccessor::FindConnectedPlayer(og);
        if (!target) return;

        // Dedupe by spell chain — emit only the HIGHEST-rank spell in each
        // rank chain so the user doesn't see "Lifeblood 1/2/3/4" as four
        // separate entries. Also drop passives, profession actives, and
        // hidden-aura spells (those are flagged DO_NOT_DISPLAY).
        std::unordered_map<uint32, uint32> firstToBest;  // first-in-chain → best spellId
        for (auto const& kv : target->GetSpellMap())
        {
            if (kv.second->State == PLAYERSPELL_REMOVED) continue;
            // Skip spells not in the active spec (specMask=0) — same rule as
            // Player::HasSpell. Talent abilities the bot no longer has the talent
            // for linger in the map with specMask=0 (removeSpell won't delete
            // talent-cost spells); without this they still showed in the editor.
            if (!kv.second->IsInSpec(target->GetActiveSpec())) continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(kv.first);
            if (!info) continue;
            if (info->IsPassive()) continue;
            if (info->HasAttribute(SPELL_ATTR0_IS_TRADESKILL)) continue;
            if (info->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY)) continue;

            uint32 const first = sSpellMgr->GetFirstSpellInChain(kv.first);
            uint32 const chainKey = first ? first : kv.first;
            auto it = firstToBest.find(chainKey);
            if (it == firstToBest.end())
            {
                firstToBest[chainKey] = kv.first;
            }
            else
            {
                SpellInfo const* existing = sSpellMgr->GetSpellInfo(it->second);
                if (existing && info->SpellLevel > existing->SpellLevel)
                    it->second = kv.first;
            }
        }
        std::ostringstream csv;
        bool first = true;
        for (auto const& kv : firstToBest)
        {
            if (!first) csv << ',';
            first = false;
            csv << kv.second;
        }
        std::ostringstream out;
        out << "SPELLBOOK\t" << echo << '\t' << csv.str();
        SendWPSP(requester, out.str());
    }

    // Slot-addressed spellbook (heroes). Echoes the slot number.
    void SendSpellbookTo(Player* requester, uint32 slot)
    {
        if (!requester || !requester->GetSession()) return;
        uint32 const guid = GuidForAccountSlot(
            requester->GetSession()->GetAccountId(), slot);
        SendSpellbookForGuid(requester, guid, std::to_string(slot));
    }

    // forward decls — definitions follow below
    void SendGearTo(Player* requester, uint32 slot);
    void SendStatsTo(Player* requester, uint32 slot);
    void SendCurrencyTo(Player* requester, std::string const& token);
    void SendInventoryTo(Player* requester);
    void SendQuestProgressTo(Player* requester);

    // Push spellbook + gear + inventory for the active body on login. The
    // spellbook feeds the rotation editor's spell picker.
    void PushControlledLoadoutTo(Player* requester, int slot)
    {
        if (!requester || slot < 0) return;
        SendSpellbookTo(requester, uint32(slot));
        SendGearTo(requester, uint32(slot));
        SendStatsTo(requester, uint32(slot));
        SendInventoryTo(requester);
    }

    // Display fields read off an Item for the inventory/gear panels.
    struct PartyItemFields { uint32 entry; uint32 count; uint32 guidLow; int32 randProp; uint32 suffix; uint32 enchant; uint32 gem[MAX_GEM_SOCKETS]; bool soulbound; };

    // Defensive read of an item's display fields. A bag/equip slot can transiently
    // hold an Item whose value-array is invalid — a partially-initialised / dangling
    // item in the update queue (the recurring use-after-free, see the wowps-
    // saveinventory crash). Reading its fields AVs, and that took the WHOLE world
    // down (2026-06-20: a player banked Frozen Orbs, the follow-up REQ_INVENTORY scan
    // dereferenced a null value-array in Object::GetUInt32Value). The panel read must
    // never be a server SPOF, so SEH-guard the field reads: a single bad slot is
    // skipped + logged and the scan continues. POD-only __try body (no C++ object
    // needs unwinding) so this is well-formed under /EHsc. Returns false on an AV.
    static bool SafeReadItemFields(Item* item, PartyItemFields& out)
    {
#ifdef _WIN32
        __try
        {
#endif
            out.entry    = item->GetEntry();
            out.count    = item->GetCount();
            out.guidLow  = item->GetGUID().GetCounter();
            out.randProp = item->GetItemRandomPropertyId();
            out.suffix   = item->GetItemSuffixFactor();
            out.enchant  = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);   // perm enchant, for the tooltip
            for (uint32 g = 0; g < MAX_GEM_SOCKETS; ++g)                    // socket gem enchant ids, so the tooltip shows socketed gems (value-array reads → inside the SEH guard)
                out.gem[g] = item->GetEnchantmentId(EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + g));
            out.soulbound = item->IsSoulBound();                            // instance bind state (value-array read → inside the SEH guard)
            return true;
#ifdef _WIN32
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#endif
    }

    // True if a party member's Player/item storage is mid-teardown — not in world,
    // being removed from world, or logging out. GetItemByPos and value-array reads
    // on such a member can hand back a FREED Item* / a null value array and AV the
    // world thread (the 2026-06-20 REQ_STATS/REQ_GEAR crash on a henchman dismissed
    // after an LFG dungeon). Every addon-protocol read of a member's fields must
    // skip it; SendInventoryTo inlines the same check, gear/stats now share this.
    static bool MemberStorageUnstable(Player* p)
    {
        if (!p || !p->IsInWorld() || p->IsDuringRemoveFromWorld())
            return true;
        WorldSession* s = p->GetSession();
        return s && s->isLogingOut();
    }

    // SEH-guarded item template fetch. Item::GetTemplate() reads the item's value
    // array (GetEntry -> Object::GetUInt32Value); a dangling / half-initialised item
    // AVs there and took the world thread down through the STATS/GEAR gearscore
    // reads, which called GetTemplate() raw. Returns nullptr on an AV (same skip-the
    // -slot contract as SafeReadItemFields), so the gearscore just omits that piece.
    static ItemTemplate const* SafeItemTemplate(Item* item)
    {
        if (!item) return nullptr;
        uint32 entry = 0;
#ifdef _WIN32
        __try { entry = item->GetEntry(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
#else
        entry = item->GetEntry();
#endif
        return sObjectMgr->GetItemTemplate(entry);
    }

    // Item::IsLocked() is another value-array read (ITEM_FIELD_FLAGS), so scanning a
    // MATE's bags for an unopened lockbox needs the same guard as the panel reads
    // above — a freed Item* still sitting in a live member's slot would otherwise AV
    // the world thread. Treats an unreadable slot as "not a candidate".
    static bool SafeIsLocked(Item* item)
    {
        if (!item) return false;
#ifdef _WIN32
        __try { return item->IsLocked(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
        return item->IsLocked();
#endif
    }

    // GEAR\t<slot>\t<eqSlot>:<itemId>:<itemGuidLow>:<randProp>:<suffixFactor>:<enchant>:<gem0>:<gem1>:<gem2>;...
    // (19 equipment slots). randProp/suffixFactor appended so the gear tooltip
    // renders a randomized item (e.g. "of the Bear") with its real stats, exactly
    // like the bag inventory does — see SendInventoryTo::emitItem. enchant + the 3
    // socket gem enchant ids let the client rebuild a link that shows the applied
    // enchant and socketed gems instead of empty sockets.
    void SendGearTo(Player* requester, uint32 slot)
    {
        if (!requester || !requester->GetSession()) return;
        uint32 const account = requester->GetSession()->GetAccountId();
        uint32 const guid = GuidForAccountSlot(account, slot);
        if (!guid) return;
        ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
        Player* target = ObjectAccessor::FindConnectedPlayer(og);
        if (!target) return;
        if (MemberStorageUnstable(target))
        {
            LOG_INFO("module", "[WowPsParty] SendGearTo: skip member guid={} "
                "slot={} — storage tearing down", guid, uint32(slot));
            return;
        }

        std::ostringstream out;
        out << "GEAR\t" << slot << '\t';
        bool first = true;
        for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
        {
            Item* item = target->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
            if (!item) continue;
            PartyItemFields f;
            if (!SafeReadItemFields(item, f))
            {
                LOG_ERROR("module", "[WowPsParty] SendGearTo: SKIPPED a bad item ptr=0x{:x} owner={} "
                          "slot={} eqSlot={} (value-array AV — dangling/uninitialised item)",
                          reinterpret_cast<uintptr_t>(item), target->GetGUID().GetCounter(), slot, uint32(i));
                continue;
            }
            if (!sObjectMgr->GetItemTemplate(f.entry))
            {
                LOG_ERROR("module", "[WowPsParty] SendGearTo: SKIPPED unknown entry={} ptr=0x{:x} owner={} "
                          "slot={} eqSlot={} (freed/garbage slot read clean)",
                          f.entry, reinterpret_cast<uintptr_t>(item), target->GetGUID().GetCounter(), slot, uint32(i));
                continue;
            }
            if (!first) out << ';';
            first = false;
            out << uint32(i) << ':' << f.entry << ':' << f.guidLow
                << ':' << f.randProp << ':' << f.suffix << ':' << f.enchant
                // Socket gem enchant ids, so the gear tooltip renders socketed gems
                // (empty sockets otherwise). Appended, so an older addon that only
                // reads the first 6 fields still parses fine.
                << ':' << f.gem[0] << ':' << f.gem[1] << ':' << f.gem[2];
        }
        SendWPSP(requester, out.str());
    }

    // ------------------------------------------------------------------------
    // GearScore — canonical GearScoreLite (3x04) algorithm, mirrored server-side
    // so the number matches what players already recognise from the addon. It's
    // a pure function of item level, quality and inventory type; gems/enchants
    // don't factor in (same as the addon). See GS_Formula / GS_ItemTypes in the
    // GearScoreLite source.
    // ------------------------------------------------------------------------
    static float GS_SlotMod(uint32 invType)
    {
        switch (invType)
        {
            case INVTYPE_HEAD: case INVTYPE_CHEST: case INVTYPE_ROBE:
            case INVTYPE_LEGS: case INVTYPE_SHIELD: case INVTYPE_WEAPON:
            case INVTYPE_WEAPONMAINHAND: case INVTYPE_WEAPONOFFHAND:
            case INVTYPE_HOLDABLE:
                return 1.0000f;
            case INVTYPE_2HWEAPON:
                return 2.0000f;
            case INVTYPE_SHOULDERS: case INVTYPE_WAIST:
            case INVTYPE_FEET: case INVTYPE_HANDS:
                return 0.7500f;
            case INVTYPE_NECK: case INVTYPE_WRISTS: case INVTYPE_FINGER:
            case INVTYPE_TRINKET: case INVTYPE_CLOAK:
                return 0.5625f;
            case INVTYPE_RANGED: case INVTYPE_THROWN:
            case INVTYPE_RANGEDRIGHT: case INVTYPE_RELIC:
                return 0.3164f;
            default:
                return 0.0f;   // shirt, tabard, bags, ammo — no score
        }
    }

    // Base per-item GearScore, before the class/Titan's-Grip slot adjustments.
    static uint32 GearScoreForItem(ItemTemplate const* proto)
    {
        if (!proto) return 0;
        float const slotMod = GS_SlotMod(proto->InventoryType);
        if (slotMod <= 0.0f) return 0;

        float ilvl = float(proto->ItemLevel);
        uint32 quality = proto->Quality;
        float qualityScale = 1.0f;
        if (quality == ITEM_QUALITY_LEGENDARY) { qualityScale = 1.3f;   quality = ITEM_QUALITY_EPIC; }
        else if (quality == ITEM_QUALITY_NORMAL) { qualityScale = 0.005f; quality = ITEM_QUALITY_UNCOMMON; }
        else if (quality == ITEM_QUALITY_POOR)   { qualityScale = 0.005f; quality = ITEM_QUALITY_UNCOMMON; }
        if (quality == ITEM_QUALITY_HEIRLOOM)    { quality = ITEM_QUALITY_RARE; ilvl = 187.05f; }
        if (quality < ITEM_QUALITY_UNCOMMON || quality > ITEM_QUALITY_EPIC)
            return 0;   // artifact / unhandled

        bool const hi = ilvl > 120.0f;   // GS_Formula["A"] vs ["B"] brackets
        float A, B;
        switch (quality)
        {
            case ITEM_QUALITY_EPIC:     A = hi ? 91.4500f : 26.0000f; B = hi ? 0.6500f : 1.2000f; break;
            case ITEM_QUALITY_RARE:     A = hi ? 81.3750f :  0.7500f; B = hi ? 0.8125f : 1.8000f; break;
            case ITEM_QUALITY_UNCOMMON: A = hi ? 73.0000f :  8.0000f; B = hi ? 1.0000f : 2.0000f; break;
            default: return 0;
        }
        float const score = std::floor(((ilvl - A) / B) * slotMod * 1.8618f * qualityScale);
        return score > 0.0f ? uint32(score) : 0;
    }

    // Whole-character GearScore (sum of equipped slots, hunter + Titan's-Grip
    // weighting as the addon applies it). Skips shirt + tabard.
    static uint32 ComputeGearScore(Player* p)
    {
        if (!p) return 0;
        bool const isHunter = p->getClass() == CLASS_HUNTER;

        float titanGrip = 1.0f;   // a 2H in main OR off hand halves both weapons
        {
            Item* mh = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            Item* oh = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            ItemTemplate const* mhp = SafeItemTemplate(mh);
            ItemTemplate const* ohp = SafeItemTemplate(oh);
            if ((mhp && mhp->InventoryType == INVTYPE_2HWEAPON) ||
                (ohp && ohp->InventoryType == INVTYPE_2HWEAPON))
                titanGrip = 0.5f;
        }

        float total = 0.0f;
        for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
        {
            if (i == EQUIPMENT_SLOT_BODY || i == EQUIPMENT_SLOT_TABARD) continue;
            Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
            if (!item) continue;
            ItemTemplate const* proto = SafeItemTemplate(item);
            if (!proto) continue;
            float s = float(GearScoreForItem(proto));
            if (isHunter && (i == EQUIPMENT_SLOT_MAINHAND || i == EQUIPMENT_SLOT_OFFHAND))
                s *= 0.3164f;
            if (isHunter && i == EQUIPMENT_SLOT_RANGED)
                s *= 5.3224f;
            // NB: faithful to GearScoreLite — its TitanGrip multiply is NOT
            // class-gated, so a hunter wielding a 2H (polearm/staff stat-stick)
            // gets BOTH ×0.3164 and ×0.5 here, exactly as the addon does. Keep
            // it that way so our number matches the player's GearScore addon.
            if (i == EQUIPMENT_SLOT_MAINHAND || i == EQUIPMENT_SLOT_OFFHAND)
                s *= titanGrip;
            total += s;
        }
        return uint32(std::floor(total));
    }

    // STATS\t<slot>\t<key>:<val>;...  — GearScore, avg item level and the live
    // computed stats for the VIEWED member (the real Player object, so talents,
    // gems, enchants and buffs are all reflected — the client can't compute these
    // for anyone but itself). Rendered by CharacterSheet.lua's stat panel.
    void SendStatsTo(Player* requester, uint32 slot)
    {
        if (!requester || !requester->GetSession()) return;
        uint32 const account = requester->GetSession()->GetAccountId();
        uint32 const guid = GuidForAccountSlot(account, slot);
        if (!guid) return;
        ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
        Player* p = ObjectAccessor::FindConnectedPlayer(og);
        if (!p) p = ObjectAccessor::FindPlayer(og);
        if (!p) return;
        if (MemberStorageUnstable(p))
        {
            LOG_INFO("module", "[WowPsParty] SendStatsTo: skip member guid={} "
                "slot={} — storage tearing down", guid, uint32(slot));
            return;
        }

        // Average item level over equipped gear (excl. shirt + tabard).
        uint32 ilvlSum = 0, ilvlCount = 0;
        for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
        {
            if (i == EQUIPMENT_SLOT_BODY || i == EQUIPMENT_SLOT_TABARD) continue;
            Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
            ItemTemplate const* proto = SafeItemTemplate(item);
            if (!proto) continue;
            ilvlSum += proto->ItemLevel;
            ++ilvlCount;
        }
        uint32 const avgIlvl = ilvlCount ? (ilvlSum / ilvlCount) : 0;

        // Spell power / spell crit are reported as the highest school (matches the
        // sheet's "Spell Power" headline for a hybrid).
        int32 spellPower = 0;
        float spellCrit = 0.0f;
        for (int s = SPELL_SCHOOL_HOLY; s < MAX_SPELL_SCHOOL; ++s)
        {
            spellPower = std::max(spellPower, p->SpellBaseDamageBonusDone(SpellSchoolMask(1 << s)));
            spellCrit  = std::max(spellCrit, p->GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + s));
        }

        std::ostringstream out;
        out << "STATS\t" << slot
            << "\tgs:"  << ComputeGearScore(p)
            << ";il:"   << avgIlvl
            << ";str:"  << uint32(p->GetStat(STAT_STRENGTH))
            << ";agi:"  << uint32(p->GetStat(STAT_AGILITY))
            << ";sta:"  << uint32(p->GetStat(STAT_STAMINA))
            << ";int:"  << uint32(p->GetStat(STAT_INTELLECT))
            << ";spi:"  << uint32(p->GetStat(STAT_SPIRIT))
            << ";arm:"  << p->GetArmor()
            << ";hp:"   << p->GetMaxHealth()
            << ";man:"  << p->GetMaxPower(POWER_MANA)
            << ";ap:"   << int32(p->GetTotalAttackPowerValue(BASE_ATTACK))
            << ";rap:"  << int32(p->GetTotalAttackPowerValue(RANGED_ATTACK))
            << ";sp:"   << spellPower
            << ";exp:"  << p->GetUInt32Value(PLAYER_EXPERTISE)
            << ";def:"  << p->GetDefenseSkillValue();
        out << std::fixed << std::setprecision(1)
            << ";mcr:"  << p->GetFloatValue(PLAYER_CRIT_PERCENTAGE)
            << ";rcr:"  << p->GetFloatValue(PLAYER_RANGED_CRIT_PERCENTAGE)
            << ";scr:"  << spellCrit
            << ";mht:"  << p->GetRatingBonusValue(CR_HIT_MELEE)
            << ";sht:"  << p->GetRatingBonusValue(CR_HIT_SPELL)
            << ";mhs:"  << p->GetRatingBonusValue(CR_HASTE_MELEE)
            << ";shs:"  << p->GetRatingBonusValue(CR_HASTE_SPELL)
            << ";dge:"  << p->GetFloatValue(PLAYER_DODGE_PERCENTAGE)
            << ";par:"  << p->GetFloatValue(PLAYER_PARRY_PERCENTAGE)
            << ";blk:"  << p->GetFloatValue(PLAYER_BLOCK_PERCENTAGE)
            << ";res:"  << p->GetRatingBonusValue(CR_CRIT_TAKEN_MELEE);
        SendWPSP(requester, out.str());
    }

    static uint32 DbCurrencyItemCount(uint32 guid, uint32 itemId)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT COALESCE(SUM(ii.`count`), 0) "
            "FROM `character_inventory` ci "
            "INNER JOIN `item_instance` ii ON ii.`guid` = ci.`item` "
            "WHERE ci.`guid` = {} AND ii.`itemEntry` = {}",
            guid, itemId);
        return q ? uint32(q->Fetch()[0].Get<uint64>()) : 0;
    }

    static uint32 DbHonorPoints(uint32 guid)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `totalHonorPoints` FROM `characters` WHERE `guid` = {}", guid);
        return q ? q->Fetch()[0].Get<uint32>() : 0;
    }

    static uint32 DbArenaPoints(uint32 guid)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `arenaPoints` FROM `characters` WHERE `guid` = {}", guid);
        return q ? q->Fetch()[0].Get<uint32>() : 0;
    }

    static uint32 CurrencyCountFor(Player* p, uint32 guid, std::string const& token)
    {
        if (token == "honor")
            return p ? p->GetHonorPoints() : DbHonorPoints(guid);
        if (token == "arena")
            return p ? p->GetArenaPoints() : DbArenaPoints(guid);

        uint32 const itemId = std::strtoul(token.c_str(), nullptr, 10);
        if (!itemId)
            return 0;
        return p ? p->GetItemCount(itemId, true) : DbCurrencyItemCount(guid, itemId);
    }

    // CURRENCY_COUNTS\t<token>\t<slot>:<name>:<count>;...
    // Sent after the player clicks a row in the native Currency tab. Counts are
    // per enrolled hero and use DB fallback for offline members.
    void SendCurrencyTo(Player* requester, std::string const& token)
    {
        if (!requester || !requester->GetSession() || token.empty()) return;

        uint32 const account = requester->GetSession()->GetAccountId();
        auto const party = sPartyMgr.GetParty(account);

        std::ostringstream out;
        out << "CURRENCY_COUNTS\t" << token << '\t';
        bool first = true;
        for (auto const& m : party)
        {
            ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(m.guid);
            Player* p = ObjectAccessor::FindConnectedPlayer(og);
            if (!p) p = ObjectAccessor::FindPlayer(og);
            if (p && MemberStorageUnstable(p))
                p = nullptr;

            if (!first) out << ';';
            first = false;
            out << uint32(m.slot) << ':' << m.name << ':'
                << CurrencyCountFor(p, m.guid, token);
        }
        SendWPSP(requester, out.str());
    }

    // INVENTORY\t<partySlot>:<bag>:<pos>:<itemId>:<count>:<itemGuidLow>;...
    // Sends EVERY party member's bag contents in one message so the addon's
    // unified inventory can render the whole party in one grid. Skips equipped
    // items (those go via GEAR). Skips empty slots.
    void SendInventoryTo(Player* requester)
    {
        if (!requester || !requester->GetSession()) return;
        uint32 const account = requester->GetSession()->GetAccountId();

        std::vector<std::string> records;   // every ';'-record, streamed in chunks
        uint32 totalSlots = 0;              // party-wide bag capacity (empty cells)

        auto emitItem = [&](uint32 partySlot, uint32 bag, uint32 pos, Item* item)
        {
            PartyItemFields f;
            if (!SafeReadItemFields(item, f))
            {
                // One bad slot must not crash the whole world (the 2026-06-20 Frozen-Orb
                // bank-then-REQ_INVENTORY AV). Skip it + log so the dangling item can be
                // root-caused; the panel just omits it and the next refresh re-reads it.
                // The raw Item* is safe to print (only the value-array DEREF faults) and
                // is the stable address to correlate against the next crash dump / the
                // item-update-queue use-after-free investigation.
                LOG_ERROR("module", "[WowPsParty] SendInventoryTo: SKIPPED a bad item ptr=0x{:x} in "
                          "partySlot={} bag={} pos={} (value-array AV — dangling/uninitialised item)",
                          reinterpret_cast<uintptr_t>(item), partySlot, bag, uint32(pos));
                return;
            }
            // A freed-but-still-mapped slot can read clean garbage (no AV) — gate on a
            // real item template so a phantom record never reaches the panel.
            if (!sObjectMgr->GetItemTemplate(f.entry))
            {
                LOG_ERROR("module", "[WowPsParty] SendInventoryTo: SKIPPED unknown entry={} ptr=0x{:x} "
                          "in partySlot={} bag={} pos={} (freed/garbage slot read clean)",
                          f.entry, reinterpret_cast<uintptr_t>(item), partySlot, bag, uint32(pos));
                return;
            }
            std::ostringstream r;
            r << partySlot << ':' << bag << ':' << pos << ':'
              << f.entry << ':' << f.count << ':' << f.guidLow
              // Random property / suffix so the addon tooltip renders the FULL item
              // (e.g. a rare with "of the Bear") instead of the base item with no
              // stats. RandomPropertyId is NEGATIVE for a random SUFFIX; the suffix
              // factor (property seed) scales its stats. Then the permanent enchant id
              // so the tooltip shows the applied enchant. Appended, so an older addon
              // that only reads the first 6 fields still parses fine.
              << ':' << f.randProp
              << ':' << f.suffix
              << ':' << f.enchant
              // Instance bind state, so the tooltip can show "Soulbound" instead of
              // the item template's static "Binds when equipped". Appended last, so an
              // older addon that only reads the first 9 fields still parses fine.
              << ':' << (f.soulbound ? 1 : 0);
            records.push_back(r.str());
        };

        for (uint8 partySlot = 0; partySlot < PARTY_SIZE; ++partySlot)
        {
            uint32 const guid = GuidForAccountSlot(account, partySlot);
            if (!guid) continue;
            ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
            // Connected-player lookup first; fall back to the in-world lookup so a
            // member it misses (e.g. an odd session state after an LFG dungeon)
            // is still read instead of silently dropped.
            Player* p = ObjectAccessor::FindConnectedPlayer(og);
            if (!p) p = ObjectAccessor::FindPlayer(og);
            if (!p) continue;

            // Skip a member whose item storage is in flux — not in world, being
            // removed from world, or mid-logout. GetItemByPos on a tearing-down
            // player can hand back a FREED Item* (the slot isn't nulled until the
            // teardown finishes), and dereferencing it for GetEntry() faulted the
            // world thread: HandleSell -> SendInventoryTo -> emitItem ACCESS_VIOLATION
            // (2026-06-17, right after an LFG-dungeon henchman was dismissed). A bot
            // logged out after a dungeon is the usual trigger — its bags are unstable
            // for a tick. The "odd session state after an LFG dungeon" the lookup
            // fallback above was added for is the SAME hazard; this makes it safe.
            WorldSession* psess = p->GetSession();
            if (!p->IsInWorld() || p->IsDuringRemoveFromWorld()
                || (psess && psess->isLogingOut()))
            {
                LOG_INFO("module",
                    "[WowPsParty] SendInventoryTo: skip member guid={} slot={} — tearing down "
                    "(inWorld={} removing={} logout={})",
                    guid, uint32(partySlot), p->IsInWorld() ? 1 : 0,
                    p->IsDuringRemoveFromWorld() ? 1 : 0,
                    (psess && psess->isLogingOut()) ? 1 : 0);
                continue;
            }
            // Pins the culprit if a dangling item ever slips past the guard above:
            // this is the LAST line in the log before such a crash, naming the member.
            LOG_DEBUG("module", "[WowPsParty] SendInventoryTo: scanning member guid={} slot={}",
                      guid, uint32(partySlot));

            // Main backpack (16 slots): bag=255 (INVENTORY_SLOT_BAG_0), pos=23..38
            totalSlots += INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
            for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
                if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                    emitItem(partySlot, INVENTORY_SLOT_BAG_0, i, item);

            // The 4 equippable bag slots (19..22). Emit one BAG record per slot —
            // including empties (bagItemId 0) — so the addon shows the bag strip
            // and lets the user equip a found bag. Then the items inside each.
            for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            {
                uint32 const bagIdx = b - INVENTORY_SLOT_BAG_START;  // 0..3
                Bag* bag = p->GetBagByPos(b);
                {
                    std::ostringstream r;
                    r << "BAG:" << uint32(partySlot) << ':' << bagIdx << ':'
                      << (bag ? bag->GetEntry() : 0);
                    records.push_back(r.str());
                }
                if (!bag) continue;
                totalSlots += bag->GetBagSize();
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (Item* item = p->GetItemByPos(b, j))
                        emitItem(partySlot, b, j, item);
            }
        }
        // Party-wide capacity + shared gold pool (requester's money is the pool;
        // PartyHooks mirrors every delta). "BAG:"/"CAP:"/"POOL:" carry fewer than
        // the 6 colons the item parser needs, so the items loop ignores them.
        { std::ostringstream r; r << "CAP:"  << totalSlots;             records.push_back(r.str()); }
        { std::ostringstream r; r << "POOL:" << requester->GetMoney();  records.push_back(r.str()); }
        // The requester's BANK bag slots: one record per slot (idx:purchased:bagEntry) so
        // the panel shows which are unlocked and which bag sits in each. <6 colons, so the
        // item parser ignores them like BAG:/CAP:/POOL:.
        for (uint8 s = BANK_SLOT_BAG_START; s < BANK_SLOT_BAG_END; ++s)
        {
            uint32 const idx = s - BANK_SLOT_BAG_START;
            uint32 const purchased = idx < requester->GetBankBagSlotCount() ? 1u : 0u;
            Item* bagItem = requester->GetItemByPos(INVENTORY_SLOT_BAG_0, s);
            std::ostringstream r;
            r << "BANKBAG:" << idx << ':' << purchased << ':' << (bagItem ? bagItem->GetEntry() : 0);
            records.push_back(r.str());
        }

        // CHUNKED send. One INVENTORY message holding the whole party's items is
        // far over the addon-message size the 3.3.5a client accepts — it silently
        // drops the oversized packet and the panel shows nothing (the bug that
        // appeared once every member loaded with a full bag). Stream it:
        //   INV_BEGIN            -> client resets
        //   INVENTORY <chunk> *N -> client APPENDS each (records never split)
        //   INV_END              -> client sorts + renders
        SendWPSP(requester, "INV_BEGIN");
        constexpr size_t MAX_PAYLOAD = 220;   // record bytes per INVENTORY message
        std::string chunk;
        auto flush = [&]()
        {
            if (!chunk.empty()) { SendWPSP(requester, "INVENTORY\t" + chunk); chunk.clear(); }
        };
        for (std::string const& rec : records)
        {
            if (!chunk.empty() && chunk.size() + 1 + rec.size() > MAX_PAYLOAD)
                flush();
            if (!chunk.empty()) chunk += ';';
            chunk += rec;
        }
        flush();
        SendWPSP(requester, "INV_END");
    }

    // TALENTS\t<slot>\t<freePoints>\t<classId>\t<rec>;<rec>;...
    //   rec = tabpage:talentId:row:col:maxRank:curRank:rank1SpellId:prereqTalentId:prereqRank
    //   The addon renders the three trees from this; the class name table for
    //   the tab titles lives client-side. Server enforces all spend rules in
    //   LearnTalent, so the client gating is purely cosmetic.
    // `echo` is the address token the client sent (slot number for an alt, or
    // "h<guid>" for a henchman) so the reply routes to the right talent tab.
    static void SendTalentsForGuid(Player* requester, uint32 guid,
                                   std::string const& echo)
    {
        if (!requester || !requester->GetSession() || !guid) return;
        Player* p = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(guid));
        if (!p) return;

        uint32 const classMask = p->getClassMask();
        std::ostringstream out;
        out << "TALENTS\t" << echo << '\t' << p->GetFreeTalentPoints()
            << '\t' << uint32(p->getClass()) << '\t';

        bool first = true;
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* t = sTalentStore.LookupEntry(i);
            if (!t) continue;
            TalentTabEntry const* tab = sTalentTabStore.LookupEntry(t->TalentTab);
            if (!tab) continue;
            if ((tab->ClassMask & classMask) == 0) continue;  // not this class
            if (tab->petTalentMask) continue;                 // skip pet trees

            uint32 maxRank = 0, curRank = 0, rank1 = 0;
            for (uint8 r = 0; r < MAX_TALENT_RANK; ++r)
            {
                uint32 const sid = t->RankID[r];
                if (!sid) break;
                if (r == 0) rank1 = sid;
                maxRank = r + 1;
                // Most talents are PASSIVE — LearnTalent stores them in the
                // talent map (addTalent), NOT the spell book, so HasSpell()
                // misses them. HasTalent() is the authoritative rank source
                // (it's what LearnTalent itself checks).
                if (p->HasTalent(sid, p->GetActiveSpec())) curRank = r + 1;
            }
            if (!maxRank) continue;

            // Prereq: a real dependency always has DependsOnRank >= 1. Resolve
            // the DBC row index to the prereq's TalentID for the client.
            uint32 prereqTalentId = 0;
            if (t->DependsOnRank > 0)
                if (TalentEntry const* dep = sTalentStore.LookupEntry(t->DependsOn))
                    prereqTalentId = dep->TalentID;

            if (!first) out << ';';
            first = false;
            out << tab->tabpage << ':' << t->TalentID << ':' << t->Row << ':'
                << t->Col << ':' << maxRank << ':' << curRank << ':' << rank1
                << ':' << prereqTalentId << ':' << t->DependsOnRank;
        }
        LOG_INFO("module",
            "[WowPsParty Talents] send token={} {} freePoints={} class={}",
            echo, p->GetName(), p->GetFreeTalentPoints(), uint32(p->getClass()));
        SendWPSP(requester, out.str());
    }

    // Slot-addressed talents (alts). Echoes the slot number.
    void SendTalentsTo(Player* requester, uint32 slot)
    {
        if (!requester || !requester->GetSession()) return;
        uint32 const guid = GuidForAccountSlot(
            requester->GetSession()->GetAccountId(), slot);
        SendTalentsForGuid(requester, guid, std::to_string(slot));
    }

    // QUEST_PROGRESS\t<rec>|<rec>|...
    //   record format: <partySlot>:<questId>:<questTitle>:<obj0name>=<done>/<total>,<obj1name>=<done>/<total>...
    //   - obj names are derived from the quest objective text or the item/NPC name
    //   - delimiter `:` inside titles is replaced with `;` to avoid clashing
    //   - the addon (QuestProgress.lua) parses and renders per-slot sections
    void SendQuestProgressTo(Player* requester)
    {
        if (!requester || !requester->GetSession()) return;
        uint32 const account = requester->GetSession()->GetAccountId();

        std::ostringstream out;
        out << "QUEST_PROGRESS\t";
        bool firstRec = true;

        auto sanitise = [](std::string s) {
            for (char& c : s)
                if (c == ':' || c == '\t' || c == '|' || c == ';' || c == '=' || c == ',')
                    c = ' ';
            return s;
        };

        for (uint8 partySlot = 0; partySlot < PARTY_SIZE; ++partySlot)
        {
            uint32 const guid = GuidForAccountSlot(account, partySlot);
            if (!guid) continue;
            ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
            Player* p = ObjectAccessor::FindConnectedPlayer(og);
            if (!p) continue;

            for (uint8 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
            {
                uint32 const questId = p->GetQuestSlotQuestId(i);
                if (!questId) continue;
                Quest const* qInfo = sObjectMgr->GetQuestTemplate(questId);
                if (!qInfo) continue;
                QuestStatus const status = p->GetQuestStatus(questId);
                if (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE) continue;

                if (!firstRec) out << '|';
                firstRec = false;

                std::string title = sanitise(qInfo->GetTitle());
                out << uint32(partySlot) << ':' << questId << ':' << title << ':';

                bool firstObj = true;

                // Item-deliver objectives
                for (uint8 j = 0; j < QUEST_ITEM_OBJECTIVES_COUNT; ++j)
                {
                    uint32 const reqId    = qInfo->RequiredItemId[j];
                    uint32 const reqCount = qInfo->RequiredItemCount[j];
                    if (!reqId || !reqCount) continue;
                    ItemTemplate const* it = sObjectMgr->GetItemTemplate(reqId);
                    std::string label = it ? sanitise(it->Name1) : std::to_string(reqId);
                    uint32 have = p->GetItemCount(reqId, true);
                    if (have > reqCount) have = reqCount;
                    if (!firstObj) out << ',';
                    firstObj = false;
                    out << label << '=' << have << '/' << reqCount;
                }

                // Kill / interact objectives
                for (uint8 j = 0; j < QUEST_OBJECTIVES_COUNT; ++j)
                {
                    int32 const reqId = qInfo->RequiredNpcOrGo[j];
                    uint32 const reqCount = qInfo->RequiredNpcOrGoCount[j];
                    if (!reqId || !reqCount) continue;
                    // Resolve creature/GO name. Negative ids = gameobjects.
                    std::string label;
                    if (reqId > 0)
                    {
                        if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(uint32(reqId)))
                            label = sanitise(ct->Name);
                        else
                            label = std::to_string(reqId);
                    }
                    else
                    {
                        if (GameObjectTemplate const* gt = sObjectMgr->GetGameObjectTemplate(uint32(-reqId)))
                            label = sanitise(gt->name);
                        else
                            label = std::to_string(reqId);
                    }
                    // QuestStatusData lives in the player's m_QuestStatus map.
                    // We can't grab it directly from Player without a friend
                    // declaration, so read the slot counter via the public
                    // wire field: GetQuestSlotCounter(slot, idx).
                    uint16 have = p->GetReqKillOrCastCurrentCount(questId, reqId);
                    if (have > reqCount) have = reqCount;
                    if (!firstObj) out << ',';
                    firstObj = false;
                    out << label << '=' << uint32(have) << '/' << reqCount;
                }
            }
        }

        if (firstRec)
        {
            // No quests at all — send a sentinel so the addon can show
            // "No active quests across the party."
            out << "NONE";
        }

        SendWPSP(requester, out.str());
    }
}

// UNEQUIP\t<partySlot>\t<eqSlotId>
// Moves the item currently equipped at eqSlotId on the slot's character into
// the first free inventory slot on that same character. Bag stays local to
// the char — no cross-char transfer here (use MOVE for that).
static void HandleUnequip(Player* requester, std::string_view payload)
{
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const partySlot = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const eqSlot    = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);

    if (!requester || !requester->GetSession()) return;
    if (eqSlot >= EQUIPMENT_SLOT_END) return;
    uint32 const account = requester->GetSession()->GetAccountId();

    QueryResult q = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}",
        account, partySlot);
    if (!q) return;
    uint32 const charGuid = q->Fetch()[0].Get<uint32>();
    Player* p = ObjectAccessor::FindConnectedPlayer(
        ObjectGuid::Create<HighGuid::Player>(charGuid));
    if (!p) return;

    Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(eqSlot));
    if (!item)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Nothing equipped at slot {}.", eqSlot);
        return;
    }

    // Find a free inventory slot to drop the item into.
    ItemPosCountVec dest;
    InventoryResult result = p->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);
    if (result != EQUIP_ERR_OK)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r No bag space to unequip ({}).", uint32(result));
        return;
    }
    p->RemoveItem(INVENTORY_SLOT_BAG_0, uint8(eqSlot), true);
    p->StoreItem(dest, item, true);

    WowPsParty::SendGearTo(requester, partySlot);
    WowPsParty::SendStatsTo(requester, partySlot);
    WowPsParty::SendInventoryTo(requester);
}

// EQUIP\t<srcPartySlot>\t<srcItemGuidLow>   (always equips to the currently-
// controlled body — destination slot is derived from the item template's class
// & subclass so the user doesn't have to pick a slot.)
// Player::CanUseItem(Item*, true) minus the soulbind-ownership guard
// (IsBindedNotWith). The shared-party gear feature deliberately equips one of
// the player's alts with another alt's Bind-on-Pickup gear — the equip re-owns
// the item to the destination, so "soulbound to a different character" must not
// block it (that was Kevin's "leather pants on my paladin: target can't use
// this item" report). We still enforce everything that genuinely matters:
// class / race / level / required-skill / required-spell (the ItemTemplate
// overload) PLUS the armor & weapon proficiency that overload omits.
static InventoryResult CanUseItemIgnoringBind(Player* dest, Item* item)
{
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto) return EQUIP_ERR_ITEM_NOT_FOUND;

    InventoryResult const res = dest->CanUseItem(proto);
    if (res != EQUIP_ERR_OK) return res;

    // Heirlooms "morph" their armor type down for a class that hasn't learned
    // the higher proficiency yet, so don't proficiency-gate them (mirrors the
    // engine's heirloom branch in CanUseItem(Item*)).
    uint32 const itemSkill = item->GetSkill();
    if (itemSkill != 0 && proto->Quality != ITEM_QUALITY_HEIRLOOM
        && dest->GetSkillValue(itemSkill) == 0)
        return EQUIP_ERR_NO_REQUIRED_PROFICIENCY;

    return EQUIP_ERR_OK;
}

// Force-persist a shared-inventory transfer for one or both members (defined
// below). Cross-character moves must flush immediately so the destination row is
// written now and no stale source row survives into a deferred, corruptable save.
static void FlushPartyTransfer(Player* a, Player* b);

// Move ONE loose, non-container item out of `from`'s inventory onto any OTHER
// party member that has room. Used to free a single slot so a bigger bag can be
// staged in a regular slot and then swapped into a bag slot on a member whose
// bags are otherwise full (the core won't run a bag-into-slot swap without the
// new bag staged somewhere first). The party inventory is shared, so which member
// ends up holding the displaced item doesn't matter. Returns true if a slot freed.
static bool RelocateOneLooseItem(uint32 account, Player* from)
{
    if (!from) return false;
    std::vector<Player*> peers;
    if (QueryResult q = CharacterDatabase.Query(
            "SELECT `guid` FROM `account_party` WHERE `account` = {}", account))
    {
        do
        {
            uint32 const g = q->Fetch()[0].Get<uint32>();
            ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(g);
            Player* p = ObjectAccessor::FindConnectedPlayer(og);
            if (!p) p = ObjectAccessor::FindPlayer(og);
            if (p && p != from) peers.push_back(p);
        } while (q->NextRow());
    }
    if (peers.empty()) return false;

    auto tryMove = [&](Item* item) -> bool
    {
        if (!item) return false;
        if (item->GetTemplate()->Class == ITEM_CLASS_CONTAINER) return false;  // never displace a bag
        for (Player* peer : peers)
        {
            ItemPosCountVec dpos;
            if (peer->CanStoreItem(NULL_BAG, NULL_SLOT, dpos, item, false) != EQUIP_ERR_OK)
                continue;
            from->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
            item->SetOwnerGUID(peer->GetGUID());
            item->FSetState(ITEM_CHANGED);
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            item->SaveToDB(tx);
            CharacterDatabase.CommitTransaction(tx);
            peer->MoveItemToInventory(dpos, item, true);
            FlushPartyTransfer(from, peer);
            return true;
        }
        return false;
    };

    for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        if (tryMove(from->GetItemByPos(INVENTORY_SLOT_BAG_0, i))) return true;
    for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        if (Bag* bag = from->GetBagByPos(b))
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                if (tryMove(from->GetItemByPos(b, j))) return true;
    return false;
}

// GetItemByGuid walks every bag slot and reads each item's value array
// (Item::GetGUID -> Object::GetGuidValue). A party member's bag can transiently
// hold a dangling / half-initialised Item* -- the recurring use-after-free this
// module already SEH-guards on every panel READ (SafeReadItemFields /
// SafeItemTemplate). The raw core lookup had no such guard: 2026-07-01 a GBANK
// deposit ran GetItemByGuid over a hero bag still holding a freed Item* (left by
// Item::SaveToDB's ITEM_REMOVED delete guard, which frees the object but can't
// clear the owning slot) and AV'd the world thread in Object::GetGuidValue. Route
// every member item lookup through this so a poisoned slot yields nullptr instead
// of taking the server down; all callers already null-check the result.
static Item* SafeGetItemByGuid(Player* p, ObjectGuid guid)
{
    if (!p) return nullptr;
#ifdef _WIN32
    __try { return p->GetItemByGuid(guid); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
#else
    return p->GetItemByGuid(guid);
#endif
}

// Persist a shared-inventory transfer IMMEDIATELY rather than leaving the row
// rewrite to the destination's next deferred _SaveInventory. The cross-character
// move/equip paths detach an item from one member and re-own + re-store it on
// another; the SOURCE row in character_inventory is only cleared when the
// destination's REPLACE (keyed on the item GUID) eventually runs. On a member
// whose bags churn from rapid party-gear edits, that deferred save keeps hitting
// the engine's defensive branches ("the player doesn't have an item at that
// position" / "is there instead" / "queued more than once" / "dangling pointer")
// which SKIP the position write — so the stale source row survives and the item
// reloads onto the wrong character on relog (apparent duplicate) or orphans
// (apparent loss). Force-draining both members' update queues here writes the
// correct rows now (the REPLACE clears any stale source row by PK=item) and
// stops stale/dangling pointers from accumulating into a later corrupted save.
// This is the same immediate-persist discipline HandleMailSend / HandleAhSell
// already use for their single owner.
static void FlushPartyTransfer(Player* a, Player* b)
{
    CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
    if (a) a->SaveInventoryAndGoldToDB(tx);
    if (b && b != a) b->SaveInventoryAndGoldToDB(tx);
    CharacterDatabase.CommitTransaction(tx);
}

// Defined below (shared with HandleUse) — opens a lootable container on its owner.
static bool OpenLootableContainer(Player* requester, Player* srcChar, Item* srcItem);
// Defined below (shared with HandleUse) — moves a mate's item onto the requester.
static Item* PullItemToRequester(Player* requester, Player* srcChar, Item* item);

static void HandleEquip(Player* requester, std::string_view payload)
{
    // Payload formats supported:
    //   "<srcSlot>\t<srcItemGuidLow>"               -- legacy, equips on session player
    //   "<srcSlot>\t<srcItemGuidLow>\t<destSlot>"   -- equip on a specific party slot
    auto firstTab = payload.find('\t');
    if (firstTab == std::string_view::npos) return;
    uint32 const srcSlot = std::strtoul(std::string(payload.substr(0, firstTab)).c_str(), nullptr, 10);

    std::string_view rest = payload.substr(firstTab + 1);
    auto secondTab = rest.find('\t');
    uint32 srcItemGuidLow = 0;
    int destSlot = -1;
    int bagIdx   = -1;   // target bag slot 0..3 for a container; -1 = let core pick
    if (secondTab == std::string_view::npos)
    {
        srcItemGuidLow = std::strtoul(std::string(rest).c_str(), nullptr, 10);
    }
    else
    {
        srcItemGuidLow = std::strtoul(std::string(rest.substr(0, secondTab)).c_str(), nullptr, 10);
        std::string_view after = rest.substr(secondTab + 1);   // "<destSlot>[\t<bagIdx>]"
        auto thirdTab = after.find('\t');
        if (thirdTab == std::string_view::npos)
        {
            destSlot = std::atoi(std::string(after).c_str());
        }
        else
        {
            destSlot = std::atoi(std::string(after.substr(0, thirdTab)).c_str());
            bagIdx   = std::atoi(std::string(after.substr(thirdTab + 1)).c_str());
        }
    }
    if (!srcItemGuidLow) return;

    if (!requester || !requester->GetSession()) return;
    uint32 const account = requester->GetSession()->GetAccountId();

    QueryResult q = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}", account, srcSlot);
    if (!q) return;
    uint32 const srcCharGuid = q->Fetch()[0].Get<uint32>();
    ObjectGuid const srcCharObj = ObjectGuid::Create<HighGuid::Player>(srcCharGuid);
    Player* srcChar = ObjectAccessor::FindConnectedPlayer(srcCharObj);
    if (!srcChar) return;

    Item* srcItem = SafeGetItemByGuid(srcChar, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!srcItem) return;

    // A lootable container (Satchel of Helpful Goods / reward pouch) that reached
    // the EQUIP route is OPENED, not equipped — these reward bags are class Misc
    // with no real bag slot, so trying to equip one just errors "target can't use
    // this item". Open it on its owner instead (same path as the USE route).
    if (OpenLootableContainer(requester, srcChar, srcItem))
        return;

    // A quest-STARTER that is ALSO equippable (Monogrammed Sash, and various
    // trinkets/off-hands) reaches the EQUIP route because the client keys the
    // action purely off equipLoc — so it would silently equip and the human
    // could never take the quest ("can equip it, but can't take its quest").
    // While the quest is still takeable, offer it instead of equipping — same
    // pull-onto-requester + quest-details flow the USE route uses. Once the
    // quest is taken/completed CanTakeQuest is false, so a later click falls
    // through and equips the item as ordinary gear.
    if (ItemTemplate const* t = srcItem->GetTemplate(); t && t->StartQuest)
    {
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(t->StartQuest);
            quest && requester->CanTakeQuest(quest, false))
        {
            Item* pulled = PullItemToRequester(requester, srcChar, srcItem);
            if (!pulled)
            {
                ChatHandler(requester->GetSession()).PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Your bags are full — free a slot to start |cffffffff{}|r's quest.", t->Name1);
                return;
            }
            requester->PlayerTalkClass->SendQuestGiverQuestDetails(quest, pulled->GetGUID(), true);
            WowPsParty::SendInventoryTo(requester);
            if (srcChar != requester) WowPsParty::SendInventoryTo(srcChar);
            return;
        }
    }

    // Resolve destination: if destSlot was specified in payload, look it
    // up; else fall back to the session player (ResolveControlledBody now
    // always returns the session player since swap is removed).
    Player* dest = nullptr;
    if (destSlot >= 0)
    {
        QueryResult qd = CharacterDatabase.Query(
            "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}",
            account, uint32(destSlot));
        if (qd)
        {
            uint32 const destCharGuid = qd->Fetch()[0].Get<uint32>();
            dest = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(destCharGuid));
        }
    }
    if (!dest) dest = WowPsParty::ResolveControlledBody(requester);
    if (!dest) return;

    // Ammo doesn't slot into the regular equipment array — Player tracks
    // the equipped ammo type via PLAYER_AMMO_ID, and arrows are consumed
    // straight from any matching stack in the wielder's bags. The
    // standard CanEquipItem / EquipItem path won't find a slot for it,
    // so handle ammo separately: cross-move into dest's bags if needed,
    // then SetAmmo(itemId).
    if (srcItem->GetTemplate()->InventoryType == INVTYPE_AMMO)
    {
        uint32 const ammoItemId = srcItem->GetEntry();
        if (srcChar != dest)
        {
            // Cross-character move: detach from src, re-own to dest, store.
            srcChar->MoveItemFromInventory(srcItem->GetBagSlot(), srcItem->GetSlot(), true);
            srcItem->SetOwnerGUID(dest->GetGUID());
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            srcItem->SaveToDB(tx);
            CharacterDatabase.CommitTransaction(tx);
            ItemPosCountVec destPos;
            if (dest->CanStoreItem(NULL_BAG, NULL_SLOT, destPos, srcItem, false) == EQUIP_ERR_OK)
            {
                dest->MoveItemToInventory(destPos, srcItem, true);
                FlushPartyTransfer(srcChar, dest);
            }
            else
            {
                // Bags full on dest — give it back to src.
                srcItem->SetOwnerGUID(srcChar->GetGUID());
                ItemPosCountVec backPos;
                if (srcChar->CanStoreItem(NULL_BAG, NULL_SLOT, backPos, srcItem, false) == EQUIP_ERR_OK)
                    srcChar->MoveItemToInventory(backPos, srcItem, true);
                CharacterDatabaseTransaction tx2 = CharacterDatabase.BeginTransaction();
                srcItem->SaveToDB(tx2);
                CharacterDatabase.CommitTransaction(tx2);
                FlushPartyTransfer(srcChar, dest);
                ChatHandler(requester->GetSession()).PSendSysMessage(
                    "|cffff5555[WowPsParty]|r {}'s bags are full — can't take ammo.",
                    dest->GetName());
                return;
            }
        }
        dest->SetAmmo(ammoItemId);
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Set ammo on {}.", dest->GetName());
        WowPsParty::SendInventoryTo(requester);
        return;
    }

    // Validate dest can USE the item (class / race / level / proficiency),
    // ignoring soulbind ownership — moving a member's BoP gear onto another of
    // the player's own alts is the whole point here, and the cross-char branch
    // re-owns the item to dest anyway. (See CanUseItemIgnoringBind.)
    InventoryResult const reason = CanUseItemIgnoringBind(dest, srcItem);
    if (reason != EQUIP_ERR_OK)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Can't equip on slot {}: target can't use this item (code {}).",
            uint32(srcSlot), uint32(reason));
        return;
    }

    // If the user dropped a CONTAINER onto a SPECIFIC bag slot, target that slot
    // (19+bagIdx) so it replaces the bag they aimed at — CanEquipItem(NULL_SLOT)
    // always routed to the FIRST bag slot regardless of where they dropped.
    uint8 const targetEqSlot =
        (bagIdx >= 0 && bagIdx < int(INVENTORY_SLOT_BAG_END - INVENTORY_SLOT_BAG_START)
         && srcItem->GetTemplate()->Class == ITEM_CLASS_CONTAINER)
        ? uint8(INVENTORY_SLOT_BAG_START + bagIdx)
        : uint8(NULL_SLOT);

    LOG_INFO("module",
        "[WowPsParty Equip] parse: srcSlot={} destSlot={} bagIdx={} class={} -> targetEqSlot={}",
        srcSlot, destSlot, bagIdx, uint32(srcItem->GetTemplate()->Class),
        uint32(targetEqSlot));

    if (srcChar == dest)
    {
        // Same character: the item is already owned by dest, so CanEquipItem's
        // bind check passes. swap=true auto-unequips the current occupant back
        // into the source bag slot (otherwise CanEquipItem returns NOT_EQUIPPABLE
        // when the slot is occupied). SwapItem is the canonical equip-into-
        // (possibly-occupied)-slot path; a manual RemoveItem + EquipItem used to
        // VANISH the item when the dest slot was full.
        uint16 eqDest;
        InventoryResult const result =
            dest->CanEquipItem(targetEqSlot, eqDest, srcItem, /*swap=*/true, /*not_loading=*/true);
        if (result != EQUIP_ERR_OK)
        {
            ChatHandler(requester->GetSession()).PSendSysMessage(
                "|cffff5555[WowPsParty]|r Equip rejected ({}). Check requirements.", uint32(result));
            return;
        }
        uint16 const srcPos = srcItem->GetPos();
        LOG_INFO("module",
            "[WowPsParty Equip] same-char swap: char={} item={} ({}) srcPos={:#x} eqDest={:#x}",
            dest->GetName(), srcItem->GetEntry(), srcItem->GetTemplate()->Name1, srcPos, eqDest);
        dest->SwapItem(srcPos, eqDest);
        FlushPartyTransfer(dest, nullptr);
    }
    else
    {
        // Cross-character transfer that PRESERVES the Item object (enchants,
        // gems, durability, charges, random props, soulbind state). Re-own it to
        // dest BEFORE the equip checks: CanEquipItem (and the CanUseItem it calls
        // internally) reject anything soulbound to another character, and that
        // guard only clears once owner == dest. On any failure we bounce the item
        // back to src so it never strands or vanishes.
        srcChar->MoveItemFromInventory(srcItem->GetBagSlot(), srcItem->GetSlot(), true);
        srcItem->SetOwnerGUID(dest->GetGUID());
        // Flush the item row now so item_instance.owner_guid tracks the new owner
        // (SetOwnerGUID leaves the item UNCHANGED, so SaveToDB would no-op without
        // marking it dirty first). MoveItemToInventory below writes the final bag
        // placement on the normal inventory save.
        srcItem->FSetState(ITEM_CHANGED);
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        srcItem->SaveToDB(tx);
        CharacterDatabase.CommitTransaction(tx);

        auto bounceBackToSrc = [&]()
        {
            srcItem->SetOwnerGUID(srcChar->GetGUID());
            ItemPosCountVec backPos;
            if (srcChar->CanStoreItem(NULL_BAG, NULL_SLOT, backPos, srcItem, false) == EQUIP_ERR_OK)
                srcChar->MoveItemToInventory(backPos, srcItem, true);
            srcItem->FSetState(ITEM_CHANGED);
            CharacterDatabaseTransaction txb = CharacterDatabase.BeginTransaction();
            srcItem->SaveToDB(txb);
            CharacterDatabase.CommitTransaction(txb);
            FlushPartyTransfer(srcChar, dest);
        };

        ItemPosCountVec destPos;
        bool canStore =
            (dest->CanStoreItem(NULL_BAG, NULL_SLOT, destPos, srcItem, false) == EQUIP_ERR_OK);
        // A container is equipped into a bag SLOT — a 1-for-1 swap where the old
        // bag's contents redistribute into the new (bigger) one. The core still
        // needs the new bag STAGED in a regular slot to run that swap, which a
        // member with full bags has no room for (Kevin's "can't equip a bigger
        // bag, bags are full"). Free one slot by shifting a loose item to another
        // shared-inventory member, then retry the store.
        if (!canStore && srcItem->GetTemplate()->Class == ITEM_CLASS_CONTAINER)
        {
            if (RelocateOneLooseItem(account, dest))
                canStore = (dest->CanStoreItem(NULL_BAG, NULL_SLOT, destPos, srcItem, false)
                            == EQUIP_ERR_OK);
        }
        if (!canStore)
        {
            // dest's bags are full — give it back so the item doesn't vanish.
            bounceBackToSrc();
            ChatHandler(requester->GetSession()).PSendSysMessage(
                "|cffff5555[WowPsParty]|r {}'s bags are full — can't take the item.", dest->GetName());
            WowPsParty::SendInventoryTo(requester);
            return;
        }
        dest->MoveItemToInventory(destPos, srcItem, true);

        // Now owner == dest, so the bind guard passes. swap=true so an occupied
        // slot's current item travels back into the bag slot atomically.
        uint16 eqDest;
        InventoryResult const result =
            dest->CanEquipItem(targetEqSlot, eqDest, srcItem, /*swap=*/true, /*not_loading=*/true);
        if (result != EQUIP_ERR_OK)
        {
            // Couldn't equip after the move (e.g. combat, unique-equipped) —
            // detach from dest and send it home so it isn't silently relocated.
            dest->MoveItemFromInventory(srcItem->GetBagSlot(), srcItem->GetSlot(), true);
            bounceBackToSrc();
            ChatHandler(requester->GetSession()).PSendSysMessage(
                "|cffff5555[WowPsParty]|r Equip rejected ({}). Check requirements.", uint32(result));
            WowPsParty::SendInventoryTo(requester);
            return;
        }

        uint16 const bagPos = srcItem->GetPos();
        LOG_INFO("module",
            "[WowPsParty Equip] x-char swap: from={} to={} item={} ({}) bagPos={:#x} eqDest={:#x}",
            srcChar->GetName(), dest->GetName(),
            srcItem->GetEntry(), srcItem->GetTemplate()->Name1, bagPos, eqDest);
        dest->SwapItem(bagPos, eqDest);

        // SwapItem re-validates internally (CanUnequipItem on the slot's current
        // occupant, the reverse store) beyond what CanEquipItem covered. If that
        // failed it left the item in dest's bags and sent the equip error to
        // dest's session (a bot), invisible to us — detect the strand and send
        // the item home with feedback rather than silently relocating it.
        // Check the item actually reached eqDest, NOT Item::IsEquipped(): that is
        // `slot < EQUIPMENT_SLOT_END`, which is FALSE for a bag correctly placed in
        // a bag slot (19-22) — using it stranded every bag-equip (Kevin's report).
        if (srcItem->GetPos() != eqDest)
        {
            dest->MoveItemFromInventory(srcItem->GetBagSlot(), srcItem->GetSlot(), true);
            bounceBackToSrc();
            ChatHandler(requester->GetSession()).PSendSysMessage(
                "|cffff5555[WowPsParty]|r Equip rejected — couldn't swap into that slot.");
            WowPsParty::SendInventoryTo(requester);
            return;
        }

        // Equip succeeded on a DIFFERENT character than the item came from:
        // persist both members now so the source's stale character_inventory row
        // can't survive into a deferred save and reload the item onto the wrong
        // character on relog (the duplicate-item / lost-gear report).
        FlushPartyTransfer(srcChar, dest);
    }

    // Refresh client UI on both sides
    WowPsParty::SendInventoryTo(requester);
    if (auto srcS = sPartyMgr.GetSlotForGuid(srcCharGuid))
    {
        WowPsParty::SendGearTo(requester, uint32(*srcS));
        WowPsParty::SendStatsTo(requester, uint32(*srcS));
    }
    if (auto dstS = sPartyMgr.GetSlotForGuid(dest->GetGUID().GetCounter()))
    {
        WowPsParty::SendGearTo(requester, uint32(*dstS));
        WowPsParty::SendStatsTo(requester, uint32(*dstS));
    }
}

// =============================================================================
// Chunked-rotation builder. The full SET_ROTATION payload for a 9-rule
// rotation exceeds the ~255-byte SendAddonMessage cap on 3.3.5a, so the
// client now sends BEGIN_ROTATION / ROTATION_RULE × N / COMMIT_ROTATION
// instead. This map buffers rules per (playerGuid << 8 | slot).
// =============================================================================
namespace {
    std::unordered_map<uint64, std::vector<WowPsParty::RotationRule>>& PendingRotationMap()
    {
        static std::unordered_map<uint64, std::vector<WowPsParty::RotationRule>> m;
        return m;
    }

    // Pending COMMON shared-rotation rules mid-save, keyed by ACCOUNT (the editor's).
    std::unordered_map<uint32, std::vector<WowPsParty::RotationRule>>& PendingSharedRotationMap()
    {
        static std::unordered_map<uint32, std::vector<WowPsParty::RotationRule>> m;
        return m;
    }

    // Pending per-mob COMMON section mid-save, keyed by ACCOUNT. Sections commit one at a
    // time (BEGIN sets the name + clears, RULE lines append, COMMIT flushes), so a single
    // in-progress section per account is enough — the name comes from BEGIN, not each RULE.
    struct PendingMobSection { std::string name; std::vector<WowPsParty::RotationRule> rules; };
    std::unordered_map<uint32, PendingMobSection>& PendingMobRotationMap()
    {
        static std::unordered_map<uint32, PendingMobSection> m;
        return m;
    }

    // -------- Oversized-message (WPSP_FRAG) reassembly -----------------------
    // 3.3.5a's SendAddonMessage silently DROPS any single message whose wire form
    // exceeds ~255 bytes. Each rotation rule is sent as its own ROTATION_RULE /
    // SHARED_ROTATION_RULE line, so one rule with a long comma-separated name list
    // (a Focus-fire rule naming a dozen mobs, a big target_name: clause, ...) could
    // blow past that limit and vanish mid-save without any error — exactly how a
    // long Common-tab focus rule disappeared. The addon now splits any oversized
    // line into ordered WPSP_FRAG chunks; we buffer them here and hand back the
    // reassembled original the moment the final chunk lands.
    struct FragmentBuffer
    {
        uint32                   total = 0;
        std::vector<std::string> chunks;
    };

    // Partial reassembly buffers keyed by (playerGuidLow << 16 | msgId). A buffer
    // is erased as soon as its message completes, so at most a handful of tiny
    // partials ever linger (one per in-flight fragmented line).
    std::unordered_map<uint64, FragmentBuffer>& FragmentBuffers()
    {
        static std::unordered_map<uint64, FragmentBuffer> m;
        return m;
    }

    // Accept one WPSP_FRAG payload: "<msgId>\t<seq>\t<total>\t<chunk>". Returns
    // true and fills `out` with the reassembled original body once every chunk has
    // arrived; false while chunks are still pending (or the fragment is malformed).
    // Every field is bounded so a broken/hostile client can't drive an allocation.
    bool ReassembleFragment(Player* player, std::string_view payload, std::string& out)
    {
        std::string_view rest = payload;
        auto nextField = [&rest]() -> std::string_view
        {
            size_t tab = rest.find('\t');
            if (tab == std::string_view::npos)
            {
                std::string_view f = rest;
                rest = std::string_view();
                return f;
            }
            std::string_view f = rest.substr(0, tab);
            rest = rest.substr(tab + 1);
            return f;
        };

        std::string_view idSv    = nextField();
        std::string_view seqSv   = nextField();
        std::string_view totalSv = nextField();
        std::string_view chunk   = rest;   // remainder is the chunk, verbatim

        if (idSv.empty() || seqSv.empty() || totalSv.empty())
            return false;

        uint32 const msgId = uint32(std::strtoul(std::string(idSv).c_str(), nullptr, 10));
        uint32 const seq   = uint32(std::strtoul(std::string(seqSv).c_str(), nullptr, 10));
        uint32 const total = uint32(std::strtoul(std::string(totalSv).c_str(), nullptr, 10));

        // A rotation rule fragments into a few ~200-byte chunks, never dozens.
        if (total == 0 || total > 64 || seq == 0 || seq > total)
            return false;

        // Key by (playerGuidLow << 16 | msgId). The client id cycles 1..4096, so
        // the 16-bit msgId mask is lossless — the two never overlap the guid bits.
        uint32 const guidLow = player->GetGUID().GetCounter();
        uint64 const key = (uint64(guidLow) << 16) | (msgId & 0xFFFF);
        auto& buffers = FragmentBuffers();

        auto it = buffers.find(key);
        if (it == buffers.end())
        {
            // Starting a fresh reassembly. We only erase a buffer on successful
            // completion, so a client that logs out or stops mid-send (or a
            // hostile one iterating msgId to leak a buffer per id) would grow the
            // map forever. A single Save & Apply never has more than one
            // fragmented rule in flight at a time, so more than a handful of live
            // partials for one player is stale junk — drop all of that player's
            // partials before starting a new one. Bounds the map at a few entries
            // per active player without a timer or a logout hook.
            uint32 live = 0;
            for (auto const& kv : buffers)
                if (uint32(kv.first >> 16) == guidLow)
                    ++live;
            if (live >= 8)
                for (auto i = buffers.begin(); i != buffers.end();)
                {
                    if (uint32(i->first >> 16) == guidLow)
                        i = buffers.erase(i);
                    else
                        ++i;
                }
            it = buffers.emplace(key, FragmentBuffer{}).first;
        }

        FragmentBuffer& buf = it->second;
        if (buf.total != total)   // first chunk of this id (or a restarted send)
        {
            buf.total = total;
            buf.chunks.assign(total, std::string());
        }
        buf.chunks[seq - 1].assign(chunk.data(), chunk.size());

        // Chunks are never empty on the wire, so "no empty slot" == complete.
        for (std::string const& c : buf.chunks)
            if (c.empty())
                return false;

        out.clear();
        for (std::string const& c : buf.chunks)
            out += c;
        FragmentBuffers().erase(key);
        return true;
    }
}

// SELL\t<srcPartySlot>\t<srcItemGuidLow>
//   Vendor-sells an item out of any party member's bag while the requester
//   has a merchant frame open. The item is destroyed and the requester is
//   credited with the standard ItemTemplate::SellPrice * count gold — the
//   shared-gold hook (PartyHooks.cpp::OnPlayerMoneyChanged) then mirrors the
//   gain to every other party member so the pool stays consistent.
//
//   We deliberately bypass the durability-refund path in HandleSellItemOpcode:
//   it only kicks in for equipped items with lost durability, which the
//   shared-inventory UI doesn't surface anyway (only bag items).
static void SendBuybackTo(Player* requester);   // defined below; refreshes the Buyback view

static void HandleSell(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const srcSlot = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);
    if (!srcItemGuidLow) return;

    uint32 const account = requester->GetSession()->GetAccountId();
    QueryResult q = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}", account, srcSlot);
    if (!q) return;
    uint32 const srcCharGuid = q->Fetch()[0].Get<uint32>();
    Player* srcChar = ObjectAccessor::FindConnectedPlayer(
        ObjectGuid::Create<HighGuid::Player>(srcCharGuid));
    if (!srcChar) return;

    Item* srcItem = SafeGetItemByGuid(srcChar, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!srcItem) return;

    ItemTemplate const* tmpl = srcItem->GetTemplate();
    if (!tmpl) return;

    if (srcItem->IsNotEmptyBag())
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Empty the bag before selling.");
        return;
    }
    if (srcItem->IsEquipped())
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Unequip the item before selling.");
        return;
    }
    if (tmpl->SellPrice == 0)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r |cffffffff{}|r has no sell value.", tmpl->Name1);
        return;
    }

    uint32 const count = srcItem->GetCount();
    uint32 const money = tmpl->SellPrice * count;
    std::string const soldName = tmpl->Name1;

    // Route the sale through the BUYBACK list (exactly like the core merchant)
    // instead of destroying the item, so an accidental sale is recoverable from
    // the party inventory's Buyback view. The item stays owned by srcChar and
    // lands in its 12-slot buyback list (in-memory like vanilla — lost on logout).
    srcChar->ItemRemovedQuestCheck(srcItem->GetEntry(), count);   // mirror core; HandleBuyback re-adds
    srcChar->RemoveItem(srcItem->GetBagSlot(), srcItem->GetSlot(), true);
    srcItem->RemoveFromUpdateQueueOf(srcChar);
    srcChar->AddItemToBuyBackSlot(srcItem, money);
    requester->ModifyMoney(int32(money));

    ChatHandler(requester->GetSession()).PSendSysMessage(
        "|cff66ccff[WowPsParty]|r Sold |cffffffff{}|r for |cffffd100{}.{}.{}|r "
        "(recover it from the Buyback view).",
        soldName,
        money / 10000, (money / 100) % 100, money % 100);

    WowPsParty::SendInventoryTo(requester);
    SendBuybackTo(requester);
}

// BB_BEGIN / BUYBACK <chunk>… / BB_END — every loaded party member's 12-slot
// buyback list (items they've sold, recoverable). Mirrors SendInventoryTo's
// chunked stream. Record:
//   <ownerSlot>:<bbIdx>:<itemId>:<count>:<price>:<itemGuidLow>:<randProp>:<suffixFactor>
static void SendBuybackTo(Player* requester)
{
    if (!requester || !requester->GetSession()) return;
    uint32 const account = requester->GetSession()->GetAccountId();

    std::vector<std::string> records;
    for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
    {
        uint32 const guid = WowPsParty::GuidForAccountSlot(account, partySlot);
        if (!guid) continue;
        ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
        Player* p = ObjectAccessor::FindConnectedPlayer(og);
        if (!p) p = ObjectAccessor::FindPlayer(og);
        if (!p) continue;
        // Same teardown guard as SendInventoryTo — a member mid-logout/removal hands
        // back freed Item*s and faults the world thread (this is called on the same
        // sell path right after SendInventoryTo, so it shares the hazard).
        WorldSession* psess = p->GetSession();
        if (!p->IsInWorld() || p->IsDuringRemoveFromWorld()
            || (psess && psess->isLogingOut()))
            continue;

        for (uint8 s = BUYBACK_SLOT_START; s < BUYBACK_SLOT_END; ++s)
        {
            Item* it = p->GetItemFromBuyBackSlot(s);
            if (!it) continue;
            WowPsParty::PartyItemFields f;
            if (!WowPsParty::SafeReadItemFields(it, f))
            {
                LOG_ERROR("module", "[WowPsParty] SendBuybackTo: SKIPPED a bad item "
                    "ptr=0x{:x} owner={} bbslot={} (value-array AV — dangling item)",
                    reinterpret_cast<uintptr_t>(it), p->GetGUID().GetCounter(), uint32(s));
                continue;
            }
            uint8 const idx = uint8(s - BUYBACK_SLOT_START);
            uint32 const price = p->GetUInt32Value(PLAYER_FIELD_BUYBACK_PRICE_1 + idx);
            std::ostringstream r;
            r << uint32(partySlot) << ':' << uint32(idx) << ':'
              << f.entry << ':' << f.count << ':' << price << ':'
              << f.guidLow << ':'
              << f.randProp << ':' << f.suffix;
            records.push_back(r.str());
        }
    }

    SendWPSP(requester, "BB_BEGIN");
    constexpr size_t MAX_PAYLOAD = 220;
    std::string chunk;
    auto flush = [&]()
    {
        if (!chunk.empty()) { SendWPSP(requester, "BUYBACK\t" + chunk); chunk.clear(); }
    };
    for (std::string const& rec : records)
    {
        if (!chunk.empty() && chunk.size() + 1 + rec.size() > MAX_PAYLOAD) flush();
        if (!chunk.empty()) chunk += ';';
        chunk += rec;
    }
    flush();
    SendWPSP(requester, "BB_END");
}

// BUYBACK\t<ownerPartySlot>\t<bbIdx> — restore a sold item from that member's
// buyback list back to the member, charging the shared pool the original sale
// price. No vendor needed (we call the storage primitives directly, not the
// vendor-gated HandleBuybackItem).
static void HandleBuyback(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const ownerSlot = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const bbIdx     = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);
    if (bbIdx >= uint32(BUYBACK_SLOT_END - BUYBACK_SLOT_START)) return;

    uint32 const account = requester->GetSession()->GetAccountId();
    QueryResult q = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}", account, ownerSlot);
    if (!q) return;
    Player* ownerChar = ObjectAccessor::FindConnectedPlayer(
        ObjectGuid::Create<HighGuid::Player>(q->Fetch()[0].Get<uint32>()));
    if (!ownerChar) return;

    uint32 const slot = BUYBACK_SLOT_START + bbIdx;
    Item* item = ownerChar->GetItemFromBuyBackSlot(slot);
    if (!item)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r That buyback item is no longer available.");
        return;
    }
    uint32 const price = ownerChar->GetUInt32Value(PLAYER_FIELD_BUYBACK_PRICE_1 + bbIdx);
    if (!requester->HasEnoughMoney(price))
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Not enough gold in the shared pool to buy that back.");
        return;
    }
    ItemPosCountVec dest;
    if (ownerChar->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r {}'s bags are full — can't buy that back.", ownerChar->GetName());
        return;
    }
    std::string const itemName = item->GetTemplate() ? item->GetTemplate()->Name1 : "item";
    requester->ModifyMoney(-int32(price));
    ownerChar->RemoveItemFromBuyBackSlot(slot, false);
    ownerChar->ItemAddedQuestCheck(item->GetEntry(), item->GetCount());
    ownerChar->StoreItem(dest, item, true);

    ChatHandler(requester->GetSession()).PSendSysMessage(
        "|cff66ccff[WowPsParty]|r Bought back |cffffffff{}|r for |cffffd100{}.{}.{}|r to {}.",
        itemName, price / 10000, (price / 100) % 100, price % 100, ownerChar->GetName());

    WowPsParty::SendInventoryTo(requester);
    SendBuybackTo(requester);
}

// One BANKVIEW record. ownerGuidLow/ownerName tag WHICH of the account's characters
// physically holds the item, so the account-wide Bank view can label "Stored on <char>"
// and cluster each alt's reserves. 3.3.5a character names never contain ':' or ';', so
// they append raw into the ':'-delimited, ';'-separated record stream without escaping.
static std::string MakeBankRecord(WowPsParty::PartyItemFields const& f,
                                  uint32 ownerGuidLow, std::string const& ownerName)
{
    std::ostringstream r;
    r << f.entry << ':' << f.count << ':' << f.guidLow << ':'
      << f.randProp << ':' << f.suffix << ':' << f.enchant
      << ':' << (f.soulbound ? 1 : 0)          // instance bind state (see SendInventoryTo)
      << ':' << ownerGuidLow << ':' << ownerName;
    return r.str();
}

// Append the REQUESTER's own LIVE bank (28 main slots + every bank-bag's contents) as
// records tagged with the requester as owner. The bag CONTAINERS themselves aren't
// emitted (those live in the BANKBAG strip); strictly their contents + loose bank items.
static void AppendLiveBankRecords(Player* requester, std::vector<std::string>& records)
{
    std::string const selfName = requester->GetName();
    uint32 const selfGuid = requester->GetGUID().GetCounter();
    auto emit = [&](Item* it)
    {
        if (!it) return;
        WowPsParty::PartyItemFields f;
        if (!WowPsParty::SafeReadItemFields(it, f))
        {
            LOG_ERROR("module", "[WowPsParty] SendBankTo: SKIPPED a bad item ptr=0x{:x} owner={}",
                reinterpret_cast<uintptr_t>(it), selfGuid);
            return;
        }
        records.push_back(MakeBankRecord(f, selfGuid, selfName));
    };
    for (uint8 s = BANK_SLOT_ITEM_START; s < BANK_SLOT_ITEM_END; ++s)
        emit(requester->GetItemByPos(INVENTORY_SLOT_BAG_0, s));
    for (uint8 b = BANK_SLOT_BAG_START; b < BANK_SLOT_BAG_END; ++b)
        if (Bag* bag = requester->GetBagByPos(b))
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                emit(requester->GetItemByPos(b, j));
}

// Classify one character_inventory row (bag/slot + item guid) as a BANK item. Rows are
// read bag-ASC, so the bag==0 rows — which include the bank-bag CONTAINERS in slots
// 67-73 — are always visited before any in-bag content rows; thus `bankBagGuids` is
// fully populated by the time a bag-contained row (bag == a container guid) is judged.
static bool ClassifyOfflineBankRow(uint32 bag, uint8 slot, uint32 itemGuid,
                                   std::unordered_set<uint32>& bankBagGuids)
{
    if (bag == 0)
    {
        if (slot >= BANK_SLOT_BAG_START && slot < BANK_SLOT_BAG_END)
        {
            bankBagGuids.insert(itemGuid);   // a bank-bag container (not itself a bank item)
            return false;
        }
        return slot >= BANK_SLOT_ITEM_START && slot < BANK_SLOT_ITEM_END;
    }
    return bankBagGuids.count(bag) != 0;     // an item sitting inside a bank bag
}

// One character's full inventory joined to item_instance, columns 0-10 laid out to match
// Item::LoadFromDB (see CHAR_SEL_CHARACTER_INVENTORY) plus ci.bag/ci.slot/ci.item/entry.
// Shared by the account-bank read and the cross-char withdraw so the bank-position rules
// stay identical. Synchronous (user-initiated, infrequent — panel open / a withdraw).
static QueryResult QueryCharInventoryForBank(uint32 ownerGuidLow)
{
    return CharacterDatabase.Query(
        "SELECT ii.creatorGuid, ii.giftCreatorGuid, ii.count, ii.duration, ii.charges, "
        "ii.flags, ii.enchantments, ii.randomPropertyId, ii.durability, ii.playedTime, "
        "ii.text, ci.bag, ci.slot, ci.item, ii.itemEntry "
        "FROM character_inventory ci JOIN item_instance ii ON ci.item = ii.guid "
        "WHERE ci.guid = {} ORDER BY ci.bag ASC, ci.slot ASC",
        ownerGuidLow);
}

// Append the bank contents of every OTHER (offline) character on the requester's account,
// making the Bank view account-wide — one place to see & retrieve reserve items no matter
// which alt banked them. Only one char per account is online at a time, so every account
// char except the requester is offline and read from the DB. A SINGLE query over the whole
// account (ordered by owner, then bag/slot) keeps this to one world-thread round-trip; the
// per-owner container set resets whenever ci.guid changes. Each bank item is briefly loaded
// into a throwaway Item* so the same SafeReadItemFields extraction as the live path is
// reused (no item_instance blob parsing), then freed.
static void AppendAccountBankRecords(Player* requester, std::vector<std::string>& records)
{
    uint32 const account  = requester->GetSession()->GetAccountId();
    uint32 const selfGuid = requester->GetGUID().GetCounter();
    QueryResult q = CharacterDatabase.Query(
        "SELECT ii.creatorGuid, ii.giftCreatorGuid, ii.count, ii.duration, ii.charges, "
        "ii.flags, ii.enchantments, ii.randomPropertyId, ii.durability, ii.playedTime, "
        "ii.text, ci.bag, ci.slot, ci.item, ii.itemEntry, ci.guid, c.name "
        "FROM characters c "
        "JOIN character_inventory ci ON ci.guid = c.guid "
        "JOIN item_instance ii ON ci.item = ii.guid "
        "WHERE c.account = {} AND c.guid <> {} "
        "AND (c.deleteInfos_Account IS NULL OR c.deleteInfos_Account = 0) "
        "ORDER BY ci.guid ASC, ci.bag ASC, ci.slot ASC",
        account, selfGuid);
    if (!q) return;

    uint32 curOwner = 0;
    bool ownerOnline = false;
    std::string ownerName;
    std::unordered_set<uint32> bankBagGuids;
    do
    {
        Field* fields = q->Fetch();
        uint32 const owner = fields[15].Get<uint32>();
        if (owner != curOwner)                       // reached a new character — reset per-owner state
        {
            curOwner = owner;
            ownerName = fields[16].Get<std::string>();
            bankBagGuids.clear();
            // Never DB-read a char that's actually in memory (defensive: 1 online/acct).
            ownerOnline = ObjectAccessor::FindConnectedPlayer(
                              ObjectGuid::Create<HighGuid::Player>(owner)) != nullptr;
        }
        if (ownerOnline) continue;

        uint32 const bag      = fields[11].Get<uint32>();
        uint8  const slot     = fields[12].Get<uint8>();
        uint32 const itemGuid = fields[13].Get<uint32>();
        uint32 const entry    = fields[14].Get<uint32>();
        if (!ClassifyOfflineBankRow(bag, slot, itemGuid, bankBagGuids))
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto) continue;
        Item* it = NewItemOrBag(proto);
        if (!it->LoadFromDB(itemGuid, ObjectGuid::Empty, fields, entry)) { delete it; continue; }
        WowPsParty::PartyItemFields f;
        if (WowPsParty::SafeReadItemFields(it, f))
            records.push_back(MakeBankRecord(f, owner, ownerName));
        delete it;
    } while (q->NextRow());
}

// Stream the account-wide BANK contents — the requester's own live bank PLUS every other
// account character's banked items — so the panel's Bank view can SHOW (and withdraw)
// reserve items across ALL the account's characters. Without this the bank was per-char.
// Mirrors SendBuybackTo's chunked, AV-safe stream.
static void SendBankTo(Player* requester)
{
    if (!requester || !requester->GetSession()) return;

    std::vector<std::string> records;
    AppendLiveBankRecords(requester, records);
    size_t const liveCount = records.size();
    AppendAccountBankRecords(requester, records);
    LOG_DEBUG("module", "[WowPsParty] SendBankTo: {} bank item(s) for account of {} ({} own live, {} from offline alts)",
        records.size(), requester->GetName(), liveCount, records.size() - liveCount);

    SendWPSP(requester, "BANK_BEGIN");
    constexpr size_t MAX_PAYLOAD = 220;
    std::string chunk;
    auto flush = [&]()
    {
        if (!chunk.empty()) { SendWPSP(requester, "BANKVIEW\t" + chunk); chunk.clear(); }
    };
    for (std::string const& rec : records)
    {
        if (!chunk.empty() && chunk.size() + 1 + rec.size() > MAX_PAYLOAD) flush();
        if (!chunk.empty()) chunk += ';';
        chunk += rec;
    }
    flush();
    SendWPSP(requester, "BANK_END");
}

// Withdraw a banked item that lives on ANOTHER (offline) character of the requester's
// account into the requester's OWN bags — the account-wide half of WITHDRAW. The item is
// loaded from the DB and, in ONE transaction, the source character's inventory row is
// deleted while the item is re-owned + stored to the requester (mirrors MailHandler's
// HandleMailTakeItem: MoveItemToInventory + SaveInventoryAndGoldToDB). Atomic, so there is
// no dupe and no orphan; bags-full is checked BEFORE the delete so a failed withdraw
// leaves the source bank untouched. Returns true if it OWNED the request (did the move or
// deliberately rejected it), false to let the caller fall through to its own error path.
static bool HandleOfflineBankWithdraw(Player* requester, uint32 itemGuidLow)
{
    // The guid comes from the client, so never trust it: resolve the real owner and
    // confirm the item belongs to THIS account before touching anything.
    QueryResult og = CharacterDatabase.Query(
        "SELECT owner_guid FROM item_instance WHERE guid = {}", itemGuidLow);
    if (!og) return false;
    uint32 const ownerGuidLow = og->Fetch()[0].Get<uint32>();
    if (!ownerGuidLow || ownerGuidLow == requester->GetGUID().GetCounter())
        return false;   // no owner, or the requester's own item (the live path's job)

    ObjectGuid const ownerGuid = ObjectGuid::Create<HighGuid::Player>(ownerGuidLow);
    if (sCharacterCache->GetCharacterAccountIdByGuid(ownerGuid) != requester->GetSession()->GetAccountId())
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r That item isn't on one of your characters.");
        return true;
    }
    if (ObjectAccessor::FindConnectedPlayer(ownerGuid))
        return false;   // owner is in memory (shouldn't happen: 1 online/acct) — not our path

    // Load the item, but ONLY if it is genuinely a BANK item of that character (so this
    // can't be used to strip an offline alt's equipped or bagged gear).
    QueryResult q = QueryCharInventoryForBank(ownerGuidLow);
    if (!q)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r That item is no longer in the bank.");
        SendBankTo(requester);
        return true;
    }
    std::unordered_set<uint32> bankBagGuids;
    Item* loaded = nullptr;
    do
    {
        Field* fields = q->Fetch();
        uint32 const bag     = fields[11].Get<uint32>();
        uint8  const slot    = fields[12].Get<uint8>();
        uint32 const rowGuid = fields[13].Get<uint32>();
        uint32 const entry   = fields[14].Get<uint32>();
        bool const isBank = ClassifyOfflineBankRow(bag, slot, rowGuid, bankBagGuids);
        if (rowGuid != itemGuidLow)
            continue;
        if (isBank)
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry))
            {
                Item* it = NewItemOrBag(proto);
                if (it->LoadFromDB(itemGuidLow, ObjectGuid::Empty, fields, entry))
                    loaded = it;
                else
                    delete it;
            }
        break;   // found the target row (bank or not) — stop scanning
    } while (q->NextRow());

    if (!loaded)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r That item is no longer in the bank.");
        SendBankTo(requester);
        return true;
    }

    // A bag can only be a bank-BAG container (excluded by ClassifyOfflineBankRow), never a
    // bank ITEM — so a Bag reaching here means corrupt data. Refuse rather than move a
    // container and strand its contents' rows (which point at the moved bag guid).
    if (loaded->IsBag())
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r That item can't be withdrawn from the bank.");
        delete loaded;
        return true;
    }

    ItemTemplate const* t = loaded->GetTemplate();
    std::string const itemName = t ? t->Name1 : "item";

    ItemPosCountVec dest;
    if (requester->CanStoreItem(NULL_BAG, NULL_SLOT, dest, loaded, false) != EQUIP_ERR_OK)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Your bags are full — couldn't withdraw |cffffffff{}|r.", itemName);
        delete loaded;
        return true;
    }

    std::string ownerName;
    sCharacterCache->GetCharacterNameByGuid(ownerGuid, ownerName);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    CharacterDatabasePreparedStatement* del =
        CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_INVENTORY_BY_ITEM);
    del->SetData(0, itemGuidLow);
    trans->Append(del);   // detach from the source character's bank

    loaded->SetState(ITEM_UNCHANGED);                     // clean slate before store (as HandleMailTakeItem)
    requester->MoveItemToInventory(dest, loaded, true);   // ITEM_NEW → new char_inventory row + owner=requester
    requester->SaveInventoryAndGoldToDB(trans);           // persist both sides in the SAME transaction
    CharacterDatabase.CommitTransaction(trans);

    LOG_INFO("module", "[WowPsParty] Account-bank withdraw: {} pulled item {} ({}) from offline char {} ({})",
        requester->GetName(), itemGuidLow, itemName, ownerName.empty() ? "?" : ownerName, ownerGuidLow);

    ChatHandler(requester->GetSession()).PSendSysMessage(
        "|cff66ccff[WowPsParty]|r Withdrew |cffffffff{}|r from {}'s bank.",
        itemName, ownerName.empty() ? "another character" : ownerName);

    WowPsParty::SendInventoryTo(requester);
    SendBankTo(requester);
    return true;
}

// WITHDRAW\t<bankItemGuidLow> — pull one item OUT of the bank back into the requester's own
// bags. If the item is in the requester's OWN live bank this is the canonical same-player
// move (RemoveItem then StoreItem); otherwise it belongs to another (offline) account
// character and HandleOfflineBankWithdraw does the cross-char DB move (account-wide bank).
// Validates the item is actually in a bank slot so this can't teleport a worn/bagged item.
static void HandleWithdraw(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    uint32 const itemGuidLow = std::strtoul(std::string(payload).c_str(), nullptr, 10);
    if (!itemGuidLow) return;

    Item* item = SafeGetItemByGuid(requester, ObjectGuid::Create<HighGuid::Item>(itemGuidLow));
    if (!item || !Player::IsBankPos(item->GetPos()))
    {
        // Not in the requester's own live bank — it may be banked on another (offline)
        // character on the account. Route there before reporting it gone.
        if (HandleOfflineBankWithdraw(requester, itemGuidLow))
            return;
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r That item is no longer in your bank.");
        WowPsParty::SendInventoryTo(requester);
        SendBankTo(requester);
        return;
    }
    ItemTemplate const* t = item->GetTemplate();
    std::string const itemName = t ? t->Name1 : "item";

    ItemPosCountVec dest;
    if (requester->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Your bags are full — couldn't withdraw |cffffffff{}|r.", itemName);
        return;
    }
    // RemoveItem BEFORE StoreItem (same double-reference hazard HandleBankDeposit guards).
    requester->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
    requester->ItemAddedQuestCheck(item->GetEntry(), item->GetCount());
    requester->StoreItem(dest, item, true);
    ChatHandler(requester->GetSession()).PSendSysMessage(
        "|cff66ccff[WowPsParty]|r Withdrew |cffffffff{}|r from your bank.", itemName);

    WowPsParty::SendInventoryTo(requester);
    SendBankTo(requester);
}

// The permanent-enchant id a spell applies (SPELL_EFFECT_ENCHANT_ITEM MiscValue),
// or 0 if the spell isn't a permanent item-enchant. Used to list/apply enchants.
static uint32 PermEnchantIdOfSpell(SpellInfo const* spell)
{
    if (!spell) return 0;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (spell->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM)
            return uint32(spell->Effects[i].MiscValue);
    return 0;
}

// An item's ON-USE spell id if that spell is a permanent item-enchant (i.e. the
// item is an ENCHANT SCROLL), else 0. The scroll's on-use spell IS the enchant
// spell, so PermEnchantIdOfSpell(it) gives the enchant — and the addon picker can
// resolve the spell id to a name like a known enchant.
static uint32 EnchantScrollUseSpell(ItemTemplate const* t)
{
    if (!t) return 0;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        if (t->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE && t->Spells[i].SpellId > 0)
        {
            uint32 const sp = uint32(t->Spells[i].SpellId);
            return PermEnchantIdOfSpell(sSpellMgr->GetSpellInfo(sp)) ? sp : 0;
            // first on-use spell only — scrolls carry exactly one
        }
    return 0;
}

// Visit every ENCHANT SCROLL in any loaded party member's bags (backpack + the 4
// equippable bags), calling fn(useSpellId, scrollItem, owner). Used to list scroll
// enchants in REQ_ENCHANTS and to find + consume the scroll in HandleEnchant, so a
// scroll is a first-class enchant source exactly like a known enchant spell.
template <class Fn>
static void ForEachPartyEnchantScroll(uint32 account, Fn&& fn)
{
    auto scan = [&](Player* p, Item* it)
    {
        if (!it) return;
        if (uint32 sp = EnchantScrollUseSpell(WowPsParty::SafeItemTemplate(it)))
            fn(sp, it, p);
    };
    for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
    {
        uint32 const guid = WowPsParty::GuidForAccountSlot(account, partySlot);
        if (!guid) continue;
        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        if (!p) continue;
        if (WowPsParty::MemberStorageUnstable(p)) continue;
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            scan(p, p->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    scan(p, p->GetItemByPos(b, uint8(j)));
    }
}

// Enchanting skill-up thresholds for an enchant spell, from its SkillLineAbility
// row. False when the spell isn't taught by the Enchanting skill line.
static bool EnchantTrivialRanks(uint32 spellId, uint32& low, uint32& high)
{
    SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
    for (SkillLineAbilityMap::const_iterator it = bounds.first; it != bounds.second; ++it)
    {
        SkillLineAbilityEntry const* ability = it->second;
        if (!ability || ability->SkillLine != SKILL_ENCHANTING)
            continue;
        low  = ability->TrivialSkillLineRankLow;
        high = ability->TrivialSkillLineRankHigh;
        return true;
    }
    return false;
}

// Skill-up odds for THIS enchanter casting THIS enchant, as the recipe-colour
// tier UpdateCraftSkill's SkillGainChance rolls with: 3=orange, 2=yellow,
// 1=green, 0=grey/none. Also 0 when the skill sits at its trained cap —
// UpdateSkillPro can't raise a maxed skill, whatever the recipe colour.
static uint8 EnchantSkillUpTier(Player* enchanter, uint32 spellId)
{
    if (!enchanter)
        return 0;
    uint32 low = 0, high = 0;
    if (!EnchantTrivialRanks(spellId, low, high))
        return 0;
    uint32 const skill = enchanter->GetPureSkillValue(SKILL_ENCHANTING);
    if (!skill || skill >= enchanter->GetPureMaxSkillValue(SKILL_ENCHANTING))
        return 0;
    if (skill >= high)             return 0;
    if (skill >= (high + low) / 2) return 1;
    if (skill >= low)              return 2;
    return 3;
}

// The percent chance UpdateCraftSkill's roll gives that tier (world config).
static uint32 EnchantSkillUpChancePct(uint8 tier)
{
    uint32 pct = 0;
    switch (tier)
    {
        case 3: pct = sWorld->getIntConfig(CONFIG_SKILL_CHANCE_ORANGE); break;
        case 2: pct = sWorld->getIntConfig(CONFIG_SKILL_CHANCE_YELLOW); break;
        case 1: pct = sWorld->getIntConfig(CONFIG_SKILL_CHANCE_GREEN);  break;
        default: break;
    }
    return std::min<uint32>(pct, 100);
}

// The loaded party member HandleEnchant picks to cast an enchant: anyone who
// knows it, preferring a caster who can still gain a skill point from it, then
// the lowest-skill one — so casts level the party's weakest enchanter first.
// Shared with REQ_ENCHANTS so the picker's skill-up preview names the same
// caster the apply will use.
static Player* PickPartyEnchanter(uint32 account, uint32 enchantSpellId)
{
    Player* enchanter = nullptr;
    for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
    {
        uint32 const guid = WowPsParty::GuidForAccountSlot(account, partySlot);
        if (!guid) continue;
        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        if (!p || !p->HasSpell(enchantSpellId)) continue;
        if (WowPsParty::MemberStorageUnstable(p)) continue;
        bool const pCanGain       = EnchantSkillUpTier(p, enchantSpellId) > 0;
        bool const currentCanGain = EnchantSkillUpTier(enchanter, enchantSpellId) > 0;
        if (!enchanter || (pCanGain && !currentCanGain)
            || (pCanGain && currentCanGain
                && p->GetPureSkillValue(SKILL_ENCHANTING)
                    < enchanter->GetPureSkillValue(SKILL_ENCHANTING)))
            enchanter = p;
    }
    return enchanter;
}

// REQ_ENCHANTS\t<targetPartySlot>\t<targetItemGuidLow> — reply with the enchant
// spell ids any loaded party member knows that FIT that specific item (so the
// picker only ever shows valid choices for what was clicked), each carrying a
// skill-up preview for the caster HandleEnchant would pick. Streamed like the
// inventory (the 3.3.5a client silently DROPS oversized addon messages):
//   ENCH_BEGIN <slot>\t<guid>                  -> client resets
//   ENCHANTS   <slot>\t<guid>\t<rec>,<rec>,…  *N -> client APPENDS each
//   ENCH_END   <slot>\t<guid>                  -> client sorts + renders
//   rec = spellId:tier:chancePct:casterSkill:casterName
//   tier: 3=orange 2=yellow 1=green 0=no gain; empty casterName = scroll-only.
static void HandleReqEnchants(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const tgtSlot        = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const tgtItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);

    uint32 const account = requester->GetSession()->GetAccountId();
    uint32 const tgtGuid = WowPsParty::GuidForAccountSlot(account, tgtSlot);
    if (!tgtGuid) return;
    Player* tgtChar = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(tgtGuid));
    if (!tgtChar) return;
    if (WowPsParty::MemberStorageUnstable(tgtChar)) return;
    Item* tgtItem = SafeGetItemByGuid(tgtChar, ObjectGuid::Create<HighGuid::Item>(tgtItemGuidLow));
    if (!tgtItem) return;

    std::vector<uint32> spellIds;
    std::unordered_set<uint32> seen;
    for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
    {
        uint32 const guid = WowPsParty::GuidForAccountSlot(account, partySlot);
        if (!guid) continue;
        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        if (!p) continue;
        if (WowPsParty::MemberStorageUnstable(p)) continue;
        for (auto const& kv : p->GetSpellMap())
        {
            if (kv.second->State == PLAYERSPELL_REMOVED) continue;
            uint32 const spellId = kv.first;
            if (seen.count(spellId)) continue;
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
            if (!PermEnchantIdOfSpell(spell)) continue;
            if (!tgtItem->IsFitToSpellRequirements(spell)) continue;
            seen.insert(spellId);
            spellIds.push_back(spellId);
        }
    }

    // Also list enchant SCROLLS in the party's bags whose enchant FITS the item, so
    // the user can apply a scroll the same way as a known enchant (HandleEnchant
    // resolves the spell id back to the scroll and consumes it). Deduped against the
    // known-spell list by spell id. A scroll apply is not a cast, so it can never
    // skill anyone up — remember which ids are scroll-only for the preview below.
    std::unordered_set<uint32> scrollOnly;
    ForEachPartyEnchantScroll(account, [&](uint32 sp, Item* /*scroll*/, Player* /*owner*/)
    {
        if (seen.count(sp)) return;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(sp);
        if (!spell || !tgtItem->IsFitToSpellRequirements(spell)) return;
        seen.insert(sp);
        spellIds.push_back(sp);
        scrollOnly.insert(sp);
    });

    // rec = spellId:tier:chancePct:casterSkill:casterName — the skill-up preview
    // for the caster HandleEnchant would pick right now.
    std::vector<std::string> records;
    uint32 skillUpCount = 0;
    for (uint32 spellId : spellIds)
    {
        Player* enchanter = scrollOnly.count(spellId) ? nullptr : PickPartyEnchanter(account, spellId);
        uint8 const tier = EnchantSkillUpTier(enchanter, spellId);
        if (tier) ++skillUpCount;
        std::ostringstream rec;
        rec << spellId << ':' << uint32(tier) << ':' << EnchantSkillUpChancePct(tier) << ':'
            << (enchanter ? enchanter->GetPureSkillValue(SKILL_ENCHANTING) : 0u) << ':'
            << (enchanter ? enchanter->GetName() : "");
        records.push_back(rec.str());
    }

    // DIAGNOSTIC ("can't enchant" report): how many applicable enchants the party
    // knows for this item. 0 here = the picker is empty because NO party member is an
    // enchanter (or none fit the item) — that's the usual "can't enchant", not a bug.
    ItemTemplate const* tgtProto = WowPsParty::SafeItemTemplate(tgtItem);
    LOG_INFO("module",
        "[WowPsParty Enchant] REQ_ENCHANTS by {} for item entry={} -> {} applicable enchant(s) across the party, {} would skill up the caster",
        requester->GetName(), tgtProto ? tgtProto->ItemId : 0u, uint32(spellIds.size()), skillUpCount);

    // CHUNKED send (mirrors SendInventoryTo): one message per ~MAX_PAYLOAD bytes
    // of records, records never split, framed by ENCH_BEGIN/ENCH_END.
    std::string hdr;
    { std::ostringstream h; h << tgtSlot << '\t' << tgtItemGuidLow; hdr = h.str(); }
    SendWPSP(requester, "ENCH_BEGIN\t" + hdr);
    constexpr size_t MAX_PAYLOAD = 220;
    std::string chunk;
    auto flush = [&]()
    {
        if (!chunk.empty()) { SendWPSP(requester, "ENCHANTS\t" + hdr + '\t' + chunk); chunk.clear(); }
    };
    for (std::string const& rec : records)
    {
        if (!chunk.empty() && chunk.size() + 1 + rec.size() > MAX_PAYLOAD)
            flush();
        if (!chunk.empty()) chunk += ',';
        chunk += rec;
    }
    flush();
    SendWPSP(requester, "ENCH_END\t" + hdr);
}

// ENCHANT\t<targetPartySlot>\t<targetItemGuidLow>\t<enchantSpellId> — apply the
// permanent enchant to a shared item (equipped or bagged) owned by any member.
// A party member who knows the enchant is the caster; reagents come from the
// shared pool. We apply directly (no spell cast) so it works cross-character.
static void HandleEnchant(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    std::string s(payload);
    auto t1 = s.find('\t');               if (t1 == std::string::npos) return;
    auto t2 = s.find('\t', t1 + 1);       if (t2 == std::string::npos) return;
    uint32 const tgtSlot        = std::strtoul(s.substr(0, t1).c_str(), nullptr, 10);
    uint32 const tgtItemGuidLow = std::strtoul(s.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
    uint32 const enchantSpellId = std::strtoul(s.substr(t2 + 1).c_str(), nullptr, 10);

    uint32 const account = requester->GetSession()->GetAccountId();
    uint32 const tgtGuid = WowPsParty::GuidForAccountSlot(account, tgtSlot);
    if (!tgtGuid) return;
    Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(tgtGuid));
    if (!owner) return;
    if (WowPsParty::MemberStorageUnstable(owner)) return;  // mid-logout: don't mutate a tearing-down inventory
    Item* item = SafeGetItemByGuid(owner, ObjectGuid::Create<HighGuid::Item>(tgtItemGuidLow));
    if (!item) return;

    SpellInfo const* spell = sSpellMgr->GetSpellInfo(enchantSpellId);
    uint32 const enchantId = PermEnchantIdOfSpell(spell);
    if (!enchantId || !sSpellItemEnchantmentStore.LookupEntry(enchantId))
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r That isn't a usable enchant.");
        return;
    }
    if (!item->IsFitToSpellRequirements(spell))
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r That enchant doesn't fit |cffffffff{}|r.",
            item->GetTemplate() ? item->GetTemplate()->Name1 : "that item");
        return;
    }

    // Find a loaded party member who actually knows the enchant — the caster.
    Player* enchanter = PickPartyEnchanter(account, enchantSpellId);
    // If nobody KNOWS the enchant, an enchant SCROLL in the party's bags can supply
    // it — the scroll IS the cost (no reagents) and is consumed on a successful apply.
    Item*   scroll      = nullptr;
    Player* scrollOwner = nullptr;
    if (!enchanter)
        ForEachPartyEnchantScroll(account, [&](uint32 sp, Item* it, Player* p)
        {
            if (!scroll && sp == enchantSpellId) { scroll = it; scrollOwner = p; }
        });

    if (!enchanter && !scroll)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r No-one knows that enchant and there's no scroll for it.");
        return;
    }

    // Reagents apply ONLY to a known-spell caster — a scroll carries its own cost.
    // Check the shared pool covers ALL of them before consuming any.
    if (enchanter)
    {
        extern uint32 WowPsParty_PartyReagentCount(Player*, uint32);
        extern void   WowPsParty_TakeReagent(Player*, uint32, uint32);
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
        {
            if (spell->Reagent[r] <= 0) continue;
            if (WowPsParty_PartyReagentCount(enchanter, uint32(spell->Reagent[r])) < uint32(spell->ReagentCount[r]))
            {
                ChatHandler(requester->GetSession()).PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Missing reagents for that enchant.");
                return;
            }
        }
        for (uint8 r = 0; r < MAX_SPELL_REAGENTS; ++r)
            if (spell->Reagent[r] > 0)
                WowPsParty_TakeReagent(enchanter, uint32(spell->Reagent[r]), uint32(spell->ReagentCount[r]));
    }

    // Apply on the item's OWNER so equipped stats take effect (no-op for a bagged
    // item until equipped). Remove the old enchant first, then set + reapply.
    ObjectGuid const casterGuid = enchanter ? enchanter->GetGUID() : scrollOwner->GetGUID();
    owner->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, false);
    item->SetEnchantment(PERM_ENCHANTMENT_SLOT, enchantId, 0, 0, casterGuid);
    owner->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, true);
    item->SetState(ITEM_CHANGED, owner);

    // Consume the scroll (its cost) AFTER the enchant landed, so a failed apply never
    // eats it — the exact dupe-loss the direct right-click bug caused.
    std::string const itemName = item->GetTemplate() ? item->GetTemplate()->Name1 : "item";
    if (scroll)
    {
        if (scroll->GetCount() > 1)
        {
            scroll->SetCount(scroll->GetCount() - 1);
            scroll->SetState(ITEM_CHANGED, scrollOwner);
        }
        else
            scrollOwner->DestroyItem(scroll->GetBagSlot(), scroll->GetSlot(), true);
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Enchanted |cffffffff{}|r ({}'s) with a scroll.",
            itemName, owner->GetName());
    }
    else
    {
        enchanter->UpdateCraftSkill(enchantSpellId);
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r {} enchanted |cffffffff{}|r ({}'s).",
            enchanter->GetName(), itemName, owner->GetName());
    }

    WowPsParty::SendGearTo(requester, tgtSlot);
    WowPsParty::SendStatsTo(requester, tgtSlot);
    WowPsParty::SendInventoryTo(requester);
}

// RUNEFORGE\t<partySlot>\t<itemGuidLow>\t<runeSpellId> — apply a Death Knight runeforging
// rune to a weapon in the party inventory. Unlike ENCHANT (which SetEnchantment-applies the
// rune directly and therefore can NOT credit the runeforging quest 12842), this makes the
// weapon's OWNER actually CAST the rune, so the real runeforging path runs — applying the
// enchant AND crediting the quest when the owner is standing at a runeforge. Runeforging is
// self-only, so the owner must be the one who knows the rune. If the real cast can't go (not
// at a runeforge / a clientless bot), it falls back to a direct enchant so a bot's weapon
// still gets the rune (bots have no quest to credit anyway).
static void HandleRuneforge(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    std::string s(payload);
    auto t1 = s.find('\t');           if (t1 == std::string::npos) return;
    auto t2 = s.find('\t', t1 + 1);   if (t2 == std::string::npos) return;
    uint32 const tgtSlot     = std::strtoul(s.substr(0, t1).c_str(), nullptr, 10);
    uint32 const itemGuidLow = std::strtoul(s.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
    std::string runeName(s.substr(t2 + 1));   // the SELECTED recipe NAME (may contain spaces)
    { auto a = runeName.find_first_not_of(" \t"); auto b = runeName.find_last_not_of(" \t");
      runeName = (a == std::string::npos) ? std::string() : runeName.substr(a, b - a + 1); }
    if (!itemGuidLow || runeName.empty()) return;

    uint32 const account = requester->GetSession()->GetAccountId();
    uint32 const tgtGuid = WowPsParty::GuidForAccountSlot(account, tgtSlot);
    if (!tgtGuid) return;
    Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(tgtGuid));
    if (!owner) return;
    if (WowPsParty::MemberStorageUnstable(owner)) return;
    Item* item = SafeGetItemByGuid(owner, ObjectGuid::Create<HighGuid::Item>(itemGuidLow));
    if (!item) return;

    ChatHandler ch(requester->GetSession());

    // The runeforge UI is TradeSkill-driven and gives the client only a NAME, so resolve it
    // to one of the OWNER's known runeforging runes — a known spell with a permanent-enchant
    // effect whose name matches. This also enforces runeforging's self-only rule: it only
    // works on the owner's own weapon, by a rune that owner has actually learned.
    auto lower = [](std::string v){ std::transform(v.begin(), v.end(), v.begin(), ::tolower); return v; };
    std::string const want = lower(runeName);
    uint32 runeSpellId = 0;
    for (auto const& kv : owner->GetSpellMap())
    {
        uint32 const sid = kv.first;
        if (!owner->HasSpell(sid)) continue;
        SpellInfo const* si = sSpellMgr->GetSpellInfo(sid);
        if (!si || !PermEnchantIdOfSpell(si)) continue;     // not a rune / enchant spell
        for (uint8 loc = 0; loc < 16; ++loc)
            if (char const* nm = si->SpellName[loc])
                if (*nm && lower(nm) == want) { runeSpellId = sid; break; }
        if (runeSpellId) break;
    }
    if (!runeSpellId)
    {
        ch.PSendSysMessage(
            "|cffff5555[WowPsParty]|r |cffffffff{}|r's owner hasn't learned the rune \"{}\" — "
            "log in that hero and train Runeforging / that rune.",
            item->GetTemplate() ? item->GetTemplate()->Name1 : "that weapon", runeName);
        return;
    }

    SpellInfo const* rune = sSpellMgr->GetSpellInfo(runeSpellId);
    uint32 const enchantId = PermEnchantIdOfSpell(rune);
    if (!rune || !enchantId || !sSpellItemEnchantmentStore.LookupEntry(enchantId))
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r That isn't a runeforging rune.");
        return;
    }
    if (!item->IsFitToSpellRequirements(rune))
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r That rune doesn't fit |cffffffff{}|r.",
            item->GetTemplate() ? item->GetTemplate()->Name1 : "that weapon");
        return;
    }

    std::string const itemName = item->GetTemplate() ? item->GetTemplate()->Name1 : "weapon";

    // Real cast first — full CheckCast (needs the owner to be AT a runeforge), which is the
    // path that both applies the rune AND fires the runeforging quest credit.
    SpellCastTargets targets;
    targets.SetItemTarget(item);
    SpellCastResult const res = owner->CastSpell(targets, rune, nullptr, TRIGGERED_NONE);
    if (res == SPELL_CAST_OK)
    {
        ch.PSendSysMessage("|cff66ccff[WowPsParty]|r {} runeforged |cffffffff{}|r.",
            owner->GetName(), itemName);
        LOG_INFO("module", "[WowPsParty Runeforge] requester={} owner={} item='{}' rune={} -> real cast (quest-crediting)",
            requester->GetGUID().GetCounter(), owner->GetGUID().GetCounter(), itemName, runeSpellId);
    }
    else
    {
        // Fallback: the owner can't cast it right now (not at a runeforge / a bot) — apply the
        // rune enchant directly so a bot's weapon still gets it. No quest credit on this path.
        owner->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, false);
        item->SetEnchantment(PERM_ENCHANTMENT_SLOT, enchantId, 0, 0, owner->GetGUID());
        owner->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, true);
        item->SetState(ITEM_CHANGED, owner);
        ch.PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Runeforged |cffffffff{}|r (applied directly — stand at a "
            "runeforge for quest credit).", itemName);
        LOG_INFO("module", "[WowPsParty Runeforge] requester={} owner={} item='{}' rune={} -> direct apply (cast res={})",
            requester->GetGUID().GetCounter(), owner->GetGUID().GetCounter(), itemName, runeSpellId, uint32(res));
    }

    WowPsParty::SendGearTo(requester, tgtSlot);
    WowPsParty::SendStatsTo(requester, tgtSlot);
    WowPsParty::SendInventoryTo(requester);
}

// Does a gem of GemColor fit a socket of SocketColor? Mirrors AC's native rules
// (HandleSocketOpcode / Item::GemsFitSockets): a META socket takes ONLY a meta gem
// and vice-versa; otherwise a gem fits if its color bitmask-overlaps the socket's.
static bool WowPsParty_GemColorFitsSocket(uint8 gemColor, uint8 socketColor)
{
    if (!socketColor) return false;
    if (socketColor == SOCKET_COLOR_META || gemColor == SOCKET_COLOR_META)
        return socketColor == SOCKET_COLOR_META && gemColor == SOCKET_COLOR_META;
    return (gemColor & socketColor) != 0;
}

// Find ONE gem item of <gemEntry> in this player's bags (backpack + the 4 bags).
// Returns the Item* (caller consumes it), or nullptr if none. Confirms the item is
// actually a gem (proto Class == ITEM_CLASS_GEM) before returning.
static Item* WowPsParty_FindOwnGem(Player* p, uint32 gemEntry)
{
    if (!p || !gemEntry) return nullptr;
    auto check = [&](Item* it) -> Item*
    {
        if (!it) return nullptr;
        ItemTemplate const* proto = WowPsParty::SafeItemTemplate(it);
        if (!proto || proto->ItemId != gemEntry || proto->Class != ITEM_CLASS_GEM) return nullptr;
        return it;
    };
    for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        if (Item* hit = check(p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))) return hit;
    for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        if (Bag* bag = p->GetBagByPos(b))
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                if (Item* hit = check(p->GetItemByPos(b, uint8(j)))) return hit;
    return nullptr;
}

// Find ONE <gemEntry> gem anywhere in the connected party's bags (the SHARED pool),
// returning the owning member in `outOwner`. Party inventory is pooled across all
// members, so a gem on ANY member is usable — the requester needn't personally carry
// it (the bot alts hold most of the loot).
static Item* WowPsParty_FindPartyGem(uint32 account, uint32 gemEntry, Player*& outOwner)
{
    outOwner = nullptr;
    if (!gemEntry) return nullptr;
    for (uint8 slot = 0; slot < WowPsParty::PARTY_SIZE; ++slot)
    {
        uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
        if (!guid) continue;
        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        if (!p || WowPsParty::MemberStorageUnstable(p)) continue;
        if (Item* hit = WowPsParty_FindOwnGem(p, gemEntry)) { outOwner = p; return hit; }
    }
    return nullptr;
}

// REQ_GEMS\t<targetPartySlot>\t<targetItemGuidLow> — reply with GEMS, the target
// item's 3 socket colors and the list of gems in the REQUESTER's OWN bags whose
// color fits at least one of those sockets (so the picker only shows gems the user
// owns AND that can actually go in this item). Distinct gem entries only.
static void HandleReqGems(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const tgtSlot        = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const tgtItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);

    uint32 const account = requester->GetSession()->GetAccountId();
    uint32 const tgtGuid = WowPsParty::GuidForAccountSlot(account, tgtSlot);
    if (!tgtGuid) return;
    Player* tgtChar = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(tgtGuid));
    if (!tgtChar) return;
    if (WowPsParty::MemberStorageUnstable(tgtChar)) return;
    Item* tgtItem = SafeGetItemByGuid(tgtChar, ObjectGuid::Create<HighGuid::Item>(tgtItemGuidLow));
    if (!tgtItem) return;
    ItemTemplate const* tgtProto = WowPsParty::SafeItemTemplate(tgtItem);
    if (!tgtProto) return;

    uint8 sockColors[MAX_GEM_SOCKETS];
    for (uint8 i = 0; i < MAX_GEM_SOCKETS; ++i)
        sockColors[i] = uint8(tgtProto->Socket[i].Color);

    // Collect every distinct gem ITEM in the SHARED PARTY pool whose color fits at least
    // one real socket — party inventory is pooled, so a gem on ANY member (usually a bot
    // alt, which holds most loot) counts, not just the requester's own bags.
    std::vector<std::pair<uint32, uint8>> gems; // (gemEntry, gemColor)
    std::unordered_set<uint32> seen;
    auto consider = [&](Item* it)
    {
        if (!it) return;
        ItemTemplate const* proto = WowPsParty::SafeItemTemplate(it);
        if (!proto || proto->Class != ITEM_CLASS_GEM || !proto->GemProperties) return;
        if (seen.count(proto->ItemId)) return;
        GemPropertiesEntry const* gp = sGemPropertiesStore.LookupEntry(proto->GemProperties);
        if (!gp) return;
        bool fits = false;
        for (uint8 i = 0; i < MAX_GEM_SOCKETS && !fits; ++i)
            if (WowPsParty_GemColorFitsSocket(uint8(gp->color), sockColors[i]))
                fits = true;
        if (!fits) return;
        seen.insert(proto->ItemId);
        gems.emplace_back(proto->ItemId, uint8(gp->color));
    };
    auto scanPlayer = [&](Player* p)
    {
        if (!p || WowPsParty::MemberStorageUnstable(p)) return;
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            consider(p->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    consider(p->GetItemByPos(b, uint8(j)));
    };
    for (uint8 slot = 0; slot < WowPsParty::PARTY_SIZE; ++slot)
    {
        uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
        if (!guid) continue;
        scanPlayer(ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid)));
    }

    // DIAGNOSTIC: fitting gems found across the whole party pool. 0 with real sockets =
    // nobody in the party owns a fitting gem, not a bug.
    uint32 const sockCount = (sockColors[0] ? 1u : 0u) + (sockColors[1] ? 1u : 0u) + (sockColors[2] ? 1u : 0u);
    LOG_INFO("module",
        "[WowPsParty Gem] REQ_GEMS by {} for item entry={} ({} socket(s)) -> {} fitting gem(s) in the party pool",
        requester->GetName(), tgtProto->ItemId, sockCount, uint32(gems.size()));

    std::ostringstream out;
    out << "GEMS\t" << tgtSlot << '\t' << tgtItemGuidLow << '\t'
        << uint32(sockColors[0]) << ',' << uint32(sockColors[1]) << ',' << uint32(sockColors[2]) << '\t';
    for (size_t i = 0; i < gems.size(); ++i)
        out << (i ? ";" : "") << gems[i].first << ':' << uint32(gems[i].second);
    SendWPSP(requester, out.str());
}

// GEM\t<targetPartySlot>\t<targetItemGuidLow>\t<socketIndex 0-2>\t<gemEntry> — socket
// one of the REQUESTER's OWN gems into a shared bot item. Validates the socket exists
// and the gem color fits (native rules), applies on the OWNER, recomputes the socket
// bonus exactly like the native handler, then consumes ONE gem AFTER the apply landed
// (so a failed apply can never eat a gem — same dupe-safety the enchant flow has).
static void HandleGem(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    std::string s(payload);
    auto t1 = s.find('\t');               if (t1 == std::string::npos) return;
    auto t2 = s.find('\t', t1 + 1);       if (t2 == std::string::npos) return;
    auto t3 = s.find('\t', t2 + 1);       if (t3 == std::string::npos) return;
    uint32 const tgtSlot        = std::strtoul(s.substr(0, t1).c_str(), nullptr, 10);
    uint32 const tgtItemGuidLow = std::strtoul(s.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
    uint32 const socketIndex    = std::strtoul(s.substr(t2 + 1, t3 - t2 - 1).c_str(), nullptr, 10);
    uint32 const gemEntry       = std::strtoul(s.substr(t3 + 1).c_str(), nullptr, 10);

    ChatHandler ch(requester->GetSession());
    if (socketIndex >= MAX_GEM_SOCKETS)
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r Invalid gem socket.");
        return;
    }

    uint32 const account = requester->GetSession()->GetAccountId();
    uint32 const tgtGuid = WowPsParty::GuidForAccountSlot(account, tgtSlot);
    if (!tgtGuid) return;
    Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(tgtGuid));
    if (!owner) return;
    if (WowPsParty::MemberStorageUnstable(owner)) return;  // mid-logout: don't mutate a tearing-down inventory
    Item* item = SafeGetItemByGuid(owner, ObjectGuid::Create<HighGuid::Item>(tgtItemGuidLow));
    if (!item) return;
    ItemTemplate const* proto = WowPsParty::SafeItemTemplate(item);
    if (!proto) return;

    uint8 const socketColor = uint8(proto->Socket[socketIndex].Color);
    if (!socketColor)
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r That item has no socket there.");
        return;
    }

    // The gem may sit in ANY party member's bags (shared pool) — find it and the member
    // who holds it; we consume it from THAT member, not necessarily the requester.
    Player* gemOwner = nullptr;
    Item* gemItem = WowPsParty_FindPartyGem(account, gemEntry, gemOwner);
    if (!gemItem || !gemOwner)
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r Nobody in the party has that gem.");
        return;
    }
    ItemTemplate const* gemProto = WowPsParty::SafeItemTemplate(gemItem);
    GemPropertiesEntry const* gemProps = gemProto ? sGemPropertiesStore.LookupEntry(gemProto->GemProperties) : nullptr;
    if (!gemProps || !gemProps->spellitemenchantement || !sSpellItemEnchantmentStore.LookupEntry(gemProps->spellitemenchantement))
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r That isn't a usable gem.");
        return;
    }
    if (!WowPsParty_GemColorFitsSocket(uint8(gemProps->color), socketColor))
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r That gem's colour doesn't fit that socket.");
        return;
    }

    // Apply on the OWNER so equipped stats take effect. Remove the old gem enchant in
    // this socket, set the new one, reapply — mirrors the native socket handler.
    EnchantmentSlot const eslot = EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + socketIndex);
    bool const bonusWas = item->GemsFitSockets();
    owner->ApplyEnchantment(item, eslot, false);
    item->SetEnchantment(eslot, gemProps->spellitemenchantement, 0, 0, requester->GetGUID());
    owner->ApplyEnchantment(item, eslot, true);

    // Recompute the socket bonus exactly like HandleSocketOpcode (~ItemHandler 1387):
    // if whether ALL sockets are now satisfied flipped, apply or remove the item's
    // socketBonus enchant in the BONUS slot.
    bool const bonusNow = item->GemsFitSockets();
    if (bonusWas ^ bonusNow)
    {
        owner->ApplyEnchantment(item, BONUS_ENCHANTMENT_SLOT, false);
        item->SetEnchantment(BONUS_ENCHANTMENT_SLOT, (bonusNow ? proto->socketBonus : 0), 0, 0, requester->GetGUID());
        owner->ApplyEnchantment(item, BONUS_ENCHANTMENT_SLOT, true);
    }
    item->SetState(ITEM_CHANGED, owner);
    item->SendUpdateSockets();

    // Consume ONE gem AFTER the socket landed — a failed apply above returned early and
    // never reached here, so a gem is never eaten for a failed socket.
    std::string const gemName = gemProto ? gemProto->Name1 : "gem";
    std::string const itemName = proto->Name1;
    if (gemItem->GetCount() > 1)
    {
        gemItem->SetCount(gemItem->GetCount() - 1);
        gemItem->SetState(ITEM_CHANGED, gemOwner);
    }
    else
        gemOwner->DestroyItem(gemItem->GetBagSlot(), gemItem->GetSlot(), true);

    ch.PSendSysMessage("|cff66ccff[WowPsParty]|r Socketed |cffffffff{}|r into |cffffffff{}|r ({}'s).",
        gemName, itemName, owner->GetName());

    WowPsParty::SendGearTo(requester, tgtSlot);
    WowPsParty::SendStatsTo(requester, tgtSlot);
    WowPsParty::SendInventoryTo(requester);
}

// DISENCHANT\t<srcPartySlot>\t<srcItemGuidLow> — disenchant a bagged item from any
// party member into enchanting mats. A party member with the Enchanting skill
// (>= the item's RequiredDisenchantSkill) is the disenchanter; the rolled mats
// drop into the party's bags and the item is consumed. Rolls the SAME table the
// Disenchant spell uses (LootTemplates_Disenchant), but directly — a clientless
// bot enchanter can't drive the core's disenchant loot WINDOW.
static void HandleDisenchant(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const srcSlot        = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);
    if (!srcItemGuidLow) return;

    uint32 const account   = requester->GetSession()->GetAccountId();
    uint32 const ownerGuid = WowPsParty::GuidForAccountSlot(account, srcSlot);
    if (!ownerGuid) return;
    Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(ownerGuid));
    if (!owner) return;
    Item* item = SafeGetItemByGuid(owner, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!item) return;
    ItemTemplate const* tmpl = item->GetTemplate();
    if (!tmpl) return;

    ChatHandler ch(requester->GetSession());
    if (item->IsEquipped() || item->IsNotEmptyBag())
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r Unequip / empty it before disenchanting.");
        return;
    }
    if (tmpl->DisenchantID == 0)
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r |cffffffff{}|r can't be disenchanted.", tmpl->Name1);
        return;
    }

    // A party member whose Enchanting skill covers this item's required level.
    // Record a per-slot diagnostic as we go so a FAILURE is explainable from the
    // log — the common transient cause is a member momentarily not connected (zoning/
    // loading) or skills not yet loaded, which reads as "no enchanter skilled enough"
    // even when the player IS over the requirement (Kevin's mum, 2026-06-16: needed
    // 25, was 100+, failed once then worked). Without this, that path logged nothing.
    Player* disenchanter = nullptr;
    std::ostringstream diag;
    for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
    {
        uint32 const g = WowPsParty::GuidForAccountSlot(account, partySlot);
        if (!g) { diag << " [slot" << uint32(partySlot) << ":empty]"; continue; }
        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g));
        if (!p)
        {
            diag << " [slot" << uint32(partySlot) << ":guid" << g << ":NOT-CONNECTED]";
            continue;
        }
        bool const hasSkill = p->HasSkill(SKILL_ENCHANTING);
        uint16 const skill  = hasSkill ? p->GetSkillValue(SKILL_ENCHANTING) : 0;
        diag << " [slot" << uint32(partySlot) << ":" << p->GetName()
             << ":ench=" << (hasSkill ? std::to_string(skill) : std::string("none")) << "]";
        if (!disenchanter && hasSkill && skill >= tmpl->RequiredDisenchantSkill)
            disenchanter = p;
    }
    if (!disenchanter)
    {
        LOG_WARN("module",
            "[WowPsParty Disenchant] FAILED 'no enchanter skilled enough' — requester={} account={} "
            "item='{}'(entry={}) requiredSkill={} | party:{}",
            requester->GetName(), account, tmpl->Name1, tmpl->ItemId,
            tmpl->RequiredDisenchantSkill, diag.str());
        ch.PSendSysMessage(
            "|cffff5555[WowPsParty]|r No party enchanter skilled enough to disenchant "
            "|cffffffff{}|r (needs Enchanting {}).", tmpl->Name1, tmpl->RequiredDisenchantSkill);
        return;
    }

    // Roll the disenchant loot directly (same table as the spell).
    Loot loot;
    loot.FillLoot(tmpl->DisenchantID, LootTemplates_Disenchant, owner, true, true);
    std::vector<std::pair<uint32, uint32>> mats;
    for (LootItem const& li : loot.items)
        if (li.itemid && li.count)
            mats.emplace_back(li.itemid, uint32(li.count));
    if (mats.empty())
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r Disenchant produced nothing — item kept.");
        return;
    }

    // Disenchant is IRREVERSIBLE, so make sure the party can actually hold the
    // rolled mats BEFORE consuming the item — never destroy it and then drop a mat
    // because every bag was full. Count free slots across the party; the item's
    // own slot frees on destroy, so it covers one mat. Conservative (ignores
    // stacking onto a partial stack), which only ever errs toward "make room".
    auto freeSlots = [](Player* p) -> uint32
    {
        uint32 n = 0;
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (!p->GetItemByPos(INVENTORY_SLOT_BAG_0, i)) ++n;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (!p->GetItemByPos(b, uint8(j))) ++n;
        return n;
    };
    uint32 partyFree = 0;
    for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
    {
        uint32 const g = WowPsParty::GuidForAccountSlot(account, partySlot);
        if (!g) continue;
        if (Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g)))
            partyFree += freeSlots(p);
    }
    if (mats.size() > size_t(partyFree) + 1)   // +1 = the item's slot that frees
    {
        ch.PSendSysMessage(
            "|cffff5555[WowPsParty]|r Not enough free bag space for the disenchant "
            "mats — make room and try again. (Item kept.)");
        return;
    }

    std::string const itemName = tmpl->Name1;
    uint32 const itemEntry = tmpl->ItemId;
    uint32 const disenchantId = tmpl->DisenchantID;
    disenchanter->UpdateCraftSkill(13262 /* Disenchant */);   // skill-up chance
    owner->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);   // consume; frees a slot

    std::ostringstream gained;
    for (auto const& m : mats)
    {
        for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
        {
            uint32 const g = WowPsParty::GuidForAccountSlot(account, partySlot);
            if (!g) continue;
            Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g));
            if (!p) continue;
            ItemPosCountVec dest;
            if (p->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, m.first, m.second) != EQUIP_ERR_OK)
                continue;
            p->StoreNewItem(dest, m.first, true);
            ItemTemplate const* mt = sObjectMgr->GetItemTemplate(m.first);
            if (!gained.str().empty()) gained << ", ";
            gained << m.second << "x " << (mt ? mt->Name1 : "?");
            break;   // stored — next mat
        }
    }

    ch.PSendSysMessage("|cff66ccff[WowPsParty]|r {} disenchanted |cffffffff{}|r -> {}.",
        disenchanter->GetName(), itemName,
        gained.str().empty() ? "nothing (party bags full)" : gained.str());

    LOG_INFO("module",
        "[WowPsParty Disenchant] requester={} owner={} disenchanter={} item='{}'(entry={}) "
        "disenchantId={} -> {}",
        requester->GetGUID().GetCounter(), owner->GetGUID().GetCounter(),
        disenchanter->GetGUID().GetCounter(), itemName, itemEntry, disenchantId,
        gained.str().empty() ? "NOTHING (bags full)" : gained.str());

    WowPsParty::SendInventoryTo(requester);
}

// PROSPECT\t<srcPartySlot>\t<srcItemGuidLow> — prospect a stack of ore from any party
// member into gems. Mirrors HandleDisenchant: a party member with Jewelcrafting skill
// (>= the ore's RequiredSkillRank) is the prospector; one prospect consumes 5 ore and
// rolls the SAME table the Prospecting spell uses (LootTemplates_Prospecting), keyed by
// the ore's item entry — directly, since a clientless bot can't drive the loot WINDOW.
static void HandleProspect(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const srcSlot        = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);
    if (!srcItemGuidLow) return;

    uint32 const account   = requester->GetSession()->GetAccountId();
    uint32 const ownerGuid = WowPsParty::GuidForAccountSlot(account, srcSlot);
    if (!ownerGuid) return;
    Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(ownerGuid));
    if (!owner) return;
    Item* item = SafeGetItemByGuid(owner, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!item) return;
    ItemTemplate const* tmpl = item->GetTemplate();
    if (!tmpl) return;

    ChatHandler ch(requester->GetSession());
    if (!tmpl->HasFlag(ITEM_FLAG_IS_PROSPECTABLE))
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r |cffffffff{}|r can't be prospected.", tmpl->Name1);
        return;
    }
    if (item->GetCount() < 5)
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r Prospecting needs a stack of at least 5 |cffffffff{}|r.", tmpl->Name1);
        return;
    }

    // A party member whose Jewelcrafting skill covers this ore's required level.
    Player* prospector = nullptr;
    std::ostringstream diag;
    for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
    {
        uint32 const g = WowPsParty::GuidForAccountSlot(account, partySlot);
        if (!g) { diag << " [slot" << uint32(partySlot) << ":empty]"; continue; }
        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g));
        if (!p) { diag << " [slot" << uint32(partySlot) << ":guid" << g << ":NOT-CONNECTED]"; continue; }
        bool const hasSkill = p->HasSkill(SKILL_JEWELCRAFTING);
        uint16 const skill  = hasSkill ? p->GetSkillValue(SKILL_JEWELCRAFTING) : 0;
        diag << " [slot" << uint32(partySlot) << ":" << p->GetName()
             << ":jc=" << (hasSkill ? std::to_string(skill) : std::string("none")) << "]";
        if (!prospector && hasSkill && skill >= tmpl->RequiredSkillRank)
            prospector = p;
    }
    if (!prospector)
    {
        LOG_WARN("module",
            "[WowPsParty Prospect] FAILED 'no jeweler skilled enough' — requester={} account={} "
            "item='{}'(entry={}) requiredSkill={} | party:{}",
            requester->GetName(), account, tmpl->Name1, tmpl->ItemId, tmpl->RequiredSkillRank, diag.str());
        ch.PSendSysMessage(
            "|cffff5555[WowPsParty]|r No party jeweler skilled enough to prospect "
            "|cffffffff{}|r (needs Jewelcrafting {}).", tmpl->Name1, tmpl->RequiredSkillRank);
        return;
    }

    // Roll the prospecting loot directly (same table as the spell), keyed by the ore entry.
    Loot loot;
    loot.FillLoot(tmpl->ItemId, LootTemplates_Prospecting, owner, true, true);
    std::vector<std::pair<uint32, uint32>> gems;
    for (LootItem const& li : loot.items)
        if (li.itemid && li.count)
            gems.emplace_back(li.itemid, uint32(li.count));
    if (gems.empty())
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r Prospecting produced nothing — ore kept.");
        return;
    }

    // Make sure the party can hold the gems BEFORE consuming the ore (prospecting is
    // irreversible). Don't assume the 5-ore consume frees a slot — it only does if the
    // stack was exactly 5 — so require the gems to fit the CURRENT free space.
    auto freeSlots = [](Player* p) -> uint32
    {
        uint32 n = 0;
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (!p->GetItemByPos(INVENTORY_SLOT_BAG_0, i)) ++n;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (!p->GetItemByPos(b, uint8(j))) ++n;
        return n;
    };
    uint32 partyFree = 0;
    for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
    {
        uint32 const g = WowPsParty::GuidForAccountSlot(account, partySlot);
        if (!g) continue;
        if (Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g)))
            partyFree += freeSlots(p);
    }
    if (gems.size() > size_t(partyFree))
    {
        ch.PSendSysMessage(
            "|cffff5555[WowPsParty]|r Not enough free bag space for the gems — make room and try again. (Ore kept.)");
        return;
    }

    std::string const itemName = tmpl->Name1;
    uint32 const itemEntry = tmpl->ItemId;
    prospector->UpdateCraftSkill(31252 /* Prospecting */);   // skill-up chance
    owner->DestroyItemCount(itemEntry, 5, true);             // consume 5 ore

    std::ostringstream gained;
    for (auto const& m : gems)
    {
        for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
        {
            uint32 const g = WowPsParty::GuidForAccountSlot(account, partySlot);
            if (!g) continue;
            Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g));
            if (!p) continue;
            ItemPosCountVec dest;
            if (p->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, m.first, m.second) != EQUIP_ERR_OK)
                continue;
            p->StoreNewItem(dest, m.first, true);
            ItemTemplate const* mt = sObjectMgr->GetItemTemplate(m.first);
            if (!gained.str().empty()) gained << ", ";
            gained << m.second << "x " << (mt ? mt->Name1 : "?");
            break;
        }
    }

    ch.PSendSysMessage("|cff66ccff[WowPsParty]|r {} prospected 5x |cffffffff{}|r -> {}.",
        prospector->GetName(), itemName,
        gained.str().empty() ? "nothing (party bags full)" : gained.str());
    LOG_INFO("module",
        "[WowPsParty Prospect] requester={} owner={} prospector={} ore='{}'(entry={}) -> {}",
        requester->GetGUID().GetCounter(), owner->GetGUID().GetCounter(),
        prospector->GetGUID().GetCounter(), itemName, itemEntry,
        gained.str().empty() ? "NOTHING (bags full)" : gained.str());

    WowPsParty::SendInventoryTo(requester);
}

// MILL\t<srcPartySlot>\t<srcItemGuidLow> — mill a stack of herbs from any party member
// into pigments. Mirrors HandleProspect: a party member with Inscription skill
// (>= the herb's RequiredSkillRank) is the scribe; one mill consumes 5 herbs and rolls
// the SAME table the Milling spell uses (LootTemplates_Milling), keyed by the herb's
// item entry — directly, since a clientless bot can't drive the loot WINDOW.
static void HandleMill(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const srcSlot        = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);
    if (!srcItemGuidLow) return;

    uint32 const account   = requester->GetSession()->GetAccountId();
    uint32 const ownerGuid = WowPsParty::GuidForAccountSlot(account, srcSlot);
    if (!ownerGuid) return;
    Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(ownerGuid));
    if (!owner) return;
    Item* item = SafeGetItemByGuid(owner, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!item) return;
    ItemTemplate const* tmpl = item->GetTemplate();
    if (!tmpl) return;

    ChatHandler ch(requester->GetSession());
    if (!tmpl->HasFlag(ITEM_FLAG_IS_MILLABLE))
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r |cffffffff{}|r can't be milled.", tmpl->Name1);
        return;
    }
    if (item->GetCount() < 5)
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r Milling needs a stack of at least 5 |cffffffff{}|r.", tmpl->Name1);
        return;
    }

    // A party member whose Inscription skill covers this herb's required level.
    Player* scribe = nullptr;
    std::ostringstream diag;
    for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
    {
        uint32 const g = WowPsParty::GuidForAccountSlot(account, partySlot);
        if (!g) { diag << " [slot" << uint32(partySlot) << ":empty]"; continue; }
        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g));
        if (!p) { diag << " [slot" << uint32(partySlot) << ":guid" << g << ":NOT-CONNECTED]"; continue; }
        bool const hasSkill = p->HasSkill(SKILL_INSCRIPTION);
        uint16 const skill  = hasSkill ? p->GetSkillValue(SKILL_INSCRIPTION) : 0;
        diag << " [slot" << uint32(partySlot) << ":" << p->GetName()
             << ":insc=" << (hasSkill ? std::to_string(skill) : std::string("none")) << "]";
        if (!scribe && hasSkill && skill >= tmpl->RequiredSkillRank)
            scribe = p;
    }
    if (!scribe)
    {
        LOG_WARN("module",
            "[WowPsParty Mill] FAILED 'no scribe skilled enough' — requester={} account={} "
            "item='{}'(entry={}) requiredSkill={} | party:{}",
            requester->GetName(), account, tmpl->Name1, tmpl->ItemId, tmpl->RequiredSkillRank, diag.str());
        ch.PSendSysMessage(
            "|cffff5555[WowPsParty]|r No party scribe skilled enough to mill "
            "|cffffffff{}|r (needs Inscription {}).", tmpl->Name1, tmpl->RequiredSkillRank);
        return;
    }

    // Roll the milling loot directly (same table as the spell), keyed by the herb entry.
    Loot loot;
    loot.FillLoot(tmpl->ItemId, LootTemplates_Milling, owner, true, true);
    std::vector<std::pair<uint32, uint32>> pigments;
    for (LootItem const& li : loot.items)
        if (li.itemid && li.count)
            pigments.emplace_back(li.itemid, uint32(li.count));
    if (pigments.empty())
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r Milling produced nothing — herbs kept.");
        return;
    }

    // Make sure the party can hold the pigments BEFORE consuming the herbs (milling is
    // irreversible). Don't assume the 5-herb consume frees a slot — it only does if the
    // stack was exactly 5 — so require the pigments to fit the CURRENT free space.
    auto freeSlots = [](Player* p) -> uint32
    {
        uint32 n = 0;
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (!p->GetItemByPos(INVENTORY_SLOT_BAG_0, i)) ++n;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (!p->GetItemByPos(b, uint8(j))) ++n;
        return n;
    };
    uint32 partyFree = 0;
    for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
    {
        uint32 const g = WowPsParty::GuidForAccountSlot(account, partySlot);
        if (!g) continue;
        if (Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g)))
            partyFree += freeSlots(p);
    }
    if (pigments.size() > size_t(partyFree))
    {
        ch.PSendSysMessage(
            "|cffff5555[WowPsParty]|r Not enough free bag space for the pigments — make room and try again. (Herbs kept.)");
        return;
    }

    std::string const itemName = tmpl->Name1;
    uint32 const itemEntry = tmpl->ItemId;
    scribe->UpdateCraftSkill(51005 /* Milling */);   // skill-up chance
    owner->DestroyItemCount(itemEntry, 5, true);      // consume 5 herbs

    std::ostringstream gained;
    for (auto const& m : pigments)
    {
        for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
        {
            uint32 const g = WowPsParty::GuidForAccountSlot(account, partySlot);
            if (!g) continue;
            Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g));
            if (!p) continue;
            ItemPosCountVec dest;
            if (p->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, m.first, m.second) != EQUIP_ERR_OK)
                continue;
            p->StoreNewItem(dest, m.first, true);
            ItemTemplate const* mt = sObjectMgr->GetItemTemplate(m.first);
            if (!gained.str().empty()) gained << ", ";
            gained << m.second << "x " << (mt ? mt->Name1 : "?");
            break;
        }
    }

    ch.PSendSysMessage("|cff66ccff[WowPsParty]|r {} milled 5x |cffffffff{}|r -> {}.",
        scribe->GetName(), itemName,
        gained.str().empty() ? "nothing (party bags full)" : gained.str());
    LOG_INFO("module",
        "[WowPsParty Mill] requester={} owner={} scribe={} herb='{}'(entry={}) -> {}",
        requester->GetGUID().GetCounter(), owner->GetGUID().GetCounter(),
        scribe->GetGUID().GetCounter(), itemName, itemEntry,
        gained.str().empty() ? "NOTHING (bags full)" : gained.str());

    WowPsParty::SendInventoryTo(requester);
}

// ---------------------------------------------------------------------------
// Recipes — taught to the party member who can LEARN them
// ---------------------------------------------------------------------------
// Every craft a recipe teaches, in the two shapes 3.3.5a data uses. 2034 of the 3066
// class-9 rows carry it behind ITEM_SPELLTRIGGER_LEARN_SPELL_ID with the generic
// "Learning" (483) on use — a pairing ObjectMgr::LoadItemTemplates rejects the item
// over, so it is safe to rely on. Exactly 10 more (Expert Cookbook, Master Cookbook,
// the four fishing books, the three first-aid manuals, Manual: The Path of Defense)
// instead put an ordinary SPELL_EFFECT_LEARN_SPELL on the ON_USE slot; reading only
// the first shape dropped every one of them silently. Empty = not a recipe we teach.
//
// The remaining 94 class-9 ON_USE rows — warlock Grimoires, Book of Glyph Mastery —
// teach nothing this way (their on-use spell is a pet dummy / a random discovery), so
// they resolve empty and fall through to the ordinary cast path, unchanged. The other
// 928 class-9 rows carry no item spell at all and never reach here.
//
// Returns a VECTOR, not one spell: Spell::EffectLearnSpell runs once per effect index,
// and item 6619 "Manual: The Path of Defense" carries three (Defensive Stance 71,
// Sunder Armor 7386, Taunt 355). Returning the first taught one ability and then
// destroyed the manual, losing the other two with no way to get them back.
static std::vector<uint32> RecipeTaughtSpells(ItemTemplate const* tmpl)
{
    std::vector<uint32> taught;
    if (!tmpl || tmpl->Class != ITEM_CLASS_RECIPE)
        return taught;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        if (tmpl->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID && tmpl->Spells[i].SpellId > 0)
        {
            taught.push_back(uint32(tmpl->Spells[i].SpellId));
            return taught;   // LoadItemTemplates allows this trigger on slot 1 only
        }
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (tmpl->Spells[i].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE || tmpl->Spells[i].SpellId <= 0)
            continue;
        SpellInfo const* si = sSpellMgr->GetSpellInfo(uint32(tmpl->Spells[i].SpellId));
        if (!si)
            continue;
        for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
            if (si->Effects[e].Effect == SPELL_EFFECT_LEARN_SPELL && si->Effects[e].TriggerSpell)
                taught.push_back(si->Effects[e].TriggerSpell);
    }
    return taught;
}

// Whether one member can be taught one recipe, and if not, why. Order matters: the
// verdict is picked across the WHOLE party, so the reason listed first wins over the
// ones below it. SKILL SHORT outranks ALREADY KNOWN deliberately — with one alt at
// the cap and another 10 points off the same profession, "already known, sell it" is
// advice that throws away a recipe the second alt still wants. Below those, nothing
// will ever change. Mirrors Player::CanUseItem's gates (PlayerStorage.cpp) plus the
// already-known test the engine does NOT do: Player::learnSpell no-ops on a known
// spell while Spell::TakeCastItem still eats the recipe, so casting to find out
// destroys it for nothing.
enum class RecipeFit : uint8
{
    Learnable,
    SkillTooLow,
    AlreadyKnown,
    NoProfession,
    Ineligible,
};

// `taught` is every spell the item confers. ALREADY KNOWN means the member knows them
// ALL — a multi-spell manual whose holder trained one of its abilities separately still
// has something to give, and calling that "already known" would advise selling it.
static RecipeFit RateRecipeFit(Player* p, ItemTemplate const* tmpl, std::vector<uint32> const& taught)
{
    bool knowsAll = true;
    for (uint32 spell : taught)
        if (!p->HasSpell(spell)) { knowsAll = false; break; }
    if (knowsAll)
        return RecipeFit::AlreadyKnown;
    if (tmpl->HasFlag2(ITEM_FLAG2_FACTION_HORDE) && p->GetTeamId(true) != TEAM_HORDE)
        return RecipeFit::Ineligible;
    if (tmpl->HasFlag2(ITEM_FLAG2_FACTION_ALLIANCE) && p->GetTeamId(true) != TEAM_ALLIANCE)
        return RecipeFit::Ineligible;
    if ((tmpl->AllowableClass & p->getClassMask()) == 0 || (tmpl->AllowableRace & p->getRaceMask()) == 0)
        return RecipeFit::Ineligible;
    if (p->GetLevel() < tmpl->RequiredLevel)
        return RecipeFit::Ineligible;
    if (tmpl->RequiredSpell != 0 && !p->HasSpell(tmpl->RequiredSpell))
        return RecipeFit::Ineligible;
    if (tmpl->RequiredSkill != 0)
    {
        uint16 const skill = p->GetSkillValue(tmpl->RequiredSkill);
        if (skill == 0)
            return RecipeFit::NoProfession;
        if (skill < tmpl->RequiredSkillRank)
            return RecipeFit::SkillTooLow;
    }
    return RecipeFit::Learnable;
}

// The account's party slots resolved to live players, indexed BY SLOT (holes stay
// null — HandleLearnRecipe indexes this with the client's srcPartySlot). Every
// `GuidForAccountSlot` is a blocking SELECT on the world thread, and the Learn drain
// fires twelve LEARNs a second — resolving per candidate turned one button press
// into hundreds of queries.
//
// A member whose storage is mid-teardown is dropped here rather than at the call
// site: this vector is both the OWNER lookup and the pool ChooseRecipeLearner reads
// value arrays off (GetTeamId/getClassMask/GetLevel/HasSpell/GetSkillValue) and then
// hands `learnSpell`. That is exactly the read `MemberStorageUnstable` exists to
// forbid — see its comment and the twenty sibling handlers that filter the same way.
// Filtering here makes a torn-down owner surface as the RECIPE_UNAVAILABLE the
// handler already sends, and makes such a member ineligible to be the learner.
//
// FindConnectedPlayer alone: ObjectAccessor::FindPlayer is that same lookup plus an
// IsInWorld() filter, so it is a strict subset and can never rescue a null — and
// MemberStorageUnstable requires IsInWorld() anyway.
static std::vector<Player*> ResolvePartySlots(uint32 account)
{
    std::vector<Player*> members(WowPsParty::PARTY_SIZE, nullptr);
    for (uint8 slot = 0; slot < WowPsParty::PARTY_SIZE; ++slot)
    {
        uint32 const g = WowPsParty::GuidForAccountSlot(account, slot);
        if (!g) continue;
        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g));
        if (!WowPsParty::MemberStorageUnstable(p))
            members[slot] = p;
    }
    return members;
}

// Who to teach a recipe to — and, when nobody can, the near-miss worth naming.
// "Nissemia is 50 Cooking short" is the only form of refusal a player can act on;
// a bare "nobody can learn it" leaves them staring at the same pile.
struct RecipeVerdict
{
    Player*   learner      = nullptr;
    RecipeFit reason       = RecipeFit::NoProfession;   // meaningful only when learner is null
    Player*   closest      = nullptr;                   // the member `reason` describes
    uint16    closestSkill = 0;
};

static RecipeVerdict ChooseRecipeLearner(Player* requester, std::vector<Player*> const& members,
                                         ItemTemplate const* tmpl, std::vector<uint32> const& taught)
{
    RecipeVerdict best;
    bool haveReason = false;

    auto consider = [&](Player* p)
    {
        if (!p || best.learner) return;
        RecipeFit const fit = RateRecipeFit(p, tmpl, taught);
        if (fit == RecipeFit::Learnable) { best.learner = p; return; }
        uint16 const skill = tmpl->RequiredSkill ? p->GetSkillValue(tmpl->RequiredSkill) : 0;
        // Lower enum value = more actionable, so it wins. Between two members stuck on
        // the same reason, the one closest to the requirement is the one to name.
        if (!haveReason || fit < best.reason || (fit == best.reason && skill > best.closestSkill))
        {
            haveReason        = true;
            best.reason       = fit;
            best.closest      = p;
            best.closestSkill = skill;
        }
    };

    // The human first: it is their character, and a recipe they can learn themselves
    // shouldn't go to a party-mate just because that mate sits in slot 0.
    consider(requester);
    for (Player* p : members)
        if (p && p != requester)
            consider(p);
    return best;
}

// Wire codes for LEARN_RESULT. The bulk "Learn" button drains one LEARN per recipe
// and tallies these client-side into a single summary, so a bag of 40 recipes costs
// one chat line instead of 40.
enum RecipeLearnCode : uint8
{
    RECIPE_LEARNED       = 1,
    RECIPE_ALREADY_KNOWN = 2,
    RECIPE_SKILL_TOO_LOW = 3,
    RECIPE_NO_PROFESSION = 4,
    RECIPE_INELIGIBLE    = 5,
    RECIPE_NOT_A_RECIPE  = 6,
    RECIPE_UNAVAILABLE   = 7,   // owner or item gone between the queue and the reply
};

static char const* SkillLineName(uint32 skillId)
{
    SkillLineEntry const* entry = sSkillLineStore.LookupEntry(skillId);
    return (entry && entry->name[LOCALE_enUS]) ? entry->name[LOCALE_enUS] : "that profession";
}

// Teach one recipe to whichever party member can learn it, and consume it.
//
// Learning is applied DIRECTLY (learnSpell + consume) rather than by moving the
// recipe onto the learner and casting it. The learner is normally a clientless
// bot, and the cross-character move has its own failure mode (full bags) that
// would strand the recipe halfway through an action the player asked for once —
// the same reason HandleDisenchant rolls its loot table by hand. `quiet` is the
// bulk path: every REFUSAL is suppressed to its machine-readable LEARN_RESULT and the
// client tallies them into one summary line, so a bag of 40 costs one line instead of
// 40. A LEARNED line is still printed — the summary only counts ("Learned 3 recipes"),
// and which craft went to which member is the part worth naming. `gen` is the client's
// run id, echoed into every reply so a straggler can't be counted into the next run.
static void TeachRecipeFromParty(Player* requester, Player* owner, Item* item, bool quiet,
                                 uint32 gen, std::vector<Player*> const& members)
{
    ItemTemplate const* tmpl = WowPsParty::SafeItemTemplate(item);
    ChatHandler ch(requester->GetSession());
    std::string const itemName = tmpl ? tmpl->Name1 : "that item";

    auto report = [&](RecipeLearnCode code, std::string const& line)
    {
        SendWPSP(requester, Acore::StringFormat("LEARN_RESULT\t{}\t{}\t{}",
            uint32(code), gen, itemName));
        if (!quiet || code == RECIPE_LEARNED)
            ch.SendSysMessage(line);
    };

    if (!tmpl)
    {
        report(RECIPE_UNAVAILABLE,
            "|cffff5555[WowPsParty]|r That item is no longer in the party's bags.");
        return;
    }

    std::vector<uint32> const taught = RecipeTaughtSpells(tmpl);
    if (taught.empty())
    {
        report(RECIPE_NOT_A_RECIPE, Acore::StringFormat(
            "|cffff5555[WowPsParty]|r |cffffffff{}|r doesn't teach anything.", tmpl->Name1));
        return;
    }

    RecipeVerdict const v = ChooseRecipeLearner(requester, members, tmpl, taught);
    if (!v.learner)
    {
        switch (v.reason)
        {
            case RecipeFit::AlreadyKnown:
                report(RECIPE_ALREADY_KNOWN, Acore::StringFormat(
                    "|cff66ccff[WowPsParty]|r {} already knows |cffffffff{}|r — nothing left to learn from it.",
                    v.closest->GetName(), tmpl->Name1));
                break;
            case RecipeFit::SkillTooLow:
                report(RECIPE_SKILL_TOO_LOW, Acore::StringFormat(
                    "|cffff5555[WowPsParty]|r |cffffffff{}|r needs {} {} — {} is the closest at {}.",
                    tmpl->Name1, SkillLineName(tmpl->RequiredSkill), tmpl->RequiredSkillRank,
                    v.closest->GetName(), v.closestSkill));
                break;
            case RecipeFit::NoProfession:
                report(RECIPE_NO_PROFESSION, Acore::StringFormat(
                    "|cffff5555[WowPsParty]|r Nobody in the party has {} — |cffffffff{}|r can't be learned.",
                    SkillLineName(tmpl->RequiredSkill), tmpl->Name1));
                break;
            default:
                report(RECIPE_INELIGIBLE, Acore::StringFormat(
                    "|cffff5555[WowPsParty]|r No party member meets |cffffffff{}|r's requirements.", tmpl->Name1));
                break;
        }
        return;
    }

    uint32 const      itemEntry   = tmpl->ItemId;
    std::string const learnerName = v.learner->GetName();
    // learnSpell runs OnPlayerLearnSpell hooks and recurses through rank chains, so
    // `item` must not be dereferenced across it — a freed Item* still sitting in a bag
    // slot is this module's most-repeated crash class. Take the guid now, re-resolve
    // after, and treat a vanished item as "already spent" rather than guessing.
    ObjectGuid const itemGuid = item->GetGUID();

    // Every spell the item confers, not just the first: the engine's own
    // Spell::EffectLearnSpell runs once per effect index, and the item is consumed
    // below either way. Skip the ones this member already trained separately —
    // learnSpell no-ops on those, and re-sending them is pointless packet traffic.
    std::string taughtList;
    for (uint32 spell : taught)
    {
        if (v.learner->HasSpell(spell))
            continue;
        v.learner->learnSpell(spell);
        if (!taughtList.empty()) taughtList += ",";
        taughtList += std::to_string(spell);
    }

    Item* spent = SafeGetItemByGuid(owner, itemGuid);
    if (spent)
    {
        if (spent->GetCount() > 1)
        {
            spent->SetCount(spent->GetCount() - 1);
            spent->SetState(ITEM_CHANGED, owner);
        }
        else
            owner->DestroyItem(spent->GetBagSlot(), spent->GetSlot(), true);
    }

    report(RECIPE_LEARNED, Acore::StringFormat(
        "|cff66ccff[WowPsParty]|r {} learned |cffffffff{}|r.", learnerName, itemName));
    LOG_INFO("module",
        "[WowPsParty Learn] requester={} owner={} learner={} recipe='{}'(entry={}) spell={}",
        requester->GetName(), owner->GetName(), learnerName, itemName, itemEntry, taughtList);

    // The bulk drain re-requests the grid once at the end of the run; pushing a full
    // inventory per recipe would send forty of them for one button press.
    if (!quiet)
    {
        WowPsParty::SendInventoryTo(requester);
        if (owner != requester) WowPsParty::SendInventoryTo(owner);
    }
}

// LEARN\t<srcPartySlot>\t<srcItemGuidLow>[\t<quiet>[\t<gen>]] — teach a recipe out of
// the shared bags to whoever can learn it. Reached two ways: right-clicking a recipe
// in the grid (bare, gen 0) and the Learn button's bulk drain (quiet=1 + its run id).
//
// Every exit that KNOWS the run id must still answer with a LEARN_RESULT. A silent
// return leaves the drain's outstanding count one too high, so the run burns its whole
// grace period and then omits the item from the summary entirely — the shape that makes
// a bulk action look like it half-worked. The one exit that cannot answer is a payload
// too short to carry a `gen`: a reply stamped with the wrong run id is worse than none,
// since the client would decrement a run this item was never part of.
static void HandleLearnRecipe(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;

    std::vector<std::string_view> field;
    for (size_t start = 0; start <= payload.size(); )
    {
        size_t const tab = payload.find('\t', start);
        field.push_back(payload.substr(start, tab == std::string_view::npos
                                             ? std::string_view::npos : tab - start));
        if (tab == std::string_view::npos) break;
        start = tab + 1;
    }
    if (field.size() < 2) return;

    auto toUint = [](std::string_view sv) { return uint32(std::strtoul(std::string(sv).c_str(), nullptr, 10)); };
    uint32 const srcSlot        = toUint(field[0]);
    uint32 const srcItemGuidLow = toUint(field[1]);
    bool const   quiet          = field.size() > 2 && field[2] == "1";
    uint32 const gen            = field.size() > 3 ? toUint(field[3]) : 0;

    uint32 const account = requester->GetSession()->GetAccountId();
    std::vector<Player*> const members = ResolvePartySlots(account);

    Player* owner = srcSlot < members.size() ? members[srcSlot] : nullptr;
    Item*   item  = (owner && srcItemGuidLow)
                  ? SafeGetItemByGuid(owner, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow))
                  : nullptr;
    // Mid-logout the inventory is tearing down; mutating it there is what the four
    // sibling handlers guard against, and the drain aims twelve of these a second at
    // up to five different owners. ResolvePartySlots already dropped such a member to
    // null, so this re-check is belt-and-braces rather than the only guard.
    if (!owner || !item || WowPsParty::MemberStorageUnstable(owner))
    {
        SendWPSP(requester, Acore::StringFormat("LEARN_RESULT\t{}\t{}\t?", uint32(RECIPE_UNAVAILABLE), gen));
        if (!quiet)
            ChatHandler(requester->GetSession()).PSendSysMessage(
                "|cffff5555[WowPsParty]|r That item is no longer reachable — try Refresh.");
        return;
    }

    TeachRecipeFromParty(requester, owner, item, quiet, gen, members);
}

// PICKLOCK\t<srcPartySlot>\t<srcItemGuidLow> — pick a locked box (junkbox / lockbox /
// strongbox) from any party member's bags using a party ROGUE's Lockpicking skill.
// Mirrors HandleDisenchant: any party member whose Lockpicking >= the box's required
// skill is the picker. A clientless bot owner can't drive the loot WINDOW, so the box's
// item-loot is rolled directly (the same LootTemplates_Item + money path the engine's
// Player::SendLoot uses) into the party's bags, then the emptied box is consumed —
// equivalent to picking the lock and looting it out. The point is the "tell your rogue
// to open it" feel: you still have to log in your rogue hero and train Lockpicking.
// TryPartyPickLock — shared pick-lock core. Picks a Lockpicking-locked box in `owner`'s bags
// with a party rogue's skill, rolls its loot into the party, and consumes it. Returns TRUE if
// the item IS a lockpicking-pickable box (a no-rogue / bags-full / empty case still emits its
// own message and counts as handled), FALSE only when it is NOT a lockpicking box (key box /
// no lock / already unlocked) so the caller can fall back. Shared by the Ctrl+K PICKLOCK
// keybind AND the right-click "open" path, so right-clicking a lockbox opens it the same way.
static bool TryPartyPickLock(Player* requester, Player* owner, Item* item)
{
    if (!requester || !requester->GetSession() || !owner || !item) return false;
    ItemTemplate const* tmpl = item->GetTemplate();
    if (!tmpl) return false;
    uint32 const account = requester->GetSession()->GetAccountId();

    ChatHandler ch(requester->GetSession());

    // It must be a Lockpicking-locked box. Read its lock (the same LOCK_KEY_SKILL table
    // the gather scanner + EffectOpenLock read) and pull the Lockpicking requirement. A
    // box needing a KEY, a non-lockpicking skill, or no lock is not ours.
    uint32 reqSkill = 0;
    bool   pickable = false;
    if (uint32 lockId = tmpl->LockID)
        if (LockEntry const* lock = sLockStore.LookupEntry(lockId))
            for (uint8 i = 0; i < 8; ++i)
            {
                if (lock->Type[i] != LOCK_KEY_SKILL) continue;
                if (SkillByLockType(LockType(lock->Index[i])) == SKILL_LOCKPICKING)
                { reqSkill = lock->Skill[i]; pickable = true; break; }
            }
    if (item->HasFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_UNLOCKED))
        pickable = false;   // already opened
    if (!pickable)
        return false;   // not a Lockpicking box (key required / no lock) — the caller handles it

    // A party member whose Lockpicking covers the box. Per-slot diagnostic like
    // HandleDisenchant so a failure (a rogue momentarily zoning / skills not loaded)
    // is explainable from the log.
    Player* picker = nullptr;
    std::ostringstream diag;
    for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
    {
        uint32 const g = WowPsParty::GuidForAccountSlot(account, partySlot);
        if (!g) { diag << " [slot" << uint32(partySlot) << ":empty]"; continue; }
        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g));
        if (!p) { diag << " [slot" << uint32(partySlot) << ":NOT-CONNECTED]"; continue; }
        bool const has = p->HasSkill(SKILL_LOCKPICKING);
        uint16 const sk = has ? p->GetSkillValue(SKILL_LOCKPICKING) : 0;
        diag << " [slot" << uint32(partySlot) << ":" << p->GetName()
             << ":lockpick=" << (has ? std::to_string(sk) : std::string("none")) << "]";
        if (!picker && has && sk >= reqSkill) picker = p;
    }
    if (!picker)
    {
        LOG_WARN("module",
            "[WowPsParty PickLock] FAILED 'no rogue skilled enough' — requester={} account={} "
            "item='{}'(entry={}) reqLockpick={} | party:{}",
            requester->GetName(), account, tmpl->Name1, tmpl->ItemId, reqSkill, diag.str());
        ch.PSendSysMessage(
            "|cffff5555[WowPsParty]|r No party rogue skilled enough to pick |cffffffff{}|r "
            "(needs Lockpicking {}). Train a rogue hero's Lockpicking.", tmpl->Name1, reqSkill);
        return true;
    }

    // Roll the box's contents the same way Player::SendLoot opens an item: money first,
    // then the item table (noEmptyError keyed off whether money rolled).
    Loot loot;
    loot.generateMoneyLoot(tmpl->MinMoneyLoot, tmpl->MaxMoneyLoot);
    loot.FillLoot(tmpl->ItemId, LootTemplates_Item, owner, true, loot.gold != 0);

    std::vector<std::pair<uint32, uint32>> drops;
    for (LootItem const& li : loot.items)
        if (li.itemid && li.count) drops.emplace_back(li.itemid, uint32(li.count));
    uint32 const money = loot.gold;

    // A box that rolled absolutely nothing (rare/edge): don't destroy it for free —
    // just unlock it so the player can open it the normal way, and bail.
    if (drops.empty() && money == 0)
    {
        item->SetFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_UNLOCKED);
        item->SetState(ITEM_CHANGED, owner);
        if (uint32 pure = picker->GetPureSkillValue(SKILL_LOCKPICKING))
            if (picker->UpdateGatherSkill(SKILL_LOCKPICKING, pure, reqSkill))
                WowPsParty::AnnounceGatherSkillUp(picker, SKILL_LOCKPICKING);
        ch.PSendSysMessage("|cff66ccff[WowPsParty]|r {} unlocked |cffffffff{}|r (it was empty).",
            picker->GetName(), tmpl->Name1);
        WowPsParty::SendInventoryTo(requester);
        return true;
    }

    // Picking is IRREVERSIBLE (the box is consumed), so confirm the party can actually
    // hold the rolled drops BEFORE destroying it — never lose item 2/3 of a multi-item
    // box to full bags. Mirrors HandleDisenchant's precheck; the box's own slot frees on
    // consume, so it covers one drop (the +1). Conservative (ignores partial-stack merges),
    // which only ever errs toward "make room".
    auto freeSlots = [](Player* p) -> uint32
    {
        uint32 n = 0;
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (!p->GetItemByPos(INVENTORY_SLOT_BAG_0, i)) ++n;
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = p->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (!p->GetItemByPos(b, uint8(j))) ++n;
        return n;
    };
    uint32 partyFree = 0;
    for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
    {
        uint32 const g = WowPsParty::GuidForAccountSlot(account, partySlot);
        if (!g) continue;
        if (Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g)))
            partyFree += freeSlots(p);
    }
    if (drops.size() > size_t(partyFree) + 1)   // +1 = the box's slot that frees on consume
    {
        ch.PSendSysMessage(
            "|cffff5555[WowPsParty]|r Not enough free bag space for |cffffffff{}|r's contents "
            "— make room and try again. (Box kept.)", tmpl->Name1);
        return true;
    }

    std::string const itemName = tmpl->Name1;
    uint32 const itemEntry = tmpl->ItemId;

    // Lockpicking skill-up for the picker (mirrors EffectOpenLock's item path), then
    // consume the box (frees its slot for the drops) and distribute across the party
    // (owner first), exactly like HandleDisenchant delivers mats.
    if (uint32 pure = picker->GetPureSkillValue(SKILL_LOCKPICKING))
        if (picker->UpdateGatherSkill(SKILL_LOCKPICKING, pure, reqSkill))
            WowPsParty::AnnounceGatherSkillUp(picker, SKILL_LOCKPICKING);
    owner->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);

    std::ostringstream gained;
    for (auto const& d : drops)
    {
        for (uint8 partySlot = 0; partySlot < WowPsParty::PARTY_SIZE; ++partySlot)
        {
            uint32 const g = WowPsParty::GuidForAccountSlot(account, partySlot);
            if (!g) continue;
            Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(g));
            if (!p) continue;
            ItemPosCountVec dest;
            if (p->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, d.first, d.second) != EQUIP_ERR_OK)
                continue;
            p->StoreNewItem(dest, d.first, true);
            ItemTemplate const* dt = sObjectMgr->GetItemTemplate(d.first);
            if (!gained.str().empty()) gained << ", ";
            gained << d.second << "x " << (dt ? dt->Name1 : "?");
            break;
        }
    }
    if (money > 0)
    {
        owner->ModifyMoney(int32(money));
        if (!gained.str().empty()) gained << ", ";
        gained << (money / 10000) << "g " << ((money % 10000) / 100) << "s " << (money % 100) << "c";
    }

    ch.PSendSysMessage("|cff66ccff[WowPsParty]|r {} picked |cffffffff{}|r -> {}.",
        picker->GetName(), itemName, gained.str().empty() ? "nothing (party bags full)" : gained.str());
    LOG_INFO("module",
        "[WowPsParty PickLock] requester={} owner={} picker={} box='{}'(entry={}) reqLockpick={} -> {}",
        requester->GetGUID().GetCounter(), owner->GetGUID().GetCounter(),
        picker->GetGUID().GetCounter(), itemName, itemEntry, reqSkill,
        gained.str().empty() ? "NOTHING (bags full)" : gained.str());

    WowPsParty::SendInventoryTo(requester);
    return true;
}

// PICKLOCK\t<srcPartySlot>\t<srcItemGuidLow> — pick a locked box from any party member's bags
// with a party rogue's Lockpicking (the Ctrl+K hover keybind). Resolves the box, then defers to
// the shared TryPartyPickLock core. A box that isn't a lockpicking lock reports so here.
static void HandlePickLock(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);
    if (!srcItemGuidLow) return;
    uint32 const srcSlot   = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const account   = requester->GetSession()->GetAccountId();
    uint32 const ownerGuid = WowPsParty::GuidForAccountSlot(account, srcSlot);
    if (!ownerGuid) return;
    Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(ownerGuid));
    if (!owner) return;
    Item* item = SafeGetItemByGuid(owner, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!item) return;
    if (!TryPartyPickLock(requester, owner, item))
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r |cffffffff{}|r isn't a pickable lockbox.",
            item->GetTemplate() ? item->GetTemplate()->Name1 : "That item");
}

// MAIL_SEND\t<srcPartySlot>\t<srcItemGuidLow>\t<recipientName> — mail a bagged item from
// any party member to <recipientName>. The item leaves the owner's bags and arrives in the
// recipient's mailbox, sent server-side (the item may be on a different char than the one
// at the mailbox, and a clientless bot owner can't drive the mail UI). Soulbound items can
// only go to your OWN characters (same account), like the real mail rules.
static void HandleMailSend(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto t1 = payload.find('\t');
    if (t1 == std::string_view::npos) return;
    auto t2 = payload.find('\t', t1 + 1);
    if (t2 == std::string_view::npos) return;
    uint32 const srcSlot        = std::strtoul(std::string(payload.substr(0, t1)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(t1 + 1, t2 - t1 - 1)).c_str(), nullptr, 10);
    std::string recipient(payload.substr(t2 + 1));
    // Trim + normalise the recipient name (the client capitalises, but be safe).
    size_t const a = recipient.find_first_not_of(" \t");
    size_t const b = recipient.find_last_not_of(" \t");
    recipient = (a == std::string::npos) ? std::string() : recipient.substr(a, b - a + 1);
    if (!srcItemGuidLow || recipient.empty()) return;

    ChatHandler ch(requester->GetSession());
    uint32 const account   = requester->GetSession()->GetAccountId();
    uint32 const ownerGuid = WowPsParty::GuidForAccountSlot(account, srcSlot);
    if (!ownerGuid) return;
    Player* owner = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(ownerGuid));
    if (!owner) return;
    Item* item = SafeGetItemByGuid(owner, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!item) return;
    ItemTemplate const* tmpl = item->GetTemplate();
    if (!tmpl) return;

    if (item->IsEquipped() || item->IsNotEmptyBag())
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r Unequip / empty it before mailing.");
        return;
    }

    ObjectGuid const rcvGuid = sCharacterCache->GetCharacterGuidByName(recipient);
    if (!rcvGuid)
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r No character named |cffffffff{}|r to mail to.", recipient);
        return;
    }
    if (rcvGuid == owner->GetGUID())
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r That item is already on |cffffffff{}|r.", recipient);
        return;
    }
    bool const sameAccount = (sCharacterCache->GetCharacterAccountIdByGuid(rcvGuid) == account);
    if (!item->CanBeTraded(true) && !sameAccount)   // mailbound=true allows BoA; soulbound only to own chars
    {
        ch.PSendSysMessage("|cffff5555[WowPsParty]|r |cffffffff{}|r is soulbound — it can only be mailed to your own characters.", tmpl->Name1);
        return;
    }

    std::string const itemName = tmpl->Name1;
    Player* const rcvPlayer = ObjectAccessor::FindConnectedPlayer(rcvGuid);

    // Detach the item from the owner's bags and hand it to the mail (the EXACT sequence the
    // core's CMSG_SEND_MAIL handler uses — SetNotRefundable, move out of inventory, force a
    // standalone save with the NEW owner so item_instance.owner_guid is rewritten now and
    // doesn't drift if the server restarts while the mail sits unread), then send it. No
    // deposit charged (party QoL).
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    item->SetNotRefundable(owner);
    owner->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
    item->DeleteFromInventoryDB(trans);
    if (item->GetState() == ITEM_UNCHANGED)
        item->FSetState(ITEM_CHANGED);   // force the save below so owner_guid updates in the DB
    item->SetOwnerGUID(rcvGuid);
    item->SaveToDB(trans);
    MailDraft(std::string("Party Inventory"), std::string("Sent from the Party Inventory."))
        .AddItem(item)
        .SendMailTo(trans, MailReceiver(rcvPlayer, rcvGuid.GetCounter()), MailSender(owner),
                    MAIL_CHECK_MASK_COPIED);
    owner->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    ch.PSendSysMessage("|cff66ccff[WowPsParty]|r Mailed |cffffffff{}|r to |cffffffff{}|r.", itemName, recipient);
    LOG_INFO("module", "[WowPsParty Mail] requester={} owner={} item='{}'(entry={}) -> recipient='{}'(guid={})",
        requester->GetGUID().GetCounter(), owner->GetGUID().GetCounter(), itemName, tmpl->ItemId,
        recipient, rcvGuid.GetCounter());
    WowPsParty::SendInventoryTo(requester);
}

// How the priced-to-sell clamp relates to the mod-ah-bot-plus buyer: each buy
// cycle (1/min) the buyer re-rolls the item's valuation through the same
// public CalculateItemValue used here — a urand roll spanning
// [base*(1-BuyoutVariationReducePercent), base*(1+BuyoutVariationAddPercent)]
// — scales it by Buyer.AcceptablePriceModifier and count, and buys out any
// player auction with buyout strictly below that. A listing is therefore only
// GUARANTEED to sell when it sits under the roll's floor.
enum class AhBotPricing
{
    Unavailable,    // module absent or buyer disabled — no clamp possible
    VendorCapped,   // a vendor sells this item; the buyer never pays above its
                    // vendor price, which plain vendoring already beats
    Priced,         // outCopper = highest stack buyout the buyer always takes
};

#if defined(WOWPS_HAS_AHBOT)
// Mirror of the buyer's PreventOverpayingForVendorItems lookup (the ahbot
// keeps its copy private): every non-trade-good item some npc_vendor sells.
// Restart-scoped by design, like the ahbot's own copy — vendor tables are
// static world content.
static std::unordered_set<uint32> const& VendorSoldItemIds()
{
    static std::unordered_set<uint32> const ids = []
    {
        std::unordered_set<uint32> set;
        if (QueryResult r = WorldDatabase.Query(
            "SELECT DISTINCT v.entry FROM item_template v JOIN npc_vendor p ON v.entry = p.item WHERE v.class != {}",
            uint32(ITEM_CLASS_TRADE_GOODS)))
            do { set.insert(r->Fetch()[0].Get<uint32>()); } while (r->NextRow());
        return set;
    }();
    return ids;
}

static AhBotPricing AhBotGuaranteedStackPrice(ItemTemplate const* tmpl, uint32 count, uint32& outCopper)
{
    if (!auctionbot->IsModuleEnabled()
        || !sConfigMgr->GetOption<bool>("AuctionHouseBot.Buyer.Enabled", false))
        return AhBotPricing::Unavailable;

    if (sConfigMgr->GetOption<bool>("AuctionHouseBot.Buyer.PreventOverpayingForVendorItems", true)
        && VendorSoldItemIds().count(tmpl->ItemId))
        return AhBotPricing::VendorCapped;

    // CalculateItemValue returns one random roll, not the base value. The roll
    // is the base scaled linearly into [1-reduce, 1+add], so the highest of N
    // samples scaled by (1-reduce)/(1+add) is always <= the lowest roll the
    // buyer can make, and converges on it as N grows (~1% short at N=24).
    // Identical samples mean variations are configured off — the valuation is
    // deterministic and the sample IS the floor.
    uint64 maxSample = 0;
    uint64 minSample = std::numeric_limits<uint64>::max();
    for (int i = 0; i < 24; ++i)
    {
        uint64 bid = 0, buyout = 0;
        auctionbot->CalculateItemValue(tmpl, bid, buyout);
        maxSample = std::max(maxSample, buyout);
        minSample = std::min(minSample, buyout);
    }

    float const reduce   = std::clamp(sConfigMgr->GetOption<float>("AuctionHouseBot.BuyoutVariationReducePercent", 0.15f), 0.0f, 0.99f);
    float const add      = std::max(sConfigMgr->GetOption<float>("AuctionHouseBot.BuyoutVariationAddPercent", 0.25f), 0.0f);
    float const modifier = sConfigMgr->GetOption<float>("AuctionHouseBot.Buyer.AcceptablePriceModifier", 1.0f);

    double const perItemFloor = minSample == maxSample
        ? double(maxSample)
        : double(maxSample) * (1.0 - reduce) / (1.0 + add);

    uint64 stack = uint64(perItemFloor * modifier) * count;
    if (stack > 0)
        --stack;   // the buyer requires buyout STRICTLY below its willing price
    outCopper = uint32(std::min<uint64>(stack, MAX_MONEY_AMOUNT));
    return AhBotPricing::Priced;
}
#else
static AhBotPricing AhBotGuaranteedStackPrice(ItemTemplate const* /*tmpl*/, uint32 /*count*/, uint32& /*outCopper*/)
{
    return AhBotPricing::Unavailable;
}
#endif

// AH_SELL\t<srcPartySlot>\t<srcItemGuidLow>\t<copperBuyout>[\t1]
//   Lists an item out of any party member's bag on the owner's faction Auction
//   House — no auctioneer required (same auctioneer-less posting the AH bot uses).
//   24h listing, start bid == buyout == the requested copper price. The deposit
//   is charged to the item's owner; the shared-gold hook (OnPlayerMoneyChanged)
//   mirrors that delta across the pool. When the auction sells (or expires) the
//   AH mails proceeds / the item back to the owner char by the normal settlement
//   path, and collecting that mail re-mirrors the gold. Item must be tradeable
//   (not soulbound), unequipped and an empty bag — the same gates the real AH
//   applies, surfaced as chat errors so the click isn't silently dropped.
//   The optional trailing "1" (the bulk AuctionAll path) means "price to sell":
//   the requested copper is clamped down to what the AH buyer bot is guaranteed
//   to pay, and items the bot won't sensibly buy (vendor-sold, or worth more at
//   a vendor) are kept in the bag with a chat notice instead of expiring on the
//   AH. The manual popup path omits the flag and lists at the exact typed price.
static void HandleAhSell(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;

    auto t1 = payload.find('\t');
    if (t1 == std::string_view::npos) return;
    auto t2 = payload.find('\t', t1 + 1);
    if (t2 == std::string_view::npos) return;

    auto t3 = payload.find('\t', t2 + 1);

    uint32 const srcSlot = std::strtoul(std::string(payload.substr(0, t1)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(t1 + 1, t2 - t1 - 1)).c_str(), nullptr, 10);
    std::string_view const copperField = t3 == std::string_view::npos
        ? payload.substr(t2 + 1) : payload.substr(t2 + 1, t3 - t2 - 1);
    uint32 copper = std::strtoul(std::string(copperField).c_str(), nullptr, 10);
    bool const priceToSell = t3 != std::string_view::npos && payload.substr(t3 + 1) == "1";
    if (!srcItemGuidLow || copper == 0) return;

    auto err = [&](std::string const& msg)
    { ChatHandler(requester->GetSession()).SendSysMessage(msg.c_str()); };

    if (copper > MAX_MONEY_AMOUNT)
    {
        err("|cffff5555[WowPsParty]|r That price is too high.");
        return;
    }

    uint32 const account = requester->GetSession()->GetAccountId();
    QueryResult q = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}", account, srcSlot);
    if (!q) return;
    uint32 const srcCharGuid = q->Fetch()[0].Get<uint32>();
    Player* srcChar = ObjectAccessor::FindConnectedPlayer(
        ObjectGuid::Create<HighGuid::Player>(srcCharGuid));
    if (!srcChar) return;

    Item* srcItem = SafeGetItemByGuid(srcChar, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!srcItem) return;
    ItemTemplate const* tmpl = srcItem->GetTemplate();
    if (!tmpl) return;

    if (srcItem->IsEquipped())          { err("|cffff5555[WowPsParty]|r Unequip the item before auctioning it."); return; }
    if (srcItem->IsNotEmptyBag())       { err("|cffff5555[WowPsParty]|r Empty the bag before auctioning it."); return; }
    if (!srcItem->CanBeTraded())        { err(Acore::StringFormat("|cffff5555[WowPsParty]|r |cffffffff{}|r is soulbound — can't be auctioned.", tmpl->Name1)); return; }
    // Same extra gates the real AH applies (AuctionHouseHandler.cpp): conjured
    // items (mage food/water) and time-limited items don't survive AH escrow.
    if (tmpl->HasFlag(ITEM_FLAG_CONJURED) || srcItem->GetUInt32Value(ITEM_FIELD_DURATION))
                                        { err(Acore::StringFormat("|cffff5555[WowPsParty]|r |cffffffff{}|r can't be auctioned.", tmpl->Name1)); return; }
    if (sAuctionMgr->GetAItem(srcItem->GetGUID())) return;  // already in an auction

    AuctionHouseEntry const* ahEntry =
        sAuctionMgr->GetAuctionHouseEntryFromFactionTemplate(srcChar->GetFaction());
    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(srcChar->GetFaction());
    if (!ahEntry || !auctionHouse)
    {
        err("|cffff5555[WowPsParty]|r No auction house is reachable for that character.");
        return;
    }

    uint32 const etime   = 2 * MIN_AUCTION_TIME;   // 24h, in seconds
    uint32 const count   = srcItem->GetCount();

    if (priceToSell)
    {
        uint32 const requested = copper;
        uint32 botPrice = 0;
        switch (AhBotGuaranteedStackPrice(tmpl, count, botPrice))
        {
        case AhBotPricing::VendorCapped:
            err(Acore::StringFormat("|cffffd100[WowPsParty]|r Kept |cffffffff{}|r — a vendor sells it, so the auction bot won't outbid the vendor price. Sell it instead.", tmpl->Name1));
            LOG_INFO("module", "[WowPsParty AhSell] priced-to-sell '{}'(entry={}) x{} skipped: vendor-sold, buyer capped at vendor price (requested={})",
                tmpl->Name1, tmpl->ItemId, count, requested);
            return;
        case AhBotPricing::Priced:
        {
            if (botPrice < copper)
                copper = botPrice;
            // 5% AH cut on the sale; if the vendor pays as much or more, listing
            // is a strict loss — keep the item for the Sell button instead.
            uint64 const vendorTotal = uint64(tmpl->SellPrice) * count;
            if (copper == 0 || uint64(copper) * 95 / 100 <= vendorTotal)
            {
                err(Acore::StringFormat("|cffffd100[WowPsParty]|r Kept |cffffffff{}|r — a vendor pays more than the auction bot will. Sell it instead.", tmpl->Name1));
                LOG_INFO("module", "[WowPsParty AhSell] priced-to-sell '{}'(entry={}) x{} skipped: bot max {} <= vendor total {} (requested={})",
                    tmpl->Name1, tmpl->ItemId, count, botPrice, vendorTotal, requested);
                return;
            }
            LOG_INFO("module", "[WowPsParty AhSell] priced-to-sell '{}'(entry={}) x{}: requested={} botGuaranteed={} listing={}",
                tmpl->Name1, tmpl->ItemId, count, requested, botPrice, copper);
            break;
        }
        case AhBotPricing::Unavailable:
            LOG_INFO("module", "[WowPsParty AhSell] priced-to-sell requested but AH buyer bot inactive; listing '{}'(entry={}) x{} at requested {}",
                tmpl->Name1, tmpl->ItemId, count, copper);
            break;
        }
    }

    uint32 const deposit = sAuctionMgr->GetAuctionDeposit(ahEntry, etime, srcItem, count);
    if (!srcChar->HasEnoughMoney(deposit))
    {
        err(Acore::StringFormat("|cffff5555[WowPsParty]|r Not enough gold for the |cffffd100{}.{}.{}|r deposit.",
            deposit / 10000, (deposit / 100) % 100, deposit % 100));
        return;
    }
    srcChar->ModifyMoney(-int32(deposit));

    AuctionEntry* AH = new AuctionEntry();
    AH->Id              = sObjectMgr->GenerateAuctionID();
    AH->houseId         = AuctionHouseId(ahEntry->houseId);
    AH->item_guid       = srcItem->GetGUID();
    AH->item_template   = srcItem->GetEntry();
    AH->itemCount       = srcItem->GetCount();
    // Owner = the human requester, NOT srcChar: the item can sit in a bot hero's
    // bags, and bots don't reliably collect mail — so proceeds (and the unsold-
    // item return) would get stuck. The human owns it, sees it in their Auctions
    // tab, and collecting that mail re-mirrors the gold across the shared pool.
    AH->owner           = requester->GetGUID();
    AH->startbid        = copper;
    AH->bidder          = ObjectGuid::Empty;
    AH->bid             = 0;
    AH->buyout          = copper;
    AH->expire_time     = GameTime::GetGameTime().count() + etime;
    AH->deposit         = deposit;
    AH->auctionHouseEntry = ahEntry;

    sAuctionMgr->AddAItem(srcItem);
    auctionHouse->AddAuction(AH);

    srcChar->MoveItemFromInventory(srcItem->GetBagSlot(), srcItem->GetSlot(), true);

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    srcItem->DeleteFromInventoryDB(trans);
    srcItem->SaveToDB(trans);
    AH->SaveToDB(trans);
    srcChar->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);

    ChatHandler(requester->GetSession()).PSendSysMessage(
        "|cff66ccff[WowPsParty]|r Auctioned |cffffffff{}|r for |cffffd100{}.{}.{}|r buyout (24h).",
        tmpl->Name1, copper / 10000, (copper / 100) % 100, copper % 100);

    WowPsParty::SendInventoryTo(requester);
}

namespace
{
    // A 3.3.5a addon can only buy at the Auction House by driving the live UI one
    // confirmed listing at a time, which is why the shopping list's Auctionator
    // path needs a human at the keyboard for every stack. Buying from inside the
    // core has no such constraint, so AH_BUY takes one shopping-list row and
    // fills it outright — the mirror image of AH_SELL, auctioneer-less the same
    // way, with the item delivered by the same won-auction mail a hand-clicked
    // buyout produces.

    // A single request is one shopping-list row: a few stacks. The cap is the
    // runaway guard, reported back as `short` so the client stops rather than
    // grinding the whole house into one item.
    constexpr uint32 AH_BUY_MAX_AUCTIONS = 40;
    constexpr uint32 AH_BUY_MAX_COUNT    = 10000;

    struct AhBuyCandidate
    {
        uint32 id;
        uint32 stack;
        uint32 buyout;
    };

    // The core blocks buying your own auction and — for an OFFLINE owner — one
    // listed by another character on your account. Party-of-5 keeps four more
    // alts logged in, so the connected owner's account has to be checked too.
    bool AhBuyOwnedByRequester(AuctionEntry const* auction, Player* requester)
    {
        if (auction->owner == requester->GetGUID())
            return true;
        uint32 const account = requester->GetSession()->GetAccountId();
        if (Player* owner = ObjectAccessor::FindConnectedPlayer(auction->owner))
            return owner->GetSession() && owner->GetSession()->GetAccountId() == account;
        return sCharacterCache->GetCharacterAccountIdByGuid(auction->owner) == account;
    }

    void CollectAhBuyCandidates(AuctionHouseObject* house, Player* requester,
                                uint32 itemEntry, std::vector<AhBuyCandidate>& out)
    {
        for (auto const& [id, auction] : house->GetAuctions())
        {
            if (!auction || auction->item_template != itemEntry) continue;
            if (!auction->buyout || !auction->itemCount) continue;
            if (AhBuyOwnedByRequester(auction, requester)) continue;
            // The escrowed item has to still be resident: SendAuctionWonMail
            // mails nothing when it isn't, and the gold would already be gone.
            if (!sAuctionMgr->GetAItem(auction->item_guid)) continue;
            out.push_back({ id, auction->itemCount, auction->buyout });
        }
    }

    // Mirrors the client's WowPsShopping_ChooseAuction so an automatic pass buys
    // what a human clicking Buy Next would: the cheapest per-unit stack that fits
    // inside what's still missing, and only when nothing fits, the smallest
    // unavoidable overshoot. Returns an index into `candidates`, or -1.
    int PickAhBuyCandidate(std::vector<AhBuyCandidate> const& candidates,
                           uint32 remaining, bool& overbuy)
    {
        int fit = -1, over = -1;
        double fitUnit = 0.0, overUnit = 0.0;
        uint32 overExcess = 0;

        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            AhBuyCandidate const& c = candidates[i];
            double const unit = double(c.buyout) / double(c.stack);
            if (c.stack <= remaining)
            {
                if (fit < 0 || unit < fitUnit
                    || (unit == fitUnit && c.stack > candidates[fit].stack))
                {
                    fit = int(i);
                    fitUnit = unit;
                }
            }
            else
            {
                uint32 const excess = c.stack - remaining;
                if (over < 0 || excess < overExcess
                    || (excess == overExcess && unit < overUnit))
                {
                    over = int(i);
                    overExcess = excess;
                    overUnit = unit;
                }
            }
        }

        overbuy = fit < 0;
        return fit >= 0 ? fit : over;
    }

    // The buyout branch of WorldSession::HandleAuctionPlaceBid, minus the
    // auctioneer-interaction check. `auction` is deleted by RemoveAuction, so
    // the caller must not touch it afterwards.
    void ExecuteAhBuyout(Player* buyer, AuctionHouseObject* house, AuctionEntry* auction)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        if (buyer->GetGUID() == auction->bidder)
            buyer->ModifyMoney(-int32(auction->buyout - auction->bid));
        else
        {
            buyer->ModifyMoney(-int32(auction->buyout));
            if (auction->bidder)
                sAuctionMgr->SendAuctionOutbiddedMail(auction, auction->buyout, buyer, trans);
        }

        auction->bidder = buyer->GetGUID();
        auction->bid    = auction->buyout;
        buyer->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_HIGHEST_AUCTION_BID,
                                         auction->buyout);

        sAuctionMgr->SendAuctionSalePendingMail(auction, trans);
        sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
        sAuctionMgr->SendAuctionWonMail(auction, trans);
        sScriptMgr->OnAuctionSuccessful(house, auction);

        auction->DeleteFromDB(trans);
        sAuctionMgr->RemoveAItem(auction->item_guid);
        house->RemoveAuction(auction);

        buyer->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
    }

    bool AhBuyNameEquals(std::string const& tmplName, std::string_view wanted)
    {
        if (tmplName.size() != wanted.size()) return false;
        for (std::size_t i = 0; i < tmplName.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(tmplName[i]))
                != std::tolower(static_cast<unsigned char>(wanted[i])))
                return false;
        return true;
    }

    // The client sends the entry id when it has one. A row pasted as plain guide
    // text for an item nobody in the party owns has none, so fall back to the
    // exact name — and because item names are not unique, break a tie towards
    // whichever of them is actually listed on this Auction House.
    uint32 ResolveAhBuyItem(uint32 entryHint, std::string_view name, AuctionHouseObject* house)
    {
        if (entryHint && sObjectMgr->GetItemTemplate(entryHint))
            return entryHint;
        if (name.empty())
            return 0;

        std::vector<uint32> matches;
        for (auto const& [id, tmpl] : *sObjectMgr->GetItemTemplateStore())
            if (AhBuyNameEquals(tmpl.Name1, name))
                matches.push_back(id);

        if (matches.size() <= 1)
            return matches.empty() ? 0 : matches.front();

        std::unordered_map<uint32, uint32> listings;
        for (uint32 id : matches)
            listings[id] = 0;
        for (auto const& [auctionId, auction] : house->GetAuctions())
            if (auction && auction->buyout)
            {
                auto itr = listings.find(auction->item_template);
                if (itr != listings.end())
                    ++itr->second;
            }

        uint32 best = matches.front(), bestListings = 0;
        for (uint32 id : matches)
            if (listings[id] > bestListings)
            {
                best = id;
                bestListings = listings[id];
            }
        return best;
    }
}

// AH_BUY\t<seq>\t<itemEntry>\t<wantCount>\t<itemName>
//   Buys out the cheapest listings of one shopping-list item on the requester's
//   own faction Auction House until <wantCount> units are secured. <itemEntry>
//   may be 0 for an item the client has never cached; <itemName> then resolves
//   it. No auctioneer is required (same as AH_SELL) and the requester pays, so
//   the shared-gold hook mirrors the spend across the pool. Everything bought
//   arrives in the requester's mailbox, exactly as a clicked buyout does.
//   <seq> is echoed untouched so a reply belonging to an aborted pass can be
//   recognised and dropped rather than credited to whatever row is current.
//
// Reply: AH_BUYRES\t<seq>\t<itemEntry>\t<boughtUnits>\t<spentCopper>\t<status>
//   full  — the requested count is covered
//   short — bought some, then ran out of listings (or hit the per-request cap)
//   none  — nothing suitable is listed
//   money — stopped because the next stack costs more than the requester has
//   err   — malformed request, unknown item, or no reachable Auction House
static void HandleAhBuy(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;

    auto field = [&payload](std::size_t& from) -> std::string_view
    {
        auto const tab = payload.find('\t', from);
        auto const out = payload.substr(from, tab == std::string_view::npos
            ? std::string_view::npos : tab - from);
        from = tab == std::string_view::npos ? payload.size() : tab + 1;
        return out;
    };
    auto number = [](std::string_view sv)
    { return uint32(std::strtoul(std::string(sv).c_str(), nullptr, 10)); };

    std::size_t cursor = 0;
    uint32 const seq       = number(field(cursor));
    uint32 const entryHint = number(field(cursor));
    uint32 want            = number(field(cursor));
    std::string_view const name = payload.substr(std::min(cursor, payload.size()));

    auto reply = [requester, seq](uint32 entry, uint32 bought, uint64 spent, char const* status)
    {
        SendWPSP(requester, Acore::StringFormat("AH_BUYRES\t{}\t{}\t{}\t{}\t{}",
            seq, entry, bought, spent, status));
    };

    if (!want) { reply(entryHint, 0, 0, "err"); return; }
    want = std::min(want, AH_BUY_MAX_COUNT);

    AuctionHouseObject* house = sAuctionMgr->GetAuctionsMap(requester->GetFaction());
    if (!house) { reply(entryHint, 0, 0, "err"); return; }

    uint32 const itemEntry = ResolveAhBuyItem(entryHint, name, house);
    ItemTemplate const* tmpl = itemEntry ? sObjectMgr->GetItemTemplate(itemEntry) : nullptr;
    if (!tmpl)
    {
        LOG_INFO("module", "[WowPsParty AhBuy] {} asked for x{} of an unresolvable item (entry={} name='{}')",
            requester->GetName(), want, entryHint, std::string(name));
        reply(entryHint, 0, 0, "err");
        return;
    }

    std::vector<AhBuyCandidate> candidates;
    CollectAhBuyCandidates(house, requester, itemEntry, candidates);

    uint32 bought = 0;
    uint64 spent  = 0;
    bool outOfMoney = false;

    for (uint32 round = 0; round < AH_BUY_MAX_AUCTIONS && bought < want; ++round)
    {
        bool overbuy = false;
        int const pick = PickAhBuyCandidate(candidates, want - bought, overbuy);
        if (pick < 0) break;

        AhBuyCandidate const chosen = candidates[pick];
        candidates.erase(candidates.begin() + pick);

        // Re-read: a rival buyer or the expiry pass may have taken this listing
        // between the snapshot and now, and AddAuction reuses no ids.
        AuctionEntry* auction = house->GetAuction(chosen.id);
        if (!auction || auction->item_template != itemEntry
            || auction->buyout != chosen.buyout || auction->itemCount != chosen.stack)
            continue;

        // The same veto the real bid handler honours (restricted accounts, and
        // whatever else a module hangs off it).
        if (!sScriptMgr->OnPlayerCanPlaceAuctionBid(requester, auction))
        {
            LOG_INFO("module", "[WowPsParty AhBuy] {} vetoed by OnPlayerCanPlaceAuctionBid on auction {}",
                requester->GetName(), chosen.id);
            break;
        }

        if (!requester->HasEnoughMoney(chosen.buyout)) { outOfMoney = true; break; }

        ExecuteAhBuyout(requester, house, auction);
        bought += chosen.stack;
        spent  += chosen.buyout;

        LOG_INFO("module", "[WowPsParty AhBuy] {} bought {} x '{}'(entry={}) for {} copper (auction {})",
            requester->GetName(), chosen.stack, tmpl->Name1, itemEntry, chosen.buyout, chosen.id);

        // The overshoot stack already covers the shortfall; buying past it would
        // be waste the manual picker would never commit.
        if (overbuy) break;
    }

    char const* status = outOfMoney ? "money"
                       : bought >= want ? "full"
                       : bought > 0 ? "short" : "none";

    LOG_INFO("module", "[WowPsParty AhBuy] {} requested x{} '{}'(entry={}): bought={} spent={} status={}",
        requester->GetName(), want, tmpl->Name1, itemEntry, bought, spent, status);

    reply(itemEntry, bought, spent, status);
}

// Forward declaration: PULL_TOOLS uses the same preserve-or-bounce transfer
// primitive as the later inventory actions.
static Item* PullItemToRequester(Player* requester, Player* srcChar, Item* item);

// Return one loose bag item owned by `owner` that fulfils either a specific
// profession-tool item requirement or a profession-tool category. The caller
// has already confirmed both players share a Map*, so their inventories are
// serialized by the same MapUpdater worker. Banked/equipped tools deliberately
// do not qualify: native profession windows only see the crafter's bags.
static Item* FindPullableProfessionTool(Player* owner, uint32 itemId, uint32 totemCategory)
{
    if (!owner) return nullptr;
    auto matches = [owner, itemId, totemCategory](Item* item)
    {
        if (!item || item->IsEquipped() || item->IsNotEmptyBag()) return false;
        ItemTemplate const* proto = WowPsParty::SafeItemTemplate(item);
        if (!proto) return false;
        return (itemId && proto->ItemId == itemId)
            || (totemCategory && owner->IsTotemCategoryCompatiableWith(proto, totemCategory));
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = owner->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (matches(item)) return item;

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = owner->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (Item* item = bag->GetItemByPos(slot))
                    if (matches(item)) return item;

    return nullptr;
}

// Pull one tool that satisfies `itemId` or `totemCategory` from a loaded,
// same-map hero. Unlike the server spell trampoline, this deliberately moves
// the tool so the native client profession UI can observe it and enable Create.
static bool PullProfessionTool(Player* requester, uint32 account, uint32 itemId, uint32 totemCategory)
{
    QueryResult members = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {}", account);
    if (!members) return false;

    do
    {
        uint32 const memberGuid = members->Fetch()[0].Get<uint32>();
        if (memberGuid == requester->GetGUID().GetCounter()) continue;
        Player* owner = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(memberGuid));
        if (!owner || WowPsParty::MemberStorageUnstable(owner)) continue;
        if (owner->GetMap() != requester->GetMap())
        {
            LOG_INFO("module", "[WowPsParty PullTools] requester={} requirement={}{} holder={} result=skipped reason=other-map",
                     requester->GetName(), itemId ? "item=" : "category=",
                     itemId ? itemId : totemCategory, owner->GetName());
            continue;
        }

        Item* tool = FindPullableProfessionTool(owner, itemId, totemCategory);
        if (!tool) continue;
        ItemTemplate const* proto = WowPsParty::SafeItemTemplate(tool);
        std::string const toolName = proto ? proto->Name1 : "tool";
        if (!PullItemToRequester(requester, owner, tool))
        {
            LOG_WARN("module", "[WowPsParty PullTools] requester={} requirement={}{} holder={} result=failed reason=crafter-bags-full",
                     requester->GetName(), itemId ? "item=" : "category=",
                     itemId ? itemId : totemCategory, owner->GetName());
            return false;
        }

        LOG_INFO("module", "[WowPsParty PullTools] requester={} requirement={}{} holder={} tool='{}' result=pulled",
                 requester->GetName(), itemId ? "item=" : "category=",
                 itemId ? itemId : totemCategory, owner->GetName(), toolName);
        return true;
    } while (members->NextRow());

    LOG_INFO("module", "[WowPsParty PullTools] requester={} requirement={}{} result=not-found scope=loaded-same-map-bags",
             requester->GetName(), itemId ? "item=" : "category=", itemId ? itemId : totemCategory);
    return false;
}

// PULL_TOOLS\t<recipeSpellId>
//   Recipe tools are not exposed by the 3.3.5 client as item links — only as a
//   display string — so the addon sends the selected recipe spell and the server
//   resolves its Totem[] (specific tool) and TotemCategory[] requirements. Pull
//   one matching tool for each missing requirement into the crafter's bags; this
//   is what makes the native "Requires <tool>" line and Create button update.
static void HandlePullTools(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    uint32 const spellId = std::strtoul(std::string(payload).c_str(), nullptr, 10);
    if (!spellId || !requester->HasSpell(spellId))
    {
        LOG_INFO("module", "[WowPsParty PullTools] requester={} recipe={} result=rejected reason=unknown-spell",
                 requester->GetName(), spellId);
        return;
    }
    SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
    if (!spell || !spell->HasAttribute(SPELL_ATTR0_IS_TRADESKILL))
    {
        LOG_INFO("module", "[WowPsParty PullTools] requester={} recipe={} result=rejected reason=not-tradeskill",
                 requester->GetName(), spellId);
        return;
    }

    uint32 const account = requester->GetSession()->GetAccountId();
    if (!WowPsParty::GetAccountSettings(account).sharedInventory)
    {
        LOG_INFO("module", "[WowPsParty PullTools] requester={} recipe={} result=skipped reason=shared-inventory-disabled",
                 requester->GetName(), spellId);
        return;
    }

    for (uint32 itemId : spell->Totem)
        if (itemId && !requester->HasItemCount(itemId))
            PullProfessionTool(requester, account, itemId, 0);
    for (uint32 totemCategory : spell->TotemCategory)
        if (totemCategory && !requester->HasItemTotemCategory(totemCategory))
            PullProfessionTool(requester, account, 0, totemCategory);

    WowPsParty::SendInventoryTo(requester);
}

// PULL_REAGENT\t<itemId>
//   Consolidate every copy of <itemId> from the OTHER party members' bags into
//   the requester's (the crafting character's) own bags. The native tradeskill
//   window counts only the crafter's OWN inventory, so without this the shared
//   reagents are invisible and "Create" stays greyed at 0/N; sliding them onto
//   the crafter makes the stock window enable crafting normally. Idempotent — a
//   re-pull moves nothing once everything is consolidated. Preserve-or-bounce
//   cross-character move (same as equip/move), so an item is never lost.
static void HandlePullReagent(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    uint32 const itemId = std::strtoul(std::string(payload).c_str(), nullptr, 10);
    if (!itemId) return;

    uint32 const account = requester->GetSession()->GetAccountId();
    bool const sharedInv = WowPsParty::GetAccountSettings(account).sharedInventory;
    LOG_INFO("module", "[WowPsParty PullReagent] {} (acct {}) requested itemId={} sharedInv={}",
        requester->GetName(), account, itemId, sharedInv ? 1 : 0);
    if (!sharedInv) return;

    QueryResult q = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {}", account);
    if (!q) return;

    uint32 moved = 0;
    do
    {
        uint32 const charGuid = q->Fetch()[0].Get<uint32>();
        if (charGuid == requester->GetGUID().GetCounter()) continue;
        Player* mate = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(charGuid));
        if (!mate || mate == requester) continue;

        std::vector<Item*> stacks;
        auto consider = [&](Item* it) {
            if (it && !it->IsEquipped() && !it->IsNotEmptyBag() && it->GetEntry() == itemId)
                stacks.push_back(it);
        };
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            consider(mate->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = mate->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    consider(bag->GetItemByPos(j));

        for (Item* it : stacks)
        {
            mate->MoveItemFromInventory(it->GetBagSlot(), it->GetSlot(), true);
            it->SetOwnerGUID(requester->GetGUID());
            it->FSetState(ITEM_CHANGED);
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            it->SaveToDB(tx);
            CharacterDatabase.CommitTransaction(tx);

            ItemPosCountVec dest;
            if (requester->CanStoreItem(NULL_BAG, NULL_SLOT, dest, it, false) == EQUIP_ERR_OK)
            {
                requester->MoveItemToInventory(dest, it, true);
                ++moved;
            }
            else
            {
                // Crafter's bags are full — hand it straight back so it never strands.
                it->SetOwnerGUID(mate->GetGUID());
                it->FSetState(ITEM_CHANGED);
                ItemPosCountVec back;
                if (mate->CanStoreItem(NULL_BAG, NULL_SLOT, back, it, false) == EQUIP_ERR_OK)
                    mate->MoveItemToInventory(back, it, true);
                CharacterDatabaseTransaction tx2 = CharacterDatabase.BeginTransaction();
                it->SaveToDB(tx2);
                CharacterDatabase.CommitTransaction(tx2);
            }
        }
        // Persist this mate + the crafter now (don't touch `it`: a reagent stack
        // may have merged into an existing one on the requester and been freed).
        if (!stacks.empty())
            FlushPartyTransfer(mate, requester);
    } while (q->NextRow());

    LOG_INFO("module", "[WowPsParty PullReagent] itemId={} -> moved {} stack(s) onto {}",
        itemId, moved, requester->GetName());
    if (moved) WowPsParty::SendInventoryTo(requester);
}

// DESTROY\t<srcPartySlot>\t<srcItemGuidLow>
// Permanently destroys an item from a party member's bags. The addon shows a
// confirmation popup first (Ctrl+Right-Click). Refuses equipped items and
// non-empty bags, same guards as HandleSell.
static void HandleDestroy(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const srcSlot = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);
    if (!srcItemGuidLow) return;

    uint32 const account = requester->GetSession()->GetAccountId();
    QueryResult q = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}", account, srcSlot);
    if (!q) return;
    uint32 const srcCharGuid = q->Fetch()[0].Get<uint32>();
    Player* srcChar = ObjectAccessor::FindConnectedPlayer(
        ObjectGuid::Create<HighGuid::Player>(srcCharGuid));
    if (!srcChar) return;

    Item* srcItem = SafeGetItemByGuid(srcChar, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!srcItem) return;
    if (srcItem->IsEquipped()) return;
    if (srcItem->IsNotEmptyBag())
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Empty the bag before destroying it.");
        return;
    }

    std::string const name = srcItem->GetTemplate() ? srcItem->GetTemplate()->Name1 : "item";
    srcChar->DestroyItem(srcItem->GetBagSlot(), srcItem->GetSlot(), true);
    ChatHandler(requester->GetSession()).PSendSysMessage(
        "|cff66ccff[WowPsParty]|r Destroyed |cffffffff{}|r.", name);
    WowPsParty::SendInventoryTo(requester);
}

// Build quality-coloured, clickable chat links for each DISTINCT item pulled from
// a container ("2x [Netherweave Cloth]"), so opening a bag tells the player exactly
// what landed in their bags instead of a bare "+N item(s)" count. The link format
// mirrors the party loot feed in PartyHooks.cpp.
static std::vector<std::string> BuildLootedItemLinks(
    std::vector<std::pair<uint32, uint32>> const& idCounts)
{
    std::vector<std::string> links;
    links.reserve(idCounts.size());
    for (auto const& [itemId, count] : idCounts)
    {
        ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(itemId);
        std::string const name = tmpl ? tmpl->Name1 : "Unknown Item";
        uint32 quality = tmpl ? tmpl->Quality : uint32(ITEM_QUALITY_NORMAL);
        if (quality >= MAX_ITEM_QUALITY) quality = ITEM_QUALITY_NORMAL;   // guard a custom OOB quality
        uint32 const color = ItemQualityColors[quality];
        links.push_back(Acore::StringFormat(
            "{}x |c{:08x}|Hitem:{}::::::::1::::|h[{}]|h|r", count, color, itemId, name));
    }
    return links;
}

// Chat-report a container open, naming what dropped. A single line when the drop is
// small, otherwise a header + one line per item — a 3.3.5a SMSG_MESSAGECHAT truncates
// around ~255 bytes and item links are long, so a fat satchel would otherwise lose
// its tail. Gold (if any) leads the list.
static void AnnounceContainerOpen(Player* player, std::string const& satchelName,
                                  std::string const& ownerName,
                                  std::vector<std::string> const& itemLinks,
                                  std::string const& goldText)
{
    ChatHandler handler(player->GetSession());
    std::string const header = Acore::StringFormat(
        "|cff66ccff[WowPsParty]|r Opened |cffffffff{}|r on {}", satchelName, ownerName);

    std::string tail = goldText;
    for (std::string const& link : itemLinks)
    {
        if (!tail.empty()) tail += ", ";
        tail += link;
    }
    if (tail.empty())
        tail = "nothing";

    if (itemLinks.size() <= 2 && header.size() + tail.size() + 2 < 240)
    {
        handler.PSendSysMessage("{}: {}", header, tail);
        return;
    }
    handler.PSendSysMessage("{}:", header);
    if (!goldText.empty())
        handler.PSendSysMessage("  |cff66ccff+|r {}", goldText);
    for (std::string const& link : itemLinks)
        handler.PSendSysMessage("  |cff66ccff+|r {}", link);
}

// Open a LOOTABLE container (Satchel of Helpful Goods, the random-dungeon reward
// bag, lootable pouches, …) ON ITS OWNER. These are BoP/unique and bound to the
// char that earned them, so they can't be moved to the requester (that fails the
// unique/bind check → a bogus "no room"). For the active char the loot window
// just pops; for an alt/bot owner (no client to click a window) we auto-loot the
// contents into ITS bags so nothing is stranded — it surfaces in the shared grid
// either way. Returns true if it CLAIMED the item (was a lootable container, even
// if blocked by death/lock), false if the item isn't a lootable container at all
// (so the caller can fall through to its own handling).
static bool OpenLootableContainer(Player* requester, Player* srcChar, Item* srcItem)
{
    if (!requester || !requester->GetSession() || !srcChar || !srcItem) return false;
    ItemTemplate const* t = srcItem->GetTemplate();
    if (!t) return false;
    if (!t->HasFlag(ITEM_FLAG_HAS_LOOT) || srcItem->IsWrapped())
        return false;   // not a lootable container — caller handles it

    LOG_INFO("module",
        "[WowPsParty Open] {} opening container entry={} '{}' owner={} alive={} lockId={} locked={}",
        requester->GetName(), t->ItemId, t->Name1, srcChar->GetName(),
        srcChar->IsAlive(), t->LockID, srcItem->IsLocked());

    if (!srcChar->IsAlive())
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Can't open |cffffffff{}|r — its owner is dead.", t->Name1);
        return true;
    }
    // ONLY gate on the lock when the item actually HAS a lock (LockID). A lock-less
    // satchel still reports IsLocked() (the UNLOCKED bit is unset) but opens fine —
    // the engine only checks the lock if LockID != 0, so the old unconditional
    // IsLocked() check produced the bogus "is locked".
    if (t->LockID && srcItem->IsLocked())
    {
        // A Lockpicking-locked box: hand off to the party rogue (the same core the Ctrl+K
        // PICKLOCK keybind uses) so right-clicking a lockbox just opens it when a party rogue
        // has the skill, instead of dead-ending. A non-lockpicking lock (key required) isn't
        // pickable, so TryPartyPickLock returns false and we report it locked as before.
        if (TryPartyPickLock(requester, srcChar, srcItem))
            return true;
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r |cffffffff{}|r is locked.", t->Name1);
        return true;
    }

    // Fill the satchel's loot from its template, then AUTO-LOOT it into the
    // OWNER's bags — for EVERYONE, the active char included. We deliberately do
    // NOT rely on the client loot window: the open came from the shared-inventory
    // addon, not a CMSG_OPEN_ITEM, so the client never initiated a loot and an
    // unsolicited SendLoot window doesn't reliably appear. That's exactly why the
    // active char's OWN satchels "wouldn't open" while alts (which always
    // auto-looted) did. Server-side auto-loot is deterministic for all of them.
    // (Satchel of Helpful Goods has 3-4 loot groups, so several items per open is
    // expected, not a bug.)
    srcChar->SendLoot(srcItem->GetGUID(), LOOT_CORPSE);
    Loot& loot = srcItem->loot;
    std::string const ownerName = srcChar->GetName();
    std::string const satchelName = t->Name1;

    // ALL-OR-NOTHING. Store every loot item; if any doesn't fit, roll back the ones
    // that did and leave the container intact for a retry. Storing only SOME while
    // keeping the container let you re-open it to re-roll the rest — an infinite
    // dupe. Gold is credited only on full success. (CanStoreNewItem is checked
    // immediately before each store, so slots consumed by earlier items count.)
    std::vector<Item*> created;
    std::vector<std::pair<uint32, uint32>> lootedSummary;   // (itemId, total count), in drop order
    bool allStored = true;
    for (LootItem& li : loot.items)
    {
        if (li.is_looted) continue;
        ItemPosCountVec dst;
        if (srcChar->CanStoreNewItem(NULL_BAG, NULL_SLOT, dst, li.itemid, li.count) != EQUIP_ERR_OK)
        { allStored = false; break; }
        Item* it = srcChar->StoreNewItem(dst, li.itemid, true, li.randomPropertyId);
        if (!it) { allStored = false; break; }
        li.is_looted = true;
        created.push_back(it);

        auto summary = std::find_if(lootedSummary.begin(), lootedSummary.end(),
            [&](auto const& e) { return e.first == li.itemid; });
        if (summary != lootedSummary.end())
            summary->second += li.count;
        else
            lootedSummary.push_back({ li.itemid, li.count });   // li.count is a bitfield — read by value
    }
    if (!allStored)
    {
        for (Item* it : created)   // undo the partial loot so nothing can be re-rolled
            srcChar->DestroyItem(it->GetBagSlot(), it->GetSlot(), true);
        srcChar->SendLootRelease(srcItem->GetGUID());
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r {}'s bags are full — free a slot, then open |cffffffff{}|r again.",
            ownerName, satchelName);
        WowPsParty::SendInventoryTo(requester);
        return true;
    }
    uint32 const lootedGold = loot.gold;
    if (loot.gold) { srcChar->ModifyMoney(int32(loot.gold)); loot.gold = 0; }
    srcChar->SendLootRelease(srcItem->GetGUID());
    // Fully looted -> the container is consumed on open, so it can never be
    // re-opened to dupe its contents.
    srcChar->DestroyItem(srcItem->GetBagSlot(), srcItem->GetSlot(), true);

    std::string goldText;
    if (lootedGold)
        goldText = Acore::StringFormat("{}g {}s {}c",
            lootedGold / 10000, (lootedGold % 10000) / 100, lootedGold % 100);
    AnnounceContainerOpen(requester, satchelName, ownerName,
        BuildLootedItemLinks(lootedSummary), goldText);
    WowPsParty::SendInventoryTo(requester);
    return true;
}

// Apply a GLYPH item to `target` in the first FREE slot whose type (major/minor)
// matches and is level-unlocked, then consume one glyph from `itemOwner`. A glyph
// item's on-use spell APPLIES the glyph, but a plain CastSpell always uses slot 0
// (m_glyphIndex defaults to 0) — that's the "every glyph lands in the first slot,
// can only equip one" bug. Mirrors Spell::EffectApplyGlyph for an empty slot.
static void ApplyGlyphFromItem(Player* target, Player* itemOwner, Item* glyphItem, uint32 glyphId)
{
    if (!target || !target->GetSession() || !itemOwner || !glyphItem) return;
    GlyphPropertiesEntry const* gp = sGlyphPropertiesStore.LookupEntry(glyphId);
    if (!gp) return;

    // Per-slot unlock level (Spell::EffectApplyGlyph): slots 0/1=15, 3=30, 2=50,
    // 4=70, 5=80. Indexed by slot.
    static uint8 const SLOT_MIN_LEVEL[MAX_GLYPH_SLOT_INDEX] = { 15, 15, 50, 30, 70, 80 };
    int slot = -1;
    for (uint8 i = 0; i < MAX_GLYPH_SLOT_INDEX; ++i)
    {
        if (target->GetLevel() < SLOT_MIN_LEVEL[i]) continue;   // socket not unlocked yet
        if (target->GetGlyph(i) != 0) continue;                 // already occupied
        GlyphSlotEntry const* gs = sGlyphSlotStore.LookupEntry(target->GetGlyphSlot(i));
        if (!gs || gs->TypeFlags != gp->TypeFlags) continue;    // major/minor mismatch
        slot = int(i);
        break;
    }
    std::string const glyphName = glyphItem->GetTemplate() ? glyphItem->GetTemplate()->Name1 : "glyph";
    if (slot < 0)
    {
        ChatHandler(target->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r No free matching glyph slot for |cffffffff{}|r "
            "(that type is full, or not unlocked at your level).", glyphName);
        return;
    }

    // Apply (empty slot, so no old glyph to strip) — same sequence as the core.
    target->SendLearnPacket(gp->SpellId, true);
    target->CastSpell(target, gp->SpellId,
        TriggerCastFlags(TRIGGERED_FULL_MASK & ~(TRIGGERED_IGNORE_SHAPESHIFT | TRIGGERED_IGNORE_CASTER_AURASTATE)));
    target->SetGlyph(uint8(slot), glyphId, true);
    target->SendTalentsInfoData(false);

    // Consume one glyph from the owner.
    if (glyphItem->GetCount() > 1)
    {
        glyphItem->SetCount(glyphItem->GetCount() - 1);
        glyphItem->SetState(ITEM_CHANGED, itemOwner);
    }
    else
        itemOwner->DestroyItem(glyphItem->GetBagSlot(), glyphItem->GetSlot(), true);

    ChatHandler(target->GetSession()).PSendSysMessage(
        "|cff66ccff[WowPsParty]|r Applied |cffffffff{}|r to glyph slot {}.", glyphName, slot + 1);
    WowPsParty::SendInventoryTo(target);
}

// BANK\t<srcPartySlot>\t<srcItemGuidLow> — deposit a shared-bag item into the
// REQUESTER's own bank. The item may live in a mate's bags, so re-own it to the
// requester first, then bank it (CanBankItem/BankItem) — the same primitives the
// core's auto-bank uses. No banker NPC is required: the shared grid is already a
// non-physical convenience. Bounces the item back to its owner if the bank is full.
static Item* PullItemToRequester(Player* requester, Player* srcChar, Item* item);   // defined below

static void HandleBankDeposit(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const srcSlot        = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);
    if (!srcItemGuidLow) return;

    uint32 const account = requester->GetSession()->GetAccountId();
    QueryResult q = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}", account, srcSlot);
    if (!q) return;
    Player* srcChar = ObjectAccessor::FindConnectedPlayer(
        ObjectGuid::Create<HighGuid::Player>(q->Fetch()[0].Get<uint32>()));
    if (!srcChar) return;
    Item* item = SafeGetItemByGuid(srcChar, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!item) return;
    ItemTemplate const* t = item->GetTemplate();
    std::string const itemName = t ? t->Name1 : "item";

    // Land the item in the REQUESTER's own bags first. PullItemToRequester does the
    // cross-char move safely and bounces it back to its owner if the requester has no
    // room, so the deposit below is always the simple, canonical same-player path. (For
    // an item already on the requester it's a no-op returning the same item.)
    Item* owned = PullItemToRequester(requester, srcChar, item);
    if (!owned)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Couldn't move |cffffffff{}|r to your bags.", itemName);
        WowPsParty::SendInventoryTo(requester);
        if (srcChar != requester) WowPsParty::SendInventoryTo(srcChar);
        return;
    }

    // Mirror WorldSession::HandleAutoBankItemOpcode EXACTLY: compute the bank slot, then
    // REMOVE the item from the bag BEFORE BankItem. Storing it while it is still
    // registered in the bag double-references the Item* into two slots and corrupts its
    // count — that is the "deposited belt shows as a 2-stack and doesn't actually move"
    // bug, and the dangling Item* that SendInventoryTo had to SEH-guard against.
    ItemPosCountVec dest;
    InventoryResult const msg = requester->CanBankItem(NULL_BAG, NULL_SLOT, dest, owned, false);
    if (msg == EQUIP_ERR_OK && !(dest.size() == 1 && dest[0].pos == owned->GetPos()))
    {
        requester->RemoveItem(owned->GetBagSlot(), owned->GetSlot(), true);
        requester->ItemRemovedQuestCheck(owned->GetEntry(), owned->GetCount());
        requester->BankItem(dest, owned, true);
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Banked |cffffffff{}|r.", itemName);
    }
    else
    {
        // Bank full (or it was already there) — leave it in the requester's bags; it was
        // moved there safely above, so nothing strands and nothing duplicates.
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Your bank is full — couldn't deposit |cffffffff{}|r.", itemName);
    }
    WowPsParty::SendInventoryTo(requester);
    if (srcChar != requester) WowPsParty::SendInventoryTo(srcChar);
}

// "Ng Ms Kc" for a copper amount.
static std::string FormatMoneyGSC(uint32 copper)
{
    uint32 const g = copper / 10000, s = (copper % 10000) / 100, c = copper % 100;
    std::ostringstream o;
    if (g) o << g << "g ";
    if (g || s) o << s << "s ";
    o << c << "c";
    return o.str();
}

// Buy the NEXT bank bag slot for the requester — banker-less, the same convenience the
// remote deposit already is. Mirrors WorldSession::HandleBuyBankSlotOpcode (price from
// BankBagSlotPrices.dbc + gold check) without the banker/CanUseBank requirement.
static void HandleBuyBankSlot(Player* requester)
{
    if (!requester || !requester->GetSession()) return;
    uint32 const slot = uint32(requester->GetBankBagSlotCount()) + 1;   // next slot, 1-based
    BankBagSlotPricesEntry const* e = sBankBagSlotPricesStore.LookupEntry(slot);
    if (!e)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r All bank bag slots are already unlocked.");
        return;
    }
    if (!requester->HasEnoughMoney(uint32(e->price)))
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Not enough gold for bank bag slot {} (costs {}).",
            slot, FormatMoneyGSC(e->price));
        return;
    }
    requester->SetBankBagSlotCount(uint8(slot));
    requester->ModifyMoney(-int32(e->price));
    ChatHandler(requester->GetSession()).PSendSysMessage(
        "|cff66ccff[WowPsParty]|r Unlocked bank bag slot {} for {}. Drop a bag onto it.",
        slot, FormatMoneyGSC(e->price));
    WowPsParty::SendInventoryTo(requester);
}

// Place a bag from a party member's bags into the requester's next FREE (purchased) bank
// bag slot. Banker-less. Pulls the bag onto the requester first (safe cross-char move +
// bounce-back), then stores it into the specific bank bag slot the canonical way
// (CanBankItem -> RemoveItem -> BankItem), so it never double-references the Item*.
static void HandleBankBag(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const srcSlot        = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);
    if (!srcItemGuidLow) return;

    uint32 const account = requester->GetSession()->GetAccountId();
    QueryResult q = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}", account, srcSlot);
    if (!q) return;
    Player* srcChar = ObjectAccessor::FindConnectedPlayer(
        ObjectGuid::Create<HighGuid::Player>(q->Fetch()[0].Get<uint32>()));
    if (!srcChar) return;
    Item* item = SafeGetItemByGuid(srcChar, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!item) return;
    ItemTemplate const* t = item->GetTemplate();
    std::string const itemName = t ? t->Name1 : "item";
    if (!item->IsBag())
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r |cffffffff{}|r isn't a bag.", itemName);
        return;
    }
    // Only an EMPTY bag can be moved into a bank bag slot. PullItemToRequester routes
    // through CanStoreItem(NULL_BAG,NULL_SLOT), which rejects a non-empty bag — both its
    // store AND bounce-back would fail, stranding the bag off its owner. Guard up front.
    if (item->IsNotEmptyBag())
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Empty |cffffffff{}|r first — only an empty bag can go "
            "into a bank bag slot.", itemName);
        return;
    }

    Item* owned = PullItemToRequester(requester, srcChar, item);
    if (!owned)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Couldn't move |cffffffff{}|r to your bags.", itemName);
        WowPsParty::SendInventoryTo(requester);
        if (srcChar != requester) WowPsParty::SendInventoryTo(srcChar);
        return;
    }

    // First free PURCHASED bank bag slot (67.. up to GetBankBagSlotCount()).
    uint8 target = 0; bool haveTarget = false;
    for (uint8 s = BANK_SLOT_BAG_START; s < BANK_SLOT_BAG_END; ++s)
    {
        if (uint32(s - BANK_SLOT_BAG_START) >= requester->GetBankBagSlotCount()) break;  // not purchased
        if (!requester->GetItemByPos(INVENTORY_SLOT_BAG_0, s)) { target = s; haveTarget = true; break; }
    }

    ItemPosCountVec dest;
    if (haveTarget
        && requester->CanBankItem(INVENTORY_SLOT_BAG_0, target, dest, owned, false) == EQUIP_ERR_OK)
    {
        requester->RemoveItem(owned->GetBagSlot(), owned->GetSlot(), true);
        requester->BankItem(dest, owned, true);
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Added |cffffffff{}|r to a bank bag slot.", itemName);
    }
    else
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r No free bank bag slot for |cffffffff{}|r — buy a slot first, "
            "or empty one. It stays in your bags.", itemName);
    }
    WowPsParty::SendInventoryTo(requester);
    if (srcChar != requester) WowPsParty::SendInventoryTo(srcChar);
}

// Move one item from a party member's bags onto the requester, preserving it on
// failure (bounce back, never stranded). Returns the item now in the requester's
// inventory (possibly a merged stack), or nullptr if the requester had no room.
// Mirrors the cross-character transfer used by PULL_REAGENT / MOVE / EQUIP.
static Item* PullItemToRequester(Player* requester, Player* srcChar, Item* item)
{
    if (!requester || !srcChar || !item) return nullptr;
    if (srcChar == requester) return item;   // already the requester's

    srcChar->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
    item->SetOwnerGUID(requester->GetGUID());
    item->FSetState(ITEM_CHANGED);
    {
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        item->SaveToDB(tx);
        CharacterDatabase.CommitTransaction(tx);
    }
    ItemPosCountVec dest;
    if (requester->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) == EQUIP_ERR_OK)
    {
        ObjectGuid const movedGuid = item->GetGUID();   // MoveItemToInventory returns void
        requester->MoveItemToInventory(dest, item, true);
        FlushPartyTransfer(srcChar, requester);
        return SafeGetItemByGuid(requester, movedGuid);      // count-1 items don't merge; pointer stays valid
    }

    // Requester's bags are full — hand it straight back so it never strands.
    item->SetOwnerGUID(srcChar->GetGUID());
    item->FSetState(ITEM_CHANGED);
    ItemPosCountVec back;
    if (srcChar->CanStoreItem(NULL_BAG, NULL_SLOT, back, item, false) == EQUIP_ERR_OK)
        srcChar->MoveItemToInventory(back, item, true);
    CharacterDatabaseTransaction tx2 = CharacterDatabase.BeginTransaction();
    item->SaveToDB(tx2);
    CharacterDatabase.CommitTransaction(tx2);
    FlushPartyTransfer(srcChar, requester);
    return nullptr;
}

// Deposit one party-inventory item into the requester's GUILD bank. Re-owns the
// item onto the requester (a guild member), then auto-places it in the first tab
// the requester may deposit to — SwapItemsWithInventory with NULL_SLOT scans that
// tab for a free/stackable slot. Bounces back to the source char on failure so an
// item is never stranded. (Mirrors HandleBankDeposit, but the guild bank has no
// player-side auto-store, so it routes through the Guild API.)
static void HandleGuildBankDeposit(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const srcSlot        = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);
    if (!srcItemGuidLow) return;

    Guild* guild = sGuildMgr->GetGuildById(requester->GetGuildId());
    if (!guild)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r You're not in a guild.");
        return;
    }

    uint32 const account = requester->GetSession()->GetAccountId();
    QueryResult q = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}", account, srcSlot);
    if (!q) return;
    Player* srcChar = ObjectAccessor::FindConnectedPlayer(
        ObjectGuid::Create<HighGuid::Player>(q->Fetch()[0].Get<uint32>()));
    if (!srcChar) return;
    Item* item = SafeGetItemByGuid(srcChar, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!item) return;
    ItemTemplate const* t = item->GetTemplate();
    std::string const itemName = t ? t->Name1 : "item";

    // Bring it onto the requester (the guild member) so the deposit is from THEIR
    // bags. Caveat: PullItemToRequester returns null both when the requester's bags
    // are full AND when a STACKABLE merged into an existing stack the requester
    // already held (its guid is gone). The latter is uncommon for a deposit (you're
    // usually moving a hero's item you don't also hold) and never loses the item —
    // it's safe in the requester's bags — but it isn't deposited; re-deposit from
    // the requester. Not auto-handled here to avoid over-depositing the requester's
    // own merged-in items.
    Item* mine = PullItemToRequester(requester, srcChar, item);
    if (!mine)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Couldn't stage |cffffffff{}|r to deposit (your bags are full, "
            "or it stacked onto one you're carrying — deposit that one).", itemName);
        WowPsParty::SendInventoryTo(requester);
        if (srcChar != requester) WowPsParty::SendInventoryTo(srcChar);
        return;
    }
    ObjectGuid const movedGuid = mine->GetGUID();

    bool deposited = false;
    for (uint8 tabId = 0; tabId < GUILD_BANK_MAX_TABS && !deposited; ++tabId)
    {
        if (!guild->MemberHasTabRights(requester->GetGUID(), tabId, GUILD_BANK_RIGHT_DEPOSIT_ITEM))
            continue;
        Item* cur = SafeGetItemByGuid(requester, movedGuid);
        if (!cur) { deposited = true; break; }
        // NULL_SLOT → auto-place in the first free/stackable slot of this tab.
        guild->SwapItemsWithInventory(requester, /*toChar*/ false, tabId, NULL_SLOT,
                                      cur->GetBagSlot(), cur->GetSlot(), 0);
        if (!SafeGetItemByGuid(requester, movedGuid))
            deposited = true;   // it left our bags -> stored in the guild bank
    }

    if (deposited)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Deposited |cffffffff{}|r to the guild bank.", itemName);
    }
    else
    {
        // No deposit rights anywhere / guild bank full — hand it back to the source
        // char, but ONLY if that char can actually hold it. If not, LEAVE it in the
        // requester's bags (still safe + visible) rather than detaching it into a
        // slotless DB limbo. (CanStoreItem is a capacity check; the item's current
        // owner doesn't matter, so it's valid to test before detaching.)
        if (Item* cur = SafeGetItemByGuid(requester, movedGuid))
            if (srcChar != requester)
            {
                ItemPosCountVec back;
                if (srcChar->CanStoreItem(NULL_BAG, NULL_SLOT, back, cur, false) == EQUIP_ERR_OK)
                {
                    requester->MoveItemFromInventory(cur->GetBagSlot(), cur->GetSlot(), true);
                    cur->SetOwnerGUID(srcChar->GetGUID());
                    cur->FSetState(ITEM_CHANGED);
                    srcChar->MoveItemToInventory(back, cur, true);
                    CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
                    cur->SaveToDB(tx);
                    CharacterDatabase.CommitTransaction(tx);
                    FlushPartyTransfer(srcChar, requester);
                }
            }
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Couldn't deposit |cffffffff{}|r — no guild-bank deposit rights or the bank is full.", itemName);
    }
    WowPsParty::SendInventoryTo(requester);
    if (srcChar != requester) WowPsParty::SendInventoryTo(srcChar);
}

// A guild / arena-team CHARTER is a petition item with NO on-use spell, so the plain
// USE path below reports "isn't usable". Right-clicking it in the panel instead has
// the leader's online heroes sign it and — once enough signatures are gathered —
// auto-creates the guild / arena team. Heroes are same-account alts; the core sign
// path normally rejects same-account signers, but mod-playerbots'
// OnPlayerbotCheckPetitionAccount waives that check for bot signers (every hero is a
// playerbot), so each distinct hero guid counts. We reuse the core sign + turn-in
// handlers verbatim (all eligibility checks + owner-facing packets), exactly as the
// playerbots PetitionSignAction does, so this stays correct as the core evolves.
// Arena charters additionally require each signer to be max level — the core handler
// self-rejects lower-level heroes, so they simply don't count toward the total.
static void HandleCharterSign(Player* owner, Item* charter, Petition const* petition)
{
    WorldSession* sess = owner->GetSession();
    if (petition->ownerGuid != owner->GetGUID())
    {
        ChatHandler(sess).PSendSysMessage(
            "|cffff5555[WowPsParty]|r That charter belongs to someone else.");
        return;
    }

    uint8 const   type      = petition->petitionType;
    bool const    isArena   = type != GUILD_CHARTER_TYPE;
    uint32 const  maxLevel  = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    uint32 const  required  = isArena ? uint32(type) - 1
                                      : sWorld->getIntConfig(CONFIG_MIN_PETITION_SIGNS);
    uint8 const   arenaSlot = isArena ? ArenaTeam::GetSlotByType(type) : uint8(0xFF);
    ObjectGuid const charterGuid = charter->GetGUID();
    uint32 const  petId     = petition->petitionId;
    // Copied now — the turn-in below frees the petition, dangling this pointer.
    std::string const petName = petition->petitionName;

    // Sign with the leader's online heroes (skipping the owner), up to the bracket size.
    // We DON'T route through HandlePetitionSignOpcode: its same-account rejection needs
    // the playerbot bypass, and its other gates fail SILENTLY (un-diagnosable — heroes
    // came back 0/4 with every gate seemingly satisfied). Instead we do the eligibility
    // checks ourselves (logged) and add the signature directly — distinct hero guids each
    // get a petition_sign row, exactly what the turn-in then reads to add members.
    std::vector<ObjectGuid> party;
    WowPsParty::GetPartyGuidsFor(owner->GetGUID(), party);

    CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
    for (ObjectGuid const& g : party)
    {
        if (g == owner->GetGUID()) continue;
        // A hired alt is parked as-is — it must NOT pick up persistent guild/arena
        // membership (an arena-team row survives the dismiss). Leave it off charters.
        if (WowPsParty::IsHiredAlt(g)) continue;

        Signatures const* cur = sPetitionMgr->GetSignature(charterGuid);
        uint32 const haveNow = cur ? uint32(cur->signatureMap.size()) : 0;
        if (haveNow >= required) break;   // bracket full — don't over-fill the team

        Player* hero = ObjectAccessor::FindConnectedPlayer(g);
        if (!hero) hero = ObjectAccessor::FindPlayer(g);
        if (!hero)
        {
            LOG_INFO("module", "[WowPsParty Charter] '{}' skip {} — offline", petName, g.ToString());
            continue;
        }
        if (cur && cur->signatureMap.find(hero->GetGUID()) != cur->signatureMap.end())
            continue;   // already signed this charter

        if (isArena)
        {
            if (hero->GetLevel() < maxLevel)
            {
                LOG_INFO("module", "[WowPsParty Charter] '{}' skip {} — level {} < {}",
                    petName, hero->GetName(), uint32(hero->GetLevel()), maxLevel);
                continue;
            }
            if (arenaSlot < MAX_ARENA_SLOT && hero->GetArenaTeamId(arenaSlot))
            {
                LOG_INFO("module", "[WowPsParty Charter] '{}' skip {} — already in a {}v{} team",
                    petName, hero->GetName(), uint32(type), uint32(type));
                continue;
            }
        }
        else if (hero->GetGuildId())
        {
            LOG_INFO("module", "[WowPsParty Charter] '{}' skip {} — already in a guild",
                petName, hero->GetName());
            continue;
        }

        uint32 const heroAcct = hero->GetSession() ? hero->GetSession()->GetAccountId() : 0;
        tx->Append("INSERT IGNORE INTO `petition_sign` "
                   "(`ownerguid`, `petitionguid`, `petition_id`, `playerguid`, `player_account`) "
                   "VALUES ({}, {}, {}, {}, {})",
                   owner->GetGUID().GetCounter(), charterGuid.GetCounter(), petId,
                   hero->GetGUID().GetCounter(), heroAcct);
        sPetitionMgr->AddSignature(charterGuid, heroAcct, hero->GetGUID());
        LOG_INFO("module", "[WowPsParty Charter] '{}' signed by {} (acct {})",
            petName, hero->GetName(), heroAcct);
    }
    CharacterDatabase.CommitTransaction(tx);

    Signatures const* sigs = sPetitionMgr->GetSignature(charterGuid);
    uint32 const have = sigs ? uint32(sigs->signatureMap.size()) : 0;
    LOG_INFO("module", "[WowPsParty Charter] '{}' now {}/{} signatures (party {})",
        petName, have, required, uint32(party.size()));

    if (have < required)
    {
        if (isArena)
            ChatHandler(sess).PSendSysMessage(
                "|cffffcc00[WowPsParty]|r |cffffffff{}|r: {}/{} signatures. Arena charters need {} "
                "level-{} heroes besides you — bring more max-level heroes into the party.",
                petName, have, required, required, maxLevel);
        else
            ChatHandler(sess).PSendSysMessage(
                "|cffffcc00[WowPsParty]|r |cffffffff{}|r: {}/{} signatures collected.",
                petName, have, required);
        WowPsParty::SendInventoryTo(owner);
        return;
    }

    // Enough signatures — auto-create by replaying the turn-in through the core handler
    // (reuses guild / arena-team creation + member-add + DB cleanup). Arena needs the 5
    // emblem fields after the guid (0 = the plain default tabard; re-style later).
    WorldPacket turnIn(CMSG_TURN_IN_PETITION, isArena ? (8 + 20) : 8);
    turnIn << charterGuid;
    if (isArena)
        turnIn << uint32(0) << uint32(0) << uint32(0) << uint32(0) << uint32(0);
    sess->HandleTurnInPetitionOpcode(turnIn);

    LOG_INFO("module", "[WowPsParty Charter] {} auto-created '{}' (type={}) with {} signatures",
        owner->GetName(), petName, uint32(type), have);
    ChatHandler(sess).PSendSysMessage(
        "|cff33ff99[WowPsParty]|r Created |cffffffff{}|r with {} hero signature(s)!", petName, have);
    WowPsParty::SendInventoryTo(owner);
}

// An on-use spell the player normally AIMS: at a unit (a quest item fed to a mob,
// like Zul'Drak Rat) or at an object (a key used on a chest). A real client sends
// whatever the player picked; the shared grid has to resolve it by hand.
static bool SpellHasExplicitTarget(SpellInfo const* si)
{
    return si && (si->GetExplicitTargetMask() & (TARGET_FLAG_UNIT_MASK | TARGET_FLAG_GAMEOBJECT_MASK)) != 0;
}

// Whether the caster is an acceptable target for their own on-use spell: the core's
// InitExplicitTargets self-fallback (ally/party/raid flags) plus the client's auto-
// self-cast for any other helpful unit spell. False for an enemy spell or an object
// opener — those must be pointed at something else, or not cast at all.
static bool SpellCanLandOnCaster(SpellInfo const* si)
{
    if (!si) return false;
    uint32 const mask = si->GetExplicitTargetMask();
    if (mask & TARGET_FLAG_GAMEOBJECT_MASK) return false;
    if (mask & (TARGET_FLAG_UNIT_ALLY | TARGET_FLAG_UNIT_PARTY | TARGET_FLAG_UNIT_RAID)) return true;
    return (mask & TARGET_FLAG_UNIT) != 0 && si->IsPositive();
}

// Whether this lock yields to the given cast item + spell, mirroring the accept
// cases of Spell::CanOpenLock: a key whose entry the lock names, the spell the lock
// names, a lock type the spell's OPEN_LOCK effect handles, or a lock with no
// requirement at all. The exact skill-value test stays with the core — this only
// decides WHICH candidate to aim at, and the core still has the final say.
static bool LockYieldsTo(uint32 lockId, SpellInfo const* si, uint32 castItemEntry)
{
    LockEntry const* lock = sLockStore.LookupEntry(lockId);
    if (!lock) return false;

    bool requiresKey = false;
    for (uint8 j = 0; j < MAX_LOCK_CASE; ++j)
    {
        switch (lock->Type[j])
        {
            case LOCK_KEY_ITEM:
                if (lock->Index[j] && lock->Index[j] == castItemEntry) return true;
                requiresKey = true;
                break;
            case LOCK_KEY_SKILL:
                requiresKey = true;
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    if (si->Effects[i].Effect == SPELL_EFFECT_OPEN_LOCK
                        && uint32(si->Effects[i].MiscValue) == lock->Index[j])
                        return true;
                break;
            case LOCK_KEY_SPELL:
                if (lock->Index[j] == si->Id) return true;
                requiresKey = true;
                break;
            default:
                break;
        }
    }
    return !requiresKey;
}

// The object the human means when they right-click a key or a charge in the shared
// grid. A normal client sends whichever object the player clicked with the targeting
// reticle; the inventory panel has no way to express that, so take the nearest
// candidate the player can actually see, whose lock this very item yields to — a
// locked door a yard closer can't steal the cast, and standing at the chest is the
// same intent the reticle would have expressed.
//
// Only OPENERS get an object picked for them. Every gameobject-target on-use item in
// the 3.3.5a data is one (all 203 carry SPELL_EFFECT_OPEN_LOCK), and without a lock
// to match against there is nothing to tell the intended object from a signpost — and
// guessing wrong would burn the item's charge on it.
static GameObject* AimableGameObject(Player* requester, SpellInfo const* si, uint32 castItemEntry)
{
    bool opensLocks = false;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        if (si->Effects[i].Effect == SPELL_EFFECT_OPEN_LOCK) { opensLocks = true; break; }
    if (!opensLocks) return nullptr;

    // You have to be standing at the thing. Bounding the sweep also keeps one click
    // from raycasting line of sight against every object in a city-sized grid.
    constexpr float MAX_AIM_RANGE = 30.0f;
    float range = si->GetMaxRange(si->IsPositive(), requester);
    if (range <= 0.0f) range = INTERACTION_DISTANCE;
    range = std::min(range, MAX_AIM_RANGE);

    struct ReachableGoCheck
    {
        ReachableGoCheck(Player const* src, float range) : _src(src), _range(range) {}
        bool operator()(GameObject* go) const
        {
            if (!go || !go->isSpawned() || !_src->IsWithinDistInMap(go, _range)) return false;
            // An already-swung door / sprung object would only fail CheckCast with
            // ALREADY_OPEN while shadowing the one the player meant.
            if (go->GetGoType() == GAMEOBJECT_TYPE_DOOR && go->GetGoState() != GO_STATE_READY) return false;
            return go->IsWithinLOSInMap(_src);
        }
        Player const* _src;
        float _range;
    };

    std::list<GameObject*> candidates;
    ReachableGoCheck check(requester, range);
    Acore::GameObjectListSearcher<ReachableGoCheck> searcher(requester, candidates, check);
    Cell::VisitObjects(requester, searcher, range);

    GameObject* nearest = nullptr;
    float nearestDist = 0.0f;
    for (GameObject* go : candidates)
    {
        if (!LockYieldsTo(go->GetGOInfo()->GetLockId(), si, castItemEntry)) continue;
        float const d = requester->GetDistance(go);
        if (!nearest || d < nearestDist) { nearest = go; nearestDist = d; }
    }
    return nearest;
}

// A still-locked item this opener would open, searched over the whole party. Openers
// with TARGET_GAMEOBJECT_ITEM_TARGET (skeleton keys, Seaforium charges, most quest
// keys) take a locked ITEM as readily as a world object, and lockboxes are exactly
// what the shared grid accumulates. Reports the holder so the caller can stage the box
// onto the requester — the core only accepts an item target out of the CASTER's bags.
static Item* FindPartyLockedItem(Player* requester, SpellInfo const* si, uint32 castItemEntry,
                                 Player*& outHolder)
{
    auto openable = [&](Item* item) -> bool
    {
        if (!WowPsParty::SafeIsLocked(item)) return false;
        ItemTemplate const* proto = WowPsParty::SafeItemTemplate(item);
        return proto && proto->LockID && LockYieldsTo(proto->LockID, si, castItemEntry);
    };
    auto findIn = [&](Player* owner) -> Item*
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = owner->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (openable(item)) return item;
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = owner->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(slot))
                        if (openable(item)) return item;
        return nullptr;
    };

    if (Item* own = findIn(requester)) { outHolder = requester; return own; }

    QueryResult members = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {}", requester->GetSession()->GetAccountId());
    if (!members) return nullptr;
    do
    {
        Player* mate = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(members->Fetch()[0].Get<uint32>()));
        if (!mate || mate == requester || WowPsParty::MemberStorageUnstable(mate)) continue;
        if (Item* box = findIn(mate)) { outHolder = mate; return box; }
    } while (members->NextRow());
    return nullptr;
}

// Point an aimed on-use spell at what the human is looking at: their selection for a
// unit-target spell, the nearest object in range — or a party lockbox — for an
// opener. Returns false, having said why, when there is nothing valid to aim at, so
// the click reports instead of being swallowed.
static bool AimAtRequesterTarget(Player* requester, SpellInfo const* si, uint32 castItemEntry,
                                 std::string const& itemName, SpellCastTargets& targets)
{
    uint32 const mask = si->GetExplicitTargetMask();
    if (mask & TARGET_FLAG_UNIT_MASK)
        if (Unit* selected = ObjectAccessor::GetUnit(*requester, requester->GetTarget()))
            if (si->CheckExplicitTarget(requester, selected) == SPELL_CAST_OK)
            {
                targets.SetUnitTarget(selected);
                return true;
            }

    if (mask & TARGET_FLAG_GAMEOBJECT_MASK)
        if (GameObject* go = AimableGameObject(requester, si, castItemEntry))
        {
            targets.SetGOTarget(go);
            return true;
        }

    if (mask & TARGET_FLAG_GAMEOBJECT_ITEM)
    {
        Player* holder = nullptr;
        if (Item* box = FindPartyLockedItem(requester, si, castItemEntry, holder))
        {
            ItemTemplate const* boxProto = WowPsParty::SafeItemTemplate(box);
            std::string const boxName = boxProto ? boxProto->Name1 : "that container";
            Item* staged = PullItemToRequester(requester, holder, box);
            if (!staged)
            {
                ChatHandler(requester->GetSession()).PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Your bags are full — free a slot to open |cffffffff{}|r.", boxName);
                return false;
            }
            targets.SetItemTarget(staged);
            return true;
        }
    }

    // Nothing picked, and the spell is a helpful one that lands on whoever cast it —
    // the client's auto-self-cast. The gate matters: without it a NEGATIVE "any unit"
    // quest item would happily fire at the requester, which CheckExplicitTarget does
    // not reject, spending the charge and destroying the item for nothing.
    if (SpellCanLandOnCaster(si))
    {
        targets.SetUnitTarget(requester);
        return true;
    }

    if (mask & TARGET_FLAG_UNIT_MASK)
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Target what you want to use |cffffffff{}|r on first.", itemName);
    else
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Stand next to what you want to use |cffffffff{}|r on.", itemName);
    return false;
}

// Re-push the shared grid after a use moved, spent or bounced something. Every exit
// from the use path needs it, including the ones that give up part-way — an item
// staged onto the requester and then abandoned would otherwise still be drawn on its
// old owner until some unrelated action refreshed the panel.
static void RefreshPartyGrid(Player* requester, Player* srcChar)
{
    WowPsParty::SendInventoryTo(requester);
    if (srcChar != requester) WowPsParty::SendInventoryTo(srcChar);
}

// Core of USE / USEBYID — use a resolved bag item. CONSUMABLES (food/
// potions) fire their on-use on the requester and lose a charge from the owner.
// NON-consumables (recipes to learn, essences/shards that CONVERT via reagents,
// clickies, clams) are pulled onto the requester and used through the engine's
// real item-use path, so the input is consumed exactly as a normal use — a bare
// triggered CastSpell skips reagent/charge/recipe consumption, which let you
// create the output without destroying the input (the infinite-dupe bug).
static void UseOwnedItemInstance(Player* requester, Player* srcChar, Item* srcItem)
{
    if (!requester || !requester->GetSession() || !srcChar || !srcItem) return;
    ItemTemplate const* t = srcItem->GetTemplate();
    if (!t) return;

    // A guild / arena-team CHARTER (petition item, no on-use spell) is signed by the
    // party's heroes instead of falling through to "isn't usable" below.
    if (Petition const* petition = sPetitionMgr->GetPetition(srcItem->GetGUID()))
    {
        HandleCharterSign(requester, srcItem, petition);
        return;
    }

    uint32 useSpell = 0;
    int32  useSpellCharges = 0;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        if (t->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE && t->Spells[i].SpellId > 0)
        {
            useSpell = uint32(t->Spells[i].SpellId);
            useSpellCharges = t->Spells[i].SpellCharges;   // 0 = reusable; <0 = expendable
            break;
        }
    if (!useSpell)
    {
        // No on-use spell — but a LOOTABLE container (satchel / reward pouch) is
        // OPENED, not "used". Shared with the EQUIP route so a satchel opens no
        // matter which action the client picked.
        if (OpenLootableContainer(requester, srcChar, srcItem))
            return;

        // Quest-STARTER item (e.g. Shattered Necklace): right-clicking opens its
        // quest. It lives in a mate's bags, so the client can't drive the quest
        // dialog — pull it onto the requester and send the quest-giver details
        // sourced from the item, so the HUMAN accepts (and the engine consumes the
        // starter item from the requester on accept), exactly like clicking it in
        // a normal bag. Without this, a quest item just reported "isn't usable".
        if (t->StartQuest)
        {
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(t->StartQuest))
            {
                Item* pulled = PullItemToRequester(requester, srcChar, srcItem);
                if (!pulled)
                {
                    ChatHandler(requester->GetSession()).PSendSysMessage(
                        "|cffff5555[WowPsParty]|r Your bags are full — free a slot to start |cffffffff{}|r's quest.", t->Name1);
                    return;
                }
                if (requester->CanTakeQuest(quest, true))
                    requester->PlayerTalkClass->SendQuestGiverQuestDetails(quest, pulled->GetGUID(), true);
                WowPsParty::SendInventoryTo(requester);
                if (srcChar != requester) WowPsParty::SendInventoryTo(srcChar);
                return;
            }
        }

        LOG_INFO("module",
            "[WowPsParty Use] {} clicked non-usable entry={} '{}' class={} flags={} (no on-use spell, not lootable, no startquest)",
            requester->GetName(), t->ItemId, t->Name1, uint32(t->Class), t->Flags);
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r |cffffffff{}|r isn't usable.", t->Name1);
        return;
    }

    // An ENCHANT SCROLL (its on-use spell is a permanent enchant) must NOT be self-
    // cast here — that applies the enchant to NOTHING and just eats the scroll (the
    // "right-clicked the scroll, it vanished, nothing got enchanted" dupe-loss), and a
    // scroll can enchant a BOT's gear which the normal client cursor can't even reach.
    // Instead ARM the addon: tell it which enchant this scroll provides, and the next
    // grid item the user clicks gets enchanted (ENCHSCROLL -> the addon's pending-
    // enchant mode -> ENCHANT, which HandleEnchant applies via its scroll path +
    // consumes the scroll). The scroll is NOT consumed here.
    SpellInfo const* const useInfo = sSpellMgr->GetSpellInfo(useSpell);
    if (PermEnchantIdOfSpell(useInfo))
    {
        std::ostringstream out;
        out << "ENCHSCROLL\t" << useSpell << '\t' << t->Name1;
        SendWPSP(requester, out.str());
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Now click the item you want to enchant with "
            "|cffffffff{}|r (right-click an item to cancel).", t->Name1);
        return;
    }

    // A RECIPE goes to whichever party member can LEARN it, which is almost never the
    // human. The requester-cast path below pulled it into the human's bags and cast it
    // there, and nothing on that path checks the recipe's RequiredSkill — neither
    // Spell::CheckItems nor CheckCast gates a player-target LEARN_SPELL — so
    // Player::CastItemUseSpell's special learning branch taught the craft to the HUMAN
    // and destroyed the recipe, or, if they already knew it, destroyed it for nothing.
    // A class-9 row that teaches NOTHING (warlock Grimoires, Book of Glyph Mastery)
    // resolves empty and falls through to the ordinary cast path below, unchanged.
    if (!RecipeTaughtSpells(t).empty())
    {
        TeachRecipeFromParty(requester, srcChar, srcItem, /*quiet=*/false, /*gen=*/0,
                             ResolvePartySlots(requester->GetSession()->GetAccountId()));
        return;
    }

    // A GLYPH's on-use spell applies the glyph. Route it to the correct free slot
    // (see ApplyGlyphFromItem) instead of CastSpell, which would always pick slot 0.
    if (useInfo)
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (useInfo->Effects[i].Effect == SPELL_EFFECT_APPLY_GLYPH)
            {
                ApplyGlyphFromItem(requester, srcChar, srcItem, uint32(useInfo->Effects[i].MiscValue));
                return;
            }

    bool const hasExplicitTarget = SpellHasExplicitTarget(useInfo);
    bool const selfServing = !hasExplicitTarget || SpellCanLandOnCaster(useInfo);

    // CONSUMABLE (food/potion): fire the effect on the requester and decrement the
    // owner's stack. This path has no dupe — the engine isn't relied on to consume.
    // A consumable that must be AIMED SOMEWHERE ELSE is excluded: the blind self-cast
    // can only fail for it, and the decrement below runs whether or not the cast
    // landed, so an Empty Cursed Ooze Jar or a Netherweave Net was destroyed for
    // nothing. Those fall through to the requester-cast path, where the engine's own
    // TakeCastItem decides whether the charge was really spent.
    if (t->Class == ITEM_CLASS_CONSUMABLE && selfServing)
    {
        requester->CastSpell(requester, useSpell, true);
        // Consume ONLY if the on-use spell is actually expendable — the same rule the
        // engine's Spell::TakeCastItem uses. A REUSABLE consumable (SpellCharges == 0,
        // e.g. the Oculus Ruby/Amber/Emerald drake essences) is used on every platform
        // and must NOT be destroyed ("the Ruby Essence vanishes after the first platform"
        // bug). Single-use food/potions (SpellCharges != 0) still decrement/destroy.
        if (useSpellCharges != 0)
        {
            if (srcItem->GetCount() > 1)
            {
                srcItem->SetCount(srcItem->GetCount() - 1);
                srcItem->SetState(ITEM_CHANGED, srcChar);
            }
            else
                srcChar->DestroyItem(srcItem->GetBagSlot(), srcItem->GetSlot(), true);
        }
        WowPsParty::SendInventoryTo(requester);
        if (srcChar != requester) WowPsParty::SendInventoryTo(srcChar);
        return;
    }

    // Two on-use item kinds MUST be cast by the (never-a-bot) REQUESTER, not the
    // owning party member — so pull the item over (like a recipe) and use it via
    // the real item-use pipeline on the requester:
    //   * A SPELL-TEACHING item that ISN'T a recipe (a mount, a companion pet): the
    //     requester must be the one who LEARNS it. Recipes left this path above —
    //     they belong to whoever has the profession, not to whoever clicked.
    //   * A REAGENT-CONSUMING CONVERSION (essence/shard "combine 3 -> 1", mote
    //     combines, …): Spell::TakeReagents gives ANY bot caster its reagents for
    //     FREE (the autonomous-rotation rule, WowPsParty_PlayerHasBotAI). Casting
    //     such a combine as the owning hero-alt/henchman therefore consumed only
    //     the single cast item while still creating the output — the infinite-
    //     essence dupe. Casting as the human requester makes the engine take the
    //     full reagent cost from the shared party inventory. (The reverse split,
    //     greater -> 3 lesser, has no reagent and is consumed via the cast item, so
    //     it stays on the owner-cast path below and never duped.)
    // Everything else (clams, clickies) is used by its OWNER through the same
    // pipeline — no cross-character pull — and the engine consumes the input
    // exactly as a normal use. The old bare triggered cast skipped that
    // consumption, creating the output while leaving the input = the infinite dupe.
    // An item that must be AIMED SOMEWHERE ELSE joins them. It used to be cast with
    // default-constructed, i.e. EMPTY, targets: CheckCast bailed SPELL_FAILED_BAD_TARGETS
    // and, because the caster was the item's OWNER — normally a henchman — even the error
    // went to a bot's session, so the right-click did nothing at all, silently. Aim it at
    // what the HUMAN picked and let the human cast, so selection, range and line of sight
    // are all judged from the player who is standing there. A self-serving item stays on
    // the owner-cast path: the core already resolves those to the caster, so pulling them
    // over would only invent a bags-full failure for something that works today.
    bool isLearn = false;
    bool hasReagent = false;
    if (useInfo)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (useInfo->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL) { isLearn = true; break; }
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (useInfo->Reagent[i] > 0 && useInfo->ReagentCount[i] > 0) { hasReagent = true; break; }
    }

    bool const castByRequester = isLearn || hasReagent || (hasExplicitTarget && !selfServing);
    Player* caster   = srcChar;
    Item*   castItem = srcItem;

    // The cast item moves BEFORE the target is resolved: aiming an opener can stage a
    // party lockbox into the requester's bags, and doing that first would strand the box
    // there if the cast item then had nowhere to go.
    if (castByRequester && srcChar != requester)
    {
        uint32 const    entry   = srcItem->GetEntry();
        ObjectGuid const srcGuid = srcItem->GetGUID();
        Item* pulled = PullItemToRequester(requester, srcChar, srcItem);
        if (!pulled)
        {
            // A stackable reagent (a recipe is count-1, but an essence stacks) can
            // MERGE into a stack the requester already holds: the moved instance is
            // consumed by the merge so its guid is gone, yet the reagent now sits in
            // the requester's bags. Distinguish that from a genuine no-room bounce-
            // back — where PullItemToRequester leaves the stack on its owner — so a
            // merge still combines and only a real full-bags case aborts. A bounce-
            // back ALSO happens when the requester already holds the item's unique
            // count (a key both they and a henchman carry) — their own copy is an
            // equally good cast source, so check for one before calling it a failure.
            //
            // Any same-entry stack is an acceptable cast source: the on-use spell
            // and its reagent cost are item-entry-keyed, not instance-keyed.
            pulled = requester->GetItemByEntry(entry);
            bool const stillOnOwner = SafeGetItemByGuid(srcChar, srcGuid) != nullptr;
            if (!pulled)
            {
                if (stillOnOwner)
                    ChatHandler(requester->GetSession()).PSendSysMessage(
                        "|cffff5555[WowPsParty]|r Your bags are full — free a slot to use |cffffffff{}|r.", t->Name1);
                RefreshPartyGrid(requester, srcChar);
                return;   // bounced back, or merged in but unfindable — never mis-cast
            }
            // The clicked instance stayed put and a copy of the requester's own is
            // being spent instead — say so, or the charge appears to come off the
            // wrong stack for no stated reason.
            if (stillOnOwner)
                ChatHandler(requester->GetSession()).PSendSysMessage(
                    "|cff66ccff[WowPsParty]|r Your bags are full — used your own |cffffffff{}|r instead.", t->Name1);
        }
        LOG_INFO("module",
            "[WowPsParty Use] {} uses entry={} '{}' guid={} (reagent={} learn={} aimed={}) as the human caster; clicked copy was {}'s.",
            requester->GetName(), entry, t->Name1, pulled->GetGUID().GetCounter(),
            hasReagent, isLearn, hasExplicitTarget, srcChar->GetName());
        caster   = requester;
        castItem = pulled;
    }

    SpellCastTargets targets;   // left default-constructed = self-cast
    if (hasExplicitTarget && !AimAtRequesterTarget(requester, useInfo, t->ItemId, t->Name1, targets))
    {
        RefreshPartyGrid(requester, srcChar);   // the cast item may already have moved
        return;
    }

    caster->CastItemUseSpell(castItem, targets, 0, 0);
    RefreshPartyGrid(requester, srcChar);
}

// USE\t<srcPartySlot>\t<srcItemGuidLow> — use a specific bag item instance
// (the shared-inventory grid's right-click). Resolves owner + item, then the
// shared core above does the work.
static void HandleUse(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const srcSlot = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);
    if (!srcItemGuidLow) return;

    uint32 const account = requester->GetSession()->GetAccountId();
    QueryResult q = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}", account, srcSlot);
    if (!q) return;
    uint32 const srcCharGuid = q->Fetch()[0].Get<uint32>();
    Player* srcChar = ObjectAccessor::FindConnectedPlayer(
        ObjectGuid::Create<HighGuid::Player>(srcCharGuid));
    if (!srcChar) return;

    Item* srcItem = SafeGetItemByGuid(srcChar, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!srcItem) return;
    UseOwnedItemInstance(requester, srcChar, srcItem);
}

// USEBYID\t<itemId> — the consumables action bar's use path: use ANY instance
// of <itemId> from the party's shared bags. The requester's own stacks drain
// first, then party-slot order, so a hero doesn't burn a mate's last potion
// while sitting on a full stack. Delegates to the same core as USE.
static void HandleUseById(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    uint32 const itemId = std::strtoul(std::string(payload).c_str(), nullptr, 10);
    if (!itemId) return;

    uint32 const account = requester->GetSession()->GetAccountId();
    Player* srcChar = nullptr;
    Item*   srcItem = nullptr;
    auto tryMember = [&](Player* p)
    {
        if (srcItem || WowPsParty::MemberStorageUnstable(p)) return;
        if (Item* it = p->GetItemByEntry(itemId)) { srcChar = p; srcItem = it; }
    };
    tryMember(requester);
    for (uint8 slot = 0; slot < WowPsParty::PARTY_SIZE && !srcItem; ++slot)
    {
        uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
        if (!guid) continue;
        ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
        Player* p = ObjectAccessor::FindConnectedPlayer(og);
        if (!p) p = ObjectAccessor::FindPlayer(og);
        if (!p || p == requester) continue;
        tryMember(p);
    }
    if (!srcItem)
    {
        ItemTemplate const* t = sObjectMgr->GetItemTemplate(itemId);
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r No |cffffffff{}|r left in the party's bags.",
            t ? t->Name1 : "such item");
        return;
    }
    LOG_INFO("module", "[WowPsParty Bar] {} uses entry={} '{}' owned by {} (guidLow={})",
        requester->GetName(), itemId, srcItem->GetTemplate() ? srcItem->GetTemplate()->Name1 : "?",
        srcChar->GetName(), srcItem->GetGUID().GetCounter());
    UseOwnedItemInstance(requester, srcChar, srcItem);
}

// BARCOUNT\t<id1>,<id2>,... — party-wide bag counts for the consumables action
// bar. Replies BARCOUNT\t<id>:<count>;... chunked under the 3.3.5a addon-message
// size cap; every requested id is echoed (0 when none) so an emptied stack greys
// its button out. The addon re-requests on INV_DIRTY / INV_END, so this stays a
// tiny targeted reply instead of the full INVENTORY stream.
static void HandleBarCounts(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    uint32 const account = requester->GetSession()->GetAccountId();

    std::vector<Player*> members;
    for (uint8 slot = 0; slot < WowPsParty::PARTY_SIZE; ++slot)
    {
        uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
        if (!guid) continue;
        ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
        Player* p = ObjectAccessor::FindConnectedPlayer(og);
        if (!p) p = ObjectAccessor::FindPlayer(og);
        if (!WowPsParty::MemberStorageUnstable(p)) members.push_back(p);
    }
    // Not enrolled (solo, no account_party rows) → count the requester alone.
    if (std::find(members.begin(), members.end(), requester) == members.end())
        members.push_back(requester);

    std::string chunk;
    auto flush = [&]()
    {
        if (!chunk.empty()) { SendWPSP(requester, "BARCOUNT\t" + chunk); chunk.clear(); }
    };
    constexpr size_t MAX_PAYLOAD = 200;
    std::string const ids(payload);
    size_t pos = 0;
    while (pos < ids.size())
    {
        size_t const comma = ids.find(',', pos);
        uint32 const id = std::strtoul(ids.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos).c_str(), nullptr, 10);
        pos = comma == std::string::npos ? ids.size() : comma + 1;
        if (!id) continue;
        uint32 count = 0;
        for (Player* p : members)
            count += p->GetItemCount(id, false);
        std::string const rec = std::to_string(id) + ':' + std::to_string(count);
        if (!chunk.empty() && chunk.size() + 1 + rec.size() > MAX_PAYLOAD)
            flush();
        if (!chunk.empty()) chunk += ';';
        chunk += rec;
    }
    flush();
    LOG_DEBUG("module", "[WowPsParty Bar] BARCOUNT for {}: {} member(s), ids='{}'",
        requester->GetName(), members.size(), ids);
}

// SPLIT\t<srcPartySlot>\t<srcItemGuidLow>\t<count> — split `count` off a stack
// into a free slot on the SAME character. Targets a known-empty slot so the
// store can't merge it straight back; counts are conserved (orig -= n, new = n).
static void HandleSplit(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    std::string s(payload);
    // 3 tab fields
    auto t1 = s.find('\t');
    if (t1 == std::string::npos) return;
    auto t2 = s.find('\t', t1 + 1);
    if (t2 == std::string::npos) return;
    uint32 const srcSlot        = std::strtoul(s.substr(0, t1).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(s.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
    uint32 const count          = std::strtoul(s.substr(t2 + 1).c_str(), nullptr, 10);
    if (!srcItemGuidLow || !count) return;

    uint32 const account = requester->GetSession()->GetAccountId();
    QueryResult q = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}", account, srcSlot);
    if (!q) return;
    uint32 const srcCharGuid = q->Fetch()[0].Get<uint32>();
    Player* srcChar = ObjectAccessor::FindConnectedPlayer(
        ObjectGuid::Create<HighGuid::Player>(srcCharGuid));
    if (!srcChar) return;

    Item* srcItem = SafeGetItemByGuid(srcChar, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!srcItem) return;
    if (count >= srcItem->GetCount()) return;   // nothing to split / would empty

    // Find a truly empty bag slot on the same char.
    uint8 destBag = 0, destSlot = 0;
    bool haveSlot = false;
    for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END && !haveSlot; ++i)
        if (!srcChar->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        { destBag = INVENTORY_SLOT_BAG_0; destSlot = i; haveSlot = true; }
    for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END && !haveSlot; ++b)
        if (Bag* bag = srcChar->GetBagByPos(b))
            for (uint32 j = 0; j < bag->GetBagSize() && !haveSlot; ++j)
                if (!srcChar->GetItemByPos(b, uint8(j)))
                { destBag = b; destSlot = uint8(j); haveSlot = true; }
    if (!haveSlot)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r No free bag slot to split into.");
        return;
    }

    uint32 const entry = srcItem->GetEntry();
    ItemPosCountVec dest;
    if (srcChar->CanStoreItem(destBag, destSlot, dest, entry, count) != EQUIP_ERR_OK)
        return;
    uint32 const beforeCount = srcItem->GetCount();
    srcItem->SetCount(beforeCount - count);
    srcItem->SetState(ITEM_CHANGED, srcChar);
    // If the store fails despite CanStoreItem succeeding, roll the source
    // count back so we don't silently destroy `count` items.
    if (!srcChar->StoreNewItem(dest, entry, true))
    {
        srcItem->SetCount(beforeCount);
        srcItem->SetState(ITEM_CHANGED, srcChar);
    }
    WowPsParty::SendInventoryTo(requester);
}

// A "general" container holds ordinary items. Specialized containers — quivers
// and ammo pouches (ITEM_CLASS_QUIVER) and profession bags (soul / herb /
// enchanting / etc., ITEM_CLASS_CONTAINER with a non-zero subclass) — only
// accept their own item family. The generic bag sort must leave both their
// slots AND their contents alone: swapping an ordinary item into a quiver slot
// is exactly what made the client throw "This item doesn't go in that
// container" when a hunter with a quiver hit Sort.
static bool IsGeneralContainer(Bag const* bag)
{
    ItemTemplate const* t = bag ? bag->GetTemplate() : nullptr;
    return t && t->Class == ITEM_CLASS_CONTAINER
             && t->SubClass == ITEM_SUBCLASS_CONTAINER;
}

// Compact + order one character's bag contents (backpack + equipped bags) the
// way Bagnon's sort does: quality first, then item class/subclass, then entry.
// Selection-sort via SwapItem so we never detach/re-store items (which would
// risk bag-type mismatches or orphaning). Containers themselves aren't moved.
static void SortBagsFor(Player* p)
{
    if (!p) return;
    std::vector<uint16> slotSeq;   // every general bag position, in display order
    std::vector<Item*>  items;     // the items currently occupying them

    auto addPos = [&](uint8 bag, uint8 slot)
    {
        slotSeq.push_back((uint16(bag) << 8) | slot);
        if (Item* it = p->GetItemByPos(bag, slot))
            items.push_back(it);
    };
    for (uint8 s = INVENTORY_SLOT_ITEM_START; s < INVENTORY_SLOT_ITEM_END; ++s)
        addPos(INVENTORY_SLOT_BAG_0, s);
    for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
    {
        Bag* bag = p->GetBagByPos(b);
        if (!bag) continue;
        if (!IsGeneralContainer(bag)) continue;   // skip quivers / profession bags
        for (uint32 j = 0; j < bag->GetBagSize(); ++j)
            addPos(b, uint8(j));
    }

    // Merge partial stacks of the same item first (Bagnon combines, not just
    // orders). Pour later stacks into earlier ones up to max stack size;
    // emptied stacks are destroyed and dropped from the list.
    for (size_t i = 0; i < items.size(); ++i)
    {
        Item* a = items[i];
        if (!a) continue;
        uint32 const maxS = a->GetMaxStackCount();
        if (maxS <= 1) continue;
        for (size_t j = i + 1; j < items.size(); ++j)
        {
            Item* b = items[j];
            if (!b || b->GetEntry() != a->GetEntry()) continue;
            if (a->GetCount() >= maxS) break;
            uint32 const space = maxS - a->GetCount();
            uint32 const mv    = std::min(space, b->GetCount());
            a->SetCount(a->GetCount() + mv);
            a->SetState(ITEM_CHANGED, p);
            if (mv >= b->GetCount())
            {
                p->DestroyItem(b->GetBagSlot(), b->GetSlot(), true);
                items[j] = nullptr;
            }
            else
            {
                b->SetCount(b->GetCount() - mv);
                b->SetState(ITEM_CHANGED, p);
            }
        }
    }
    items.erase(std::remove(items.begin(), items.end(), nullptr), items.end());

    std::stable_sort(items.begin(), items.end(), [](Item* a, Item* b)
    {
        ItemTemplate const* ta = a->GetTemplate();
        ItemTemplate const* tb = b->GetTemplate();
        if (!ta || !tb) return ta != nullptr;
        if (ta->Quality  != tb->Quality)  return ta->Quality  > tb->Quality;   // rarer first
        if (ta->Class    != tb->Class)    return ta->Class    < tb->Class;
        if (ta->SubClass != tb->SubClass) return ta->SubClass < tb->SubClass;
        if (a->GetEntry() != b->GetEntry()) return a->GetEntry() < b->GetEntry();
        return a->GetCount() > b->GetCount();
    });

    for (size_t i = 0; i < items.size() && i < slotSeq.size(); ++i)
    {
        uint16 const target = slotSeq[i];
        Item* desired = items[i];
        if (desired->GetPos() == target) continue;
        p->SwapItem(desired->GetPos(), target);
    }
}

// SORT_BAGS — sort every online party member's bags, then refresh the addon.
static void HandleSortBags(Player* requester)
{
    if (!requester || !requester->GetSession()) return;
    uint32 const account = requester->GetSession()->GetAccountId();
    for (uint8 slot = 0; slot < WowPsParty::PARTY_SIZE; ++slot)
    {
        uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
        if (!guid) continue;
        Player* p = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(guid));
        if (p) SortBagsFor(p);
    }
    WowPsParty::SendInventoryTo(requester);
}

// REDISTRIBUTE_ITEMS — emergency balance pass for the shared party inventory.
// Moves loose bag items away from cramped same-map heroes until free slots are
// roughly even. This is intentionally slot-pressure driven, not item sorting:
// Sort organizes each character's bags; Redistribute frees individual bags when
// one member is full while the party still has room.
static void HandleRedistributeItems(Player* requester)
{
    if (!requester || !requester->GetSession()) return;
    uint32 const account = requester->GetSession()->GetAccountId();

    std::vector<Player*> heroes;
    for (uint8 slot = 0; slot < WowPsParty::PARTY_SIZE; ++slot)
    {
        uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
        if (!guid) continue;
        Player* p = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(guid));
        if (!p || !p->IsInWorld() || p->GetMap() != requester->GetMap())
            continue;
        heroes.push_back(p);
    }

    if (heroes.size() < 2)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Redistribute needs at least two loaded "
            "party members on this map.");
        WowPsParty::SendInventoryTo(requester);
        return;
    }

    auto canMove = [](Item* item) -> bool
    {
        if (!item) return false;
        if (item->IsEquipped() || item->IsInTrade() || item->IsNotEmptyBag())
            return false;
        ItemTemplate const* tmpl = item->GetTemplate();
        if (!tmpl) return false;
        if (tmpl->Class == ITEM_CLASS_CONTAINER)
            return false;
        return true;
    };

    auto firstMovableFor = [&](Player* owner, Player* target, ItemPosCountVec& destPos) -> Item*
    {
        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (Item* item = owner->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                if (canMove(item))
                {
                    destPos.clear();
                    if (target->CanStoreItem(
                            NULL_BAG, NULL_SLOT, destPos, item, false) == EQUIP_ERR_OK)
                        return item;
                }

        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            if (Bag* bag = owner->GetBagByPos(b))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    if (Item* item = owner->GetItemByPos(b, uint8(j)))
                        if (canMove(item))
                        {
                            destPos.clear();
                            if (target->CanStoreItem(
                                    NULL_BAG, NULL_SLOT, destPos, item, false) == EQUIP_ERR_OK)
                                return item;
                        }
        return nullptr;
    };

    auto byFreeSlots = [](Player const* a, Player const* b)
    {
        return a->GetFreeInventorySpace() < b->GetFreeInventorySpace();
    };

    uint32 moved = 0;
    constexpr uint32 MAX_MOVES = 250;
    while (moved < MAX_MOVES)
    {
        std::stable_sort(heroes.begin(), heroes.end(), byFreeSlots);
        Player* src = nullptr;
        Player* dst = nullptr;
        Item* item = nullptr;
        ItemPosCountVec destPos;

        for (size_t si = 0; si < heroes.size() && !item; ++si)
        {
            Player* candidateSrc = heroes[si];
            uint32 const srcFree = candidateSrc->GetFreeInventorySpace();
            for (size_t di = heroes.size(); di > 0; --di)
            {
                Player* candidateDst = heroes[di - 1];
                if (candidateDst == candidateSrc)
                    continue;
                uint32 const dstFree = candidateDst->GetFreeInventorySpace();
                if (dstFree <= srcFree + 1)
                    break;
                item = firstMovableFor(candidateSrc, candidateDst, destPos);
                if (item)
                {
                    src = candidateSrc;
                    dst = candidateDst;
                    break;
                }
            }
        }

        if (!item || !src || !dst)
            break;

        ObjectGuid const srcGuid = src->GetGUID();
        src->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
        item->SetOwnerGUID(dst->GetGUID());
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        item->SaveToDB(tx);
        CharacterDatabase.CommitTransaction(tx);

        ItemPosCountVec verifyPos;
        if (dst->CanStoreItem(NULL_BAG, NULL_SLOT, verifyPos, item, false) == EQUIP_ERR_OK)
        {
            dst->MoveItemToInventory(verifyPos, item, true);
            FlushPartyTransfer(src, dst);
            ++moved;
            continue;
        }

        item->SetOwnerGUID(srcGuid);
        ItemPosCountVec backPos;
        if (src->CanStoreItem(NULL_BAG, NULL_SLOT, backPos, item, false) == EQUIP_ERR_OK)
            src->MoveItemToInventory(backPos, item, true);
        CharacterDatabaseTransaction tx2 = CharacterDatabase.BeginTransaction();
        item->SaveToDB(tx2);
        CharacterDatabase.CommitTransaction(tx2);
        FlushPartyTransfer(src, dst);
        break;
    }

    if (moved)
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Redistributed |cffffffff{}|r item(s) across party bags.",
            moved);
    else
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r No movable items needed redistributing.");
    WowPsParty::SendInventoryTo(requester);
}

// SELL_TRASH — vendor-sell every grey (poor quality) item across the whole
// party's bags in one click. Mirrors HandleSell's credit-to-requester (the
// shared-gold hook then mirrors the gain across the party), but loops over all
// members and only touches ITEM_QUALITY_POOR items with a sell value. Money is
// credited once at the end so the shared-pool hook fires a single time.
static void HandleSellTrash(Player* requester)
{
    if (!requester || !requester->GetSession()) return;
    uint32 const account = requester->GetSession()->GetAccountId();

    uint32 totalMoney = 0;
    uint32 soldCount  = 0;

    auto trySell = [&](Player* owner, Item* item)
    {
        if (!item) return;
        ItemTemplate const* tmpl = item->GetTemplate();
        if (!tmpl) return;
        if (tmpl->Quality != ITEM_QUALITY_POOR) return;
        if (tmpl->SellPrice == 0) return;
        if (item->IsEquipped() || item->IsNotEmptyBag()) return;   // greys are neither, but guard
        totalMoney += tmpl->SellPrice * item->GetCount();
        soldCount  += 1;
        owner->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
    };

    for (uint8 slot = 0; slot < WowPsParty::PARTY_SIZE; ++slot)
    {
        uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
        if (!guid) continue;
        Player* p = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(guid));
        if (!p) continue;

        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            trySell(p, p->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
        for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
        {
            Bag* bag = p->GetBagByPos(b);
            if (!bag) continue;
            for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                trySell(p, p->GetItemByPos(b, uint8(j)));
        }
    }

    if (soldCount == 0)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r No grey items to sell.");
        return;
    }

    requester->ModifyMoney(int32(totalMoney));
    ChatHandler(requester->GetSession()).PSendSysMessage(
        "|cff66ccff[WowPsParty]|r Sold |cffffffff{}|r grey item(s) for |cffffd100{}.{}.{}|r.",
        soldCount, totalMoney / 10000, (totalMoney / 100) % 100, totalMoney % 100);

    WowPsParty::SendInventoryTo(requester);
}

// MOVE\t<srcPartySlot>\t<srcItemGuidLow>\t<destPartySlot>
//   Move an item from src char's bag into dest char's bags (free slot). Uses
//   the same Item-preserving path as EQUIP; doesn't equip on arrival. For
//   in-character rearrangement srcPartySlot == destPartySlot is supported.
static void HandleMove(Player* requester, std::string_view payload)
{
    auto p1 = payload.find('\t');
    if (p1 == std::string_view::npos) return;
    auto p2 = payload.find('\t', p1 + 1);
    if (p2 == std::string_view::npos) return;
    uint32 const srcSlot = std::strtoul(std::string(payload.substr(0, p1)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(p1 + 1, p2 - p1 - 1)).c_str(), nullptr, 10);
    uint32 const dstSlot = std::strtoul(std::string(payload.substr(p2 + 1)).c_str(), nullptr, 10);
    if (!srcItemGuidLow) return;
    if (!requester || !requester->GetSession()) return;
    uint32 const account = requester->GetSession()->GetAccountId();

    QueryResult qs = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}", account, srcSlot);
    if (!qs) return;
    QueryResult qd = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}", account, dstSlot);
    if (!qd) return;

    Player* srcChar = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(qs->Fetch()[0].Get<uint32>()));
    Player* dstChar = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(qd->Fetch()[0].Get<uint32>()));
    if (!srcChar || !dstChar) return;

    Item* item = SafeGetItemByGuid(srcChar, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!item) return;

    if (srcChar == dstChar)
    {
        // Same-char rearrange: find best free slot in own bags.
        ItemPosCountVec pos;
        if (srcChar->CanStoreItem(NULL_BAG, NULL_SLOT, pos, item, true /*swap*/) != EQUIP_ERR_OK)
            return;
        srcChar->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
        srcChar->StoreItem(pos, item, true);
        FlushPartyTransfer(srcChar, nullptr);
    }
    else
    {
        srcChar->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
        item->SetOwnerGUID(dstChar->GetGUID());
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        item->SaveToDB(tx);
        CharacterDatabase.CommitTransaction(tx);
        ItemPosCountVec pos;
        if (dstChar->CanStoreItem(NULL_BAG, NULL_SLOT, pos, item, false) == EQUIP_ERR_OK)
            dstChar->MoveItemToInventory(pos, item, true);
        else
        {
            // bounce back
            item->SetOwnerGUID(srcChar->GetGUID());
            ItemPosCountVec backPos;
            if (srcChar->CanStoreItem(NULL_BAG, NULL_SLOT, backPos, item, false) == EQUIP_ERR_OK)
            {
                srcChar->MoveItemToInventory(backPos, item, true);
                // Re-credit the collect-quest counter MoveItemFromInventory decremented;
                // the item never actually left srcChar.
                srcChar->ItemAddedQuestCheck(item->GetEntry(), item->GetCount());
            }
            CharacterDatabaseTransaction tx2 = CharacterDatabase.BeginTransaction();
            item->SaveToDB(tx2);
            CharacterDatabase.CommitTransaction(tx2);
        }
        // Persist both members now — a stale source row that survives a deferred
        // save reloads the item onto the wrong character on relog.
        FlushPartyTransfer(srcChar, dstChar);
    }

    WowPsParty::SendInventoryTo(requester);
}

// TAKE\t<srcPartySlot>\t<srcItemGuidLow> — move the FULL item (whole stack) out of
// the party member in srcPartySlot and into the REQUESTER's own bags. The mirror of
// HandleMove's cross-char branch, but the destination is always the human who clicked,
// so loot that landed on the wrong character (e.g. a key/quest item the healer grabbed)
// can be pulled to yourself. Non-destructive; bounces back untouched if your bags are
// full or the item already belongs to you.
static void HandleTake(Player* requester, std::string_view payload)
{
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    uint32 const srcSlot        = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(tab + 1)).c_str(), nullptr, 10);
    if (!srcItemGuidLow) return;
    if (!requester || !requester->GetSession()) return;
    uint32 const account = requester->GetSession()->GetAccountId();

    uint32 const srcGuid = WowPsParty::GuidForAccountSlot(account, srcSlot);
    if (!srcGuid) return;
    Player* srcChar = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(srcGuid));
    if (!srcChar) return;

    Item* item = SafeGetItemByGuid(srcChar, ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!item) return;

    ItemTemplate const* t = WowPsParty::SafeItemTemplate(item);
    std::string const itemName = t ? t->Name1 : "item";

    if (srcChar == requester)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r |cffffffff{}|r is already in your bags.", itemName);
        return;
    }

    srcChar->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
    item->SetOwnerGUID(requester->GetGUID());
    CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
    item->SaveToDB(tx);
    CharacterDatabase.CommitTransaction(tx);

    ItemPosCountVec pos;
    if (requester->CanStoreItem(NULL_BAG, NULL_SLOT, pos, item, false) == EQUIP_ERR_OK)
    {
        requester->ItemAddedQuestCheck(item->GetEntry(), item->GetCount());
        requester->MoveItemToInventory(pos, item, true);
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cff66ccff[WowPsParty]|r Took |cffffffff{}|r from {}.", itemName, srcChar->GetName());
    }
    else
    {
        // Bags full — bounce the item back to the source character untouched.
        item->SetOwnerGUID(srcChar->GetGUID());
        ItemPosCountVec backPos;
        if (srcChar->CanStoreItem(NULL_BAG, NULL_SLOT, backPos, item, false) == EQUIP_ERR_OK)
        {
            srcChar->MoveItemToInventory(backPos, item, true);
            // MoveItemFromInventory above decremented srcChar's collect-quest counter;
            // the item is back on srcChar, so re-credit it or the counter desyncs down.
            srcChar->ItemAddedQuestCheck(item->GetEntry(), item->GetCount());
        }
        CharacterDatabaseTransaction tx2 = CharacterDatabase.BeginTransaction();
        item->SaveToDB(tx2);
        CharacterDatabase.CommitTransaction(tx2);
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Your bags are full — couldn't take |cffffffff{}|r.", itemName);
    }

    FlushPartyTransfer(srcChar, requester);
    WowPsParty::SendInventoryTo(requester);
}

// GOTO_DELTA — DISABLED (2026-06-03). The right-click-the-map teleport was
// removed: the client hook also fired on Blizzard's NORMAL world map, so a
// stray right-click teleported the whole party by accident. The feature is
// gone entirely — GMs use `.tele`. This handler is now a no-op so any
// GOTO_DELTA a stale/un-synced addon copy might still send is ignored instead
// of teleporting anyone. (Prior teleport implementation is in git history.)
static void HandleGotoDelta(Player* requester, std::string_view /*payload*/)
{
    if (!requester) return;
    LOG_INFO("module",
        "[WowPsParty] GOTO_DELTA ignored (feature disabled) from guid={}",
        requester->GetGUID().GetCounter());
}

namespace WowPsParty
{
    // Session-independent bootstrap. Pulls every unenrolled char on the
    // account and inserts them into free party slots. Returns the count
    // enrolled, and fills `messages` with human-readable lines describing
    // each step (caller decides where to surface them — chat for player,
    // log for SOAP/admin invocation).
    //
    // This is shared between the WPSP HandleBootstrapParty path (driven
    // from an in-world session) and the .wowps_admin bootstrap command
    // (driven from SOAP/console). Same logic, no duplication, so the
    // test runner exercising it via SOAP exercises the same code path
    // the addon button hits.
    uint32 BootstrapPartyForAccount(uint32 account, std::vector<std::string>& messages)
    {
        QueryResult existing = CharacterDatabase.Query(
            "SELECT `slot`, `guid` FROM `account_party` WHERE `account` = {} ORDER BY `slot`",
            account);
        std::vector<uint8> takenSlots;
        if (existing)
        {
            do { takenSlots.push_back(existing->Fetch()[0].Get<uint8>()); }
            while (existing->NextRow());
        }
        if (takenSlots.size() >= 5)
        {
            messages.emplace_back("Party is already full (5/5).");
            return 0;
        }

        std::vector<uint8> freeSlots;
        for (uint8 s = 0; s < 5; ++s)
        {
            bool taken = false;
            for (uint8 t : takenSlots) if (t == s) { taken = true; break; }
            if (!taken) freeSlots.push_back(s);
        }

        QueryResult q = CharacterDatabase.Query(
            "SELECT c.`guid`, c.`name` FROM `characters` c "
            "LEFT JOIN `account_party` ap ON ap.`guid` = c.`guid` "
            "WHERE c.`account` = {} AND ap.`guid` IS NULL "
            "ORDER BY c.`guid` LIMIT 5", account);
        if (!q)
        {
            messages.emplace_back("No unenrolled chars on this account.");
            return 0;
        }

        uint32 enrolled = 0;
        auto slotIt = freeSlots.begin();
        do
        {
            if (slotIt == freeSlots.end()) break;
            Field* f = q->Fetch();
            uint32 const guid = f[0].Get<uint32>();
            std::string const name = f[1].Get<std::string>();
            uint8 const slot = *slotIt++;

            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `account_party` (`account`, `slot`, `guid`, `is_active_on_login`) "
                "VALUES ({}, {}, {}, {})",
                account, slot, guid, (slot == 0 ? 1u : 0u));
            tx->Append(
                "UPDATE `characters` SET `party_slot` = {} WHERE `guid` = {}",
                slot, guid);
            CharacterDatabase.CommitTransaction(tx);

            messages.emplace_back(Acore::StringFormat(
                "Enrolled {} (guid={}) at slot {}.", name, guid, uint32(slot)));
            ++enrolled;
        } while (q->NextRow());

        if (enrolled == 0)
            messages.emplace_back("No new chars to enroll.");
        else
            messages.emplace_back(Acore::StringFormat(
                "Bootstrapped {} char(s). Log out and back in to spawn the full party.", enrolled));

        return enrolled;
    }
}

static void HandleBootstrapParty(Player* requester)
{
    if (!requester || !requester->GetSession()) return;
    uint32 const account = requester->GetSession()->GetAccountId();

    std::vector<std::string> messages;
    uint32 const enrolled = WowPsParty::BootstrapPartyForAccount(account, messages);
    ChatHandler ch(requester->GetSession());
    for (auto const& m : messages)
        ch.PSendSysMessage("|cff66ccff[WowPsParty]|r {}", m);
    if (enrolled > 0)
        WowPsParty::SendRosterTo(requester);
}

// ---------------------------------------------------------------------------
// Key staging — a locked world object the party could open should open for the
// human, whoever is carrying the key.
//
// A lock's ITEM requirement (Lock.dbc LOCK_KEY_ITEM — Skadi's Harpoon Launchers
// are lock 1777 -> item 37372 "Harpoon") is enforced by the CLIENT, not by us.
// The lock id ships to the client in SMSG_GAMEOBJECT_QUERY_RESPONSE, the client
// resolves it against the LOCAL player's own bags, and when the key isn't there
// it never sends CMSG_GAMEOBJ_USE at all — GameObject::Use would have run the
// object quite happily. So unlike every other shared-inventory requirement there
// is no server-side `return` to relax with a trampoline: the key has to
// physically be in the human's bags before they click. That is exactly what the
// Party Inventory panel's Take was being used for by hand.
//
// So do it for them: while a human stands near an object whose lock names a key
// they don't hold, consolidate that key out of the party's bags. The sweep
// starts well outside interaction range so the object is already clickable by
// the time they walk up to it.
// ---------------------------------------------------------------------------

// Far enough that the key lands before the player reaches the ~5 yard
// interaction range, near enough that we only ever react to an object they are
// actually walking up to.
static constexpr float KEY_STAGE_RANGE_YD = 30.0f;
static constexpr uint32 KEY_STAGE_INTERVAL_MS = 1000;

// The key items named by the locks of spawned objects around `human`.
static void CollectNearbyLockKeys(Player* human, std::vector<uint32>& keys)
{
    struct LockedGameObjectCheck
    {
        LockedGameObjectCheck(Player const* src, float range) : _src(src), _range(range) { }
        bool operator()(GameObject* go) const
        {
            return go && go->isSpawned() && go->GetGOInfo()->GetLockId()
                && _src->IsWithinDistInMap(go, _range);
        }
        Player const* _src;
        float _range;
    };

    std::list<GameObject*> nearby;
    LockedGameObjectCheck check(human, KEY_STAGE_RANGE_YD);
    Acore::GameObjectListSearcher<LockedGameObjectCheck> searcher(human, nearby, check);
    Cell::VisitObjects(human, searcher, KEY_STAGE_RANGE_YD);

    for (GameObject* go : nearby)
    {
        LockEntry const* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
        if (!lock) continue;
        for (uint8 j = 0; j < MAX_LOCK_CASE; ++j)
            if (lock->Type[j] == LOCK_KEY_ITEM && lock->Index[j]
                && std::find(keys.begin(), keys.end(), lock->Index[j]) == keys.end())
                keys.push_back(lock->Index[j]);
    }
}

// Every loose copy of `itemId` in `owner`'s bags and KEYRING — collected before
// anything moves, because the first MoveItemFromInventory invalidates the slot
// walk. Keys sit in the keyring far more often than in a bag, so unlike the
// profession-tool search this covers it. SafeItemTemplate, not GetEntry: a
// mate's bag can hold a freed Item* whose value array faults on read.
static void CollectLooseCopies(Player* owner, uint32 itemId, std::vector<Item*>& out)
{
    auto consider = [&](Item* item)
    {
        if (!item || item->IsEquipped() || item->IsNotEmptyBag() || item->IsInTrade()) return;
        ItemTemplate const* proto = WowPsParty::SafeItemTemplate(item);
        if (proto && proto->ItemId == itemId)
            out.push_back(item);
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(owner->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 slot = KEYRING_SLOT_START; slot < CURRENCYTOKEN_SLOT_END; ++slot)
        consider(owner->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = owner->GetBagByPos(bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                consider(bag->GetItemByPos(slot));
}

// Move every copy of `key` that `owner` is carrying onto the human, counting them
// into `moved`. Whole stacks, not one key: a consumable key (a harpoon) is spent
// per click, and dripping the next one a second later is the same friction this
// removes. False means the human's bags are full, so the caller should stop —
// nothing else is going to fit either.
static bool DrainKeyFromHolder(Player* human, Player* owner, uint32 key, uint32& moved)
{
    std::vector<Item*> copies;
    CollectLooseCopies(owner, key, copies);
    for (Item* copy : copies)
    {
        ItemTemplate const* proto = WowPsParty::SafeItemTemplate(copy);
        std::string const keyName = proto ? proto->Name1 : "key";
        // PullItemToRequester returns null BOTH when the human's bags are full and
        // when the stack merged into one they already held, and the Item* is gone
        // either way — so ask the human's own count which of the two happened
        // rather than dereferencing it.
        uint32 const before = human->GetItemCount(key);
        if (!PullItemToRequester(human, owner, copy) && human->GetItemCount(key) == before)
        {
            LOG_WARN("module", "[WowPsParty KeyStage] {} could not take '{}'(entry={}) from {} — bags full",
                     human->GetName(), keyName, key, owner->GetName());
            return false;
        }
        ++moved;
        LOG_INFO("module", "[WowPsParty KeyStage] {} took '{}'(entry={}) from {} for a nearby locked object",
                 human->GetName(), keyName, key, owner->GetName());
    }
    return true;
}

// Consolidate every party-held copy of `keys` onto the human; returns how many
// landed. Henchmen and hired alts count as holders even though their bags are
// hidden from the Party Inventory panel: "any of my party members" is the whole
// party, and a key nobody can even see is the worst case of all.
static uint32 StageKeysFromParty(Player* human, std::vector<uint32> const& keys)
{
    std::vector<ObjectGuid> party;
    WowPsParty::GetPartyGuidsFor(human->GetGUID(), party);

    uint32 moved = 0;
    for (ObjectGuid const& memberGuid : party)
    {
        if (memberGuid == human->GetGUID()) continue;
        Player* owner = ObjectAccessor::FindConnectedPlayer(memberGuid);
        if (!owner || WowPsParty::MemberStorageUnstable(owner)) continue;
        // Same-Map* holders only — an off-map member is driven by another
        // MapUpdater thread, and mutating its bags from here races its save (the
        // use-after-free the reagent trampolines document).
        if (owner->GetMap() != human->GetMap()) continue;

        for (uint32 key : keys)
            if (!DrainKeyFromHolder(human, owner, key, moved))
                return moved;   // one bags-full warning per sweep, not one per holder
    }
    return moved;
}

// Drives the staging once a second, from the world tick — MapMgr::Update has
// already joined every map worker by the time WorldScript::OnUpdate runs, so
// reading one member's bags and writing another's is not racing a map update.
// This is the same window the follow ticker moves party members in.
class PartyKeyStageWorldScript : public WorldScript
{
public:
    PartyKeyStageWorldScript() : WorldScript("PartyKeyStageWorldScript", {
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE
    }) { }

    // The sweep only ever logs when it moves something, so without this there is
    // no way to tell "armed and nothing to do" from "this build predates it".
    void OnStartup() override
    {
        LOG_INFO("module", "[WowPsParty KeyStage] sweep registered (interval={}ms, range={}y)",
                 KEY_STAGE_INTERVAL_MS, KEY_STAGE_RANGE_YD);
    }

    void OnUpdate(uint32 diff) override
    {
        if (!WowPsParty::IsEnabled()) return;
        _accum += diff;
        if (_accum < KEY_STAGE_INTERVAL_MS) return;
        _accum = 0;

        for (ObjectGuid const& leaderGuid : ActivePartyLeaders())
            StageFor(ObjectAccessor::FindConnectedPlayer(leaderGuid));
    }

private:
    // The distinct leaders of every tracked follow directive — read from our own
    // in-memory directives rather than the session list or account_party, so a
    // once-a-second sweep costs no query and never sees an unrelated player.
    static std::vector<ObjectGuid> ActivePartyLeaders()
    {
        std::vector<ObjectGuid> followers;
        WowPsParty::GetAllFollowers(followers);

        std::vector<ObjectGuid> leaders;
        for (ObjectGuid const& follower : followers)
        {
            ObjectGuid const leader = WowPsParty::GetLeaderFor(follower);
            if (leader && std::find(leaders.begin(), leaders.end(), leader) == leaders.end())
                leaders.push_back(leader);
        }
        return leaders;
    }

    static void StageFor(Player* human)
    {
        if (!human || WowPsParty::MemberStorageUnstable(human)) return;
        if (sPlayerbotsMgr.GetPlayerbotAI(human)) return;   // only a human clicks objects
        if (!human->IsAlive()) return;   // a corpse run past a chest shouldn't shuffle bags
        WorldSession* session = human->GetSession();
        if (!session || !WowPsParty::GetAccountSettings(session->GetAccountId()).sharedInventory)
            return;

        std::vector<uint32> keys;
        CollectNearbyLockKeys(human, keys);
        if (keys.empty()) return;

        // Already able to open everything in reach — don't walk the party's bags
        // (and don't hoover up spare copies of a key that is doing its job).
        if (std::all_of(keys.begin(), keys.end(),
                        [human](uint32 key) { return human->HasItemCount(key); }))
            return;

        if (StageKeysFromParty(human, keys))
            WowPsParty::SendInventoryTo(human);
    }

    uint32 _accum = 0;
};

class PartyAddonProtocolScript : public PlayerScript
{
public:
    PartyAddonProtocolScript() : PlayerScript("PartyAddonProtocolScript", {
        // PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE is the hook that ACTUALLY
        // fires OnPlayerBeforeSendChatMessage (see
        // src/server/game/Scripting/ScriptDefines/PlayerScript.cpp:182).
        // PLAYERHOOK_ON_CHAT is a different bit and was silently swallowing
        // every WPSP client->server command for the entire life of the
        // module (PET_BAR_SET, GOTO_DELTA, UNEQUIP, REQ_*, etc.). Login-
        // time pushes still worked because those go via OnPlayerLogin.
        PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE,
        PLAYERHOOK_ON_LOGIN
    }) { }

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override
    {
        if (!WowPsParty::IsEnabled() || !player) return;
        if (lang != LANG_ADDON) return;
        if (msg.compare(0, WPSP_PREFIX_LEN, "WPSP\t") != 0) return;

        std::string_view body(msg.data() + WPSP_PREFIX_LEN, msg.size() - WPSP_PREFIX_LEN);
        std::string_view command = body;
        std::string_view payload;
        if (auto tab = body.find('\t'); tab != std::string_view::npos)
        {
            command = body.substr(0, tab);
            payload = body.substr(tab + 1);
        }

        // Reassemble an oversized line the addon split into WPSP_FRAG chunks (a
        // single rotation rule can exceed the 3.3.5a ~255-byte addon-message cap).
        // Once complete, dispatch the reassembled body as if it had arrived whole;
        // `reassembled` is the backing store the string_views point into, so it
        // must outlive the rest of this method.
        std::string reassembled;
        if (command == "WPSP_FRAG")
        {
            if (!ReassembleFragment(player, payload, reassembled))
                return;   // more chunks pending (or malformed) — nothing to dispatch yet
            body = reassembled;
            command = body;
            payload = std::string_view();
            if (auto tab = body.find('\t'); tab != std::string_view::npos)
            {
                command = body.substr(0, tab);
                payload = body.substr(tab + 1);
            }
            LOG_INFO("module",
                "[WowPsParty] WPSP_FRAG reassembled from guid={} -> cmd='{}' body_len={}",
                player->GetGUID().GetCounter(), std::string(command), uint32(body.size()));
        }

        // Unconditional inbound-message trace: every WPSP command, who sent
        // it, and the payload length. Lets me see in Server.log exactly which
        // commands reach the dispatcher versus which never arrive (i.e., the
        // addon is broken on the send side).
        LOG_INFO("module",
            "[WowPsParty] WPSP RECV: from guid={} cmd='{}' payload_len={}",
            player->GetGUID().GetCounter(),
            std::string(command),
            uint32(payload.size()));

        if (command == "REQ_ROSTER")
        {
            WowPsParty::SendRosterTo(player);
        }
        else if (command == "REQ_SETTINGS")
        {
            WowPsParty::SendSettingsTo(player);
        }
        else if (command == "SET_SETTING")
        {
            // SET_SETTING\t<key>\t<0|1>
            std::string s(payload);
            auto t = s.find('\t');
            if (t == std::string::npos) return;
            std::string const key = s.substr(0, t);
            bool const val = std::strtoul(s.substr(t + 1).c_str(), nullptr, 10) != 0;
            uint32 const account = player->GetSession()->GetAccountId();
            WowPsParty::SetAccountSetting(account, key, val);
            LOG_INFO("module",
                "[WowPsParty] SET_SETTING account={} {}={}", account, key, val ? 1 : 0);
            WowPsParty::SendSettingsTo(player);   // echo back the full set
        }
        else if (command == "SET_XPRATE")
        {
            // SET_XPRATE\t<quest|kill>\t<rate>   rate = percent (100-500, server clamps)
            std::string s(payload);
            auto t = s.find('\t');
            if (t == std::string::npos) return;
            std::string const which = s.substr(0, t);
            if (which != "quest" && which != "kill") return;
            // rate is unvalidated here on purpose — SetAccountXpRate clamps it to
            // [XP_RATE_MIN, XP_RATE_MAX], so any out-of-range / garbage value is
            // bounded before it touches the DB.
            uint32 const rate = std::strtoul(s.substr(t + 1).c_str(), nullptr, 10);
            uint32 const account = player->GetSession()->GetAccountId();
            WowPsParty::SetAccountXpRate(account, which == "quest", rate);
            LOG_INFO("module",
                "[WowPsParty] SET_XPRATE account={} {}={}", account, which, rate);
            WowPsParty::SendSettingsTo(player);   // echo back the clamped value
        }
        else if (command == "REQ_BLACKLIST")
        {
            WowPsParty::SendLootBlacklistTo(player);
        }
        else if (command == "BL_ADD")
        {
            // BL_ADD\t<item id | exact item name>
            std::string const text(payload);
            uint32 const account = player->GetSession()->GetAccountId();
            uint32 shared = 0;
            uint32 const entry = WowPsParty::ResolveItemEntryForBlacklist(text, &shared);
            if (!entry)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff66ccff[WowPsParty]|r no item called \"{}\" — shift-click the item "
                    "into the box, or type its exact name.", text);
                return;
            }
            ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(entry);
            std::string const name = tmpl ? tmpl->Name1 : std::to_string(entry);
            if (WowPsParty::AddLootBlacklist(account, entry))
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff66ccff[WowPsParty]|r your party will leave |cffffffff|Hitem:{}::::::::1::::|h[{}]|h|r behind.",
                    entry, name);
                // Say so rather than picking silently: the player asked for a name and
                // got one specific item out of several that answer to it.
                if (shared > 1)
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cff66ccff[WowPsParty]|r {} items share that name — this is entry {}. "
                        "Shift-click the one you mean if it isn't this one.", shared, entry);
            }
            else
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff66ccff[WowPsParty]|r {} is already blacklisted.", name);
            LOG_INFO("module",
                "[WowPsParty Blacklist] account={} add '{}' -> entry {}", account, text, entry);
            WowPsParty::SendLootBlacklistTo(player);
        }
        else if (command == "BL_DEL")
        {
            // BL_DEL\t<item id>
            uint32 const entry = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            uint32 const account = player->GetSession()->GetAccountId();
            if (WowPsParty::RemoveLootBlacklist(account, entry))
            {
                ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(entry);
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff66ccff[WowPsParty]|r your party will pick up {} again.",
                    tmpl ? tmpl->Name1 : std::to_string(entry));
            }
            LOG_INFO("module",
                "[WowPsParty Blacklist] account={} remove entry {}", account, entry);
            WowPsParty::SendLootBlacklistTo(player);
        }
        else if (command == "REQ_HENCHMEN")
        {
            WowPsParty::SendHenchmenTo(player);
        }
        else if (command == "HIRE_HENCHMAN")
        {
            // HIRE_HENCHMAN\t<guid>\t<role>
            std::string s(payload);
            auto t = s.find('\t');
            uint32 const guid = std::strtoul(
                (t == std::string::npos ? s : s.substr(0, t)).c_str(), nullptr, 10);
            std::string const role = (t == std::string::npos) ? "dps" : s.substr(t + 1);
            std::string msg;
            bool const ok = WowPsParty::HireHenchman(player, guid, role, msg);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r {}", msg);
            if (ok) WowPsParty::SendHenchmenTo(player);  // refresh (the hired one is now busy)
        }
        else if (command == "FILL_PARTY")
        {
            // Complete the party to 1T/1H/3D with random-pool henchmen, roles chosen
            // from the current party's set roles (15% off). No arguments.
            std::string msg;
            WowPsParty::FillPartyRandomly(player, msg);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r {}", msg);
            WowPsParty::SendHenchmenTo(player);   // refresh (hired ones are now busy)
        }
        else if (command == "DISMISS_HENCHMAN")
        {
            uint32 const guid = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            WowPsParty::DismissHenchman(player, guid);
            WowPsParty::SendHenchmenTo(player);
        }
        else if (command == "DISMISS_ALL_HENCHMEN")
        {
            WowPsParty::DismissAllHenchmen(player);
            WowPsParty::SendHenchmenTo(player);
        }
        else if (command == "REQ_ALTS")
        {
            WowPsParty::SendAltsTo(player);
        }
        else if (command == "HIRE_ALT")
        {
            // HIRE_ALT\t<guid>
            uint32 const guid = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            std::string msg;
            bool const ok = WowPsParty::HireAlt(player, guid, msg);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r {}", msg);
            if (ok) WowPsParty::SendAltsTo(player);   // refresh (the hired one is now busy)
        }
        else if (command == "DISMISS_ALT")
        {
            uint32 const guid = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            WowPsParty::DismissHiredAlt(player, guid);
            WowPsParty::SendAltsTo(player);
        }
        else if (command == "DISMISS_ALL_ALTS")
        {
            WowPsParty::DismissAllHiredAlts(player);
            WowPsParty::SendAltsTo(player);
        }
        else if (command == "REQ_SPELLBOOK")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            WowPsParty::SendSpellbookForGuid(player, guid, token);
        }
        else if (command == "REQ_GEAR")
        {
            uint32 slot = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            WowPsParty::SendGearTo(player, slot);
        }
        // REQ_STATS\t<slot>  →  STATS\t<slot>\t<key>:<val>;...
        else if (command == "REQ_STATS")
        {
            uint32 slot = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            WowPsParty::SendStatsTo(player, slot);
        }
        else if (command == "REQ_CURRENCY")
        {
            WowPsParty::SendCurrencyTo(player, std::string(payload));
        }
        // REQ_GENROT\t<token>  →  GENROT\t<token>\t<dsl>
        // REQ_GENROT replies with a generated rotation DSL for the editor.
        else if (command == "REQ_GENROT")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            uint8 cls = 0;
            if (Player* p = ObjectAccessor::FindConnectedPlayer(
                    ObjectGuid::Create<HighGuid::Player>(guid)))
                cls = p->getClass();
            else
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `class` FROM `characters` WHERE `guid` = {}", guid);
                if (q) cls = q->Fetch()[0].Get<uint8>();
            }
            // Match the role the bot actually runs (henchman/alt directive),
            // so Generate previews the same rotation hire applied; falls back
            // to the class default role when there's no directive.
            std::string const genRole = WowPsParty::RoleForGuid(
                ObjectGuid::Create<HighGuid::Player>(guid));
            // Bake the member's actual spec so Generate previews the SAME per-spec
            // rotation hire applied (a Frost mage sees the Frost list, not a cross-
            // school one); -1 (no talents) falls back to the basic rotation.
            std::string dsl = WowPsParty::DefaultRotationForClass(
                cls, genRole, WowPsParty::DominantTreeForGuid(guid));
            std::replace(dsl.begin(), dsl.end(), '|', '~');
            std::ostringstream out;
            out << "GENROT\t" << token << '\t' << dsl;
            SendWPSP(player, out.str());
        }
        else if (command == "REQ_INVENTORY")
        {
            WowPsParty::SendInventoryTo(player);
        }
        // REQ_TARGETMODE\t<token>  →  TARGETMODE\t<token>\t<mode>
        else if (command == "REQ_TARGETMODE")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            std::string mode = "master";
            if (guid)
            {
                // The in-memory cache is what AssistTarget actually reads, so it
                // is authoritative for the dropdown. The DB only overrides it
                // when the user explicitly saved a mode — a row can also exist
                // with an EMPTY strategies_csv (created by a rotation-only
                // commit), so an empty value must NOT mask the cache (that was
                // the "tank henchman shows master" display drift).
                mode = WowPsParty::GetTargetMode(guid);
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `strategies_csv` FROM `party_loadout` WHERE `guid` = {}", guid);
                if (q)
                {
                    std::string s = q->Fetch()[0].Get<std::string>();
                    if (!s.empty()) mode = s;
                }
            }
            std::ostringstream out;
            out << "TARGETMODE\t" << token << '\t' << mode;
            SendWPSP(player, out.str());
        }
        // SET_TARGETMODE\t<token>\t<mode>
        else if (command == "SET_TARGETMODE")
        {
            std::string rest;
            std::string const token = WowPsParty::SplitToken(std::string(payload), rest);
            std::string mode = rest;
            // Whitelist — only known modes reach the DB (also blocks any
            // injection via the stored-into-SQL string).
            if (mode != "master" && mode != "tank" && mode != "nearest"
                && mode != "loose" && mode != "lowest" && mode != "highest")
                return;
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`) VALUES ({}, '{}', '', '', '', '') "
                "ON DUPLICATE KEY UPDATE `strategies_csv` = VALUES(`strategies_csv`)",
                guid, mode);
            // SYNCHRONOUS commit, like every party_loadout write below it: hiring or
            // inviting this character re-reads the row into the runtime caches, so a
            // write still queued in the DB worker would hand the refresh the OLD
            // value and silently revert the toggle the player just set.
            CharacterDatabase.DirectCommitTransaction(tx);
            WowPsParty::TargetModeCacheSet(guid, mode);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Target mode set to '{}'.", mode);
        }
        // REQ_LEADDUNGEON\t<token>  →  LEADDUNGEON\t<token>\t<0|1>
        else if (command == "REQ_LEADDUNGEON")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            bool on = true;
            if (guid)
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `glyphs_csv` FROM `party_loadout` WHERE `guid` = {}", guid);
                if (q && q->Fetch()[0].Get<std::string>() == "0") on = false;
                else if (!q) on = WowPsParty::GetLeadInDungeon(guid);
            }
            std::ostringstream out;
            out << "LEADDUNGEON\t" << token << '\t' << (on ? 1 : 0);
            SendWPSP(player, out.str());
        }
        // REQ_ROTATION\t<token>  ->  ROT_BEGIN\t<token> / ROT_CHUNK\t<frag> / ROT_END\t<token>
        // The editor pulls the SAVED rotation from party_loadout (guid-keyed,
        // authoritative) so a reshuffled party slot never shows the previous
        // occupant's rotation out of the slot-keyed client cache. Chunked (the DSL
        // can exceed the ~255-byte addon-message cap and would otherwise arrive
        // EMPTY — the same footgun that hid the Common list); the '|' field sep
        // becomes '~' inside the helper for the editor's import parser.
        else if (command == "REQ_ROTATION")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            std::string dsl;
            if (guid)
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `priority_actions_json` FROM `party_loadout` "
                    "WHERE `guid` = {}", guid);
                if (q) dsl = q->Fetch()[0].Get<std::string>();
            }
            SendChunkedRotation(player, "ROT_BEGIN\t" + token, "ROT_CHUNK",
                                dsl, "ROT_END\t" + token);
        }
        // SET_LEADDUNGEON\t<token>\t<0|1>
        else if (command == "SET_LEADDUNGEON")
        {
            std::string rest;
            std::string const token = WowPsParty::SplitToken(std::string(payload), rest);
            bool const on = (rest != "0");
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`) VALUES ({}, '', '', '{}', '', '') "
                "ON DUPLICATE KEY UPDATE `glyphs_csv` = VALUES(`glyphs_csv`)",
                guid, on ? "1" : "0");
            CharacterDatabase.DirectCommitTransaction(tx);   // sync — a later hire/invite re-reads this row
            WowPsParty::LeadDungeonCacheSet(guid, on);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Lead-in-dungeons: {}.", on ? "ON" : "OFF");
        }
        // REQ_WAITTHREAT\t<token>  ->  WAITTHREAT\t<token>\t<0|1>
        // Reports the EFFECTIVE value (explicit override, else the default) so the
        // editor checkbox shows the REAL runtime behaviour even for a bot the user
        // never configured. The runtime gate (WaitForHumanTank) defaults EVERY
        // non-tank — hero and henchman alike — to WAIT under a tank lead, so an
        // unset bot reports ON. (The old readout showed hero->OFF via a per-type
        // default the runtime stopped consulting, so heroes appeared "not waiting"
        // in the editor while they actually held — Kevin: "i dont have wait for
        // tank threat enabled yet ... bots are just waiting". Unchecking it now
        // writes an explicit '0' that truly opts the bot into blasting.)
        else if (command == "REQ_WAITTHREAT")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            bool on = true;
            if (guid)
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `wait_tank_threat` FROM `party_loadout` WHERE `guid` = {}", guid);
                std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
                if (v == "0")      on = false;
                else               on = true;   // '1' or unset -> wait (runtime default)
            }
            std::ostringstream out;
            out << "WAITTHREAT\t" << token << '\t' << (on ? 1 : 0);
            SendWPSP(player, out.str());
        }
        // SET_WAITTHREAT\t<token>\t<0|1>  — explicit override (the user toggled it)
        else if (command == "SET_WAITTHREAT")
        {
            std::string rest;
            std::string const token = WowPsParty::SplitToken(std::string(payload), rest);
            if (rest != "0" && rest != "1") return;   // strict: ignore a malformed/empty value
            bool const on = (rest == "1");
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`, `wait_tank_threat`) "
                "VALUES ({}, '', '', '', '', '', '{}') "
                "ON DUPLICATE KEY UPDATE `wait_tank_threat` = VALUES(`wait_tank_threat`)",
                guid, on ? "1" : "0");
            CharacterDatabase.DirectCommitTransaction(tx);   // sync — a later hire/invite re-reads this row
            WowPsParty::WaitTankThreatCacheSet(guid, on ? 1 : 0);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Wait for tank threat: {}.", on ? "ON" : "OFF");
        }
        // REQ_SAFEPULL\t<token>  ->  SAFEPULL\t<token>\t<0|1>
        // Reports the EFFECTIVE value (explicit override, else the per-type default:
        // a HERO/alt safe-pulls, a HENCHMAN barges) so the editor checkbox shows the
        // real runtime behaviour even for a bot the user never configured.
        else if (command == "REQ_SAFEPULL")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            bool on = true;
            if (guid)
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `safe_pull` FROM `party_loadout` WHERE `guid` = {}", guid);
                std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
                if (v == "1")      on = true;
                else if (v == "0") on = false;
                else               on = !WowPsParty::IsHenchman(
                                            ObjectGuid::Create<HighGuid::Player>(guid));
            }
            std::ostringstream out;
            out << "SAFEPULL\t" << token << '\t' << (on ? 1 : 0);
            SendWPSP(player, out.str());
        }
        // SET_SAFEPULL\t<token>\t<0|1>  — explicit override (the user toggled it)
        else if (command == "SET_SAFEPULL")
        {
            std::string rest;
            std::string const token = WowPsParty::SplitToken(std::string(payload), rest);
            if (rest != "0" && rest != "1") return;   // strict: ignore a malformed/empty value
            bool const on = (rest == "1");
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`, `safe_pull`) "
                "VALUES ({}, '', '', '', '', '', '{}') "
                "ON DUPLICATE KEY UPDATE `safe_pull` = VALUES(`safe_pull`)",
                guid, on ? "1" : "0");
            CharacterDatabase.DirectCommitTransaction(tx);   // sync — a later hire/invite re-reads this row
            WowPsParty::SafePullCacheSet(guid, on ? 1 : 0);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Safe pull: {}.", on ? "ON" : "OFF");
        }
        // REQ_FOLLOWPATH\t<token>  ->  FOLLOWPATH\t<token>\t<0|1>
        // Reports the EFFECTIVE value: explicit override, else the default (ON) — so the
        // editor checkbox shows the real runtime behaviour even for an unconfigured tank.
        else if (command == "REQ_FOLLOWPATH")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            bool on = true;
            if (guid)
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `follow_path` FROM `party_loadout` WHERE `guid` = {}", guid);
                std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
                on = (v != "0");   // '' or '1' -> ON; only an explicit '0' turns it off
            }
            std::ostringstream out;
            out << "FOLLOWPATH\t" << token << '\t' << (on ? 1 : 0);
            SendWPSP(player, out.str());
        }
        // SET_FOLLOWPATH\t<token>\t<0|1>  — explicit override (the user toggled it)
        else if (command == "SET_FOLLOWPATH")
        {
            std::string rest;
            std::string const token = WowPsParty::SplitToken(std::string(payload), rest);
            if (rest != "0" && rest != "1") return;   // strict: ignore a malformed/empty value
            bool const on = (rest == "1");
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`, `follow_path`) "
                "VALUES ({}, '', '', '', '', '', '{}') "
                "ON DUPLICATE KEY UPDATE `follow_path` = VALUES(`follow_path`)",
                guid, on ? "1" : "0");
            CharacterDatabase.DirectCommitTransaction(tx);   // sync — a later hire/invite re-reads this row
            WowPsParty::FollowPathCacheSet(guid, on ? 1 : 0);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Follow recorded path: {}.", on ? "ON" : "OFF");
        }
        // REQ_PULLGRAYS\t<token>  ->  PULLGRAYS\t<token>\t<0|1>
        // Reports the stored value (explicit column, else the default OFF) so the editor
        // checkbox shows the real runtime behaviour even for an unconfigured tank.
        else if (command == "REQ_PULLGRAYS")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            bool on = false;
            if (guid)
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `pull_grays` FROM `party_loadout` WHERE `guid` = {}", guid);
                std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
                on = (v == "1");   // '' or '0' -> OFF; only an explicit '1' turns it on
            }
            std::ostringstream out;
            out << "PULLGRAYS\t" << token << '\t' << (on ? 1 : 0);
            SendWPSP(player, out.str());
        }
        // SET_PULLGRAYS\t<token>\t<0|1>  — explicit override (the user toggled it)
        else if (command == "SET_PULLGRAYS")
        {
            std::string rest;
            std::string const token = WowPsParty::SplitToken(std::string(payload), rest);
            if (rest != "0" && rest != "1") return;   // strict: ignore a malformed/empty value
            bool const on = (rest == "1");
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`, `pull_grays`) "
                "VALUES ({}, '', '', '', '', '', '{}') "
                "ON DUPLICATE KEY UPDATE `pull_grays` = VALUES(`pull_grays`)",
                guid, on ? "1" : "0");
            CharacterDatabase.DirectCommitTransaction(tx);   // sync — a later hire/invite re-reads this row
            WowPsParty::PullGraysCacheSet(guid, on ? 1 : 0);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Pull gray mobs: {}.", on ? "ON" : "OFF");
        }
        // REQ_ANCHORTANK\t<token>  ->  ANCHORTANK\t<token>\t<0|1>
        // Reports the stored value (explicit column, else the default OFF — there
        // is no per-type default) so the editor checkbox shows the real runtime
        // behaviour even for a bot the user never configured.
        else if (command == "REQ_ANCHORTANK")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            bool on = false;
            if (guid)
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `anchor_tank` FROM `party_loadout` WHERE `guid` = {}", guid);
                std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
                on = (v == "1");   // unset/'0' -> OFF
            }
            std::ostringstream out;
            out << "ANCHORTANK\t" << token << '\t' << (on ? 1 : 0);
            SendWPSP(player, out.str());
        }
        // SET_ANCHORTANK\t<token>\t<0|1>  — explicit value (the user toggled it)
        else if (command == "SET_ANCHORTANK")
        {
            std::string rest;
            std::string const token = WowPsParty::SplitToken(std::string(payload), rest);
            if (rest != "0" && rest != "1") return;   // strict: ignore a malformed/empty value
            bool const on = (rest == "1");
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`, `anchor_tank`) "
                "VALUES ({}, '', '', '', '', '', '{}') "
                "ON DUPLICATE KEY UPDATE `anchor_tank` = VALUES(`anchor_tank`)",
                guid, on ? "1" : "0");
            CharacterDatabase.DirectCommitTransaction(tx);   // sync — a later hire/invite re-reads this row
            WowPsParty::AnchorTankCacheSet(guid, on ? 1 : 0);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Anchor on tank: {}.", on ? "ON" : "OFF");
        }
        // REQ_PULLCOUNT\t<token>  ->  PULLCOUNT\t<token>\t<1..8>
        // Reports the EFFECTIVE lead-tank multi-pull size (explicit column, else the
        // default 3) so the editor stepper shows the real runtime value.
        else if (command == "REQ_PULLCOUNT")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            int n = 3;   // default
            if (guid)
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `pull_count` FROM `party_loadout` WHERE `guid` = {}", guid);
                std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
                int const parsed = v.empty() ? 0 : std::atoi(v.c_str());
                if (parsed >= 1 && parsed <= 8) n = parsed;   // unset/out-of-range -> default 3
            }
            std::ostringstream out;
            out << "PULLCOUNT\t" << token << '\t' << n;
            SendWPSP(player, out.str());
        }
        // SET_PULLCOUNT\t<token>\t<1..8>  — the tank's multi-pull cluster size
        else if (command == "SET_PULLCOUNT")
        {
            std::string rest;
            std::string const token = WowPsParty::SplitToken(std::string(payload), rest);
            int const n = rest.empty() ? 0 : std::atoi(rest.c_str());
            if (n < 1 || n > 8) return;   // strict: ignore an out-of-range value
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`, `pull_count`) "
                "VALUES ({}, '', '', '', '', '', '{}') "
                "ON DUPLICATE KEY UPDATE `pull_count` = VALUES(`pull_count`)",
                guid, n);
            CharacterDatabase.DirectCommitTransaction(tx);   // sync — a later hire/invite re-reads this row
            WowPsParty::PullCountCacheSet(guid, n);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Multi-pull count: {}.", n);
        }
        // REQ_LEADDIST\t<token>  ->  LEADDIST\t<token>\t<5..40>
        // Reports the EFFECTIVE lead-tank dungeon lead distance (explicit column,
        // else the default 10) so the editor slider shows the real runtime value.
        // MUST match WowPsParty::BotLeadDistance's default (10) — they diverged once
        // (this replied 15 while the tank actually led at 10), so the slider lied.
        else if (command == "REQ_LEADDIST")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            int n = 10;   // default — keep in sync with BotLeadDistance
            if (guid)
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `lead_distance` FROM `party_loadout` WHERE `guid` = {}", guid);
                std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
                int const parsed = v.empty() ? 0 : std::atoi(v.c_str());
                if (parsed >= 5 && parsed <= 40) n = parsed;   // unset/out-of-range -> default 10
            }
            std::ostringstream out;
            out << "LEADDIST\t" << token << '\t' << n;
            SendWPSP(player, out.str());
        }
        // SET_LEADDIST\t<token>\t<5..40>  — the tank's dungeon lead distance (yds)
        else if (command == "SET_LEADDIST")
        {
            std::string rest;
            std::string const token = WowPsParty::SplitToken(std::string(payload), rest);
            int const n = rest.empty() ? 0 : std::atoi(rest.c_str());
            if (n < 5 || n > 40) return;   // strict: ignore an out-of-range value
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`, `lead_distance`) "
                "VALUES ({}, '', '', '', '', '', '{}') "
                "ON DUPLICATE KEY UPDATE `lead_distance` = VALUES(`lead_distance`)",
                guid, n);
            CharacterDatabase.DirectCommitTransaction(tx);   // sync — a later hire/invite re-reads this row
            WowPsParty::LeadDistCacheSet(guid, n);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Lead distance: {} yds.", n);
        }
        // REQ_ENGAGERANGE\t<token>  ->  ENGAGERANGE\t<token>\t<10..40>
        // Reports the EFFECTIVE lead-tank initial engage range (explicit column, else
        // the default 20) so the editor slider shows the real runtime value. MUST match
        // WowPsParty::BotEngageRange's default (20).
        else if (command == "REQ_ENGAGERANGE")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            int n = 20;   // default — keep in sync with BotEngageRange
            if (guid)
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `engage_range` FROM `party_loadout` WHERE `guid` = {}", guid);
                std::string v = q ? q->Fetch()[0].Get<std::string>() : std::string();
                int const parsed = v.empty() ? 0 : std::atoi(v.c_str());
                if (parsed >= 10 && parsed <= 40) n = parsed;   // unset/out-of-range -> default 20
            }
            std::ostringstream out;
            out << "ENGAGERANGE\t" << token << '\t' << n;
            SendWPSP(player, out.str());
        }
        // SET_ENGAGERANGE\t<token>\t<10..40>  — the tank's auto-pull opener scan radius (yds)
        else if (command == "SET_ENGAGERANGE")
        {
            std::string rest;
            std::string const token = WowPsParty::SplitToken(std::string(payload), rest);
            int const n = rest.empty() ? 0 : std::atoi(rest.c_str());
            if (n < 10 || n > 40) return;   // strict: ignore an out-of-range value
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`, `engage_range`) "
                "VALUES ({}, '', '', '', '', '', '{}') "
                "ON DUPLICATE KEY UPDATE `engage_range` = VALUES(`engage_range`)",
                guid, n);
            CharacterDatabase.DirectCommitTransaction(tx);   // sync — a later hire/invite re-reads this row
            WowPsParty::EngageRangeCacheSet(guid, n);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Engage range: {} yds.", n);
        }
        // REQ_ROLE\t<token>  ->  ROLE\t<token>\t<tank|healer|dps>
        // The bot's effective role, so the editor can gray out controls that don't
        // apply to it. Prefer the live FOLLOWER directive (RoleForGuid); fall back to
        // persistent storage with the SAME precedence as MGMT_LIST (account_party.role
        // -> party_loadout.role -> 'dps') so a bot with no live directive still resolves.
        else if (command == "REQ_ROLE")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            std::string role;
            if (guid)
            {
                role = WowPsParty::RoleForGuid(ObjectGuid::Create<HighGuid::Player>(guid));
                if (role.empty())
                {
                    QueryResult q = CharacterDatabase.Query(
                        "SELECT COALESCE(ap.role, NULLIF(pl.role, ''), 'dps') AS role "
                        "FROM `characters` c "
                        "LEFT JOIN `account_party` ap ON ap.guid = c.guid AND ap.account = c.account "
                        "LEFT JOIN `party_loadout` pl ON pl.guid = c.guid "
                        "WHERE c.guid = {}",
                        guid);
                    if (q) role = q->Fetch()[0].Get<std::string>();
                }
            }
            if (role != "tank" && role != "healer" && role != "dps") role = "dps";
            std::ostringstream out;
            out << "ROLE\t" << token << '\t' << role;
            SendWPSP(player, out.str());
        }
        else if (command == "EQUIP")
        {
            HandleEquip(player, payload);
        }
        else if (command == "UNEQUIP")
        {
            HandleUnequip(player, payload);
        }
        else if (command == "MOVE")
        {
            HandleMove(player, payload);
        }
        else if (command == "TAKE")
        {
            HandleTake(player, payload);
        }
        else if (command == "SELL")
        {
            HandleSell(player, payload);
        }
        else if (command == "AH_SELL")
        {
            HandleAhSell(player, payload);
        }
        else if (command == "AH_BUY")
        {
            HandleAhBuy(player, payload);
        }
        else if (command == "PULL_REAGENT")
        {
            HandlePullReagent(player, payload);
        }
        else if (command == "PULL_TOOLS")
        {
            HandlePullTools(player, payload);
        }
        else if (command == "DESTROY")
        {
            HandleDestroy(player, payload);
        }
        else if (command == "USE")
        {
            HandleUse(player, payload);
        }
        else if (command == "USEBYID")
        {
            HandleUseById(player, payload);
        }
        else if (command == "BARCOUNT")
        {
            HandleBarCounts(player, payload);
        }
        else if (command == "SPLIT")
        {
            HandleSplit(player, payload);
        }
        else if (command == "BANK")
        {
            HandleBankDeposit(player, payload);
        }
        else if (command == "REQ_BANK")
        {
            SendBankTo(player);
        }
        else if (command == "WITHDRAW")
        {
            HandleWithdraw(player, payload);
        }
        else if (command == "GBANK")
        {
            HandleGuildBankDeposit(player, payload);
        }
        else if (command == "BUY_BANK_SLOT")
        {
            HandleBuyBankSlot(player);
        }
        else if (command == "BANK_BAG")
        {
            HandleBankBag(player, payload);
        }
        else if (command == "SORT_BAGS")
        {
            HandleSortBags(player);
        }
        else if (command == "REDISTRIBUTE_ITEMS")
        {
            HandleRedistributeItems(player);
        }
        else if (command == "SELL_TRASH")
        {
            HandleSellTrash(player);
        }
        else if (command == "REQ_BUYBACK")
        {
            SendBuybackTo(player);
        }
        else if (command == "BUYBACK")
        {
            HandleBuyback(player, payload);
        }
        else if (command == "REQ_ENCHANTS")
        {
            HandleReqEnchants(player, payload);
        }
        else if (command == "ENCHANT")
        {
            HandleEnchant(player, payload);
        }
        else if (command == "RUNEFORGE")
        {
            HandleRuneforge(player, payload);
        }
        else if (command == "REQ_GEMS")
        {
            HandleReqGems(player, payload);
        }
        else if (command == "GEM")
        {
            HandleGem(player, payload);
        }
        else if (command == "DISENCHANT")
        {
            HandleDisenchant(player, payload);
        }
        else if (command == "PROSPECT")
        {
            HandleProspect(player, payload);
        }
        else if (command == "MILL")
        {
            HandleMill(player, payload);
        }
        else if (command == "LEARN")
        {
            HandleLearnRecipe(player, payload);
        }
        else if (command == "PICKLOCK")
        {
            HandlePickLock(player, payload);
        }
        else if (command == "MAIL_SEND")
        {
            HandleMailSend(player, payload);
        }
        else if (command == "COME_HITHER")
        {
            // Recall the party to me and hold them ~2s — drag them out of ground
            // effects. `player` is the active controlled body the followers track.
            WowPsParty::RecallFollowers(player, 2000);
        }
        else if (command == "PULL_MORE")
        {
            // Send the lead tank to body-pull the nearest out-of-combat mob and hold
            // that order ~2s — chain-pull micro for M+. Re-armed on each keypress, so
            // the player spams it to chase a far pull (same 2s window as COME_HITHER).
            WowPsParty::PullNearestExtra(player, 2000);
        }
        // REQ_TALENTS\t<token>  → TALENTS\t...  (alt slot OR henchman "h<guid>")
        else if (command == "REQ_TALENTS")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            WowPsParty::SendTalentsForGuid(player, guid, token);
        }
        // LEARN_TALENT\t<token>\t<talentId>\t<rank 0-based>. Henchmen are fixed:
        // their talents are read-only, so a henchman token is rejected.
        else if (command == "LEARN_TALENT")
        {
            std::string s(payload);
            auto t1 = s.find('\t');
            if (t1 == std::string::npos) return;
            auto t2 = s.find('\t', t1 + 1);
            if (t2 == std::string::npos) return;
            std::string const token = s.substr(0, t1);
            uint32 const talentId = std::strtoul(s.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
            uint32 const rank     = std::strtoul(s.substr(t2 + 1).c_str(), nullptr, 10);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
            if (WowPsParty::IsHenchman(og)) return;   // henchman talents are fixed
            Player* p = ObjectAccessor::FindConnectedPlayer(og);
            if (!p) return;
            uint32 const freeBefore = p->GetFreeTalentPoints();
            p->LearnTalent(talentId, rank, false);  // false = normal spend rules
            LOG_INFO("module",
                "[WowPsParty Talents] LEARN token={} {} talentId={} rank={} "
                "freeBefore={} freeAfter={}",
                token, p->GetName(), talentId, rank, freeBefore,
                p->GetFreeTalentPoints());
            WowPsParty::SendTalentsForGuid(player, guid, token);
        }
        // RESET_TALENTS\t<token>  — free, anywhere. Henchmen are read-only.
        else if (command == "RESET_TALENTS")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
            if (WowPsParty::IsHenchman(og)) return;   // henchman talents are fixed
            Player* p = ObjectAccessor::FindConnectedPlayer(og);
            if (!p) return;
            p->resetTalents(true);   // noResetCost = true
            WowPsParty::SendTalentsForGuid(player, guid, token);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Reset talents (free).");
        }
        else if (command == "GOTO_DELTA")
        {
            HandleGotoDelta(player, payload);
        }
        else if (command == "BOOTSTRAP_PARTY")
        {
            HandleBootstrapParty(player);
        }
        else if (command == "REQ_QUEST_PROGRESS")
        {
            WowPsParty::SendQuestProgressTo(player);
        }
        else if (command == "RECORD_PATH_TOGGLE")
        {
            WowPsParty::TogglePathRecording(player);
        }
        else if (command == "RECORD_PATH_CLEAR")
        {
            if (player->GetMap() && player->GetMap()->IsDungeon())
            {
                uint32 const mapId   = player->GetMapId();
                uint32 const removed = WowPsParty::ClearPath(mapId, player);
                if (removed)
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cff66ccff[WowPsParty]|r Cleared this wing's path ({} waypoints).", removed);
                else
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cffff5555[WowPsParty]|r No recorded path here to clear.");
            }
            else
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Stand inside a dungeon to clear its path.");
            }
        }
        else if (command == "REQ_PATH_INFO")
        {
            uint32 const mapId = player->GetMapId();
            uint32 const n = WowPsParty::GetPathWaypointCount(mapId);
            std::ostringstream out;
            out << "PATH_INFO\t" << mapId << '\t' << n;
            SendWPSP(player, out.str());
        }
        // ---------- Chunked rotation save ------------------------------------
        // WoW 3.3.5a's SendAddonMessage silently drops anything past ~255
        // bytes. A 9-rule rotation easily exceeds 350 chars, so the whole
        // SET_ROTATION message used to vanish without a trace. The chunked
        // BEGIN / ROTATION_RULE / COMMIT_ROTATION sequence keeps every
        // individual message tiny — and a single rule that is itself over-long
        // (a big comma-separated focus/target_name list) arrives pre-reassembled
        // from its WPSP_FRAG chunks (see the dispatch head), so it can't be the
        // one message that silently vanishes either.
        else if (command == "BEGIN_ROTATION")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            PendingRotationMap()[guid].clear();
            LOG_INFO("module",
                "[WowPsParty] BEGIN_ROTATION editor={} target_guid={}",
                player->GetGUID().GetCounter(), guid);
        }
        else if (command == "ROTATION_RULE")
        {
            // payload = "<token>\t<condition>\t<action>\t<priority>[\t<flags>]"
            std::string s(payload);
            std::vector<std::string> fields;
            {
                size_t p = 0;
                while (p <= s.size())
                {
                    size_t t = s.find('\t', p);
                    if (t == std::string::npos)
                    {
                        fields.push_back(s.substr(p));
                        break;
                    }
                    fields.push_back(s.substr(p, t - p));
                    p = t + 1;
                }
            }
            if (fields.size() < 4) return;
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, fields[0]);
            if (!guid) return;
            WowPsParty::RotationRule r;
            r.condition = fields[1];
            r.action    = fields[2];
            r.priority  = std::atoi(fields[3].c_str());
            r.flags     = fields.size() >= 5 ? fields[4] : "";
            PendingRotationMap()[guid].push_back(std::move(r));
            LOG_INFO("module",
                "[WowPsParty] ROTATION_RULE target_guid={} cond='{}' act='{}' prio={} flags='{}'",
                guid, fields[1], fields[2], std::atoi(fields[3].c_str()),
                fields.size() >= 5 ? fields[4] : "");
        }
        else if (command == "COMMIT_ROTATION")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            auto& pending = PendingRotationMap();
            auto it = pending.find(guid);
            std::vector<WowPsParty::RotationRule> rules;
            if (it != pending.end())
            {
                rules = std::move(it->second);
                pending.erase(it);
            }
            // Sort by priority desc (matches ParseRotationString convention).
            std::stable_sort(rules.begin(), rules.end(),
                [](WowPsParty::RotationRule const& a, WowPsParty::RotationRule const& b)
                { return a.priority > b.priority; });

            std::string stored = WowPsParty::SerialiseRotationRules(rules);
            // Spell names carry apostrophes ("Avenger's Shield", "Hunter's
            // Mark") — escape before it goes into the '{}' literal or the ' ends
            // the string and the INSERT throws a 1064 syntax error.
            CharacterDatabase.EscapeString(stored);
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            // The empty strings for the other columns are INSERT-path
            // placeholders only — the ON DUPLICATE KEY UPDATE touches just
            // priority_actions_json, so an existing row's mode/lead are kept.
            // Don't "simplify" this into a plain INSERT: that would clobber them.
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`) VALUES ({}, '', '', '', '', '{}') "
                "ON DUPLICATE KEY UPDATE `priority_actions_json` = VALUES(`priority_actions_json`)",
                guid, stored);
            // SYNCHRONOUS commit: a save immediately followed by a roster invite of
            // the SAME character makes Enroll re-read party_loadout into the cache,
            // and an async write still in the worker queue would hand it the
            // pre-save rules — quietly undoing the save that just reported success.
            CharacterDatabase.DirectCommitTransaction(tx);

            // A HENCHMAN never keeps a custom rotation (it's wiped on enroll), so an
            // EMPTY save must not leave it ruleless — that gutted its combat (Kevin:
            // "they fight much worse"). When the committed list is empty for a
            // henchman, restore its class DEFAULT into the cache instead of clearing.
            ObjectGuid const rotOg = ObjectGuid::Create<HighGuid::Player>(guid);
            if (rules.empty() && WowPsParty::IsHenchman(rotOg))
            {
                if (Player* h = ObjectAccessor::FindConnectedPlayer(rotOg))
                {
                    WowPsParty::RotationCacheSet(guid, WowPsParty::ParseRotationString(
                        WowPsParty::DefaultRotationForClass(h->getClass(), WowPsParty::RoleForGuid(rotOg),
                            WowPsParty::DominantTreeForGuid(guid))));
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cff66ccff[WowPsParty]|r Henchman rotation cleared — restored its class default.");
                    LOG_INFO("module",
                        "[WowPsParty] COMMIT_ROTATION henchman guid={} empty -> restored class default", guid);
                    return;
                }
            }

            WowPsParty::RotationCacheSet(guid, rules);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Saved {} rule(s).", uint32(rules.size()));
            LOG_INFO("module",
                "[WowPsParty] COMMIT_ROTATION target_guid={} n_rules={}",
                guid, uint32(rules.size()));
        }
        // ---------- COMMON shared rotation (account-wide, chunked like above) -------
        // Mirrors BEGIN/ROTATION_RULE/COMMIT but account-scoped (no target token): the
        // common pre-rotation that runs on EVERY party bot. Stored in
        // party_shared_rotation + the g_sharedRotation cache.
        else if (command == "BEGIN_SHARED_ROTATION")
        {
            PendingSharedRotationMap()[player->GetSession()->GetAccountId()].clear();
        }
        else if (command == "SHARED_ROTATION_RULE")
        {
            // payload = "<condition>\t<action>\t<priority>[\t<flags>]"
            std::string s(payload);
            std::vector<std::string> f;
            size_t p = 0;
            while (p <= s.size())
            {
                size_t t = s.find('\t', p);
                if (t == std::string::npos) { f.push_back(s.substr(p)); break; }
                f.push_back(s.substr(p, t - p));
                p = t + 1;
            }
            if (f.size() < 3) return;
            WowPsParty::RotationRule r;
            r.condition = f[0];
            r.action    = f[1];
            r.priority  = std::atoi(f[2].c_str());
            r.flags     = f.size() >= 4 ? f[3] : "";
            PendingSharedRotationMap()[player->GetSession()->GetAccountId()].push_back(std::move(r));
        }
        else if (command == "COMMIT_SHARED_ROTATION")
        {
            uint32 const account = player->GetSession()->GetAccountId();
            auto& pending = PendingSharedRotationMap();
            auto it = pending.find(account);
            std::vector<WowPsParty::RotationRule> rules;
            if (it != pending.end()) { rules = std::move(it->second); pending.erase(it); }
            std::stable_sort(rules.begin(), rules.end(),
                [](WowPsParty::RotationRule const& a, WowPsParty::RotationRule const& b)
                { return a.priority > b.priority; });
            std::string stored = WowPsParty::SerialiseRotationRules(rules);
            CharacterDatabase.EscapeString(stored);
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_shared_rotation` (`account`, `priority_actions_json`) VALUES ({}, '{}') "
                "ON DUPLICATE KEY UPDATE `priority_actions_json` = VALUES(`priority_actions_json`)",
                account, stored);
            CharacterDatabase.CommitTransaction(tx);
            WowPsParty::SharedRotationCacheSet(account, rules);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Saved {} common rule(s) — runs on the whole party.",
                uint32(rules.size()));
        }
        // REQ_SHARED_ROTATION  ->  SHARED_ROT_BEGIN / SHARED_ROT_CHUNK\t<frag> / SHARED_ROT_END
        // Chunked (the account-wide Common list can be large — a fully-scripted one
        // overflowed the ~255-byte addon-message cap and arrived EMPTY, so the editor
        // showed no Common rules at all). The helper converts the '|' field separator
        // to '~' for the editor's import parser, exactly as the single-message send did.
        else if (command == "REQ_SHARED_ROTATION")
        {
            uint32 const account = player->GetSession()->GetAccountId();
            QueryResult q = CharacterDatabase.Query(
                "SELECT `priority_actions_json` FROM `party_shared_rotation` WHERE `account` = {}", account);
            std::string dsl = q ? q->Fetch()[0].Get<std::string>() : std::string();
            SendChunkedRotation(player, "SHARED_ROT_BEGIN", "SHARED_ROT_CHUNK",
                                dsl, "SHARED_ROT_END");
        }
        // ---------- per-mob COMMON sections (account-wide, chunked like above) ------
        // A named bucket of Common rules per boss. Stored raw in party_mob_rotation +
        // the g_mobRotation cache; gated with target_name:<mob> only at eval time. The
        // RULE payload matches SHARED_ROTATION_RULE (no name) — the section name comes
        // from the preceding BEGIN, so one section commits per BEGIN/COMMIT pair.
        else if (command == "BEGIN_MOB_ROTATION")
        {
            std::string const name = WowPsParty::SanitizeMobName(std::string(payload));
            auto& sec = PendingMobRotationMap()[player->GetSession()->GetAccountId()];
            sec.name = name;
            sec.rules.clear();
        }
        else if (command == "MOB_ROTATION_RULE")
        {
            // payload = "<condition>\t<action>\t<priority>[\t<flags>]"
            std::string s(payload);
            std::vector<std::string> f;
            size_t p = 0;
            while (p <= s.size())
            {
                size_t t = s.find('\t', p);
                if (t == std::string::npos) { f.push_back(s.substr(p)); break; }
                f.push_back(s.substr(p, t - p));
                p = t + 1;
            }
            if (f.size() < 3) return;
            WowPsParty::RotationRule r;
            r.condition = f[0];
            r.action    = f[1];
            r.priority  = std::atoi(f[2].c_str());
            r.flags     = f.size() >= 4 ? f[3] : "";
            PendingMobRotationMap()[player->GetSession()->GetAccountId()].rules.push_back(std::move(r));
        }
        else if (command == "COMMIT_MOB_ROTATION")
        {
            uint32 const account = player->GetSession()->GetAccountId();
            std::string name = WowPsParty::SanitizeMobName(std::string(payload));
            auto& pending = PendingMobRotationMap();
            auto it = pending.find(account);
            std::vector<WowPsParty::RotationRule> rules;
            if (it != pending.end())
            {
                if (name.empty()) name = it->second.name;   // fall back to BEGIN's name
                rules = std::move(it->second.rules);
                pending.erase(it);
            }
            if (name.empty()) return;
            std::stable_sort(rules.begin(), rules.end(),
                [](WowPsParty::RotationRule const& a, WowPsParty::RotationRule const& b)
                { return a.priority > b.priority; });
            std::string escName = name;
            CharacterDatabase.EscapeString(escName);
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            if (rules.empty())
            {
                // An emptied section is dropped entirely (matches the Common tab, where an
                // empty commit clears the rotation) so it doesn't linger in the dropdown.
                tx->Append("DELETE FROM `party_mob_rotation` WHERE `account` = {} AND `mob_name` = '{}'",
                           account, escName);
            }
            else
            {
                std::string stored = WowPsParty::SerialiseRotationRules(rules);
                CharacterDatabase.EscapeString(stored);
                tx->Append(
                    "INSERT INTO `party_mob_rotation` (`account`, `mob_name`, `priority_actions_json`) "
                    "VALUES ({}, '{}', '{}') "
                    "ON DUPLICATE KEY UPDATE `priority_actions_json` = VALUES(`priority_actions_json`)",
                    account, escName, stored);
            }
            CharacterDatabase.CommitTransaction(tx);
            WowPsParty::SharedRotationCacheSetMob(account, name, rules);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Saved {} rule(s) for \"{}\" — runs on the whole party vs that mob.",
                uint32(rules.size()), name);
        }
        // DELETE_MOB_ROTATION\t<name>  — remove a whole section (dropdown entry + rules).
        else if (command == "DELETE_MOB_ROTATION")
        {
            uint32 const account = player->GetSession()->GetAccountId();
            std::string const name = WowPsParty::SanitizeMobName(std::string(payload));
            if (name.empty()) return;
            std::string escName = name;
            CharacterDatabase.EscapeString(escName);
            CharacterDatabase.Execute(
                "DELETE FROM `party_mob_rotation` WHERE `account` = {} AND `mob_name` = '{}'",
                account, escName);
            WowPsParty::DeleteMobRotation(account, name);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Removed the \"{}\" mob rules.", name);
        }
        // REQ_MOB_ROTATIONS  ->  MOB_LIST_BEGIN / MOB_LIST_ITEM\t<name>[\t<name>...] / MOB_LIST_END
        // Chunked (like the inventory stream): with 200 sections scripted, a single
        // tab-joined message would blow past the ~255-byte addon-message cap and drop, so
        // names are batched into lines kept well under it. BEGIN resets the client's list,
        // each ITEM appends, END rebuilds the dropdown.
        else if (command == "REQ_MOB_ROTATIONS")
        {
            uint32 const account = player->GetSession()->GetAccountId();
            // Read straight from the DB (authoritative) so a just-logged-in editor sees
            // sections even before any bot-login refresh populated the cache.
            QueryResult q = CharacterDatabase.Query(
                "SELECT `mob_name` FROM `party_mob_rotation` WHERE `account` = {} ORDER BY `mob_name`",
                account);
            SendWPSP(player, "MOB_LIST_BEGIN");
            std::string line = "MOB_LIST_ITEM";
            if (q)
            {
                do
                {
                    std::string const name = q->Fetch()[0].Get<std::string>();
                    if (line.size() + 1 + name.size() > 200 && line != "MOB_LIST_ITEM")
                    {
                        SendWPSP(player, line);
                        line = "MOB_LIST_ITEM";
                    }
                    line += "\t" + name;
                } while (q->NextRow());
            }
            if (line != "MOB_LIST_ITEM") SendWPSP(player, line);
            SendWPSP(player, "MOB_LIST_END");
        }
        // REQ_MOB_ROTATION\t<name>  ->  MOB_ROT_BEGIN / MOB_ROT_CHUNK / MOB_ROT_END
        // A fully-scripted boss's DSL can be long, so the reply is CHUNKED (unlike the
        // single-message REQ_SHARED_ROTATION): if a big section's DSL overflowed the
        // ~255-byte addon-message cap it would arrive EMPTY, and re-saving that section
        // would then clear its stored rules. Splitting the raw DSL into <=200-byte pieces
        // (the client concatenates them verbatim before parsing) removes that footgun.
        else if (command == "REQ_MOB_ROTATION")
        {
            uint32 const account = player->GetSession()->GetAccountId();
            std::string const name = WowPsParty::SanitizeMobName(std::string(payload));
            if (name.empty()) return;
            std::string escName = name;
            CharacterDatabase.EscapeString(escName);
            QueryResult q = CharacterDatabase.Query(
                "SELECT `priority_actions_json` FROM `party_mob_rotation` "
                "WHERE `account` = {} AND `mob_name` = '{}'", account, escName);
            std::string dsl = q ? q->Fetch()[0].Get<std::string>() : std::string();
            // DB stores '|'-delimited fields; the editor's parser wants '~' ('|' is a
            // WoW escape prefix). Same conversion REQ_SHARED_ROTATION does. The DSL uses
            // no tabs, so chunking on raw byte offsets is safe — the client just
            // reassembles the exact string. Name is on BEGIN/END only (not each chunk).
            std::replace(dsl.begin(), dsl.end(), '|', '~');
            SendWPSP(player, "MOB_ROT_BEGIN\t" + name);
            for (size_t i = 0; i < dsl.size(); i += 200)
                SendWPSP(player, "MOB_ROT_CHUNK\t" + dsl.substr(i, 200));
            SendWPSP(player, "MOB_ROT_END\t" + name);
        }
        else if (command == "SET_ROTATION")
        {
            // SET_ROTATION\t<slot>\t<dsl>
            // Bypasses the .party setrotation chat command so the DSL's `|`
            // delimiter doesn't collide with WoW chat colour codes (`|c`).
            auto tab = payload.find('\t');
            if (tab == std::string_view::npos) return;
            uint32 const slot = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
            std::string const dsl(payload.substr(tab + 1));
            if (slot >= WowPsParty::PARTY_SIZE) return;

            uint32 const accountId = player->GetSession()->GetAccountId();
            QueryResult q = CharacterDatabase.Query(
                "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}",
                accountId, slot);
            if (!q) return;
            uint32 const guid = q->Fetch()[0].Get<uint32>();

            auto rules = WowPsParty::ParseRotationString(dsl);
            std::string stored = WowPsParty::SerialiseRotationRules(rules);
            // Spell names carry apostrophes ("Avenger's Shield", "Hunter's
            // Mark") — escape before it goes into the '{}' literal or the ' ends
            // the string and the INSERT throws a 1064 syntax error.
            CharacterDatabase.EscapeString(stored);

            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`) VALUES ({}, '', '', '', '', '{}') "
                "ON DUPLICATE KEY UPDATE `priority_actions_json` = VALUES(`priority_actions_json`)",
                guid, stored);
            CharacterDatabase.DirectCommitTransaction(tx);   // sync — a later hire/invite re-reads this row

            WowPsParty::RotationCacheSet(guid, rules);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Saved {} rule(s) on slot {}.",
                uint32(rules.size()), slot);
        }
        else if (command == "MGMT_LIST")
        {
            SendMgmtList(player);
        }
        else if (command == "MGMT_MOVE")
        {
            // MGMT_MOVE\t<guid>\t<up|down> — hand-sort a character within the FULL
            // account roster (all characters, enrolled or not), so the whole list
            // can be organised. The order persists in characters.roster_order and
            // is rewritten densely on every move, so it auto-saves. This is purely a
            // display order — independent of the party slot (A–E), which still drives
            // formation/login via account_party.
            auto tab = payload.find('\t');
            if (tab == std::string_view::npos) return;
            uint32 const moveGuid = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
            std::string_view const dir = payload.substr(tab + 1);
            if (dir != "up" && dir != "down") return;
            if (!moveGuid) return;
            uint32 const accountId = player->GetSession()->GetAccountId();

            // Read the current full ordering. This ORDER BY MUST sort identically to
            // SendMgmtList's, or a swap would target a different neighbour than the
            // panel shows. The first move materialises the legacy enrolled-first /
            // name order into concrete indices.
            QueryResult q = CharacterDatabase.Query(
                "SELECT c.`guid` FROM `characters` c "
                "LEFT JOIN `account_party` ap ON ap.guid = c.guid AND ap.account = c.account "
                "WHERE c.`account` = {} "
                "AND (c.`deleteInfos_Account` IS NULL OR c.`deleteInfos_Account` = 0) "
                "ORDER BY (c.`roster_order` IS NULL) ASC, c.`roster_order` ASC, "
                "         COALESCE(ap.`slot`, 255) ASC, c.`name` ASC",
                accountId);
            if (!q) return;

            std::vector<uint32> order;
            do { order.push_back(q->Fetch()[0].Get<uint32>()); } while (q->NextRow());

            int idx = -1;
            for (size_t i = 0; i < order.size(); ++i)
                if (order[i] == moveGuid) { idx = int(i); break; }
            if (idx < 0) return;   // not this account's character

            int const swapWith = (dir == "up") ? idx - 1 : idx + 1;
            if (swapWith < 0 || swapWith >= int(order.size()))
                return;            // already at the top/bottom of the list
            std::swap(order[idx], order[swapWith]);

            // Persist a dense 0..N-1 order for the whole list (N is tiny — one
            // account's characters), so there are never gaps or NULLs to break ties.
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            for (size_t i = 0; i < order.size(); ++i)
                tx->Append("UPDATE `characters` SET `roster_order` = {} WHERE `guid` = {}",
                           uint16(i), order[i]);
            // SYNCHRONOUS commit: SendMgmtList below re-reads roster_order to render
            // the new order. An async CommitTransaction may not have landed yet, so
            // the panel would render the stale order (arrow click looks like a no-op).
            CharacterDatabase.DirectCommitTransaction(tx);

            // Push the reordered roster straight back so the panel updates instantly.
            SendMgmtList(player);
            LOG_INFO("module", "[WowPsParty] MGMT_MOVE account={} guid={} dir={} new_index={}",
                     accountId, moveGuid, std::string(dir), swapWith);
        }
        else if (command == "MGMT_ROLE")
        {
            // MGMT_ROLE\t<slot>\t<role>   role ∈ {tank, healer, dps}
            auto tab = payload.find('\t');
            if (tab == std::string_view::npos) return;
            uint32 const slot = std::strtoul(std::string(payload.substr(0, tab)).c_str(), nullptr, 10);
            std::string role(payload.substr(tab + 1));
            if (role != "tank" && role != "healer" && role != "dps") return;
            if (slot >= WowPsParty::PARTY_SIZE) return;

            uint32 const accountId = player->GetSession()->GetAccountId();
            // Resolve the guid at this slot so the role can also be mirrored into
            // party_loadout.role — that per-character copy survives a kick (which
            // deletes the account_party row), so a re-invited char keeps its role.
            QueryResult slotChar = CharacterDatabase.Query(
                "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}",
                accountId, slot);
            if (!slotChar) return;
            uint32 const slotGuid = slotChar->Fetch()[0].Get<uint32>();

            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "UPDATE `account_party` SET `role` = '{}' WHERE `account` = {} AND `slot` = {}",
                role, accountId, slot);
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`, `role`) "
                "VALUES ({}, '', '', '', '', '', '{}') "
                "ON DUPLICATE KEY UPDATE `role` = VALUES(`role`)",
                slotGuid, role);
            // SYNCHRONOUS commit: SetActiveFollowers below re-queries
            // account_party.role to rebuild the directives. An async commit isn't
            // visible to that synchronous read yet, so the directive (and thus
            // RoleForGuid / the LFG role check) kept the OLD role — a priest set
            // to Healer still answered the dungeon role check as DPS.
            CharacterDatabase.DirectCommitTransaction(tx);

            // Re-install follow directives so the new role takes effect this tick.
            WowPsParty::ClearFollowersForAccount(accountId);
            WowPsParty::SetActiveFollowers(accountId, player->GetGUID());

            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Slot {} role set to |cffffffff{}|r.", slot, role);
        }
        else if (command == "MGMT_MY_ROLE")
        {
            // MGMT_MY_ROLE\t<role>  — set the CONTROLLED character's OWN role with no
            // enrollment needed (solo mode). Stores to account_party.role if the char
            // happens to be enrolled, else to its per-character party_loadout.role,
            // then refreshes the leader-role cache so it takes effect immediately.
            std::string role(payload);
            if (role != "tank" && role != "healer" && role != "dps") return;
            uint32 const accountId = player->GetSession()->GetAccountId();
            uint32 const guidLow   = player->GetGUID().GetCounter();
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            if (sPartyMgr.GetSlotForGuid(guidLow))
                tx->Append("UPDATE `account_party` SET `role` = '{}' WHERE `guid` = {}", role, guidLow);
            else
                tx->Append(
                    "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                    "`gear_lock_json`, `priority_actions_json`, `role`) "
                    "VALUES ({}, '', '', '', '', '', '{}') "
                    "ON DUPLICATE KEY UPDATE `role` = VALUES(`role`)",
                    guidLow, role);
            CharacterDatabase.DirectCommitTransaction(tx);   // sync so the cache read below sees it
            WowPsParty::SetLeaderRoleForChar(accountId, player->GetGUID());
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Your role is now |cffffffff{}|r.", role);
        }
        else if (command == "MGMT_KICK")
        {
            // MGMT_KICK\t<slot>
            uint32 const slot = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            if (slot >= WowPsParty::PARTY_SIZE) return;
            uint32 const accountId = player->GetSession()->GetAccountId();

            QueryResult q = CharacterDatabase.Query(
                "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}",
                accountId, slot);
            if (!q)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Slot {} is already empty.", slot);
                return;
            }
            uint32 const kickedGuid = q->Fetch()[0].Get<uint32>();
            if (kickedGuid == player->GetGUID().GetCounter())
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff5555[WowPsParty]|r You can't kick the character you're playing — "
                    "log in as a different one first.");
                return;
            }

            // If the kicked char is currently spawned as a bot, remove it from
            // the group (else it lingers as an offline party member the user
            // has to uninvite by hand) and log it out.
            Player* botPlayer = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(kickedGuid));
            if (botPlayer && sPlayerbotsMgr.GetPlayerbotAI(botPlayer))
            {
                if (botPlayer->GetGroup())
                    botPlayer->RemoveFromGroup();
                if (PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(player))
                    mgr->LogoutPlayerBot(botPlayer->GetGUID());
            }

            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append("DELETE FROM `account_party` WHERE `account` = {} AND `slot` = {}",
                       accountId, slot);
            tx->Append("UPDATE `characters` SET `party_slot` = NULL WHERE `guid` = {}", kickedGuid);
            // SYNCHRONOUS commit: SetActiveFollowers below — and any MGMT_INVITE the
            // player fires straight after — re-query account_party. With an async
            // commit the deleted row could still be visible, so the follow-up invite
            // saw a full roster and failed with "Party is full (5/5)". Same race
            // Leave / MGMT_ROLE / MGMT_MOVE were already fixed for.
            CharacterDatabase.DirectCommitTransaction(tx);

            // Rebuild alt directives (kicked alt is gone from account_party so
            // it won't return). SetActiveFollowers preserves henchmen; do NOT
            // ClearFollowersForAccount first — that wipes henchmen too.
            WowPsParty::SetActiveFollowers(accountId, player->GetGUID());

            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Kicked slot {} from the party.", slot);
        }
        else if (command == "MGMT_INVITE")
        {
            // MGMT_INVITE\t<guid>
            uint32 const targetGuid = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            if (!targetGuid) return;
            uint32 const accountId = player->GetSession()->GetAccountId();

            // Resolve the name for the enroll API.
            QueryResult q = CharacterDatabase.Query(
                "SELECT `name`, `account` FROM `characters` WHERE `guid` = {}", targetGuid);
            if (!q)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Character not found.");
                return;
            }
            std::string const name = q->Fetch()[0].Get<std::string>();
            uint32 const charAccount = q->Fetch()[1].Get<uint32>();
            if (charAccount != accountId)
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff5555[WowPsParty]|r That character isn't on your account.");
                return;
            }

            auto result = sPartyMgr.Enroll(player, targetGuid, name);
            switch (result)
            {
                case WowPsParty::EnrollResult::Ok:
                {
                    // Immediate spawn if we have room and the active player
                    // is online.
                    // Install the follow directive BEFORE spawning the bot.
                    // The directive is what gates the bot out of mod-playerbots'
                    // default AI in PlayerbotAI::UpdateAI. If we spawn first, the
                    // bot can run a tick of default AI before the directive lands
                    // — that's how a freshly-invited warrior alt cast Shield Block
                    // (a default tank-strategy ability) that isn't in its rotation.
                    // SetActiveFollowers erases+rebuilds the !henchman directives
                    // while KEEPING henchmen (do NOT ClearFollowersForAccount —
                    // that wiped the player's henchmen).
                    WowPsParty::SetActiveFollowers(accountId, player->GetGUID());
                    if (PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(player))
                    {
                        mgr->AddPlayerBot(
                            ObjectGuid::Create<HighGuid::Player>(targetGuid), accountId);
                    }
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cff66ccff[WowPsParty]|r Invited |cffffffff{}|r to the party.", name);
                    break;
                }
                case WowPsParty::EnrollResult::AlreadyEnrolled:
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cffff5555[WowPsParty]|r |cffffffff{}|r is already in the party.", name);
                    break;
                case WowPsParty::EnrollResult::PartyFull:
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cffff5555[WowPsParty]|r Party is full (5/5). Kick someone first.");
                    break;
                default:
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cffff5555[WowPsParty]|r Couldn't invite |cffffffff{}|r.", name);
                    break;
            }
        }
        else if (command == "CLEAR_ROTATION")
        {
            std::string const token(payload);
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "UPDATE `party_loadout` SET `priority_actions_json` = '' WHERE `guid` = {}", guid);
            CharacterDatabase.DirectCommitTransaction(tx);   // sync — a later hire/invite re-reads this row

            // The editor sends CLEAR_ROTATION (not an empty COMMIT) whenever the
            // saved rule list is empty. A HENCHMAN must never be left ruleless —
            // that gutted its combat (Kevin: "they fight much worse") — so mirror
            // the empty-COMMIT path: restore its class default into the cache
            // instead of clearing it. Non-henchmen clear as before.
            ObjectGuid const rotOg = ObjectGuid::Create<HighGuid::Player>(guid);
            if (WowPsParty::IsHenchman(rotOg))
            {
                if (Player* h = ObjectAccessor::FindConnectedPlayer(rotOg))
                {
                    WowPsParty::RotationCacheSet(guid, WowPsParty::ParseRotationString(
                        WowPsParty::DefaultRotationForClass(h->getClass(), WowPsParty::RoleForGuid(rotOg),
                            WowPsParty::DominantTreeForGuid(guid))));
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cff66ccff[WowPsParty]|r Henchman rotation cleared — restored its class default.");
                    LOG_INFO("module",
                        "[WowPsParty] CLEAR_ROTATION henchman guid={} -> restored class default", guid);
                    return;
                }
            }

            WowPsParty::RotationCacheClear(guid);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Cleared rotation.");
        }
        else if (command == "PING")
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON,
                                         player->GetGUID(), player->GetGUID(),
                                         std::string("WPSP\tPONG"), /*chatTag=*/0);
            player->GetSession()->SendPacket(&data);
        }
        else if (command == "REQ_XMOG")
        {
            SendXmogCollectionTo(player);
        }
        else if (command == "XMOG_APPLY")
        {
            HandleXmogApply(player, payload);
        }
        else
        {
            if (WowPsParty::IsLogVerbose())
                LOG_INFO("module", "[WowPsParty] unhandled WPSP command='{}' from guid={}",
                         std::string(command), player->GetGUID().GetCounter());
        }

        // Swallow the message — don't broadcast / echo as chat.
        msg.clear();
        type = CHAT_MSG_SYSTEM;  // safe no-op type
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!WowPsParty::IsEnabled() || !player) return;
        // Defer the initial ROSTER push so the client's addon has a chance to
        // register its CHAT_MSG_ADDON handler. The addon's PLAYER_ENTERING_WORLD
        // also requests REQ_ROSTER ~3s after login as a belt-and-braces.
        ObjectGuid const guid = player->GetGUID();
        player->m_Events.AddEventAtOffset([guid]()
        {
            if (Player* alive = ObjectAccessor::FindConnectedPlayer(guid))
                WowPsParty::SendRosterTo(alive);
        }, std::chrono::seconds(4));
    }
};

void AddPartyAddonProtocolScripts()
{
    new PartyAddonProtocolScript();
    new PartyKeyStageWorldScript();   // hand the human the key to whatever they're standing at
}
