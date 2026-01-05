#include <iostream>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "reactor.hpp"
#include "connection_pool.hpp"
#include "common/protocol.hpp"
#include "common/utils.hpp"

class ChatServer {
public:
    ChatServer(int port = 8888, int backlog = 128) 
        : port_(port), backlog_(backlog), reactor_(std::make_unique<Reactor>()) {}
    
    bool init() {
        // 创建socket
        server_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (server_fd_ < 0) {
            perror("socket creation failed");
            return false;
        }
        
        // 设置SO_REUSEADDR
        int opt = 1;
        if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("setsockopt failed");
            return false;
        }
        
        // 绑定地址
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);
        
        if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind failed");
            return false;
        }
        
        // 开始监听
        if (listen(server_fd_, backlog_) < 0) {
            perror("listen failed");
            return false;
        }
        
        std::cout << "Chat server started on port " << port_ << std::endl;
        return true;
    }
    
    void start() {
        // 添加服务器socket到reactor
        reactor_->add_event(server_fd_, EPOLLIN | EPOLLET, 
            [this](int fd, uint32_t events) {
                handle_accept(fd);
            });
        
        // 启动reactor
        reactor_->run();
    }
    
    void stop() {
        reactor_->stop();
        close(server_fd_);
        std::cout << "Chat server stopped." << std::endl;
    }
    
