#include "http_conn.h"
#include "url_params.h"

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <strings.h>   // strncasecmp 大小写不敏感比较
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

// ---- 静态成员定义 ----
std::atomic<int> HttpConn::user_count{0};
int              HttpConn::epoll_fd = -1;
UserCache        HttpConn::user_cache;

// ---- 模块内 epoll 辅助函数（替代原重复的自由函数） ----
namespace {

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void add_fd(int epollfd, int fd, bool one_shot, int trig_mode) {
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events  = (trig_mode == 1) ? (EPOLLIN | EPOLLET | EPOLLRDHUP)
                                   : (EPOLLIN | EPOLLRDHUP);
    if (one_shot) ev.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    set_nonblocking(fd);
}

void remove_fd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}

void mod_fd(int epollfd, int fd, int ev_flags, int trig_mode) {
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events  = (trig_mode == 1)
                     ? (ev_flags | EPOLLET | EPOLLONESHOT | EPOLLRDHUP)
                     : (ev_flags | EPOLLONESHOT | EPOLLRDHUP);
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &ev);
}

// HTTP 响应常量
constexpr std::string_view k200Title   = "OK";
constexpr std::string_view k400Title   = "Bad Request";
constexpr std::string_view k400Body    = "Your request has bad syntax or is inherently impossible to satisfy.\n";
constexpr std::string_view k403Title   = "Forbidden";
constexpr std::string_view k403Body    = "You do not have permission to get file from this server.\n";
constexpr std::string_view k404Title   = "Not Found";
constexpr std::string_view k404Body    = "The requested file was not found on this server.\n";
constexpr std::string_view k500Title   = "Internal Error";
constexpr std::string_view k500Body    = "There was an unusual problem serving the request file.\n";

} // namespace

// ============================================================
//  生命周期
// ============================================================

void HttpConn::init(int sockfd, const sockaddr_in& addr,
                    std::string_view root, int trig_mode, int close_log,
                    std::string_view user, std::string_view passwd,
                    std::string_view db_name) {
    sockfd_     = sockfd;
    address_    = addr;
    trig_mode_  = trig_mode;
    m_close_log = close_log;
    doc_root_   = std::string(root);
    sql_user_   = std::string(user);
    sql_passwd_ = std::string(passwd);
    sql_name_   = std::string(db_name);

    // add_fd 调用前先设置 trig_mode_，确保使用正确的触发模式
    add_fd(epoll_fd, sockfd_, /*one_shot=*/true, trig_mode_);
    user_count.fetch_add(1, std::memory_order_relaxed);

    reset_state();
}

void HttpConn::reset_state() {
    mysql          = nullptr;
    bytes_to_send_ = 0;
    bytes_sent_    = 0;
    check_state_   = CheckState::RequestLine;
    method_        = Method::Get;
    linger_        = false;
    is_cgi_        = false;
    content_length_ = 0;
    line_start_    = 0;
    checked_idx_   = 0;
    read_idx_      = 0;
    write_idx_     = 0;
    iv_count_      = 0;
    m_state        = 0;

    url_.clear();
    version_.clear();
    host_.clear();
    request_body_.clear();
    real_file_.clear();
    mmap_file_.release();

    improv.store(false, std::memory_order_relaxed);
    timer_flag.store(false, std::memory_order_relaxed);

    read_buf_.fill('\0');
    write_buf_.fill('\0');
    std::memset(iv_, 0, sizeof(iv_));
    std::memset(&file_stat_, 0, sizeof(file_stat_));
}

void HttpConn::close_conn(bool real_close) {
    if (real_close && sockfd_ != -1) {
        remove_fd(epoll_fd, sockfd_);
        sockfd_ = -1;
        user_count.fetch_sub(1, std::memory_order_relaxed);
    }
}

// ============================================================
//  读取
// ============================================================

bool HttpConn::read_once() {
    if (read_idx_ >= kReadBufSize) return false;

    if (trig_mode_ == 0) {
        // LT 模式：每次 epoll 通知只调用一次 recv
        int n = static_cast<int>(
            recv(sockfd_, read_buf_.data() + read_idx_,
                 kReadBufSize - read_idx_, 0));
        if (n <= 0) return false;
        read_idx_ += n;
        return true;
    }

    // ET 模式：循环读取直到 EAGAIN
    while (true) {
        int n = static_cast<int>(
            recv(sockfd_, read_buf_.data() + read_idx_,
                 kReadBufSize - read_idx_, 0));
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;
        }
        if (n == 0) return false;
        read_idx_ += n;
    }
    return true;
}

// ============================================================
//  HTTP 解析 — 状态机
// ============================================================

std::string_view HttpConn::get_line() const noexcept {
    return {read_buf_.data() + line_start_,
            static_cast<std::size_t>(checked_idx_ - line_start_)};
}

