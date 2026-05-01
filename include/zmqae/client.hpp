#pragma once

#ifndef SPDLOG_HEADER_ONLY
#define SPDLOG_HEADER_ONLY
#endif

#include <atomic>
#include <chrono>
#include <memory>
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
#include "types.hpp"
#include "uuid.hpp"

namespace zmqae {

class client {
    struct state {
        zmq::socket_t socket;
        std::shared_ptr<detail::mpsc_queue<std::vector<zmq::message_t>>> send_queue;
        std::shared_ptr<detail::mpsc_queue<result>> recv_queue;
        std::atomic<bool> running{true};
    };

    struct pending_entry {
        std::string effect;
        perform_callback callback;
        std::chrono::steady_clock::time_point deadline;
        bool has_timeout{false};
    };

public:
    explicit client(const std::string &endpoint)
        : client{endpoint, detail::get_default_context()}
    {}

    client(const std::string &endpoint, zmq::context_t &ctx) {
        state_ = std::make_shared<state>();
        state_->socket = zmq::socket_t{ctx, zmq::socket_type::dealer};
        state_->socket.set(zmq::sockopt::linger, 0);
        state_->socket.connect(endpoint);

        state_->send_queue =
            std::make_shared<detail::mpsc_queue<std::vector<zmq::message_t>>>();
        state_->recv_queue =
            std::make_shared<detail::mpsc_queue<result>>();

        start_zmq_thread();
    }

    ~client() {
        close();
    }

    client(const client &) = delete;
    client &operator=(const client &) = delete;

    client(client &&other) noexcept
        : state_{std::move(other.state_)}
        , zmq_thread_{std::move(other.zmq_thread_)}
        , pending_{std::move(other.pending_)}
    {}

    client &operator=(client &&other) noexcept {
        if (this != &other) {
            close();
            state_ = std::move(other.state_);
            zmq_thread_ = std::move(other.zmq_thread_);
            pending_ = std::move(other.pending_);
        }
        return *this;
    }

    void perform(const std::string &effect,
                 const json &payload,
                 perform_callback callback) {
        perform(effect, payload, {}, std::move(callback), 0);
    }

    void perform(const std::string &effect,
                 const json &payload,
                 perform_callback callback,
                 int timeout_ms) {
        perform(effect, payload, {}, std::move(callback), timeout_ms);
    }

    void perform(const std::string &effect,
                 const json &payload,
                 const std::vector<std::vector<std::byte>> &bins,
                 perform_callback callback) {
        perform(effect, payload, bins, std::move(callback), 0);
    }

    void perform(const std::string &effect,
                 const json &payload,
                 const std::vector<std::vector<std::byte>> &bins,
                 perform_callback callback,
                 int timeout_ms) {
        if (!state_ || !state_->running.load()) {
            return;
        }

        std::string id = generate_uuid();

        perform_message msg;
        msg.id = id;
        msg.effect = effect;
        msg.payload = payload;
        msg.binary_frames = static_cast<int>(bins.size());
        msg.binary_data = bins;

        auto frames = detail::serialize_perform(msg);
        state_->send_queue->push(std::move(frames));

        pending_entry entry;
        entry.effect = effect;
        entry.callback = std::move(callback);
        entry.has_timeout = timeout_ms > 0;
        if (entry.has_timeout) {
            entry.deadline = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds{timeout_ms};
        }
        pending_.emplace(std::move(id), std::move(entry));
    }

    void poll() {
        if (!state_ || !state_->running.load()) {
            return;
        }

        auto items = state_->recv_queue->drain();
        for (auto &item : items) {
            auto it = pending_.find(item.id());
            if (it != pending_.end()) {
                bool final = item.is_final();
                perform_callback cb = it->second.callback;
                if (final) {
                    pending_.erase(it);
                }
                cb(std::move(item));
            }
        }

        auto now = std::chrono::steady_clock::now();
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.has_timeout && now > it->second.deadline) {
                perform_callback cb = std::move(it->second.callback);
                std::string id = it->first;
                std::string effect = it->second.effect;
                it = pending_.erase(it);
                cb(result::make_timeout(std::move(id), std::move(effect)));
            } else {
                ++it;
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
        pending_.clear();
    }

private:
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
                    if (msgs.empty()) {
                        continue;
                    }

                    zmq::message_t &body = msgs[0];
                    std::vector<zmq::message_t> bins;
                    for (size_t i = 1; i < msgs.size(); ++i) {
                        bins.push_back(std::move(msgs[i]));
                    }

                    auto parsed = detail::parse_incoming_message(body, bins);
                    if (parsed) {
                        s->recv_queue->push(std::move(*parsed));
                    } else {
                        SPDLOG_ERROR("client: failed to parse incoming message: {}",
                                     parsed.error());
                    }
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
    std::unordered_map<std::string, pending_entry> pending_;
};

}
