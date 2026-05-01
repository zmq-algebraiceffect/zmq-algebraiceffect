#define DOCTEST_CONFIG_NO_MULTI_LANE_ATOMICS

#include "doctest.h"

#include <chrono>
#include <string>
#include <thread>

#include "zmqae/zmqae.hpp"

namespace {

void wait_for_threads(int ms = 100) {
    std::this_thread::sleep_for(std::chrono::milliseconds{ms});
}

} // namespace

TEST_CASE("NESTED-01: get_client returns nullptr when no nested endpoint set") {
    zmqae::router r{"inproc://test-nested-01"};
    r.on("Check", [](std::shared_ptr<zmqae::perform_context> ctx) {
        CHECK(ctx->get_client() == nullptr);
        ctx->resume(nullptr);
    });

    zmqae::client c{"inproc://test-nested-01"};
    wait_for_threads();

    bool done = false;
    c.perform("Check", {}, [&](zmqae::result) { done = true; });

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{500};
    while (!done && std::chrono::steady_clock::now() < deadline) {
        r.poll();
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(done);
}

TEST_CASE("NESTED-02: handler performs nested effect via get_client") {
    zmqae::router inner{"inproc://test-nested-02-inner"};
    inner.on("Transcribe", [](std::shared_ptr<zmqae::perform_context> ctx) {
        auto text = ctx->payload()["audio"].get<std::string>();
        ctx->resume("transcribed: " + text);
    });

    zmqae::router outer{"inproc://test-nested-02-outer"};
    outer.set_nested_endpoint("inproc://test-nested-02-inner");
    outer.on("Translate", [](std::shared_ptr<zmqae::perform_context> ctx) {
        auto *nested = ctx->get_client();
        CHECK(nested != nullptr);

    // Perform nested effect — do NOT resume here
        // nested perform callback will resume ctx — do NOT resume here
        nested->perform("Transcribe", ctx->payload(),
                        [ctx](zmqae::result res) {
            if (res.is_ok()) {
                ctx->resume(res.value());
            } else {
                ctx->error(res.error());
            }
        });
    });

    zmqae::client c{"inproc://test-nested-02-outer"};
    wait_for_threads();

    bool done = false;
    std::string result_str;
    zmqae::json payload;
    payload["audio"] = "hello world";
    c.perform("Translate", payload, [&](zmqae::result res) {
        if (res.is_ok()) {
            result_str = res.value().get<std::string>();
            done = true;
        }
    });

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{500};
    while (!done && std::chrono::steady_clock::now() < deadline) {
        outer.poll();
        inner.poll();
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(done);
    CHECK(result_str == "transcribed: hello world");
}

TEST_CASE("NESTED-03: nested effect error propagates to original client") {
    zmqae::router inner{"inproc://test-nested-03-inner"};
    inner.on("Fail", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->error("inner failure");
    });

    zmqae::router outer{"inproc://test-nested-03-outer"};
    outer.set_nested_endpoint("inproc://test-nested-03-inner");
    outer.on("Delegate", [](std::shared_ptr<zmqae::perform_context> ctx) {
        auto *nested = ctx->get_client();
        nested->perform("Fail", {},
                        [ctx](zmqae::result res) {
            if (res.is_error()) {
                ctx->error("delegated: " + res.error());
            } else {
                ctx->resume(res.value());
            }
        });
    });

    zmqae::client c{"inproc://test-nested-03-outer"};
    wait_for_threads();

    bool done = false;
    std::string error_str;
    c.perform("Delegate", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            error_str = res.error();
            done = true;
        }
    });

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{500};
    while (!done && std::chrono::steady_clock::now() < deadline) {
        outer.poll();
        inner.poll();
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(done);
    CHECK(error_str == "delegated: inner failure");
}

TEST_CASE("NESTED-04: two-level nesting (A -> B -> C)") {
    zmqae::router r3{"inproc://test-nested-04-l3"};
    r3.on("Fetch", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("fetched_data");
    });

    zmqae::router r2{"inproc://test-nested-04-l2"};
    r2.set_nested_endpoint("inproc://test-nested-04-l3");
    r2.on("Process", [](std::shared_ptr<zmqae::perform_context> ctx) {
        auto *nested = ctx->get_client();
        nested->perform("Fetch", {},
                        [ctx](zmqae::result res) {
            if (res.is_ok()) {
                ctx->resume("processed_" + res.value().get<std::string>());
            } else {
                ctx->error(res.error());
            }
        });
    });

    zmqae::router r1{"inproc://test-nested-04-l1"};
    r1.set_nested_endpoint("inproc://test-nested-04-l2");
    r1.on("Compose", [](std::shared_ptr<zmqae::perform_context> ctx) {
        auto *nested = ctx->get_client();
        nested->perform("Process", {},
                        [ctx](zmqae::result res) {
            if (res.is_ok()) {
                ctx->resume("composed_" + res.value().get<std::string>());
            } else {
                ctx->error(res.error());
            }
        });
    });

    zmqae::client c{"inproc://test-nested-04-l1"};
    wait_for_threads();

    bool done = false;
    std::string result_str;
    c.perform("Compose", {}, [&](zmqae::result res) {
        if (res.is_ok()) {
            result_str = res.value().get<std::string>();
            done = true;
        }
    });

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{1000};
    while (!done && std::chrono::steady_clock::now() < deadline) {
        r1.poll();
        r2.poll();
        r3.poll();
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(done);
    CHECK(result_str == "composed_processed_fetched_data");
}

TEST_CASE("NESTED-05: close router cleans up nested client") {
    zmqae::router r{"inproc://test-nested-05"};
    r.set_nested_endpoint("inproc://test-nested-05-nonexistent");
    CHECK(r.is_open());
    r.close();
    CHECK_FALSE(r.is_open());
}

TEST_CASE("NESTED-06: set_nested_endpoint can be called before handlers") {
    zmqae::router inner{"inproc://test-nested-06-inner"};
    inner.on("Echo", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume(ctx->payload());
    });

    zmqae::router outer{"inproc://test-nested-06-outer"};
    outer.set_nested_endpoint("inproc://test-nested-06-inner");
    outer.on("Proxy", [](std::shared_ptr<zmqae::perform_context> ctx) {
        auto *nested = ctx->get_client();
        nested->perform("Echo", ctx->payload(),
                        [ctx](zmqae::result res) {
            if (res.is_ok()) {
                ctx->resume(res.value());
            } else {
                ctx->error(res.error());
            }
        });
    });

    zmqae::client c{"inproc://test-nested-06-outer"};
    wait_for_threads();

    bool done = false;
    c.perform("Proxy", "hello", [&](zmqae::result res) {
        if (res.is_ok() && res.value() == "hello") {
            done = true;
        }
    });

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{500};
    while (!done && std::chrono::steady_clock::now() < deadline) {
        outer.poll();
        inner.poll();
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(done);
}
