#ifndef CONNECTION_POOL_HPP
#define CONNECTION_POOL_HPP

#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include "common/packet.hpp"

class ClientConnection {
public:
    int fd;
    std::string nickname;
    std::string ip;
    int port;
    time_t connect_time;
    
    // 读缓冲区
    std::vector<char> read_buffer;
    size_t packet_length;
    
    ClientConnection(int client_fd, const std::string& client_ip, int client_port);
    bool append_data(const char* data, size_t len);
    std::vector<Packet> extract_packets();
};

class ConnectionPool {
public:
    static ConnectionPool& instance();
    
    // 添加/移除连接
    bool add_connection(int fd, const std::string& ip, int port);
    bool remove_connection(int fd);
    
    // 查找连接
    std::shared_ptr<ClientConnection> get_connection(int fd);
    
    // 广播消息
    void broadcast(const Packet& packet, int exclude_fd = -1);
    
    // 获取所有连接
    std::vector<int> get_all_fds() const;
    
    // 获取在线用户数
    size_t size() const;
    
    // 设置昵称
    bool set_nickname(int fd, const std::string& nickname);
    
private:
    ConnectionPool() = default;
    ~ConnectionPool() = default;
    
    mutable std::mutex mutex_;
    std::unordered_map<int, std::shared_ptr<ClientConnection>> connections_;
};
#endif // CONNECTION_POOL_HPP