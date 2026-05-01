#pragma once

#ifndef SPDLOG_HEADER_ONLY
#define SPDLOG_HEADER_ONLY
#endif

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include "../types.hpp"
#include "../uuid.hpp"

namespace zmqae::detail {

inline std::vector<zmq::message_t> serialize_perform(const perform_message &msg) {
    json header;
    header["id"] = msg.id;
    header["effect"] = msg.effect;
    header["payload"] = msg.payload;
    if (msg.binary_frames > 0) {
        header["binary_frames"] = msg.binary_frames;
    }

    std::string header_str = header.dump();
    std::vector<zmq::message_t> frames;
    frames.reserve(1 + msg.binary_data.size());
    frames.emplace_back(header_str.data(), header_str.size());

    for (const auto &bin : msg.binary_data) {
        frames.emplace_back(bin.data(), bin.size());
    }

    return frames;
}

inline std::vector<zmq::message_t> serialize_resume(const resume_message &msg) {
    json header;
    header["id"] = msg.id;
    header["value"] = msg.value;
    if (!msg.final_field) {
        header["final"] = false;
    }
    if (msg.binary_frames > 0) {
        header["binary_frames"] = msg.binary_frames;
    }

    std::string header_str = header.dump();
    std::vector<zmq::message_t> frames;
    frames.reserve(1 + msg.binary_data.size());
    frames.emplace_back(header_str.data(), header_str.size());

    for (const auto &bin : msg.binary_data) {
        frames.emplace_back(bin.data(), bin.size());
    }

    return frames;
}

inline std::vector<zmq::message_t> serialize_error(const std::string &id,
                                                    const std::string &error_msg) {
    json header;
    header["id"] = id;
    header["error"] = error_msg;

    std::string header_str = header.dump();
    std::vector<zmq::message_t> frames;
    frames.emplace_back(header_str.data(), header_str.size());
    return frames;
}

inline std::vector<std::byte> zmq_msg_to_bytes(const zmq::message_t &msg) {
    const auto *data = static_cast<const std::byte *>(msg.data());
    size_t size = msg.size();
    return std::vector<std::byte>{data, data + size};
}

inline expected<perform_message, std::string>
parse_perform(zmq::message_t &body, std::vector<zmq::message_t> &bins) {
    std::string body_str{static_cast<const char *>(body.data()), body.size()};

    json header;
    try {
        header = json::parse(body_str);
    } catch (const json::parse_error &e) {
        return expected<perform_message, std::string>::err(
            std::string{"invalid message: JSON parse error: "} + e.what());
    }

    if (!header.is_object()) {
        return expected<perform_message, std::string>::err(
            "invalid message: header is not a JSON object");
    }

    if (!header.contains("id") || !header["id"].is_string()) {
        return expected<perform_message, std::string>::err(
            "invalid message: missing or invalid 'id' field");
    }

    if (!header.contains("effect") || !header["effect"].is_string()) {
        return expected<perform_message, std::string>::err(
            "invalid message: missing or invalid 'effect' field");
    }

    if (!header.contains("payload")) {
        return expected<perform_message, std::string>::err(
            "invalid message: missing 'payload' field");
    }

    perform_message msg;
    msg.id = header["id"].get<std::string>();
    msg.effect = header["effect"].get<std::string>();
    msg.payload = header["payload"];

    int declared_bins = 0;
    if (header.contains("binary_frames")) {
        if (!header["binary_frames"].is_number_integer()) {
            return expected<perform_message, std::string>::err(
                "invalid message: 'binary_frames' is not an integer");
        }
        declared_bins = header["binary_frames"].get<int>();
        if (declared_bins < 0) {
            return expected<perform_message, std::string>::err(
                "invalid message: 'binary_frames' is negative");
        }
    }

    if (declared_bins > 8) {
        return expected<perform_message, std::string>::err(
            "invalid message: 'binary_frames' exceeds maximum of 8");
    }

    if (static_cast<int>(bins.size()) != declared_bins) {
        return expected<perform_message, std::string>::err(
            "invalid message: binary_frames count (" + std::to_string(declared_bins) +
            ") does not match actual frame count (" + std::to_string(bins.size()) + ")");
    }

    msg.binary_frames = declared_bins;
    for (auto &bin : bins) {
        msg.binary_data.push_back(zmq_msg_to_bytes(bin));
    }

    return expected<perform_message, std::string>::ok(std::move(msg));
}

inline expected<result, std::string>
parse_incoming_message(zmq::message_t &body, std::vector<zmq::message_t> &bins) {
    std::string body_str{static_cast<const char *>(body.data()), body.size()};

    json header;
    try {
        header = json::parse(body_str);
    } catch (const json::parse_error &e) {
        SPDLOG_ERROR("parse_incoming_message: JSON parse error: {}", e.what());
        std::string fallback_id = generate_uuid();
        return expected<result, std::string>::ok(
            result::make_error(fallback_id,
                               std::string{"invalid message: JSON parse error: "} + e.what()));
    }

    if (!header.is_object()) {
        std::string fallback_id = generate_uuid();
        return expected<result, std::string>::ok(
            result::make_error(fallback_id, "invalid message: header is not a JSON object"));
    }

    if (!header.contains("id") || !header["id"].is_string()) {
        std::string fallback_id = generate_uuid();
        return expected<result, std::string>::ok(
            result::make_error(fallback_id, "invalid message: missing or invalid 'id' field"));
    }

    std::string id = header["id"].get<std::string>();

    // type discrimination order: error > value > effect
    if (header.contains("error")) {
        if (!header["error"].is_string()) {
            return expected<result, std::string>::ok(
                result::make_error(id, "invalid message: 'error' field is not a string"));
        }
        std::string error_msg = header["error"].get<std::string>();
        return expected<result, std::string>::ok(result::make_error(id, std::move(error_msg)));
    }

    if (header.contains("value")) {
        json value = header["value"];

        bool is_final = true;
        if (header.contains("final")) {
            if (header["final"].is_boolean()) {
                is_final = header["final"].get<bool>();
            }
        }

        int declared_bins = 0;
        if (header.contains("binary_frames")) {
            if (!header["binary_frames"].is_number_integer()) {
                return expected<result, std::string>::ok(
                    result::make_error(id, "invalid message: 'binary_frames' is not an integer"));
            }
            declared_bins = header["binary_frames"].get<int>();
            if (declared_bins < 0) {
                return expected<result, std::string>::ok(
                    result::make_error(id, "invalid message: 'binary_frames' is negative"));
            }
        }

        if (declared_bins > 8) {
            return expected<result, std::string>::ok(
                result::make_error(id, "invalid message: 'binary_frames' exceeds maximum of 8"));
        }

        if (static_cast<int>(bins.size()) != declared_bins) {
            return expected<result, std::string>::ok(
                result::make_error(
                    id, "invalid message: binary_frames count (" +
                            std::to_string(declared_bins) +
                            ") does not match actual frame count (" +
                            std::to_string(bins.size()) + ")"));
        }

        std::vector<std::vector<std::byte>> binary_data;
        for (auto &bin : bins) {
            binary_data.push_back(zmq_msg_to_bytes(bin));
        }

        return expected<result, std::string>::ok(
            result::make_resume(id, std::move(value), std::move(binary_data), is_final));
    }

    if (header.contains("effect")) {
        // perform message received on client side — unexpected
        return expected<result, std::string>::ok(
            result::make_error(id, "invalid message: received 'perform' on client side"));
    }

    return expected<result, std::string>::ok(
        result::make_error(id, "invalid message: unknown message type"));
}

}
