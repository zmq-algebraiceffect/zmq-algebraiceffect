# AGENTS.md — zmq-algebraiceffect C++ Library

> AI agentが自律的に作業するためのガイド。

## このプロジェクトとは

ZMQ上でAlgebraic Effectsのセマンティクスを実現するC++17 header-only library。

```
perform("Transcribe", audio)  →  [ハンドラノード]  →  resume({ text: "..." })
```

## 作業開始の手順

1. `DESIGN.md` — 設計判断とAPI定義（必読）
2. `TESTCASES.md` — テストケース定義
3. `include/zmqae/` — ヘッダファイル群
4. `../protocol/PROTOCOL.md` — プロトコル仕様

## ディレクトリ構成

| パス | 役割 |
|---|---|
| `include/zmqae/zmqae.hpp` | メインエントリ（全てをinclude） |
| `include/zmqae/client.hpp` | DEALER client クラス |
| `include/zmqae/router.hpp` | ROUTER router クラス |
| `include/zmqae/perform_context.hpp` | 非同期resume context |
| `include/zmqae/types.hpp` | 値型・callback型・Result |
| `include/zmqae/uuid.hpp` | UUID v4生成 |
| `include/zmqae/zmqae.h` | C API (extern "C") |
| `include/zmqae/detail/` | 内部実装（queue, context, message） |
| `extern/` | submodules（libzmq, cppzmq, json, spdlog, doctest） |
| `tests/` | doctestテスト |
| `examples/` | CUI通信サンプル |

## ビルド

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/tests/zmqae_tests
./build/examples/handler tcp://*:5555 &
./build/examples/performer tcp://localhost:5555
```

## コーディング規約

- `~/.agents/docs/C++.md` に従う
- namespace: `zmqae`（snake_case）
- `using namespace` 禁止
- `{}` 初期化
- ポインタ `*`・参照 `&` は型側に寄せる

## 依存関係（全てsubmodule）

| ライブラリ | ライセンス | 用途 |
|---|---|---|
| libzmq | MPL-2.0 | ZMQ通信基盤 |
| cppzmq | MIT | C++ RAII wrapper |
| nlohmann/json | MIT | JSON処理 |
| spdlog | MIT | logging |
| doctest | MIT | testing |

## Pitfalls

- ZMQ socket はスレッドをまたいで共有しない（1 socket = 1 thread）
- handler は `poll()` 内でメインスレッド実行。ZMQスレッドではない
- `perform_context::resume()` は一回性（二回目はno-op）
- `shared_ptr<perform_context>` で所有権共有。非同期handler向け
- ROUTER返信にはidentity frameが必要（perform_contextが管理）
- Windowsでは `ipc://` 非対応
- inline static context は shared library 境界で別インスタンスになる場合がある

## コミット規約

```
type(scope): message

type:  feat | fix | docs | test | refactor | build
scope: core | c-api | test | example | cmake
```
