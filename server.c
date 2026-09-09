#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>

#define PORT 8080
#define MAX_EVENTS 10
#define BUFFER_SIZE 1024
#define INITIAL_CAPACITY 4096

// Client connection state tracking
typedef struct{
    int fd;
    char *buf;
    size_t len;
    size_t capacity;
} client_state_t;

// clean up client state and close connection
// helper function
void clean_up_client_state(int epoll_fd, client_state_t **state_ptr) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, (*state_ptr)->fd, NULL);
    close((*state_ptr)->fd);
    free((*state_ptr)->buf);
    free(*state_ptr);
    *state_ptr = NULL;
}

int make_socket_non_blocking(int fd) {
    // get all existing flags
    int flags = fcntl(fd, F_GETFL, 0);

    // ensure non block flag is set
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    // creating listener, AF_INET = IPv4, SOCK_STREAM = TCP
    // other options AF_INET6 = IPv6, SOCK_DGRAM = UDP 
    // etc
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    // setting socket option
    // SOL_SOCKET = settings applied to the socket it self rather specific protocol
    // SO_REUSEADDR = allow port to be bound immediately, bypassing timewait (if exists)
    // 1 enabling it, 0 means disabling
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // bind to all available network interfaces, listen to all interfaces lan, wifi, local
    address.sin_port = htons(8080); // htons = host to network short (Big Endian)

    if (bind(server_fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    
    // realistically because at this point we know no flags has been set
    // its probably better to just do straight
    // fcntl(fd, F_SETFL, O_NONBLOCK)
    // but for the sake learning to write a setter function for a socket flag
    // we are using make_socket_non_blocking
    if (make_socket_non_blocking(server_fd) < 0) {
        perror("make non blocking failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll creation error");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct epoll_event ev;

    ev.data.fd = server_fd; // to know who triggers the event (for us)
    ev.events = EPOLLIN | EPOLLET; // triggers when there is data to read

    // server_fd in this param context is meant for kernel, the server_fd inside
    // epoll_event is for us developers to know which fd triggers the events
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        perror("epoll_ctl failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct epoll_event events[MAX_EVENTS];

    
    
    while (1) {
        // blocks until at least one socket has activity
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_fd) {
                // new connections
                // need while loop on accept because we set the server_fd
                // with EPOLLET, since epoll_wait will trigger once
                // if multiple clients connect at the same time
                // only one client will be processed
                // need to loop accept until no clients left
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);

                    // there is another function accept4()
                    // behaves like accept() however it takes additional parameter
                    // to specify flag therefore we can set non blocking flag
                    // at socket creation
                    // accept4(server_fd, (struct sockaddr *)&client_addr, &client_len, SOCK_NONBLOCK)
                    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // All pending connections accepted
                            break;
                        }
                        perror("accept error");
                        break;
                    }

                    // Make the new client socket non-blocking
                    if (make_socket_non_blocking(client_fd) < 0) {
                        close(client_fd);
                        continue;
                    }
                    
                    // create new client state for new client
                    client_state_t *state = malloc(sizeof(client_state_t));
                    if (!state) {
                        close(client_fd);
                        continue;
                    }
                    state->buf = malloc(INITIAL_CAPACITY);
                    if (!state->buf) {
                        free(state);
                        close(client_fd);
                        continue;
                    }

                    // Register the new client socket with epoll
                    // EPOLLET switch monitoring from level-trigger(default) to edge-triggered
                    // level-triggered = fires as long as buffer is not empty
                    // edge-triggered = fires only when data arrives (state change) 
                    // edge-triggered = lower system call overhead but must use non blocking fds 
                    // to prevent starving
                    // edge-triggered = must loop read() until it returns EAGAIN or EWOULDBLOCK
                    struct epoll_event ev;
                    ev.events = EPOLLIN | EPOLLET; // Read events + Edge-Triggered mode (optional, standard for high perf)
                    ev.data.ptr = state;           // make data.ptr point to the client state

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                        perror("epoll_ctl: client_fd failed");
                        close(client_fd);
                    } else {
                        printf("New connection accepted: client_fd %d\n", client_fd);
                    }
                }
                

            } else {
                // existing connections
                int client_fd = events[i].data.fd;
                char temp_buf[BUFFER_SIZE];
                int connection_closed = 0;
                client_state_t *state = (client_state_t *)events[i].data.ptr;
                
                while (1) {
                    ssize_t bytes_read = read(client_fd, temp_buf, sizeof(temp_buf));

                    if (bytes_read > 0) {
                        // expand client state buffer if needed
                        if (state->len + bytes_read >= state->capacity) {
                            size_t new_capacity = state->capacity * 2;
                            char *new_buf = realloc(state->buf, new_capacity);
                            if (!new_buf) {
                                perror("realloc failed");
                                clean_up_client_state(epoll_fd, &state);
                                break;
                            }
                            state->buf = new_buf;
                            state->capacity = new_capacity;
                        }
                        
                        memcpy(state->buf + state->len, temp_buf, bytes_read);
                        state->len += bytes_read;
                    } else if (bytes_read == 0) {
                        // Client closed the connection cleanly (EOF)
                        printf("Client fd %d disconnected\n", client_fd);
                        connection_closed = 1;
                        break;
                    } else { // -1
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Socket buffer is completely drained
                            // It is now safe to yield control back to epoll_wait
                            break;
                        } else if (errno == EINTR) {
                            // Interrupted by a system call
                            // continue reading
                            continue;
                        } else {
                            // socket error
                            perror("read error");
                            connection_closed = 1;
                            break;
                        }
                    }
                }

                // clean up if client disconnect/socket error
                if (connection_closed) {
                    clean_up_client_state(epoll_fd, &state);
                } else {
                    // entire request has been read
                    // send your HTTP response and close the socket
                    const char *response = 
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 13\r\n"
                        "\r\n"
                        "Hello, World!";
                    write(client_fd, response, strlen(response));
                    
                    clean_up_client_state(epoll_fd, &state);
                }
            }
        }
    }
}