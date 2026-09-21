#pragma once
#include "file_descriptor.hpp"
#include <vector>
#include <cstdio>
#include <cerrno>
#include <cstdint>
#include <string_view>
#include <charconv>
#include <string>
#include <algorithm>

enum class HttpMethod : std::uint8_t {
    GET = 0,
    POST = 1,
    PUT = 2,
    PATCH = 3,
    DELETE = 4,
    HEAD = 5,
    OPTIONS = 6,
    TRACE = 7,
    CONNECT = 8
};

enum class ConnectionState : std::uint8_t {
    NeedMoreData = 0,
    RequestComplete = 1, // connection close peacfully (EOF)
    ConnectionClosed = 2, // connection disconnected, nothing can be sent back
    BadRequest = 3, // request was malformed; connection is alive, send a 400 before closing
    URITooLong = 4, // request line exceeded MAX_REQUEST_LINE_LENGTH; send a 414 before closing
    PayloadTooLarge = 5 // Content-Length exceeded MAX_CONTENT_LENGTH; send a 413 before closing
};

struct HeaderField {
    std::string_view name;
    std::string_view value;
};

struct RequestLineField {
    HttpMethod method;
    std::string_view request_url;
    std::string_view http_version;
};


struct Request {
    RequestLineField request_line;
    std::vector<HeaderField> headers;
    std::string_view body;
};

enum class RequestParseState : std::uint8_t {
    NoErrors = 0,
    NeedMoreData = 1,
    Malformed = 2, 
    URLTooLong = 3
};

enum class VersionStatus {
    Valid,
    BadRequest_400,
    VersionNotSupported_505
};

VersionStatus ValidateHttpVersion(std::string_view version) {
    // example HTTP/1.1, must be 8 char long 
    if (version.length() != 8) {
        return VersionStatus::BadRequest_400;
    }

    if (version.substr(0, 5) != "HTTP/") {
        return VersionStatus::BadRequest_400;
    }

    if (version[6] != '.') {
        return VersionStatus::BadRequest_400;
    }

    if (!std::isdigit(static_cast<unsigned char>(version[5])) || 
        !std::isdigit(static_cast<unsigned char>(version[7]))) {
        return VersionStatus::BadRequest_400;
    }

    // supported version
    if (version == "HTTP/1.1" || version == "HTTP/1.0") {
        return VersionStatus::Valid;
    }

    return VersionStatus::VersionNotSupported_505;
}

class Connection {

private:
MyFileDescriptor m_client_socket;
static constexpr int TEMP_BUFFER_SIZE = 4096;
static constexpr int MAX_REQUEST_LINE_LENGTH = 8192;
static constexpr int MAX_CONTENT_LENGTH = 1048576;
std::vector<char> m_buffer;
ssize_t m_header_end_index;
ssize_t m_header_begin_index;
std::uint8_t m_state; // 0 - no match, 1 - match first, 2 - match second, ... so on   
Request m_request;
ssize_t m_content_length;

private:
    bool FindHeaderEnding(const ssize_t& read_size) {
        // \r\n\r\n
        for (size_t i = m_buffer.size() - read_size; i < m_buffer.size(); i++){
            switch (m_state)
            {
            case 0: // no match
                if (m_buffer[i] == '\r') m_state = 1;
                break;
            case 1: // \r
                if (m_buffer[i] == '\n') m_state = 2;
                else if (m_buffer[i] != '\r') m_state = 0;
                break;
            case 2: // \r\n
                if (m_buffer[i] == '\r') m_state = 3;
                else m_state = 0;
                break;
            case 3:// \r\n\r
                if (m_buffer[i] == '\n') m_state = 4;
                else m_state = 0;
                break;
            default:
                break;
            }
            if (m_state == 4) {
                m_header_end_index = i;
                break;
            }
        }
        return m_header_end_index != -1;
    };
    constexpr bool IsValidHttpMethod(std::string_view method) {
        // A quick check on length can instantly discard most invalid inputs
        if (method.empty() || method.length() > 7) return false; 

        switch (method[0]) {
            case 'G':
                if (method == "GET") {
                    m_request.request_line.method = HttpMethod::GET;
                    return true;
                }
                break;
            case 'P': 
                if (method == "POST") {
                    m_request.request_line.method = HttpMethod::POST;
                    return true;
                } 
                else if (method == "PUT") {
                    m_request.request_line.method = HttpMethod::PUT;
                    return true;
                } 
                else if (method == "PATCH") {
                    m_request.request_line.method = HttpMethod::PATCH;
                    return true;
                }
                break;
            case 'D':
                if (method == "DELETE") {
                    m_request.request_line.method = HttpMethod::DELETE;
                    return true;
                }
                break;
            case 'H': 
                if (method == "HEAD") {
                    m_request.request_line.method = HttpMethod::HEAD;
                    return true;
                }
                break;

            case 'O':
                if (method == "OPTIONS") {
                    m_request.request_line.method = HttpMethod::OPTIONS;
                    return true;
                }
                break;
            case 'T': 
                if (method == "TRACE") {
                    m_request.request_line.method = HttpMethod::TRACE;
                    return true;
                }
                break;
            case 'C': 
                if (method == "CONNECT") {
                    m_request.request_line.method = HttpMethod::CONNECT;
                    return true;
                }
                break;  
            default:  return false;
        }
        return false;
    }

