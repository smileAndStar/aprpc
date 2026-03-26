#include "rpcprovider.h"
#include "rpcapplication.h"
#include <google/protobuf/descriptor.h>
#include "rpcheader.pb.h"
#include "logger.h"
#include "zookeeperutil.h"

void RpcProvider::NotifyService(google::protobuf::Service* service) {
    ServiceInfo serviceInfo;    // 创建服务信息对象

    // 获取服务对象的描述信息
    const google::protobuf::ServiceDescriptor *pserviceDesc = service->GetDescriptor();
    // 获取服务的名字
    std::string service_name = pserviceDesc->name();
    // 获取服务对象service的方法的数量
    int methodCnt = pserviceDesc->method_count();

    LOG_INFO("NotifyService: service_name=%s", service_name.c_str());
    
    // 填充serviceInfo对象
    serviceInfo.service_ = service;         // 保存服务对象本身
    for (int i = 0; i < methodCnt; ++i) {   // 遍历服务对象的所有方法
        // 获取了服务对象指定下标的服务方法的描述符
        const google::protobuf::MethodDescriptor *pmethodDesc = pserviceDesc->method(i);
        std::string method_name = pmethodDesc->name();

        LOG_INFO("NotifyService: method_name=%s", method_name.c_str());

        // 存储服务方法名称和方法描述符的映射
        serviceInfo.methodMap_.insert({method_name, pmethodDesc});
    }

    // 存储服务名称和服务信息的映射
    serviceMap_.insert({service_name, serviceInfo});
}

void RpcProvider::Run() {
    // 启动rpc服务节点，开始提供rpc远程网络调用服务

    std::string ip = RpcApplication::GetConfig().Load<std::string>("rpc.server_ip");
    uint16_t port = RpcApplication::GetConfig().Load<int>("rpc.server_port");

    try {
        // 创建Acceptor对象，监听指定的IP和端口
        boost::asio::ip::tcp::acceptor acceptor(
            io_context_, boost::asio::ip::tcp::endpoint(
                boost::asio::ip::make_address(ip), port
            ) 
        );

        LOG_INFO("RpcProvider::Run rpc server start at %s:%d", ip.c_str(), port);

        // == 异步接受连接 == 
        /**
         * @note 运行流程
         * 1. do_accept() 被调用一次，注册 async_accept 回调函数
         * 2. 在线程池中运行 io_context_.run()，处理所有异步事件
         *   在 do_accept() 中：
            * 1. 注册 一个 async_accept --> 接受连接的异步回调函数
            * 2. 立刻返回（没有阻塞）
            * 3. 某个时刻：有客户端连接，Asio 调用 accept 回调
            * 4. 回调中：创建 Session，启动读写(处理rpc并响应)，然后再次调用 do_accept()
            * 5. 再次注册下一个 async_accept
            * 6. 循环往复
         */

        // 封装一个递归lambda函数用于接受连接
        std::function<void()> do_accept = [&, this]() {
            acceptor.async_accept(  // async_accept ：注册异步接受连接回调(不阻塞)，立即返回
                // Tip. acceptor.async_accept(/* handler */); 
                // 注册一次 async_accept 后，io_context_.run() 会监听该事件，当有连接到来时，调用该回调函数
                // 注意只能接受一次连接，想要持续接受连接，必须递归调用
                [&, this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
                    if (!ec) {  // 如果接受连接没有错误
                        // 创建一个新的session会话来处理连接
                        auto session = std::make_shared<Session>(std::move(socket), *this);
                        session->Start();  // 启动会话
                    }
                    do_accept(); // 继续接受下一个连接
                }
            );
        };
        do_accept(); // 在启动线程前调用一次，开始接受连接


        // 把当前rpc节点上要发布的服务全部注册到zk上面，让rpc client可以从zk上发现服务
        // session timeout   30s     zkclient 网络I/O线程  1/3 * timeout 时间发送ping心跳
        ZkClient zkCli;
        zkCli.Start();
        // service_name为永久性节点    method_name为临时性节点
        for (auto &sp : serviceMap_) 
        {
            // /service_name   /UserServiceRpc
            std::string service_path = "/" + sp.first;
            zkCli.Create(service_path.c_str(), nullptr, 0);
            for (auto &mp : sp.second.methodMap_) 
            {
                // /service_name/method_name   /UserServiceRpc/Login
                std::string method_path = service_path + "/" + mp.first;    // 根据服务名称和方法名称构建zk路径
                char method_path_data[128] = {0};
                sprintf(method_path_data, "%s:%d", ip.c_str(), port);   // 在zk节点上存储rpc服务的网络地址，格式：ip:port
                // ZOO_EPHEMERAL    临时性节点
                zkCli.Create(method_path.c_str(), method_path_data, strlen(method_path_data), ZOO_EPHEMERAL);
            }
        }

        // === 线程池 ===
        /* 为什么几个线程能支撑上万个连接？
         * 核心原因: 线程不被 I/O 阻塞
         * 线程只在做两件事：
            1.等 epoll
            2.执行 handler（业务逻辑）
         */
        std::vector<std::thread> threads;
        for (int i = 0; i < 6; ++i) {
            threads.emplace_back([this]() { io_context_.run(); });  // 运行io_context_.run(), 处理异步事件
        }

        for (auto& thread : threads) {
            thread.join();  // 等待所有线程完成
        }
    } catch (std::exception& e) {
        LOG_ERR("RpcProvider::Run exception: %s", e.what());
    }
}

