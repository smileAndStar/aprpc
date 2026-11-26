#pragma once

#include "google/protobuf/service.h"
#include <cstddef>
#include <deque>
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
         * @brief NotifyService 发布rpc服务接口,函数中会将服务对象
         *        和方法信息存储到serviceMap_中
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
                      provider_(provider),
                      strand_(boost::asio::make_strand(socket_.get_executor())) {}

                /**
                 * @brief 启动会话
                 */
                void Start();

                /**
                 * @brief 将完整响应加入会话发送队列，保持连接供后续请求复用。
                 * @param response 含长度前缀的响应帧；值参数由队列持有到发送完成。
                 * @note 可由 RPC 完成线程调用；通过 strand 串行访问 socket 与队列。
                 * @note 非阻塞；分配或投递失败可抛出异常，I/O 失败则关闭会话。
                 */
                void DoWrite(std::string response);

            private:
                /**
                 * @brief 读取数据
                 */
                void DoRead();

                /**
                 * @brief 尝试从接收缓冲区中解析完整帧并处理，解决半包/粘包问题
                 */
                void TryParseAndHandle();

                /**
                 * @brief 压缩接收缓冲区：回收读游标之前已消费的字节（延迟压缩）
                 *
                 * 正常消费路径只移动读游标（recv_offset_），单帧推进为 O(1)、不搬移数据；
                 * 本函数由 TryParseAndHandle 在解析推进不下去时调用一次，通过触发门槛
                 * （整个缓冲区已消费完、已消费前缀达到阈值、已消费前缀不少于剩余数据）
                 * 减少对同一段粘包残留的重复搬移。
                 *
                 * @note erase 本身是 O(remaining)，其代价至多按已消费字节数摊还，
                 *       因此本函数不提供按帧数（尤其是变长消息）的 O(1) 保证。
                 */
                void CompactRecvBuffer();

                // 仅在 strand 上调用；同一连接始终只有一个 async_write 在途。
                void DoWriteNext();

                boost::asio::ip::tcp::socket socket_;  // TCP套接字
                RpcProvider& provider_;                 // 引用RpcProvider对象
                boost::asio::strand<boost::asio::any_io_executor> strand_;
                std::deque<std::shared_ptr<std::string>> write_queue_; ///< 由 strand 保护的完整帧队列。
                std::vector<char> buffer_;               // 单次读取的临时缓冲区
                std::string recv_buffer_;                // 持久化接收缓冲区，跨多次读取累积数据
                std::size_t recv_offset_ = 0;            // 读游标：recv_buffer_ 中已消费字节数（未压缩前缀）
        };
        
        /**
         * @brief 处理请求
         * @param session 会话对象
         * @param service_name 服务名称
         * @param method_name 方法名称
         * @param args_data 序列化后的参数数据首地址（指向接收缓冲区，不拷贝）
         * @param args_size 参数数据长度
         *
         * @note args_data 只保证在本函数同步执行期间有效：protobuf 的 ParseFromArray
         *       会立即把数据反序列化进 message 对象，因此调用方无需再复制一份 args。
         */
        void HandleRequest(std::shared_ptr<Session> session,
                           const std::string& service_name,
                           const std::string& method_name,
                           const char* args_data,
                           std::size_t args_size);

        /**
         * @brief 发送RPC响应（用于Closure回调）
         * @param session 会话对象
         * @param response 响应消息对象
         */
        void SendRpcResponse(std::shared_ptr<Session> session, google::protobuf::Message* response);
};