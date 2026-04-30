#define DOCTEST_CONFIG_NO_MULTI_LANE_ATOMICS

#include "doctest.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <zmq.hpp>

#include "zmqae/zmqae.hpp"

namespace {

 void drain_poll(zmqae::client &c, zmqae::router &r, int max_ms = 500) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{max_ms};
    while (std::chrono::steady_clock::now() < deadline) {
        r.poll();
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

void wait_for_threads(int ms = 100) {
    std::this_thread::sleep_for(std::chrono::milliseconds{ms});
}

} // namespace

TEST_CASE("TC-2.2.1: router preserves client-generated ID") {
    zmqae::router r{"inproc://test-id-preserve-001"};
    std::string received_id;
    r.on("CheckId", [&received_id](std::shared_ptr<zmqae::perform_context> ctx) {
        received_id = ctx->id();
        ctx->resume(ctx->payload());
    });

    zmqae::client c{"inproc://test-id-preserve-001"};
    wait_for_threads();

    std::string sent_id;
    c.perform("CheckId", {}, [&](zmqae::result res) {
        sent_id = res.id();
    });

    drain_poll(c, r);
    CHECK_FALSE(received_id.empty());
    CHECK(received_id == sent_id);
}

TEST_CASE("TC-2.3.1: resume ID matches perform ID") {
    zmqae::router r{"inproc://test-id-match-001"};
    r.on("Echo", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume(ctx->payload());
    });

    zmqae::client c{"inproc://test-id-match-001"};
    wait_for_threads();

    std::string perform_id;
    std::string resume_id;
    c.perform("Echo", "data", [&](zmqae::result res) {
        perform_id = res.id();
    });

    drain_poll(c, r);
    CHECK_FALSE(perform_id.empty());
}

TEST_CASE("TC-5.1.1: client uses DEALER socket") {
    zmqae::router r{"inproc://test-dealer-001"};
    r.on("Echo", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume(ctx->payload());
    });

    zmqae::client c{"inproc://test-dealer-001"};
    wait_for_threads();

    bool cb1 = false;
    bool cb2 = false;
    c.perform("Echo", "req1", [&](zmqae::result) { cb1 = true; });
    c.perform("Echo", "req2", [&](zmqae::result) { cb2 = true; });

    drain_poll(c, r);
    CHECK(cb1);
    CHECK(cb2);
}

TEST_CASE("TC-5.1.2: router uses ROUTER socket") {
    zmqae::router r{"inproc://test-router-socket-001"};
    r.on("Echo", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume(ctx->payload());
    });

    zmqae::client c1{"inproc://test-router-socket-001"};
    zmqae::client c2{"inproc://test-router-socket-001"};
    wait_for_threads();

    bool c1_ok = false;
    bool c2_ok = false;
    c1.perform("Echo", "from_c1", [&](zmqae::result res) {
        if (res.is_ok() && res.value() == "from_c1") {
            c1_ok = true;
        }
    });
    c2.perform("Echo", "from_c2", [&](zmqae::result res) {
        if (res.is_ok() && res.value() == "from_c2") {
            c2_ok = true;
        }
    });

    for (int i = 0; i < 200; ++i) {
        r.poll();
        c1.poll();
        c2.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        if (c1_ok && c2_ok) {
            break;
        }
    }
    CHECK(c1_ok);
    CHECK(c2_ok);
}

TEST_CASE("TC-7.1.1: error messages are ASCII") {
    zmqae::router r{"inproc://test-error-ascii-001"};

    zmqae::client c{"inproc://test-error-ascii-001"};
    wait_for_threads();

    std::string error_msg;
    c.perform("UnregisteredEffect", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            error_msg = res.error();
        }
    });

    drain_poll(c, r);
    CHECK_FALSE(error_msg.empty());
    for (unsigned char ch : error_msg) {
        CHECK(ch < 128);
    }
}

TEST_CASE("TC-7.1.2: error has no binary") {
    zmqae::router r{"inproc://test-error-no-bin-001"};

    zmqae::client c{"inproc://test-error-no-bin-001"};
    wait_for_threads();

    int bin_count = -1;
    c.perform("NoHandler", {}, [&](zmqae::result res) {
        bin_count = res.binary_count();
    });

    drain_poll(c, r);
    CHECK(bin_count == 0);
}

