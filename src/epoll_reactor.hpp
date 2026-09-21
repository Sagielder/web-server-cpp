#pragma once
#include "file_descriptor.hpp"
#include <sys/epoll.h>
#include <vector>
#include <stdexcept>
#include <cerrno>
#include <cstring>

class EpollReactor
{
private:
    MyFileDescriptor m_epoll_fd;
    static constexpr int MAX_EVENTS = 10;

public:
    EpollReactor() : m_epoll_fd(epoll_create1(0)) {
        if (m_epoll_fd.GetFileDescriptor() == -1) {
            throw std::runtime_error(std::string("epoll_create1 failed: ") + std::strerror(errno));
        }
    }

    ~EpollReactor() = default;

    int Add(const int& fd, const uint32_t& events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        return epoll_ctl(m_epoll_fd.GetFileDescriptor(), EPOLL_CTL_ADD, fd, &ev);
    }

    int Modify(const int& fd, const uint32_t& events) {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        return epoll_ctl(m_epoll_fd.GetFileDescriptor(), EPOLL_CTL_MOD, fd, &ev);
    }

    int Remove(const int& fd) {
        return epoll_ctl(m_epoll_fd.GetFileDescriptor(), EPOLL_CTL_DEL, fd, nullptr);
    }

    // Blocks until at least one fd is ready
    // just the ready events (length == nfds, not MAX_EVENTS).
    std::vector<epoll_event> Wait() {
        while (1) {
            std::vector<epoll_event> events(MAX_EVENTS);
            int nfds = epoll_wait(m_epoll_fd.GetFileDescriptor(), events.data(), MAX_EVENTS, -1);
            if (nfds < 0) {
                if (errno == EINTR) continue; // interrupted by a signal, not a real failure
                throw std::runtime_error(std::string("epoll_wait failed: ") + std::strerror(errno));
            }
            events.resize(static_cast<size_t>(nfds));
            return events;
        }
    }
};
