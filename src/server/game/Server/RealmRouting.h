#ifndef WOWPS_REALM_ROUTING_H
#define WOWPS_REALM_ROUTING_H

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Every configured alias is another listing of this same world process.
namespace RealmRouting
{
    inline std::vector<uint32_t> RealmIds(uint32_t primary, std::string const& aliases)
    {
        std::vector<uint32_t> ids{primary};
        std::istringstream input(aliases);
        for (std::string token; input >> token;)
        {
            uint32_t id = 0;
            auto const parsed = std::from_chars(token.data(), token.data() + token.size(), id);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || !id || id > 255)
                throw std::invalid_argument("RealmAliases requires realm IDs from 1 through 255");
            if (std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
        }
        return ids;
    }
}

#endif
