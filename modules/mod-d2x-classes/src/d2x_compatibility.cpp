#include "D2xClasses.h"
#include "D2xWire.h"

#include "DynamicObject.h"
#include "Log.h"
#include "Opcodes.h"
#include "Player.h"
#include "PrivateClassPolicy.h"
#include "ScriptMgr.h"
#include "UpdateFields.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <unordered_set>

namespace
{
    bool UsesStockAssets(Player const* viewer)
    {
        return viewer && !viewer->GetSession()->CanUsePrivateClassAssets();
    }

    uint32 StockSpell(uint32 id)
    {
        if (id < d2x::FIRST_CLASS_SPELL || id > d2x::LAST_CLASS_SPELL)
            return id;
        auto found = std::ranges::lower_bound(d2x::STAND_INS, id, {}, &d2x::StandIn::spell);
        return found != d2x::STAND_INS.end() && found->spell == id ? found->stock : 0;
    }

    bool NeedsInspection(uint16 opcode)
    {
        switch (opcode)
        {
            case SMSG_SPELL_START: case SMSG_SPELL_GO:
            case SMSG_SPELL_FAILURE: case SMSG_SPELL_FAILED_OTHER:
            case SMSG_SPELLLOGEXECUTE: case MSG_CHANNEL_START:
            case SMSG_SPELLNONMELEEDAMAGELOG: case SMSG_PERIODICAURALOG:
            case SMSG_SPELLHEALLOG: case SMSG_SPELLENERGIZELOG:
            case SMSG_SPELLDAMAGESHIELD: case SMSG_PROCRESIST:
            case SMSG_SPELLORDAMAGE_IMMUNE: case SMSG_SPELLLOGMISS:
            case SMSG_AURA_UPDATE: case SMSG_AURA_UPDATE_ALL:
            case SMSG_PARTY_MEMBER_STATS: case SMSG_PARTY_MEMBER_STATS_FULL:
            case SMSG_DISPEL_FAILED: case SMSG_SPELLDISPELLOG: case SMSG_SPELLSTEALLOG:
            case SMSG_FRIEND_STATUS:
                return true;
            default:
                return false;
        }
    }

    // The original packet belongs to a broadcast and may reach patched clients
    // too. Work exclusively on this recipient's copy; never scan arbitrary words.
    bool Rewrite(d2x::wire::Bytes& bytes, uint16 opcode)
    {
        using d2x::wire::Cursor;
        Cursor input(bytes);
        switch (opcode)
        {
            case SMSG_FRIEND_STATUS:
            {
                uint8 result = input.Byte();
                input.Skip(8);
                if (result == 6 || result == 7) // FRIEND_ADDED_ONLINE/OFFLINE
                    input.String();
                if (result == 2 || result == 6) // FRIEND_ONLINE/ADDED_ONLINE
                {
                    input.Skip(9); // status, area, level
                    size_t at = input.pos;
                    uint32 cls = input.Word();
                    input.Put(at, PrivateClasses::VisibleClass(uint8(cls), false));
                }
                return true;
            }
            case SMSG_SPELL_START:
            case SMSG_SPELL_GO:
                input.PackedGuid();
                input.PackedGuid();
                input.Skip(1);
                return input.Spell(StockSpell);
            case SMSG_SPELL_FAILURE:
            case SMSG_SPELL_FAILED_OTHER:
                input.PackedGuid();
                input.Skip(1);
                return input.Spell(StockSpell);
            case SMSG_SPELLLOGEXECUTE:
            case MSG_CHANNEL_START:
                input.PackedGuid();
                return input.Spell(StockSpell);
            case SMSG_SPELLNONMELEEDAMAGELOG:
            case SMSG_PERIODICAURALOG:
            case SMSG_SPELLHEALLOG:
            case SMSG_SPELLENERGIZELOG:
                input.PackedGuid();
                input.PackedGuid();
                return input.Spell(StockSpell);
            case SMSG_SPELLDAMAGESHIELD:
            case SMSG_PROCRESIST:
            case SMSG_SPELLORDAMAGE_IMMUNE:
                input.Skip(16); // These three use full, not packed, guids.
                return input.Spell(StockSpell);
            case SMSG_SPELLLOGMISS:
                return input.Spell(StockSpell);
            case SMSG_AURA_UPDATE:
            case SMSG_AURA_UPDATE_ALL:
                d2x::wire::Auras(bytes, StockSpell);
                return true;
            case SMSG_PARTY_MEMBER_STATS:
            case SMSG_PARTY_MEMBER_STATS_FULL:
                d2x::wire::PartyAuras(bytes, opcode == SMSG_PARTY_MEMBER_STATS_FULL, StockSpell);
                return true;
            case SMSG_DISPEL_FAILED:
            case SMSG_SPELLDISPELLOG:
            case SMSG_SPELLSTEALLOG:
            {
                if (opcode == SMSG_DISPEL_FAILED)
                    input.Skip(16);
                else
                {
                    input.PackedGuid();
                    input.PackedGuid();
                }
                if (!input.Spell(StockSpell))
                    return false;
                uint32 count;
                if (opcode == SMSG_DISPEL_FAILED)
                    count = uint32((bytes.size() - input.pos) / 4);
                else
                {
                    input.Skip(1);
                    count = input.Word();
                }
                for (uint32 i = 0; i < count; ++i)
                {
                    input.Spell(StockSpell);
                    if (opcode != SMSG_DISPEL_FAILED)
                        input.Skip(1);
                }
                return true;
            }
            default:
                return true;
        }
    }

