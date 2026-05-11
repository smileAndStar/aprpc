#pragma once

#include "logwriter.h"

#include <string>
#include <cstddef>
#include <time.h>

/**
 * @class MmapLogWriter
 * @brief 基于内存映射（mmap）的高性能日志落盘写入器
 * @details 该类负责管理日志文件的内存映射，支持按天切分日志文件，并在内存中累积日志行以减少磁盘写入次数。
 * 它会根据当前时间自动切换日志文件，并确保在写入日志行时有足够的映射空间。
 * 如果累积的未同步字节数超过阈值，会强制异步同步到磁盘以保证数据安全。
 * 专门处理日志字符串的文件映射和落盘逻辑。
 */
class MmapLogWriter final : public LogWriter {
public:
    MmapLogWriter();
    ~MmapLogWriter() override;

    /**
     * @brief 向文件尾部追加日志行
     * @param line 日志字符串
     * @param nowtm 当前时间，用于按天切分文件
     */
    void Append(const std::string& line, const tm& nowtm) override;

private:
    static constexpr std::size_t kMapGrowStepBytes = 1 * 1024 * 1024;  ///< 每次扩展映射的最小字节数(1MB)
    static constexpr std::size_t kMinMapBytes = 1 * 1024 * 1024;   ///< 映射的最小初始大小(1MB)
    static constexpr std::size_t kSyncThresholdBytes = 256 * 1024;  ///< 累积多少字节未同步时强制异步同步到磁盘(256KB)

    int fd_;    // 文件描述符
    void* map_ptr_; // 映射的内存指针
    std::size_t map_size_;  // 当前映射的大小
    std::size_t write_offset_;  // 当前写入偏移，表示已使用的字节数
    int current_year_;  // 当前映射的日志文件对应的年份
    int current_month_; // 当前映射的日志文件对应的月份
    int current_day_;   // 当前映射的日志文件对应的日期
    std::size_t dirty_bytes_;   // 累积的未同步字节数
    std::string file_name_;     // 当前日志文件名，用于检测文件是否被外部删除
    int last_check_second_of_day_;  // 上次检测文件的秒级时间戳，用于节流避免每次写都 stat

    /**
     * @brief 将字节数对齐到页面边界
     * 为什么要对齐到页面边界？
     * 因为内存映射通常以页面为单位进行管理，对齐可以提高性能并减少碎片。
     * @param n 字节数
     * @return 对齐后的字节数
     */
    std::size_t AlignToPage(std::size_t n); 

    /**
     * @brief 确保文件准备就绪
     * 判断当前映射的文件是否是当天的文件，如果不是则切换到当天的文件。
     * @param nowtm 当前时间
     */
    void EnsureFileReady(const tm& nowtm);

    /**
     * @brief 打开当天的日志文件并建立映射
     * @param nowtm 当前时间
     */
    void OpenForDay(const tm& nowtm);

    /**
     * @brief 确保映射有足够的空间容纳即将写入的日志行
     * @param additional_bytes 即将写入的日志行的字节数
     */
    void EnsureCapacity(std::size_t additional_bytes);

    /**
     * @brief 关闭当前映射
     */
    void Close();
};
