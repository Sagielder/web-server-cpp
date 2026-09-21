#pragma once
#include "connection.hpp"
#include <functional>
#include <string>

struct Response {
    int status_code;
    std::string response_phrase;
    std::string content_type;
    std::string body;

    Response(int code = 200, std::string phrase = "OK", std::string type = "text/plain", std::string b = "")
        : status_code(code), 
          response_phrase(std::move(phrase)),
          content_type(std::move(type)), 
          body(std::move(b)) // std::move avoids unnecessary string copies
    {};
};

using Handler = std::function<Response(const Request&)>;

struct StringHash {
    using is_transparent = void; // Enables heterogeneous lookup capability

    size_t operator()(std::string_view sv) const {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::string& s) const {
        return std::hash<std::string_view>{}(s);
    }
};

std::string HttpMethodToString(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET:     return "GET";
        case HttpMethod::POST:    return "POST";
        case HttpMethod::PUT:     return "PUT";
        case HttpMethod::PATCH:   return "PATCH";
        case HttpMethod::DELETE:  return "DELETE";
        case HttpMethod::HEAD:    return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        case HttpMethod::TRACE:   return "TRACE";
        case HttpMethod::CONNECT: return "CONNECT";
        default:                 return "UNKNOWN";
    }
}

class Router {

private:
std::unordered_map<HttpMethod, std::unordered_map<std::string, Handler, StringHash, std::equal_to<void>>> m_route_table;
public:
    Router() {};

    Response Dispatch(const Request& req) {
        std::unordered_map<HttpMethod, std::unordered_map<std::string, Handler, StringHash, std::equal_to<void>>>::iterator 
            map_itor = m_route_table.find(req.request_line.method);
        if (map_itor == m_route_table.end()) {
            // not found
            return Response(404, "Not Found", "text/plain", "");
        }
        std::unordered_map<std::string, Handler, StringHash, std::equal_to<void>>::iterator 
            inner_itor = map_itor->second.find(req.request_line.request_url);
        if (inner_itor == map_itor->second.end()) {
            // not found 
            return Response(404, "Not Found", "text/plain", "");
        }

        try { return inner_itor->second(req); } 
        catch (const std::exception& e) { return Response{500, "Internal Server Error", "text/plain", ""}; } // in event of a crash 
    }

    void Add(HttpMethod method, std::string path, Handler handler) {
        // if exists, replace
        m_route_table[method][path] = handler;
    }
};
