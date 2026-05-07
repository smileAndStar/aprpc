#include "mmaplogwriter.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

MmapLogWriter::MmapLogWriter()
    : fd_(-1), map_ptr_(nullptr), map_size_(0), write_offset_(0),
      current_year_(-1), current_month_(-1), current_day_(-1), dirty_bytes_(0) {}

MmapLogWriter::~MmapLogWriter() {
    Close();
}

void MmapLogWriter::Append(const std::string& line, const tm& nowtm) {
    EnsureFileReady(nowtm); // 确保当前映射的文件是当天的文件，如果不是则切换到当天的文件
    EnsureCapacity(line.size());    // 确保映射有足够的空间容纳即将写入的日志行，如果不够则扩展映射 

    // 将日志行复制到映射的内存中，并更新写入偏移和未同步字节数
    std::memcpy(static_cast<char*>(map_ptr_) + write_offset_, line.data(), line.size());
    write_offset_ += line.size();
    dirty_bytes_ += line.size();

    if (dirty_bytes_ >= kSyncThresholdBytes) {
        msync(map_ptr_, write_offset_, MS_ASYNC);   // 异步同步到磁盘，减少频繁同步带来的性能影响
        dirty_bytes_ = 0;
    }
}

// 将字节数对齐到页面边界
// 例如，如果页面大小是 4096 字节，那么 AlignToPage(5000) 将返回 8192，因为 8192 是大于等于 5000 的最小的 4096 的倍数。
std::size_t MmapLogWriter::AlignToPage(std::size_t n) {
    // 获取系统页面大小
    long page = sysconf(_SC_PAGESIZE);  
    // 如果获取失败，使用默认页面大小(4096字节)
    const std::size_t page_size = page > 0 ? static_cast<std::size_t>(page) : 4096;  
    // 将 n 向上对齐到 page_size 的倍数
    return ((n + page_size - 1) / page_size) * page_size;   
}

void MmapLogWriter::EnsureFileReady(const tm& nowtm) {
    // 如果当前映射的文件已经是当天的文件，则直接返回
    if (fd_ != -1 && current_year_ == nowtm.tm_year &&
        current_month_ == nowtm.tm_mon && current_day_ == nowtm.tm_mday) {
        return;
    }

    // 否则需要切换到当天的文件
    OpenForDay(nowtm);
}

void MmapLogWriter::OpenForDay(const tm& nowtm) {
    Close();    // 先关闭之前的文件和映射

    // 文件名，格式："YYYY-MM-DD-log.txt"
    char file_name[64]; 
    snprintf(file_name, sizeof(file_name), "%d-%d-%d-log.txt",
             nowtm.tm_year + 1900, nowtm.tm_mon + 1, nowtm.tm_mday);

    // 以读写方式打开文件，如果不存在则创建，权限为644
    fd_ = open(file_name, O_RDWR | O_CREAT, 0644);  
    if (fd_ == -1) {
        std::cerr << "Failed to open log file: " << file_name << std::endl;
        std::exit(EXIT_FAILURE);
    }

    struct stat st; // 获取文件状态，主要是为了获取当前文件大小
    if (fstat(fd_, &st) == -1) {
        std::cerr << "Failed to stat log file: " << file_name << std::endl;
        std::exit(EXIT_FAILURE);
    }

    write_offset_ = static_cast<std::size_t>(st.st_size);   // 从文件末尾开始写入
    map_size_ = AlignToPage(write_offset_ + kMapGrowStepBytes); // 初始映射大小至少要能容纳当前文件内容加上一次扩展的空间
    if (map_size_ < kMinMapBytes) { // 确保映射的最小初始大小
        map_size_ = AlignToPage(kMinMapBytes);
    }

    // 扩展文件大小以适应映射大小，如果文件已经足够大则 ftruncate 不会缩小文件
    if (ftruncate(fd_, static_cast<off_t>(map_size_)) == -1) {
        std::cerr << "Failed to resize log file: " << file_name << std::endl;
        std::exit(EXIT_FAILURE);
    }

    // 建立内存映射，允许读写，映射共享（写入会反映到文件）
    map_ptr_ = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (map_ptr_ == MAP_FAILED) {
        map_ptr_ = nullptr;
        std::cerr << "Failed to mmap log file: " << file_name << std::endl;
        std::exit(EXIT_FAILURE);
    }

    current_year_ = nowtm.tm_year;
    current_month_ = nowtm.tm_mon;
    current_day_ = nowtm.tm_mday;
    dirty_bytes_ = 0;
}

void MmapLogWriter::EnsureCapacity(std::size_t additional_bytes) {
    if (write_offset_ + additional_bytes <= map_size_) {    // 当前映射有足够空间，无需扩展
        return;
    }

    std::size_t new_map_size = map_size_;
    // 不断增加映射大小，直到能够容纳即将写入的日志行
    while (write_offset_ + additional_bytes > new_map_size) {   
        new_map_size += kMapGrowStepBytes;
    }
    new_map_size = AlignToPage(new_map_size);

    // 先将当前映射的内容异步同步到磁盘，确保数据不丢失
    msync(map_ptr_, write_offset_, MS_ASYNC);
    if (munmap(map_ptr_, map_size_) == -1) {
        std::cerr << "Failed to unmap log file" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    // 扩展文件大小以适应新的映射大小
    if (ftruncate(fd_, static_cast<off_t>(new_map_size)) == -1) {
        std::cerr << "Failed to grow log file" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    // 重新建立映射
    map_ptr_ = mmap(nullptr, new_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (map_ptr_ == MAP_FAILED) {
        map_ptr_ = nullptr;
        std::cerr << "Failed to remap log file" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    map_size_ = new_map_size;
}

void MmapLogWriter::Close() {
    if (map_ptr_ != nullptr) {
        msync(map_ptr_, write_offset_, MS_SYNC);    // 先强制同步到磁盘，确保数据不丢失
        if (munmap(map_ptr_, map_size_) == -1) {    // 解除内存映射
            std::cerr << "Failed to unmap log file" << std::endl;
        }
        map_ptr_ = nullptr;
    }

    if (fd_ != -1) {
        // 截断到实际写入长度，避免预分配空间留在文件尾部
        ftruncate(fd_, static_cast<off_t>(write_offset_));
        close(fd_);
        fd_ = -1;
    }

    map_size_ = 0;
    write_offset_ = 0;
    current_year_ = -1;
    current_month_ = -1;
    current_day_ = -1;
    dirty_bytes_ = 0;
}
