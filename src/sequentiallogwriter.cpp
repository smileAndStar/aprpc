/**
 * @file sequentiallogwriter.cpp
 * @brief 顺序写（sequential append）日志落盘后端
 *
 * ============================ 为什么顺序写更快 ============================
 * 相比"每条日志都 open/write/close 一次"的朴素实现，本实现省下了四处开销：
 *
 *  1. 免去反复打开关闭：文件只在首次写入、切换日期或检测到被外部删除时才
 *     重新打开（OpenForDay），其余时间一直复用同一个 FILE*。
 *  2. 用户态缓冲摊薄系统调用：OpenForDay 中通过 setvbuf 设置 8KB 全缓冲，
 *     绝大多数 Append 只是一次 memcpy 进用户缓冲区，不陷入内核；只有缓冲区
 *     写满或 Flush 时才真正触发 write(2)。
 *  3. 契合文件系统与磁盘的顺序访问特性：追加写使文件偏移单调增长，配合内核
 *     页缓存的预读与回写、块分配器的顺序分配，能减少随机寻道和元数据抖动。
 *  4. 免去 stdio 自身的加锁：写入方只有 Logger 的单后台线程，FILE* 不存在
 *     多线程竞争，因此可以安全使用 fwrite_unlocked，省掉 _IO_flockfile 的开销。
 *
 * ============================ 落盘的数据流与语义 ============================
 *
 *     业务/I-O 线程              Logger 后台线程                 内核              磁盘
 *     LOG_INFO -> 内存队列  ->  Append -> fwrite_unlocked  ->  write(2)  ->  fsync
 *                               (用户态 8KB 缓冲)        (page cache)     (介质)
 *
 * 注意 Flush() 只调用 fflush，语义是"把用户态缓冲交给内核 page cache"，
 * 并不等于数据已经写入磁盘。本类不提供 fsync，因此这是"异步批量落盘"，
 * 而不是"掉电不丢"的强持久化保证。
 */

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
