#include "sequentiallogwriter.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

// 构造函数，初始化文件指针与日期状态
SequentialLogWriter::SequentialLogWriter()
    : fp_(nullptr), current_year_(-1), current_month_(-1),
      current_day_(-1), last_check_second_of_day_(-1) {}

// 析构函数，关闭文件并刷新缓冲区
SequentialLogWriter::~SequentialLogWriter() {
    Close();
}

// 追加一行日志，按天切分文件并在脏数据过多时自动刷新
void SequentialLogWriter::Append(const std::string& line, const tm& nowtm) {
    EnsureFileReady(nowtm); // 确保当前日志文件已准备好

    const char* data = line.data();
    std::size_t remaining = line.size();
    
    // 无锁写入，直接调用 fwrite_unlocked 以减少线程同步开销，循环直到所有数据写入完成
    while (remaining > 0) {
        const std::size_t written = WriteUnlocked(data, remaining); // 返回实际写入的字节数
        if (written == 0) { // 写入失败
            std::cerr << "Failed to append log file: " << std::strerror(errno) << std::endl;
            std::exit(EXIT_FAILURE);
        }
        data += written;    // 移动数据指针
        remaining -= written;   // 减少剩余字节数
    }
}

void SequentialLogWriter::Flush() {
    if (fp_ != nullptr) {
        fflush(fp_);    // 用户态缓冲区刷新到内核态，确保数据尽快落盘
    }
}

// 检查当前文件是否对应当前日期，并检测文件是否被外部删除
void SequentialLogWriter::EnsureFileReady(const tm& nowtm) {
    if (fp_ != nullptr && current_year_ == nowtm.tm_year &&
        current_month_ == nowtm.tm_mon && current_day_ == nowtm.tm_mday) {  // 如果日期未变，检查文件是否被外部删除
        
        // 同一秒内已检查过，跳过 stat 系统调用以保持性能
        int cur_sec = nowtm.tm_hour * 3600 + nowtm.tm_min * 60 + nowtm.tm_sec;
        if (cur_sec == last_check_second_of_day_) {
            return;
        }
        last_check_second_of_day_ = cur_sec;

        // 同一日期内，检测文件是否被外部删除或替换
        struct stat fd_st, path_st;
        int fd = fileno(fp_);
        if (fstat(fd, &fd_st) == 0) {
            if (stat(file_name_.c_str(), &path_st) != 0 ||
                fd_st.st_ino != path_st.st_ino) {
                OpenForDay(nowtm);
            }
        }
        return;
    }// 日期已变，切分新文件

    OpenForDay(nowtm);
}

// 关闭旧文件，打开以当天日期命名的新日志文件，并设置文件缓冲
void SequentialLogWriter::OpenForDay(const tm& nowtm) {
    Close();

    // 按 "YYYY-MM-DD-log.txt" 格式生成日志文件名
    char file_name_buf[64];
    snprintf(file_name_buf, sizeof(file_name_buf), "%d-%d-%d-log.txt",
             nowtm.tm_year + 1900, nowtm.tm_mon + 1, nowtm.tm_mday);
    file_name_ = file_name_buf;

    // 优先尝试以 "ae" 模式打开（支持 O_CLOEXEC 语义），失败则回退到 "a"
    fp_ = fopen(file_name_.c_str(), "ae");
    if (fp_ == nullptr) {
        fp_ = fopen(file_name_.c_str(), "a");
    }
    if (fp_ == nullptr) {
        std::cerr << "Failed to open log file: " << file_name_ << std::endl;
        std::exit(EXIT_FAILURE);
    }

    // _IOFBF:设置全缓冲模式，file_buffer_ 作为 FILE 内部缓冲区(用户缓冲区)，减少系统调用次数，当缓冲区满或调用 fflush 时才会写入内核缓冲区
    // 参数：文件指针, 缓冲区地址, 模式, 缓冲区大小
    if (setvbuf(fp_, file_buffer_.data(), _IOFBF, file_buffer_.size()) != 0) {
        std::cerr << "Failed to set log file buffer: " << file_name_ << std::endl;
        std::exit(EXIT_FAILURE);
    }

    current_year_ = nowtm.tm_year;
    current_month_ = nowtm.tm_mon;
    current_day_ = nowtm.tm_mday;
    last_check_second_of_day_ = -1;
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
    file_name_.clear();
    last_check_second_of_day_ = -1;
}

// 无锁写入，直接调用 fwrite_unlocked 以减少线程同步开销
std::size_t SequentialLogWriter::WriteUnlocked(const char* data, std::size_t len) {
    return fwrite_unlocked(data, 1, len, fp_);
}