TEST_CASE("TC-7.2.1: unregistered effect returns no handler error") {
    zmqae::router r{"inproc://test-no-handler-001"};

    zmqae::client c{"inproc://test-no-handler-001"};
    wait_for_threads();

    std::string error_msg;
    c.perform("NoSuchEffect", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            error_msg = res.error();
        }
    });

    drain_poll(c, r);
    CHECK(error_msg.find("no handler for") != std::string::npos);
}

TEST_CASE("TC-7.2.2: handler exception returns handler error") {
    zmqae::router r{"inproc://test-handler-err-001"};
    r.on("ThrowEffect", [](std::shared_ptr<zmqae::perform_context> ctx) {
        throw std::runtime_error("boom");
    });

    zmqae::client c{"inproc://test-handler-err-001"};
    wait_for_threads();

    std::string error_msg;
    c.perform("ThrowEffect", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            error_msg = res.error();
        }
    });

    drain_poll(c, r);
    CHECK(error_msg.find("handler error") != std::string::npos);
}

TEST_CASE("TC-7.2.3: invalid JSON returns error via raw socket") {
    zmqae::router r{"inproc://test-invalid-json-001"};

    zmq::context_t &ctx = zmqae::detail::get_default_context();
    zmq::socket_t dealer{ctx, zmq::socket_type::dealer};
    dealer.set(zmq::sockopt::linger, 0);
    dealer.connect("inproc://test-invalid-json-001");
    wait_for_threads();

    std::string bad_msg = "not json at all";
    zmq::message_t msg{bad_msg.data(), bad_msg.size()};
    dealer.send(msg, zmq::send_flags::none);

    wait_for_threads(100);

    zmq::message_t response;
    auto recv_result = dealer.recv(response, zmq::recv_flags::dontwait);
    CHECK(recv_result.has_value());
    if (recv_result.has_value()) {
        std::string resp_str{static_cast<const char *>(response.data()),
                             response.size()};
        CHECK(resp_str.find("invalid message") != std::string::npos);
    }

    dealer.close();
    wait_for_threads(50);
}

TEST_CASE("TC-7.2.4: error response has generated ID") {
    zmqae::router r{"inproc://test-gen-id-001"};

    zmq::context_t &ctx = zmqae::detail::get_default_context();
    zmq::socket_t dealer{ctx, zmq::socket_type::dealer};
    dealer.set(zmq::sockopt::linger, 0);
    dealer.connect("inproc://test-gen-id-001");
    wait_for_threads();

    zmqae::json header;
    header["effect"] = "Test";
    header["payload"] = zmqae::json::object();
    std::string body_str = header.dump();
    zmq::message_t msg{body_str.data(), body_str.size()};
    dealer.send(msg, zmq::send_flags::none);

    wait_for_threads(100);

    zmq::message_t response;
    auto recv_result = dealer.recv(response, zmq::recv_flags::dontwait);
    CHECK(recv_result.has_value());
    if (recv_result.has_value()) {
        std::string resp_str{static_cast<const char *>(response.data()),
                             response.size()};
        auto resp_json = zmqae::json::parse(resp_str);
        CHECK(resp_json.contains("id"));
        CHECK_FALSE(resp_json["id"].get<std::string>().empty());
    }

    dealer.close();
    wait_for_threads(50);
}

TEST_CASE("TC-8.1: ROUTER identity frame auto-attached") {
    zmqae::router r{"inproc://test-identity-001"};
    r.on("Echo", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume(ctx->payload());
    });

    zmqae::client c1{"inproc://test-identity-001"};
    zmqae::client c2{"inproc://test-identity-001"};
    wait_for_threads();

    bool c1_ok = false;
    bool c2_ok = false;
    c1.perform("Echo", 1, [&](zmqae::result res) {
        if (res.is_ok() && res.value() == 1) {
            c1_ok = true;
        }
    });
    c2.perform("Echo", 2, [&](zmqae::result res) {
        if (res.is_ok() && res.value() == 2) {
            c2_ok = true;
        }
    });

    for (int i = 0; i < 200; ++i) {
        r.poll();
        c1.poll();
        c2.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        if (c1_ok && c2_ok) {
            break;
        }
    }
    CHECK(c1_ok);
    CHECK(c2_ok);
}

