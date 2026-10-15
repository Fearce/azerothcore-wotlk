#ifndef WOWPS_REALM_ACCESS_POLICY_H
#define WOWPS_REALM_ACCESS_POLICY_H

#include <charconv>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace RealmAccess
{
    class Policy
    {
    public:
        static Policy Parse(std::string_view text)
        {
            Policy policy;
            text = Trim(text);
            if (text.empty())
                return policy;

            for (std::size_t start = 0; start <= text.size();)
            {
                auto end = text.find(';', start);
                if (end == std::string_view::npos)
                    end = text.size();
                auto const entry = text.substr(start, end - start);
                auto const colon = entry.find(':');
                if (colon == std::string_view::npos || entry.find(':', colon + 1) != std::string_view::npos)
                    throw std::invalid_argument("RealmAccess requires realm:accounts entries");

                auto const realm = Number(Trim(entry.substr(0, colon)));
                if (realm > 255 || policy._accounts.count(realm))
                    throw std::invalid_argument("RealmAccess requires unique realm IDs from 1 through 255");

                std::set<uint32_t> accounts;
                std::istringstream input{std::string(entry.substr(colon + 1))};
                for (std::string account; input >> account;)
                    accounts.insert(Number(account));
                if (accounts.empty())
                    throw std::invalid_argument("RealmAccess requires at least one positive account ID per realm");
                policy._accounts.emplace(realm, std::move(accounts));
                if (end == text.size())
                    break;
                start = end + 1;
            }
            return policy;
        }

        bool Allows(uint32_t realm, uint32_t account) const
        {
            auto const rule = _accounts.find(realm);
            return rule == _accounts.end() || rule->second.count(account) != 0;
        }

        std::size_t RestrictedRealms() const { return _accounts.size(); }

    private:
        static std::string_view Trim(std::string_view text)
        {
            auto const first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return {};
            return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        }

        static uint32_t Number(std::string_view token)
        {
            if (token.empty())
                throw std::invalid_argument("RealmAccess requires a numeric ID");
            uint32_t value = 0;
            auto const parsed = std::from_chars(token.data(), token.data() + token.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || !value)
                throw std::invalid_argument("RealmAccess requires positive unsigned numeric IDs");
            return value;
        }

        std::map<uint32_t, std::set<uint32_t>> _accounts;
    };
}

#endif
