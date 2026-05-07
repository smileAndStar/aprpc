#include "rpcapplication.h"
#include "rpcconfig.h"
#include "rpcheader.pb.h"
#include "user.pb.h"

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct BenchResult {
    uint64_t ops = 0;
    uint64_t errors = 0;
    uint64_t total_ns = 0;
    uint64_t max_ns = 0;
    std::vector<uint64_t> latencies;
};

void PrintUsage(const char* prog) {
    std::cout
        << "Usage: " << prog << " -i config.yaml [threads] [duration_sec] [service]\n"
        << "  -i config.yaml     : RPC config file (required, server IP/port)\n"
        << "  threads            : concurrent client threads, default 8\n"
        << "  duration_sec       : benchmark duration in seconds, default 10\n"
        << "  service            : login | register, default login\n";
}

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

bool BuildLoginFrame(std::string& frame) {
    fixbug::LoginRequest req;
    req.set_username("bench");
    req.set_password("bench");

    std::string args_str;
    if (!req.SerializeToString(&args_str)) {
        return false;
    }

    rpcheader::RpcHeader rpc_header;
    rpc_header.set_service_name("UserServiceRPC");
    rpc_header.set_method_name("Login");
    rpc_header.set_args_size(args_str.size());

    std::string header_str;
    if (!rpc_header.SerializeToString(&header_str)) {
        return false;
    }

    uint32_t header_size_net = htonl(static_cast<uint32_t>(header_str.size()));
    frame.clear();
    frame.append(reinterpret_cast<const char*>(&header_size_net), 4);
    frame.append(header_str);
    frame.append(args_str);
    return true;
}

bool BuildRegisterFrame(std::string& frame) {
    fixbug::RegisterRequest req;
    req.set_id(1);
    req.set_username("bench");
    req.set_password("bench");

    std::string args_str;
    if (!req.SerializeToString(&args_str)) {
        return false;
    }

    rpcheader::RpcHeader rpc_header;
    rpc_header.set_service_name("UserServiceRPC");
    rpc_header.set_method_name("Register");
    rpc_header.set_args_size(args_str.size());

    std::string header_str;
    if (!rpc_header.SerializeToString(&header_str)) {
        return false;
    }

    uint32_t header_size_net = htonl(static_cast<uint32_t>(header_str.size()));
    frame.clear();
    frame.append(reinterpret_cast<const char*>(&header_size_net), 4);
    frame.append(header_str);
    frame.append(args_str);
    return true;
}

bool VerifyLoginResponse(const std::vector<char>& body, uint32_t len) {
    fixbug::LoginResponse resp;
    if (!resp.ParseFromArray(body.data(), static_cast<int>(len))) {
        return false;
    }
    return resp.result().errcode() == 0;
}