private:
    void handle_accept(int server_fd) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        while (true) {
            int client_fd = accept4(server_fd, (struct sockaddr*)&client_addr, 
                                   &addr_len, SOCK_NONBLOCK);
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; // 没有更多连接
                }
                perror("accept failed");
                break;
            }
            
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
            int client_port = ntohs(client_addr.sin_port);
            
            std::cout << "New connection from " << client_ip << ":" 
                      << client_port << " (fd: " << client_fd << ")" << std::endl;
            
            // 添加到连接池
            if (ConnectionPool::instance().add_connection(client_fd, client_ip, client_port)) {
                // 添加到reactor
                reactor_->add_event(client_fd, EPOLLIN | EPOLLET | EPOLLRDHUP,
                    [this](int fd, uint32_t events) {
                        handle_client(fd, events);
                    });
                
                // 发送欢迎消息
                Packet welcome_pkt;
                welcome_pkt.type = MSG_TYPE_SYSTEM;
                welcome_pkt.length = snprintf(welcome_pkt.data, MAX_DATA_SIZE,
                    "Welcome to the chat room! Please set your nickname with /nick <name>");
                send_packet(client_fd, welcome_pkt);
            } else {
                close(client_fd);
            }
        }
    }
    
    void handle_client(int client_fd, uint32_t events) {
        if (events & EPOLLRDHUP || events & EPOLLHUP || events & EPOLLERR) {
            // 连接关闭或出错
            handle_disconnect(client_fd);
            return;
        }
        
        if (events & EPOLLIN) {
            handle_read(client_fd);
        }
    }
    
    void handle_read(int client_fd) {
        char buffer[4096];
        ssize_t bytes_read;
        
        while (true) {
            bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);
            if (bytes_read <= 0) {
                if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break; // 数据读完
                }
                handle_disconnect(client_fd);
                break;
            }
            
            // 添加到连接缓冲区
            auto conn = ConnectionPool::instance().get_connection(client_fd);
            if (conn && conn->append_data(buffer, bytes_read)) {
                // 提取完整的数据包
                auto packets = conn->extract_packets();
                for (const auto& packet : packets) {
                    process_packet(client_fd, packet);
                }
            }
        }
    }
    
    void process_packet(int client_fd, const Packet& packet) {
        auto conn = ConnectionPool::instance().get_connection(client_fd);
        if (!conn) return;
        
        switch (packet.type) {
            case MSG_TYPE_SET_NICKNAME: {
                std::string nickname(packet.data, packet.length);
                ConnectionPool::instance().set_nickname(client_fd, nickname);
                
                // 通知所有用户
                Packet sys_msg;
                sys_msg.type = MSG_TYPE_SYSTEM;
                sys_msg.length = snprintf(sys_msg.data, MAX_DATA_SIZE,
                    "User %s has joined the chat room", nickname.c_str());
                ConnectionPool::instance().broadcast(sys_msg);
                break;
            }
            
            case MSG_TYPE_CHAT: {
                // 构建聊天消息
                Packet chat_msg;
                chat_msg.type = MSG_TYPE_CHAT;
                
                if (conn->nickname.empty()) {
                    // 未设置昵称
                    Packet error_msg;
                    error_msg.type = MSG_TYPE_SYSTEM;
                    error_msg.length = snprintf(error_msg.data, MAX_DATA_SIZE,
                        "Please set your nickname first with /nick <name>");
                    send_packet(client_fd, error_msg);
                    return;
                }
                
                // 格式: [昵称] 消息
                chat_msg.length = snprintf(chat_msg.data, MAX_DATA_SIZE,
                    "[%s] %.*s", conn->nickname.c_str(), 
                    (int)packet.length, packet.data);
                
                // 广播消息
                ConnectionPool::instance().broadcast(chat_msg);
                break;
            }
            
            case MSG_TYPE_COMMAND: {
                handle_command(client_fd, std::string(packet.data, packet.length));
                break;
            }
            
            default:
                break;
        }
    }
    
    void handle_command(int client_fd, const std::string& cmd) {
        auto conn = ConnectionPool::instance().get_connection(client_fd);
        if (!conn) return;
        
        Packet response;
        response.type = MSG_TYPE_SYSTEM;
        
        if (cmd == "/users") {
            // 列出在线用户
            auto fds = ConnectionPool::instance().get_all_fds();
            std::string user_list = "Online users (" + std::to_string(fds.size()) + "):\n";
            
            for (int fd : fds) {
                auto user = ConnectionPool::instance().get_connection(fd);
                if (user && !user->nickname.empty()) {
                    user_list += "  " + user->nickname + "\n";
                }
            }
            
            response.length = std::min(user_list.size(), (size_t)MAX_DATA_SIZE);
            memcpy(response.data, user_list.c_str(), response.length);
            send_packet(client_fd, response);
        }
        else if (cmd.find("/nick ") == 0) {
            // 设置昵称
            std::string nickname = cmd.substr(6);
            if (!nickname.empty()) {
                ConnectionPool::instance().set_nickname(client_fd, nickname);
                response.length = snprintf(response.data, MAX_DATA_SIZE,
                    "Nickname set to: %s", nickname.c_str());
                send_packet(client_fd, response);
            }
        }
        else if (cmd == "/help") {
            std::string help = "Available commands:\n"
                               "  /nick <name>  - Set your nickname\n"
                               "  /users        - List online users\n"
                               "  /help         - Show this help\n"
                               "  /quit         - Exit chat room";
            response.length = std::min(help.size(), (size_t)MAX_DATA_SIZE);
            memcpy(response.data, help.c_str(), response.length);
            send_packet(client_fd, response);
        }
    }
    
    void handle_disconnect(int client_fd) {
        auto conn = ConnectionPool::instance().get_connection(client_fd);
        if (conn) {
            std::cout << "Connection closed: " << conn->ip << ":" 
                      << conn->port << " (" << conn->nickname << ")" << std::endl;
            
            // 通知其他用户
            if (!conn->nickname.empty()) {
                Packet leave_msg;
                leave_msg.type = MSG_TYPE_SYSTEM;
                leave_msg.length = snprintf(leave_msg.data, MAX_DATA_SIZE,
                    "User %s has left the chat room", conn->nickname.c_str());
                ConnectionPool::instance().broadcast(leave_msg, client_fd);
            }
        }
        
        // 从reactor移除
        reactor_->del_event(client_fd);
        
        // 从连接池移除
        ConnectionPool::instance().remove_connection(client_fd);
        
        // 关闭socket
        close(client_fd);
    }
    
    bool send_packet(int fd, const Packet& packet) {
        // 发送数据包（包含长度字段）
        uint32_t total_len = sizeof(PacketHeader) + packet.length;
        std::vector<char> buffer(total_len);
        memcpy(buffer.data(), &packet.header, sizeof(PacketHeader));
        memcpy(buffer.data() + sizeof(PacketHeader), packet.data, packet.length);
        
        ssize_t sent = send(fd, buffer.data(), total_len, 0);
        return sent == (ssize_t)total_len;
    }
    
private:
    int port_;
    int backlog_;
    int server_fd_;
    std::unique_ptr<Reactor> reactor_;
};

void signal_handler(int sig) {
    std::cout << "\nReceived signal " << sig << ", shutting down..." << std::endl;
    exit(0);
}

int main(int argc, char* argv[]) {
    int port = 8888;
    
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    ChatServer server(port);
    
    if (!server.init()) {
        std::cerr << "Failed to initialize server" << std::endl;
        return 1;
    }
    
    server.start();
    
    return 0;
}