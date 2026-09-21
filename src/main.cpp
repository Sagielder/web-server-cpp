#include "server.hpp"
#include <cstdio>
#include <cstdlib>

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 8080;

    Router router;

    router.Add(HttpMethod::GET, "/", [](const Request&) {
        return Response(200, "OK", "text/plain", "");
    });

    router.Add(HttpMethod::POST, "/", [](const Request& req) {
        return Response(200, "OK", "text/plain", std::string(req.body));
    });

    router.Add(HttpMethod::POST, "/Test", [](const Request& req) {
        return Response(200, "OK", "text/plain", std::string(req.body));
    });

    router.Add(HttpMethod::GET, "/Health", [](const Request&) {
        return Response(200, "OK", "text/plain", "Check");
    });
    try {
        Server server(port, std::move(router));
        std::printf("Listening on port %d...\n", port);
        server.Run();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "Server failed to start: %s\n", e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
