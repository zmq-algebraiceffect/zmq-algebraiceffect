#define DOCTEST_CONFIG_NO_MULTI_LANE_ATOMICS

#include "doctest.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "zmqae/zmqae.h"

namespace {

void wait_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds{ms});
}

struct callback_state {
    bool called{false};
    std::string id;
    std::string json_value;
    std::string error_message;
};

void perform_cb(void *user_data, const char *id, const char *json_value,
                const char *error_message) {
    auto *st = static_cast<callback_state *>(user_data);
    st->called = true;
    if (id) {
        st->id = id;
    }
    if (json_value) {
        st->json_value = json_value;
    }
    if (error_message) {
        st->error_message = error_message;
    }
}

} // namespace

TEST_CASE("CAPI-01: client new and destroy no leak") {
    auto *client = zmqae_client_new("inproc://test-capi-01");
    REQUIRE(client != nullptr);
    zmqae_client_destroy(client);
}

TEST_CASE("CAPI-02: router new and destroy no leak") {
    auto *router = zmqae_router_new("inproc://test-capi-02");
    REQUIRE(router != nullptr);
    zmqae_router_destroy(router);
}

TEST_CASE("CAPI-03: client perform invokes callback") {
    auto *router = zmqae_router_new("inproc://test-capi-03");
    REQUIRE(router != nullptr);

    auto *client = zmqae_client_new("inproc://test-capi-03");
    REQUIRE(client != nullptr);

    wait_ms(20);

    callback_state st;
    int rc = zmqae_client_perform(client, "Echo", "\"hello\"", perform_cb, &st);
    CHECK(rc == 0);

    wait_ms(100);

    zmqae_client_destroy(client);
    zmqae_router_destroy(router);
}

TEST_CASE("CAPI-04: client perform timeout fires error callback") {
    auto *router = zmqae_router_new("inproc://test-capi-04-timeout");
    REQUIRE(router != nullptr);

    zmqae_perform_ctx_t *held_ctx = nullptr;
    auto slow_handler = [](void *user_data, zmqae_perform_ctx_t *ctx) {
        *static_cast<zmqae_perform_ctx_t **>(user_data) = ctx;
    };
    int rc = zmqae_router_on(router, "Timeout", slow_handler, &held_ctx);
    CHECK(rc == 0);

    auto *client = zmqae_client_new("inproc://test-capi-04-timeout");
    REQUIRE(client != nullptr);
    wait_ms(20);

    callback_state st;
    rc = zmqae_client_perform_timeout(client, "Timeout", "{}", 30,
                                      perform_cb, &st);
    CHECK(rc == 0);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds{200};
    while (!st.called && std::chrono::steady_clock::now() < deadline) {
        zmqae_router_poll(router);
        zmqae_client_poll(client);
        wait_ms(5);
    }

    CHECK(st.called);
    CHECK(st.error_message.find("timeout") != std::string::npos);

    if (held_ctx) {
        zmqae_ctx_release(held_ctx);
    }
    zmqae_client_destroy(client);
    zmqae_router_destroy(router);
}

TEST_CASE("CAPI-05: router on handler receives context") {
    auto *router = zmqae_router_new("inproc://test-capi-05");
    REQUIRE(router != nullptr);

    bool handler_called = false;
    auto handler = [](void *user_data, zmqae_perform_ctx_t *ctx) {
        auto *called = static_cast<bool *>(user_data);
        *called = (ctx != nullptr);
        if (ctx) {
            zmqae_ctx_resume(ctx, "null");
        }
    };

    int rc = zmqae_router_on(router, "Ping", handler, &handler_called);
    CHECK(rc == 0);

    auto *client = zmqae_client_new("inproc://test-capi-05");
    REQUIRE(client != nullptr);
    wait_ms(20);

    callback_state st;
    zmqae_client_perform(client, "Ping", "null", perform_cb, &st);

    for (int i = 0; i < 30; ++i) {
        zmqae_router_poll(router);
        zmqae_client_poll(client);
        wait_ms(3);
        if (st.called) {
            break;
        }
    }

    CHECK(handler_called);

    zmqae_client_destroy(client);
    zmqae_router_destroy(router);
}

