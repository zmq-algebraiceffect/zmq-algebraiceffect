#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "doctest.h"

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include "zmqae/detail/message.hpp"
#include "zmqae/types.hpp"
#include "zmqae/uuid.hpp"

namespace {

// Safe JSON object construction — avoids initializer_list ambiguity on GCC
zmqae::json mk_obj(std::initializer_list<std::pair<const char *, zmqae::json>> pairs) {
    zmqae::json j;
    for (const auto &p : pairs) {
        j[p.first] = p.second;
    }
    return j;
}

zmq::message_t str_to_msg(const std::string &s) {
    return zmq::message_t{s.data(), s.size()};
}

std::string msg_to_str(const zmq::message_t &msg) {
    return std::string{static_cast<const char *>(msg.data()), msg.size()};
}

std::vector<std::byte> to_bytes(const std::string &s) {
    auto *data = reinterpret_cast<const std::byte *>(s.data());
    return std::vector<std::byte>{data, data + s.size()};
}

} // namespace

// ─── TC-1.1.x: perform required field validation ───────────────────────

TEST_CASE("TC-1.1.1: perform with all required fields accepted") {
    zmqae::perform_message msg;
    msg.id = zmqae::generate_uuid();
    msg.effect = "TestEffect";
    msg.payload = mk_obj({{"key", "value"}});

    auto frames = zmqae::detail::serialize_perform(msg);
    zmq::message_t body = std::move(frames[0]);
    std::vector<zmq::message_t> bins;
    for (size_t i = 1; i < frames.size(); ++i) {
        bins.push_back(std::move(frames[i]));
    }

    auto result = zmqae::detail::parse_perform(body, bins);
    CHECK(result.has_value());
    CHECK(result->id == msg.id);
    CHECK(result->effect == msg.effect);
    CHECK(result->payload == msg.payload);
    CHECK(result->binary_frames == 0);
}

