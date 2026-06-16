#pragma once

#include <optional>
#include <string>
#include <string_view>

// Parse a single parameter from an application/x-www-form-urlencoded body.
// Body format: "key1=value1&key2=value2"
// Returns the raw (non-URL-decoded) value for the given key, or nullopt if
// the key is absent. Values are bounded by '&' or end-of-string.
inline std::optional<std::string> get_param(std::string_view body,
                                            std::string_view key) {
    // Build the search prefix "key="
    std::string prefix;
    prefix.reserve(key.size() + 1);
    prefix.append(key);
    prefix += '=';

    std::size_t pos = 0;
    while (pos < body.size()) {
        // Find the next '&' to bound the current token
        std::size_t amp = body.find('&', pos);
        std::string_view token = (amp == std::string_view::npos)
                                     ? body.substr(pos)
                                     : body.substr(pos, amp - pos);

        if (token.size() > prefix.size() &&
            token.substr(0, prefix.size()) == prefix) {
            return std::string(token.substr(prefix.size()));
        }

        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return std::nullopt;
}
