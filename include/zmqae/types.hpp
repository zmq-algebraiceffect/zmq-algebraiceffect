#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace zmqae {

using json = nlohmann::json;

struct perform_message {
    std::string id;
    std::string effect;
    json payload;
    int binary_frames{0};
    std::vector<std::vector<std::byte>> binary_data;
};

struct resume_message {
    std::string id;
    json value;
    int binary_frames{0};
    std::vector<std::vector<std::byte>> binary_data;
};

class result {
    std::string id_;
    bool is_error_{false};
    json value_;
    std::string error_;
    std::vector<std::vector<std::byte>> binary_data_;

    result(std::string id, json value, std::vector<std::vector<std::byte>> bins)
        : id_{std::move(id)}
        , is_error_{false}
        , value_{std::move(value)}
        , binary_data_{std::move(bins)}
    {}

    result(std::string id, std::string error)
        : id_{std::move(id)}
        , is_error_{true}
        , error_{std::move(error)}
    {}

public:
    static result make_resume(std::string id, json value,
                              std::vector<std::vector<std::byte>> bins = {}) {
        return result{std::move(id), std::move(value), std::move(bins)};
    }

    static result make_error(std::string id, std::string error) {
        return result{std::move(id), std::move(error)};
    }

    static result make_timeout(std::string id, std::string effect) {
        return result{std::move(id), "timeout: " + effect};
    }

    result(result &&) = default;
    result &operator=(result &&) = default;
    result(const result &) = default;
    result &operator=(const result &) = default;
    ~result() = default;

    bool is_ok() const { return !is_error_; }
    bool is_error() const { return is_error_; }
    const json &value() const { return value_; }
    const std::string &error() const { return error_; }
    const std::string &id() const { return id_; }
    int binary_count() const { return static_cast<int>(binary_data_.size()); }
    const std::vector<std::byte> &binary(int index) const { return binary_data_.at(index); }
};

using perform_callback = std::function<void(result)>;

class perform_context;

using handler_fn = std::function<void(std::shared_ptr<perform_context>)>;

namespace detail {

template <typename T, typename E>
class expected {
    std::variant<T, E> storage_;

    explicit expected(std::variant<T, E> storage) : storage_{std::move(storage)} {}

public:
    static expected ok(T value) {
        return expected{std::variant<T, E>{std::in_place_index<0>, std::move(value)}};
    }

    static expected err(E error) {
        return expected{std::variant<T, E>{std::in_place_index<1>, std::move(error)}};
    }

    bool has_value() const { return storage_.index() == 0; }
    explicit operator bool() const { return has_value(); }

    const T &value() const & { return std::get<0>(storage_); }
    T &value() & { return std::get<0>(storage_); }
    T &&value() && { return std::get<0>(std::move(storage_)); }

    const T &operator*() const & { return std::get<0>(storage_); }
    T &operator*() & { return std::get<0>(storage_); }
    T &&operator*() && { return std::get<0>(std::move(storage_)); }

    const T *operator->() const { return &std::get<0>(storage_); }
    T *operator->() { return &std::get<0>(storage_); }

    const E &error() const & { return std::get<1>(storage_); }
    E &error() & { return std::get<1>(storage_); }
};

}

}