TEST_CASE("TC-1.1.2: perform missing id returns error") {
    zmqae::json header = mk_obj({{"effect", "Test"}, {"payload", "data"}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_perform(body, bins);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().find("id") != std::string::npos);
}

TEST_CASE("TC-1.1.3: perform missing effect returns error") {
    zmqae::json header = mk_obj({{"id", "some-uuid"}, {"payload", "data"}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_perform(body, bins);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().find("effect") != std::string::npos);
}

TEST_CASE("TC-1.1.4: perform missing payload returns error") {
    zmqae::json header = mk_obj({{"id", "some-uuid"}, {"effect", "Test"}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_perform(body, bins);
    CHECK_FALSE(result.has_value());
}

TEST_CASE("TC-1.1.5: perform with null payload accepted") {
    zmqae::json header = mk_obj({{"id", zmqae::generate_uuid()},
                          {"effect", "Test"},
                          {"payload", nullptr}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_perform(body, bins);
    REQUIRE(result.has_value());
    CHECK(result->payload.is_null());
}

// ─── TC-1.2.x: resume required field validation ────────────────────────

TEST_CASE("TC-1.2.1: resume with all required fields accepted") {
    zmqae::json header = mk_obj({{"id", zmqae::generate_uuid()}, {"value", 42}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_incoming_message(body, bins);
    CHECK(result.has_value());
    CHECK(result->is_ok());
    CHECK(result->value() == 42);
}

TEST_CASE("TC-1.2.2: resume with null value accepted") {
    zmqae::json header = mk_obj({{"id", zmqae::generate_uuid()}, {"value", nullptr}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_incoming_message(body, bins);
    CHECK(result.has_value());
    CHECK(result->is_ok());
    CHECK(result->value().is_null());
}

// ─── TC-1.3.x: error required field validation ─────────────────────────

TEST_CASE("TC-1.3.1: error with all required fields accepted") {
    zmqae::json header = mk_obj({{"id", zmqae::generate_uuid()},
                          {"error", "something went wrong"}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_incoming_message(body, bins);
    CHECK(result.has_value());
    CHECK(result->is_error());
    CHECK(result->error() == "something went wrong");
}

TEST_CASE("TC-1.3.2: error message ignores binary_frames") {
    zmqae::json header = mk_obj({{"id", zmqae::generate_uuid()},
                          {"error", "err"},
                          {"binary_frames", 2}});
    auto body = str_to_msg(header.dump());

    std::vector<zmq::message_t> bins;
    bins.push_back(str_to_msg("audio"));
    bins.push_back(str_to_msg("video"));

    auto result = zmqae::detail::parse_incoming_message(body, bins);
    CHECK(result.has_value());
    CHECK(result->is_error());
    CHECK(result->binary_count() == 0);
}

// ─── TC-3.x: type discrimination ──────────────────────────────────────

TEST_CASE("TC-3.1: error key discriminated as error") {
    zmqae::json header = mk_obj({{"id", "uuid-001"}, {"error", "fail"}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_incoming_message(body, bins);
    CHECK(result.has_value());
    CHECK(result->is_error());
}

TEST_CASE("TC-3.2: value key only discriminated as resume") {
    zmqae::json header = mk_obj({{"id", "uuid-002"}, {"value", "ok"}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_incoming_message(body, bins);
    CHECK(result.has_value());
    CHECK(result->is_ok());
}

TEST_CASE("TC-3.3: effect key discriminated as perform on client side") {
    zmqae::json header = mk_obj({{"id", "uuid-003"},
                          {"effect", "DoSomething"},
                          {"payload", nullptr}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_incoming_message(body, bins);
    CHECK(result.has_value());
    CHECK(result->is_error());
}

TEST_CASE("TC-3.4: no discriminating keys returns error") {
    zmqae::json header = mk_obj({{"id", "uuid-004"}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_incoming_message(body, bins);
    CHECK(result.has_value());
    CHECK(result->is_error());
}

// ─── TC-4.x: binary_frames handling ───────────────────────────────────

TEST_CASE("TC-4.1.1: binary_frames omitted defaults to 0") {
    zmqae::json header = mk_obj({{"id", zmqae::generate_uuid()},
                          {"effect", "Test"},
                          {"payload", {}}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_perform(body, bins);
    REQUIRE(result.has_value());
    CHECK(result->binary_frames == 0);
}

TEST_CASE("TC-4.2.1: binary_frames 1 with 1 frame succeeds") {
    zmqae::json header = mk_obj({{"id", zmqae::generate_uuid()},
                          {"effect", "Test"},
                          {"payload", {}},
                          {"binary_frames", 1}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;
    bins.push_back(str_to_msg("binary_data"));

    auto result = zmqae::detail::parse_perform(body, bins);
    REQUIRE(result.has_value());
    CHECK(result->binary_frames == 1);
    CHECK(result->binary_data.size() == 1);
}

TEST_CASE("TC-4.2.2: binary_frames 1 with 0 frames returns error") {
    zmqae::json header = mk_obj({{"id", zmqae::generate_uuid()},
                          {"effect", "Test"},
                          {"payload", {}},
                          {"binary_frames", 1}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_perform(body, bins);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().find("does not match") != std::string::npos);
}

TEST_CASE("TC-4.2.3: binary_frames 0 with 1 frame returns error") {
    zmqae::json header = mk_obj({{"id", zmqae::generate_uuid()},
                          {"effect", "Test"},
                          {"payload", {}},
                          {"binary_frames", 0}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;
    bins.push_back(str_to_msg("extra"));

    auto result = zmqae::detail::parse_perform(body, bins);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().find("does not match") != std::string::npos);
}

TEST_CASE("TC-4.3.1: multiple binary frames preserve order") {
    std::vector<std::byte> audio = to_bytes("AUDIO_DATA");
    std::vector<std::byte> video = to_bytes("VIDEO_DATA");

    zmqae::json header = mk_obj({{"id", zmqae::generate_uuid()},
                          {"effect", "Test"},
                          {"payload", {}},
                          {"binary_frames", 0}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;
    bins.push_back(str_to_msg("extra"));

    auto result = zmqae::detail::parse_perform(body, bins);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().find("does not match") != std::string::npos);
}

TEST_CASE("TC-4.3.1: multiple binary frames preserve order") {
    std::vector<std::byte> audio = to_bytes("AUDIO_DATA");
    std::vector<std::byte> video = to_bytes("VIDEO_DATA");

    zmqae::json header = mk_obj({{"id", zmqae::generate_uuid()},
                          {"effect", "Test"},
                          {"payload", {}},
                          {"binary_frames", 2}});
    auto body = str_to_msg(header.dump());

    std::vector<zmq::message_t> bins;
    bins.emplace_back(audio.data(), audio.size());
    bins.emplace_back(video.data(), video.size());

    auto result = zmqae::detail::parse_perform(body, bins);
    REQUIRE(result.has_value());
    REQUIRE(result->binary_data.size() == 2);

    auto &bin0 = result->binary_data[0];
    auto &bin1 = result->binary_data[1];
    CHECK(std::string{reinterpret_cast<const char *>(bin0.data()), bin0.size()} ==
          "AUDIO_DATA");
    CHECK(std::string{reinterpret_cast<const char *>(bin1.data()), bin1.size()} ==
          "VIDEO_DATA");
}

// ─── MSG-SER-x: serialization round-trip and edge cases ────────────────

TEST_CASE("MSG-SER-01: perform message round-trip") {
    zmqae::perform_message orig;
    orig.id = zmqae::generate_uuid();
    orig.effect = "RoundTrip";
    orig.payload = mk_obj({{"nested", true}, {"count", 7}});
    orig.binary_frames = 0;

    auto frames = zmqae::detail::serialize_perform(orig);
    zmq::message_t body = std::move(frames[0]);
    std::vector<zmq::message_t> bins;
    for (size_t i = 1; i < frames.size(); ++i) {
        bins.push_back(std::move(frames[i]));
    }

    auto parsed = zmqae::detail::parse_perform(body, bins);
    REQUIRE(parsed.has_value());
    CHECK(parsed->id == orig.id);
    CHECK(parsed->effect == orig.effect);
    CHECK(parsed->payload == orig.payload);
    CHECK(parsed->binary_frames == orig.binary_frames);
}

TEST_CASE("MSG-SER-02: resume message round-trip") {
    zmqae::resume_message orig;
    orig.id = zmqae::generate_uuid();
    orig.value = mk_obj({{"result", 3.14}});
    orig.binary_frames = 0;

    auto frames = zmqae::detail::serialize_resume(orig);
    zmq::message_t body = std::move(frames[0]);
    std::vector<zmq::message_t> bins;
    for (size_t i = 1; i < frames.size(); ++i) {
        bins.push_back(std::move(frames[i]));
    }

    auto parsed = zmqae::detail::parse_incoming_message(body, bins);
    REQUIRE(parsed.has_value());
    CHECK(parsed->is_ok());
    CHECK(parsed->value() == orig.value);
}

TEST_CASE("MSG-SER-03: binary_frames omitted not in JSON dump") {
    zmqae::perform_message msg;
    msg.id = zmqae::generate_uuid();
    msg.effect = "NoBin";
    msg.payload = {};
    msg.binary_frames = 0;

    auto frames = zmqae::detail::serialize_perform(msg);
    std::string body_str = msg_to_str(frames[0]);
    auto header = zmqae::json::parse(body_str);

    CHECK_FALSE(header.contains("binary_frames"));
}

TEST_CASE("MSG-SER-04: binary_frames > 0 included in JSON dump") {
    zmqae::perform_message msg;
    msg.id = zmqae::generate_uuid();
    msg.effect = "WithBin";
    msg.payload = {};
    msg.binary_frames = 1;
    msg.binary_data.push_back(to_bytes("data"));

    auto frames = zmqae::detail::serialize_perform(msg);
    std::string body_str = msg_to_str(frames[0]);
    auto header = zmqae::json::parse(body_str);

    CHECK(header.contains("binary_frames"));
    CHECK(header["binary_frames"].get<int>() == 1);
}

TEST_CASE("MSG-SER-05: invalid JSON handled gracefully") {
    std::string bad_json = "{not valid json!!!";
    auto body = str_to_msg(bad_json);
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_incoming_message(body, bins);
    CHECK(result.has_value());
    CHECK(result->is_error());
}

TEST_CASE("MSG-SER-06: non-UUID id string accepted by parse_perform") {
    zmqae::json header = mk_obj({{"id", "not-a-uuid"},
                          {"effect", "Test"},
                          {"payload", {}}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_perform(body, bins);
    CHECK(result.has_value());
    CHECK(result->id == "not-a-uuid");
}

TEST_CASE("MSG-SER-07: negative binary_frames returns error") {
    zmqae::json header = mk_obj({{"id", zmqae::generate_uuid()},
                          {"effect", "Test"},
                          {"payload", {}},
                          {"binary_frames", -1}});
    auto body = str_to_msg(header.dump());
    std::vector<zmq::message_t> bins;

    auto result = zmqae::detail::parse_perform(body, bins);
    CHECK_FALSE(result.has_value());
    CHECK(result.error().find("negative") != std::string::npos);
}

TEST_CASE("MSG-SER-08: error message serialized as single frame") {
    auto frames = zmqae::detail::serialize_error("uuid-err", "test error");
    CHECK(frames.size() == 1);

    std::string body_str = msg_to_str(frames[0]);
    auto header = zmqae::json::parse(body_str);
    CHECK(header["id"].get<std::string>() == "uuid-err");
    CHECK(header["error"].get<std::string>() == "test error");
    CHECK_FALSE(header.contains("binary_frames"));
}
