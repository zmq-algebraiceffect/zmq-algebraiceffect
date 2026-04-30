#pragma once

#include <zmq.hpp>

namespace zmqae::detail {

// Default ZMQ context — inline static, thread-safe init (C++17).
// WARNING: shared library boundaries get separate instances.
//          Pass an explicit context if sharing across .dylib/.so.
inline zmq::context_t &get_default_context() {
    static zmq::context_t ctx{1};
    return ctx;
}

} // namespace zmqae::detail
