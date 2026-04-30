# DESIGN.md — zmq-algebraiceffect C++ Library

> C++17 header-only library implementing the zmq-algebraiceffect protocol.
>
> **Review history**: Codex (gpt-5.5) + Oracle (glm-5.1) review反映済み。

---

## Architecture Overview

```
┌─────────────────────────────────────────────────┐
│  zmqae/  (namespace zmqae)                      │
│                                                  │
│  ┌──────────────┐  ┌──────────────┐             │
│  │  client       │  │  router      │             │
│  │  (DEALER)     │  │  (ROUTER)    │             │
│  │               │  │              │             │
│  │  perform()    │  │  on()        │             │
│  │  poll()       │  │  poll()      │             │
│  └──────┬───────┘  └──────┬───────┘             │
│         │                  │                      │
│  ┌──────┴──────────────────┴───────┐             │
│  │  detail/                        │             │
│  │  ├─ context (ZMQ context管理)    │             │
│  │  ├─ message (JSON+binary frame)  │             │
│  │  ├─ thread_queue (MPSC queue)    │             │
│  │  └─ uuid (UUID v4生成)           │             │
│  └─────────────────────────────────┘             │
│                                                  │
│  C API (extern "C")                              │
│  ┌─────────────────────────────────┐             │
│  │  zmqae_* 関数群                  │             │
│  │  opaque handle → C++ object     │             │
│  └─────────────────────────────────┘             │
└─────────────────────────────────────────────────┘
```

---

## Design Decisions

### D1: 非同期resume (Handler Execution Model)

Router側のハンドラは `std::shared_ptr<perform_context>` を受け取る。
ハンドラは `ctx->resume(value)` を任意のタイミングで呼び出すことができる。

**重要**: ハンドラは `poll()` 内でメインスレッド上で実行される（PROTOCOL.md Model A準拠）。
ZMQスレッドはsocket I/Oとqueue操作のみを行い、ハンドラの実行やcallbackの呼び出しは一切行わない。

```cpp
// 同期ハンドラ — poll()内でメインスレッド実行
router.on("Ping", [](std::shared_ptr<zmqae::perform_context> ctx) {
    ctx->resume(ctx->payload());  // 即座にresume
});

// 非同期ハンドラ — shared_ptrをキャプチャして安全に別スレッドでresume
router.on("Transcribe", [](std::shared_ptr<zmqae::perform_context> ctx) {
    std::thread([ctx]() {
        auto result = heavy_transcription(ctx->payload());
        ctx->resume(result);  // 任意タイミングでresume
    }).detach();
});
```

**内部フロー**:
1. ZMQスレッドがROUTERでメッセージを受信
2. identity frameを保存し、JSON headerをパース
3. `shared_ptr<perform_context>` を生成して recv_queue に積む
4. ZMQスレッドは即座に次のメッセージ受信へ
5. メインスレッドの `poll()` が recv_queue をdrain
6. effect名でハンドラを検索（exact match）
7. ハンドラを実行（try/catchで例外をcatch → `ctx->error()` に変換）
8. ハンドラ内で `ctx->resume(value)` が呼ばれると:
   - resume JSONを生成
   - send_queue（MPSC）に積む
   - ZMQスレッドの次回send処理でROUTERから送信

**perform_context の所有権**:
- `shared_ptr<perform_context>` で管理
- 同期ハンドラ: refcount 1→2→1→0（オーバーヘッド最小）
- 非同期ハンドラ: shared_ptrをclosureにmove → contextは生き続ける
- `resume()` / `error()` は一回性: 内部の `std::atomic<bool>` で二重呼び出しを防止
- 未resumeのcontextが破棄される場合: デストラクタで `"handler error: context dropped without resume"` を自動送信

`perform_context` は以下を保持:
- performの `id` (UUID)
- `identity frame` (ROUTER返信用、`zmq::message_t` として所有)
- `effect` 名
- `payload` (nlohmann::json)
- binary frames (存在する場合)
- send_queue の参照（MPSC）
- `std::atomic<bool> resumed_` （一回性保証）

### D2: C++ が基盤、C API は薄いwrapper

内部実装はC++17 + cppzmq。C APIは `extern "C"` でopaque handleを公開。

