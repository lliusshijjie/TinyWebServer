#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include <queue>
#include <vector>
#include "../log/log.h"

class util_timer;

struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

class util_timer
{
public:
    util_timer() : cancelled(false) {}

public:
    time_t expire;

    void (* cb_func)(client_data *);
    client_data *user_data;
    bool cancelled;       // lazy-deletion marker
};

class sort_timer_lst
{
public:
    sort_timer_lst() = default;
    ~sort_timer_lst();

    void        add_timer(util_timer *timer);
    util_timer* adjust_timer(util_timer *timer);  // returns new pointer
    void        del_timer(util_timer *timer);
    void        tick();

private:
    struct Compare {
        bool operator()(const util_timer* a, const util_timer* b) const {
            return a->expire > b->expire;  // min-heap
        }
    };

    void purge();  // rebuild heap, free cancelled nodes

    std::priority_queue<util_timer*, std::vector<util_timer*>, Compare> heap_;
    static constexpr std::size_t kPurgeThreshold = 512;
    std::size_t cancelled_count_ = 0;
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;
};

void cb_func(client_data *user_data);

#endif
