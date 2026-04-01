#include "zookeeperutil.h"
#include "rpcapplication.h"
#include <semaphore.h>
#include <iostream>

// 全局的watcher观察器   zkserver给zkclient的通知
// 触发条件：zkclient和zkserver连接成功后，zkserver会给zkclient发送一个通知，触发这个观察器的回调函数
void global_watcher(zhandle_t *zh, int type, int state, const char *path, void *watcherCtx)
{   
    // type 表示回调的消息类型，state 表示zkclient和zkserver连接状态 path 表示zkserver给zkclient发送通知的znode节点路径，\
    watcherCtx 表示用户自定义的上下文对象，这里是一个信号量指针
    // 当前只处理zkclient和zkserver连接成功的通知，其他类型的通知暂不处理
    if (type == ZOO_SESSION_EVENT)  // 回调的消息类型是和会话相关的消息类型
    {
        if (state == ZOO_CONNECTED_STATE)  // zkclient和zkserver连接成功
        {   
            // 通过zoo_get_context()获取到之前在zookeeper_init()中设置的上下文对象，这里是一个信号量指针
            sem_t *sem = (sem_t*)zoo_get_context(zh);
            sem_post(sem);  // 连接成功，释放信号量，让zookeeper_init()函数返回
        }
    }
}

ZkClient::ZkClient() : m_zhandle(nullptr){}

ZkClient::~ZkClient()
{
    if (m_zhandle != nullptr)
        zookeeper_close(m_zhandle); // 关闭句柄，释放资源
}

// 连接zkserver
void ZkClient::Start()
{
    std::string ip = RpcApplication::GetInstance().GetConfig().Load<std::string>("zookeeper.server_ip");
    std::string port = RpcApplication::GetInstance().GetConfig().Load<std::string>("zookeeper.server_port");
    std::string connstr = ip + ":" + port;
    
    /*
    zookeeper_mt：多线程版本
    zookeeper的API客户端程序提供了三个线程
    1. API调用线程 
    2. 网络I/O线程  pthread_create  poll
    3. watcher回调线程 pthread_create
    */
    // zookeeper_init()函数会创建一个新的线程来处理网络I/O和watcher回调，所以这个函数是非阻塞的，\
    调用后会立即返回，返回值是一个zhandle_t类型的指针，表示zkclient的句柄，如果连接失败则返回nullptr
    m_zhandle = zookeeper_init(connstr.c_str(), global_watcher, 30000, nullptr, nullptr, 0);
    if (nullptr == m_zhandle) 
    {
        std::cout << "zookeeper_init error!" << std::endl;
        exit(EXIT_FAILURE);
    }

    sem_t sem;  // 定义一个信号量对象
    sem_init(&sem, 0, 0);   // 初始化信号量，初始值为0，第二个参数为0表示线程间共享
    zoo_set_context(m_zhandle, &sem);   // 在zookeeper_init()函数中设置上下文对象，这里是一个信号量指针，供全局watcher观察器使用

    sem_wait(&sem); // sem_wait会对信号减一操作,如果信号量的值为0，则调用线程会被阻塞，直到信号量的值大于0时才会继续执行
    std::cout << "zookeeper_init success!" << std::endl;
}

void ZkClient::Create(const char *path, const char *data, int datalen, int state)
{
    char path_buffer[128];
    int bufferlen = sizeof(path_buffer);
    int flag;
    // 判断path表示的znode节点是否存在，如果存在，就不重复创建了
    flag = zoo_exists(m_zhandle, path, 0, nullptr);
    if (ZNONODE == flag) // 表示path的znode节点不存在
    {
        // 创建指定path的znode节点
        flag = zoo_create(m_zhandle, path, data, datalen,
            &ZOO_OPEN_ACL_UNSAFE, state, path_buffer, bufferlen);
        if (flag == ZOK)
        {
            std::cout << "znode create success... path:" << path << std::endl;
        }
        else
        {
            std::cout << "flag:" << flag << std::endl;
            std::cout << "znode create error... path:" << path << std::endl;
            exit(EXIT_FAILURE);
        }
    }
}

// 根据指定的path，获取znode节点的值
std::string ZkClient::GetData(const char *path)
{
    char buffer[64];
    int bufferlen = sizeof(buffer);
    int flag = zoo_get(m_zhandle, path, 0, buffer, &bufferlen, nullptr);
    if (flag != ZOK)
    {
        std::cout << "get znode error... path:" << path << std::endl;
        return "";
    }
    else
    {
        return buffer;
    }
}