```cpp
// C++ API
namespace zmqae {
    class client { /* ... */ };
    class router { /* ... */ };
}

// C API
extern "C" {
    typedef struct zmqae_client_s zmqae_client_t;
    zmqae_client_t *zmqae_client_new(const char *endpoint);
    void zmqae_client_destroy(zmqae_client_t *client);
    // ...
}
```

C APIの制約:
- 全関数は `noexcept` — C++例外は内部でcatchしてエラーコードに変換
- 全 `extern "C"` 関数の実装は try/catch でwrap
- opaque handle → 内部のC++オブジェクトへのポインタ
- callback は `void (*callback)(void *user_data)` パターン
- エラー報告は戻り値（int: 0=success, 負数=error）
- コンストラクタ失敗時はNULLを返す + `zmqae_last_error()` で詳細取得

### D3: オプションタイムアウト付きperform

```cpp
// タイムアウトなし（デフォルト、ユーザー責任）
client.perform("AskLLM", payload, [](zmqae::result res) {
    // ...
});

// タイムアウト付き（ミリ秒）
client.perform("AskLLM", payload, callback, 5000);
```

タイムアウト時は callback に error result が渡される:
```cpp
if(res.is_error()) {
    // res.error() == "timeout: AskLLM"
}
```

内部では `std::chrono::steady_clock` でdeadlineを管理。
**タイムアウトチェックは `poll()` 内で実行**（メインスレッド）。
timeout結果はrecv_queueに積まれ、poll()でcallbackが呼ばれる。
ZMQスレッドはタイムアウトチェックを行わない。

pending mapはメインスレッドのみがアクセス:
- `perform()` が追加（メインスレッド）
- `poll()` がresume受信で削除（メインスレッド）
- `poll()` がtimeout checkで削除（メインスレッド）

### D4: poll() は non-blocking drain

```cpp
// oFのupdate()内で呼ぶ想定
void ofApp::update() {
    client.poll();   // timeout=0, 全メッセージをdrain
    router.poll();   // 同上
}
```

poll()の内部実装:
1. recv_queueをdrain → perform_context または result を取得
2. client: pending mapからIDでcallbackを検索 → 実行
3. router: effect名でhandlerを検索 → 実行
4. timeout check: 期限切れのpending entryを検索 → error callback → 削除

**注意**: ハンドラが16ms以上かかるとdraw()をブロックする。
重い処理は別スレッドにdispatchすること。サンプルコードで明示する。

### D5: ZMQ Context — デフォルト + ユーザー提供

```cpp
namespace zmqae::detail {
    // 便利なデフォルト（単一バイナリ用）
    inline zmq::context_t &get_default_context() {
        static zmq::context_t ctx{1};
        return ctx;
    }
}
```

コンストラクタで明示的に提供も可能:
```cpp
zmqae::client client(endpoint);                    // デフォルトcontext使用
zmqae::client client(endpoint, custom_context);   // ユーザー提供context
```

**inline static contextの注意事項**:
- C++17のthread-safe initializationは保証される
- **shared library境界をまたぐ場合は別インスタンスになる**:
  同一プロセス内で異なる `.dylib`/`.so` からincludeすると別contextが生成される。
  `inproc://` がlibrary間で動作しない可能性がある。
  この場合は明示的にcontextを共有すること。
- static破棄順序に注意: client/routerをstatic変数にしないこと

### D6: ライフサイクル管理

```
construct()  → start ZMQ thread → socket connect/bind
poll()       → check running_ → drain recv_queue → call handlers (main thread)
perform()    → check running_ → push to send_queue (main thread)
close()      → running_ = false → signal ZMQ thread → join → close socket
~dtor()      → close() if not already closed
```

- `std::atomic<bool> running_{true}` でshutdown制御
- `close()` はブロッキング: ZMQスレッドのjoinを待つ
- ZMQスレッドの終了: running_フラグをチェック → send_queueをdrain → socket close → exit
- `poll()` / `perform()` は `running_` をチェック、falseなら早期return

---

## Public C++ API

### Types

