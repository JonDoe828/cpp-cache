#include <catch2/catch_test_macros.hpp>

#include "storage/CachedKvStore.h"
#include "storage/InMemoryKvStore.h"
#include "storage/KvCodec.h"
#include "storage/KvStateMachine.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using cppcache::storage::CachedKvStore;
using cppcache::storage::IKvStore;
using cppcache::storage::InMemoryKvStore;
using cppcache::storage::KvCommand;
using cppcache::storage::KvOperation;
using cppcache::storage::KvSnapshot;
using cppcache::storage::KvStateMachine;

TEST_CASE("in-memory KV store supports CRUD and snapshots", "[storage]") {
  InMemoryKvStore store;
  REQUIRE_FALSE(store.get("missing"));

  store.put("alpha", "one");
  store.put("beta", "two");
  REQUIRE(store.get("alpha") == "one");
  REQUIRE(store.size() == 2);

  const IKvStore::Entries snapshot = store.snapshot();
  REQUIRE(store.erase("alpha"));
  REQUIRE_FALSE(store.erase("alpha"));
  store.restore(snapshot);

  REQUIRE(store.get("alpha") == "one");
  REQUIRE(store.get("beta") == "two");
}

TEST_CASE("cache eviction never removes authoritative KV data", "[storage]") {
  CachedKvStore store(std::make_unique<InMemoryKvStore>(), 1);
  store.put("alpha", "one");
  store.put("beta", "two");

  REQUIRE(store.size() == 2);
  REQUIRE(store.get("alpha") == "one");
  REQUIRE(store.get("beta") == "two");
}

TEST_CASE("cached KV writes and restores cannot return stale values",
          "[storage]") {
  CachedKvStore store(std::make_unique<InMemoryKvStore>(), 2);
  store.put("key", "old");
  REQUIRE(store.get("key") == "old");

  store.put("key", "new");
  REQUIRE(store.get("key") == "new");

  store.restore({{"key", "restored"}});
  REQUIRE(store.get("key") == "restored");
  REQUIRE(store.erase("key"));
  REQUIRE_FALSE(store.get("key"));
}

TEST_CASE("cached KV rejects a null backing store", "[storage][boundary]") {
  REQUIRE_THROWS_AS(CachedKvStore(nullptr, 2), std::invalid_argument);
}

TEST_CASE("KV state machine applies deterministic commands",
          "[state-machine]") {
  auto store =
      std::make_unique<CachedKvStore>(std::make_unique<InMemoryKvStore>(), 2);
  KvStateMachine stateMachine(std::move(store));

  auto putResult =
      stateMachine.apply({KvOperation::Put, "key", "a", "client-1", 1});
  REQUIRE(putResult.applied);
  REQUIRE(putResult.value == "a");

  auto appendResult =
      stateMachine.apply({KvOperation::Append, "key", "b", "client-1", 2});
  REQUIRE(appendResult.applied);
  REQUIRE(appendResult.value == "ab");
  REQUIRE(stateMachine.get("key") == "ab");

  auto eraseResult =
      stateMachine.apply({KvOperation::Erase, "key", "", "client-1", 3});
  REQUIRE(eraseResult.applied);
  REQUIRE(eraseResult.value == "ab");
  REQUIRE_FALSE(stateMachine.get("key"));
}

TEST_CASE("KV state machine rejects duplicate client requests",
          "[state-machine]") {
  KvStateMachine stateMachine(std::make_unique<InMemoryKvStore>());
  const KvCommand command{KvOperation::Put, "key", "first", "client-1", 1};

  REQUIRE(stateMachine.apply(command).applied);
  REQUIRE_FALSE(stateMachine.apply(command).applied);
  REQUIRE(stateMachine.get("key") == "first");
}

TEST_CASE("KV snapshots preserve data and request deduplication",
          "[state-machine][snapshot]") {
  KvStateMachine source(std::make_unique<InMemoryKvStore>());
  const KvCommand command{KvOperation::Put, "key", "value", "client-1", 7};
  REQUIRE(source.apply(command).applied);

  const KvSnapshot snapshot = source.snapshot();
  KvStateMachine restored(std::make_unique<InMemoryKvStore>());
  restored.restore(snapshot);

  REQUIRE(restored.get("key") == "value");
  REQUIRE_FALSE(restored.apply(command).applied);
}

TEST_CASE("KV request deduplication metadata remains bounded",
          "[state-machine][boundary]") {
  KvStateMachine stateMachine(std::make_unique<InMemoryKvStore>());
  for (std::size_t client = 0;
       client <= KvStateMachine::kMaxTrackedClients; ++client) {
    REQUIRE(stateMachine
                .apply({KvOperation::Put, "key", std::to_string(client),
                        "client-" + std::to_string(client), 1})
                .applied);
  }

  REQUIRE(stateMachine.snapshot().clients.size() ==
          KvStateMachine::kMaxTrackedClients);
}

TEST_CASE("KV deduplication evicts the least recently applied client",
          "[state-machine][boundary]") {
  KvStateMachine stateMachine(std::make_unique<InMemoryKvStore>());
  for (std::size_t client = 0; client < KvStateMachine::kMaxTrackedClients;
       ++client) {
    REQUIRE(stateMachine
                .apply({KvOperation::Put, "key", "value",
                        "client-" + std::to_string(client), 1})
                .applied);
  }

  REQUIRE(stateMachine
              .apply({KvOperation::Put, "key", "refreshed", "client-0", 2})
              .applied);

  KvStateMachine restored(std::make_unique<InMemoryKvStore>());
  restored.restore(cppcache::storage::decodeSnapshot(
      cppcache::storage::encodeSnapshot(stateMachine.snapshot())));
  REQUIRE(restored
              .apply({KvOperation::Put, "key", "new", "new-client", 1})
              .applied);

  const auto snapshot = restored.snapshot();
  REQUIRE(snapshot.clients.contains("client-0"));
  REQUIRE_FALSE(snapshot.clients.contains("client-1"));
  REQUIRE(snapshot.clients.contains("new-client"));
}

TEST_CASE("KV wire codec uses platform-independent big-endian integers",
          "[state-machine][codec]") {
  const KvCommand command{KvOperation::Append, "k", "v", "c", 0x0102030405060708};
  const std::string encoded = cppcache::storage::encodeCommand(command);

  REQUIRE(static_cast<unsigned char>(encoded.at(0)) == 1);
  REQUIRE(encoded.substr(1, 8) == std::string("\0\0\0\0\0\0\0\1", 8));
  REQUIRE(encoded.substr(encoded.size() - 8) ==
          std::string("\1\2\3\4\5\6\7\10", 8));
  REQUIRE(cppcache::storage::decodeCommand(encoded).requestId ==
          command.requestId);
}

TEST_CASE("KV state machine serializes concurrent command application",
          "[state-machine][thread]") {
  KvStateMachine stateMachine(std::make_unique<InMemoryKvStore>());
  std::atomic<bool> start{false};
  std::vector<std::thread> workers;

  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker] {
      while (!start.load(std::memory_order_acquire)) {
      }
      for (std::uint64_t request = 1; request <= 200; ++request) {
        stateMachine.apply({KvOperation::Put, "key-" + std::to_string(worker),
                            std::to_string(request),
                            "client-" + std::to_string(worker), request});
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (auto &worker : workers)
    worker.join();

  REQUIRE(stateMachine.size() == 4);
  for (int worker = 0; worker < 4; ++worker)
    REQUIRE(stateMachine.get("key-" + std::to_string(worker)) == "200");
}
