#pragma once

#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <mysql/mysql.h>

class connection_pool;

// 线程安全的内存用户凭据缓存
// 启动时从 MySQL 一次性加载，注册成功后同步更新
class UserCache {
public:
    // 从 user 表批量加载用户名→密码
    void load(connection_pool* pool);

    // 验证用户凭据（共享读锁）
    bool authenticate(std::string_view user, std::string_view pass) const;

    // 注册新用户（排他写锁），同时写入内存和数据库
    // 使用 mysql_real_escape_string 防 SQL 注入
    // 成功返回 true，用户名已存在或数据库操作失败返回 false
    bool register_user(std::string_view user, std::string_view pass, MYSQL* db);

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::string> users_;
};
