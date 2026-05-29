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
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemTemplate.h"
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

    // Resolve the slot's character guid for the account.
    static uint32 GuidForAccountSlot(uint32 account, uint32 slot)
    {
        QueryResult q = CharacterDatabase.Query(
            "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}",
            account, slot);
        return q ? q->Fetch()[0].Get<uint32>() : 0;
    }

    static Player* ResolveControlledBody(Player* session)
    {
        if (!session) return nullptr;
        if (Unit* charm = session->GetCharm())
            if (charm->IsPlayer())
                return charm->ToPlayer();
        return session;  // not possessing anyone — your own body
    }

    // SPELLBOOK\t<slot>\t<spellId1,spellId2,...>
    void SendSpellbookTo(Player* requester, uint32 slot)
    {
        if (!requester || !requester->GetSession()) return;
        uint32 const account = requester->GetSession()->GetAccountId();
        uint32 const guid = GuidForAccountSlot(account, slot);
        if (!guid) return;
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
        out << "SPELLBOOK\t" << slot << '\t' << csv.str();
        SendWPSP(requester, out.str());
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

        std::ostringstream out;
        out << "INVENTORY\t";
        bool first = true;
        uint32 totalSlots = 0;   // party-wide bag capacity (for empty-cell count)

        // Separator helper for non-item trailing records (BAG/CAP/POOL).
        auto sep = [&]() { if (!first) out << ';'; first = false; };

        auto emit = [&](uint32 partySlot, uint32 bag, uint32 pos, Item* item)
        {
            sep();
            out << partySlot << ':' << bag << ':' << pos << ':'
                << item->GetEntry() << ':' << item->GetCount() << ':'
                << item->GetGUID().GetCounter();
        };

        for (uint8 partySlot = 0; partySlot < PARTY_SIZE; ++partySlot)
        {
            uint32 const guid = GuidForAccountSlot(account, partySlot);
            if (!guid) continue;
            ObjectGuid const og = ObjectGuid::Create<HighGuid::Player>(guid);
            Player* p = ObjectAccessor::FindConnectedPlayer(og);
            if (!p) continue;

            // Main backpack (16 slots): bag=255 (INVENTORY_SLOT_BAG_0), pos=23..38
            totalSlots += INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
            for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            {
                Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
                if (item)
                    emit(partySlot, INVENTORY_SLOT_BAG_0, i, item);
            }
            // The 4 equippable bag slots (19..22). Emit one BAG record per
            // slot — including empties (bagItemId 0) — so the addon can show
            // them in the bag strip and let the user equip a found bag. Then
            // emit the items inside any equipped bag.
            for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            {
                uint32 const bagIdx = b - INVENTORY_SLOT_BAG_START;  // 0..3
                Bag* bag = p->GetBagByPos(b);
                sep();
                out << "BAG:" << uint32(partySlot) << ':' << bagIdx << ':'
                    << (bag ? bag->GetEntry() : 0);
                if (!bag) continue;
                totalSlots += bag->GetBagSize();
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                {
                    Item* item = p->GetItemByPos(b, j);
                    if (item)
                        emit(partySlot, b, j, item);
                }
            }
        }
        // Total party bag capacity, so the addon can render empty cells up to
        // the real free space rather than a fixed minimum.
        sep();
        out << "CAP:" << totalSlots;
        // Shared gold pool. PartyHooks mirrors every money delta across the
        // party, so the requester's GetMoney() is the pool value. "POOL:" /
        // "CAP:" / "BAG:" all carry fewer than the 6 colons the item parser
        // needs, so the items loop ignores them and the dedicated parsers
        // pick them up.
        sep();
        out << "POOL:" << requester->GetMoney();
        SendWPSP(requester, out.str());
    }

    // TALENTS\t<slot>\t<freePoints>\t<classId>\t<rec>;<rec>;...
    //   rec = tabpage:talentId:row:col:maxRank:curRank:rank1SpellId:prereqTalentId:prereqRank
    //   The addon renders the three trees from this; the class name table for
    //   the tab titles lives client-side. Server enforces all spend rules in
    //   LearnTalent, so the client gating is purely cosmetic.
    void SendTalentsTo(Player* requester, uint32 slot)
    {
        if (!requester || !requester->GetSession()) return;
        uint32 const account = requester->GetSession()->GetAccountId();
        uint32 const guid = GuidForAccountSlot(account, slot);
        if (!guid) return;
        Player* p = ObjectAccessor::FindConnectedPlayer(
            ObjectGuid::Create<HighGuid::Player>(guid));
        if (!p) return;

        uint32 const classMask = p->getClassMask();
        std::ostringstream out;
        out << "TALENTS\t" << slot << '\t' << p->GetFreeTalentPoints()
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
            "[WowPsParty Talents] send slot={} {} freePoints={} class={}",
            slot, p->GetName(), p->GetFreeTalentPoints(), uint32(p->getClass()));
        SendWPSP(requester, out.str());
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
    if (secondTab == std::string_view::npos)
    {
        srcItemGuidLow = std::strtoul(std::string(rest).c_str(), nullptr, 10);
    }
    else
    {
        srcItemGuidLow = std::strtoul(std::string(rest.substr(0, secondTab)).c_str(), nullptr, 10);
        destSlot       = std::atoi(std::string(rest.substr(secondTab + 1)).c_str());
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

    // Validate dest can use the item. The Item* overload checks armor
    // proficiency (Cloth/Leather/Mail/Plate skill) via Item::GetSkill();
    // the ItemTemplate* overload doesn't, so a level-20 hunter would
    // "pass" the check on mail bracers and end up equipping them.
    InventoryResult const reason = dest->CanUseItem(srcItem, /*not_loading=*/true);
    if (reason != EQUIP_ERR_OK)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Can't equip on slot {}: target can't use this item (code {}).",
            uint32(srcSlot), uint32(reason));
        return;
    }

    // Find a suitable equip slot. swap=true so an item already in the slot
    // gets unequipped into a bag automatically (otherwise CanEquipItem
    // returns NOT_EQUIPPABLE when the slot is occupied -- user had to
    // manually unequip first, per Kevin's report).
    uint16 eqDest;
    InventoryResult result = dest->CanEquipItem(NULL_SLOT, eqDest, srcItem, /*swap=*/true, /*not_loading=*/true);
    if (result != EQUIP_ERR_OK)
    {
        ChatHandler(requester->GetSession()).PSendSysMessage(
            "|cffff5555[WowPsParty]|r Equip rejected ({}). Check requirements.", uint32(result));
        return;
    }

    if (srcChar == dest)
    {
        // SwapItem is the canonical equip-into-(possibly-occupied)-slot
        // path. It atomically unequips the current occupant back into the
        // source bag slot and equips the new item. The previous
        // RemoveItem + EquipItem sequence VANISHED items when the dest
        // slot was occupied (hunter replacing a bow with another bow):
        // RemoveItem detached the new bow from its bag slot, EquipItem
        // then refused to clobber the equipped bow, and the new item
        // leaked — still owned by the player in DB but gone from UI.
        uint16 const srcPos = srcItem->GetPos();
        LOG_INFO("module",
            "[WowPsParty Equip] same-char swap: char={} item={} ({}) srcPos={:#x} eqDest={:#x}",
            dest->GetName(), srcItem->GetEntry(), srcItem->GetTemplate()->Name1,
            srcPos, eqDest);
        dest->SwapItem(srcPos, eqDest);
    }
    else
    {
        // Cross-character transfer that PRESERVES the Item object (and thus
        // its enchants, gems, durability, charges, random properties, soulbind
        // state). MoveItemFromInventory detaches the Item from srcChar without
        // destroying it; we re-parent via SetOwnerGUID + persist via
        // SaveInventoryAndGoldToDB; MoveItemToInventory threads it into dest's
        // bag at the position CanStoreItem found.
        srcChar->MoveItemFromInventory(srcItem->GetBagSlot(), srcItem->GetSlot(), true);
        srcItem->SetOwnerGUID(dest->GetGUID());
        // Persist the ownership change immediately so a worldserver crash
        // doesn't leave the item half-orphaned (rows in characters_inventory
        // would otherwise still point at srcChar).
        CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
        srcItem->SaveToDB(tx);
        CharacterDatabase.CommitTransaction(tx);

        ItemPosCountVec destPos;
        if (dest->CanStoreItem(NULL_BAG, NULL_SLOT, destPos, srcItem, false) == EQUIP_ERR_OK)
        {
            dest->MoveItemToInventory(destPos, srcItem, true);
            uint16 dest2;
            // swap=true (same reason as the same-character branch above)
            if (dest->CanEquipItem(NULL_SLOT, dest2, srcItem, true, true) == EQUIP_ERR_OK)
            {
                // Use SwapItem here too: the new item is now sitting in
                // dest's bag, dest's old equipped item (if any) needs to
                // travel back into that bag slot atomically. Manual
                // RemoveItem + EquipItem had the same vanish-bug as the
                // same-char branch.
                uint16 const bagPos = srcItem->GetPos();
                LOG_INFO("module",
                    "[WowPsParty Equip] x-char swap: from={} to={} item={} ({}) bagPos={:#x} eqDest={:#x}",
                    srcChar->GetName(), dest->GetName(),
                    srcItem->GetEntry(), srcItem->GetTemplate()->Name1,
                    bagPos, dest2);
                dest->SwapItem(bagPos, dest2);
            }
        }
        else
        {
            // dest's bags are full — give it back to src so the item doesn't
            // vanish. (Edge case; the UI should have warned earlier.)
            srcItem->SetOwnerGUID(srcChar->GetGUID());
            ItemPosCountVec backPos;
            if (srcChar->CanStoreItem(NULL_BAG, NULL_SLOT, backPos, srcItem, false) == EQUIP_ERR_OK)
                srcChar->MoveItemToInventory(backPos, srcItem, true);
            CharacterDatabaseTransaction tx2 = CharacterDatabase.BeginTransaction();
            srcItem->SaveToDB(tx2);
            CharacterDatabase.CommitTransaction(tx2);
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

// GOTO_DELTA\t<dWorldX>\t<dWorldY>   — instant teleport. The addon
// computes a world-space (X, Y) delta from a right-click on the world map
// relative to the player's mapped position. We just `TeleportTo` the
// session player there; PartyFollow's catch-up teleport drags the bots
// onto the new map/coords within ~1 second on the next tick.
//
// The old "scout walks ahead, bots fight along the way" implementation
// was great for in-zone travel through hostile territory, but Kevin
// wants this to be a fast-travel tool for recording dungeon paths and
// hopping between zones — so instant TP is the right answer.
static void HandleGotoDelta(Player* requester, std::string_view payload)
{
    if (!requester || !requester->GetSession()) return;
    auto tab = payload.find('\t');
    if (tab == std::string_view::npos) return;
    float const dx = std::strtof(std::string(payload.substr(0, tab)).c_str(), nullptr);
    float const dy = std::strtof(std::string(payload.substr(tab + 1)).c_str(), nullptr);

    float const targetX = requester->GetPositionX() + dx;
    float const targetY = requester->GetPositionY() + dy;
    float targetZ = requester->GetMap()->GetHeight(
        requester->GetPhaseMask(), targetX, targetY, MAX_HEIGHT);
    if (targetZ <= INVALID_HEIGHT)
        targetZ = requester->GetPositionZ();
    requester->UpdateAllowedPositionZ(targetX, targetY, targetZ);

    LOG_INFO("module",
        "[WowPsParty] GOTO_DELTA from guid={} TELEPORT -> ({:.1f},{:.1f},{:.1f})",
        requester->GetGUID().GetCounter(), targetX, targetY, targetZ);

    // NearTeleportTo is the "warp instantly within the same map" path. It
    // bypasses the pre-checks in TeleportTo (combat, in-flight, recent
    // death, taxi, transport) that were silently dropping the request.
    requester->NearTeleportTo(targetX, targetY, targetZ,
                              requester->GetOrientation());

    // Drag the rest of the party with us. Previously the catch-up
    // teleport in PartyFollow handled this asynchronously, but it had
    // a one-tick lag, occasionally lost bots across map boundaries,
    // and felt janky when the user was zone-hopping. Teleport every
    // connected party member in this same frame; ring them out by 2.5y
    // so they don't pile on top of the requester (PartyFollow's next
    // tick redistributes them into formation anyway).
    uint32 const account = requester->GetSession()->GetAccountId();
    QueryResult qP = CharacterDatabase.Query(
        "SELECT `guid` FROM `account_party` WHERE `account` = {}", account);
    if (qP)
    {
        int formIdx = 0;
        do
        {
            uint32 const g = qP->Fetch()[0].Get<uint32>();
            if (g == requester->GetGUID().GetCounter()) continue;
            Player* m = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(g));
            if (!m) continue;

            float const angle = (2.0f * float(M_PI) / 4.0f) * float(formIdx);
            float const fx = targetX + std::cos(angle) * 2.5f;
            float const fy = targetY + std::sin(angle) * 2.5f;
            float fz = requester->GetMap()->GetHeight(
                m->GetPhaseMask(), fx, fy, MAX_HEIGHT);
            if (fz <= INVALID_HEIGHT) fz = targetZ;
            m->UpdateAllowedPositionZ(fx, fy, fz);

            if (m->GetMapId() == requester->GetMapId())
                m->NearTeleportTo(fx, fy, fz, requester->GetOrientation());
            else
                m->TeleportTo(requester->GetMapId(), fx, fy, fz,
                              requester->GetOrientation());

            LOG_INFO("module",
                "[WowPsParty] GOTO_DELTA follower {} -> ({:.1f},{:.1f},{:.1f})",
                m->GetName(), fx, fy, fz);
            ++formIdx;
        } while (qP->NextRow());
    }

    ChatHandler(requester->GetSession()).PSendSysMessage(
        "|cff66ccff[WowPsParty]|r Teleported party to ({:.0f}, {:.0f}).",
        targetX, targetY);
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
        else if (command == "REQ_SPELLBOOK")
        {
            uint32 slot = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            WowPsParty::SendSpellbookTo(player, slot);
        }
        else if (command == "REQ_GEAR")
        {
            uint32 slot = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            WowPsParty::SendGearTo(player, slot);
        }
        else if (command == "REQ_INVENTORY")
        {
            WowPsParty::SendInventoryTo(player);
        }
        // REQ_TARGETMODE\t<slot>  →  TARGETMODE\t<slot>\t<mode>
        else if (command == "REQ_TARGETMODE")
        {
            uint32 const slot = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            uint32 const account = player->GetSession()->GetAccountId();
            uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
            std::string mode = "master";
            if (guid)
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `strategies_csv` FROM `party_loadout` WHERE `guid` = {}", guid);
                if (q)
                {
                    std::string s = q->Fetch()[0].Get<std::string>();
                    if (!s.empty()) mode = s;
                }
            }
            std::ostringstream out;
            out << "TARGETMODE\t" << slot << '\t' << mode;
            SendWPSP(player, out.str());
        }
        // SET_TARGETMODE\t<slot>\t<mode>
        else if (command == "SET_TARGETMODE")
        {
            std::string s(payload);
            auto tab = s.find('\t');
            if (tab == std::string::npos) return;
            uint32 const slot = std::strtoul(s.substr(0, tab).c_str(), nullptr, 10);
            std::string mode = s.substr(tab + 1);
            if (slot >= WowPsParty::PARTY_SIZE) return;
            // Whitelist — only known modes reach the DB (also blocks any
            // injection via the stored-into-SQL string).
            if (mode != "master" && mode != "tank" && mode != "nearest"
                && mode != "loose")
                return;
            uint32 const account = player->GetSession()->GetAccountId();
            uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
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
                "|cff66ccff[WowPsParty]|r Target mode for slot {} set to '{}'.", slot, mode);
        }
        // REQ_LEADDUNGEON\t<slot>  →  LEADDUNGEON\t<slot>\t<0|1>
        else if (command == "REQ_LEADDUNGEON")
        {
            uint32 const slot = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            uint32 const account = player->GetSession()->GetAccountId();
            uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
            bool on = true;
            if (guid)
            {
                QueryResult q = CharacterDatabase.Query(
                    "SELECT `glyphs_csv` FROM `party_loadout` WHERE `guid` = {}", guid);
                if (q && q->Fetch()[0].Get<std::string>() == "0") on = false;
            }
            std::ostringstream out;
            out << "LEADDUNGEON\t" << slot << '\t' << (on ? 1 : 0);
            SendWPSP(player, out.str());
        }
        // SET_LEADDUNGEON\t<slot>\t<0|1>
        else if (command == "SET_LEADDUNGEON")
        {
            std::string s(payload);
            auto tab = s.find('\t');
            if (tab == std::string::npos) return;
            uint32 const slot = std::strtoul(s.substr(0, tab).c_str(), nullptr, 10);
            bool const on = (s.substr(tab + 1) != "0");
            if (slot >= WowPsParty::PARTY_SIZE) return;
            uint32 const account = player->GetSession()->GetAccountId();
            uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
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
                "|cff66ccff[WowPsParty]|r Slot {} lead-in-dungeons: {}.",
                slot, on ? "ON" : "OFF");
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
        // REQ_TALENTS\t<slot>  → TALENTS\t...
        else if (command == "REQ_TALENTS")
        {
            uint32 const slot = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            WowPsParty::SendTalentsTo(player, slot);
        }
        // LEARN_TALENT\t<slot>\t<talentId>\t<rank 0-based>
        else if (command == "LEARN_TALENT")
        {
            std::string s(payload);
            auto t1 = s.find('\t');
            if (t1 == std::string::npos) return;
            auto t2 = s.find('\t', t1 + 1);
            if (t2 == std::string::npos) return;
            uint32 const slot     = std::strtoul(s.substr(0, t1).c_str(), nullptr, 10);
            uint32 const talentId = std::strtoul(s.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
            uint32 const rank     = std::strtoul(s.substr(t2 + 1).c_str(), nullptr, 10);
            if (slot >= WowPsParty::PARTY_SIZE) return;
            uint32 const account = player->GetSession()->GetAccountId();
            uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
            if (!guid) return;
            Player* p = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(guid));
            if (!p) return;
            uint32 const freeBefore = p->GetFreeTalentPoints();
            p->LearnTalent(talentId, rank, false);  // false = normal spend rules
            LOG_INFO("module",
                "[WowPsParty Talents] LEARN slot={} {} talentId={} rank={} "
                "freeBefore={} freeAfter={}",
                slot, p->GetName(), talentId, rank, freeBefore,
                p->GetFreeTalentPoints());
            WowPsParty::SendTalentsTo(player, slot);
        }
        // RESET_TALENTS\t<slot>  — free, anywhere.
        else if (command == "RESET_TALENTS")
        {
            uint32 const slot = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            if (slot >= WowPsParty::PARTY_SIZE) return;
            uint32 const account = player->GetSession()->GetAccountId();
            uint32 const guid = WowPsParty::GuidForAccountSlot(account, slot);
            if (!guid) return;
            Player* p = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(guid));
            if (!p) return;
            p->resetTalents(true);   // noResetCost = true
            WowPsParty::SendTalentsTo(player, slot);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Reset talents for slot {} (free).", slot);
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
            uint32 const slot = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            if (slot >= WowPsParty::PARTY_SIZE) return;
            uint64 const key = (uint64(player->GetGUID().GetCounter()) << 8) | slot;
            PendingRotationMap()[key].clear();
            LOG_INFO("module",
                "[WowPsParty] BEGIN_ROTATION guid={} slot={}",
                player->GetGUID().GetCounter(), slot);
        }
        else if (command == "ROTATION_RULE")
        {
            // payload = "<slot>\t<condition>\t<action>\t<priority>[\t<flags>]"
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
            uint32 const slot = std::strtoul(fields[0].c_str(), nullptr, 10);
            if (slot >= WowPsParty::PARTY_SIZE) return;
            uint64 const key = (uint64(player->GetGUID().GetCounter()) << 8) | slot;
            WowPsParty::RotationRule r;
            r.condition = fields[1];
            r.action    = fields[2];
            r.priority  = std::atoi(fields[3].c_str());
            r.flags     = fields.size() >= 5 ? fields[4] : "";
            PendingRotationMap()[key].push_back(std::move(r));
            LOG_INFO("module",
                "[WowPsParty] ROTATION_RULE guid={} slot={} cond='{}' act='{}' prio={} flags='{}'",
                player->GetGUID().GetCounter(), slot,
                fields[1], fields[2], std::atoi(fields[3].c_str()),
                fields.size() >= 5 ? fields[4] : "");
        }
        else if (command == "COMMIT_ROTATION")
        {
            uint32 const slot = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            if (slot >= WowPsParty::PARTY_SIZE) return;
            uint64 const key = (uint64(player->GetGUID().GetCounter()) << 8) | slot;
            auto& pending = PendingRotationMap();
            auto it = pending.find(key);
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

            uint32 const accountId = player->GetSession()->GetAccountId();
            QueryResult q = CharacterDatabase.Query(
                "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}",
                accountId, slot);
            if (!q) return;
            uint32 const guid = q->Fetch()[0].Get<uint32>();

            std::string const stored = WowPsParty::SerialiseRotationRules(rules);
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
            LOG_INFO("module",
                "[WowPsParty] COMMIT_ROTATION guid={} slot={} n_rules={}",
                player->GetGUID().GetCounter(), slot, uint32(rules.size()));
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
            std::string const stored = WowPsParty::SerialiseRotationRules(rules);

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
            CharacterDatabase.CommitTransaction(tx);

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

            // If the kicked char is currently spawned as a bot, log it out.
            Player* botPlayer = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid::Create<HighGuid::Player>(kickedGuid));
            if (botPlayer && sPlayerbotsMgr.GetPlayerbotAI(botPlayer))
            {
                if (PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(player))
                    mgr->LogoutPlayerBot(botPlayer->GetGUID());
            }

            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append("DELETE FROM `account_party` WHERE `account` = {} AND `slot` = {}",
                       accountId, slot);
            tx->Append("UPDATE `characters` SET `party_slot` = NULL WHERE `guid` = {}", kickedGuid);
            CharacterDatabase.CommitTransaction(tx);

            WowPsParty::ClearFollowersForAccount(accountId);
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
                    if (PlayerbotMgr* mgr = sPlayerbotsMgr.GetPlayerbotMgr(player))
                    {
                        mgr->AddPlayerBot(
                            ObjectGuid::Create<HighGuid::Player>(targetGuid), accountId);
                    }
                    WowPsParty::ClearFollowersForAccount(accountId);
                    WowPsParty::SetActiveFollowers(accountId, player->GetGUID());
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
            uint32 const slot = std::strtoul(std::string(payload).c_str(), nullptr, 10);
            if (slot >= WowPsParty::PARTY_SIZE) return;
            uint32 const accountId = player->GetSession()->GetAccountId();
            QueryResult q = CharacterDatabase.Query(
                "SELECT `guid` FROM `account_party` WHERE `account` = {} AND `slot` = {}",
                accountId, slot);
            if (!q) return;
            uint32 const guid = q->Fetch()[0].Get<uint32>();
            CharacterDatabaseTransaction tx = CharacterDatabase.BeginTransaction();
            tx->Append(
                "UPDATE `party_loadout` SET `priority_actions_json` = '' WHERE `guid` = {}", guid);
            CharacterDatabase.CommitTransaction(tx);
            WowPsParty::RotationCacheClear(guid);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff66ccff[WowPsParty]|r Cleared rotation for slot {}.", slot);
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
