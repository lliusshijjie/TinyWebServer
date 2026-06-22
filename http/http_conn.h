#pragma once

#include <array>
#include <atomic>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <mysql/mysql.h>

#include "../CGImysql/sql_connection_pool.h"
#include "../log/log.h"
#include "mmap_guard.h"
#include "user_cache.h"

class HttpConn {
public:
    static constexpr int kFileNameLen  = 200;
    static constexpr int kReadBufSize  = 2048;
    static constexpr int kWriteBufSize = 1024;

    enum class Method     { Get, Post };
    enum class CheckState { RequestLine, Header, Content };
    enum class HttpCode   {
        NoRequest, GetRequest, BadRequest, NoResource,
        ForbiddenRequest, FileRequest, InternalError, ClosedConnection
    };
    enum class LineStatus { Ok, Bad, Open };
    enum class UrlAction  {
        RegisterPage,   // '0' -> /register.html
        LoginPage,      // '1' -> /log.html
        LoginSubmit,    // '2' POST -> verify credentials
        RegisterSubmit, // '3' POST -> create user
        PicturePage,    // '5' -> /picture.html
        VideoPage,      // '6' -> /video.html
        FansPage,       // '7' -> /fans.html
        StaticFile      // other -> serve file directly
    };

    // 所有连接共享；由 WebServer 初始化一次
    static std::atomic<int> user_count;
    static int              epoll_fd;
    static UserCache        user_cache;

    // 线程池和主事件循环共享的状态（Reactor 握手协议）
    std::atomic<bool> improv{false};
    std::atomic<bool> timer_flag{false};
    int               m_state{0};   // 0 = read, 1 = write
    MYSQL*            mysql{nullptr};

    HttpConn()  = default;
    ~HttpConn() { close_conn(); }
    HttpConn(const HttpConn&)            = delete;
    HttpConn& operator=(const HttpConn&) = delete;

    // 每个新连接由 WebServer 调用一次
    void init(int sockfd, const sockaddr_in& addr,
              std::string_view root, int trig_mode, int close_log,
              std::string_view user, std::string_view passwd,
              std::string_view db_name);

    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();

    const sockaddr_in* get_address() const noexcept { return &address_; }

    // 启动时一次性加载用户名/密码表到共享缓存
    static void load_user_cache(connection_pool* pool) {
        user_cache.load(pool);
    }

private:
    // HTTP 解析相关
    void       reset_state();
    LineStatus parse_line();
    HttpCode   process_read();
    HttpCode   parse_request_line(std::string_view text);
    HttpCode   parse_headers(std::string_view text);
    HttpCode   parse_content(std::string_view text);
    HttpCode   do_request();

    static UrlAction classify_url(std::string_view last_char);
    std::string_view get_line() const noexcept;

    // HTTP 响应构建相关
    bool process_write(HttpCode ret);
    bool add_response(const char* fmt, ...);
    bool add_status_line(int status, std::string_view title);
    bool add_headers(int content_len);
    bool add_content_length(int content_len);
    bool add_content_type();
    bool add_linger();
    bool add_blank_line();
    bool add_content(std::string_view content);

    // ------- 连接标识 -------
    int         sockfd_{-1};
    sockaddr_in address_{};

    // ------- 连接级配置 -------
    std::string doc_root_;
    int         trig_mode_{0};
    int         m_close_log;  // 0=open, 1=close (used by LOG macros)
    std::string sql_user_;
    std::string sql_passwd_;
    std::string sql_name_;

    // ------- 读缓冲 -------
    std::array<char, kReadBufSize> read_buf_{};
    int read_idx_{0};
    int checked_idx_{0};
    int line_start_{0};

    // ------- 写缓冲 -------
    std::array<char, kWriteBufSize> write_buf_{};
    int   write_idx_{0};
    int   bytes_to_send_{0};
    int   bytes_sent_{0};
    iovec iv_[2]{};
    int   iv_count_{0};

    // ------- HTTP 解析状态 -------
    CheckState  check_state_{CheckState::RequestLine};
    Method      method_{Method::Get};
    bool        is_cgi_{false};
    bool        linger_{false};
    std::string url_;
    std::string version_;
    std::string host_;
    long        content_length_{0};
    std::string request_body_;

    // ------- 文件服务 -------
    std::string real_file_;
    struct stat file_stat_{};
    MmapGuard   mmap_file_;
};

// 向后兼容别名 — 已有调用方可继续使用 'http_conn'
using http_conn = HttpConn;