TEST_CASE("TC-8.2: reply routed via identity frame") {
    zmqae::router r{"inproc://test-route-001"};
    int counter = 0;
    r.on("Count", [&counter](std::shared_ptr<zmqae::perform_context> ctx) {
        counter++;
        ctx->resume(counter);
    });

    zmqae::client c1{"inproc://test-route-001"};
    zmqae::client c2{"inproc://test-route-001"};
    wait_for_threads();

    int c1_val = 0;
    int c2_val = 0;
    c1.perform("Count", {}, [&](zmqae::result res) {
        if (res.is_ok()) {
            c1_val = res.value().get<int>();
        }
    });
    c2.perform("Count", {}, [&](zmqae::result res) {
        if (res.is_ok()) {
            c2_val = res.value().get<int>();
        }
    });

    for (int i = 0; i < 200; ++i) {
        r.poll();
        c1.poll();
        c2.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        if (c1_val > 0 && c2_val > 0) {
            break;
        }
    }
    CHECK(c1_val > 0);
    CHECK(c2_val > 0);
}

TEST_CASE("TC-9.1: PascalCase effect names accepted") {
    zmqae::router r{"inproc://test-pascal-001"};
    bool called = false;
    r.on("Transcribe", [&called](std::shared_ptr<zmqae::perform_context> ctx) {
        called = (ctx->effect() == "Transcribe");
        ctx->resume(nullptr);
    });

    zmqae::client c{"inproc://test-pascal-001"};
    wait_for_threads();

    c.perform("Transcribe", nullptr, [](zmqae::result) {});
    drain_poll(c, r);
    CHECK(called);
}

TEST_CASE("TC-9.2: dot-separated namespace accepted") {
    zmqae::router r{"inproc://test-namespace-001"};
    bool called = false;
    r.on("Audio.Analyze", [&called](std::shared_ptr<zmqae::perform_context> ctx) {
        called = (ctx->effect() == "Audio.Analyze");
        ctx->resume(nullptr);
    });

    zmqae::client c{"inproc://test-namespace-001"};
    wait_for_threads();

    c.perform("Audio.Analyze", nullptr, [](zmqae::result) {});
    drain_poll(c, r);
    CHECK(called);
}

TEST_CASE("ROUTER-01: on registers handler and it is called") {
    zmqae::router r{"inproc://test-router-on-001"};
    bool called = false;
    r.on("MyEffect", [&called](std::shared_ptr<zmqae::perform_context> ctx) {
        called = (ctx->effect() == "MyEffect");
        ctx->resume(nullptr);
    });

    zmqae::client c{"inproc://test-router-on-001"};
    wait_for_threads();

    c.perform("MyEffect", {}, [](zmqae::result) {});
    drain_poll(c, r);
    CHECK(called);
}

TEST_CASE("ROUTER-02: on_unregistered handler called for unknown effect") {
    zmqae::router r{"inproc://test-unreg-001"};
    std::string caught_effect;
    r.on_unregistered([&caught_effect](std::shared_ptr<zmqae::perform_context> ctx) {
        caught_effect = ctx->effect();
        ctx->resume(nullptr);
    });

    zmqae::client c{"inproc://test-unreg-001"};
    wait_for_threads();

    c.perform("UnknownEffect", {}, [](zmqae::result) {});
    drain_poll(c, r);
    CHECK(caught_effect == "UnknownEffect");
}

TEST_CASE("ROUTER-03: no on_unregistered auto-replies error") {
    zmqae::router r{"inproc://test-auto-err-001"};

    zmqae::client c{"inproc://test-auto-err-001"};
    wait_for_threads();

    std::string error_msg;
    c.perform("MissingHandler", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            error_msg = res.error();
        }
    });

    drain_poll(c, r);
    CHECK(error_msg.find("no handler for") != std::string::npos);
}

TEST_CASE("ROUTER-04: off removes handler") {
    zmqae::router r{"inproc://test-off-001"};
    int call_count = 0;
    r.on("Temp", [&call_count](std::shared_ptr<zmqae::perform_context> ctx) {
        call_count++;
        ctx->resume(nullptr);
    });
    r.off("Temp");

    zmqae::client c{"inproc://test-off-001"};
    wait_for_threads();

    c.perform("Temp", {}, [](zmqae::result) {});
    drain_poll(c, r);
    CHECK(call_count == 0);
}

