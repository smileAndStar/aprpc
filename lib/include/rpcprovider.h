#pragma once

#include "google/protobuf/service.h"
#include <unordered_map>
#include <boost/asio.hpp>

/**
 * @brief RpcProvider 用于发布rpc服务的网络对象类
 *        1.网络功能的封装（基于Boost.Asio库实现）
 *        2.NotifyService
 */
class RpcProvider {
    public:
        /**
         * @brief NotifyService 发布rpc服务接口
         * @param service 需要发布的服务对象
         */
        void NotifyService(google::protobuf::Service* service);

        /**
         * @brief Run 启动rpc服务节点，开始提供rpc远程网络调用服务  
         */
        void Run();

    private:
        boost::asio::io_context io_context_;    // Boost.Asio IO上下文对象

        /**
         * @brief ServiceInfo 保存服务对象和服务方法的结构体
         */
        struct ServiceInfo {
            google::protobuf::Service* service_;   // 保存服务对象
            std::unordered_map<std::string, const google::protobuf::MethodDescriptor*> methodMap_; // 保存服务方法
        };
        // 保存注册的服务对象和其服务方法的映射表
        std::unordered_map<std::string, ServiceInfo> serviceMap_;

        /**
         * @brief ASIO会话类
         */
        class Session : public std::enable_shared_from_this<Session> {
            public:
                /**
                 * @brief 构造函数
                 * @param socket TCP套接字
                 * @param provider RpcProvider对象引用
                 */
                Session(boost::asio::ip::tcp::socket socket, RpcProvider& provider)
                    : socket_(std::move(socket)),
                      provider_(provider) {}

                /**
                 * @brief 启动会话
                 */
                void Start();

                /**
                 * @brief 写入数据
                 * @param response 响应数据
                 */
                void DoWrite(const std::string& response);

            private:
                /**
                 * @brief 读取数据
                 */
                void DoRead();

                /**
                 * @brief 尝试从接收缓冲区中解析完整帧并处理，解决半包/粘包问题
                 */
                void TryParseAndHandle();

                boost::asio::ip::tcp::socket socket_;  // TCP套接字
                RpcProvider& provider_;                 // 引用RpcProvider对象
                std::vector<char> buffer_;               // 单次读取的临时缓冲区
                std::string recv_buffer_;                // 持久化接收缓冲区，跨多次读取累积数据
        };
        
        /**
         * @brief 处理请求
         * @param session 会话对象
         * @param request_data 请求数据
         */
        void HandleRequest(std::shared_ptr<Session> session, const std::string& request_data);

        /**
         * @brief 发送RPC响应（用于Closure回调）
         * @param session 会话对象
         * @param response 响应消息对象
         */
        void SendRpcResponse(std::shared_ptr<Session> session, google::protobuf::Message* response);
};