HttpConn::LineStatus HttpConn::parse_line() {
    for (; checked_idx_ < read_idx_; ++checked_idx_) {
        char c = read_buf_[checked_idx_];
        if (c == '\r') {
            if (checked_idx_ + 1 == read_idx_) return LineStatus::Open;
            if (read_buf_[checked_idx_ + 1] == '\n') {
                read_buf_[checked_idx_++] = '\0';
                read_buf_[checked_idx_++] = '\0';
                return LineStatus::Ok;
            }
            return LineStatus::Bad;
        } else if (c == '\n') {
            if (checked_idx_ > 0 && read_buf_[checked_idx_ - 1] == '\r') {
                read_buf_[checked_idx_ - 1] = '\0';
                read_buf_[checked_idx_++]   = '\0';
                return LineStatus::Ok;
            }
            return LineStatus::Bad;
        }
    }
    return LineStatus::Open;
}

HttpConn::HttpCode HttpConn::process_read() {
    LineStatus line_st = LineStatus::Ok;
    HttpCode   ret     = HttpCode::NoRequest;

    while ((check_state_ == CheckState::Content && line_st == LineStatus::Ok) ||
           (line_st = parse_line()) == LineStatus::Ok) {
        std::string_view text = get_line();
        line_start_ = checked_idx_;

        LOG_INFO("%.*s", static_cast<int>(text.size()), text.data());

        switch (check_state_) {
        case CheckState::RequestLine:
            ret = parse_request_line(text);
            if (ret == HttpCode::BadRequest) return HttpCode::BadRequest;
            break;
        case CheckState::Header:
            ret = parse_headers(text);
            if (ret == HttpCode::BadRequest) return HttpCode::BadRequest;
            if (ret == HttpCode::GetRequest) return do_request();
            break;
        case CheckState::Content:
            ret = parse_content(text);
            if (ret == HttpCode::GetRequest) return do_request();
            line_st = LineStatus::Open;
            break;
        default:
            return HttpCode::InternalError;
        }
    }
    return HttpCode::NoRequest;
}

HttpConn::HttpCode HttpConn::parse_request_line(std::string_view text) {
    // 提取请求方法
    auto sp1 = text.find_first_of(" \t");
    if (sp1 == std::string_view::npos) return HttpCode::BadRequest;

    std::string_view method_sv = text.substr(0, sp1);
    if (method_sv == "GET")       { method_ = Method::Get;  }
    else if (method_sv == "POST") { method_ = Method::Post; is_cgi_ = true; }
    else return HttpCode::BadRequest;

    // 提取 URL
    auto url_s = text.find_first_not_of(" \t", sp1);
    if (url_s == std::string_view::npos) return HttpCode::BadRequest;
    auto url_e = text.find_first_of(" \t", url_s);
    if (url_e == std::string_view::npos) return HttpCode::BadRequest;
    std::string_view url_sv = text.substr(url_s, url_e - url_s);

    // 提取版本号
    auto ver_s = text.find_first_not_of(" \t", url_e);
    if (ver_s == std::string_view::npos) return HttpCode::BadRequest;
    std::string_view ver_sv = text.substr(ver_s);
    // 去除行尾空白字符和空字节
    while (!ver_sv.empty() && (ver_sv.back() == '\0' || ver_sv.back() == '\r'))
        ver_sv.remove_suffix(1);
    if (strncasecmp(ver_sv.data(), "HTTP/1.1", 8) != 0)
        return HttpCode::BadRequest;

    // 剥离协议头（http:// 或 https://）
    if (url_sv.size() > 7 && strncasecmp(url_sv.data(), "http://", 7) == 0) {
        url_sv.remove_prefix(7);
        auto slash = url_sv.find('/');
        if (slash == std::string_view::npos) return HttpCode::BadRequest;
        url_sv.remove_prefix(slash);
    } else if (url_sv.size() > 8 && strncasecmp(url_sv.data(), "https://", 8) == 0) {
        url_sv.remove_prefix(8);
        auto slash = url_sv.find('/');
        if (slash == std::string_view::npos) return HttpCode::BadRequest;
        url_sv.remove_prefix(slash);
    }

    if (url_sv.empty() || url_sv[0] != '/') return HttpCode::BadRequest;

    url_         = (url_sv.size() == 1) ? "/judge.html" : std::string(url_sv);
    check_state_ = CheckState::Header;
    return HttpCode::NoRequest;
}

