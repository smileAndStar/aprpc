#pragma once

#include "logwriter.h"
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>

/**
 * @brief 基于普通顺序追加写的日志落盘写入器
 *
 * 行为类似 muduo 的 append file 路径：复用 FILE 缓冲区，按天切分文件，
 * 通过顺序 fwrite 追加日志，避免每条日志单独 open/close。
 */
class SequentialLogWriter final : public LogWriter {
public:
    SequentialLogWriter();
    ~SequentialLogWriter() override;

    void Append(const std::string& line, const tm& nowtm) override;
    void Flush() override;

private:
    static constexpr std::size_t kFileBufferBytes = 64 * 1024;
    static constexpr std::size_t kFlushThresholdBytes = 256 * 1024;

    FILE* fp_;                                          ///< 当前打开的日志文件指针
    std::array<char, kFileBufferBytes> file_buffer_;    ///< FILE 内部缓冲区，减少系统调用次数
    int current_year_;                                  ///< 当前日志文件对应的年份
    int current_month_;                                 ///< 当前日志文件对应的月份
    int current_day_;                                   ///< 当前日志文件对应的日期
    std::size_t dirty_bytes_;                           ///< 当前缓冲区中尚未刷新的字节数
    std::string file_name_;                             ///< 当前日志文件名，用于检测文件是否被外部删除
    int last_check_second_of_day_;                      ///< 上次检测文件的秒级时间戳，用于节流避免每次写都 stat

    void EnsureFileReady(const tm& nowtm);
    void OpenForDay(const tm& nowtm);
    void Close();
    std::size_t WriteUnlocked(const char* data, std::size_t len);
};