```cpp
namespace zmqae {
    // --- 値型 ---
    using json = nlohmann::json;

    struct perform_message {
        std::string id;          // UUID v4
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

    // --- Result型 (resume or error) ---
    class result {
    public:
        bool is_ok() const;
        bool is_error() const;
        const json &value() const;         // is_ok()時のみ
        const std::string &error() const;  // is_error()時のみ
        const std::string &id() const;
        int binary_count() const;          // binary frame数
        const std::vector<std::byte> &binary(int index) const;
    };

    // --- Callback型 ---
    using perform_callback = std::function<void(result)>;

    // --- perform_context (Router側ハンドラ内で使用) ---
    // shared_ptrで渡される。非同期resumeに対応。
    class perform_context {
    public:
        const std::string &id() const;
        const std::string &effect() const;
        const json &payload() const;
        const std::vector<std::byte> &binary(int index) const;
        int binary_count() const;

        // 一回性。二回目以降の呼び出しは何もしない（log warning）。
        void resume(const json &value);
        void resume(const json &value,
                    const std::vector<std::vector<std::byte>> &bins);
        void error(const std::string &message);

        bool is_resumed() const;  // 既にresume/error済みか
    };

    // --- Handler型 ---
    using handler_fn = std::function<void(std::shared_ptr<perform_context>)>;

    // --- UUID生成 ---
    std::string generate_uuid();
}
```

### client (DEALER)

```cpp
namespace zmqae {
    class client {
    public:
        // endpointにconnect（DEALER）
        explicit client(const std::string &endpoint);
        client(const std::string &endpoint, zmq::context_t &ctx);
        ~client();

        client(const client &) = delete;
        client &operator=(const client &) = delete;
        client(client &&) noexcept;
        client &operator=(client &&) noexcept;

        // --- perform ---
        void perform(const std::string &effect,
                     const json &payload,
                     perform_callback callback);

        void perform(const std::string &effect,
                     const json &payload,
                     perform_callback callback,
                     int timeout_ms);

        void perform(const std::string &effect,
                     const json &payload,
                     const std::vector<std::vector<std::byte>> &bins,
                     perform_callback callback);

        void perform(const std::string &effect,
                     const json &payload,
                     const std::vector<std::vector<std::byte>> &bins,
                     perform_callback callback,
                     int timeout_ms);

        // --- poll ---
        void poll();  // non-blocking drain, recv_queue → callback

        // --- lifecycle ---
        bool is_open() const;  // close()済みでなければtrue
        void close();          // blocking: ZMQ thread join
    };
}
```

### router (ROUTER)

```cpp
namespace zmqae {
    class router {
    public:
        // endpointにbind（ROUTER）
        explicit router(const std::string &endpoint);
        router(const std::string &endpoint, zmq::context_t &ctx);
        ~router();

        router(const router &) = delete;
        router &operator=(const router &) = delete;
        router(router &&) noexcept;
        router &operator=(router &&) noexcept;

        // --- handler registration ---
        // effect名でexact match。
        // 登録はpoll()開始前（construct後、最初のpoll()前）を推奨。
        // poll()中の登録変更はスレッドセーフだが、推奨しない。
        void on(const std::string &effect, handler_fn handler);
        void off(const std::string &effect);
        void clear_handlers();

        // --- default handler (unregistered effects) ---
        // 未設定時は "no handler for: <effect>" errorを自動返信
        void on_unregistered(handler_fn handler);

        // --- poll ---
        void poll();  // non-blocking drain, recv_queue → handler

        // --- lifecycle ---
        bool is_open() const;  // close()済みでなければtrue
        void close();          // blocking: ZMQ thread join
    };
}
```

---

## C API

