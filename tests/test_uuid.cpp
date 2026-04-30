#define DOCTEST_CONFIG_NO_MULTI_LANE_ATOMICS

#include "doctest.h"

#include <regex>
#include <string>
#include <unordered_set>

#include "zmqae/uuid.hpp"

TEST_CASE("TC-2.1.1: UUID v4 lowercase hex format") {
    std::string uuid = zmqae::generate_uuid();
    std::regex pattern{
        R"(^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$)"};
    CHECK(std::regex_match(uuid, pattern));
}

TEST_CASE("TC-2.1.2: uppercase UUID characters") {
    std::string uuid = zmqae::generate_uuid();
    std::regex upper_pattern{"[A-F]"};
    CHECK_FALSE(std::regex_search(uuid, upper_pattern));
}

TEST_CASE("TC-2.1.3: invalid UUID format rejected") {
    std::string bad_uuid = "not-a-uuid";
    std::regex pattern{
        R"(^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$)"};
    CHECK_FALSE(std::regex_match(bad_uuid, pattern));
}

TEST_CASE("UUID-01: generated UUID is v4 format") {
    std::string uuid = zmqae::generate_uuid();
    REQUIRE(uuid.size() == 36);

    char version_char = uuid[14];
    CHECK(version_char == '4');

    char variant_char = uuid[19];
    bool valid_variant = (variant_char == '8' || variant_char == '9' ||
                          variant_char == 'a' || variant_char == 'b');
    CHECK(valid_variant);
}

TEST_CASE("UUID-02: 1000 consecutive UUIDs are unique") {
    std::unordered_set<std::string> seen;
    seen.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        std::string uuid = zmqae::generate_uuid();
        CHECK(seen.insert(uuid).second);
    }
    CHECK(seen.size() == 1000);
}

TEST_CASE("UUID-03: generated UUID is lowercase hex") {
    for (int i = 0; i < 50; ++i) {
        std::string uuid = zmqae::generate_uuid();
        for (char c : uuid) {
            if (c == '-') {
                continue;
            }
            bool is_lower_hex = (c >= '0' && c <= 'f');
            CHECK(is_lower_hex);
        }
    }
}