    RequestParseState ExtractRequestLine() {
        ssize_t request_line_end_index = 0;
        std::string_view temp_buffer(m_buffer.data(), m_buffer.size());
        // Skip any leading blank lines (RFC 9112 allows robust servers to skip these)
        size_t temp_index = temp_buffer.find_first_not_of("\r\n");
        if (temp_index == std::string::npos) {
            m_buffer.clear(); // clear meaningless \r\n
            return RequestParseState::NeedMoreData;
        }
        temp_buffer = temp_buffer.substr(temp_index);
        temp_index = temp_buffer.find_first_of("\n\r"); // find end of line for request line
        if (temp_index == std::string::npos) {
            if (temp_buffer.size() > MAX_REQUEST_LINE_LENGTH) return RequestParseState::URLTooLong; // 414 error, request line too long
            // more data needed, need to read more
            return RequestParseState::NeedMoreData;
        }
        request_line_end_index = temp_index;
        temp_buffer = temp_buffer.substr(0, temp_index); // extract just the request line
        temp_index = temp_buffer.find_first_of(' '); // method
        if (temp_index == std::string::npos) return RequestParseState::Malformed; // malformed
        std::string_view temp_method = temp_buffer.substr(0, temp_index);
        if (!IsValidHttpMethod(temp_method)) return RequestParseState::Malformed; // malformed
        temp_buffer = temp_buffer.substr(temp_index + 1);
        temp_index = temp_buffer.find_first_of(' '); // URL
        if (temp_index == std::string::npos) return RequestParseState::Malformed; // malformed
        m_request.request_line.request_url = temp_buffer.substr(0, temp_index);
        temp_buffer = temp_buffer.substr(temp_index + 1);
        // temp_buffer was already truncated to just the request line above,
        // so whatever's left here is the HTTP version
        m_request.request_line.http_version = temp_buffer;
        m_header_begin_index = request_line_end_index + 2; // skip both the '\r' and the '\n'
        if (ValidateHttpVersion(m_request.request_line.http_version) != VersionStatus::Valid) return RequestParseState::Malformed;
        return RequestParseState::NoErrors;
    }
    RequestParseState ExtractHeaders() {
        // shouldnt occur as this will be called after ExtractRequestLine()
        if (m_header_begin_index == -1 || m_header_end_index == -1) return RequestParseState::Malformed; // vague, maybe need a specific code for this
        std::string_view header_content(m_buffer.data() + m_header_begin_index, m_header_end_index - m_header_begin_index + 1);
        size_t temp_index;
        std::string_view temp_line;
        // parse line by line
        // only last 2 \r\n left from \r\n\r\n
        while (header_content.size() != 2) {
            temp_index = header_content.find_first_of("\r\n");
            if (temp_index == std::string::npos) return RequestParseState::Malformed; // no headers, shouldnt happen
            temp_line = header_content.substr(0, temp_index);
            header_content = header_content.substr(temp_index + 2);
            temp_index = temp_line.find_first_of(':');
            if (temp_index == std::string::npos) return RequestParseState::Malformed; // malformed headers
            HeaderField temp_header_field;
            temp_header_field.name = temp_line.substr(0, temp_index);
            if (temp_header_field.name.find_first_of(' ') != std::string::npos) return RequestParseState::Malformed; // malformed headers
            temp_header_field.value = temp_line.substr(temp_index + 1);
            // strip any leading/trailing spaces or tab
            while (!temp_header_field.value.empty() && (temp_header_field.value.front() == ' ' || temp_header_field.value.front() == '\t')) {
                temp_header_field.value.remove_prefix(1);
            }
            while (!temp_header_field.value.empty() && (temp_header_field.value.back() == ' ' || temp_header_field.value.back() == '\t')) {
                temp_header_field.value.remove_suffix(1);
            }

            m_request.headers.push_back(temp_header_field);
            
        }
        if (m_request.headers.empty()) return RequestParseState::Malformed; // empty headers. not allowed in HTTP1.1 onwards. idk if should enforce 
        return RequestParseState::NoErrors;
    }
    
