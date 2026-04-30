# TESTCASES.md — zmq-algebraiceffect C++ Library Test Cases

> protocol/tests/compliance.md の36テストケース + 実装固有テストの対応表。
> 全テストは doctest で実装する。

---

## 1. Message Serialization / Parsing (test_message.cpp)

### protocol compliance

| Test ID | 内容 | 判定基準 |
|---|---|---|
| TC-1.1.1 | perform必須フィールド全てあり → 受理 | 例外なし |
| TC-1.1.2 | perform `id` 欠落 → エラー | parse失敗、error reasonに "id" 含む |
| TC-1.1.3 | perform `effect` 欠落 → エラー | parse失敗、error reasonに "effect" 含む |
| TC-1.1.4 | perform `payload` 欠落 → エラー | parse失敗 |
| TC-1.1.5 | perform `payload` が `null` → 受理 | payload == json(nullptr) |
| TC-1.2.1 | resume必須フィールド全てあり → 受理 | 例外なし |
| TC-1.2.2 | resume `value` が `null` → 受理 | value == json(nullptr) |
| TC-1.3.1 | error必須フィールド全てあり → 受理 | 例外なし |
| TC-1.3.2 | errorメッセージにbinary_frames → 無視 | frame 1のみ処理 |
| TC-3.1 | `error`キーあり → errorとして判別 | header.contains("error") == true |
| TC-3.2 | `value`キーのみ → resumeとして判別 | header.contains("value") && !header.contains("error") |
| TC-3.3 | `effect`キーのみ → performとして判別 | header.contains("effect") && !header.contains("value") && !header.contains("error") |
| TC-3.4 | いずれのキーもなし → 無効メッセージ | parse失敗 |
| TC-4.1.1 | `binary_frames`省略 → 0として扱う | binary_frames == 0 |
| TC-4.2.1 | `binary_frames:1` + 1frame → 正常 | binary_data.size() == 1 |
| TC-4.2.2 | `binary_frames:1` + 0frame → エラー | parse失敗、frame count mismatch |
| TC-4.2.3 | `binary_frames:0` + 1frame → エラー | parse失敗 |
| TC-4.3.1 | 複数binary frameの順序維持 | binary[0] == audio, binary[1] == video |

### implementation-specific

| Test ID | 内容 | 判定基準 |
|---|---|---|
| MSG-SER-01 | perform_message → JSON → ZMQ frames → round-trip | 元のmessageと一致 |
| MSG-SER-02 | resume_message → JSON → ZMQ frames → round-trip | 元のmessageと一致 |
| MSG-SER-03 | binary_frames省略時はJSONに含めない | dump結果に "binary_frames" 含まない |
| MSG-SER-04 | binary_frames > 0 時はJSONに含める | dump結果に "binary_frames" 含む |
| MSG-SER-05 | 不正JSON（parse error）→ エラー | expected<_, string> の unexpected |
| MSG-SER-06 | UUID形式不正 → エラー | expected<_, string> の unexpected |
| MSG-SER-07 | binary_frames負数 → エラー | expected<_, string> の unexpected |
| MSG-SER-08 | errorメッセージはframe 1のみ | serialize時にbinary framesを含めない |

---

## 2. UUID Generation (test_uuid.cpp)

| Test ID | 内容 | 判定基準 |
|---|---|---|
| TC-2.1.1 | UUID v4 lowercase hex → 受理 | regex match: `^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$` |
| TC-2.1.2 | UUID大文字 → エラーまたは正規化 | 正規化後はlowercase |
| TC-2.1.3 | UUID形式不正 → エラー | parse失敗 |
| UUID-01 | 生成されたUUIDがv4形式 | version bits == 4 |
| UUID-02 | 連続生成で重複なし | 1000件生成で全てユニーク |
| UUID-03 | 生成はlowercase hex | regex match (lowercase) |

---

## 3. Client Lifecycle + Perform (test_client.cpp)

| Test ID | 内容 | 判定基準 |
|---|---|---|
| CLIENT-01 | コンストラクタ → is_open() == true | 接続成功 |
| CLIENT-02 | close() → is_open() == false | ブロッキングでthread join完了 |
| CLIENT-03 | close()後のperform → エラー | running_ == false |
| CLIENT-04 | close()後のpoll() → 何もしない | running_ == false |
| CLIENT-05 | デストラクタでclose()される | leakなし（asan） |
| CLIENT-06 | move構築 → 元のobjectはis_open() == false | 所有権移動 |
| CLIENT-07 | ユーザー提供context → 動作 | 独自contextで通信可能 |
| CLIENT-08 | perform(callback) → callbackが呼ばれる | resume受信後callback実行 |
| CLIENT-09 | perform(callback, timeout) → タイムアウト時callback | is_error() == true, errorに "timeout" 含む |
| CLIENT-10 | perform(binary, callback) → binary付き送信 | binary_frames > 0 |
| CLIENT-11 | 未接続先へのperform → エラーハンドリング | exceptionまたはerror callback |
| CLIENT-12 | poll() で全メッセージdrain | 複数resume受信 → 全callback呼び出し |

---

## 4. Router Lifecycle + Handler (test_router.cpp)

