# RPC 框架学习笔记

这是一个基于 C++ 开发的高性能 RPC (Remote Procedure Call) 框架，使用了 Google Protobuf 进行数据序列化，并利用 Boost.Asio 进行网络通信。

## 1. 项目结构

```text
├── bin/                # 可执行文件及配置文件目录
│   ├── config.yaml     # 框架配置文件
│   ├── consumer        # 客户端示例程序
│   └── provider        # 服务端示例程序
├── build/              # 构建目录
├── config/             # 配置文件源码
├── example/            # 示例代码
│   ├── callee/         # 服务提供者 (Callee) 代码
│   ├── caller/         # 服务调用者 (Caller) 代码
│   └── proto_gen/      # Protobuf 生成的 C++ 文件
├── lib/                # 库文件输出目录
└── src/                # 框架核心源码
    ├── include/        # 头文件
    │   ├── rpcapplication.h # 框架初始化与配置管理
    │   ├── rpcprovider.h    # 服务端网络对象
    │   ├── rpcchannel.h     # 客户端通道
    │   ├── rpccontroller.h  # RPC 控制器
    │   └── rpcconfig.h      # 配置加载器
    ├── rpcheader/      # RPC 协议头定义 (Protobuf)
    ├── rpcapplication.cpp
    ├── rpcprovider.cpp
    ├── rpcchannel.cpp
    ├── rpccontroller.cpp
    └── ...
```

## 2. 核心组件

### 2.1 RpcApplication
- **作用**: 框架的基础类，负责框架的初始化和配置加载。
- **实现**: 单例模式。在程序启动时调用 `Init` 加载配置文件（如 `config.yaml`），提供全局的配置访问接口。

### 2.2 RpcProvider (服务端)
- **作用**: 负责发布 RPC 服务，启动网络服务器，处理远程调用请求。
- **实现**:
    - 使用 **Boost.Asio** 实现高性能的网络通信。
    - 维护一个 `serviceMap_`，存储注册的服务对象及其方法描述符 (`MethodDescriptor`)。
    - 采用 **Proactor 模型**（通过 `io_context`）配合线程池（默认 6 个线程）处理并发请求。
    - **NotifyService**: 将用户实现的服务对象注册到框架中。
    - **Run**: 启动 TCP 服务器，监听端口，接受连接并分发请求。

### 2.3 RpcChannel (客户端)
- **作用**: 继承自 `google::protobuf::RpcChannel`，负责数据的序列化、网络发送和响应接收。
- **实现**:
    - 重写了 `CallMethod` 方法。
    - 当客户端调用 Stub 对象的方法时，最终会调用 `CallMethod`。
    - 负责按照自定义协议组装数据，建立连接，发送请求，并等待响应。

### 2.4 RpcHeader
- **作用**: 定义 RPC 调用的元数据。
- **定义**: 使用 Protobuf 定义 (`src/rpcheader/rpcheader.proto`)。
    ```protobuf
    message RpcHeader {
        bytes service_name = 1;
        bytes method_name = 2;
        uint32 args_size = 3;
    }
    ```

## 3. 通信协议设计

为了解决 TCP 粘包/拆包问题，框架设计了自定义的应用层协议。

**数据包格式**:
```text
+----------------+-------------------------+-----------------------+
|  header_size   |       header_str        |       args_str        |
+----------------+-------------------------+-----------------------+
|    4 字节      |   (Protobuf 序列化)     |   (Protobuf 序列化)   |
| (网络字节序)   | service_name, method... |      请求参数数据     |
+----------------+-------------------------+-----------------------+
```

1. **header_size**: 4 字节整数，记录 `header_str` 的长度。
2. **header_str**: 包含服务名、方法名和参数长度的序列化数据。
3. **args_str**: 具体的请求参数序列化数据。

**服务端处理流程**:
1. 读取前 4 个字节，获取 `header_size`。
2. 读取 `header_size` 长度的数据，反序列化为 `RpcHeader` 对象，获取 `service_name` 和 `method_name`。
3. 根据 `RpcHeader` 中的 `args_size`，读取请求参数数据。
4. 根据 `service_name` 和 `method_name` 在 `serviceMap_` 中找到对应的服务和方法。
5. 反序列化参数，调用本地服务方法。
6. 将执行结果序列化，并加上长度前缀发送回客户端。

## 4. 调用流程

1. **定义服务**: 使用 `.proto` 文件定义服务接口和消息类型。
2. **生成代码**: 使用 `protoc` 生成 C++ 代码 (`.pb.cc`, `.pb.h`)。
3. **服务端开发**:
    - 继承生成的 Service 类，实现虚函数（业务逻辑）。
    - 初始化 `RpcApplication`。
    - 创建 `RpcProvider`，调用 `NotifyService` 注册服务。
    - 调用 `Run` 启动服务。
4. **客户端开发**:
    - 初始化 `RpcApplication`。
    - 创建 `RpcChannel` 对象。
    - 创建生成的 Stub 类对象，传入 `RpcChannel`。
    - 调用 Stub 对象的方法（就像调用本地方法一样）。
    - `RpcChannel::CallMethod` 被触发，完成网络通信。

## 5. 关键技术点

- **Protobuf**: 用于高效的数据序列化和反序列化，以及服务接口定义。
- **Boost.Asio**: 用于实现异步、非阻塞的网络 I/O，支持高并发。
- **多线程**: 服务端使用线程池处理 I/O 事件，提高吞吐量。
- **自定义协议**: 解决 TCP 传输中的边界问题。