bool VerifyRegisterResponse(const std::vector<char>& body, uint32_t len) {
    fixbug::RegisterResponse resp;
    if (!resp.ParseFromArray(body.data(), static_cast<int>(len))) {
        return false;
    }
    return resp.result().errcode() == 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        PrintUsage(argv[0]);
        return 0;
    }

    RpcApplication::Init(argc, argv);

    const std::string server_ip = RpcApplication::GetConfig().Load<std::string>("rpc.server_ip");
    const uint16_t server_port = static_cast<uint16_t>(
        RpcApplication::GetConfig().Load<int>("rpc.server_port"));

    const int threads = ParsePositiveInt(argc > optind ? argv[optind] : nullptr, 8);
    const int duration_seconds =
        ParsePositiveInt(argc > optind + 1 ? argv[optind + 1] : nullptr, 10);
    const std::string service = argc > optind + 2 ? argv[optind + 2] : "login";

    bool is_login;
    if (service == "login") {
        is_login = true;
    } else if (service == "register") {
        is_login = false;
    } else {
        std::cerr << "Invalid service: " << service << " (expected login or register)\n";
        PrintUsage(argv[0]);
        return 1;
    }

    // 构造一条完整的 RPC 请求帧，所有线程复用同一份数据
    std::string request_frame;
    bool (*verify_response)(const std::vector<char>&, uint32_t);
    if (is_login) {
        if (!BuildLoginFrame(request_frame)) {
            std::cerr << "Failed to build Login request frame\n";
            return 1;
        }
        verify_response = VerifyLoginResponse;
    } else {
        if (!BuildRegisterFrame(request_frame)) {
            std::cerr << "Failed to build Register request frame\n";
            return 1;
        }
        verify_response = VerifyRegisterResponse;
    }

    std::cout << "rpc_bench start: threads=" << threads
              << ", duration=" << duration_seconds << "s"
              << ", service=" << (is_login ? "Login" : "Register")
              << ", server=" << server_ip << ":" << server_port << "\n";

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::vector<BenchResult> results(static_cast<size_t>(threads));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(threads));

    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&, i]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            BenchResult local;
            constexpr std::size_t kLatencyReserve = 100000;
            local.latencies.reserve(kLatencyReserve);

            while (!stop.load(std::memory_order_relaxed)) {
                auto t1 = std::chrono::steady_clock::now();

                bool ok = false;
                try {
                    boost::asio::io_context ioc;
                    boost::asio::ip::tcp::socket sock(ioc);
                    boost::asio::ip::tcp::resolver resolver(ioc);
                    boost::asio::connect(
                        sock, resolver.resolve(server_ip, std::to_string(server_port)));

                    boost::asio::write(sock, boost::asio::buffer(request_frame));

                    uint32_t resp_len_net = 0;
                    boost::asio::read(
                        sock, boost::asio::buffer(&resp_len_net, sizeof(resp_len_net)));
                    uint32_t resp_len = ntohl(resp_len_net);

                    std::vector<char> resp_body(resp_len);
                    boost::asio::read(sock, boost::asio::buffer(resp_body));

                    ok = verify_response(resp_body, resp_len);
                } catch (const std::exception&) {
                    ok = false;
                }

                auto t2 = std::chrono::steady_clock::now();
                uint64_t ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());

                local.ops += 1;
                local.total_ns += ns;
                local.max_ns = std::max(local.max_ns, ns);
                if (local.latencies.size() < kLatencyReserve) {
                    local.latencies.push_back(ns);
                }
                if (!ok) {
                    local.errors += 1;
                }
            }
            results[static_cast<size_t>(i)] = std::move(local);
        });
    }

    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
    stop.store(true, std::memory_order_release);

    for (auto& t : workers) {
        t.join();
    }
    const auto end = std::chrono::steady_clock::now();

    // 汇总统计
    uint64_t total_ops = 0;
    uint64_t total_errors = 0;
    uint64_t total_ns = 0;
    uint64_t max_ns = 0;
    std::vector<uint64_t> all_latencies;

    for (auto& r : results) {
        total_ops += r.ops;
        total_errors += r.errors;
        total_ns += r.total_ns;
        max_ns = std::max(max_ns, r.max_ns);
        all_latencies.insert(all_latencies.end(), r.latencies.begin(), r.latencies.end());
    }

    const double elapsed_seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - begin).count();
    const double throughput =
        (elapsed_seconds > 0.0) ? (static_cast<double>(total_ops) / elapsed_seconds) : 0.0;
    const double avg_us = (total_ops > 0) ? (static_cast<double>(total_ns) / static_cast<double>(total_ops) / 1000.0) : 0.0;

    // 计算分位数
    double p50_us = 0.0, p99_us = 0.0, p999_us = 0.0;
    if (!all_latencies.empty()) {
        std::sort(all_latencies.begin(), all_latencies.end());
        auto percentile = [&](double p) -> double {
            std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(all_latencies.size()));
            if (idx >= all_latencies.size()) {
                idx = all_latencies.size() - 1;
            }
            return static_cast<double>(all_latencies[idx]) / 1000.0;
        };
        p50_us = percentile(0.50);
        p99_us = percentile(0.99);
        p999_us = percentile(0.999);
    }

    std::cout << std::fixed << std::setprecision(2)
              << "elapsed=" << elapsed_seconds << "s"
              << ", total_reqs=" << total_ops
              << ", errors=" << total_errors
              << ", reqs/s=" << throughput << "\n"
              << "latency(us): avg=" << avg_us
              << ", p50=" << p50_us
              << ", p99=" << p99_us
              << ", p999=" << p999_us
              << ", max=" << (static_cast<double>(max_ns) / 1000.0)
              << "\n";

    std::cout << "Tip: vary thread count to find the provider saturation point.\n";
    std::cout.flush();
    std::fflush(nullptr);

    return 0;
}
