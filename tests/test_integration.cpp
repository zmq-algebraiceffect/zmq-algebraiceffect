#define DOCTEST_CONFIG_NO_MULTI_LANE_ATOMICS

#include "doctest.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "zmqae/zmqae.hpp"

namespace {

void drain_poll(zmqae::client &c, zmqae::router &r, int max_ms = 100) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{max_ms};
    while (std::chrono::steady_clock::now() < deadline) {
        r.poll();
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

void wait_for_threads(int ms = 20) {
    std::this_thread::sleep_for(std::chrono::milliseconds{ms});
}

std::vector<std::byte> to_bytes(const std::string &s) {
    auto *data = reinterpret_cast<const std::byte *>(s.data());
    return std::vector<std::byte>{data, data + s.size()};
}

} // namespace

TEST_CASE("TC-5.2.3: inproc transport round-trip") {
    zmqae::router r{"inproc://test-inproc-001"};
    r.on("Ping", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("pong");
    });

    zmqae::client c{"inproc://test-inproc-001"};
    wait_for_threads();

    bool ok = false;
    c.perform("Ping", nullptr, [&](zmqae::result res) {
        if (res.is_ok() && res.value() == "pong") {
            ok = true;
        }
    });

    drain_poll(c, r);
    CHECK(ok);
}

TEST_CASE("TC-5.3.1: default port 5555 connectable") {
    zmqae::router r{"tcp://127.0.0.1:15555"};
    r.on("Test", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("ok");
    });

    zmqae::client c{"tcp://127.0.0.1:15555"};
    wait_for_threads();

    bool ok = false;
    c.perform("Test", nullptr, [&](zmqae::result res) {
        if (res.is_ok() && res.value() == "ok") {
            ok = true;
        }
    });

    drain_poll(c, r);
    CHECK(ok);
}

TEST_CASE("TC-5.3.2: custom port connectable") {
    int port = 15556;
    std::string endpoint = "tcp://127.0.0.1:" + std::to_string(port);
    zmqae::router r{endpoint};
    r.on("Test", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("ok");
    });

    zmqae::client c{endpoint};
    wait_for_threads();

    bool ok = false;
    c.perform("Test", nullptr, [&](zmqae::result res) {
        if (res.is_ok()) {
            ok = true;
        }
    });

    drain_poll(c, r);
    CHECK(ok);
}

TEST_CASE("TC-6.1.1: each node owns independent thread") {
    auto t1 = std::this_thread::get_id();

    zmqae::router r{"inproc://test-thread-indep-001"};
    std::thread::id handler_id;
    r.on("Check", [&handler_id](std::shared_ptr<zmqae::perform_context> ctx) {
        handler_id = std::this_thread::get_id();
        ctx->resume(nullptr);
    });

    zmqae::client c{"inproc://test-thread-indep-001"};
    wait_for_threads();

    c.perform("Check", {}, [](zmqae::result) {});
    drain_poll(c, r);
    CHECK(handler_id == t1);
}

TEST_CASE("TC-6.2.1: socket access only from ZMQ thread") {
    zmqae::router r{"inproc://test-socket-thread-001"};
    r.on("Safe", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("safe");
    });

    zmqae::client c{"inproc://test-socket-thread-001"};
    wait_for_threads();

    bool ok = false;
    c.perform("Safe", nullptr, [&](zmqae::result res) {
        if (res.is_ok()) {
            ok = true;
        }
    });

    drain_poll(c, r);
    CHECK(ok);
}

TEST_CASE("TC-6.2.2: inter-thread communication via queue") {
    zmqae::router r{"inproc://test-queue-001"};
    r.on("Echo", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume(ctx->payload());
    });

    zmqae::client c{"inproc://test-queue-001"};
    wait_for_threads();

    int received = 0;
    for (int i = 0; i < 3; ++i) {
        c.perform("Echo", i, [&received](zmqae::result) { received++; });
    }

    drain_poll(c, r);
    CHECK(received == 3);
}

