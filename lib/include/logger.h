#pragma once
#include "asyncqueue.h"

#define LOG_INFO(logmsgformat, ...) \
    do { \
        Logger &logger = Logger::GetInstance(); \
        logger.SetLogLevel(INFO); \
        char log_buffer[1024]; \
        snprintf(log_buffer, sizeof(log_buffer), logmsgformat, ##__VA_ARGS__); \
        logger.Log(log_buffer); \
    } while(0);

#define LOG_WARNING(logmsgformat, ...) \
    do { \
        Logger &logger = Logger::GetInstance(); \
        logger.SetLogLevel(WARNING); \
        char log_buffer[1024]; \
        snprintf(log_buffer, sizeof(log_buffer), logmsgformat, ##__VA_ARGS__); \
        logger.Log(log_buffer); \
    } while(0);

#define LOG_ERR(logmsgformat, ...) \
    do { \
        Logger &logger = Logger::GetInstance(); \
        logger.SetLogLevel(ERROR); \
        char log_buffer[1024]; \
        snprintf(log_buffer, sizeof(log_buffer), logmsgformat, ##__VA_ARGS__); \
        logger.Log(log_buffer); \
    } while(0);

enum LogLevel {
    INFO,
    WARNING,
    ERROR
};

// Aprpc 日志系统
class Logger {
public:
    /*
     * @brief 获取日志系统实例
     * @return Logger& 日志系统实例
     **/
    static Logger& GetInstance();

    /*
     * @brief 设置日志级别
     * @param level 日志级别
     **/
    void SetLogLevel(LogLevel level);

    /*
     * @brief 获取日志级别字符串
     * @param level 日志级别
     * @return std::string 日志级别字符串
     **/
    std::string LogLevelToString(LogLevel level);

    /*
     * @brief 写日志
     * @param message 日志消息
     **/
    void Log(std::string message);

private:
    LogLevel log_level; // 当前日志级别
    AsyncQueue<std::string> log_queue; // 日志消息队列

    Logger();
    Logger(const Logger&) = delete; //  禁止拷贝构造函数
    Logger(Logger&&) = delete;  //  禁止移动构造函数
};