#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    m_cancelled_count = 0;
}

sort_timer_lst::~sort_timer_lst()
{
    while (!m_heap.empty())
    {
        delete m_heap.top();
        m_heap.pop();
    }
}

void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    m_heap.push(timer);
}

util_timer *sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return NULL;
    }

    //惰性删除：标记旧节点已取消，新建节点入堆
    timer->cancelled = true;
    ++m_cancelled_count;

    util_timer *fresh = new util_timer;
    fresh->expire = timer->expire;
    fresh->cb_func = timer->cb_func;
    fresh->user_data = timer->user_data;

    m_heap.push(fresh);

    if (m_cancelled_count >= 512)
        purge();

    return fresh;
}

void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    timer->cancelled = true;
    ++m_cancelled_count;
}

void sort_timer_lst::tick()
{
    if (m_heap.empty())
    {
        return;
    }

    time_t cur = time(NULL);

    while (!m_heap.empty())
    {
        util_timer *top = m_heap.top();
        if (top->expire > cur)
        {
            break;
        }

        m_heap.pop();

        //惰性删除：跳过已取消的节点
        if (top->cancelled)
        {
            --m_cancelled_count;
            delete top;
            continue;
        }

        top->cb_func(top->user_data);
        delete top;
    }

    if (m_cancelled_count >= 512)
        purge();
}

void sort_timer_lst::purge()
{
    //重建堆，清除已取消的节点
    std::vector<util_timer *> live;

    while (!m_heap.empty())
    {
        util_timer *t = m_heap.top();
        m_heap.pop();
        if (t->cancelled)
        {
            delete t;
        }
        else
        {
            live.push_back(t);
        }
    }

    for (size_t i = 0; i < live.size(); ++i)
        m_heap.push(live[i]);

    m_cancelled_count = 0;
}

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
