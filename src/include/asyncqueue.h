#pragma once
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>   // 条件变量

// 异步写日志队列
template<typename T>
class AsyncQueue {
public:
    /*
     * @brief 将日志项加入队列
     * @param item 日志项
     **/
    void push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);  // 加锁
        queue_.push(item);  // 将日志项加入队列
        cond_var_.notify_one();  // 通知等待的线程(执行pop函数的线程)有新的日志项可用
    }

    /*
    * @brief 从队列中获取日志项
    * @return T 日志项
    **/
    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);  // 加锁
        while (queue_.empty()) {
            // 如果队列为空，等待条件变量通知
            cond_var_.wait(lock);   // 等待通知并释放锁
        }

        T item = queue_.front();  // 获取队列头部的日志项
        queue_.pop();  // 从队列中移除日志项
        return item;  // 返回日志项
    }
private:
    std::queue<T> queue_;  // 队列
    std::mutex mutex_;     // 互斥锁
    std::condition_variable cond_var_;  // 条件变量
};