TEST_CASE("INT-01: client to router to resume round-trip") {
    zmqae::router r{"inproc://test-int-01"};
    r.on("Add", [](std::shared_ptr<zmqae::perform_context> ctx) {
        auto a = ctx->payload()["a"].get<int>();
        auto b = ctx->payload()["b"].get<int>();
        ctx->resume(a + b);
    });

    zmqae::client c{"inproc://test-int-01"};
    wait_for_threads();

    int result = 0;
    c.perform("Add", {{"a", 3}, {"b", 4}}, [&](zmqae::result res) {
        if (res.is_ok()) {
            result = res.value().get<int>();
        }
    });

    drain_poll(c, r);
    CHECK(result == 7);
}

TEST_CASE("INT-02: client to router to error round-trip") {
    zmqae::router r{"inproc://test-int-02"};
    r.on("Fail", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->error("intentional");
    });

    zmqae::client c{"inproc://test-int-02"};
    wait_for_threads();

    bool got_error = false;
    c.perform("Fail", {}, [&](zmqae::result res) {
        got_error = res.is_error();
    });

    drain_poll(c, r);
    CHECK(got_error);
}

TEST_CASE("INT-03: binary perform and resume round-trip") {
    zmqae::router r{"inproc://test-int-03"};
    r.on("Binarize", [](std::shared_ptr<zmqae::perform_context> ctx) {
        std::vector<std::vector<std::byte>> bins;
        for (int i = 0; i < ctx->binary_count(); ++i) {
            bins.push_back(ctx->binary(i));
        }
        ctx->resume("done", bins);
    });

    zmqae::client c{"inproc://test-int-03"};
    wait_for_threads();

    auto bin_data = to_bytes("BINARY_PAYLOAD");
    std::vector<std::vector<std::byte>> send_bins = {bin_data};

    bool ok = false;
    c.perform("Binarize", {}, send_bins, [&](zmqae::result res) {
        if (res.is_ok() && res.binary_count() == 1) {
            auto &recv = res.binary(0);
            ok = (std::string{reinterpret_cast<const char *>(recv.data()),
                              recv.size()} == "BINARY_PAYLOAD");
        }
    });

    drain_poll(c, r);
    CHECK(ok);
}

TEST_CASE("INT-04: multiple clients simultaneous perform") {
    zmqae::router r{"inproc://test-int-04"};
    r.on("Echo", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume(ctx->payload());
    });

    zmqae::client c1{"inproc://test-int-04"};
    zmqae::client c2{"inproc://test-int-04"};
    zmqae::client c3{"inproc://test-int-04"};
    wait_for_threads();

    int count = 0;
    auto cb = [&count](zmqae::result res) {
        if (res.is_ok()) {
            count++;
        }
    };
    c1.perform("Echo", 1, cb);
    c2.perform("Echo", 2, cb);
    c3.perform("Echo", 3, cb);

    for (int i = 0; i < 50; ++i) {
        r.poll();
        c1.poll();
        c2.poll();
        c3.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        if (count == 3) {
            break;
        }
    }
    CHECK(count == 3);
}

TEST_CASE("INT-05: client timeout returns error callback") {
    std::shared_ptr<zmqae::perform_context> held_ctx;
    zmqae::router r{"inproc://test-int-05-timeout"};
    r.on("Timeout", [&held_ctx](std::shared_ptr<zmqae::perform_context> ctx) {
        held_ctx = ctx;
    });

    zmqae::client c{"inproc://test-int-05-timeout"};
    wait_for_threads();

    bool timed_out = false;
    c.perform("Timeout", {}, [&](zmqae::result res) {
        if (res.is_error() && res.error().find("timeout") != std::string::npos) {
            timed_out = true;
        }
    }, 30);

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

TEST_CASE("INT-06: close during pending performs cleans up") {
    zmqae::router r{"inproc://test-int-06"};
    r.on("Slow", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("done");
    });

    zmqae::client c{"inproc://test-int-06"};
    wait_for_threads();

    c.perform("Slow", {}, [](zmqae::result) {});
    c.close();
    CHECK_FALSE(c.is_open());
}

