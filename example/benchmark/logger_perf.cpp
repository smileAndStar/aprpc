#include "logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

// 单个压测线程的统计信息
struct ThreadStats {
    uint64_t ops = 0;       // 总日志调用次数
    uint64_t total_ns = 0;  // 总耗时（纳秒）
    uint64_t max_ns = 0;    // 单次最大耗时（纳秒）
};

// 解析正整数参数，解析失败或为空时返回默认值
int ParsePositiveInt(const char* value, int default_value) {
    if (value == nullptr) {
        return default_value;
    }
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

// 打印命令行用法说明
void PrintUsage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [threads] [duration_seconds] [message_size] [writer_mode]\n"
        << "  threads           : producer thread count, default 8\n"
        << "  duration_seconds  : benchmark duration, default 10\n"
        << "  message_size      : log message bytes, default 128\n"
        << "  writer_mode       : mmap | sequential, default sequential\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        PrintUsage(argv[0]);
        return 0;
    }

    // 解析命令行参数：线程数、压测时长、消息大小、落盘模式
    const int threads = ParsePositiveInt(argc > 1 ? argv[1] : nullptr, 8);
    const int duration_seconds = ParsePositiveInt(argc > 2 ? argv[2] : nullptr, 10);
    const int message_size = ParsePositiveInt(argc > 3 ? argv[3] : nullptr, 128);
    const std::string writer_mode = argc > 4 ? argv[4] : "sequential";

    // 初始化日志系统并切换到指定的落盘后端
    Logger& logger = Logger::GetInstance();
    if (!logger.SetWriteModeByName(writer_mode)) {
        std::cerr << "Invalid writer_mode: " << writer_mode << "\n";
        PrintUsage(argv[0]);
        return 1;
    }

    std::cout << "logger_perf start: threads=" << threads
              << ", duration=" << duration_seconds << "s"
              << ", message_size=" << message_size << " bytes"
              << ", writer_mode=" << logger.GetWriteModeName() << "\n";

    logger.SetLogLevel(INFO);

    // 构造固定长度的日志负载内容
    std::string payload(static_cast<size_t>(message_size), 'x');
    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::vector<ThreadStats> stats(static_cast<size_t>(threads));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(threads));

    // 启动压测工作线程，等待统一信号后开始循环写入日志
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&start, &stop, &stats, i, &payload]() {
            // 自旋等待全局开始信号，确保所有线程尽量同时启动
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            // 循环写入日志并统计每次调用的延迟
            ThreadStats local;
            while (!stop.load(std::memory_order_relaxed)) {
                auto t1 = std::chrono::steady_clock::now();
                Logger::GetInstance().Log(payload);
                auto t2 = std::chrono::steady_clock::now();

                uint64_t ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
                local.ops += 1;
                local.total_ns += ns;
                local.max_ns = std::max(local.max_ns, ns);
            }
            stats[static_cast<size_t>(i)] = local;
        });
    }

    // 主线程：发送开始信号，睡眠指定时长后发送停止信号
    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
    stop.store(true, std::memory_order_release);

    // 等待所有工作线程结束
    for (auto& t : workers) {
        t.join();
    }
    const auto end = std::chrono::steady_clock::now();

    // 汇总所有线程的统计结果
    uint64_t total_ops = 0;
    uint64_t total_ns = 0;
    uint64_t max_ns = 0;

    for (const auto& s : stats) {
        total_ops += s.ops;
        total_ns += s.total_ns;
        max_ns = std::max(max_ns, s.max_ns);
    }

    // 计算吞吐量与平均/最大延迟
    const double elapsed_seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - begin).count();
    const double throughput = (elapsed_seconds > 0.0)
                                  ? (static_cast<double>(total_ops) / elapsed_seconds)
                                  : 0.0;
    const double avg_call_us = (total_ops > 0)
                                   ? (static_cast<double>(total_ns) / static_cast<double>(total_ops) / 1000.0)
                                   : 0.0;

    // 输出压测结果
    std::cout << std::fixed << std::setprecision(2)
              << "elapsed_s=" << elapsed_seconds
              << ", total_logs=" << total_ops
              << ", logs_per_s=" << throughput
              << ", avg_log_call_us=" << avg_call_us
              << ", max_log_call_us=" << (static_cast<double>(max_ns) / 1000.0)
              << "\n";

    std::cout << "Tip: increase threads and compare logs_per_s / avg_log_call_us.\n";
    std::cout.flush();
    std::fflush(nullptr);

    return 0;
}
