#include "sequentiallogwriter.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>

// 构造函数，初始化文件指针与日期状态
SequentialLogWriter::SequentialLogWriter()
    : fp_(nullptr), current_year_(-1), current_month_(-1),
      current_day_(-1), dirty_bytes_(0) {}

// 析构函数，关闭文件并刷新缓冲区
SequentialLogWriter::~SequentialLogWriter() {
    Close();
}

// 追加一行日志，按天切分文件并在脏数据过多时自动刷新
void SequentialLogWriter::Append(const std::string& line, const tm& nowtm) {
    EnsureFileReady(nowtm);

    const char* data = line.data();
    std::size_t remaining = line.size();
    while (remaining > 0) {
        const std::size_t written = WriteUnlocked(data, remaining);
        if (written == 0) {
            std::cerr << "Failed to append log file: " << std::strerror(errno) << std::endl;
            std::exit(EXIT_FAILURE);
        }
        data += written;
        remaining -= written;
    }

    dirty_bytes_ += line.size();
    if (dirty_bytes_ >= kFlushThresholdBytes) {
        Flush();
    }
}

// 刷新文件缓冲区到磁盘
void SequentialLogWriter::Flush() {
    if (fp_ != nullptr) {
        fflush(fp_);
        dirty_bytes_ = 0;
    }
}

// 检查当前文件是否对应当前日期，若不是则切换到当天文件
void SequentialLogWriter::EnsureFileReady(const tm& nowtm) {
    if (fp_ != nullptr && current_year_ == nowtm.tm_year &&
        current_month_ == nowtm.tm_mon && current_day_ == nowtm.tm_mday) {
        return;
    }

    OpenForDay(nowtm);
}

// 关闭旧文件，打开以当天日期命名的新日志文件，并设置文件缓冲
void SequentialLogWriter::OpenForDay(const tm& nowtm) {
    Close();

    // 按 "YYYY-MM-DD-log.txt" 格式生成日志文件名
    char file_name[64];
    snprintf(file_name, sizeof(file_name), "%d-%d-%d-log.txt",
             nowtm.tm_year + 1900, nowtm.tm_mon + 1, nowtm.tm_mday);

    // 优先尝试以 "ae" 模式打开（支持 O_CLOEXEC 语义），失败则回退到 "a"
    fp_ = fopen(file_name, "ae");
    if (fp_ == nullptr) {
        fp_ = fopen(file_name, "a");
    }
    if (fp_ == nullptr) {
        std::cerr << "Failed to open log file: " << file_name << std::endl;
        std::exit(EXIT_FAILURE);
    }

    // 设置全缓冲模式，复用内部缓冲区以减少系统调用
    if (setvbuf(fp_, file_buffer_.data(), _IOFBF, file_buffer_.size()) != 0) {
        std::cerr << "Failed to set log file buffer: " << file_name << std::endl;
        std::exit(EXIT_FAILURE);
    }

    current_year_ = nowtm.tm_year;
    current_month_ = nowtm.tm_mon;
    current_day_ = nowtm.tm_mday;
    dirty_bytes_ = 0;
}

// 关闭当前日志文件并清空日期状态
void SequentialLogWriter::Close() {
    if (fp_ != nullptr) {
        Flush();
        fclose(fp_);
        fp_ = nullptr;
    }

    current_year_ = -1;
    current_month_ = -1;
    current_day_ = -1;
    dirty_bytes_ = 0;
}

// 无锁写入，直接调用 fwrite_unlocked 以减少线程同步开销
std::size_t SequentialLogWriter::WriteUnlocked(const char* data, std::size_t len) {
    return fwrite_unlocked(data, 1, len, fp_);
}
