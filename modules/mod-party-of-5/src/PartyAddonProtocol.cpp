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
#include "Pet.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "AiObjectContext.h"
#include "Value.h"
#include "AuctionHouseMgr.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "LootMgr.h"   // Loot / LootItem — open lootable satchels from the shared bags
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
#include <cstdlib>
#include <sstream>
#include <vector>

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
        std::string const payload = BuildRosterPayload(player->GetSession()->GetAccountId());
        SendWPSP(player, "ROSTER\t" + payload);
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

    // HENCHMEN\t<rec>;<rec>;...   rec = guid:name:cls:level:role:cost
    // The hireable random-pool candidates near the player's level.
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
                << HenchmanHireCost(c.level);
        }
        std::string const body = out.str();
        LOG_INFO("module",
            "[WowPsParty Henchmen] SendHenchmenTo guid={} level={} team={} "
            "candidates={} payload_bytes={}",
            player->GetGUID().GetCounter(), uint32(player->GetLevel()),
            uint32(player->GetTeamId()), uint32(cands.size()), uint32(body.size()));
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
    void SendInventoryTo(Player* requester);
    void SendQuestProgressTo(Player* requester);

    // Push spellbook + gear + inventory for the active body on login. The
    // spellbook feeds the rotation editor's spell picker.
    void PushControlledLoadoutTo(Player* requester, int slot)
    {
        if (!requester || slot < 0) return;
        SendSpellbookTo(requester, uint32(slot));
        SendGearTo(requester, uint32(slot));
        SendInventoryTo(requester);
    }

    // GEAR\t<slot>\t<eqSlot>:<itemId>:<itemGuidLow>;...   (19 equipment slots)
    void SendGearTo(Player* requester, uint32 slot)
    {
        if (!requester || !requester->GetSession()) return;
        uint32 const account = requester->GetSession()->GetAccountId();
        uint32 const guid = GuidForAccountSlot(account, slot);
        if (!guid) return;
        ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
        Player* target = ObjectAccessor::FindConnectedPlayer(og);
        if (!target) return;

        std::ostringstream out;
        out << "GEAR\t" << slot << '\t';
        bool first = true;
        for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
        {
            Item* item = target->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
            if (!item) continue;
            if (!first) out << ';';
            first = false;
            out << uint32(i) << ':' << item->GetEntry() << ':' << item->GetGUID().GetCounter();
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
            std::ostringstream r;
            r << partySlot << ':' << bag << ':' << pos << ':'
              << item->GetEntry() << ':' << item->GetCount() << ':'
              << item->GetGUID().GetCounter()
              // Random property / suffix so the addon tooltip renders the FULL item
              // (e.g. a rare with "of the Bear") instead of the base item with no
              // stats. RandomPropertyId is NEGATIVE for a random SUFFIX; the suffix
              // factor (property seed) scales its stats. Appended, so an older addon
              // that only reads the first 6 fields still parses fine.
              << ':' << item->GetItemRandomPropertyId()
              << ':' << item->GetItemSuffixFactor();
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

    Item* srcItem = srcChar->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!srcItem) return;

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
    }

    // Refresh client UI on both sides
    WowPsParty::SendInventoryTo(requester);
    if (auto srcS = sPartyMgr.GetSlotForGuid(srcCharGuid))
        WowPsParty::SendGearTo(requester, uint32(*srcS));
    if (auto dstS = sPartyMgr.GetSlotForGuid(dest->GetGUID().GetCounter()))
        WowPsParty::SendGearTo(requester, uint32(*dstS));
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

    Item* srcItem = srcChar->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
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

    srcChar->DestroyItem(srcItem->GetBagSlot(), srcItem->GetSlot(), true);
    requester->ModifyMoney(int32(money));

    ChatHandler(requester->GetSession()).PSendSysMessage(
        "|cff66ccff[WowPsParty]|r Sold |cffffffff{}|r for |cffffd100{}.{}.{}|r.",
        soldName,
        money / 10000, (money / 100) % 100, money % 100);

    WowPsParty::SendInventoryTo(requester);
}

