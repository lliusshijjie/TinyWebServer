#include "lst_timer.h"
#include "../http/http_conn.h"

// -------- sort_timer_lst: min-heap + lazy deletion --------

sort_timer_lst::~sort_timer_lst() {
    while (!heap_.empty()) {
        delete heap_.top();
        heap_.pop();
    }
}

void sort_timer_lst::add_timer(util_timer *timer) {
    if (!timer) return;
    heap_.push(timer);
}

util_timer* sort_timer_lst::adjust_timer(util_timer *timer) {
    if (!timer) return nullptr;

    // Cancel the old node (lazy delete).
    timer->cancelled = true;
    ++cancelled_count_;

    // Create a new node with the same payload and updated expire.
    auto *fresh = new util_timer;
    fresh->expire    = timer->expire;
    fresh->cb_func   = timer->cb_func;
    fresh->user_data = timer->user_data;

    heap_.push(fresh);

    if (cancelled_count_ >= kPurgeThreshold)
        purge();

    return fresh;
}

void sort_timer_lst::del_timer(util_timer *timer) {
    if (!timer) return;
    timer->cancelled = true;
    ++cancelled_count_;
}

void sort_timer_lst::tick() {
    if (heap_.empty()) return;

    time_t now = time(nullptr);

    while (!heap_.empty()) {
        util_timer *top = heap_.top();
        if (top->expire > now) break;

        heap_.pop();

        if (top->cancelled) {
            --cancelled_count_;
            delete top;
            continue;
        }

        // Fire the callback (closes fd, removes from epoll, decrements user_count).
        top->cb_func(top->user_data);
        delete top;
    }

    if (cancelled_count_ >= kPurgeThreshold)
        purge();
}

void sort_timer_lst::purge() {
    // Rebuild the heap keeping only live nodes.
    std::vector<util_timer*> live;
    live.reserve(heap_.size());

    while (!heap_.empty()) {
        util_timer *t = heap_.top();
        heap_.pop();
        if (t->cancelled) {
            delete t;
        } else {
            live.push_back(t);
        }
    }

    for (auto *t : live)
        heap_.push(t);

    cancelled_count_ = 0;
}

// -------- Utils --------

void Utils::init(int timeslot) {
    m_TIMESLOT = timeslot;
}

int Utils::setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void Utils::sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

void Utils::addsig(int sig, void(handler)(int), bool restart) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void Utils::timer_handler() {
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info) {
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data) {
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    HttpConn::user_count.fetch_sub(1, std::memory_order_relaxed);
}
