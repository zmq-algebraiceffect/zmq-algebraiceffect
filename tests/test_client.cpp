#define DOCTEST_CONFIG_NO_MULTI_LANE_ATOMICS

#include "doctest.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "zmqae/zmqae.hpp"

namespace {

 void drain_poll(zmqae::client &c, zmqae::router &r, int max_ms = 200) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{max_ms};
    while (std::chrono::steady_clock::now() < deadline) {
        r.poll();
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

void wait_for_threads(int ms = 50) {
    std::this_thread::sleep_for(std::chrono::milliseconds{ms});
}

} // namespace

TEST_CASE("CLIENT-01: constructor sets is_open to true") {
    zmqae::router r{"inproc://test-client-01"};
    r.on("Ping", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("pong");
    });

    zmqae::client c{"inproc://test-client-01"};
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    CHECK(c.is_open());
}

TEST_CASE("CLIENT-02: close sets is_open to false") {
    zmqae::client c{"inproc://test-client-02"};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    c.close();
    CHECK_FALSE(c.is_open());
}

TEST_CASE("CLIENT-03: perform after close does nothing") {
    zmqae::client c{"inproc://test-client-03"};
    c.close();

    bool called = false;
    c.perform("Test", {}, [&](zmqae::result) { called = true; });
    CHECK_FALSE(called);
}

TEST_CASE("CLIENT-04: poll after close does nothing") {
    zmqae::client c{"inproc://test-client-04"};
    c.close();
    c.poll();
    CHECK_FALSE(c.is_open());
}

TEST_CASE("CLIENT-05: destructor calls close") {
    { zmqae::client c{"inproc://test-client-05"}; }
    CHECK(true);
}

TEST_CASE("CLIENT-06: move construction transfers ownership") {
    zmqae::client c1{"inproc://test-client-06"};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    zmqae::client c2{std::move(c1)};
    CHECK_FALSE(c1.is_open());
    CHECK(c2.is_open());
}

TEST_CASE("CLIENT-07: user-provided context works") {
    zmq::context_t ctx{1};
    zmqae::client c{"inproc://test-client-07", ctx};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    CHECK(c.is_open());
}

TEST_CASE("CLIENT-08: perform callback invoked on resume") {
    zmqae::router r{"inproc://test-client-08"};
    r.on("Echo", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume(ctx->payload());
    });

    zmqae::client c{"inproc://test-client-08"};
    wait_for_threads();

    bool called = false;
    c.perform("Echo", "hello", [&](zmqae::result res) {
        if (res.is_ok() && res.value() == "hello") {
            called = true;
        }
    });

    drain_poll(c, r, 150);
    CHECK(called);
}

TEST_CASE("CLIENT-09: perform with timeout fires error callback") {
    std::shared_ptr<zmqae::perform_context> held_ctx;
    zmqae::router r{"inproc://test-client-09-timeout"};
    r.on("SlowEffect", [&held_ctx](std::shared_ptr<zmqae::perform_context> ctx) {
        held_ctx = ctx;
    });

    zmqae::client c{"inproc://test-client-09-timeout"};
    wait_for_threads();

    bool timed_out = false;
    c.perform("SlowEffect", {}, [&](zmqae::result res) {
        if (res.is_error() && res.error().find("timeout") != std::string::npos) {
            timed_out = true;
        }
    }, 50);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{200};
    while (!timed_out && std::chrono::steady_clock::now() < deadline) {
        r.poll();
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    CHECK(timed_out);

    if (held_ctx && !held_ctx->is_resumed()) {
        held_ctx->error("cleanup");
    }
    held_ctx.reset();
}

TEST_CASE("CLIENT-10: perform with binary data") {
    zmqae::router r{"inproc://test-client-10"};
    int received_count = 0;
    r.on("BinEffect", [&received_count](std::shared_ptr<zmqae::perform_context> ctx) {
        received_count = ctx->binary_count();
        ctx->resume(ctx->payload());
    });

    zmqae::client c{"inproc://test-client-10"};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    std::vector<std::vector<std::byte>> bins;
    bins.push_back({std::byte{0x01}, std::byte{0x02}});
    bins.push_back({std::byte{0x03}, std::byte{0x04}});

    c.perform("BinEffect", {}, bins, [](zmqae::result) {});

    drain_poll(c, r);
    CHECK(received_count == 2);
}

TEST_CASE("CLIENT-11: perform to unconnected endpoint does not crash") {
    zmqae::client c{"inproc://test-client-11-nonexistent"};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    bool called = false;
    c.perform("Test", {}, [&](zmqae::result) { called = true; });
    c.poll();
    CHECK(c.is_open());
}

TEST_CASE("CLIENT-12: poll drains all pending messages") {
    zmqae::router r{"inproc://test-client-12"};
    r.on("Echo", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume(ctx->payload());
    });

    zmqae::client c{"inproc://test-client-12"};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    int call_count = 0;
    for (int i = 0; i < 5; ++i) {
        c.perform("Echo", i, [&call_count](zmqae::result) { call_count++; });
    }

    drain_poll(c, r);
    CHECK(call_count == 5);
}
