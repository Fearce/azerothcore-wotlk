#ifndef D2X_WIRE_H
#define D2X_WIRE_H

#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace d2x::wire
{
    using Bytes = std::vector<uint8_t>;

    class Cursor
    {
    public:
        explicit Cursor(Bytes& bytes) : data(bytes) { }
        void Skip(size_t count)
        {
            if (count > data.size() - pos)
                throw std::runtime_error("truncated private-mod packet");
            pos += count;
        }
        uint8_t Byte() { Skip(1); return data[pos - 1]; }
        uint32_t Word()
        {
            uint32_t value = 0;
            for (unsigned i = 0; i < 4; ++i)
                value |= uint32_t(Byte()) << (i * 8);
            return value;
        }
        uint64_t Wide()
        {
            uint64_t low = Word();
            return low | (uint64_t(Word()) << 32);
        }
        void PackedGuid() { Skip(std::popcount(Byte())); }
        void String() { while (Byte()) { } }
        void Put(size_t at, uint32_t value)
        {
            if (at > data.size() || data.size() - at < 4)
                throw std::runtime_error("invalid private-mod packet offset");
            for (unsigned i = 0; i < 4; ++i)
                data[at + i] = uint8_t(value >> (i * 8));
        }
        template<class Mapper> bool Spell(Mapper map)
        {
            size_t at = pos;
            uint32_t original = Word();
            uint32_t visible = map(original);
            Put(at, visible);
            return original == 0 || visible != 0;
        }
        Bytes& data;
        size_t pos = 0;
    };

    inline void Append(Bytes& output, Bytes const& input, size_t start, size_t end)
    {
        output.insert(output.end(), input.begin() + start, input.begin() + end);
    }

    // Aura updates are variable length. Hidden helpers become a slot removal,
    // with their entire original payload consumed, never a zero id plus junk.
    template<class Mapper> void Auras(Bytes& bytes, Mapper map)
    {
        Cursor input(bytes);
        input.PackedGuid();
        Bytes output;
        Append(output, bytes, 0, input.pos);
        while (input.pos < bytes.size())
        {
            size_t start = input.pos;
            input.Byte(); // slot
            size_t idAt = input.pos;
            uint32_t id = input.Word();
            if (id)
            {
                uint8_t flags = input.Byte();
                input.Skip(2); // level, stacks/charges
                if (!(flags & 0x08))
                    input.PackedGuid();
                if (flags & 0x20)
                    input.Skip(8); // maximum and remaining duration
            }
            uint32_t visible = map(id);
            input.Put(idAt, visible);
            Append(output, bytes, start, id && !visible ? start + 5 : input.pos);
        }
        bytes = std::move(output);
    }

    template<class Mapper> void RaidAuraBlock(Cursor& input, Mapper map)
    {
        uint64_t mask = input.Wide();
        for (unsigned i = 0; i < 64; ++i)
            if (mask & (uint64_t(1) << i))
            {
                input.Spell(map);
                input.Skip(1);
            }
    }

    template<class Mapper> void PartyAuras(Bytes& bytes, bool full, Mapper map)
    {
        Cursor input(bytes);
        if (full)
            input.Skip(1);
        input.PackedGuid();
        uint32_t mask = input.Word();
        constexpr unsigned widths[] = {2, 4, 4, 1, 2, 2, 2, 2, 4};
        for (unsigned i = 0; i < 9; ++i)
            if (mask & (1u << i))
                input.Skip(widths[i]);
        if (mask & 0x200)
            RaidAuraBlock(input, map);
        if (mask & 0x400)
            input.Skip(8);
        if (mask & 0x800)
            input.String();
        constexpr unsigned petWidths[] = {2, 4, 4, 1, 2, 2};
        for (unsigned i = 0; i < 6; ++i)
            if (mask & (0x1000u << i))
                input.Skip(petWidths[i]);
        if (mask & 0x40000)
            RaidAuraBlock(input, map);
        if (mask & 0x80000)
            input.Skip(4);
        if (input.pos != bytes.size())
            throw std::runtime_error("unexpected private-mod party packet tail");
    }
}

#endif
