#include <iostream>
#include <cstring>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <termios.h>
#include "common/protocol.hpp"
#include "common/utils.hpp"

class ChatClient {
public:
    ChatClient(const std::string& server_ip, int port)
        : server_ip_(server_ip), port_(port), running_(false) {}
    
    bool connect_to_server() {
        // 创建socket
        sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd_ < 0) {
            perror("socket creation failed");
            return false;
        }
        
        // 设置服务器地址
        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port_);
        
        if (inet_pton(AF_INET, server_ip_.c_str(), &serv_addr.sin_addr) <= 0) {
            std::cerr << "Invalid address/Address not supported" << std::endl;
            return false;
        }
        
        // 连接服务器
        if (connect(sockfd_, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            perror("connection failed");
            return false;
        }
        
        std::cout << "Connected to server " << server_ip_ << ":" << port_ << std::endl;
        return true;
    }
    
    void start() {
        running_ = true;
        
        // 启动接收线程
        std::thread recv_thread(&ChatClient::receive_messages, this);
        
        // 主线程处理用户输入
        handle_user_input();
        
        // 等待接收线程结束
        recv_thread.join();
    }
    
    void stop() {
        running_ = false;
        if (sockfd_ >= 0) {
            close(sockfd_);
            sockfd_ = -1;
        }
    }
    
private:
    void handle_user_input() {
        std::string input;
        
        std::cout << "\nCommands:\n"
                  << "  /nick <name>  - Set nickname\n"
                  << "  /users        - List online users\n"
                  << "  /help         - Show help\n"
                  << "  /quit         - Exit\n\n";
        
        while (running_) {
            std::cout << "> ";
            std::getline(std::cin, input);
            
            if (!running_) break;
            
            if (input.empty()) continue;
            
            if (input == "/quit") {
                std::cout << "Goodbye!" << std::endl;
                stop();
                break;
            }
            
            // 处理命令
            if (input[0] == '/') {
                Packet packet;
                packet.type = MSG_TYPE_COMMAND;
                packet.length = std::min(input.size(), (size_t)MAX_DATA_SIZE);
                memcpy(packet.data, input.c_str(), packet.length);
                send_packet(packet);
            }
            else if (input.find("/nick ") == 0) {
                // 设置昵称
                std::string nickname = input.substr(6);
                Packet packet;
                packet.type = MSG_TYPE_SET_NICKNAME;
                packet.length = std::min(nickname.size(), (size_t)MAX_DATA_SIZE);
                memcpy(packet.data, nickname.c_str(), packet.length);
                send_packet(packet);
            }
            else {
                // 发送聊天消息
                Packet packet;
                packet.type = MSG_TYPE_CHAT;
                packet.length = std::min(input.size(), (size_t)MAX_DATA_SIZE);
                memcpy(packet.data, input.c_str(), packet.length);
                send_packet(packet);
            }
        }
    }
    
    void receive_messages() {
        std::vector<char> buffer(4096);
        std::vector<char> packet_buffer;
        size_t expected_len = 0;
        
        while (running_) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sockfd_, &readfds);
            
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            
            int ret = select(sockfd_ + 1, &readfds, nullptr, nullptr, &tv);
            
            if (ret < 0) {
                perror("select error");
                break;
            }
            
            if (ret == 0) continue; // 超时
            
            if (FD_ISSET(sockfd_, &readfds)) {
                ssize_t bytes_read = recv(sockfd_, buffer.data(), buffer.size(), 0);
                
                if (bytes_read <= 0) {
                    if (bytes_read == 0) {
                        std::cout << "\nServer disconnected" << std::endl;
                    } else {
                        perror("recv error");
                    }
                    stop();
                    break;
                }
                
                // 处理接收到的数据
                packet_buffer.insert(packet_buffer.end(), buffer.begin(), 
                                     buffer.begin() + bytes_read);
                
                // 解析数据包
                while (!packet_buffer.empty()) {
                    if (expected_len == 0) {
                        // 检查是否有完整的包头
                        if (packet_buffer.size() < sizeof(PacketHeader)) {
                            break;
                        }
                        
                        PacketHeader header;
                        memcpy(&header, packet_buffer.data(), sizeof(PacketHeader));
                        expected_len = ntohl(header.length) + sizeof(PacketHeader);
                    }
                    
                    // 检查是否有完整的数据包
                    if (packet_buffer.size() >= expected_len) {
                        // 提取数据包
                        Packet packet;
                        memcpy(&packet.header, packet_buffer.data(), sizeof(PacketHeader));
                        
                        size_t data_len = expected_len - sizeof(PacketHeader);
                        packet.length = std::min(data_len, (size_t)MAX_DATA_SIZE);
                        memcpy(packet.data, packet_buffer.data() + sizeof(PacketHeader), 
                               packet.length);
                        
                        // 显示消息
                        display_message(packet);
                        
                        // 移除已处理的数据
                        packet_buffer.erase(packet_buffer.begin(), 
                                           packet_buffer.begin() + expected_len);
                        expected_len = 0;
                    } else {
                        break;
                    }
                }
            }
        }
    }
    
    void display_message(const Packet& packet) {
        std::string message(packet.data, packet.length);
        
        // 根据消息类型显示
        switch (packet.type) {
            case MSG_TYPE_CHAT:
                std::cout << "\n" << message << std::endl;
                break;
                
            case MSG_TYPE_SYSTEM:
                std::cout << "\n[System] " << message << std::endl;
                break;
                
            default:
                break;
        }
        
        std::cout << "> " << std::flush;
    }
    
    bool send_packet(const Packet& packet) {
        // 发送数据包（包含长度字段）
        uint32_t total_len = sizeof(PacketHeader) + packet.length;
        std::vector<char> buffer(total_len);
        
        // 设置包头（网络字节序）
        PacketHeader header;
        header.length = htonl(packet.length);
        memcpy(buffer.data(), &header, sizeof(PacketHeader));
        memcpy(buffer.data() + sizeof(PacketHeader), packet.data, packet.length);
        
        ssize_t sent = send(sockfd_, buffer.data(), total_len, 0);
        return sent == (ssize_t)total_len;
    }
    
private:
    std::string server_ip_;
    int port_;
    int sockfd_;
    std::atomic<bool> running_;
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <port>" << std::endl;
        return 1;
    }
    
    std::string server_ip = argv[1];
    int port = atoi(argv[2]);
    
    ChatClient client(server_ip, port);
    
    if (!client.connect_to_server()) {
        return 1;
    }
    
    client.start();
    
    return 0;
}