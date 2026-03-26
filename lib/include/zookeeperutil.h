#pragma once

#include <semaphore.h>
#include <string>
#include <zookeeper/zookeeper.h>

// 封装的zk客户端类
class ZkClient
{
public:
    ZkClient();
    ~ZkClient();

    /* 
     * @brief zkclient启动连接zkserver
     **/
    void Start();

    /*
     * @brief zkclient在zkserver上创建znode节点
     * @param path znode节点路径
     * @param data znode节点数据
     * @param datalen znode节点数据长度
     * @param state znode节点状态（永久性节点/临时性节点）
     **/
    void Create(const char *path, const char *data, int datalen, int state=0);

    /*
     * @brief 根据参数指定的znode节点路径，获取znode节点的值
     **/
    std::string GetData(const char *path);

private:
    // zookeeper的客户端句柄
    zhandle_t *m_zhandle;
};