    bool CaseInsensitiveEquals(std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
            std::equal(a.begin(), a.end(), b.begin(), [](unsigned char c1, unsigned char c2) { 
                return std::tolower(c1) == std::tolower(c2); 
            });
    }

    HeaderField* GetHeaderByName(std::string_view name) {
        if (m_request.headers.empty()) return nullptr;


        for (size_t i = 0; i < m_request.headers.size(); i++) {
            
            if (CaseInsensitiveEquals(m_request.headers[i].name, name)) {
                return &m_request.headers[i]; 
            }
        }

        return nullptr;
    };

public:

    int GetFileDescriptor() {
        return m_client_socket.GetFileDescriptor();
    }

    Connection(const int& client_fd) : m_client_socket(client_fd) {
        m_header_end_index = -1;
        m_state = 0;
        m_content_length = -1;
        m_header_begin_index = -1;
    };


    Request* GetRequest() {
        return &m_request;
    }
    ConnectionState Read() {
        char temp_buf[TEMP_BUFFER_SIZE];
        ssize_t bytes_read = read(m_client_socket.GetFileDescriptor(), temp_buf, sizeof(temp_buf));
        if (bytes_read > 0) {
            m_buffer.insert(m_buffer.end(), temp_buf, temp_buf + bytes_read);
            RequestParseState parse_result = ExtractRequestLine();
            if (parse_result == RequestParseState::NeedMoreData) return ConnectionState::NeedMoreData;
            else if (parse_result == RequestParseState::URLTooLong) return ConnectionState::URITooLong;
            else if (parse_result != RequestParseState::NoErrors) {
                // request line or headers were malformed - connection is still alive, send an error response
                return ConnectionState::BadRequest;
            }
            if (!m_request.headers.empty() || FindHeaderEnding(bytes_read)) {
                if (m_request.headers.empty() && ExtractHeaders() != RequestParseState::NoErrors) return ConnectionState::BadRequest;
                if (m_content_length < 0) {
                    HeaderField* content_length_header = GetHeaderByName("Content-Length");
                    if (content_length_header == nullptr) return ConnectionState::RequestComplete; // finish reading everything
                    std::from_chars(content_length_header->value.data(),
                        content_length_header->value.data() + content_length_header->value.size(), m_content_length);
                    if (m_content_length < 0) return ConnectionState::BadRequest; // malformed
                    if (m_content_length > MAX_CONTENT_LENGTH) return ConnectionState::PayloadTooLarge; // exceed max content length
                }
                
               
                
                if (m_buffer.size() - m_header_end_index - 1 >= static_cast<size_t>(m_content_length)) {
                    m_request.body = std::string_view(m_buffer.data() + m_header_end_index + 1, m_content_length);
                    return ConnectionState::RequestComplete;
                }
                else return ConnectionState::NeedMoreData;
            } else {
                // need more data
                return ConnectionState::NeedMoreData;
            }

        } else if (bytes_read == 0) {
            // Client closed the connection cleanly (EOF)
            printf("Client fd %d disconnected\n", m_client_socket.GetFileDescriptor());
            m_client_socket.Destroy();
            return ConnectionState::ConnectionClosed;
        } else { // -1
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer is completely drained
                // It is now safe to yield control back to epoll_wait
                return ConnectionState::NeedMoreData;
            } else if (errno == EINTR) {
                // Interrupted by a system call
                // read again
                return Read();
            } else {
                // socket error
                perror("read error");
                printf("Client fd %d disconnected\n", m_client_socket.GetFileDescriptor());
                return ConnectionState::ConnectionClosed;
            }
        }

    };
};