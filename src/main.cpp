#include "server.hpp"
#include <cstdio>
#include <cstdlib>

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 8080;

    try {
        Server server(port);
        std::printf("Listening on port %d...\n", port);
        server.Run();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "Server failed to start: %s\n", e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