HttpConn::HttpCode HttpConn::parse_headers(std::string_view text) {
    // 空行（parse_line 将 \r\n 替换为 \0\0，因此首字符为 '\0'）
    if (text[0] == '\0') {
        if (content_length_ != 0) {
            check_state_ = CheckState::Content;
            return HttpCode::NoRequest;
        }
        return HttpCode::GetRequest;
    }

    // 辅助函数：大小写不敏感前缀匹配 + 去除值的前导空白
    auto header_val = [&](std::string_view prefix) -> std::string_view {
        if (text.size() <= prefix.size()) return {};
        if (strncasecmp(text.data(), prefix.data(), prefix.size()) != 0) return {};
        std::string_view val = text.substr(prefix.size());
        auto s = val.find_first_not_of(" \t");
        return (s == std::string_view::npos) ? std::string_view{} : val.substr(s);
    };

    if (auto v = header_val("Connection:"); !v.empty()) {
        linger_ = (strncasecmp(v.data(), "keep-alive", 10) == 0);
    } else if (auto v = header_val("Content-length:"); !v.empty()) {
        content_length_ = std::stol(std::string(v));
    } else if (auto v = header_val("Host:"); !v.empty()) {
        host_ = std::string(v);
    } else {
        LOG_INFO("unknown header: %.*s",
                     static_cast<int>(text.size()), text.data());
    }
    return HttpCode::NoRequest;
}

HttpConn::HttpCode HttpConn::parse_content(std::string_view text) {
    if (read_idx_ >= static_cast<int>(checked_idx_) + content_length_) {
        request_body_ = std::string(text.substr(0, content_length_));
        return HttpCode::GetRequest;
    }
    return HttpCode::NoRequest;
}

// ============================================================
//  请求分发
// ============================================================

HttpConn::UrlAction HttpConn::classify_url(std::string_view seg) {
    if (seg.empty()) return UrlAction::StaticFile;
    switch (seg[0]) {
    case '0': return UrlAction::RegisterPage;
    case '1': return UrlAction::LoginPage;
    case '2': return UrlAction::LoginSubmit;
    case '3': return UrlAction::RegisterSubmit;
    case '5': return UrlAction::PicturePage;
    case '6': return UrlAction::VideoPage;
    case '7': return UrlAction::FansPage;
    default:  return UrlAction::StaticFile;
    }
}

HttpConn::HttpCode HttpConn::do_request() {
    // 找到最后一个 '/' 后面的字符，确定路由动作
    auto last_slash = url_.rfind('/');
    std::string_view seg = (last_slash == std::string::npos)
                               ? std::string_view(url_)
                               : std::string_view(url_).substr(last_slash + 1);

    UrlAction action = classify_url(seg);

    // CGI 处理：POST 登录或注册
    if (is_cgi_ &&
        (action == UrlAction::LoginSubmit || action == UrlAction::RegisterSubmit)) {
        auto opt_user = get_param(request_body_, "user");
        auto opt_pass = get_param(request_body_, "passwd");

        if (!opt_user || !opt_pass) {
            url_   = "/logError.html";
            action = UrlAction::StaticFile;
        } else {
            if (action == UrlAction::LoginSubmit) {
                url_ = user_cache.authenticate(*opt_user, *opt_pass)
                           ? "/welcome.html" : "/logError.html";
            } else {
                url_ = user_cache.register_user(*opt_user, *opt_pass, mysql)
                           ? "/log.html" : "/registerError.html";
            }
            action = UrlAction::StaticFile;
        }
    }

    // 将 URL 动作映射到文件路径
    switch (action) {
    case UrlAction::RegisterPage:   real_file_ = doc_root_ + "/register.html"; break;
    case UrlAction::LoginPage:      real_file_ = doc_root_ + "/log.html";      break;
    case UrlAction::PicturePage:    real_file_ = doc_root_ + "/picture.html";  break;
    case UrlAction::VideoPage:      real_file_ = doc_root_ + "/video.html";    break;
    case UrlAction::FansPage:       real_file_ = doc_root_ + "/fans.html";     break;
    default:                        real_file_ = doc_root_ + url_;             break;
    }

    if (stat(real_file_.c_str(), &file_stat_) < 0) return HttpCode::NoResource;
    if (!(file_stat_.st_mode & S_IROTH))            return HttpCode::ForbiddenRequest;
    if (S_ISDIR(file_stat_.st_mode))               return HttpCode::BadRequest;

    int fd = open(real_file_.c_str(), O_RDONLY);
    if (fd < 0) return HttpCode::NoResource;

    void* addr = mmap(nullptr, static_cast<std::size_t>(file_stat_.st_size),
                      PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) {
        LOG_ERROR("mmap failed for %s: %s", real_file_.c_str(), strerror(errno));
        return HttpCode::InternalError;
    }

    mmap_file_ = MmapGuard(addr, static_cast<std::size_t>(file_stat_.st_size));
    return HttpCode::FileRequest;
}

// ============================================================
//  响应构建
// ============================================================