    class d2x_stock_packets : public ServerScript
    {
    public:
        d2x_stock_packets() : ServerScript("d2x_stock_packets", {SERVERHOOK_ON_PACKET_SEND_REWRITE}) { }

        PacketRewrite OnPacketSendRewrite(WorldSession* session, WorldPacket const& packet, WorldPacket& replacement) override
        {
            if (session->CanUsePrivateClassAssets() || !NeedsInspection(packet.GetOpcode()))
                return PacketRewrite::Unchanged;
            d2x::wire::Bytes bytes(packet.contents(), packet.contents() + packet.size());
            try
            {
                if (!Rewrite(bytes, packet.GetOpcode()))
                    return PacketRewrite::Dropped; // server-only helper, no client visual
            }
            catch (std::exception const& error)
            {
                LOG_ERROR("module", "[WowPs PrivateMods] packet={} dropped reason={}", packet.GetOpcode(), error.what());
                return PacketRewrite::Dropped;
            }
            if (bytes.size() == packet.size() && std::equal(bytes.begin(), bytes.end(), packet.contents()))
                return PacketRewrite::Unchanged;
            replacement.Initialize(packet.GetOpcode(), bytes.size());
            replacement.append(bytes.data(), bytes.size());
            static thread_local std::unordered_set<uint16> logged;
            if (logged.insert(packet.GetOpcode()).second)
                LOG_INFO("module", "[WowPs PrivateMods] packet={} fallback=stock reason=ordinary-client", packet.GetOpcode());
            return PacketRewrite::Replaced;
        }
    };

    class d2x_stock_units : public UnitScript
    {
    public:
        d2x_stock_units() : UnitScript("d2x_stock_units", true,
            {UNITHOOK_SHOULD_TRACK_VALUES_UPDATE_POS_BY_INDEX, UNITHOOK_ON_PATCH_VALUES_UPDATE}) { }

        bool ShouldTrackValuesUpdatePosByIndex(Unit const*, uint8, uint16 index) override
        {
            return index == UNIT_FIELD_BYTES_0 || index == UNIT_CREATED_BY_SPELL || index == UNIT_CHANNEL_SPELL;
        }

        void OnPatchValuesUpdate(Unit const* unit, ByteBuffer& buffer, BuildValuesCachePosPointers& positions, Player* viewer) override
        {
            if (!UsesStockAssets(viewer))
                return;
            for (auto const& [index, offset] : positions.other)
            {
                uint32 value = unit->GetUInt32Value(index);
                if (index == UNIT_FIELD_BYTES_0)
                {
                    uint8 cls = uint8(value >> 8);
                    value = (value & ~0xff00u) | (uint32(viewer->GetSession()->GetVisibleClass(cls)) << 8);
                }
                else if (index == UNIT_CREATED_BY_SPELL || index == UNIT_CHANNEL_SPELL)
                    value = StockSpell(value);
                else
                    continue;
                buffer.put<uint32>(offset, value);
            }
            if (positions.UnitFieldFactionTemplatePos >= 0 && unit->GetFaction() == d2x::FACTION_TEMPLATE_CONFUSED)
                buffer.put<uint32>(positions.UnitFieldFactionTemplatePos, 14); // stock hostile monster
        }
    };

    class d2x_stock_dynamic_objects : public WorldObjectScript
    {
    public:
        d2x_stock_dynamic_objects() : WorldObjectScript("d2x_stock_dynamic_objects",
            {WORLDOBJECTHOOK_ON_DYNAMIC_OBJECT_VALUES_UPDATE_FIELD}) { }

        void OnDynamicObjectValuesUpdateField(DynamicObject const*, Player const* viewer, uint16 index, uint32& value) override
        {
            if (index == DYNAMICOBJECT_SPELLID && UsesStockAssets(viewer))
                value = StockSpell(value);
        }
    };
}

void AddSC_d2x_compatibility()
{
    new d2x_stock_packets();
    new d2x_stock_units();
    new d2x_stock_dynamic_objects();
    LOG_INFO("module", "[WowPs PrivateMods] compatibility=registered stock-spell-mappings={}", d2x::STAND_INS.size());
}
