// handler.cpp — CUI action handler/responder for zmq-algebraiceffect
// Usage: ./handler [endpoint]
//   endpoint: tcp://*:5555 (default)

#define SPDLOG_HEADER_ONLY
#include <zmqae/zmqae.hpp>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <thread>

static std::atomic<bool> g_running{true};

static void signal_handler(int /*signum*/) {
    g_running.store(false);
}

int main(int argc, char *argv[]) {
    const std::string endpoint = (argc > 1) ? argv[1] : "tcp://*:5555";

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::printf("=== zmq-algebraiceffect handler ===\n");
    std::printf("Binding on: %s\n", endpoint.c_str());
    std::printf("Registered effects: Echo, Add\n");
    std::printf("Press Ctrl+C to exit\n\n");

    zmqae::router router{endpoint};

    // Echo handler — returns payload as-is
    router.on("Echo", [](std::shared_ptr<zmqae::perform_context> ctx) {
        std::printf("[Echo] id=%s payload=%s\n",
                    ctx->id().c_str(),
                    ctx->payload().dump().c_str());
        ctx->resume(ctx->payload());
    });

    // Add handler — expects {"a":N, "b":N}, returns sum
    router.on("Add", [](std::shared_ptr<zmqae::perform_context> ctx) {
        std::printf("[Add]  id=%s payload=%s\n",
                    ctx->id().c_str(),
                    ctx->payload().dump().c_str());

        try {
            double a = ctx->payload().at("a").get<double>();
            double b = ctx->payload().at("b").get<double>();
            ctx->resume(zmqae::json{{"sum", a + b}});
        } catch (const zmqae::json::exception &e) {
            ctx->error(std::string{"invalid payload: "} + e.what());
        }
    });

    // Default handler for unregistered effects
    router.on_unregistered([](std::shared_ptr<zmqae::perform_context> ctx) {
        std::printf("[???]  id=%s effect=%s (unregistered)\n",
                    ctx->id().c_str(),
                    ctx->effect().c_str());
        // Auto error is sent by default if on_unregistered is not set,
        // but since we set it, we must call error() ourselves:
        ctx->error("no handler for: " + ctx->effect());
    });

    std::printf("Handler ready. Waiting for performs...\n");

    while (g_running.load()) {
        router.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    router.close();
    std::printf("\nShutting down.\n");
    return 0;
}