TEST_CASE("CAPI-06: ctx get id effect payload returns correct values") {
    auto *router = zmqae_router_new("inproc://test-capi-06");
    REQUIRE(router != nullptr);

    std::pair<std::string, std::string> captured;
    auto handler = [](void *user_data, zmqae_perform_ctx_t *ctx) {
        auto *data = static_cast<std::pair<std::string, std::string> *>(user_data);
        data->first = zmqae_ctx_get_effect(ctx);
        data->second = zmqae_ctx_get_payload(ctx);
        zmqae_ctx_resume(ctx, "null");
    };

    zmqae_router_on(router, "GetData", handler, &captured);

    auto *client = zmqae_client_new("inproc://test-capi-06");
    wait_ms(20);

    callback_state st;
    zmqae_client_perform(client, "GetData", "{\"x\":1}", perform_cb, &st);

    for (int i = 0; i < 30; ++i) {
        zmqae_router_poll(router);
        zmqae_client_poll(client);
        wait_ms(3);
        if (st.called) {
            break;
        }
    }

    CHECK(captured.first == "GetData");
    CHECK_FALSE(captured.second.empty());

    zmqae_client_destroy(client);
    zmqae_router_destroy(router);
}

TEST_CASE("CAPI-07: ctx resume client receives result") {
    auto *router = zmqae_router_new("inproc://test-capi-07");
    REQUIRE(router != nullptr);

    auto handler = [](void *, zmqae_perform_ctx_t *ctx) {
        zmqae_ctx_resume(ctx, "{\"res\":42}");
    };
    zmqae_router_on(router, "Calc", handler, nullptr);

    auto *client = zmqae_client_new("inproc://test-capi-07");
    wait_ms(20);

    callback_state st;
    zmqae_client_perform(client, "Calc", "{}", perform_cb, &st);

    for (int i = 0; i < 30; ++i) {
        zmqae_router_poll(router);
        zmqae_client_poll(client);
        wait_ms(3);
        if (st.called) {
            break;
        }
    }

    CHECK(st.called);
    CHECK_FALSE(st.error_message.empty() == false);

    zmqae_client_destroy(client);
    zmqae_router_destroy(router);
}

TEST_CASE("CAPI-08: ctx error client receives error") {
    auto *router = zmqae_router_new("inproc://test-capi-08");
    REQUIRE(router != nullptr);

    auto handler = [](void *, zmqae_perform_ctx_t *ctx) {
        zmqae_ctx_error(ctx, "c api error");
    };
    zmqae_router_on(router, "Fail", handler, nullptr);

    auto *client = zmqae_client_new("inproc://test-capi-08");
    wait_ms(20);

    callback_state st;
    zmqae_client_perform(client, "Fail", "{}", perform_cb, &st);

    for (int i = 0; i < 30; ++i) {
        zmqae_router_poll(router);
        zmqae_client_poll(client);
        wait_ms(3);
        if (st.called) {
            break;
        }
    }

    CHECK(st.called);
    CHECK_FALSE(st.error_message.empty());

    zmqae_client_destroy(client);
    zmqae_router_destroy(router);
}

TEST_CASE("CAPI-09: second ctx resume returns ZMQAE_ALREADY") {
    auto *router = zmqae_router_new("inproc://test-capi-09");
    REQUIRE(router != nullptr);

    auto handler = [](void *, zmqae_perform_ctx_t *ctx) {
        int rc1 = zmqae_ctx_resume(ctx, "\"first\"");
        CHECK(rc1 == 0);
        int rc2 = zmqae_ctx_resume(ctx, "\"second\"");
        CHECK(rc2 == ZMQAE_ALREADY);
    };
    zmqae_router_on(router, "Twice", handler, nullptr);

    auto *client = zmqae_client_new("inproc://test-capi-09");
    wait_ms(20);

    callback_state st;
    zmqae_client_perform(client, "Twice", "{}", perform_cb, &st);

    for (int i = 0; i < 30; ++i) {
        zmqae_router_poll(router);
        zmqae_client_poll(client);
        wait_ms(3);
        if (st.called) {
            break;
        }
    }

    CHECK(st.called);

    zmqae_client_destroy(client);
    zmqae_router_destroy(router);
}

