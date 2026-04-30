#include "zmqae/zmqae.h"
#include "zmqae/zmqae.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

thread_local std::string tl_last_error;

void set_last_error(const std::string &msg) {
    tl_last_error = msg;
}

template <typename T>
T *opaque_ptr(void *p) {
    return static_cast<T *>(p);
}

template <typename T>
const T *opaque_ptr(const void *p) {
    return static_cast<const T *>(p);
}

struct c_client {
    std::unique_ptr<zmqae::client> impl;
};

struct c_router {
    std::unique_ptr<zmqae::router> impl;
    std::unordered_map<std::string, std::pair<zmqae_handler_fn, void *>> handlers;
};

struct c_ctx {
    std::shared_ptr<zmqae::perform_context> impl;
};

} // namespace

extern "C" {

const char *zmqae_last_error(void) {
    if (tl_last_error.empty()) {
        return "no error";
    }
    return tl_last_error.c_str();
}

zmqae_client_t *zmqae_client_new(const char *endpoint) {
    try {
        if (!endpoint) {
            set_last_error("endpoint is null");
            return nullptr;
        }
        auto *c = new c_client;
        c->impl = std::make_unique<zmqae::client>(std::string{endpoint});
        return reinterpret_cast<zmqae_client_t *>(c);
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return nullptr;
    } catch (...) {
        set_last_error("unknown error");
        return nullptr;
    }
}

void zmqae_client_destroy(zmqae_client_t *client) {
    try {
        delete opaque_ptr<c_client>(client);
    } catch (...) {
    }
}

int zmqae_client_perform(zmqae_client_t *client, const char *effect,
                          const char *json_payload,
                          zmqae_perform_callback callback, void *user_data) {
    try {
        if (!client) {
            return ZMQAE_INVALID;
        }
        auto *c = opaque_ptr<c_client>(client);
        if (!c->impl || !c->impl->is_open()) {
            return ZMQAE_CLOSED;
        }
        auto json = json_payload ? zmqae::json::parse(json_payload) : zmqae::json{};
        c->impl->perform(std::string{effect}, json,
                         [callback, user_data](zmqae::result res) {
            const char *id_str = res.id().c_str();
            const char *val_str = res.is_ok() ? res.value().dump().c_str() : nullptr;
            const char *err_str = res.is_error() ? res.error().c_str() : nullptr;
            callback(user_data, id_str, val_str, err_str);
        });
        return ZMQAE_OK;
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return ZMQAE_ERROR;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

int zmqae_client_perform_timeout(zmqae_client_t *client, const char *effect,
                                  const char *json_payload, int timeout_ms,
                                  zmqae_perform_callback callback,
                                  void *user_data) {
    try {
        if (!client) {
            return ZMQAE_INVALID;
        }
        auto *c = opaque_ptr<c_client>(client);
        if (!c->impl || !c->impl->is_open()) {
            return ZMQAE_CLOSED;
        }
        auto json = json_payload ? zmqae::json::parse(json_payload) : zmqae::json{};
        c->impl->perform(std::string{effect}, json,
                         [callback, user_data](zmqae::result res) {
            const char *id_str = res.id().c_str();
            const char *val_str = res.is_ok() ? res.value().dump().c_str() : nullptr;
            const char *err_str = res.is_error() ? res.error().c_str() : nullptr;
            callback(user_data, id_str, val_str, err_str);
        }, timeout_ms);
        return ZMQAE_OK;
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return ZMQAE_ERROR;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

int zmqae_client_perform_binary(zmqae_client_t *client, const char *effect,
                                 const char *json_payload,
                                 const zmqae_binary_t *bins, int bin_count,
                                 int timeout_ms,
                                 zmqae_perform_callback callback,
                                 void *user_data) {
    try {
        if (!client) {
            return ZMQAE_INVALID;
        }
        auto *c = opaque_ptr<c_client>(client);
        if (!c->impl || !c->impl->is_open()) {
            return ZMQAE_CLOSED;
        }
        auto json = json_payload ? zmqae::json::parse(json_payload) : zmqae::json{};

        std::vector<std::vector<std::byte>> vec_bins;
        if (bins && bin_count > 0) {
            vec_bins.reserve(bin_count);
            for (int i = 0; i < bin_count; ++i) {
                auto *data = static_cast<const std::byte *>(bins[i].data);
                vec_bins.emplace_back(data, data + bins[i].size);
            }
        }

        c->impl->perform(std::string{effect}, json, vec_bins,
                         [callback, user_data](zmqae::result res) {
            const char *id_str = res.id().c_str();
            const char *val_str = res.is_ok() ? res.value().dump().c_str() : nullptr;
            const char *err_str = res.is_error() ? res.error().c_str() : nullptr;
            callback(user_data, id_str, val_str, err_str);
        }, timeout_ms);
        return ZMQAE_OK;
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return ZMQAE_ERROR;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

int zmqae_client_poll(zmqae_client_t *client) {
    try {
        if (!client) {
            return ZMQAE_INVALID;
        }
        auto *c = opaque_ptr<c_client>(client);
        if (!c->impl) {
            return ZMQAE_CLOSED;
        }
        c->impl->poll();
        return ZMQAE_OK;
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return ZMQAE_ERROR;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

int zmqae_client_close(zmqae_client_t *client) {
    try {
        if (!client) {
            return ZMQAE_INVALID;
        }
        auto *c = opaque_ptr<c_client>(client);
        if (c->impl) {
            c->impl->close();
        }
        return ZMQAE_OK;
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return ZMQAE_ERROR;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

zmqae_router_t *zmqae_router_new(const char *endpoint) {
    try {
        if (!endpoint) {
            set_last_error("endpoint is null");
            return nullptr;
        }
        auto *r = new c_router;
        r->impl = std::make_unique<zmqae::router>(std::string{endpoint});
        return reinterpret_cast<zmqae_router_t *>(r);
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return nullptr;
    } catch (...) {
        set_last_error("unknown error");
        return nullptr;
    }
}

void zmqae_router_destroy(zmqae_router_t *router) {
    try {
        delete opaque_ptr<c_router>(router);
    } catch (...) {
    }
}

int zmqae_router_on(zmqae_router_t *router, const char *effect,
                     zmqae_handler_fn handler, void *user_data) {
    try {
        if (!router) {
            return ZMQAE_INVALID;
        }
        auto *r = opaque_ptr<c_router>(router);
        if (!r->impl) {
            return ZMQAE_CLOSED;
        }
        std::string key{effect};
        r->handlers[key] = {handler, user_data};
        r->impl->on(key, [r, key](std::shared_ptr<zmqae::perform_context> ctx) {
            auto it = r->handlers.find(key);
            if (it != r->handlers.end()) {
                auto *wrapped = new c_ctx;
                wrapped->impl = ctx;
                it->second.first(it->second.second,
                                 reinterpret_cast<zmqae_perform_ctx_t *>(wrapped));
            }
        });
        return ZMQAE_OK;
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return ZMQAE_ERROR;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

int zmqae_router_off(zmqae_router_t *router, const char *effect) {
    try {
        if (!router) {
            return ZMQAE_INVALID;
        }
        auto *r = opaque_ptr<c_router>(router);
        if (!r->impl) {
            return ZMQAE_CLOSED;
        }
        std::string key{effect};
        r->handlers.erase(key);
        r->impl->off(key);
        return ZMQAE_OK;
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return ZMQAE_ERROR;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

int zmqae_router_poll(zmqae_router_t *router) {
    try {
        if (!router) {
            return ZMQAE_INVALID;
        }
        auto *r = opaque_ptr<c_router>(router);
        if (!r->impl) {
            return ZMQAE_CLOSED;
        }
        r->impl->poll();
        return ZMQAE_OK;
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return ZMQAE_ERROR;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

int zmqae_router_close(zmqae_router_t *router) {
    try {
        if (!router) {
            return ZMQAE_INVALID;
        }
        auto *r = opaque_ptr<c_router>(router);
        if (r->impl) {
            r->impl->close();
        }
        return ZMQAE_OK;
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return ZMQAE_ERROR;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

const char *zmqae_ctx_get_id(zmqae_perform_ctx_t *ctx) {
    if (!ctx) {
        return "";
    }
    return opaque_ptr<c_ctx>(ctx)->impl->id().c_str();
}

const char *zmqae_ctx_get_effect(zmqae_perform_ctx_t *ctx) {
    if (!ctx) {
        return "";
    }
    return opaque_ptr<c_ctx>(ctx)->impl->effect().c_str();
}

const char *zmqae_ctx_get_payload(zmqae_perform_ctx_t *ctx) {
    if (!ctx) {
        return "";
    }
    return opaque_ptr<c_ctx>(ctx)->impl->payload().dump().c_str();
}

int zmqae_ctx_binary_count(zmqae_perform_ctx_t *ctx) {
    if (!ctx) {
        return 0;
    }
    return opaque_ptr<c_ctx>(ctx)->impl->binary_count();
}

int zmqae_ctx_get_binary(zmqae_perform_ctx_t *ctx, int index,
                          const void **out_data, int *out_size) {
    if (!ctx || index < 0) {
        return ZMQAE_INVALID;
    }
    auto *c = opaque_ptr<c_ctx>(ctx);
    try {
        auto &bin = c->impl->binary(index);
        if (out_data) {
            *out_data = bin.data();
        }
        if (out_size) {
            *out_size = static_cast<int>(bin.size());
        }
        return ZMQAE_OK;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

int zmqae_ctx_resume(zmqae_perform_ctx_t *ctx, const char *json_value) {
    try {
        if (!ctx) {
            return ZMQAE_INVALID;
        }
        auto *c = opaque_ptr<c_ctx>(ctx);
        if (c->impl->is_resumed()) {
            return ZMQAE_ALREADY;
        }
        auto val = json_value ? zmqae::json::parse(json_value) : zmqae::json{};
        c->impl->resume(val);
        return ZMQAE_OK;
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return ZMQAE_ERROR;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

int zmqae_ctx_resume_binary(zmqae_perform_ctx_t *ctx, const char *json_value,
                             const zmqae_binary_t *bins, int bin_count) {
    try {
        if (!ctx) {
            return ZMQAE_INVALID;
        }
        auto *c = opaque_ptr<c_ctx>(ctx);
        if (c->impl->is_resumed()) {
            return ZMQAE_ALREADY;
        }
        auto val = json_value ? zmqae::json::parse(json_value) : zmqae::json{};

        std::vector<std::vector<std::byte>> vec_bins;
        if (bins && bin_count > 0) {
            vec_bins.reserve(bin_count);
            for (int i = 0; i < bin_count; ++i) {
                auto *data = static_cast<const std::byte *>(bins[i].data);
                vec_bins.emplace_back(data, data + bins[i].size);
            }
        }

        c->impl->resume(val, vec_bins);
        return ZMQAE_OK;
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return ZMQAE_ERROR;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

int zmqae_ctx_error(zmqae_perform_ctx_t *ctx, const char *error_message) {
    try {
        if (!ctx) {
            return ZMQAE_INVALID;
        }
        auto *c = opaque_ptr<c_ctx>(ctx);
        if (c->impl->is_resumed()) {
            return ZMQAE_ALREADY;
        }
        c->impl->error(error_message ? std::string{error_message}
                                      : std::string{"unknown error"});
        return ZMQAE_OK;
    } catch (const std::exception &e) {
        set_last_error(e.what());
        return ZMQAE_ERROR;
    } catch (...) {
        return ZMQAE_ERROR;
    }
}

void zmqae_ctx_release(zmqae_perform_ctx_t *ctx) {
    try {
        delete opaque_ptr<c_ctx>(ctx);
    } catch (...) {
    }
}

}