```c
// zmqae.h
#ifdef __cplusplus
extern "C" {
#endif

/* --- Error codes --- */
#define ZMQAE_OK           0
#define ZMQAE_ERROR       -1
#define ZMQAE_TIMEOUT     -2
#define ZMQAE_INVALID     -3
#define ZMQAE_CLOSED      -4
#define ZMQAE_ALREADY     -5  /* 既にresume済み */

/* --- Opaque types --- */
typedef struct zmqae_client_s      zmqae_client_t;
typedef struct zmqae_router_s      zmqae_router_t;
typedef struct zmqae_perform_ctx_s zmqae_perform_ctx_t;

/* --- Error detail --- */
const char *zmqae_last_error(void);  /* 直近のエラー詳細（thread-local） */

/* --- Binary data --- */
typedef struct {
    const void *data;
    int size;
} zmqae_binary_t;

/* --- Client perform callback --- */
typedef void (*zmqae_perform_callback)(void *user_data,
                                        const char *id,
                                        const char *json_value,
                                        const char *error_message);

/* --- Router handler callback --- */
typedef void (*zmqae_handler_fn)(void *user_data, zmqae_perform_ctx_t *ctx);

/* --- Client --- */
zmqae_client_t *zmqae_client_new(const char *endpoint);
void zmqae_client_destroy(zmqae_client_t *client);

int zmqae_client_perform(zmqae_client_t *client,
                          const char *effect,
                          const char *json_payload,
                          zmqae_perform_callback callback,
                          void *user_data);

int zmqae_client_perform_timeout(zmqae_client_t *client,
                                  const char *effect,
                                  const char *json_payload,
                                  int timeout_ms,
                                  zmqae_perform_callback callback,
                                  void *user_data);

int zmqae_client_perform_binary(zmqae_client_t *client,
                                 const char *effect,
                                 const char *json_payload,
                                 const zmqae_binary_t *bins,
                                 int bin_count,
                                 int timeout_ms,  /* 0 = no timeout */
                                 zmqae_perform_callback callback,
                                 void *user_data);

int zmqae_client_poll(zmqae_client_t *client);

int zmqae_client_close(zmqae_client_t *client);

/* --- Router --- */
zmqae_router_t *zmqae_router_new(const char *endpoint);
void zmqae_router_destroy(zmqae_router_t *router);

int zmqae_router_on(zmqae_router_t *router,
                     const char *effect,
                     zmqae_handler_fn handler,
                     void *user_data);

int zmqae_router_off(zmqae_router_t *router, const char *effect);

int zmqae_router_poll(zmqae_router_t *router);

int zmqae_router_close(zmqae_router_t *router);

/* --- Perform context (from handler callback) --- */
const char *zmqae_ctx_get_id(zmqae_perform_ctx_t *ctx);
const char *zmqae_ctx_get_effect(zmqae_perform_ctx_t *ctx);
const char *zmqae_ctx_get_payload(zmqae_perform_ctx_t *ctx);
int zmqae_ctx_binary_count(zmqae_perform_ctx_t *ctx);
int zmqae_ctx_get_binary(zmqae_perform_ctx_t *ctx, int index,
                          const void **out_data, int *out_size);

/* Resume: 同期・非同期どちらでも呼び出し可能 */
int zmqae_ctx_resume(zmqae_perform_ctx_t *ctx, const char *json_value);
int zmqae_ctx_resume_binary(zmqae_perform_ctx_t *ctx, const char *json_value,
                             const zmqae_binary_t *bins, int bin_count);
int zmqae_ctx_error(zmqae_perform_ctx_t *ctx, const char *error_message);

/* contextの参照を解放（resume/error後またはhandler内で不要になった時） */
void zmqae_ctx_release(zmqae_perform_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
```

**C APIの設計原則**:
- `zmqae_perform_ctx_t` は内部で `shared_ptr<perform_context>` を保持
- identity frameはctx内部で管理 → `zmqae_ctx_resume()` は正しいDEALERにルーティング
- `zmqae_ctx_release()` でshared_ptrの参照を解放
- 全関数の実装は `try { ... } catch(...) { return ZMQAE_ERROR; }` でwrap
- `zmqae_last_error()` はthread-local storageに直近の例外メッセージを保持

---

## Internal Architecture

### Threading Model

**原則**: ZMQスレッドはsocket I/Oとqueue操作のみ。ハンドラ・callbackは全てメインスレッドの `poll()` 内で実行。

```
[Main Thread]                        [ZMQ Thread]
                                     ┌─ loop ─────────────────┐
  client.perform("Effect", cb)  ───→ │ send_queue.pop()        │
                                     │ DEALER.send(msg)        │
                                     │ DEALER.recv(resume)     │
                                     │ recv_queue.push(result) │
                                     │ timeout entriesは       │
                                     │   一切触らない          │
                                     └─────────────────────────┘
  client.poll()
    drain recv_queue
    check timeouts (main thread only)
    → callback(result)

                                     ┌─ loop ─────────────────┐
                                     │ send_queue.pop()        │
  router.poll()                 ←─── │ ROUTER.recv(msg)        │
    drain recv_queue                  │ recv_queue.push(ctx)   │
    → handler(shared_ptr<ctx>)        │                        │
    ctx->resume(value)           ───→ │ send_queue.pop()       │
                                      │ ROUTER.send(resume)    │
                                     └─────────────────────────┘
```