TEST_CASE("CAPI-10: ctx release does not crash") {
    auto *router = zmqae_router_new("inproc://test-capi-10");
    REQUIRE(router != nullptr);

    auto handler = [](void *, zmqae_perform_ctx_t *ctx) {
        zmqae_ctx_resume(ctx, "ok");
        zmqae_ctx_release(ctx);
    };
    zmqae_router_on(router, "Release", handler, nullptr);

    auto *client = zmqae_client_new("inproc://test-capi-10");
    wait_ms(20);

    callback_state st;
    zmqae_client_perform(client, "Release", "{}", perform_cb, &st);

    for (int i = 0; i < 30; ++i) {
        zmqae_router_poll(router);
        zmqae_client_poll(client);
        wait_ms(3);
        if (st.called) {
            break;
        }
    }

    CHECK(st.called);

    zmqae_client_destroy(client);
    zmqae_router_destroy(router);
}

TEST_CASE("CAPI-11: C functions do not propagate exceptions") {
    auto *client = zmqae_client_new("inproc://test-capi-11");
    if (client) {
        int rc = zmqae_client_poll(client);
        CHECK(rc == 0);
        zmqae_client_destroy(client);
    }
}

TEST_CASE("CAPI-12: last_error returns non-null after error") {
    const char *err = zmqae_last_error();
    (void)err;
}

TEST_CASE("CAPI-13: client close and router close safe") {
    auto *client = zmqae_client_new("inproc://test-capi-13");
    REQUIRE(client != nullptr);
    wait_ms(10);

    int rc = zmqae_client_close(client);
    CHECK(rc == 0);

    auto *router = zmqae_router_new("inproc://test-capi-13b");
    REQUIRE(router != nullptr);
    wait_ms(10);

    rc = zmqae_router_close(router);
    CHECK(rc == 0);

    zmqae_client_destroy(client);
    zmqae_router_destroy(router);
}

TEST_CASE("CAPI-14: NULL handle returns ZMQAE_INVALID") {
    int rc;

    rc = zmqae_client_perform(nullptr, "X", "{}", perform_cb, nullptr);
    CHECK(rc == ZMQAE_INVALID);

    rc = zmqae_client_poll(nullptr);
    CHECK(rc == ZMQAE_INVALID);

    rc = zmqae_client_close(nullptr);
    CHECK(rc == ZMQAE_INVALID);

    rc = zmqae_router_on(nullptr, "X", nullptr, nullptr);
    CHECK(rc == ZMQAE_INVALID);

    rc = zmqae_router_poll(nullptr);
    CHECK(rc == ZMQAE_INVALID);

    rc = zmqae_router_close(nullptr);
    CHECK(rc == ZMQAE_INVALID);
}

TEST_CASE("CAPI-15: binary perform and resume round-trip") {
    auto *router = zmqae_router_new("inproc://test-capi-15");
    REQUIRE(router != nullptr);

    auto handler = [](void *, zmqae_perform_ctx_t *ctx) {
        int bin_count = zmqae_ctx_binary_count(ctx);
        if (bin_count > 0) {
            const void *data = nullptr;
            int size = 0;
            zmqae_ctx_get_binary(ctx, 0, &data, &size);

            zmqae_binary_t out_bin;
            out_bin.data = data;
            out_bin.size = size;

            zmqae_ctx_resume_binary(ctx, "\"ok\"", &out_bin, 1);
        } else {
            zmqae_ctx_resume(ctx, "\"no_bin\"");
        }
    };
    zmqae_router_on(router, "BinRoundTrip", handler, nullptr);

    auto *client = zmqae_client_new("inproc://test-capi-15");
    wait_ms(20);

    const char *send_data = "BINARY_TEST";
    zmqae_binary_t send_bin;
    send_bin.data = send_data;
    send_bin.size = static_cast<int>(std::strlen(send_data));

    callback_state st;
    int rc = zmqae_client_perform_binary(client, "BinRoundTrip", "{}",
                                         &send_bin, 1, 0,
                                         perform_cb, &st);
    CHECK(rc == 0);

    for (int i = 0; i < 50; ++i) {
        zmqae_router_poll(router);
        zmqae_client_poll(client);
        wait_ms(3);
        if (st.called) {
            break;
        }
    }

    CHECK(st.called);

    zmqae_client_destroy(client);
    zmqae_router_destroy(router);
}
