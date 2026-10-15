#ifndef WOWPS_REALM_COUNT_ALIASES_H
#define WOWPS_REALM_COUNT_ALIASES_H

#include <charconv>
#include <cstdint>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

// Listing aliases share a count; they never create another realmcharacters row.
// Duplicating rows would inflate LOGIN_SEL_SUM_REALM_CHARACTERS account limits.
class RealmCountAliases
{
public:
    static RealmCountAliases Parse(std::string const& text)
    {
        RealmCountAliases aliases;
        std::istringstream input(text);
        for (std::string token; input >> token;)
        {
            auto const colon = token.find(':');
            if (colon == std::string::npos || token.find(':', colon + 1) != std::string::npos)
                throw std::invalid_argument("RealmCharacterCountAliases requires alias:source pairs separated by spaces");
            auto const alias = Number(std::string_view(token).substr(0, colon));
            auto const source = Number(std::string_view(token).substr(colon + 1));
            if (alias == source || !aliases._sources.emplace(alias, source).second)
                throw std::invalid_argument("RealmCharacterCountAliases requires distinct aliases and sources without duplicate aliases");
        }
        for (auto const& entry : aliases._sources)
            if (aliases._sources.count(entry.second))
                throw std::invalid_argument("RealmCharacterCountAliases must point directly to a primary realm, without chains or cycles");
        return aliases;
    }

    uint32_t Source(uint32_t realm) const
    {
        auto const found = _sources.find(realm);
        return found == _sources.end() ? realm : found->second;
    }

    std::size_t Size() const { return _sources.size(); }

private:
    static uint32_t Number(std::string_view text)
    {
        if (text.empty())
            throw std::invalid_argument("RealmCharacterCountAliases requires positive realm IDs");
        uint32_t value = 0;
        auto const result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !value || value > 255)
            throw std::invalid_argument("RealmCharacterCountAliases requires realm IDs from 1 through 255");
        return value;
    }

    std::map<uint32_t, uint32_t> _sources;
};

#endif