TEST_CASE("INT-07: inproc client and router in same process") {
    zmqae::router r{"inproc://test-int-07"};
    r.on("Hi", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("hello");
    });

    zmqae::client c{"inproc://test-int-07"};
    wait_for_threads();

    bool ok = false;
    c.perform("Hi", nullptr, [&](zmqae::result res) {
        if (res.is_ok() && res.value() == "hello") {
            ok = true;
        }
    });

    drain_poll(c, r);
    CHECK(ok);
}

TEST_CASE("INT-08: poll is non-blocking") {
    zmqae::router r{"inproc://test-int-08"};
    r.on("X", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume(nullptr);
    });

    zmqae::client c{"inproc://test-int-08"};
    wait_for_threads();

    auto start = std::chrono::steady_clock::now();
    r.poll();
    c.poll();
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::milliseconds{50});
}

TEST_CASE("INT-09: callback not invoked before poll") {
    zmqae::router r{"inproc://test-int-09"};
    bool handler_called = false;
    r.on("Y", [&handler_called](std::shared_ptr<zmqae::perform_context> ctx) {
        handler_called = true;
        ctx->resume(nullptr);
    });

    zmqae::client c{"inproc://test-int-09"};
    wait_for_threads();

    bool callback_called = false;
    c.perform("Y", nullptr, [&](zmqae::result) { callback_called = true; });

    wait_for_threads(50);
    CHECK(callback_called == false);

    drain_poll(c, r);
    CHECK(callback_called);
}

TEST_CASE("INT-10: async handler resume from separate thread") {
    zmqae::router r{"inproc://test-int-10"};
    r.on("AsyncWork", [](std::shared_ptr<zmqae::perform_context> ctx) {
        std::thread([ctx]() {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
            ctx->resume("async_result");
        }).detach();
    });

    zmqae::client c{"inproc://test-int-10"};
    wait_for_threads();

    bool ok = false;
    c.perform("AsyncWork", nullptr, [&](zmqae::result res) {
        if (res.is_ok() && res.value() == "async_result") {
            ok = true;
        }
    });

    for (int i = 0; i < 50; ++i) {
        r.poll();
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{3});
        if (ok) {
            break;
        }
    }
    CHECK(ok);
}

TEST_CASE("INT-11: handler exception results in client error") {
    zmqae::router r{"inproc://test-int-11"};
    r.on("Crash", [](std::shared_ptr<zmqae::perform_context> ctx) {
        throw std::runtime_error("handler exploded");
    });

    zmqae::client c{"inproc://test-int-11"};
    wait_for_threads();

    std::string error_msg;
    c.perform("Crash", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            error_msg = res.error();
        }
    });

    drain_poll(c, r);
    CHECK(error_msg.find("handler error") != std::string::npos);
}

TEST_CASE("INT-12: large binary frame count (8 frames) handled") {
    zmqae::router r{"inproc://test-int-12"};
    r.on("ManyBins", [](std::shared_ptr<zmqae::perform_context> ctx) {
        std::vector<std::vector<std::byte>> bins;
        for (int i = 0; i < ctx->binary_count(); ++i) {
            bins.push_back(ctx->binary(i));
        }
        ctx->resume("ok", bins);
    });

    zmqae::client c{"inproc://test-int-12"};
    wait_for_threads();

    std::vector<std::vector<std::byte>> send_bins;
    for (int i = 0; i < 8; ++i) {
        send_bins.push_back(to_bytes("frame" + std::to_string(i)));
    }

    bool ok = false;
    c.perform("ManyBins", {}, send_bins, [&](zmqae::result res) {
        if (res.is_ok() && res.binary_count() == 8) {
            ok = true;
        }
    });

    drain_poll(c, r);
    CHECK(ok);
}

TEST_CASE("INT-13: binary_frames exceeding max (9) returns error") {
    zmqae::router r{"inproc://test-int-13"};

    zmqae::client c{"inproc://test-int-13"};
    wait_for_threads();

    std::vector<std::vector<std::byte>> send_bins;
    for (int i = 0; i < 9; ++i) {
        send_bins.push_back(to_bytes("x"));
    }

    c.perform("OverMax", {}, send_bins, [](zmqae::result res) {
        CHECK(res.is_error());
    });

    drain_poll(c, r);
}
