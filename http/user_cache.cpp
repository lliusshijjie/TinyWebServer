#include "user_cache.h"

#include <memory>
#include <string>
#include "../CGImysql/sql_connection_pool.h"
#include "../log/log.h"

// LOG_INFO / LOG_ERROR 宏依赖此变量（0 = 日志开启）
namespace { int m_close_log = 0; }

void UserCache::load(connection_pool* pool) {
    MYSQL* mysql = nullptr;
    connectionRAII conn(&mysql, pool);

    if (mysql_query(mysql, "SELECT username,passwd FROM user")) {
        LOG_ERROR("UserCache load failed: %s", mysql_error(mysql));
        return;
    }

    // 结果集 RAII 自动释放
    auto result = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>(
        mysql_store_result(mysql), mysql_free_result);

    if (!result) {
        LOG_ERROR("UserCache: mysql_store_result returned NULL");
        return;
    }

    std::unique_lock lock(mutex_);
    users_.clear();

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result.get())) != nullptr) {
        if (row[0] && row[1]) {
            users_.emplace(row[0], row[1]);
        }
    }

    LOG_INFO("UserCache: loaded %zu user(s)", users_.size());
}

bool UserCache::authenticate(std::string_view user,
                             std::string_view pass) const {
    std::shared_lock lock(mutex_);
    auto it = users_.find(std::string(user));
    return it != users_.end() && it->second == pass;
}

bool UserCache::register_user(std::string_view user, std::string_view pass,
                              MYSQL* db) {
    // 先在读锁下检查是否已存在，避免不必要的写操作
    {
        std::shared_lock rlock(mutex_);
        if (users_.count(std::string(user))) {
            return false;
        }
    }

    // 对用户名和密码进行转义，防止 SQL 注入
    // mysql_real_escape_string 最多需要 2*len+1 字节的缓冲区
    std::string escaped_user(user.size() * 2 + 1, '\0');
    std::string escaped_pass(pass.size() * 2 + 1, '\0');

    unsigned long eu_len = mysql_real_escape_string(
        db, escaped_user.data(), user.data(), user.size());
    unsigned long ep_len = mysql_real_escape_string(
        db, escaped_pass.data(), pass.data(), pass.size());

    escaped_user.resize(eu_len);
    escaped_pass.resize(ep_len);

    std::string sql =
        "INSERT INTO user(username, passwd) VALUES('" +
        escaped_user + "', '" + escaped_pass + "')";

    std::unique_lock wlock(mutex_);

    // 在写锁下二次检查（双重检查锁定模式）
    if (users_.count(std::string(user))) {
        return false;
    }

    if (mysql_query(db, sql.c_str()) != 0) {
        LOG_ERROR("UserCache register failed: %s", mysql_error(db));
        return false;
    }

    users_.emplace(std::string(user), std::string(pass));
    return true;
}