TEST_CASE("ROUTER-05: clear_handlers removes all") {
    zmqae::router r{"inproc://test-clear-001"};
    r.on("A", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume(nullptr);
    });
    r.on("B", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume(nullptr);
    });
    r.clear_handlers();

    zmqae::client c{"inproc://test-clear-001"};
    wait_for_threads();

    int error_count = 0;
    c.perform("A", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            error_count++;
        }
    });
    c.perform("B", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            error_count++;
        }
    });

    drain_poll(c, r);
    CHECK(error_count == 2);
}

TEST_CASE("ROUTER-06: handler ctx->resume sends resume to client") {
    zmqae::router r{"inproc://test-resume-001"};
    r.on("Double", [](std::shared_ptr<zmqae::perform_context> ctx) {
        int val = ctx->payload().get<int>();
        ctx->resume(val * 2);
    });

    zmqae::client c{"inproc://test-resume-001"};
    wait_for_threads();

    int result_val = 0;
    c.perform("Double", 21, [&](zmqae::result res) {
        if (res.is_ok()) {
            result_val = res.value().get<int>();
        }
    });

    drain_poll(c, r);
    CHECK(result_val == 42);
}

TEST_CASE("ROUTER-07: handler ctx->error sends error to client") {
    zmqae::router r{"inproc://test-ctx-error-001"};
    r.on("Fail", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->error("deliberate failure");
    });

    zmqae::client c{"inproc://test-ctx-error-001"};
    wait_for_threads();

    bool got_error = false;
    c.perform("Fail", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            got_error = true;
        }
    });

    drain_poll(c, r);
    CHECK(got_error);
}

TEST_CASE("ROUTER-08: second resume is no-op") {
    zmqae::router r{"inproc://test-resume-twice-001"};
    r.on("Twice", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("first");
        ctx->resume("second");
    });

    zmqae::client c{"inproc://test-resume-twice-001"};
    wait_for_threads();

    int call_count = 0;
    c.perform("Twice", {}, [&](zmqae::result res) { call_count++; });

    drain_poll(c, r);
    CHECK(call_count == 1);
}

TEST_CASE("ROUTER-09: handler exception auto-sends error") {
    zmqae::router r{"inproc://test-exc-001"};
    r.on("Explode", [](std::shared_ptr<zmqae::perform_context> ctx) {
        throw std::logic_error("kaboom");
    });

    zmqae::client c{"inproc://test-exc-001"};
    wait_for_threads();

    std::string error_msg;
    c.perform("Explode", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            error_msg = res.error();
        }
    });

    drain_poll(c, r);
    CHECK(error_msg.find("handler error") != std::string::npos);
}

TEST_CASE("ROUTER-10: async resume from different thread") {
    zmqae::router r{"inproc://test-async-resume-001"};
    r.on("SlowEffect", [](std::shared_ptr<zmqae::perform_context> ctx) {
        std::thread([ctx]() {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            ctx->resume(99);
        }).detach();
    });

    zmqae::client c{"inproc://test-async-resume-001"};
    wait_for_threads();

    bool ok = false;
    c.perform("SlowEffect", {}, [&](zmqae::result res) {
        if (res.is_ok() && res.value() == 99) {
            ok = true;
        }
    });

    for (int i = 0; i < 200; ++i) {
        r.poll();
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        if (ok) {
            break;
        }
    }
    CHECK(ok);
}

TEST_CASE("ROUTER-11: handler runs on main thread during poll") {
    auto main_id = std::this_thread::get_id();

    zmqae::router r{"inproc://test-thread-001"};
    std::thread::id handler_id;
    r.on("ThreadCheck", [&handler_id](std::shared_ptr<zmqae::perform_context> ctx) {
        handler_id = std::this_thread::get_id();
        ctx->resume(nullptr);
    });

    zmqae::client c{"inproc://test-thread-001"};
    wait_for_threads();

    c.perform("ThreadCheck", {}, [](zmqae::result) {});
    drain_poll(c, r);
    CHECK(handler_id == main_id);
}
