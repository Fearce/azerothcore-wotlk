#include "PrivateClassPolicy.h"
#include "D2xWire.h"

#include <iostream>
#include <stdexcept>

using namespace d2x::wire;

void Require(bool value)
{
    if (!value)
        throw std::runtime_error("private class regression");
}

uint32_t Map(uint32_t id)
{
    return id == 110000 ? 133 : id == 112000 ? 0 : id;
}

void Word(Bytes& data, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        data.push_back(uint8_t(value >> (i * 8)));
}

void Policy()
{
    using namespace PrivateClasses;
    for (bool enabled : {false, true})
        for (uint32_t owner : {0u, 17u, 18u})
            for (uint32_t preview : {0u, 2u, 3u})
                for (bool bot : {false, true})
                    Require(CanUse(enabled, owner, preview, 17, 2, bot, 1, true) ==
                        (enabled && owner == 17 && preview == 2 && !bot));
    Require(!CanUse(true, 17, 1, 17, 1, false, 1, true));
    Require(!CanUse(true, 17, 2, 17, 2, false, 1, false));
    for (uint8_t cls = 0; cls < 20; ++cls)
    {
        Require(VisibleClass(cls, true) == cls);
        Require(VisibleClass(cls, false) == (cls == 12 ? 9 : cls == 13 ? 8 : cls));
        Require(IsCustom(cls) == (cls == 12 || cls == 13));
    }
}

void AuraPackets()
{
    Bytes data{0, 4}; // packed target, slot
    Word(data, 110000);
    data.insert(data.end(), {0x28, 80, 1}); // self caster, timed
    Word(data, 5000);
    Word(data, 3000);
    data.push_back(7);
    Word(data, 112000);
    data.insert(data.end(), {0, 80, 1, 3, 11, 22}); // packed caster
    data.push_back(8);
    Word(data, 0); // existing removal
    Bytes original = data;
    Auras(data, Map);
    Require(data.size() == original.size() - 6);
    Cursor input(data);
    input.PackedGuid();
    Require(input.Byte() == 4 && input.Word() == 133);
    input.Skip(11);
    Require(input.Byte() == 7 && input.Word() == 0);
    Require(input.Byte() == 8 && input.Word() == 0);
    Require(input.pos == data.size());
    // Stock bytes must survive bit for bit, including mixed duration/caster flags.
    Word(original = Bytes{0, 1}, 133);
    original.insert(original.end(), {8, 80, 1});
    Bytes stock = original;
    Auras(stock, Map);
    Require(stock == original);
    stock.pop_back();
    bool refused = false;
    try { Auras(stock, Map); } catch (std::runtime_error const&) { refused = true; }
    Require(refused);
}

void PartyPackets()
{
    for (bool full : {false, true})
    {
        Bytes data;
        if (full)
            data.push_back(0);
        data.push_back(0); // packed player
        Word(data, 0x000c0e01); // status, player auras, pet guid/name, pet auras, vehicle
        data.insert(data.end(), {1, 0});
        Word(data, 1); Word(data, 0); // aura mask
        Word(data, 110000); data.push_back(1);
        Word(data, 0); Word(data, 0); // pet guid
        data.insert(data.end(), {'P', 'e', 't', 0});
        Word(data, 0); Word(data, 0x80000000); // last pet aura slot
        Word(data, 112000); data.push_back(1);
        Word(data, 110000); // vehicle id must NEVER be mistaken for spell
        Bytes original = data;
        PartyAuras(data, full, Map);
        Cursor input(data);
        input.Skip(full ? 16 : 15);
        Require(input.Word() == 133);
        input.Skip(21);
        Require(input.Word() == 0);
        input.Skip(1);
        Require(input.Word() == 110000);
        Require(data.size() == original.size());
        data.pop_back();
        bool refused = false;
        try { PartyAuras(data, full, Map); } catch (std::runtime_error const&) { refused = true; }
        Require(refused);
    }
}

int main()
{
    Policy();
    AuraPackets();
    PartyPackets();
    std::cout << "PASS: access matrix, class fallback, aura framing, sparse party/pet auras, truncation, untouched non-spell data\n";
}
