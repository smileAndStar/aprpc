#pragma once
#include <cstddef>
#include <string>
#include <cstdio>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

/**
 * @def LOG_INFO(logmsgformat, ...)
 * @brief 输出 INFO 级别的日志
 * @param logmsgformat 日志格式字符串，支持 printf 风格格式化
 */
#define LOG_INFO(logmsgformat, ...) \
    do { \
        Logger &logger = Logger::GetInstance(); \
        logger.SetLogLevel(INFO); \
        char log_buffer[1024]; \
        snprintf(log_buffer, sizeof(log_buffer), logmsgformat, ##__VA_ARGS__); \
        logger.Log(log_buffer); \
    } while(0);

/**
 * @def LOG_WARNING(logmsgformat, ...)
 * @brief 输出 WARNING 级别的日志
 * @param logmsgformat 日志格式字符串，支持 printf 风格格式化
 */
#define LOG_WARNING(logmsgformat, ...) \
    do { \
        Logger &logger = Logger::GetInstance(); \
        logger.SetLogLevel(WARNING); \
        char log_buffer[1024]; \
        snprintf(log_buffer, sizeof(log_buffer), logmsgformat, ##__VA_ARGS__); \
        logger.Log(log_buffer); \
    } while(0);

/**
 * @def LOG_ERR(logmsgformat, ...)
 * @brief 输出 ERROR 级别的日志
 * @param logmsgformat 日志格式字符串，支持 printf 风格格式化
 */
#define LOG_ERR(logmsgformat, ...) \
    do { \
        Logger &logger = Logger::GetInstance(); \
        logger.SetLogLevel(ERROR); \
        char log_buffer[1024]; \
        snprintf(log_buffer, sizeof(log_buffer), logmsgformat, ##__VA_ARGS__); \
        logger.Log(log_buffer); \
    } while(0);

/**
 * @enum LogLevel
 * @brief 定义日志的严重级别
 */
enum LogLevel {
    INFO,       ///< 普通信息日志
    WARNING,    ///< 警告日志
    ERROR       ///< 错误日志
};

/**
 * @enum LogWriteMode
 * @brief 日志落盘后端类型
 */
enum class LogWriteMode {
    MMAP,           ///< 基于 mmap 的追加写
    SEQUENTIAL      ///< 基于普通顺序追加写
};

/**
 * @class Logger
 * @brief 高性能双缓冲单例日志系统
 * 
 * 采用双缓冲（Double Buffering）架构，并支持 mmap / 顺序写两种落盘后端。
 * 业务线程通过锁极快地将消息缓存到 active_buffer_，
 * 后台线程离线提取 flush_buffer_，再交给选定的写入器完成实际落盘，
 * 实现低延迟、高吞吐的无锁化落盘操作。
 */
class Logger {
public:
    /**
     * @brief 获取日志系统实例
     * @return Logger& 日志系统实例
     **/
    static Logger& GetInstance();

    /**
     * @brief 设置日志级别
     * @param level 日志级别
     **/
    void SetLogLevel(LogLevel level);

    /**
     * @brief 获取日志级别
     * @param level 日志级别
     * @return std::string 日志级别字符串
     **/
    std::string LogLevelToString(LogLevel level);

    /**
     * @brief 写日志
     * @param message 日志消息
     **/
    void Log(std::string message);

    /**
     * @brief 显式切换日志落盘后端(mmap / sequential)
     * @param mode 落盘后端类型
     */
    void SetWriteMode(LogWriteMode mode);

    /**
     * @brief 通过字符串切换日志落盘后端
     * @param mode_name 支持 mmap / sequential(不区分大小写)
     * @return 是否切换成功
     */
    bool SetWriteModeByName(const std::string& mode_name);

    /**
     * @brief 获取当前日志落盘后端名称
     * @return std::string 当前后端名称
     */
    std::string GetWriteModeName();

    ~Logger();

private:
    /**
     * @struct LogItem
     * @brief 存储单条日志的核心内容
     */
    struct LogItem {
        LogLevel level;         ///< 对应的日志级别
        std::string message;    ///< 预分配未格式化的原始字符串
    };

    /** @name 双缓冲机制核心变量 */
    ///@{
    static constexpr std::size_t kSwapThreshold = 1024;    ///< active_buffer_ 积累多少条目触发后台线程唤醒
    static constexpr std::size_t kBufferReserve = 4096;    ///< vector 缓冲区的预分配容量（防止扩容导致抖动）
    static constexpr auto kFlushInterval = std::chrono::milliseconds(200); ///< 定时落盘的时间间隔(200ms),超时后不攒满也要落盘

    std::vector<LogItem> active_buffer_;       ///< 前端缓冲（工作线程通过互斥锁进行写入）
    std::vector<LogItem> flush_buffer_;        ///< 后端缓冲（仅后台单线程进行消费、处理和落盘）
    
    std::mutex buffer_mutex_;                  ///< 保护 active_buffer_ 和 swap 流程的互斥锁
    std::condition_variable buffer_cv_;        ///< 用于前后端线程调度唤醒的条件变量
    std::thread write_log_thread_;             ///< 后台落盘线程
    bool stop_requested_ = false;              ///< 进程退出时通知后台线程停止
    LogWriteMode write_mode_ = LogWriteMode::MMAP;   ///< 当前选定的日志落盘后端
    ///@}

    /**
     * @brief 默认构造函数（启动后台落盘线程）
     */
    Logger();

    Logger(const Logger&) = delete;  ///< 禁止拷贝构造函数
    Logger(Logger&&) = delete;       ///< 禁止移动构造函数
};
