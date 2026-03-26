#include "logger.h"
#include <time.h>
#include <iostream>

Logger& Logger::GetInstance() {
    static Logger instance; // 局部静态变量，线程安全
    return instance;
}

Logger::Logger() {
    // 启动写日志线程，持续将队列中的日志消息写入文本文件
    std::thread writeLogThread([this]() {
        while (true) {
            // 获取日志消息并根据日期写入日志文件
            time_t now = time(nullptr);
            tm *nowtm = localtime(&now);
            
            // 文件名格式：年-月-日-log.txt
            char fileName[64];
            sprintf(fileName, "%d-%d-%d-log.txt", nowtm->tm_year+1900, nowtm->tm_mon+1, nowtm->tm_mday);
            
            FILE* fp = fopen(fileName, "a+");   // 以追加模式打开日志文件
            if (!fp) {
                std::cerr << "Failed to open log file: " << fileName << std::endl;
                exit(EXIT_FAILURE);
            }

            char timeBuffer[64];
            sprintf(timeBuffer, " %02d:%02d:%02d => [%s]", 
                    nowtm->tm_hour, nowtm->tm_min, nowtm->tm_sec, LogLevelToString(log_level).c_str());

            std::string logMessage = log_queue.pop(); // 从队列中获取日志消息
            logMessage = timeBuffer + logMessage + "\n"; // 添加时间戳和换行符
            fputs(logMessage.c_str(), fp); // 写入日志消息
            fclose(fp); // 关闭文件
        }
    });

    // detach线程，使其在后台运行
    writeLogThread.detach();
}

void Logger::SetLogLevel(LogLevel level) {
    log_level = level;
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
    log_queue.push(message);
}