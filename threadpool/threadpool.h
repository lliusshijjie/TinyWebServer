#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    threadpool(int actor_model, connection_pool *connPool,
               int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    void run();

    int m_thread_number;
    int m_max_requests;
    std::deque<T *> m_workqueue;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    connection_pool *m_connPool;
    std::atomic<bool> m_stop{false};
    std::function<void(T *)> m_dispatcher;
    std::vector<std::thread> m_workers;
};

template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool,
                          int thread_number, int max_requests)
    : m_thread_number(thread_number)
    , m_max_requests(max_requests)
    , m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::invalid_argument(
            "threadpool: thread_number and max_requests must be positive");

    // 根据actor_model一次性设置分发函数，消除热路径上的运行时分支
    if (actor_model == 1)
    {
        // reactor 模式：工作线程负责 I/O + 处理
        m_dispatcher = [this](T *request)
        {
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    request->improv.store(true, std::memory_order_release);
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }
                else
                {
                    request->improv.store(true, std::memory_order_release);
                    request->timer_flag.store(true, std::memory_order_release);
                }
            }
            else
            {
                if (request->write())
                {
                    request->improv.store(true, std::memory_order_release);
                }
                else
                {
                    request->improv.store(true, std::memory_order_release);
                    request->timer_flag.store(true, std::memory_order_release);
                }
            }
        };
    }
    else
    {
        // proactor 模式：主线程已完成 I/O，工作线程只负责处理
        m_dispatcher = [this](T *request)
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        };
    }

    // 创建工作线程，异常安全：如果部分线程创建失败，停止已创建的线程并重抛异常
    m_workers.reserve(thread_number);
    try
    {
        for (int i = 0; i < thread_number; ++i)
            m_workers.emplace_back([this] { run(); });
    }
    catch (...)
    {
        m_stop.store(true, std::memory_order_release);
        m_cond.notify_all();
        for (auto &t : m_workers)
        {
            if (t.joinable())
                t.join();
        }
        throw;
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop.store(true, std::memory_order_release);
    }
    m_cond.notify_all();
    for (auto &t : m_workers)
    {
        if (t.joinable())
            t.join();
    }
}

template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_workqueue.size() >= static_cast<size_t>(m_max_requests))
            return false;
        request->m_state = state;
        m_workqueue.push_back(request);
    }
    m_cond.notify_one();
    return true;
}

template <typename T>
bool threadpool<T>::append_p(T *request)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_workqueue.size() >= static_cast<size_t>(m_max_requests))
            return false;
        m_workqueue.push_back(request);
    }
    m_cond.notify_one();
    return true;
}

template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        T *request = nullptr;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cond.wait(lock, [this] {
                return m_stop.load(std::memory_order_acquire)
                       || !m_workqueue.empty();
            });

            // 停止信号 + 队列已清空 → 安全退出
            if (m_stop.load(std::memory_order_acquire) && m_workqueue.empty())
                return;

            request = m_workqueue.front();
            m_workqueue.pop_front();
        }  // 锁在此释放（RAII），确保处理请求时不持有锁

        if (request)
            m_dispatcher(request);
    }
}

#endif
