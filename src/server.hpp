#include <sys/socket.h>
#include "connection.hpp"
#include <unordered_map>
#include "epoll_reactor.hpp"
#include <memory>
#include <netinet/in.h>
#include <stdexcept>
#include <cstring>
#include <sys/uio.h>
#include "router.hpp"
#include <thread>

class Server {
private:
MyFileDescriptor m_listen_socket;
EpollReactor m_epoll_reactor;
std::unordered_map<int, std::unique_ptr<Connection>> m_connections;
Router m_router;
std::mutex m_connections_mutex;

private:

    void EraseConnFromMap(int fd) {
        m_connections_mutex.lock();
        m_connections.erase(fd);
        m_connections_mutex.unlock();
    }
    void WorkerLoop() {
        while (1) {
            std::vector<epoll_event> events;
            try {
                events = m_epoll_reactor.Wait();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "WorkerLoop: Wait() failed: %s\n", e.what());
                continue;
            }
            for (epoll_event event : events) {
                if (event.data.fd == m_listen_socket.GetFileDescriptor()) {
                    while (1) {
                        struct sockaddr_in client_addr;
                        socklen_t client_len = sizeof(client_addr);

                        // accept with non blocking flag
                        std::unique_ptr<Connection> conn = std::make_unique<Connection>(accept4(m_listen_socket.GetFileDescriptor(), 
                                (struct sockaddr *)&client_addr, &client_len, SOCK_NONBLOCK));
                        
                        if (conn->GetFileDescriptor() == -1) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                // All pending connections accepted
                                break;
                            }
                            perror("accept error");
                            break;
                        }

                        // capture the fd before moving conn - conn is null after the move
                        int new_fd = conn->GetFileDescriptor();

                        // insert into the map before Add() succeeds,
                        // the fd is immediately visible to epoll_wait() on every thread
                        m_connections_mutex.lock();
                        m_connections.try_emplace(new_fd, std::move(conn));
                        m_connections_mutex.unlock();

                        if (m_epoll_reactor.Add(new_fd, EPOLLIN | EPOLLET | EPOLLONESHOT) == -1) {
                            perror("epoll_ctl: client_fd failed");
                            // registration failed
                            // remove it from the map 
                            EraseConnFromMap(new_fd);
                        } else {
                            printf("New connection accepted: client_fd %d\n", new_fd);
                        }

                    }
                } else {
                    int client_fd = event.data.fd;
                    m_connections_mutex.lock();
                    auto it = m_connections.find(client_fd);
                    if (it == m_connections.end()) {
                        // shouldn't happen, but don't crash the whole server over one stale event
                        m_connections_mutex.unlock();
                        std::fprintf(stderr, "WorkerLoop: got event for untracked fd %d\n", client_fd);
                        continue;
                    }
                    std::unique_ptr<Connection>& conn = it->second;
                    m_connections_mutex.unlock();
                    ConnectionState conn_state = conn->Read();
                    switch (conn_state)
                    {
                    case ConnectionState::RequestComplete: {
                         // route
                        Response res = m_router.Dispatch(*conn->GetRequest());
                        WriteResponse(client_fd, res);
                        if (conn->GetKeepAlive()) {
                            // keep alive
                            conn->Reset();
                            m_epoll_reactor.Modify(client_fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
                        } else {
                            EraseConnFromMap(client_fd);
                        }
                        break;
                    }
                    case ConnectionState::BadRequest:
                        // malformed request - connection is alive, tell the client why before closing
                        WriteResponse(client_fd, Response(400, "Bad Request", "text/plain", "Bad Request"));
                        EraseConnFromMap(client_fd);
                        break;
                    case ConnectionState::URITooLong:
                        WriteResponse(client_fd, Response(414, "URI Too Long", "text/plain", "URI Too Long"));
                        EraseConnFromMap(client_fd);
                        break;
                    case ConnectionState::PayloadTooLarge:
                        WriteResponse(client_fd, Response(413, "Payload Too Large", "text/plain", "Payload Too Large"));
                        EraseConnFromMap(client_fd);
                        break;
                    case ConnectionState::ConnectionClosed:
                        EraseConnFromMap(client_fd);
                        break;
                    case ConnectionState::NeedMoreData:
                        // not finish reading
                        // rearm it
                        m_epoll_reactor.Modify(client_fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
                        break;
                    case ConnectionState::HeaderFieldsTooLarge:
                        WriteResponse(client_fd, Response(431, "Request Header Fields Too Large", "text/plain", "Request Header Fields Too Large"));
                    default:
                        break;
                    }
                }

            }
        }
        
    };

    
public:
    void WriteResponse(int client_fd, const Response& res) {
        std::string headers =
            "HTTP/1.1 " + std::to_string(res.status_code) + " " + res.response_phrase + "\r\n"
            "Content-Type:" + res.content_type + "\r\n"
            "Content-Length:" + std::to_string(res.body.size()) + "\r\n"
            "\r\n";
        struct iovec iov[2];
        iov[0].iov_base = const_cast<char*>(headers.data());
        iov[0].iov_len  = headers.size();
        iov[1].iov_base = const_cast<char*>(res.body.data());
        iov[1].iov_len  = res.body.size();

        writev(client_fd, iov, 2);
    }

    Server(int port, Router router) : m_listen_socket(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)), m_router(std::move(router)) {
        if (m_listen_socket.GetFileDescriptor() < 0) {
            throw std::runtime_error(std::string("socket creation failed: ") + std::strerror(errno));
        }

        int opt = 1;
        // setting socket option
        // SOL_SOCKET = settings applied to the socket itself rather specific protocol
        // SO_REUSEADDR = allow port to be bound immediately, bypassing timewait (if exists)
        // 1 enabling it, 0 means disabling
        if (setsockopt(m_listen_socket.GetFileDescriptor(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            throw std::runtime_error(std::string("setsockopt SO_REUSEADDR failed: ") + std::strerror(errno));
        }

        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY; // bind to all available network interfaces, listen to all interfaces lan, wifi, local
        address.sin_port = htons(port); // htons = host to network short (Big Endian)

        if (bind(m_listen_socket.GetFileDescriptor(), (const struct sockaddr *)&address, sizeof(address)) < 0) {
            throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
        }

        if (listen(m_listen_socket.GetFileDescriptor(), SOMAXCONN) < 0) {
            throw std::runtime_error(std::string("listen failed: ") + std::strerror(errno));
        }

        // triggers when there is data to read
        if (m_epoll_reactor.Add(m_listen_socket.GetFileDescriptor(), EPOLLIN | EPOLLET) == -1) {
            throw std::runtime_error(std::string("epoll_ctl failed: ") + std::strerror(errno));
        }
    };

    void Run(unsigned int thread_count = std::thread::hardware_concurrency()) {
        if (thread_count == 0) thread_count = 1;
        std::vector<std::thread> workers;
        for (unsigned int i = 1; i < thread_count; i++) {
            workers.emplace_back([this]() {WorkerLoop();});
        }
        WorkerLoop(); // main thread runs as well
        // only reached if WorkerLoop() ever returns/throws on the main thread -
        // required so still-running threads aren't destroyed while joinable
        for (auto& t : workers) t.join();
    };

    };