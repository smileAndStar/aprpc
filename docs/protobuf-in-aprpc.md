# Protobuf 在 APRPC 中的作用与技术手册

> 面向对象：需要在 APRPC 上开发业务服务、扩展框架，或准备基于本框架面试/答辩的开发者。
>
> 本文所有结论均以当前 `src/`、`example/` 源码为准。凡是**生成代码提供但当前项目并未调用**的 API，均已显式标注为「未使用」，避免把 protobuf 的通用能力误当成框架的既有功能。

---

## 目录

1. [引入背景](#1-引入背景)
2. [与 JSON 及其他序列化方案的对比](#2-与-json-及其他序列化方案的对比)
3. [Protobuf 在本框架中的三层职责](#3-protobuf-在本框架中的三层职责)
4. [协议布局与两个 proto 文件](#4-协议布局与两个-proto-文件)
5. [具体方法清单](#5-具体方法清单)
   - 5.1 [消息对象的创建与生命周期](#51-消息对象的创建与生命周期)
   - 5.2 [序列化接口](#52-序列化接口)
   - 5.3 [反序列化接口（含校验）](#53-反序列化接口含校验)
   - 5.4 [字段访问与修改](#54-字段访问与修改)
   - 5.5 [服务描述、路由与回调（RPC 元编程）](#55-服务描述路由与回调rpc-元编程)
   - 5.6 [控制器（RpcController）](#56-控制器rpccontroller)
6. [与网络层的集成方式](#6-与网络层的集成方式)
7. [与存储层/配置层的集成方式](#7-与存储层配置层的集成方式)
8. [端到端完整示例](#8-端到端完整示例)
9. [约束、陷阱与注意事项](#9-约束陷阱与注意事项)
10. [附录：方法速查表](#10-附录方法速查表)

---

## 1. 引入背景

### 1.1 手写序列化会遇到什么

一个 RPC 框架的最小内核是「把本地的一次函数调用，变成网络上的一段字节，再在对端还原成一次函数调用」。中间这段字节如何组织，就是序列化方案要解决的问题。如果手写，会立刻撞上四堵墙：

| 问题 | 手写方案的典型困境 |
|---|---|
| **边界与可变长字段** | 参数字段长度不固定，必须自己设计「字段长度 + 字段内容」的编码，还要处理长度字段本身的字节序 |
| **版本兼容** | 服务端新增一个字段，客户端不知道要跳过几个字节，很容易整包解析错位 |
| **跨语言** | 客户端用 Java、服务端用 C++ 时，两边的结构体布局和字节序假设必须逐字节对齐 |
| **接口契约** | 「有哪些服务、每个服务有哪些方法、参数和返回值是什么类型」这件事，需要有一份机器可读的定义，否则框架无法做通用路由和参数校验 |

### 1.2 APRPC 对序列化层的四项硬需求

结合源码，本框架对序列化层提出的是这四项要求：

1. **可生成接口契约**：`RpcProvider::NotifyService` 需要拿到服务的 `ServiceDescriptor`、方法的 `MethodDescriptor`，才能在运行时把「服务名 + 方法名」映射到一次 C++ 虚函数调用。参见 `src/rpcprovider.cpp:20-43`：

   ```cpp
   const google::protobuf::ServiceDescriptor *pserviceDesc = service->GetDescriptor();
   std::string service_name = pserviceDesc->name();
   int methodCnt = pserviceDesc->method_count();
   for (int i = 0; i < methodCnt; ++i) {
       const google::protobuf::MethodDescriptor *pmethodDesc = pserviceDesc->method(i);
       serviceInfo.methodMap_.insert({pmethodDesc->name(), pmethodDesc});
   }
   ```

2. **可生成类型安全的调用端**：客户端只应该写 `stub.Login(&controller, &request, &response, nullptr)`，而不是手工拼字节。

3. **请求体、响应体、控制头三者统一编码**：注意本框架连**内部协议头**都是 protobuf（`rpcheader.proto`），而不是手写的变长字符串编码。

4. **支持协议演进**：服务端滚动升级时，老客户端不能因为多了一个字段就解析失败。

Protobuf 同时满足这四项——**这正是选它的根本原因：它不只提供序列化，还提供 IDL + 代码生成 + RPC 元编程接口**。纯序列化库（JSON、MessagePack）只能满足第 3 项。

### 1.3 构建期的接入方式

```bash
# 仅当 .proto 变更时才需要重新生成；生成的 pb.cc/pb.h 已纳入版本管理
cd src/rpcheader && protoc -I. --cpp_out=. rpcheader.proto
cd example/proto_gen && protoc -I. --cpp_out=. user.proto
```

生成产物被显式编入库与可执行文件：

- `src/CMakeLists.txt:8-11`：`rpcheader.pb.cc` 编入静态库 `rpc`
- `example/callee/CMakeLists.txt:1-4` 与 `example/caller/CMakeLists.txt:1-4`：`user.pb.cc` 分别编入 `provider` 与 `consumer`

---

## 2. 与 JSON 及其他序列化方案的对比

### 2.1 横向对比表

| 维度 | **Protobuf** | JSON | XML | Thrift | MessagePack | 手写二进制 |
|---|---|---|---|---|---|---|
| 编码形态 | 二进制（TLV + varint） | 文本 | 文本 | 二进制 | 二进制 | 二进制 |
| 是否需 IDL / schema | **必需**（`.proto`） | 不需要 | 可选（XSD） | 必需（`.thrift`） | 不需要 | 必需（人脑里的） |
| 字段如何标识 | **数字 tag** | 字段名字符串 | 元素标签名 | 数字 ID | 字段名/位置 | 自行约定 |
| 相同数据的体积 | 小 | 大（字段名重复出现） | 最大 | 小 | 很小 | 最小 |
| 编解码速度 | 快（生成代码直接读写） | 慢（文本解析 + 类型推断） | 最慢 | 快 | 快 | 最快 |
| 类型安全 | **编译期** | 运行时 | 运行时 | 编译期 | 运行时 | 编译期 |
| 前后兼容 | **好**（字段号稳定，未知字段可跳过） | 弱（靠约定） | 中 | 好 | 弱 | 难 |
| 自描述性 | 否（解码需 `.proto`） | 是 | 是 | 否 | 部分 | 否 |
| 人类可读 / 可手工编辑 | 差（需 `protoc --decode`） | 好 | 好 | 差 | 差 | 不可读 |
| 官方跨语言支持 | 10+ 语言 | 全部语言 | 全部语言 | 若干 | 多语言三方库 | 需自己实现 |
| 是否自带 RPC 抽象 | **有**（Service / Stub / Channel / Controller / Closure） | 无 | 有（SOAP/WSDL） | 有 | 无 | 无 |

### 2.2 关键差异解读

**① 决定性的差别是「有没有 RPC 抽象」，而不是「快多少」**

JSON 只回答「对象怎么变成字节」。Protobuf 额外回答「有哪些服务、哪些方法、参数类型是什么」，并生成：

- 服务端基类 `UserServiceRPC`（业务类继承它，重写 `Login`/`Register`）
- 客户端 Stub 类 `UserServiceRPC_Stub`（内部持有 `RpcChannel*`）
- 运行时描述符 `ServiceDescriptor` / `MethodDescriptor`
- 通用分发入口 `Service::CallMethod`

本框架的 `RpcProvider::HandleRequest` 正是靠第 3、4 项完成「服务名 + 方法名 → 具体 C++ 虚函数」的通用路由（`src/rpcprovider.cpp:326`）：

```cpp
service->CallMethod(method, nullptr, request, response, done);
```

如果换成 JSON，框架就必须自己实现一套 IDL 解析器、代码生成器和反射表，等于把 protobuf 重写一遍。

**② 数字 tag 带来的体积与解析优势**

Protobuf 的每个字段编码为 `(field_number << 3) | wire_type`，**字段名完全不出现在字节流里**。JSON 每发一次 `{"username": "zhang san", "password": "123456"}`，`username`/`password` 这 15 个字符（加上引号和冒号共 21 字节）都要在网络上重复传输并逐字符解析；Protobuf 只传两个 tag 字节。

再加上 varint（小整数用 1 字节表示）、`uint32` 等类型定长或变长压缩，`bytes`/`string` 只传长度 + 内容。所以：

- **网络字节数**显著更少（字段名越长的结构化数据优势越明显）
- **解析不需要做字符串比较**，直接按 tag 分派到生成代码的对应字段

**③ 兼容性机制的本质**

Protobuf 的前后兼容建立在两条规则上：

1. **字段号一旦分配就不能改语义**；新增字段用新号，删除字段用 `reserved` 占位
2. **解码时遇到不认识的字段号，可以安全跳过**（因为每个字段自带 wire type，能算出它的长度）

于是老客户端收到新服务端多发的字段时，会静静地跳过；新客户端收不到老服务端没发的字段时，该字段保持默认值。而 JSON 的兼容性完全依赖双方的约定（比如「字段缺失时怎么办」），没有类型系统兜底。

**④ Protobuf 不适用的场景——项目自身就是例证**

Protobuf 需要 `.proto` 才能解码，**不可读、不可手工编辑**。所以本框架的**运行配置用 YAML 而不是 protobuf**（`config/config.yaml`），因为配置是人写的、需要注释和可读性：

```yaml
rpc:
  server_ip: "127.0.0.1"
  server_port: 8000
logger:
  write_mode: "sequential"
```

同一个项目里两种格式并存，各自解决不同问题——**「机器之间传的」用 protobuf，「人写的」用 YAML**。这也是面试时解释选型的好角度。

**⑤ 关于「Protobuf 一定比 JSON 快」的准确说法**

`docs/aprpc-project-review-and-interview-guide.md` 中的口径值得遵守：Protobuf 通常更快，但**不应该无条件断言**。小消息、特定数据分布、不同实现库下，结果可能反转。要下结论必须做同口径 benchmark（本项目已有 `bin/logger_perf` 与 `bin/rpc_bench` 作为压测基础设施，但**目前没有针对序列化本身的对比压测**）。

---

## 3. Protobuf 在本框架中的三层职责

| 层次 | 职责 | 具体体现 |
|---|---|---|
| **第一层：IDL** | 定义数据结构与远程调用接口 | `src/rpcheader/rpcheader.proto`（内部协议头）、`example/proto_gen/user.proto`（示例业务服务） |
| **第二层：编解码** | 二进制序列化/反序列化 | `SerializeToString` / `SerializeToArray` / `ParseFromString` / `ParseFromArray` |
| **第三层：RPC 元编程** | 用描述符 + 原型 + 通用 `CallMethod` 实现「不依赖具体类型」的通用路由与调用 | `ServiceDescriptor`、`MethodDescriptor`、`GetRequestPrototype`、`Service::CallMethod`、`RpcChannel`、`RpcController`、`Closure` |

第三层是这套框架「能成为框架」的关键：`RpcProvider` 和 `RpcProvider::HandleRequest` 是**完全不感知业务类型**的通用代码。它们不知道 `LoginRequest` 长什么样，却能把任意字节流还原成正确的请求对象并调用正确的业务方法——靠的就是 `MethodDescriptor` + `GetRequestPrototype().New()` 这套反射式的元编程接口。

---

## 4. 协议布局与两个 proto 文件

### 4.1 内部协议头：`src/rpcheader/rpcheader.proto`

```protobuf
syntax = "proto3";
package rpcheader;

message RpcHeader {
    bytes service_name = 1;
    bytes method_name = 2;
    uint32 args_size = 3;
}
```

三个字段分别解决：

- `service_name` / `method_name`：服务端在哪张表里找到目标方法（`serviceMap_` + `methodMap_`）
- `args_size`：**接收方据此知道请求体有多长**，从而判定「一帧是否收完」

> **为什么协议头也用 protobuf，而不是手写一个 `"name1|name2|123"` 的字符串？**
>
> 三个理由：① 复用已有的编解码能力，不必手写变长字符串编码；② 字段可以演进（将来加版本号、加 `request_id` 只需分配新字段号，老端自动跳过）；③ 与请求体用同一套机制，减少协议分支。
>
> **但外层仍然必须有固定的 4 字节长度前缀**——因为接收方需要先知道「这段 protobuf 头在 TCP 流的哪里结束」，才能开始解析它。protobuf 本身是自描述的（自带字段 tag 和长度），但它**不告诉你消息边界在哪**，这是长度前缀存在的意义。

### 4.2 示例业务服务：`example/proto_gen/user.proto`

```protobuf
syntax = "proto3";
package fixbug;

option cc_generic_services = true;   // ← 关键：没有这行不会生成 Service/Stub

message ResultCode {
    int32 errcode = 1;
    bytes errmsg = 2;
}

message LoginRequest {
    bytes username = 1;
    bytes password = 2;
}

message LoginResponse {
    ResultCode result = 1;
    bool success = 2;
}

message RegisterRequest {
    uint32 id = 1;
    bytes username = 2;
    bytes password = 3;
}

message RegisterResponse {
    ResultCode result = 1;
    bool success = 2;
}

service UserServiceRPC {
    rpc Login(LoginRequest) returns(LoginResponse);
    rpc Register(RegisterRequest) returns(RegisterResponse);
}
```

> **`option cc_generic_services = true` 是必须的。** 它是「是否生成基于 `Service`/`Stub`/`RpcChannel` 的 C++ 服务代码」的开关。默认值为 `false`，此时 `protoc` 只生成 message 类，**不会**生成 `UserServiceRPC` 基类、`UserServiceRPC_Stub`、`descriptor()`、`CallMethod()` 等——本框架的整套路由机制都会失效。注意 `rpcheader.proto` **没有**这一行，这是正确的，因为它只定义数据、不定义服务。

### 4.3 请求帧布局

```
+----------------+---------------------------------+---------------------------+
|  header_size   |          header_str             |         args_str          |
+----------------+---------------------------------+---------------------------+
|     4 字节     |      Protobuf 序列化 RpcHeader   |   Protobuf 序列化请求参数   |
|  (网络字节序)   |  service_name/method_name/...   |      （业务 message）      |
+----------------+---------------------------------+---------------------------+
      uint32                   ≤ 64 KB                     ≤ 64 MB（约定）
                                  ↑                            ↑
                        header_size 是其长度          args_size（在 header 里）是其长度
```

响应帧更简单，只有长度前缀 + 响应体：

```
+----------------+---------------------------+
|  response_len  |        response_str       |
+----------------+---------------------------+
|     4 字节     |  Protobuf 序列化响应 message |
+----------------+---------------------------+
```

---

## 5. 具体方法清单

> **阅读约定**：
> - 「框架使用」= 当前 `src/` 或 `example/` 中确有调用，并给出文件行号
> - 「未使用」= 生成代码提供了该 API，但当前项目没有任何调用点
> - `T` 表示具体 message 类型（如 `LoginRequest`），`M` 表示基类 `google::protobuf::Message`

---

### 5.1 消息对象的创建与生命周期

#### 5.1.1 直接构造（栈对象）

```cpp
T value;
T value(/* 拷贝源 */);
```

| 项 | 说明 |
|---|---|
| 输入 | 无参构造，或拷贝源对象 |
| 输出 | 已初始化的 message 对象，所有字段为类型默认值（数值 0 / 布尔 false / 字符串空 / 子 message 为「未设置」） |
| 框架使用 | `example/caller/calluserservice.cpp:21`、`37`、`42`、`63`、`125`；`example/benchmark/rpc_bench.cpp:53` |
| 典型用法 | 客户端填请求、接响应 |

```cpp
fixbug::LoginRequest request;         // 请求对象
request.set_username("zhang san");
request.set_password("123456");

fixbug::LoginResponse response;       // 响应对象，由 stub 填充
stub.Login(nullptr, &request, &response, nullptr);
```

#### 5.1.2 `M New()` —— 多态创建（框架泛型路由的基石）

```cpp
virtual M* New() const = 0;   // google::protobuf::Message
```

| 项 | 说明 |
|---|---|
| 输入 | 无 |
| 输出 | 堆上新分配的、与该对象**同具体类型**的空 message，**调用方负责 `delete`** |
| 框架使用 | `src/rpcprovider.cpp:306`、`312` |
| 典型用法 | 在只知道 `Message*` 基类指针时创建同类型的空对象 |

```cpp
google::protobuf::Message *request  = service->GetRequestPrototype(method).New();
google::protobuf::Message *response = service->GetResponsePrototype(method).New();
// ...
delete request;    // 框架显式释放
// 注意：response 在 SendRpcResponse 里释放（src/rpcprovider.cpp:359）
```

> 这里体现的是 protobuf 的**原型模式（Prototype）**：`GetRequestPrototype(method)` 返回的是该方法的请求 message 原型，`.New()` 克隆出一个可写的空实例。整个 `HandleRequest` 是**类型无关**的——它不知道也不关心 `method` 对应的是 `Login` 还是 `Register`。这是本框架最值得强调的 protobuf 用法。

#### 5.1.3 `Clear()` / `CopyFrom()` / `MergeFrom()` / `Swap()`

| 方法 | 签名 | 输入 | 输出 | 框架状态 |
|---|---|---|---|---|
| `Clear()` | `void Clear()` | 无 | 全部字段重置为默认值 | **未使用** |
| `CopyFrom()` | `void CopyFrom(const M&)` | 源 message | 深拷贝覆盖本对象 | **未使用**（生成代码中作为拷贝赋值实现） |
| `MergeFrom()` | `void MergeFrom(const M&)` | 源 message | 单字段合并：标量覆盖、repeated 追加、子 message 递归合并 | **未使用** |
| `Swap()` | `void Swap(T*)` | 同类型对象 | 交换两者内容（通常只换指针，O(1)） | **未使用** |

> **`Clear()` vs `CopyFrom()` 的语义差异**值得记住：`Clear()` 用于复用对象（避免反复分配），`MergeFrom()` 用于「累加」语义（如分片接收后合并），而 `CopyFrom()` 是「替换」语义。

#### 5.1.4 `GetRequestPrototype` / `GetResponsePrototype`（服务侧）

```cpp
const google::protobuf::Message& GetRequestPrototype(const MethodDescriptor*) const;
const google::protobuf::Message& GetResponsePrototype(const MethodDescriptor*) const;
```

| 项 | 说明 |
|---|---|
| 输入 | `MethodDescriptor*`（从 `methodMap_` 查表得到） |
| 输出 | 该方法的请求 / 响应 message **原型对象引用**（只读，所有权不转移） |
| 框架使用 | `src/rpcprovider.cpp:306`、`312` |
| 声明位置 | 生成代码 `example/proto_gen/user.pb.h:718`、`720` |

这是生成代码提供、由框架消费的关键接口：**服务端在没有具体类型信息的情况下，通过它拿到「该方法该用什么请求类型」**。

---

### 5.2 序列化接口

#### 5.2.1 `SerializeToString()` —— 本框架最主要的序列化入口

```cpp
bool SerializeToString(std::string* output) const;
```

| 项 | 说明 |
|---|---|
| 输入 | 输出参数 `output` 的指针，**不能为 nullptr** |
| 输出 | 返回 `bool` 表示是否成功；成功时 `output` 被覆盖为序列化后的二进制串 |
| 框架使用 | 请求侧 `src/rpcchannel.cpp:31`；协议头 `src/rpcchannel.cpp:44`；响应侧 `src/rpcprovider.cpp:344` |
| 示例侧使用 | `example/caller/calluserservice.cpp:68`、`76`；`example/benchmark/rpc_bench.cpp:57`、`67`、`86`、`96` |

**典型用法（客户端组装请求头 + 请求体）** —— `src/rpcchannel.cpp:29-56`：

```cpp
std::string args_str;                         // 序列化后的请求参数
if (!request->SerializeToString(&args_str)) {
    controller->SetFailed("request SerializeToString failed!");
    return;
}

rpcheader::RpcHeader rpcheader;               // 构建协议头
rpcheader.set_service_name(service_name);
rpcheader.set_method_name(method_name);
rpcheader.set_args_size(args_str.size());     // ← 长度写入头里，接收方据此切帧

std::string header_str;
if (!rpcheader.SerializeToString(&header_str)) {
    controller->SetFailed("rpcheader SerializeToString failed!");
    return;
}

uint32_t header_size = htonl(header_str.size());   // 转网络字节序
std::string send_buf;
send_buf.append((char*)&header_size, 4);      // 1. 4 字节 header_size
send_buf.append(header_str);                  // 2. 序列化后的 header
send_buf.append(args_str);                    // 3. 序列化后的请求体
```

> **注意 `set_args_size(args_str.size())` 的类型转换**：`args_size` 在 proto 里是 `uint32`，而 `std::string::size()` 返回 `size_t`。当前代码依赖隐式窄化，**在大消息（>4GB）时会截断**。实际受 64MB 约定限制不会触发，但这是值得补一个显式校验/断言的位置。

#### 5.2.2 `SerializeAsString()`

```cpp
std::string SerializeAsString() const;
```

| 项 | 说明 |
|---|---|
| 输入 | 无 |
| 输出 | 直接返回序列化后的 `std::string`（失败时返回空串，**无法区分「失败」与「序列化结果本来就是空」**） |
| 框架状态 | **未使用** |

优点是不必先准备输出容器；缺点是没有错误反馈。框架选择 `SerializeToString` 正是因为需要明确的失败信号（见上面 `if (!...)` 判断）。

#### 5.2.3 `SerializeToArray()` / `SerializePartialToString()` / `ByteSizeLong()`

| 方法 | 签名 | 输入 / 输出 | 框架状态 |
|---|---|---|---|
| `SerializeToArray` | `bool SerializeToArray(void* data, int size) const` | 入：目标缓冲区 + 容量；出：是否成功 | **未使用** |
| `SerializePartialToString` | `bool SerializePartialToString(std::string*) const` | 同 `SerializeToString`，但对未初始化的字段不做检查 | **未使用** |
| `ByteSizeLong` | `size_t ByteSizeLong() const` | 出：序列化后所需字节数 | **未使用** |

> **`SerializeToArray` 与 `ByteSizeLong` 的组合是零拷贝写网络的常用手段**：先用 `ByteSizeLong()` 算出精确长度，`resize` 一次缓冲区，再 `SerializeToArray` 直接写进去，可以省掉 `std::string` 的一次分配与拷贝。当前框架的网络层用的是「拼接进一个 `std::string` 再整体发送」（`src/rpcchannel.cpp:53-56`、`src/rpcprovider.cpp:349-352`），属于可优化点。

---

### 5.3 反序列化接口（含校验）

#### 5.3.1 `ParseFromString()` —— 客户端解析响应

```cpp
bool ParseFromString(const std::string& data);
```

| 项 | 说明 |
|---|---|
| 输入 | 待解析的二进制字符串 |
| 输出 | `bool`：`true` 表示解析成功；`false` 表示数据损坏或不是合法编码。**失败时对象状态未定义，不应继续使用** |
| 框架使用 | `src/rpcchannel.cpp:115` |
| 示例使用 | 无 |

```cpp
std::vector<char> response_body(response_len);
boost::asio::read(socket, boost::asio::buffer(response_body));

std::string response_str(response_body.data(), response_len);   // ← 这里有一次拷贝
if (!response->ParseFromString(response_str)) {
    controller->SetFailed("ParseFromString response failed!");
    return;
}
```

> 这个拷贝可以省掉，直接用下面的 `ParseFromArray`。

#### 5.3.2 `ParseFromArray()` —— 服务端解析请求头与请求体（零拷贝）

```cpp
bool ParseFromArray(const void* data, int size);
```

| 项 | 说明 |
|---|---|
| 输入 | `data`：数据首地址（**不要求以 `\0` 结尾**）；`size`：有效字节数 |
| 输出 | `bool`：是否解析成功 |
| 框架使用 | 协议头 `src/rpcprovider.cpp:190`；请求体 `src/rpcprovider.cpp:307` |
| 示例使用 | `example/caller/calluserservice.cpp:126`；`example/benchmark/rpc_bench.cpp:110`、`118` |

**典型用法一：解析协议头**（`src/rpcprovider.cpp:188-196`）：

```cpp
rpcheader::RpcHeader rpcHeader;
if (!rpcHeader.ParseFromArray(frame_ptr + 4, static_cast<int>(header_size))) {
    LOG_ERR("Session::TryParseAndHandle parse rpc_header_str error, close connection!");
    recv_buffer_.clear();
    recv_offset_ = 0;
    return;   // 协议错误，断连
}
uint32_t args_size = rpcHeader.args_size();
```

**典型用法二：直接把接收缓冲区里的请求体反序列化**（`src/rpcprovider.cpp:306-311`）：

```cpp
google::protobuf::Message *request = service->GetRequestPrototype(method).New();
if (!request->ParseFromArray(args_data, static_cast<int>(args_size))) {
    LOG_ERR("RpcProvider::HandleRequest parse args_str error!");
    delete request;
    return;
}
```

> **为什么这里用 `ParseFromArray` 而不是 `ParseFromString`？**
>
> 因为 `args_data` 指向的是 `recv_buffer_` 中间的一段连续内存，**它不是独立的、以 `\0` 结尾的字符串**。如果先构造 `std::string` 再解析，就要为每一条请求体做一次完整拷贝（大消息时非常昂贵）。`ParseFromArray` 直接在原缓冲区上解析，protobuf 内部会把数据拷进 message 对象的字段里，因此 **`args_data` 只需在本函数同步执行期间有效**——这一点已在 `src/include/rpcprovider.h:101-105` 中显式注释说明。

#### 5.3.3 `ParseFromIstream()` / `MergeFromString()` / `ParsePartialFromString()`

| 方法 | 签名 | 输入 / 输出 | 框架状态 |
|---|---|---|---|
| `ParseFromIstream` | `bool ParseFromIstream(std::istream*)` | 入：输入流；出：是否成功 | **未使用** |
| `MergeFromString` | `bool MergeFromString(const std::string&)` | 入：二进制串；出：是否成功。**合并**而非替换 | **未使用** |
| `ParsePartialFromString` | `bool ParsePartialFromString(const std::string&)` | 同 `ParseFromString`，但容忍未初始化字段 | **未使用** |
| `MergePartialFromCodedStream` | `bool MergePartialFromCodedStream(io::CodedInputStream*)` | 底层流式解析入口，供高级用法 | **未使用** |

> `MergeFromString` 与 `ParseFromString` 的区别：前者保留对象中已有的 repeated 字段并追加，后者先 `Clear()` 再解析。**同一个 message 对象被复用解析多条消息时，误用 `MergeFromString` 会导致数据累积**，这是常见 bug。

#### 5.3.4 校验方法

| 方法 / 机制 | 说明 | 框架状态 |
|---|---|---|
| `ParseFromArray` / `ParseFromString` 的**返回值** | **本框架唯一真正生效的校验手段**。返回 `false` 即断连，见 `src/rpcprovider.cpp:190-194`、`307-311` | **使用中** |
| `IsInitialized()` | `bool IsInitialized() const`。proto3 已移除 `required` 字段，因此该函数**恒返回 `true`**，不具备实际校验价值 | **未使用** |
| `has_xxx()` | 字段存在性检查。**仅对 message 类型字段（及 proto3 `optional` 标量）生成** | 生成代码中存在（如 `user.pb.h:411` 的 `has_result()`），**未使用** |
| 长度上限校验 | 协议头 `header_size ∈ [1, 64KB]`，见 `src/rpcprovider.cpp:175` | **已实现**（请求头） |
| 长度上限校验 | 请求体 `args_size ≤ 64MB` | **未实现** |
| 长度上限校验 | 响应体 `response_len` | **未实现**（客户端 `src/rpcchannel.cpp:110` 直接按对端给的长度分配内存） |

**关于 `has_xxx()` 的 proto3 陷阱**（重要）：

`user.proto` 中 `LoginRequest.username` 是 `bytes` 类型（标量类），**proto3 不会为它生成 `has_username()`**。这意味着服务端**无法区分**「客户端显式传了空字符串」和「客户端根本没设置这个字段」——两者读出来都是 `""`。

而 `LoginResponse.result` 是 message 类型，所以**有** `has_result()`。如果业务上需要区分「字段未设置」与「设置为默认值」，必须把该字段包成 message（或用 proto3 的 `optional` 关键字）。

#### 5.3.5 字段存在性和校验的常见组合

```cpp
// 判断子 message 是否被设置过（仅 message 字段可用）
if (!req.has_result()) {
    // 客户端没有传 result
}

// 判断 repeated 字段是否为空（标量 repeated 用 size() 而非 has_）
if (req.items_size() == 0) { /* ... */ }

// 业务级校验仍然必须自己写：protobuf 只保证「结构合法」，不保证「语义合法」
if (request->username().empty() || request->password().empty()) {
    response->mutable_result()->set_errcode(1);
    response->mutable_result()->set_errmsg("username or password is empty");
    done->Run();
    return;
}
```

> **必须强调**：`ParseFromArray` 返回 `true` 只说明「字节流符合 protobuf 编码规范」，**不代表业务参数合法**。用户名是否为空、密码长度是否合规、id 是否越界，都需要业务代码自己校验。示例中的 `UserService::Login` 就没有做这类校验。

---

### 5.4 字段访问与修改

Protobuf 为每个字段生成一套**命名规则固定**的访问器。以 `user.proto` 为例：

#### 5.4.1 访问器命名规则总表

假设字段名为 `foo`：

| 生成的方法 | 适用字段类型 | 签名示例 | 输入 / 输出 | 使用状态 |
|---|---|---|---|---|
| `foo()` | 全部 | `const T& foo() const` / `T foo() const` | 出：字段值。**返回引用**（string/bytes/message）或**返回值**（标量） | **使用中** |
| `set_foo(v)` | 全部（非 repeated） | `void set_foo(const T& value)` | 入：新值 | **使用中** |
| `mutable_foo()` | string / bytes / message / repeated | `T* mutable_foo()` | 出：可写指针。**不存在时自动创建**（对 message 字段即「延迟分配」） | **使用中** |
| `clear_foo()` | 全部 | `void clear_foo()` | 无 | 生成代码中存在，**未使用** |
| `has_foo()` | **仅 message 字段**（及 proto3 `optional` 标量） | `bool has_foo() const` | 出：是否被设置过 | 生成代码中存在，**未使用** |
| `release_foo()` | string / message | `T* release_foo()` | 出：指针，**释放所有权**（对象不再持有，调用方负责 `delete`） | 生成代码中存在，**未使用** |
| `set_allocated_foo(p)` | string / message | `void set_allocated_foo(T* value)` | 入：指针，**接管所有权** | 生成代码中存在，**未使用** |
| `add_foo(v)` | repeated | `void add_foo(const T& value)` | 入：追加元素 | 无 repeated 字段，不适用 |
| `foo_size()` / `foo(i)` | repeated | `int foo_size() const` / `const T& foo(int i) const` | 出：元素数 / 第 i 个元素 | 无 repeated 字段，不适用 |

#### 5.4.2 标量（`uint32` / `int32` / `bool`）

```protobuf
uint32 id = 1;
bool success = 2;
```

生成的接口（`user.pb.h:181-184`、`422-426`）：

```cpp
::google::protobuf::uint32 id() const;
void set_id(::google::protobuf::uint32 value);
void clear_id();

bool success() const;
void set_success(bool value);
void clear_success();
```

典型用法：

```cpp
// 写
register_request.set_id(666);
response->set_success(true);

// 读
uint32_t id = request->id();
if (response.success()) { /* ... */ }
```

#### 5.4.3 `bytes` / `string`

```protobuf
bytes username = 1;
```

生成的接口（`user.pb.h:284-296`）——注意 string/bytes 的重载与所有权方法：

```cpp
const ::std::string& username() const;                    // 读：返回 const 引用，无拷贝
void set_username(const ::std::string& value);            // 写：拷贝
void set_username(::std::string&& value);                 // 写：移动（C++11 起）
void set_username(const char* value);                     // 写：C 字符串
void set_username(const void* value, size_t size);        // 写：显式长度（可含 '\0'）
::std::string* mutable_username();                        // 取可写指针
::std::string* release_username();                        // 释放所有权
void set_allocated_username(::std::string* username);     // 接管所有权
void clear_username();
```

典型用法：

```cpp
// 读（无拷贝，适合只读）
std::string name = request->username();

// 写（三种写法，性能依次提升）
request.set_username("zhang san");                        // const char*
std::string s = "zhang san";
request.set_username(s);                                  // 拷贝
request.set_username(std::move(s));                       // 移动，s 之后不可用

// 含 '\0' 的二进制内容（bytes 字段的典型场景）
request.set_username(raw_data, raw_len);
```

> **`bytes` vs `string` 的差别不只是命名习惯**：`string` 类型在解析时会做 **UTF-8 合法性校验**（非法编码会导致解析失败），而 `bytes` **不做任何校验**。本项目的 `username`/`password`/`service_name`/`method_name` 都用了 `bytes`，好处是省掉校验开销、可承载任意二进制，代价是**失去了「这是合法 UTF-8 文本」这一层保障**——业务侧若要按文本处理，需自行确认编码。

#### 5.4.4 嵌套 message（子消息）

```protobuf
message LoginResponse {
    ResultCode result = 1;   // ← message 类型字段
}
```

生成的接口（`user.pb.h:410-420`）——**注意它有 `has_`，且没有 `set_result()`**：

```cpp
bool has_result() const;
void clear_result();
const ::fixbug::ResultCode& result() const;        // 读：只读引用
::fixbug::ResultCode* release_result();            // 释放所有权
::fixbug::ResultCode* mutable_result();            // 写：可写指针（自动创建）
void set_allocated_result(::fixbug::ResultCode* result);
```

**message 类型字段没有 `set_foo(const T&)`，必须通过 `mutable_foo()` 写。** 典型用法（`example/callee/userservice.cpp:59-63`）：

```cpp
// 写法一：一次调用拿到可写指针，再逐个填字段
fixbug::ResultCode* code = response->mutable_result();
code->set_errcode(0);
code->set_errmsg("");
response->set_success(login_result);

// 写法二：链式调用（避免多次查找子对象）
response->mutable_result()->set_errcode(1);
response->mutable_result()->set_errmsg("login failed");

// 读（不触发任何分配）
if (response.result().errcode() == 0) { /* ... */ }
```

> **`mutable_result()` 的「自动创建」语义很关键**：即使 `result` 从未被设置，调用 `mutable_result()` 也会把它创建出来（默认值 `errcode=0, errmsg=""`），所以**业务代码里不需要也不能用「先检查再创建」的写法**。反过来说，`result()` 是只读的，对未设置的 message 字段返回一个默认实例，不会创建。

#### 5.4.5 字段访问的实际分布

| 访问器 | 框架代码 | 示例代码 |
|---|---|---|
| `xxx()` 读 | `rpcHeader.service_name()` / `.method_name()` / `.args_size()`（`src/rpcprovider.cpp:213-214`） | `request->username()` / `->password()` / `->id()`（`userservice.cpp:51,52,82,83,84`）；`response.success()` / `response.result().errcode()`（`calluserservice.cpp:30,31,50`） |
| `set_xxx()` 写 | `rpcheader.set_service_name/set_method_name/set_args_size`（`src/rpcchannel.cpp:39-41`） | `request.set_username/set_password/set_id`（`calluserservice.cpp:22,23,38-40`）；`response->set_success(...)`（`userservice.cpp:63,91`） |
| `mutable_xxx()` | — | `response->mutable_result()`（`userservice.cpp:59,88`） |

---

### 5.5 服务描述、路由与回调（RPC 元编程）

这一层是「Protobuf 作为 RPC 框架」的核心，也是与 JSON 拉开差距的地方。

#### 5.5.1 `ServiceDescriptor` / `MethodDescriptor` —— 运行时自省

```cpp
const google::protobuf::ServiceDescriptor* service->GetDescriptor();
const std::string& ServiceDescriptor::name() const;
int ServiceDescriptor::method_count() const;
const MethodDescriptor* ServiceDescriptor::method(int index) const;
const std::string& MethodDescriptor::name() const;
```

| 项 | 说明 |
|---|---|
| 输入 / 输出 | 从服务对象取描述符；从描述符取名称、方法数、第 i 个方法 |
| 框架使用 | `src/rpcprovider.cpp:20-43`（`NotifyService` 建表） |
| 典型用法 | 在服务启动时把「服务名 → {方法名 → MethodDescriptor}」构建成 `serviceMap_`，运行时 O(1) 查表 |

```cpp
// src/rpcprovider.cpp:20-39
const google::protobuf::ServiceDescriptor *pserviceDesc = service->GetDescriptor();
std::string service_name = pserviceDesc->name();
int methodCnt = pserviceDesc->method_count();

serviceInfo.service_ = service;
for (int i = 0; i < methodCnt; ++i) {
    const google::protobuf::MethodDescriptor *pmethodDesc = pserviceDesc->method(i);
    std::string method_name = pmethodDesc->name();
    serviceInfo.methodMap_.insert({method_name, pmethodDesc});
}
serviceMap_.insert({service_name, serviceInfo});
```

> **`NotifyService` 是业务代码唯一需要显式调用的注册入口**（`example/callee/userservice.cpp:105`）：`provider.NotifyService(new UserService());`。注意这里 `new` 出来的对象所有权交给框架，但**框架并未 `delete` 它**——属于已知的轻度所有权不清晰问题。

#### 5.5.2 `Service::CallMethod()` —— 服务端通用分发

```cpp
virtual void CallMethod(const MethodDescriptor* method,
                        RpcController* controller,
                        const Message* request,
                        Message* response,
                        Closure* done);
```

| 项 | 说明 |
|---|---|
| 输入 | `method`：目标方法描述符；`controller`：控制器（本框架传 `nullptr`）；`request`：已反序列化的请求；`response`：待填充的响应；`done`：业务方法结束后必须调用的回调 |
| 输出 | 无返回值。通过填充 `response` 和调用 `done` 产生效果 |
| 框架使用 | `src/rpcprovider.cpp:326` |
| 声明位置 | 生成代码 `example/proto_gen/user.pb.h:713-717` |

```cpp
// src/rpcprovider.cpp:324-326
// === 在框架上根据远程 rpc 调用请求，调用服务对象的方法 ===
// protobuf 会根据 method 描述符，调用对应的服务方法，并传入 request、response、done
service->CallMethod(method, nullptr, request, response, done);
```

**它内部做了什么**：生成代码根据 `method` 的描述符做一次 `switch`/比较，把调用转发到 `UserServiceRPC::Login` 或 `Register` 这两个虚函数上。业务类（`UserService`）重写它们，于是控制权回到业务代码。这就是「**服务名 + 方法名 → 具体 C++ 虚函数**」闭环的最后一跳。

> 传给 `CallMethod` 的 `controller` 是 `nullptr`，而业务方法里又常常不会用它——这意味着框架目前**不向业务层暴露任何请求级上下文**（比如「这次调用是谁发起的」）。这是可以扩展的点。

#### 5.5.3 `RpcChannel::CallMethod()` —— 客户端统一出口

```cpp
virtual void CallMethod(const MethodDescriptor* method,
                        RpcController* controller,
                        const Message* request,
                        Message* response,
                        Closure* done) = 0;
```

| 项 | 说明 |
|---|---|
| 输入 | 同上一节，但方向相反：`request` 是待发送的、`response` 是待填充的 |
| 输出 | 无返回值（同步实现下，返回即代表已完成） |
| 框架实现 | `src/rpcchannel.cpp:10-119`（`RpcChannel : public google::protobuf::RpcChannel`） |
| 触发时机 | 客户端调用 `stub.Login(...)` 时，由生成的 Stub 代码转入 |

```cpp
// example/caller/calluserservice.cpp:19-27
fixbug::UserServiceRPC_Stub stub(new RpcChannel());
fixbug::LoginRequest request;
request.set_username("zhang san");
request.set_password("123456");

fixbug::LoginResponse response;
stub.Login(nullptr, &request, &response, nullptr);   // ← 这一行最终进入 RpcChannel::CallMethod
```

**Stub 扮演的角色**（`user.pb.h:727-750`）：它只做两件事——保存 `RpcChannel*`，以及把「具体方法」翻译成对应的 `MethodDescriptor*` + 类型转换，然后调用 `channel_->CallMethod(...)`。**真正的传输逻辑完全在 `RpcChannel` 里**，这就是「Stub 决定类型安全，Channel 决定传输方式」的分工。

#### 5.5.4 `Closure` 与 `NewCallback<>()` —— 完成回调

```cpp
class Closure { virtual void Run() = 0; };

template <typename Class, typename... Args>
Closure* NewCallback(Class* object, void (Class::*method)(Args...), Args... args);
```

| 项 | 说明 |
|---|---|
| 输入 | 目标对象指针 + 成员函数指针 + 绑定的参数 |
| 输出 | **堆上分配的** `Closure*`，`Run()` 时以绑定参数调用该成员函数。**所有权归调用方，需要 `delete`** |
| 框架使用 | `src/rpcprovider.cpp:315-322` |
| 业务侧使用 | `example/callee/userservice.cpp:67`、`93` 的 `done->Run()` |

```cpp
// src/rpcprovider.cpp:314-322
google::protobuf::Closure* done = google::protobuf::NewCallback<RpcProvider,
                                                                 std::shared_ptr<Session>,
                                                                 google::protobuf::Message*>(
    this,
    &RpcProvider::SendRpcResponse,   // 绑定的成员函数
    session,                          // 参数 1：会话
    response                          // 参数 2：待发送的响应
);
service->CallMethod(method, nullptr, request, response, done);
```

业务方法最后必须调用 `done->Run()`（`example/callee/userservice.cpp:59-67`）：

```cpp
void UserService::Login(google::protobuf::RpcController* controller,
                        const fixbug::LoginRequest* request,
                        fixbug::LoginResponse* response,
                        google::protobuf::Closure* done) {
    std::string name = request->username();
    std::string pwd  = request->password();
    bool login_result = Login(name, pwd);              // 调用本地业务方法

    fixbug::ResultCode* code = response->mutable_result();
    code->set_errcode(0);
    code->set_errmsg("");
    response->set_success(login_result);

    done->Run();   // ← 触发 SendRpcResponse → 序列化 → 异步发送
}
```

> **`Closure` 是 protobuf 提供的「延迟动作」抽象，不是并发原语。** 当前实现里 `done->Run()` 是**同步执行**的（直接调进 `SendRpcResponse`），它做的事是「序列化并投递异步写」。因此：
> - 调用 `done->Run()` **不能晚于** 业务方法返回太久（`request` 对象在 `HandleRequest` 返回后就被 `delete` 了）；
> - 也**不能忘记调用**，否则客户端会一直等不到响应（当前框架没有超时兜底）。

#### 5.5.5 `NewCallback` 的所有权问题

`NewCallback` 返回的 `Closure*` 是**堆对象**，protobuf 不会自动释放。当前代码在 `HandleRequest` 中创建了它（`src/rpcprovider.cpp:315`）却没有 `delete`，业务示例也只调用 `done->Run()`。因此**每处理一个请求就会泄漏一个 `Closure` 对象**。

处理方式（供改造参考）：在 `SendRpcResponse` 末尾 `delete done;`（需要把 `done` 也绑进回调），或改用 `google::protobuf::NewPermanentCallback`（供多次调用，但仍需手动释放），或改用 `std::function` 自行管理生命周期。

---

### 5.6 控制器（RpcController）

`google::protobuf::RpcController` 是 protobuf 定义的**调用状态载体**抽象类。本框架实现了它：`src/include/rpccontroller.h`。

```cpp
class RpcController : public google::protobuf::RpcController {
    void Reset();                                   // 清空状态，供复用
    bool Failed() const;                            // 是否失败
    std::string ErrorText() const;                  // 错误文本
    void StartCancel();                             // 请求取消（未实现）
    void SetFailed(const std::string& reason);      // 设为失败并记录原因
    bool IsCanceled() const;                        // 是否已取消
    void NotifyOnCancel(google::protobuf::Closure* callback);  // 取消回调（未实现）
};
```

| 方法 | 输入 | 输出 | 框架实现状态 |
|---|---|---|---|
| `Reset()` | 无 | 无 | 已实现（`rpccontroller.cpp:7-10`） |
| `Failed()` | 无 | `bool` | 已实现 |
| `ErrorText()` | 无 | `std::string` | 已实现 |
| `SetFailed(reason)` | 错误原因 | 无 | 已实现 |
| `StartCancel()` | 无 | 无 | **空实现**（`rpccontroller.cpp:20-22`） |
| `IsCanceled()` | 无 | 恒 `false` | **桩实现** |
| `NotifyOnCancel(cb)` | 回调 | 无 | **空实现**（`rpccontroller.cpp:33-35`） |

典型用法（客户端）：

```cpp
// example/caller/calluserservice.cpp:43-55
RpcController controller;
stub.Register(&controller, &register_request, &register_response, nullptr);

if (controller.Failed()) {
    std::cout << "rpc register error: " << controller.ErrorText() << std::endl;
} else {
    // 业务层面的错误仍然要看响应体里的 ResultCode
    if (register_response.result().errcode() == 0) { /* ... */ }
}
```

> **这里有两条错误通道，不要混淆**：
> - **`RpcController`** 承载**框架级**错误（序列化失败、服务未在 ZooKeeper 注册、地址非法）
> - **响应体里的 `ResultCode`** 承载**业务级**错误（用户名不存在、密码错误）
>
> 业务代码两者都要检查。示例中的 `Login` 调用传了 `nullptr` 作为 controller（`calluserservice.cpp:27`），所以那条路径**只能**靠 `response.result().errcode()` 判断——而框架级错误在那次调用中会被静默丢弃（甚至触发空指针解引用，见下文 9.4）。

---

## 6. 与网络层的集成方式

### 6.1 请求路径（客户端 → 服务端）

| 步骤 | 位置 | Protobuf 参与点 |
|---|---|---|
| ① 业务调用 Stub | `example/caller/calluserservice.cpp:27` | 构造 `LoginRequest` / `LoginResponse`（栈对象） |
| ② Stub 转入 Channel | 生成代码 `user.pb.h:727-750` | 把具体方法翻译成 `MethodDescriptor*` |
| ③ 序列化请求体 | `src/rpcchannel.cpp:31` | **`request->SerializeToString(&args_str)`** |
| ④ 构造并序列化协议头 | `src/rpcchannel.cpp:38-47` | `set_service_name` / `set_method_name` / `set_args_size` + **`SerializeToString`** |
| ⑤ 拼帧 + 长度前缀 | `src/rpcchannel.cpp:50-56` | 无（`htonl` 转字节序，纯字节拼接） |
| ⑥ TCP 发送 | `src/rpcchannel.cpp:101` | — |
| ⑦ 服务端收字节、切帧 | `src/rpcprovider.cpp:157-223` | 无（纯长度前缀切帧，见 `TryParseAndHandle`） |
| ⑧ 反序列化协议头 | `src/rpcprovider.cpp:190` | **`rpcHeader.ParseFromArray(...)`** |
| ⑨ 查表找方法 | `src/rpcprovider.cpp:285-300` | `serviceMap_` → `MethodDescriptor*` |
| ⑩ 多态创建请求对象 | `src/rpcprovider.cpp:306` | **`GetRequestPrototype(method).New()`** |
| ⑪ 反序列化请求体 | `src/rpcprovider.cpp:307` | **`request->ParseFromArray(args_data, args_size)`** |
| ⑫ 创建响应对象 + 回调 | `src/rpcprovider.cpp:312-322` | `GetResponsePrototype(method).New()` + **`NewCallback`** |
| ⑬ 通用分发 | `src/rpcprovider.cpp:326` | **`service->CallMethod(method, ...)`** |
| ⑭ 业务填充响应 | `example/callee/userservice.cpp:51-63` | `request->username()`、`mutable_result()`、`set_success()` |

### 6.2 响应路径（服务端 → 客户端）

| 步骤 | 位置 | Protobuf 参与点 |
|---|---|---|
| ① 业务调用 `done->Run()` | `example/callee/userservice.cpp:67` | — |
| ② 进入 `SendRpcResponse` | `src/rpcprovider.cpp:333` | — |
| ③ 序列化响应 | `src/rpcprovider.cpp:344` | **`response->SerializeToString(&response_str)`** |
| ④ 加长度前缀 | `src/rpcprovider.cpp:345-352` | `htonl` + 拼接 |
| ⑤ 异步发送 | `src/rpcprovider.cpp:354`（`DoWrite`） | 无 |
| ⑥ 客户端读 4 字节长度 | `src/rpcchannel.cpp:106-107` | — |
| ⑦ 按长度读响应体 | `src/rpcchannel.cpp:110-111` | — |
| ⑧ 反序列化响应 | `src/rpcchannel.cpp:115` | **`response->ParseFromString(response_str)`** |
| ⑨ 业务读结果 | `example/caller/calluserservice.cpp:30-34` | `response.result().errcode()`、`response.success()` |

### 6.3 集成要点小结

1. **Protobuf 只负责「对象 ↔ 字节」，不负责「字节的边界」**。帧边界由外层 4 字节长度前缀 + `recv_buffer_` 累积逻辑（`TryParseAndHandle`）保证。两者职责必须分清——这是回答「长度前缀与 protobuf 是什么关系」的关键。
2. **服务端全程类型无关**。除业务类重写的虚函数外，`RpcProvider` 中没有任何一处出现 `fixbug::` 前缀，这是 `MethodDescriptor` + `New()` + `CallMethod` 三者配合的结果。
3. **请求体和协议头用的是同一套 protobuf 机制，但用的是不同的 message 类型**：协议头是 `rpcheader::RpcHeader`（框架私有），请求体是业务 message（用户定义）。二者的耦合点是 `args_size` 字段。

---

## 7. 与存储层/配置层的集成方式

**需要明确：当前 APRPC 没有任何基于 Protobuf 的持久化或存储路径。** 对照源码逐项确认：

| 项目 | 当前实现 | 是否用 Protobuf |
|---|---|---|
| 运行配置 | YAML（`config/config.yaml`），由 `RpcConfig::Load<T>` 读取 | **否**（刻意的：配置要人手写、要注释） |
| 日志落盘 | 纯文本行，按天切分 `YYYY-M-D-log.txt` | **否** |
| ZooKeeper 服务注册数据 | 纯字符串 `"ip:port"`（`src/rpcprovider.cpp:108-111`） | **否** |
| 请求/响应在途数据 | — | **是**（唯一的 protobuf 使用场景） |

```cpp
// 注册到 ZK 的是手拼字符串，不是 protobuf（src/rpcprovider.cpp:108-111）
char method_path_data[128] = {0};
sprintf(method_path_data, "%s:%d", ip.c_str(), port);
zkCli.Create(method_path.c_str(), method_path_data, strlen(method_path_data), ZOO_EPHEMERAL);
```

### 如果要在存储层引入 Protobuf —— 必须注意的坑

Protobuf 的编码**不自带消息边界**，所以「一个文件里顺序存多条 message」不能简单地连续 `SerializeToArray` 后拼接，否则读的时候无法知道第一条在哪里结束。三种可行方案：

| 方案 | 做法 | 适用场景 |
|---|---|---|
| **长度前缀（推荐，与网络层风格统一）** | 复用本框架的做法：`[4B len][protobuf bytes]` 循环写入 | 日志、消息队列、append-only 文件 |
| **`writeDelimitedTo` / `parseDelimitedFrom`** | protobuf 官方提供的流式 API，内部帮你写 varint 长度前缀 | C++ 侧需借助 `google::protobuf::util::DelimitedMessage` 或手写；Java 侧原生支持 |
| **外层容器 message** | 定义 `message Batch { repeated Item items = 1; }`，整体序列化一次 | 一次性批量读写、数据量可控 |

一个示意（长度前缀方案，风格与本框架网络层一致）：

```cpp
// 写
void WriteMessage(int fd, const google::protobuf::Message& msg) {
    std::string body;
    msg.SerializeToString(&body);
    uint32_t len = body.size();
    write(fd, &len, 4);
    write(fd, body.data(), body.size());
}

// 读
bool ReadMessage(int fd, google::protobuf::Message* msg) {
    uint32_t len = 0;
    if (read(fd, &len, 4) != 4) return false;
    if (len == 0 || len > kMaxBodySize) return false;   // ← 长度上限校验不可省
    std::string body(len, '\0');
    if (read(fd, body.data(), len) != (ssize_t)len) return false;
    return msg->ParseFromArray(body.data(), len);
}
```

> **注意那个长度上限校验**：这与第 9 节讨论的网络侧问题是同一个——凡是「对端给的长度直接决定我读多少字节」，就必须有上限。

---

## 8. 端到端完整示例

### 8.1 定义服务（`user.proto` 节选）

```protobuf
syntax = "proto3";
package fixbug;
option cc_generic_services = true;

message LoginRequest {
    bytes username = 1;
    bytes password = 2;
}
message LoginResponse {
    ResultCode result = 1;
    bool success = 2;
}
service UserServiceRPC {
    rpc Login(LoginRequest) returns(LoginResponse);
}
```

### 8.2 服务端：实现并发布

```cpp
#include "user.pb.h"
#include "rpcapplication.h"
#include "rpcprovider.h"

class UserService : public fixbug::UserServiceRPC {
public:
    void Login(::google::protobuf::RpcController* controller,
               const ::fixbug::LoginRequest* request,
               ::fixbug::LoginResponse* response,
               ::google::protobuf::Closure* done) override {
        // ① 读取请求字段
        const std::string name = request->username();
        const std::string pwd  = request->password();

        // ② 业务级校验（protobuf 只保证结构合法，不保证语义合法）
        if (name.empty() || pwd.empty()) {
            response->mutable_result()->set_errcode(1);
            response->mutable_result()->set_errmsg("username or password is empty");
            response->set_success(false);
            done->Run();
            return;
        }

        // ③ 执行本地业务
        const bool ok = DoLocalLogin(name, pwd);

        // ④ 填充响应（message 字段用 mutable_xxx()）
        fixbug::ResultCode* code = response->mutable_result();
        code->set_errcode(ok ? 0 : 2);
        code->set_errmsg(ok ? "" : "invalid credential");
        response->set_success(ok);

        // ⑤ 必须调用，触发序列化 + 异步发送
        done->Run();
    }
};

int main(int argc, char* argv[]) {
    RpcApplication::Init(argc, argv);
    RpcProvider provider;
    provider.NotifyService(new UserService());   // 注册进框架，构建 serviceMap_
    provider.Run();                              // 阻塞，等待远程调用
}
```

### 8.3 客户端：调用

```cpp
#include "user.pb.h"
#include "rpcchannel.h"
#include "rpccontroller.h"

int main(int argc, char** argv) {
    RpcApplication::Init(argc, argv);

    fixbug::UserServiceRPC_Stub stub(new RpcChannel());

    RpcController controller;                      // 框架级错误通道
    fixbug::LoginRequest request;
    request.set_username("zhang san");
    request.set_password("123456");
    fixbug::LoginResponse response;

    stub.Login(&controller, &request, &response, nullptr);

    if (controller.Failed()) {                     // ① 先看框架级错误
        std::cerr << "rpc error: " << controller.ErrorText() << "\n";
        return 1;
    }
    if (response.result().errcode() != 0) {        // ② 再看业务级错误
        std::cerr << "business error: " << response.result().errmsg() << "\n";
        return 1;
    }
    std::cout << "login success = " << response.success() << "\n";
}
```

### 8.4 手工拼一帧（验证协议、做半包/粘包测试时用）

```cpp
// 来自 example/caller/calluserservice.cpp:63-83
fixbug::LoginRequest req;
req.set_username("half_packet_user");
req.set_password("half_pwd");

std::string args_str;
req.SerializeToString(&args_str);                     // 1. 序列化请求体

rpcheader::RpcHeader rpc_header;                      // 2. 构造协议头
rpc_header.set_service_name("UserServiceRPC");
rpc_header.set_method_name("Login");
rpc_header.set_args_size(args_str.size());

std::string header_str;
rpc_header.SerializeToString(&header_str);            // 3. 序列化协议头

uint32_t header_size_net = htonl(header_str.size());  // 4. 长度转网络字节序

std::string full_frame;
full_frame.append((char*)&header_size_net, 4);        // 5. [4B][header][args]
full_frame.append(header_str);
full_frame.append(args_str);
```

---

## 9. 约束、陷阱与注意事项

### 9.1 必须记住的三条硬约束

| 约束 | 后果 | 位置 |
|---|---|---|
| `.proto` 必须写 `option cc_generic_services = true;` | 否则不生成 Service/Stub，框架路由完全失效 | `example/proto_gen/user.proto:6` |
| 字段号（`= 1`、`= 2`）一旦上线**不能改语义**，删字段要 `reserved` | 破坏前后兼容，滚动升级时整包解析错位 | 协议设计层面 |
| 生成的 `*.pb.cc` / `*.pb.h` **已纳入版本管理** | 改完 `.proto` 后必须重新 `protoc` 并提交生成文件；反过来，不跑 `protoc` 就用旧生成代码会与 `.proto` 不一致 | `src/CMakeLists.txt:10`、`example/*/CMakeLists.txt:3` |

### 9.2 协议层：长度上限只做了一半

- **请求头 `header_size`**：已校验 `∈ [1, 64KB]`，超限断连（`src/rpcprovider.cpp:175`）。
- **请求体 `args_size`**：**未校验**。`args_size` 来自对端，直接参与 `frame_size` 计算（`src/rpcprovider.cpp:199-200`），理论上需要一个 64MB 上限。
- **响应体 `response_len`**：**客户端完全未校验**：

  ```cpp
  // src/rpcchannel.cpp:105-111
  boost::asio::read(socket, boost::asio::buffer(&response_len, sizeof(response_len)));
  response_len = ntohl(response_len);
  std::vector<char> response_body(response_len);   // ← 直接按对端给的长度分配
  ```

  一个恶意或损坏的服务端返回 `0xFFFFFFFF` 会让客户端立刻尝试分配约 4GB。**客户端侧的风险实际上高于服务端**，因为服务端至少是逐 4KB 累积、可以中途断连。

- 长度运算的溢出：请求侧已把 `4 + header_size + args_size` 提升到 `size_t` 计算（`src/rpcprovider.cpp:199-200`）；客户端的 `response_len` 路径尚无类似保护。
- 协议头**没有 magic number / 版本号**，因此任何 4 字节都可能被当作合法长度。加上 magic 可以让「把非本协议数据当帧解析」这种情况直接被识别。

> 这与 `docs/aprpc-project-review-and-interview-guide.md` 第 15 节 P0 第 1 项「增加请求头、请求体和响应体长度上限，并做无溢出的长度运算」是同一件事，改造时请以该文档为准。

### 9.3 `proto3` 的字段存在性语义

- 标量字段（`uint32` / `bool` / `bytes` / `string`）**没有 `has_xxx()`**，无法区分「未设置」与「设置为默认值」。
- 只有 message 类型字段（及 proto3 `optional` 标量）才有 `has_xxx()`。
- `IsInitialized()` 在 proto3 下恒为 `true`，**不能用来做校验**（因为 proto3 移除了 `required`）。

### 9.4 空指针风险：`controller` 未做完整判空

`RpcChannel::CallMethod` 内部对 `controller` 的判空**不一致**：

```cpp
// src/rpcchannel.cpp:31-35 —— 没有判空，直接解引用
if (!request->SerializeToString(&args_str)) {
    controller->SetFailed("request SerializeToString failed!");
    return;
}

// src/rpcchannel.cpp:74-78 —— 有判空
if (controller != nullptr) {
    controller->SetFailed(method_path + " is not exist!");
}
```

而示例中 `stub.Login(nullptr, &request, &response, nullptr)`（`example/caller/calluserservice.cpp:27`）确实传了 `nullptr`。因此**在序列化失败时会触发空指针解引用崩溃**。调用 Stub 时传非空 `controller` 可规避，这也是 8.3 示例中推荐始终传 `controller` 的原因。

### 9.5 `Closure` 生命周期

- `NewCallback` 返回的对象是堆分配的，**protobuf 不自动释放**，当前 `HandleRequest` 未 `delete`，每请求泄漏一个对象。
- 业务方法**必须调用 `done->Run()`**，否则客户端永远等不到响应（框架目前没有超时兜底）。
- `done->Run()` 必须在 `request` 仍有效时调用——`request` 在 `HandleRequest` 返回后被 `delete`（`src/rpcprovider.cpp:329`）。

### 9.6 其他

- **`set_args_size(args_str.size())` 的窄化**：`size_t` → `uint32`，超 4GB 会截断（当前受 64MB 约定限制不会触发，但值得加断言）。
- **生成代码的版本敏感性**：当前生成文件使用 `::google::protobuf::` 全限定名与 `LANG_CXX11` 宏，属于 protobuf 3.x 较早版本的输出风格。**升级 protobuf 后必须重新 `protoc`**，否则可能因 `PROTOBUF_NAMESPACE_ID` / Abseil 依赖变化而编译失败。
- **`CopyFrom` / `MergeFrom` 的选择**：复用同一个 message 对象解析多条消息时，若用 `MergeFromString` 会产生数据累积，应使用 `ParseFromString`（内部先 `Clear`）。

---

## 10. 附录：方法速查表

### 10.1 框架实际使用（含代码位置）

| 类别 | 方法 | 输入 | 输出 | 使用位置 |
|---|---|---|---|---|
| 创建 | `T value;` | — | 栈上空对象 | `calluserservice.cpp:21,25,37,42` |
| 创建 | `M* New()` | — | 堆上新同类型对象（需 `delete`） | `rpcprovider.cpp:306,312` |
| 创建 | `GetRequestPrototype(m)` | `MethodDescriptor*` | 请求原型（只读引用） | `rpcprovider.cpp:306` |
| 创建 | `GetResponsePrototype(m)` | `MethodDescriptor*` | 响应原型（只读引用） | `rpcprovider.cpp:312` |
| 序列化 | `SerializeToString(std::string*)` | 输出容器 | `bool` | `rpcchannel.cpp:31,44`；`rpcprovider.cpp:344` |
| 反序列化 | `ParseFromArray(const void*, int)` | 缓冲区 + 长度 | `bool` | `rpcprovider.cpp:190,307`；`calluserservice.cpp:126`；`rpc_bench.cpp:110,118` |
| 反序列化 | `ParseFromString(const std::string&)` | 字符串 | `bool` | `rpcchannel.cpp:115` |
| 字段写 | `set_foo(v)` | 新值 | — | `rpcchannel.cpp:39-41`；`calluserservice.cpp:22,23,38-40`；`userservice.cpp:61,62,63,89,90,91` |
| 字段写 | `mutable_foo()`（message/string） | — | 可写指针（自动创建） | `userservice.cpp:59,88` |
| 字段读 | `foo()` | — | 值 / const 引用 | `rpcprovider.cpp:213-214`；`userservice.cpp:51,52,82,83,84`；`calluserservice.cpp:30,31,50,51` |
| 元编程 | `GetDescriptor()` | — | `ServiceDescriptor*` | `rpcprovider.cpp:20` |
| 元编程 | `ServiceDescriptor::name()` | — | `const std::string&` | `rpcprovider.cpp:22` |
| 元编程 | `ServiceDescriptor::method_count()` | — | `int` | `rpcprovider.cpp:23` |
| 元编程 | `ServiceDescriptor::method(i)` | 下标 | `MethodDescriptor*` | `rpcprovider.cpp:24` |
| 元编程 | `MethodDescriptor::name()` | — | `const std::string&` | `rpcprovider.cpp:25` |
| 分发 | `Service::CallMethod(...)` | 描述符 + req/resp + done | — | `rpcprovider.cpp:326` |
| 回调 | `NewCallback<...>(obj, fn, args...)` | 对象 + 成员函数 + 参数 | `Closure*`（需 `delete`） | `rpcprovider.cpp:315-322` |
| 回调 | `Closure::Run()` | — | — | `userservice.cpp:67,93` |
| 控制器 | `SetFailed(reason)` | 原因 | — | `rpcchannel.cpp:33,45,75,83,116` |
| 控制器 | `Failed()` / `ErrorText()` | — | `bool` / `std::string` | `calluserservice.cpp:47,48` |

### 10.2 生成代码提供但当前项目未使用

| 方法 | 用途 | 备注 |
|---|---|---|
| `SerializeAsString()` | 直接返回序列化字符串 | 无错误反馈 |
| `SerializeToArray(buf, size)` | 写入已有缓冲区 | 配合 `ByteSizeLong()` 可零拷贝 |
| `SerializePartialToString()` | 容忍未初始化字段 | proto3 下与 `SerializeToString` 等价 |
| `ByteSizeLong()` | 预估序列化长度 | 用于一次 `resize` 避免重复分配 |
| `ParseFromIstream()` | 从 `istream` 解析 | 文件/流场景 |
| `MergeFromString()` / `MergeFrom()` | 合并语义解析 | 注意与 `ParseFromString` 的区别 |
| `ParsePartialFromString()` | 容忍未初始化字段 | — |
| `Clear()` | 重置全部字段 | 对象复用场景 |
| `CopyFrom()` | 深拷贝 | 生成代码内部用于拷贝赋值 |
| `Swap()` | O(1) 交换内容 | 双缓冲式优化可用 |
| `has_xxx()` | 字段存在性 | 仅 message 字段生成（如 `has_result()`） |
| `clear_xxx()` | 重置单个字段 | — |
| `release_xxx()` | 释放所有权 | — |
| `set_allocated_xxx()` | 接管所有权 | — |
| `IsInitialized()` | 是否全部必填字段已设置 | **proto3 恒为 `true`** |
| `NewCallback` 的 `NewPermanentCallback` | 可多次执行的闭包 | 仍由调用方释放 |
| `Arena` 相关接口 | 批量内存池分配 | 高吞吐场景可降低分配开销 |

### 10.3 方法命名规则总结

对任意字段 `foo`：

```
foo()                     → 读
set_foo(v)                → 写（非 repeated）
mutable_foo()             → 取可写指针（string/bytes/message/repeated）
clear_foo()               → 重置
has_foo()                 → 存在性（仅 message 字段 / proto3 optional）
release_foo()             → 释放所有权（string/message）
set_allocated_foo(p)      → 接管所有权（string/message）
add_foo(v)                → 追加元素（repeated）
foo_size() / foo(i)       → repeated 元素数 / 第 i 个元素
```

---

## 参考

- `src/rpcheader/rpcheader.proto` — 内部协议头定义
- `example/proto_gen/user.proto` — 示例业务服务定义
- `src/rpcchannel.cpp` — 客户端序列化、发送、响应解析
- `src/rpcprovider.cpp` — 服务端切帧、反序列化、通用路由
- `src/include/rpcprovider.h`、`src/include/rpcchannel.h`、`src/include/rpccontroller.h` — 接口声明与注释
- `example/callee/userservice.cpp`、`example/caller/calluserservice.cpp`、`example/benchmark/rpc_bench.cpp` — 使用示例
- `docs/aprpc-project-review-and-interview-guide.md` — 第 8 节（Protobuf 知识点）、第 12.5 节（口径差异）、第 15 节（演进优先级）
- `AGENTS.md` — 协议约定与构建说明