// Session实现
void RpcProvider::Session::Start() {
    DoRead(); 
}

void RpcProvider::Session::DoRead() {
    std::shared_ptr<RpcProvider::Session> self(shared_from_this());  // 获取shared_ptr指向当前对象的指针，保证对象在异步操作期间存活！
    buffer_.resize(4096);  // 调整单次读取的临时缓冲区大小

    socket_.async_read_some(    // 异步读取数据，不会阻塞，当有数据到达时调用回调函数
        boost::asio::buffer(buffer_), 
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                recv_buffer_.append(buffer_.data(), length);  // 追加到持久化缓冲区
                TryParseAndHandle();                          // 尝试解析完整帧
            }
        }
    );
}

void RpcProvider::Session::TryParseAndHandle() {
    // 循环处理：一次 recv 可能包含多条完整消息（粘包），也可能不足一条（半包）
    while (true) {
        // === 第一步：判断是否收到了完整的 4 字节 header_size ===
        if (recv_buffer_.size() < 4) {
            DoRead();  // 数据不足，继续等待
            return;
        }

        uint32_t header_size = 0;
        recv_buffer_.copy((char*)&header_size, 4, 0);
        header_size = ntohl(header_size);  // 网络字节序转主机字节序

        // === 第二步：判断是否收到了完整的 header ===
        if (recv_buffer_.size() < 4 + header_size) {
            DoRead();  // header 数据不足，继续等待
            return;
        }

        // 反序列化 header，取出 args_size
        std::string rpc_header_str = recv_buffer_.substr(4, header_size);
        rpcheader::RpcHeader rpcHeader;
        if (!rpcHeader.ParseFromString(rpc_header_str)) {
            LOG_ERR("Session::TryParseAndHandle parse rpc_header_str error, close connection!");
            recv_buffer_.clear();
            return;  // 协议错误，丢弃数据，连接将在 Session 析构时关闭
        }
        uint32_t args_size = rpcHeader.args_size();

        // === 第三步：判断是否收到了完整的 args ===
        if (recv_buffer_.size() < 4 + header_size + args_size) {
            DoRead();  // args 数据不足，继续等待
            return;
        }

        // === 第四步：取出完整的一帧数据，从缓冲区移除 ===
        std::string request_data = recv_buffer_.substr(0, 4 + header_size + args_size);
        recv_buffer_.erase(0, 4 + header_size + args_size);  // 已消费，移除

        provider_.HandleRequest(shared_from_this(), request_data);
        // 继续循环，处理 recv_buffer_ 中可能残留的下一条完整消息（解决粘包）
    }
}

void RpcProvider::Session::DoWrite(const std::string& response) {
    std::shared_ptr<RpcProvider::Session> self(shared_from_this());  // 获取shared_ptr指向当前对象的指针，保证对象在异步操作期间存活！

    // 异步写入数据，不会阻塞，当数据发送完成时调用回调函数
    boost::asio::async_write(
        socket_,
        boost::asio::buffer(response),
        [this, self](boost::system::error_code ec, std::size_t /*length*/) {    // 发送完成回调
            if (!ec) {
                // 关闭连接
                boost::system::error_code ignored_ec;   // 忽略错误码
                socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
            }
        }
    );
}

