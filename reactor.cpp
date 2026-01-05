#ifndef REACTOR_HPP
#define REACTOR_HPP

#include <sys/epoll.h>
#include <functional>
#include <unordered_map>
#include <memory>
#include <vector>
#include <atomic>
#include "common/protocol.hpp"

class Reactor {
public:
    using EventCallback = std::function<void(int fd, uint32_t events)>;
    
    Reactor(int max_events = 1024);
    ~Reactor();
    
    // 添加/修改/删除事件监听
    bool add_event(int fd, uint32_t events, const EventCallback& cb);
    bool mod_event(int fd, uint32_t events, const EventCallback& cb);
    bool del_event(int fd);
    
    // 运行事件循环
    void run();
    void stop();
    
private:
    int epoll_fd_;
    int max_events_;
    std::atomic<bool> running_;
    std::unordered_map<int, EventCallback> callbacks_;
    std::unique_ptr<epoll_event[]> events_;
    
    void handle_events(int ready_num);
};

#endif // REACTOR_HPP