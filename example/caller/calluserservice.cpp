#include <iostream>
#include <thread>
#include <chrono>
#include <arpa/inet.h>
#include <boost/asio.hpp>
#include "rpcapplication.h"
#include "rpcconfig.h"
#include "user.pb.h"
#include "rpcchannel.h"
#include "rpccontroller.h"
#include "rpcheader.pb.h"


int main(int argc, char **argv) {
    // 框架初始化
    RpcApplication::Init(argc, argv);

    // 通过rpc调用远程的用户服务
    fixbug::UserServiceRPC_Stub stub(new RpcChannel());
    // rpc 方法的请求
    fixbug::LoginRequest request;
    request.set_username("zhang san");
    request.set_password("123456");
    // rpc 方法的响应
    fixbug::LoginResponse response;
    // 发起 rpc 方法的调用
    stub.Login(nullptr, &request, &response, nullptr);

    // 读取 rpc 调用的结果
    if (response.result().errcode() == 0) {
        std::cout << "rpc login response: " << response.success() << std::endl;
    } else {
        std::cout << "rpc login response error: " << response.result().errmsg() << std::endl;
    }

    // 注册
    fixbug::RegisterRequest register_request;
    register_request.set_id(666);
    register_request.set_username("smile");
    register_request.set_password("114514");

    fixbug::RegisterResponse register_response; // rpc 方法的响应
    RpcController controller;   // rpc 控制器
    
    stub.Register(&controller, &register_request, &register_response, nullptr);

    if (controller.Failed()) {
        std::cout << "rpc register error: " << controller.ErrorText() << std::endl;
    } else {    // 正常返回
        if (register_response.result().errcode() == 0) {
            std::cout << "rpc register response: " << register_response.success() << std::endl;
        } else {
            std::cout << "rpc register response error: " << register_response.result().errmsg() << std::endl;
        }
    }
    
    // ================================================================
    // 半包测试：手动拆包，验证框架的 recv_buffer_ 拼包逻辑是否正确
    // ================================================================
    std::cout << "\n========== 半包测试开始 ==========" << std::endl;
    {
        // 1. 手动构造一条完整的 Login RPC 请求帧（与框架发送格式完全一致）
        fixbug::LoginRequest half_req;
        half_req.set_username("half_packet_user");
        half_req.set_password("half_pwd");

        std::string args_str;
        half_req.SerializeToString(&args_str);

        rpcheader::RpcHeader rpc_header;
        rpc_header.set_service_name("UserServiceRPC");
        rpc_header.set_method_name("Login");
        rpc_header.set_args_size(args_str.size());

        std::string header_str;
        rpc_header.SerializeToString(&header_str);

        uint32_t header_size_net = htonl(header_str.size());

        std::string full_frame;
        full_frame.append((char*)&header_size_net, 4);  // 4字节 header_size
        full_frame.append(header_str);                   // header
        full_frame.append(args_str);                     // args

        std::cout << "[半包测试] 完整帧大小: " << full_frame.size() << " 字节" << std::endl;

        // 2. 将帧拆成三段，模拟三次 TCP 分片到达
        size_t part1_len = 2;                               // 只发前 2 字节（连 header_size 都没发完）
        size_t part2_len = full_frame.size() / 2;           // 再发一半
        size_t part3_len = full_frame.size() - part1_len - part2_len;  // 剩余部分

        // 3. 建立原始 TCP 连接（绕过 ZooKeeper，直连 rpc server）
        std::string server_ip = RpcApplication::GetConfig().Load<std::string>("rpc.server_ip");
        uint16_t server_port  = RpcApplication::GetConfig().Load<int>("rpc.server_port");

        boost::asio::io_context ioc;
        boost::asio::ip::tcp::socket sock(ioc);
        boost::asio::ip::tcp::resolver resolver(ioc);
        boost::asio::connect(sock, resolver.resolve(server_ip, std::to_string(server_port)));
        std::cout << "[半包测试] 已连接服务器 " << server_ip << ":" << server_port << std::endl;

        // 4. 第一段：仅发 2 字节（半包：连 header_size 字段都没发完）
        boost::asio::write(sock, boost::asio::buffer(full_frame.data(), part1_len));
        std::cout << "[半包测试] 发送第 1 段: " << part1_len << " 字节，睡眠 300ms..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 5. 第二段：再发一半
        boost::asio::write(sock, boost::asio::buffer(full_frame.data() + part1_len, part2_len));
        std::cout << "[半包测试] 发送第 2 段: " << part2_len << " 字节，睡眠 300ms..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 6. 第三段：发送剩余字节，此时一帧才完整
        boost::asio::write(sock, boost::asio::buffer(full_frame.data() + part1_len + part2_len, part3_len));
        std::cout << "[半包测试] 发送第 3 段: " << part3_len << " 字节，帧已完整" << std::endl;

        // 7. 读取响应（4字节长度前缀 + 响应体）
        uint32_t resp_len_net = 0;
        boost::asio::read(sock, boost::asio::buffer(&resp_len_net, 4));
        uint32_t resp_len = ntohl(resp_len_net);

        std::vector<char> resp_body(resp_len);
        boost::asio::read(sock, boost::asio::buffer(resp_body));

        // 8. 反序列化并验证结果
        fixbug::LoginResponse half_resp;
        if (half_resp.ParseFromArray(resp_body.data(), resp_len)) {
            if (half_resp.result().errcode() == 0) {
                std::cout << "[半包测试] 成功! 服务端正确拼包并响应, login success = "
                          << half_resp.success() << std::endl;
            } else {
                std::cout << "[半包测试] 业务错误: " << half_resp.result().errmsg() << std::endl;
            }
        } else {
            std::cout << "[半包测试] 失败! 响应反序列化错误" << std::endl;
        }
    }
    std::cout << "========== 半包测试结束 ==========" << std::endl;

    return 0;
}