各node(client/router)は:
1. 専用ZMQスレッドを1つ所有
2. `send_queue`: any thread → ZMQ thread (**MPSC**, mutex-based)
3. `recv_queue`: ZMQ thread → main thread (SPSC, mutex-based)
4. `poll()` で recv_queue をdrain → callback/handler実行（メインスレッド）
5. `std::atomic<bool> running_` でshutdown制御

### Thread-Safe Queue

MPSC (Multi-Producer Single-Consumer) mutex-based queue:

```cpp
namespace zmqae::detail {
    template <typename T>
    class mpsc_queue {
        std::queue<T> queue_;
        std::mutex mutex_;
    public:
        void push(T item);           // any thread
        std::optional<T> pop();      // consumer thread
        std::vector<T> drain();      // consumer thread (poll()用)
        bool empty() const;          // consumer thread
    };
}
```

### Message Serialization

```cpp
namespace zmqae::detail {
    // perform → ZMQ multipart frames
    std::vector<zmq::message_t> serialize_perform(const perform_message &msg);

    // ZMQ multipart → message parsing (with validation)
    // 戻り値: 成功時はparsed message、失敗時はerror reason
    std::expected<perform_message, std::string>
        parse_perform(zmq::message_t &body, std::vector<zmq::message_t> &bins);

    std::expected<resume_message, std::string>
        parse_resume(zmq::message_t &body, std::vector<zmq::message_t> &bins);
}
```

バリデーション:
- JSON parse失敗 → error
- 必須フィールド欠落（`id`, `effect`, `payload` / `id`, `value` / `id`, `error`）→ error
- UUID形式不正 → error（またはlowercase正規化）
- `binary_frames` 負数 → error
- `binary_frames` 宣言数と実際のframe数不一致 → 破棄 + error返信
- errorメッセージにbinary_framesがある → 無視してframe 1のみ処理
- 不明キー → 無視（`additionalProperties: false` は実装側で強制しない）

### UUID v4 Generation

```cpp
namespace zmqae::detail {
    inline std::string generate_uuid() {
        static thread_local std::mt19937 gen{std::random_device{}()};
        // xxxxxxxx-xxxx-4xxx-[89ab]xx-xxxxxxxxxxxx
        // ...
    }
}
```

### Timeout Management

全てメインスレッドの `poll()` 内で実行:

```cpp
// poll()内
void client::poll() {
    if(!running_) return;

    // 1. recv_queueをdrain
    auto items = recv_queue_.drain();
    for(auto &item : items) {
        auto it = pending_.find(item.id);
        if(it != pending_.end()) {
            it->second.callback(std::move(item));
            pending_.erase(it);
        }
    }

    // 2. timeout check（メインスレッドのみがpending_にアクセス）
    auto now = std::chrono::steady_clock::now();
    for(auto it = pending_.begin(); it != pending_.end();) {
        if(it->second.has_timeout && now > it->second.deadline) {
            it->second.callback(result::make_timeout(it->first, it->second.effect));
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}
```

pending_entry:
```cpp
struct pending_entry {
    std::string id;
    std::string effect;         // timeout時のエラーメッセージ用
    perform_callback callback;
    std::chrono::steady_clock::time_point deadline;
    bool has_timeout{false};
};
```

### Handler Exception Safety

poll()内のhandler呼び出しはtry/catchでwrap:

```cpp
try {
    handler(ctx);
} catch(const std::exception &e) {
    if(!ctx->is_resumed()) {
        ctx->error(std::string("handler error: ") + e.what());
    }
} catch(...) {
    if(!ctx->is_resumed()) {
        ctx->error("handler error: unknown exception");
    }
}
```

---

## Directory Structure

