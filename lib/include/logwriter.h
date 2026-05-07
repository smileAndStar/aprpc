#pragma once

#include <string>
#include <time.h>

/**
 * @brief 日志落盘写入器抽象接口
 *
 * Logger 后台线程只依赖该接口，具体可以切到 mmap 或普通顺序写实现。
 */
class LogWriter {
public:
    virtual ~LogWriter() = default;

    /**
     * @brief 向日志文件追加一行文本
     * @param line 已完成格式化的日志文本
     * @param nowtm 当前时间，用于按天切分文件
     */
    virtual void Append(const std::string& line, const tm& nowtm) = 0;

    /**
     * @brief 刷新写入器内部缓冲
     *
     * 顺序写后端需要主动 fflush；mmap 后端默认无需额外操作。
     */
    virtual void Flush() {}
};
