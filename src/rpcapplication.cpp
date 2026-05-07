#include <iostream>
#include <exception>
#include <unistd.h>
#include "rpcapplication.h"
#include "logger.h"

RpcConfig RpcApplication::m_config;

void ShowArgsHelp() {
    std::cout << "Usage: command [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -i,  configfile name" << std::endl;
}

void RpcApplication::Init(int argc, char* argv[]) {
    if (argc < 2) {
        ShowArgsHelp();
        exit(EXIT_FAILURE);
    }

    int opt = 0;
    std::string config_file;
    while((opt = getopt(argc, argv, "i:")) != -1) {
        switch (opt) {
            case 'i':
                config_file = optarg;
                break;
            
            case '?':
                // getopt 遇到未知选项或缺少参数时返回 '?'
                ShowArgsHelp();
                exit(EXIT_FAILURE);

            default:
                break;
        }
    }

    // 检查是否提供了配置文件
    if (config_file.empty()) {
        std::cerr << "Error: config file not provided\n" << std::endl;
        ShowArgsHelp();
        exit(EXIT_FAILURE);
    }

    // 加载配置文件
    m_config.LoadConfigFile(config_file);
    
    // 配置加载完成后再初始化日志系统，这样才能根据配置切换落盘后端
    Logger& logger = Logger::GetInstance();
    try {
        std::string write_mode = m_config.Load<std::string>("logger.write_mode");
        if (!logger.SetWriteModeByName(write_mode)) {
            std::cerr << "Warning: invalid logger.write_mode '" << write_mode
                      << "', fallback to mmap" << std::endl;
        }
    } catch (const std::exception&) {
        // logger.write_mode 是可选配置，缺省时保持默认 mmap
    }

    std::cout << "RPC Server will start at " << m_config.Load<std::string>("rpc.server_ip")
                                      << ":" << m_config.Load<int>("rpc.server_port") << "\n"
                 << "ZooKeeper address is " << m_config.Load<std::string>("zookeeper.server_ip") 
                                      << ":" << m_config.Load<int>("zookeeper.server_port") << "\n"
                 << "Logger write mode is " << logger.GetWriteModeName() << std::endl;
}
    
RpcApplication& RpcApplication::GetInstance() {
    static RpcApplication instance; // 线程安全，只会创建一次(静态变量)
    return instance;
}

RpcConfig& RpcApplication::GetConfig() {
    return m_config;
}
