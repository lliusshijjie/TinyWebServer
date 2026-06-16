#pragma once

#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <mysql/mysql.h>

class connection_pool;

// Thread-safe, in-memory user credential cache.
// Loaded once at startup from MySQL; updated on successful registration.
class UserCache {
public:
    // Bulk-load username→password from the 'user' table.
    void load(connection_pool* pool);

    // Returns true if (user, pass) exists in the cache (shared/read lock).
    bool authenticate(std::string_view user, std::string_view pass) const;

    // Insert a new user both in-memory and into the DB (exclusive/write lock).
    // Uses mysql_real_escape_string to prevent SQL injection.
    // Returns true on success, false if the username already exists or DB fails.
    bool register_user(std::string_view user, std::string_view pass, MYSQL* db);

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::string> users_;
};
