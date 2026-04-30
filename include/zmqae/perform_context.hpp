#pragma once

#ifndef SPDLOG_HEADER_ONLY
#define SPDLOG_HEADER_ONLY
#endif

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include "detail/message.hpp"
#include "detail/thread_queue.hpp"
#include "types.hpp"

namespace zmqae {

class perform_context {
public:
    perform_context(
        std::string id,
        std::string effect,
        json payload,
        std::vector<std::vector<std::byte>> binary_data,
        zmq::message_t identity_frame,
        std::shared_ptr<detail::mpsc_queue<std::vector<zmq::message_t>>> send_queue)
        : id_{std::move(id)}
        , effect_{std::move(effect)}
        , payload_(std::move(payload))
        , binary_data_{std::move(binary_data)}
        , identity_frame_{std::move(identity_frame)}
        , send_queue_{std::move(send_queue)}
        , resumed_{false}
    {}

    ~perform_context() {
        bool expected = false;
        if (resumed_.compare_exchange_strong(expected, true)) {
            try {
                auto frames = detail::serialize_error(
                    id_, "handler error: context dropped without resume");
                send_frame(std::move(frames));
                SPDLOG_WARN("perform_context destroyed without resume: id={}", id_);
            } catch (const std::exception &e) {
                SPDLOG_ERROR("perform_context destructor error: {}", e.what());
            } catch (...) {
                SPDLOG_ERROR("perform_context destructor error: unknown");
            }
        }
    }

    perform_context(const perform_context &) = delete;
    perform_context &operator=(const perform_context &) = delete;
    perform_context(perform_context &&) = delete;
    perform_context &operator=(perform_context &&) = delete;

    const std::string &id() const { return id_; }
    const std::string &effect() const { return effect_; }
    const json &payload() const { return payload_; }
    int binary_count() const { return static_cast<int>(binary_data_.size()); }
    const std::vector<std::byte> &binary(int index) const { return binary_data_.at(index); }

    void resume(const json &value) {
        resume(value, {});
    }

    void resume(const json &value,
                const std::vector<std::vector<std::byte>> &bins) {
        bool expected = false;
        if (!resumed_.compare_exchange_strong(expected, true)) {
            SPDLOG_WARN("perform_context::resume() called but already resumed: id={}", id_);
            return;
        }

        resume_message msg;
        msg.id = id_;
        msg.value = value;
        msg.binary_frames = static_cast<int>(bins.size());
        msg.binary_data = bins;

        auto frames = detail::serialize_resume(msg);
        send_frame(std::move(frames));
    }

    void error(const std::string &message) {
        bool expected = false;
        if (!resumed_.compare_exchange_strong(expected, true)) {
            SPDLOG_WARN("perform_context::error() called but already resumed: id={}", id_);
            return;
        }

        auto frames = detail::serialize_error(id_, message);
        send_frame(std::move(frames));
    }

    bool is_resumed() const { return resumed_.load(); }

private:
    void send_frame(std::vector<zmq::message_t> frames) {
        std::vector<zmq::message_t> full_frames;
        full_frames.reserve(frames.size() + 1);
        full_frames.push_back(
            zmq::message_t{identity_frame_.data(), identity_frame_.size()});
        for (auto &frame : frames) {
            full_frames.push_back(std::move(frame));
        }
        send_queue_->push(std::move(full_frames));
    }

    std::string id_;
    std::string effect_;
    json payload_;
    std::vector<std::vector<std::byte>> binary_data_;
    zmq::message_t identity_frame_;
    std::shared_ptr<detail::mpsc_queue<std::vector<zmq::message_t>>> send_queue_;
    std::atomic<bool> resumed_;
};

}