void RpcProvider::HandleRequest(std::shared_ptr<Session> session, const std::string& request_data) {
    /**
     * @note 第一步：读取远程 rpc调用请求的字符流
     * 
     * 字符流包含的信息：
     * 1.数据头的长度 header_size
     * 2.数据头 rpc_header_str
     * 3.请求参数 args_str
     */

    // 从字符流中读取数据头的长度信息
    uint32_t header_size = 0;
    request_data.copy((char*)&header_size, 4, 0);
    header_size = ntohl(header_size);  // 网络字节序转主机字节序

    // 根据header_size读取数据头字符串,并反序列化数据
    std::string rpc_header_str = request_data.substr(4, header_size);
    rpcheader::RpcHeader rpcHeader;
    if (rpcHeader.ParseFromString(rpc_header_str)) {   // 数据头反序列化成功
        // 获取反序列化结果
        std::string service_name = rpcHeader.service_name();   // 获取服务名称
        std::string method_name = rpcHeader.method_name();     // 获取方法名称
        uint32_t args_size = rpcHeader.args_size();           // 获取参数长度

        // 获取args_str参数字符串
        std::string args_str = request_data.substr(4 + header_size, args_size);

        LOG_INFO("RpcProvider::HandleRequest receive rpc request: \
                 service_name=%s method_name=%s args_size=%u",
                 service_name.c_str(), method_name.c_str(), args_size);

        /**
         * @note 第二步：根据 rpc 请求，查找注册的服务对象以及相应的方法
         */
        // 获取service和method
        auto sit = serviceMap_.find(service_name); // 查找服务是否存在
        if (sit == serviceMap_.end()) {
            LOG_ERR("RpcProvider::HandleRequest service %s not found!", service_name.c_str());
            return;
        }
        ServiceInfo serviceInfo = sit->second; // 获取服务信息结构体
        auto mit = serviceInfo.methodMap_.find(method_name); // 查找方法是否存在
        if (mit == serviceInfo.methodMap_.end()) {
            LOG_ERR("RpcProvider::HandleRequest method %s not found!", method_name.c_str());
            return;
        }

        // 服务对象
        google::protobuf::Service* service = serviceInfo.service_;
        // 服务对象的方法描述符
        const google::protobuf::MethodDescriptor* method = mit->second;

        /**
         * @note 第三步：反序列化参数，调用方法，获取响应结果
         */
        // 创建请求request和响应response消息对象
        google::protobuf::Message *request = service->GetRequestPrototype(method).New(); // 创建请求对象
        if (!request->ParseFromString(args_str)) {  // 反序列化请求参数
            LOG_ERR("RpcProvider::HandleRequest parse args_str error!");
            delete request;
            return;
        }
        google::protobuf::Message *response = service->GetResponsePrototype(method).New();  // 创建响应对象

        // 创建回调对象，用于处理rpc方法调用完成后的响应发送
        google::protobuf::Closure* done = google::protobuf::NewCallback<RpcProvider,
                                                                         std::shared_ptr<Session>,
                                                                         google::protobuf::Message*>(
            this,
            &RpcProvider::SendRpcResponse,
            session,
            response
        );

        // === 在框架上根据远程 rpc 调用请求，调用服务对象的方法 === 
        // protobuf会根据method描述符，调用对应的服务方法,并传入request、response、done参数,最终填充好response对象，并调用done回调
        service->CallMethod(method, nullptr, request, response, done);
        
        // 释放request内存
        delete request;
    } else {
        LOG_ERR("RpcProvider::HandleRequest parse rpc_header_str error!");
        return;
     }
}

// rpc方法调用完成后的回调函数
void RpcProvider::SendRpcResponse(std::shared_ptr<Session> session, google::protobuf::Message* response) {
    // ==== 组织 rpc 响应的字符流，并通过网络发送回 rpc 调用方 ====
    /**
     * 使用长度前缀方式发送响应，避免消息边界问题
     * 字符流包含的信息：
     * 1.响应数据的长度 response_size (4字节)
     * 2.响应消息 response_str
     */

    std::string response_str;
    // 序列化响应消息
    if (response->SerializeToString(&response_str)) {
        uint32_t response_len = htonl(response_str.size());  // 主机字节序转网络字节序
        
        // 组装发送数据：由于是异步发送(async_write)，底层 buffer 引用的内存必须存活到发送完毕
        // 拼接为一个完整 std::string，通过值传递或移动语义保持其生命周期
        std::string send_buf;
        send_buf.append((char*)&response_len, 4);
        send_buf.append(response_str);
        
        // 发送响应数据
        session->DoWrite(send_buf);
    } else {
        LOG_ERR("RpcProvider::SendRpcResponse serialize response error!");
    }
    // 释放response内存
    delete response;
}