```
zmq-algebraiceffect/
├── DESIGN.md
├── PLAN.md
├── AGENTS.md
├── CMakeLists.txt                    # ビルドシステム
├── LICENSE                           # MIT
│
├── include/
│   └── zmqae/
│       ├── zmqae.hpp                 # メインエントリ (全てをinclude)
│       ├── client.hpp                # client クラス
│       ├── router.hpp                # router クラス
│       ├── types.hpp                 # 値型 (message, result, json alias)
│       ├── perform_context.hpp       # perform_context クラス
│       ├── uuid.hpp                  # UUID v4 生成
│       ├── zmqae.h                   # C API ヘッダ
│       └── detail/
│           ├── context.hpp           # ZMQ context 管理
│           ├── message.hpp           # メッセージ serialize/parse
│           └── thread_queue.hpp      # MPSC queue
│
├── tests/
│   ├── CMakeLists.txt
│   ├── test_message.cpp              # メッセージ serialize/parse
│   ├── test_uuid.cpp                 # UUID 生成・形式
│   ├── test_client.cpp               # client lifecycle + perform
│   ├── test_router.cpp               # router lifecycle + handler
│   ├── test_integration.cpp          # client ↔ router 通信
│   └── test_c_api.cpp                # C API wrapper
│
├── examples/
│   ├── CMakeLists.txt
│   ├── performer.cpp                 # CUI: アクション送信側
│   └── handler.cpp                   # CUI: アクション受信/返信側
│
└── extern/
    ├── libzmq/                       # submodule
    ├── cppzmq/                       # submodule
    ├── json/                         # nlohmann/json submodule
    ├── spdlog/                       # submodule
    └── doctest/                      # submodule
```

---

## Dependency Summary

| 依存関係 | バージョン | ライセンス | 用途 | 組み込み方 |
|---|---|---|---|---|
| libzmq | 4.3.x | MPL-2.0 | ZMQ通信基盤 | submodule, CMakeでビルド |
| cppzmq | 4.10+ | MIT | C++ RAII wrapper | submodule, header-only |
| nlohmann/json | 3.x | MIT | JSON serialize/parse | submodule, header-only |
| spdlog | 1.x | MIT | logging | submodule, header-only |
| doctest | 2.x | MIT | testing | submodule, header-only |

---

## Platform Considerations

### macOS
- 全transport対応: tcp, ipc, inproc
- Xcode toolchain + clang

### Windows
- **ipc:// 非対応** (ZMQ 4.3.3未満、4.3.3+でも限定的)
- inproc:// はZMQ 4.2+で対応
- `tcp://` は正常動作
- `#ifdef _WIN32` でipc対応を条件コンパイル

### Linux
- 全transport対応: tcp, ipc, inproc
- gcc 9+ / clang 10+ でC++17対応

---

## CMake Structure

```cmake
cmake_minimum_required(VERSION 3.14)
project(zmq-algebraiceffect VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Header-only library
add_library(zmqae INTERFACE)
target_include_directories(zmqae INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Submodule dependencies
add_subdirectory(extern/libzmq)
add_subdirectory(extern/cppzmq)
target_include_directories(zmqae INTERFACE extern/json/single_include)
target_include_directories(zmqae INTERFACE extern/spdlog/include)
target_link_libraries(zmqae INTERFACE cppzmq libzmq)

# Tests
option(ZMQAE_BUILD_TESTS "Build tests" ON)
if(ZMQAE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
option(ZMQAE_BUILD_EXAMPLES "Build examples" ON)
if(ZMQAE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
```

---

## Compliance Test Coverage

`protocol/tests/compliance.md` の36テストケースとの対応:

| テストID | テスト内容 | 実装ファイル |
|---|---|---|
| TC-1.x (message format) | perform/resume/errorの必須フィールド | test_message.cpp |
| TC-2.x (message ID) | UUID形式・生成責務・対応付け | test_uuid.cpp, test_integration.cpp |
| TC-3.x (type discrimination) | error > value > effect の判別順序 | test_message.cpp |
| TC-4.x (binary frames) | binary_frames の省略・一致・順序 | test_message.cpp, test_integration.cpp |
| TC-5.x (transport) | DEALER/ROUTER・tcp/ipc/inproc | test_integration.cpp |
| TC-6.x (threading) | スレッド分離・poll()・イベントループ | test_integration.cpp |
| TC-7.x (error handling) | エラー形式・種類 | test_router.cpp |
| TC-8.x (identity frame) | ROUTER identity frame | test_integration.cpp |
| TC-9.x (naming) | PascalCase・名前空間 | test_router.cpp |
