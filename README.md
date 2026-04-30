# zmq-algebraiceffect

C++17 header-only library that implements [Algebraic Effects](https://github.com/zmq-algebraiceffect/protocol) semantics over ZeroMQ.

```cpp
// Client (performer) side
zmqae::client client{"tcp://localhost:5555"};

auto result = client.perform("Transcribe", {{"audio", "base64..."}});
// → handler processes it asynchronously
// → client receives the resume value
```

```cpp
// Router (handler) side
zmqae::router router{"tcp://*:5555"};

router.on("Transcribe", [](std::shared_ptr<zmqae::perform_context> ctx) {
    auto audio = ctx->payload()["audio"].get<std::string>();
    ctx->resume({{"text", transcribe(audio)}});
});

while (router.poll(std::chrono::milliseconds{100})) {}
```

## Overview

This library implements the [zmq-algebraiceffect protocol](https://github.com/zmq-algebraiceffect/protocol) (v0.0.1), which brings Algebraic Effects-style `perform`/`resume` semantics to distributed systems via ZeroMQ.

Key features:

- **Header-only** — just `#include <zmqae/zmqae.hpp>`, no compilation needed
- **Async handlers** — handlers run on your main thread via `poll()`, can resume at any time
- **Binary frames** — zero-copy binary data transfer alongside JSON payloads
- **C API** — `#include <zmqae/zmqae.h>` for C/MaxMSP/FFI integration
- **Cross-platform** — macOS, Windows, Linux

## Quickstart

### Prerequisites

- C++17 compiler (clang++ / g++ / MSVC)
- CMake 3.14+
- Git (for submodule fetch)

### Build

```bash
git clone --recurse-submodules https://github.com/zmq-algebraiceffect/zmq-algebraiceffect.git
cd zmq-algebraiceffect

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Run Tests

```bash
./build/tests/zmqae_tests
```

### Run Examples

Terminal 1 — start the handler:

```bash
./build/examples/handler tcp://*:5555
```

Terminal 2 — send actions:

```bash
./build/examples/performer tcp://localhost:5555
```

## Project Structure

```
include/zmqae/
├── zmqae.hpp              # Main entry (includes everything)
├── client.hpp             # DEALER client — perform(), poll()
├── router.hpp             # ROUTER router — on(), poll()
├── perform_context.hpp    # Async resume context (shared_ptr)
├── types.hpp              # Value types, Result, callbacks
├── uuid.hpp               # UUID v4 generation
├── zmqae.h                # C API (extern "C")
└── detail/
    ├── context.hpp        # Inline static ZMQ context
    ├── message.hpp        # Serialize/parse protocol messages
    └── thread_queue.hpp   # MPSC queue (ZMQ thread → main thread)

src/
└── c_api.cpp              # C API implementation

tests/                     # 103 doctest test cases
examples/                  # CUI performer + handler
```

## API

### C++ API

```cpp
#include <zmqae/zmqae.hpp>
```

#### Client

```cpp
zmqae::client client{"tcp://localhost:5555"};

// Perform an effect (blocks until resume or error)
auto result = client.perform("Echo", {{"msg", "hello"}});

// Perform with timeout
auto result = client.perform("SlowOp", payload, std::chrono::seconds{5});

// Perform with binary frames
std::vector<std::vector<std::byte>> bins = {...};
auto result = client.perform_binary("Transcribe", payload, bins);

// Poll for async results (non-blocking drain)
client.poll();
```

#### Router

```cpp
zmqae::router router{"tcp://*:5555"};

// Register handler
router.on("Echo", [](std::shared_ptr<zmqae::perform_context> ctx) {
    ctx->resume(ctx->payload());
});

// Unregister
router.off("Echo");

// Poll and dispatch handlers (call from main thread)
while (router.poll(std::chrono::milliseconds{100})) {}

// Cleanup
router.close();
```

#### Perform Context

```cpp
// Inside a handler:
ctx->id();            // UUID string
ctx->effect();        // Effect name string
ctx->payload();       // nlohmann::json
ctx->binary_count();  // Number of binary frames
ctx->binary(0);       // Access binary frame by index

ctx->resume(value);                    // Resume with JSON value
ctx->resume(value, binary_frames);     // Resume with binary data
ctx->resume();                         // Resume with null

ctx->error("something went wrong");    // Resume with error
```

### C API

```cpp
#include <zmqae/zmqae.h>
```

See [`include/zmqae/zmqae.h`](include/zmqae/zmqae.h) for the full C API declaration.

## Dependencies

All dependencies are included as git submodules:

| Library | License | Purpose |
|---|---|---|
| [libzmq](https://github.com/zeromq/libzmq) | MPL-2.0 | ZeroMQ core |
| [cppzmq](https://github.com/zeromq/cppzmq) | MIT | C++ RAII wrapper for ZMQ |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON handling |
| [spdlog](https://github.com/gabime/spdlog) | MIT | Logging |
| [doctest](https://github.com/doctest/doctest) | MIT | Testing |

## Configuration

- **Default port**: 5555 (configurable via endpoint string)
- **Socket types**: DEALER (client) / ROUTER (server) — REQ/REP is not used
- **Threading model**: ZMQ I/O runs on a background thread; handlers execute on the main thread during `poll()`

## Design Decisions

See [DESIGN.md](DESIGN.md) for the full design rationale, API signatures, and threading model.

## Protocol Spec

See [PROTOCOL.md](https://github.com/zmq-algebraiceffect/protocol/blob/main/PROTOCOL.md) for the wire protocol specification.

## License

MIT License. See [LICENSE](LICENSE).
