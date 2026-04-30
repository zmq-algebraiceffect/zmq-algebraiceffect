// performer.cpp — CUI action sender for zmq-algebraiceffect
// Usage: ./performer [endpoint]
//   endpoint: tcp://localhost:5555 (default)

#define SPDLOG_HEADER_ONLY
#include <zmqae/zmqae.hpp>

#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char *argv[]) {
    const std::string endpoint = (argc > 1) ? argv[1] : "tcp://localhost:5555";

    std::printf("=== zmq-algebraiceffect performer ===\n");
    std::printf("Endpoint: %s\n", endpoint.c_str());
    std::printf("Commands:\n");
    std::printf("  <effect> <json_payload>  — perform an effect\n");
    std::printf("  quit                     — exit\n");
    std::printf("\nExample: Echo {\"hello\":\"world\"}\n");
    std::printf("         Add {\"a\":3,\"b\":7}\n\n");

    zmqae::client client{endpoint};

    std::string line;
    while (true) {
        std::printf("> ");
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) {
            break;
        }

        if (line == "quit" || line == "exit") {
            break;
        }

        auto space_pos = line.find(' ');
        if (space_pos == std::string::npos) {
            std::printf("Usage: <effect> <json_payload>\n");
            continue;
        }

        std::string effect = line.substr(0, space_pos);
        std::string json_str = line.substr(space_pos + 1);

        zmqae::json payload;
        try {
            payload = zmqae::json::parse(json_str);
        } catch (const zmqae::json::parse_error &e) {
            std::printf("Invalid JSON: %s\n", e.what());
            continue;
        }

        std::printf("Performing '%s'...\n", effect.c_str());

        client.perform(effect, payload, [](zmqae::result res) {
            if (res.is_ok()) {
                std::printf("Result: %s\n", res.value().dump().c_str());
            } else {
                std::printf("Error: %s\n", res.error().c_str());
            }
        }, 5000);


        for (int i = 0; i < 100; ++i) {
            client.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }

    client.close();
    std::printf("Bye.\n");
    return 0;
}
