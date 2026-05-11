#include "logger.h"
#include "mmaplogwriter.h"
#include "logwriter.h"
#include "sequentiallogwriter.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <memory>
#include <string>
#include <thread>
#include <time.h>

namespace {

thread_local LogLevel tls_log_level = INFO; // 线程局部存储的日志级别，默认为 INFO

// 将字符串转换为小写
std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), 
        [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

// 根据日志落盘后端类型创建对应的 LogWriter 实例
std::unique_ptr<LogWriter> CreateLogWriter(LogWriteMode mode) {
    switch (mode) {
        case LogWriteMode::MMAP:
            return std::make_unique<MmapLogWriter>();
        case LogWriteMode::SEQUENTIAL:
            return std::make_unique<SequentialLogWriter>();
        default:    // 默认使用顺序写后端。
            return std::make_unique<SequentialLogWriter>();
    }
}

}  // namespace

Logger& Logger::GetInstance() {
    static Logger instance; // 局部静态变量，线程安全
    return instance;
}

// 构造函数，启动写日志线程
Logger::Logger() {  
    //容器预分配，减少扩容带来的性能抖动
    active_buffer_.reserve(kBufferReserve);
    flush_buffer_.reserve(kBufferReserve);

    // 启动写日志线程，持续将队列中的日志消息写入文本文件
    write_log_thread_ = std::thread([this]() {
        LogWriteMode active_mode = LogWriteMode::SEQUENTIAL;
        std::unique_ptr<LogWriter> writer;  // 当前使用的日志写入器实例。

        while (true) {
            LogWriteMode target_mode = LogWriteMode::SEQUENTIAL;
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                // 空闲时阻塞等待，退出时也会被 stop_requested_ 唤醒
                buffer_cv_.wait(lock, [this]() {
                    return stop_requested_ || !active_buffer_.empty();
                });

                if (!stop_requested_ && active_buffer_.size() < kSwapThreshold) {
                    // 已经有日志但未满阈值，等待一个短暂窗口，尝试凑齐一批
                    buffer_cv_.wait_for(lock, kFlushInterval, [this]() {
                        return stop_requested_ || active_buffer_.size() >= kSwapThreshold;
                    });
                }

                if (active_buffer_.empty() && stop_requested_) {
                    break;
                }

                // 交换前后端缓冲区，准备落盘
                if (!active_buffer_.empty()) {
                    active_buffer_.swap(flush_buffer_);
                }

                target_mode = write_mode_;
            }

            if (!flush_buffer_.empty()) {
                if (!writer || active_mode != target_mode) {    // 如果当前写入器未初始化或需要切换后端，则创建新的写入器实例
                    writer = CreateLogWriter(target_mode);  // 基类指针指向派生类实例，利用多态调用正确的 Append 和 Flush 方法
                    active_mode = target_mode;
                }

                // 将 flush_buffer_ 中的日志条目逐条写入文件，格式化输出时间戳和日志级别
                for (const auto& item : flush_buffer_) {
                    time_t now = time(nullptr);
                    tm nowtm;
                    localtime_r(&now, &nowtm);

                    char time_buffer[64];
                    snprintf(time_buffer, sizeof(time_buffer), " %02d:%02d:%02d => [%s]",
                             nowtm.tm_hour, nowtm.tm_min, nowtm.tm_sec,
                             LogLevelToString(item.level).c_str());

                    std::string line = time_buffer + item.message + "\n";
                    writer->Append(line, nowtm);
                }
                writer->Flush();    // 主动刷新，确保日志及时落盘
                flush_buffer_.clear();
            }
        }
    });
}

Logger::~Logger() {
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        stop_requested_ = true;
    }

    // 回收线程资源
    buffer_cv_.notify_one();
    if (write_log_thread_.joinable()) {
        write_log_thread_.join();
    }
}

void Logger::SetLogLevel(LogLevel level) {
    tls_log_level = level;
}

void Logger::SetWriteMode(LogWriteMode mode) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    write_mode_ = mode;
}

// 通过字符串切换日志落盘后端，支持 "mmap"、"sequential"
bool Logger::SetWriteModeByName(const std::string& mode_name) {
    const std::string normalized = ToLower(mode_name);
    if (normalized == "mmap") {
        SetWriteMode(LogWriteMode::MMAP);
        return true;
    }

    if (normalized == "sequential") {
        SetWriteMode(LogWriteMode::SEQUENTIAL);
        return true;
    }

    return false;
}

std::string Logger::GetWriteModeName() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    switch (write_mode_) {
        case LogWriteMode::MMAP:
            return "mmap";
        case LogWriteMode::SEQUENTIAL:
            return "sequential";
        default:
            return "unknown";
    }
}

std::string Logger::LogLevelToString(LogLevel level) {
    switch (level) {
        case INFO:
            return "INFO";
        case WARNING:
            return "WARNING";
        case ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

void Logger::Log(std::string message) {
    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (stop_requested_) {
            return;
        }

        bool was_empty = active_buffer_.empty();
        // 将日志消息添加到 active_buffer_ 中
        active_buffer_.push_back(LogItem{tls_log_level, std::move(message)});
        
        // 两种情况需要唤醒后台线程：
        // 1. 从空变非空，启动本轮聚合/落盘
        // 2. 数量达到阈值，提前触发本轮落盘
        should_notify = was_empty || active_buffer_.size() >= kSwapThreshold;
    }

    if (should_notify) {
        buffer_cv_.notify_one();    // 避免惊群
    }
}