// AH_SELL\t<srcPartySlot>\t<srcItemGuidLow>\t<copperBuyout>
//   Lists an item out of any party member's bag on the owner's faction Auction
//   House — no auctioneer required (same auctioneer-less posting the AH bot uses).
//   24h listing, start bid == buyout == the requested copper price. The deposit
//   is charged to the item's owner; the shared-gold hook (OnPlayerMoneyChanged)
//   mirrors that delta across the pool. When the auction sells (or expires) the
//   AH mails proceeds / the item back to the owner char by the normal settlement
//   path, and collecting that mail re-mirrors the gold. Item must be tradeable
//   (not soulbound), unequipped and an empty bag — the same gates the real AH
//   applies, surfaced as chat errors so the click isn't silently dropped.
static void HandleAhSell(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;

    auto t1 = payload.find('\t');
    if (t1 == std::string_view::npos) return;
    auto t2 = payload.find('\t', t1 + 1);
    if (t2 == std::string_view::npos) return;

    uint32 const srcSlot = std::strtoul(std::string(payload.substr(0, t1)).c_str(), nullptr, 10);
    uint32 const srcItemGuidLow = std::strtoul(std::string(payload.substr(t1 + 1, t2 - t1 - 1)).c_str(), nullptr, 10);
    uint32 const copper = std::strtoul(std::string(payload.substr(t2 + 1)).c_str(), nullptr, 10);
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

    Item* srcItem = srcChar->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
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

    Item* srcItem = srcChar->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
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

// USE\t<srcPartySlot>\t<srcItemGuidLow> — use a bag consumable. The item's
// ON_USE spell fires on the requesting (controlled) character; a consumable
// loses one charge from its owner. Lets you right-click food/potions/quest
// items in the shared bag like a normal bag.
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

    Item* srcItem = srcChar->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!srcItem) return;
    ItemTemplate const* t = srcItem->GetTemplate();
    if (!t) return;

    uint32 useSpell = 0;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        if (t->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE && t->Spells[i].SpellId > 0)
        {
            useSpell = uint32(t->Spells[i].SpellId);
            break;
        }
    if (!useSpell)
    {
        // No on-use spell — but a LOOTABLE container (Satchel of Helpful Goods, the
        // random-dungeon reward bag, lootable pouches, …) is OPENED, not "used":
        // right-click pops its loot. These are BoP/unique and BOUND to the char that
        // earned them, so they must be opened ON THAT CHAR — never moved to the
        // requester (that fails the unique/bind check → the bogus "no room").
        if (t->HasFlag(ITEM_FLAG_HAS_LOOT) && !srcItem->IsWrapped())
        {
            if (!srcChar->IsAlive())
            {
                ChatHandler(requester->GetSession()).PSendSysMessage(
                    "|cffff5555[WowPsParty]|r Can't open |cffffffff{}|r — its owner is dead.", t->Name1);
                return;
            }
            // ONLY gate on the lock when the item actually HAS a lock (LockID). A
            // lock-less satchel still reports IsLocked() (the UNLOCKED bit is unset)
            // but opens fine — the engine only checks the lock if LockID != 0, so the
            // old unconditional IsLocked() check produced the bogus "is locked".
            if (t->LockID && srcItem->IsLocked())
            {
                ChatHandler(requester->GetSession()).PSendSysMessage(
                    "|cffff5555[WowPsParty]|r |cffffffff{}|r is locked.", t->Name1);
                return;
            }

            // The owning char opens it. SendLoot fills the item's loot from its
            // template (engine does the heavy lifting). If the owner is the human
            // (active char), that's all we do — the loot window pops and they loot
            // it normally. If the owner is an ALT/bot (no client to click a window),
            // auto-loot the contents into ITS bags so the loot isn't stranded; it
            // surfaces in the shared inventory either way.
            srcChar->SendLoot(srcItem->GetGUID(), LOOT_CORPSE);
            if (srcChar == requester)
            {
                WowPsParty::SendInventoryTo(requester);
                return;
            }

            Loot& loot = srcItem->loot;
            if (loot.gold)
            {
                srcChar->ModifyMoney(int32(loot.gold));   // shared-gold mirror runs off the money hook
                loot.gold = 0;
            }
            for (LootItem& li : loot.items)
            {
                if (li.is_looted || li.freeforall) continue;
                ItemPosCountVec dst;
                if (srcChar->CanStoreNewItem(NULL_BAG, NULL_SLOT, dst, li.itemid, li.count) != EQUIP_ERR_OK)
                    continue;   // owner bag full for this one — leave it in the satchel
                if (srcChar->StoreNewItem(dst, li.itemid, true, li.randomPropertyId))
                    li.is_looted = true;
            }
            srcChar->SendLootRelease(srcItem->GetGUID());   // clear the owner's loot state
            bool emptied = (loot.gold == 0);
            for (LootItem const& li : loot.items)
                if (!li.is_looted) { emptied = false; break; }
            if (emptied)
                srcChar->DestroyItem(srcItem->GetBagSlot(), srcItem->GetSlot(), true);
            std::string const ownerName = srcChar->GetName();
            std::string const satchelName = t->Name1;
            ChatHandler(requester->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Opened |cffffffff{}|r on {}.", satchelName, ownerName);
            WowPsParty::SendInventoryTo(requester);
            return;
        }
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r |cffffffff{}|r isn't usable.", t->Name1);
        return;
    }

    requester->CastSpell(requester, useSpell, true);
    if (t->Class == ITEM_CLASS_CONSUMABLE)
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

    Item* srcItem = srcChar->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
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

    Item* item = srcChar->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(srcItemGuidLow));
    if (!item) return;

    if (srcChar == dstChar)
    {
        // Same-char rearrange: find best free slot in own bags.
        ItemPosCountVec pos;
        if (srcChar->CanStoreItem(NULL_BAG, NULL_SLOT, pos, item, true /*swap*/) != EQUIP_ERR_OK)
            return;
        srcChar->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
        srcChar->StoreItem(pos, item, true);
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
                srcChar->MoveItemToInventory(backPos, item, true);
            CharacterDatabaseTransaction tx2 = CharacterDatabase.BeginTransaction();
            item->SaveToDB(tx2);
            CharacterDatabase.CommitTransaction(tx2);
        }
    }

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
        // REQ_GENROT\t<token>  →  GENROT\t<token>\t<dsl>
        // "Generate rotation" button: hand back the class-default rotation for
        // the member so the editor can populate an empty rotation the user can
        // then tweak + Save & Apply. Reuses DefaultRotationForClass (the same
        // default the bot already runs on hire). Converts the '|' field sep to
        // '~' for the editor's import parser (which avoids '|', a WoW escape).
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
            std::string dsl = WowPsParty::DefaultRotationForClass(cls, genRole);
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
                && mode != "loose" && mode != "lowest")
                return;
            uint32 const guid = WowPsParty::ResolveLoadoutToken(player, token);
            if (!guid) return;
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "INSERT INTO `party_loadout` (`guid`, `strategies_csv`, `talents_hex`, `glyphs_csv`, "
                "`gear_lock_json`, `priority_actions_json`) VALUES ({}, '{}', '', '', '', '') "
                "ON DUPLICATE KEY UPDATE `strategies_csv` = VALUES(`strategies_csv`)",
                guid, mode);
            CharacterDatabase.CommitTransaction(tx);
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
        // REQ_ROTATION\t<token>  →  ROTATION\t<token>\t<dsl>
        // The editor pulls the SAVED rotation from party_loadout (guid-keyed,
        // authoritative) so a reshuffled party slot never shows the previous
        // occupant's rotation out of the slot-keyed client cache. '|' field sep
        // → '~' for the editor's import parser (which avoids '|', a WoW escape).
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
            std::replace(dsl.begin(), dsl.end(), '|', '~');
            std::ostringstream out;
            out << "ROTATION\t" << token << '\t' << dsl;
            SendWPSP(player, out.str());
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
            CharacterDatabase.CommitTransaction(tx);
            WowPsParty::LeadDungeonCacheSet(guid, on);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Lead-in-dungeons: {}.", on ? "ON" : "OFF");
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
        else if (command == "SELL")
        {
            HandleSell(player, payload);
        }
        else if (command == "AH_SELL")
        {
            HandleAhSell(player, payload);
        }
        else if (command == "PULL_REAGENT")
        {
            HandlePullReagent(player, payload);
        }
        else if (command == "DESTROY")
        {
            HandleDestroy(player, payload);
        }
        else if (command == "USE")
        {
            HandleUse(player, payload);
        }
        else if (command == "SPLIT")
        {
            HandleSplit(player, payload);
        }
        else if (command == "SORT_BAGS")
        {
            HandleSortBags(player);
        }
        else if (command == "SELL_TRASH")
        {
            HandleSellTrash(player);
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
                uint32 const mapId = player->GetMapId();
                WowPsParty::ClearPath(mapId);
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff66ccff[WowPsParty]|r Cleared dungeon path for map {}.", mapId);
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
        // individual message tiny.
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
            CharacterDatabase.CommitTransaction(tx);

            WowPsParty::RotationCacheSet(guid, rules);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Saved {} rule(s).", uint32(rules.size()));
            LOG_INFO("module",
                "[WowPsParty] COMMIT_ROTATION target_guid={} n_rules={}",
                guid, uint32(rules.size()));
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
            CharacterDatabase.CommitTransaction(tx);

            WowPsParty::RotationCacheSet(guid, rules);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Saved {} rule(s) on slot {}.",
                uint32(rules.size()), slot);
        }
        else if (command == "MGMT_LIST")
        {
            // Reply with one record per character on the account:
            //   <guid>:<name>:<race>:<class>:<level>:<slot|-1>:<role>;...
            uint32 const accountId = player->GetSession()->GetAccountId();
            QueryResult q = CharacterDatabase.Query(
                "SELECT c.guid, c.name, c.race, c.class, c.level, "
                "       COALESCE(ap.slot, 255) AS slot, "
                "       COALESCE(ap.role, 'dps') AS role "
                "FROM `characters` c "
                "LEFT JOIN `account_party` ap ON ap.guid = c.guid AND ap.account = c.account "
                "WHERE c.account = {} AND (c.deleteInfos_Account IS NULL OR c.deleteInfos_Account = 0) "
                "ORDER BY COALESCE(ap.slot, 255), c.name",
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
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "UPDATE `account_party` SET `role` = '{}' WHERE `account` = {} AND `slot` = {}",
                role, accountId, slot);
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
            CharacterDatabase.CommitTransaction(tx);

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
            CharacterDatabase.CommitTransaction(tx);
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
}
