#pragma once

#ifndef SPDLOG_HEADER_ONLY
#define SPDLOG_HEADER_ONLY
#endif

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include "detail/context.hpp"
#include "detail/message.hpp"
#include "detail/thread_queue.hpp"
#include "perform_context.hpp"
#include "types.hpp"
#include "uuid.hpp"

namespace zmqae {

class router {
    struct state {
        zmq::socket_t socket;
        std::shared_ptr<detail::mpsc_queue<std::vector<zmq::message_t>>> send_queue;
        std::shared_ptr<detail::mpsc_queue<std::shared_ptr<perform_context>>> recv_queue;
        std::atomic<bool> running{true};
    };

public:
    explicit router(const std::string &endpoint)
        : router{endpoint, detail::get_default_context()}
    {}

    router(const std::string &endpoint, zmq::context_t &ctx) {
        state_ = std::make_shared<state>();
        state_->socket = zmq::socket_t{ctx, zmq::socket_type::router};
        state_->socket.set(zmq::sockopt::linger, 0);
        state_->socket.bind(endpoint);

        state_->send_queue =
            std::make_shared<detail::mpsc_queue<std::vector<zmq::message_t>>>();
        state_->recv_queue =
            std::make_shared<detail::mpsc_queue<std::shared_ptr<perform_context>>>();

        start_zmq_thread();
    }

    ~router() {
        close();
    }

    router(const router &) = delete;
    router &operator=(const router &) = delete;

    router(router &&other) noexcept
        : state_{std::move(other.state_)}
        , zmq_thread_{std::move(other.zmq_thread_)}
        , handlers_{std::move(other.handlers_)}
        , unregistered_handler_{std::move(other.unregistered_handler_)}
    {}

    router &operator=(router &&other) noexcept {
        if (this != &other) {
            close();
            state_ = std::move(other.state_);
            zmq_thread_ = std::move(other.zmq_thread_);
            handlers_ = std::move(other.handlers_);
            unregistered_handler_ = std::move(other.unregistered_handler_);
        }
        return *this;
    }

    void on(const std::string &effect, handler_fn handler) {
        handlers_[effect] = std::move(handler);
    }

    void off(const std::string &effect) {
        handlers_.erase(effect);
    }

    void clear_handlers() {
        handlers_.clear();
    }

    void on_unregistered(handler_fn handler) {
        unregistered_handler_ = std::move(handler);
    }

    void poll() {
        if (!state_ || !state_->running.load()) {
            return;
        }

        auto items = state_->recv_queue->drain();
        for (auto &ctx : items) {
            auto it = handlers_.find(ctx->effect());
            if (it != handlers_.end()) {
                dispatch_handler(it->second, ctx);
            } else if (unregistered_handler_.has_value()) {
                dispatch_handler(*unregistered_handler_, ctx);
            } else {
                ctx->error("no handler for: " + ctx->effect());
            }
        }
    }

    bool is_open() const {
        return state_ && state_->running.load();
    }

    void close() {
        if (!state_) {
            return;
        }
        bool expected = true;
        if (!state_->running.compare_exchange_strong(expected, false)) {
            return;
        }
        if (zmq_thread_.joinable()) {
            zmq_thread_.join();
        }
        state_->socket.close();
        state_.reset();
        handlers_.clear();
        unregistered_handler_.reset();
    }

private:
    static void dispatch_handler(const handler_fn &handler,
                                 std::shared_ptr<perform_context> ctx) {
        try {
            handler(ctx);
        } catch (const std::exception &e) {
            if (!ctx->is_resumed()) {
                ctx->error(std::string{"handler error: "} + e.what());
            }
        } catch (...) {
            if (!ctx->is_resumed()) {
                ctx->error("handler error: unknown exception");
            }
        }
    }

    void start_zmq_thread() {
        auto s = state_;
        zmq_thread_ = std::thread{[s]() {
            while (s->running.load()) {
                while (auto frames = s->send_queue->pop()) {
                    send_multipart(s->socket, *frames);
                }

                zmq::pollitem_t items[] = {
                    {static_cast<void *>(s->socket), 0, ZMQ_POLLIN, 0}};
                zmq::poll(items, 1, std::chrono::milliseconds{10});

                if (items[0].revents & ZMQ_POLLIN) {
                    auto msgs = recv_multipart(s->socket);
                    if (msgs.size() < 2) {
                        SPDLOG_ERROR("router: received message with fewer than 2 frames");
                        continue;
                    }

                    zmq::message_t identity = std::move(msgs[0]);
                    zmq::message_t &body = msgs[1];

                    std::vector<zmq::message_t> bins;
                    for (size_t i = 2; i < msgs.size(); ++i) {
                        bins.push_back(std::move(msgs[i]));
                    }

                    auto parsed = detail::parse_perform(body, bins);
                    if (!parsed) {
                        std::string error_id = generate_uuid();
                        auto error_frames =
                            detail::serialize_error(error_id, parsed.error());

                        std::vector<zmq::message_t> full_frames;
                        full_frames.reserve(error_frames.size() + 1);
                        full_frames.push_back(std::move(identity));
                        for (auto &f : error_frames) {
                            full_frames.push_back(std::move(f));
                        }
                        s->send_queue->push(std::move(full_frames));
                        continue;
                    }

                    auto ctx = std::make_shared<perform_context>(
                        std::move(parsed->id),
                        std::move(parsed->effect),
                        std::move(parsed->payload),
                        std::move(parsed->binary_data),
                        std::move(identity),
                        s->send_queue);

                    s->recv_queue->push(std::move(ctx));
                }
            }

            while (auto frames = s->send_queue->pop()) {
                send_multipart(s->socket, *frames);
            }
        }};
    }

    static void send_multipart(zmq::socket_t &socket,
                               std::vector<zmq::message_t> &frames) {
        for (size_t i = 0; i < frames.size(); ++i) {
            auto flags = (i < frames.size() - 1)
                             ? zmq::send_flags::sndmore
                             : zmq::send_flags::none;
            socket.send(frames[i], flags);
        }
    }

    static std::vector<zmq::message_t> recv_multipart(zmq::socket_t &socket) {
        std::vector<zmq::message_t> msgs;
        while (true) {
            zmq::message_t msg;
            auto recv_result = socket.recv(msg, zmq::recv_flags::none);
            if (!recv_result.has_value()) {
                break;
            }
            bool has_more = msg.more();
            msgs.push_back(std::move(msg));
            if (!has_more) {
                break;
            }
        }
        return msgs;
    }

    std::shared_ptr<state> state_;
    std::thread zmq_thread_;
    std::unordered_map<std::string, handler_fn> handlers_;
    std::optional<handler_fn> unregistered_handler_;
};

}