| Test ID | 内容 | 判定基準 |
|---|---|---|
| TC-2.2.1 | Client側がID生成 → Router側はID変更しない | 受信performのidがそのままresumeに反映 |
| TC-2.3.1 | resumeのid == performのid | 完全一致 |
| TC-5.1.1 | ClientはDEALERを使用 | socket type確認 |
| TC-5.1.2 | RouterはROUTERを使用 | socket type確認 |
| TC-7.1.1 | エラーメッセージは英語 | ASCII only |
| TC-7.1.2 | エラーにbinaryなし | frame 1のみ |
| TC-7.2.1 | 未登録エフェクト → "no handler for: X" | error message match |
| TC-7.2.2 | handler例外 → "handler error: X" | error message match |
| TC-7.2.3 | 不正JSON → "invalid message: X" | error message match |
| TC-7.2.4 | perform ID不明 → 実装生成ID | idが空でない |
| TC-8.1 | ROUTER identity frame自動付加 | 受信時にidentity frame存在 |
| TC-8.2 | 返信時identity frame先頭付加 | 正しいDEALERにルーティング |
| TC-9.1 | PascalCase effect名 → 受理 | "Transcribe", "AskLLM" |
| TC-9.2 | ドット区切り名前空間 → 受理 | "Audio.Analyze" |
| ROUTER-01 | on("Effect", handler) → handler呼び出し | ctx->effect() == "Effect" |
| ROUTER-02 | 未登録エフェクト → on_unregistered handler | on_unregisteredが呼ばれる |
| ROUTER-03 | on_unregistered未設定 → 自動error返信 | "no handler for: X" |
| ROUTER-04 | off("Effect") → handler解除 | 該当effectでhandler呼ばれない |
| ROUTER-05 | clear_handlers() → 全解除 | 全effectでerror返信 |
| ROUTER-06 | handler内ctx->resume() → resume送信 | client側でcallback呼ばれる |
| ROUTER-07 | handler内ctx->error() → error送信 | client側でis_error() == true |
| ROUTER-08 | ctx->resume()二回目 → 何もしない | 2回目のresumeは送信されない |
| ROUTER-09 | handler例外 → 自動error送信 | "handler error: ..." |
| ROUTER-10 | shared_ptr<ctx>を別スレッドにmove → 安全 | 非同期resume成功 |
| ROUTER-11 | poll()中handler実行はメインスレッド | std::this_thread::get_id() 一致 |

---

## 5. Integration (test_integration.cpp)

### protocol compliance

| Test ID | 内容 | 判定基準 |
|---|---|---|
| TC-5.2.1 | tcp:// transport → 通信成功 | perform → resume round-trip |
| TC-5.2.2 | ipc:// transport → 通信成功 | perform → resume round-trip (non-Windows) |
| TC-5.2.3 | inproc:// transport → 通信成功 | perform → resume round-trip |
| TC-5.3.1 | デフォルトポート5555 → 接続可能 | 接続成功 |
| TC-5.3.2 | ポート設定可能 → 接続可能 | 任意ポートで接続 |
| TC-6.1.1 | ZMQ socket共有禁止 → スレッド分離 | 各nodeが独立thread所有 |
| TC-6.2.1 | メインスレッドZMQ直接操作禁止 | socket accessはZMQ threadのみ |
| TC-6.2.2 | スレッド間通信はqueue | poll()でdrain確認 |

### implementation-specific

| Test ID | 内容 | 判定基準 |
|---|---|---|
| INT-01 | client → router → resume round-trip (tcp) | callback value == 期待値 |
| INT-02 | client → router → error round-trip (tcp) | is_error() == true |
| INT-03 | binary perform → binary resume round-trip | binary data同一性 |
| INT-04 | 複数client同時perform → 全resume受信 | callback全件呼ばれる |
| INT-05 | client timeout → error callback | is_error(), "timeout" 含む |
| INT-06 | close()中のpending performs → cleanup | leakなし、callback呼ばれる |
| INT-07 | inproc:// client ↔ router 同一プロセス | 通信成功 |
| INT-08 | poll()非ブロッキング確認 | 即座にreturn |
| INT-09 | perform後poll()なし → callback未実行 | poll()後に初めてcallback |
| INT-10 | 非同期handler（別スレッドresume）→ 動作 | round-trip成功 |
| INT-11 | handler例外 → clientがerror受信 | is_error(), "handler error" 含む |
| INT-12 | 大量binary frame (8 frames) → 正常処理 | 全frame取得可能 |
| INT-13 | binary_frames上限超過 (9 frames) → エラー | parse失敗またはerror返信 |

---

## 6. C API (test_c_api.cpp)

| Test ID | 内容 | 判定基準 |
|---|---|---|
| CAPI-01 | zmqae_client_new / destroy → leakなし | asan clean |
| CAPI-02 | zmqae_router_new / destroy → leakなし | asan clean |
| CAPI-03 | zmqae_client_perform → callback呼ばれる | json_valueが期待値 |
| CAPI-04 | zmqae_client_perform_timeout → timeout callback | error_messageに "timeout" 含む |
| CAPI-05 | zmqae_router_on → handler呼ばれる | ctx != NULL |
| CAPI-06 | zmqae_ctx_get_id/effect/payload → 正しい値 | strcmp一致 |
| CAPI-07 | zmqae_ctx_resume → clientがresult受信 | callback呼ばれる |
| CAPI-08 | zmqae_ctx_error → clientがerror受信 | error_message != NULL |
| CAPI-09 | zmqae_ctx_resume二回目 → ZMQAE_ALREADY | 戻り値 == ZMQAE_ALREADY |
| CAPI-10 | zmqae_ctx_release → 安全 | crashなし |
| CAPI-11 | C++例外がC境界を越えない | 全関数noexcept |
| CAPI-12 | zmqae_last_error → エラー詳細取得 | NULLでない文字列 |
| CAPI-13 | zmqae_client_close / zmqae_router_close → 安全 | is_open() == false |
| CAPI-14 | NULL handle渡し → crashしない | ZMQAE_INVALID返却 |
| CAPI-15 | binary perform/resume via C API → round-trip | data一致 |

---

## Total: 36 protocol compliance + 49 implementation-specific = 85 test cases
