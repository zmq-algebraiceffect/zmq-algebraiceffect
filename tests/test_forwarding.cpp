#define DOCTEST_CONFIG_NO_MULTI_LANE_ATOMICS

#include "doctest.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "zmqae/zmqae.hpp"

namespace {

void drain_poll_all(int max_ms = 500) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{max_ms};
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

void wait_for_threads(int ms = 100) {
    std::this_thread::sleep_for(std::chrono::milliseconds{ms});
}

void poll_loop(std::vector<zmqae::router *> routers,
               std::vector<zmqae::client *> clients,
               int max_ms = 500) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{max_ms};
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto *r : routers) {
            r->poll();
        }
        for (auto *c : clients) {
            c->poll();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

} // namespace

TEST_CASE("FWD-01: unhandled effect forwarded to parent") {
    zmqae::router parent{"inproc://test-fwd-01-parent"};
    parent.on("ParentEffect", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("parent_handled");
    });

    zmqae::router child{"inproc://test-fwd-01-child"};
    child.set_parent("inproc://test-fwd-01-parent");

    zmqae::client c{"inproc://test-fwd-01-child"};
    wait_for_threads();

    bool ok = false;
    c.perform("ParentEffect", {}, [&](zmqae::result res) {
        if (res.is_ok() && res.value() == "parent_handled") {
            ok = true;
        }
    });

    poll_loop({&child, &parent}, {&c});
    CHECK(ok);
}

TEST_CASE("FWD-02: local handler takes priority over parent") {
    zmqae::router parent{"inproc://test-fwd-02-parent"};
    parent.on("Shared", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("parent");
    });

    zmqae::router child{"inproc://test-fwd-02-child"};
    child.on("Shared", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("child");
    });
    child.set_parent("inproc://test-fwd-02-parent");

    zmqae::client c{"inproc://test-fwd-02-child"};
    wait_for_threads();

    std::string who;
    c.perform("Shared", {}, [&](zmqae::result res) {
        if (res.is_ok()) {
            who = res.value().get<std::string>();
        }
    });

    poll_loop({&child, &parent}, {&c});
    CHECK(who == "child");
}

TEST_CASE("FWD-03: parent error forwarded back to client") {
    zmqae::router parent{"inproc://test-fwd-03-parent"};
    parent.on("Fail", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->error("parent_says_no");
    });

    zmqae::router child{"inproc://test-fwd-03-child"};
    child.set_parent("inproc://test-fwd-03-parent");

    zmqae::client c{"inproc://test-fwd-03-child"};
    wait_for_threads();

    bool got_error = false;
    std::string error_msg;
    c.perform("Fail", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            got_error = true;
            error_msg = res.error();
        }
    });

    poll_loop({&child, &parent}, {&c});
    CHECK(got_error);
    CHECK(error_msg.find("parent_says_no") != std::string::npos);
}

TEST_CASE("FWD-04: forwarding with binary data round-trip") {
    zmqae::router parent{"inproc://test-fwd-04-parent"};
    parent.on("Binarize", [](std::shared_ptr<zmqae::perform_context> ctx) {
        std::vector<std::vector<std::byte>> bins;
        for (int i = 0; i < ctx->binary_count(); ++i) {
            bins.push_back(ctx->binary(i));
        }
        ctx->resume("done", bins);
    });

    zmqae::router child{"inproc://test-fwd-04-child"};
    child.set_parent("inproc://test-fwd-04-parent");

    zmqae::client c{"inproc://test-fwd-04-child"};
    wait_for_threads();

    auto bin_data = std::vector<std::byte>{
        static_cast<std::byte>('B'), static_cast<std::byte>('I'), static_cast<std::byte>('N')};
    std::vector<std::vector<std::byte>> send_bins = {bin_data};

    bool ok = false;
    c.perform("Binarize", {}, send_bins, [&](zmqae::result res) {
        if (res.is_ok() && res.binary_count() == 1) {
            auto &recv = res.binary(0);
            ok = (recv.size() == 3 &&
                  recv[0] == static_cast<std::byte>('B') &&
                  recv[1] == static_cast<std::byte>('I') &&
                  recv[2] == static_cast<std::byte>('N'));
        }
    });

    poll_loop({&child, &parent}, {&c});
    CHECK(ok);
}

TEST_CASE("FWD-05: no handler and no parent returns error") {
    zmqae::router child{"inproc://test-fwd-05-child"};

    zmqae::client c{"inproc://test-fwd-05-child"};
    wait_for_threads();

    std::string error_msg;
    c.perform("OrphanEffect", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            error_msg = res.error();
        }
    });

    poll_loop({&child}, {&c});
    CHECK(error_msg.find("no handler for") != std::string::npos);
}

TEST_CASE("FWD-06: set_parent can be called after construction") {
    zmqae::router parent{"inproc://test-fwd-06-parent"};
    parent.on("Late", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("late_bound");
    });

    zmqae::router child{"inproc://test-fwd-06-child"};

    zmqae::client c{"inproc://test-fwd-06-child"};
    wait_for_threads();

    child.set_parent("inproc://test-fwd-06-parent");
    wait_for_threads();

    bool ok = false;
    c.perform("Late", {}, [&](zmqae::result res) {
        if (res.is_ok() && res.value() == "late_bound") {
            ok = true;
        }
    });

    poll_loop({&child, &parent}, {&c});
    CHECK(ok);
}

TEST_CASE("FWD-07: three-level forwarding chain") {
    zmqae::router grandparent{"inproc://test-fwd-07-gp"};
    grandparent.on("Deep", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("grandparent");
    });

    zmqae::router mid{"inproc://test-fwd-07-mid"};
    mid.set_parent("inproc://test-fwd-07-gp");

    zmqae::router leaf{"inproc://test-fwd-07-leaf"};
    leaf.set_parent("inproc://test-fwd-07-mid");

    zmqae::client c{"inproc://test-fwd-07-leaf"};
    wait_for_threads();

    bool ok = false;
    c.perform("Deep", {}, [&](zmqae::result res) {
        if (res.is_ok() && res.value() == "grandparent") {
            ok = true;
        }
    });

    poll_loop({&leaf, &mid, &grandparent}, {&c});
    CHECK(ok);
}

TEST_CASE("FWD-08: clear_parent stops forwarding") {
    zmqae::router parent{"inproc://test-fwd-08-parent"};
    parent.on("Gone", [](std::shared_ptr<zmqae::perform_context> ctx) {
        ctx->resume("should_not_reach");
    });

    zmqae::router child{"inproc://test-fwd-08-child"};
    child.set_parent("inproc://test-fwd-08-parent");
    child.clear_parent();

    zmqae::client c{"inproc://test-fwd-08-child"};
    wait_for_threads();

    std::string error_msg;
    c.perform("Gone", {}, [&](zmqae::result res) {
        if (res.is_error()) {
            error_msg = res.error();
        }
    });

    poll_loop({&child, &parent}, {&c});
    CHECK(error_msg.find("no handler for") != std::string::npos);
}
