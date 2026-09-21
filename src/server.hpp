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

class Server {
private:
MyFileDescriptor m_listen_socket;
EpollReactor m_epoll_reactor;
std::unordered_map<int, std::unique_ptr<Connection>> m_connections;
Router m_router;
public:
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

    void Run() {
        while (1) {

            for (epoll_event event : m_epoll_reactor.Wait()) {
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

                        if (m_epoll_reactor.Add(conn->GetFileDescriptor(), EPOLLIN | EPOLLET | EPOLLONESHOT) == -1) {
                            perror("epoll_ctl: client_fd failed");
                        } else {
                            printf("New connection accepted: client_fd %d\n", conn->GetFileDescriptor());
                            // try_emplace is safer than emplace, will not destroy the conn if key exists
                            m_connections.try_emplace(conn->GetFileDescriptor(), std::move(conn));
                        }

                    }
                } else {
                    int client_fd = event.data.fd;
                    std::unique_ptr<Connection>& conn = m_connections.at(client_fd);
                    ConnectionState conn_state = conn->Read();
                    if (conn_state == ConnectionState::RequestComplete) {
                        // route
                        Response res = m_router.Dispatch(*conn->GetRequest());
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
                    
                        m_connections.erase(client_fd);
                    } else if (conn_state == ConnectionState::ConnectionClosed) {
                        m_connections.erase(client_fd);
                    } else {
                        // not finish reading
                        // rearm it
                        m_epoll_reactor.Modify(client_fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
                    }
                }

            }
        }
        
    };
};