bool HttpConn::add_response(const char* fmt, ...) {
    if (write_idx_ >= kWriteBufSize) return false;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(write_buf_.data() + write_idx_,
                      kWriteBufSize - 1 - write_idx_, fmt, args);
    va_end(args);
    if (n < 0 || n >= kWriteBufSize - 1 - write_idx_) return false;
    write_idx_ += n;
    return true;
}

bool HttpConn::add_status_line(int status, std::string_view title) {
    return add_response("HTTP/1.1 %d %.*s\r\n",
                        status,
                        static_cast<int>(title.size()), title.data());
}

bool HttpConn::add_content_length(int content_len) {
    return add_response("Content-Length:%d\r\n", content_len);
}

bool HttpConn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool HttpConn::add_linger() {
    return add_response("Connection:%s\r\n",
                        linger_ ? "keep-alive" : "close");
}

bool HttpConn::add_blank_line() {
    return add_response("%s", "\r\n");
}

bool HttpConn::add_headers(int content_len) {
    return add_content_length(content_len) && add_linger() && add_blank_line();
}

bool HttpConn::add_content(std::string_view content) {
    return add_response("%.*s",
                        static_cast<int>(content.size()), content.data());
}

bool HttpConn::process_write(HttpCode ret) {
    switch (ret) {
    case HttpCode::InternalError:
        add_status_line(500, k500Title);
        add_headers(static_cast<int>(k500Body.size()));
        if (!add_content(k500Body)) return false;
        break;

    case HttpCode::BadRequest:
        add_status_line(404, k404Title);
        add_headers(static_cast<int>(k404Body.size()));
        if (!add_content(k404Body)) return false;
        break;

    case HttpCode::ForbiddenRequest:
        add_status_line(403, k403Title);
        add_headers(static_cast<int>(k403Body.size()));
        if (!add_content(k403Body)) return false;
        break;

    case HttpCode::NoResource:
        add_status_line(404, k404Title);
        add_headers(static_cast<int>(k404Body.size()));
        if (!add_content(k404Body)) return false;
        break;

    case HttpCode::FileRequest:
        add_status_line(200, k200Title);
        if (file_stat_.st_size != 0) {
            add_headers(static_cast<int>(file_stat_.st_size));
            iv_[0].iov_base = write_buf_.data();
            iv_[0].iov_len  = static_cast<std::size_t>(write_idx_);
            iv_[1].iov_base = mmap_file_.get();
            iv_[1].iov_len  = static_cast<std::size_t>(file_stat_.st_size);
            iv_count_       = 2;
            bytes_to_send_  = write_idx_ + static_cast<int>(file_stat_.st_size);
            return true;
        } else {
            constexpr std::string_view empty_body = "<html><body></body></html>";
            add_headers(static_cast<int>(empty_body.size()));
            if (!add_content(empty_body)) return false;
        }
        break;

    default:
        return false;
    }

    iv_[0].iov_base = write_buf_.data();
    iv_[0].iov_len  = static_cast<std::size_t>(write_idx_);
    iv_count_       = 1;
    bytes_to_send_  = write_idx_;
    return true;
}

// ============================================================
//  写操作
// ============================================================

bool HttpConn::write() {
    if (bytes_to_send_ == 0) {
        mod_fd(epoll_fd, sockfd_, EPOLLIN, trig_mode_);
        reset_state();
        return true;
    }

    while (true) {
        ssize_t n = writev(sockfd_, iv_, iv_count_);

        if (n < 0) {
            if (errno == EAGAIN) {
                mod_fd(epoll_fd, sockfd_, EPOLLOUT, trig_mode_);
                return true;
            }
            mmap_file_.release();
            return false;
        }

        bytes_sent_    += static_cast<int>(n);
        bytes_to_send_ -= static_cast<int>(n);

        // 部分写入后调整 iovec 指针
        if (bytes_sent_ >= static_cast<int>(iv_[0].iov_len)) {
            iv_[0].iov_len  = 0;
            iv_[1].iov_base = mmap_file_.get() + (bytes_sent_ - write_idx_);
            iv_[1].iov_len  = static_cast<std::size_t>(bytes_to_send_);
        } else {
            iv_[0].iov_base = write_buf_.data() + bytes_sent_;
            iv_[0].iov_len -= static_cast<std::size_t>(n);
        }

        if (bytes_to_send_ <= 0) {
            mmap_file_.release();
            mod_fd(epoll_fd, sockfd_, EPOLLIN, trig_mode_);
            if (linger_) {
                reset_state();
                return true;
            }
            return false;
        }
    }
}

// ============================================================
//  线程池调用入口
// ============================================================

void HttpConn::process() {
    HttpCode read_ret = process_read();
    if (read_ret == HttpCode::NoRequest) {
        mod_fd(epoll_fd, sockfd_, EPOLLIN, trig_mode_);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
        return;
    }
    mod_fd(epoll_fd, sockfd_, EPOLLOUT, trig_mode_);
}
