#include <catch2/catch_test_macros.hpp>

#include "raft/FileRaftStorage.h"
#include "raft/MemoryRaftStorage.h"
#include "raft/RaftCore.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using cppcache::raft::AppendEntriesRequest;
using cppcache::raft::FileRaftStorage;
using cppcache::raft::LogEntry;
using cppcache::raft::MemoryRaftStorage;
using cppcache::raft::PersistedRaftState;
using cppcache::raft::RaftCore;
using cppcache::raft::Role;
using cppcache::storage::KvCommand;
using cppcache::storage::KvOperation;

namespace {

class CountingStorage final : public cppcache::raft::IRaftStorage {
public:
  PersistedRaftState load() const override { return state; }

  void save(const PersistedRaftState &newState) override {
    state = newState;
    ++saveCount;
  }

  PersistedRaftState state;
  std::size_t saveCount{0};
};

KvCommand command(std::string value, std::uint64_t requestId = 1) {
  return {KvOperation::Put, "key", std::move(value), "client", requestId};
}

void elect(RaftCore &candidate, RaftCore &voter, std::uint32_t voterId) {
  const auto request = candidate.startElection();
  const auto response = voter.handleRequestVote(request);
  REQUIRE(response.voteGranted);
  REQUIRE(candidate.recordVote(voterId, response));
  REQUIRE(candidate.role() == Role::Leader);
}

} // namespace

TEST_CASE("Raft elects a leader with a majority vote", "[raft][election]") {
  RaftCore first(1, {2, 3}, std::make_shared<MemoryRaftStorage>());
  RaftCore second(2, {1, 3}, std::make_shared<MemoryRaftStorage>());

  elect(first, second, 2);
  REQUIRE(first.currentTerm() == 1);
  REQUIRE(first.leaderId() == 1);
  REQUIRE(second.role() == Role::Follower);
}

TEST_CASE("Raft grants at most one vote per term", "[raft][election]") {
  RaftCore voter(3, {1, 2}, std::make_shared<MemoryRaftStorage>());
  const auto firstVote = voter.handleRequestVote({1, 1, 0, 0});
  const auto secondVote = voter.handleRequestVote({1, 2, 0, 0});

  REQUIRE(firstVote.voteGranted);
  REQUIRE_FALSE(secondVote.voteGranted);
}

TEST_CASE("Raft commits an entry only after majority replication",
          "[raft][replication]") {
  RaftCore leader(1, {2, 3}, std::make_shared<MemoryRaftStorage>());
  RaftCore follower(2, {1, 3}, std::make_shared<MemoryRaftStorage>());
  elect(leader, follower, 2);

  REQUIRE(leader.append(command("value")) == 2);
  REQUIRE(leader.commitIndex() == 0);

  auto request = leader.makeAppendEntries(2);
  REQUIRE(request);
  auto response = follower.handleAppendEntries(*request);
  REQUIRE_FALSE(response.success);
  leader.recordAppendEntriesResponse(2, response);

  request = leader.makeAppendEntries(2);
  REQUIRE(request);
  response = follower.handleAppendEntries(*request);
  REQUIRE(response.success);
  leader.recordAppendEntriesResponse(2, response);
  REQUIRE(leader.commitIndex() == 2);

  const auto heartbeat = leader.makeAppendEntries(2);
  REQUIRE(heartbeat);
  REQUIRE(heartbeat->entries.empty());
  REQUIRE(follower.handleAppendEntries(*heartbeat).success);

  const auto leaderCommitted = leader.takeCommitted();
  const auto followerCommitted = follower.takeCommitted();
  REQUIRE(leaderCommitted.size() == 2);
  REQUIRE(followerCommitted.size() == 2);
  REQUIRE(leaderCommitted.back().command.value == "value");
  REQUIRE(followerCommitted.back().command.value == "value");
}

TEST_CASE("Raft replication builders tolerate concurrent leader step-down",
          "[raft][regression]") {
  RaftCore node(1, {}, std::make_shared<MemoryRaftStorage>());
  node.startElection();
  REQUIRE(node.role() == Role::Leader);

  node.handleRequestVote({node.currentTerm() + 1, 2, 0, 0});
  REQUIRE(node.role() == Role::Follower);
  REQUIRE_FALSE(node.makeAppendEntries(2));
  REQUIRE_FALSE(node.makeInstallSnapshot(2));
}

TEST_CASE("Raft follower does not persist unchanged heartbeats",
          "[raft][persistence][regression]") {
  auto storage = std::make_shared<CountingStorage>();
  RaftCore follower(2, {1}, storage);
  const AppendEntriesRequest heartbeat{1, 1, 0, 0, {}, 0};

  REQUIRE(follower.handleAppendEntries(heartbeat).success);
  const std::size_t savesAfterNewTerm = storage->saveCount;
  REQUIRE(savesAfterNewTerm == 1);
  for (int heartbeatIndex = 0; heartbeatIndex < 20; ++heartbeatIndex)
    REQUIRE(follower.handleAppendEntries(heartbeat).success);
  REQUIRE(storage->saveCount == savesAfterNewTerm);
}

TEST_CASE("Raft repairs a conflicting follower suffix", "[raft][replication]") {
  RaftCore follower(2, {1, 3}, std::make_shared<MemoryRaftStorage>());
  AppendEntriesRequest oldLeader{1, 1, 0, 0, {LogEntry{1, command("old")}}, 0};
  REQUIRE(follower.handleAppendEntries(oldLeader).success);

  AppendEntriesRequest newLeader{2, 3, 0, 0, {LogEntry{2, command("new")}}, 1};
  REQUIRE(follower.handleAppendEntries(newLeader).success);
  const auto committed = follower.takeCommitted();
  REQUIRE(committed.size() == 1);
  REQUIRE(committed.front().command.value == "new");
  REQUIRE(follower.currentTerm() == 2);
}

TEST_CASE("Raft rejects candidates with stale logs", "[raft][election]") {
  RaftCore voter(2, {1, 3}, std::make_shared<MemoryRaftStorage>());
  REQUIRE(
      voter.handleAppendEntries({2, 3, 0, 0, {LogEntry{2, command("new")}}, 0})
          .success);

  const auto response = voter.handleRequestVote({3, 1, 0, 0});
  REQUIRE_FALSE(response.voteGranted);
  REQUIRE(response.term == 3);
}

TEST_CASE("Raft compacts and installs snapshots", "[raft][snapshot]") {
  RaftCore source(1, {}, std::make_shared<MemoryRaftStorage>());
  source.startElection();
  REQUIRE(source.role() == Role::Leader);
  REQUIRE(source.append(command("one", 1)) == 2);
  REQUIRE(source.append(command("two", 2)) == 3);
  REQUIRE(source.takeCommitted().size() == 3);
  source.compact(3, "snapshot-data");

  RaftCore target(2, {1}, std::make_shared<MemoryRaftStorage>());
  const auto response = target.handleInstallSnapshot(
      {source.currentTerm(), 1, 3, source.currentTerm(), "snapshot-data"});
  REQUIRE(response.success);
  REQUIRE(target.snapshotIndex() == 3);
  REQUIRE(target.snapshot() == "snapshot-data");
}

TEST_CASE("file Raft storage round-trips durable state",
          "[raft][persistence]") {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("cpp-cache-raft-" + std::to_string(suffix) + ".bin");

  FileRaftStorage storage(path);
  PersistedRaftState expected;
  expected.currentTerm = 4;
  expected.votedFor = 2;
  expected.snapshotIndex = 7;
  expected.snapshotTerm = 3;
  expected.snapshot = "snapshot";
  expected.commitIndex = 7;
  expected.log.push_back({4, command("value", 9)});
  storage.save(expected);

  std::ifstream rawState(path, std::ios::binary);
  std::string headerAndTerm(16, '\0');
  rawState.read(headerAndTerm.data(),
                static_cast<std::streamsize>(headerAndTerm.size()));
  REQUIRE(headerAndTerm.substr(0, 8) == "CPPRAFT4");
  REQUIRE(headerAndTerm.substr(8, 8) ==
          std::string("\0\0\0\0\0\0\0\4", 8));

  const PersistedRaftState actual = storage.load();
  REQUIRE(actual.currentTerm == expected.currentTerm);
  REQUIRE(actual.votedFor == expected.votedFor);
  REQUIRE(actual.snapshotIndex == expected.snapshotIndex);
  REQUIRE(actual.snapshotTerm == expected.snapshotTerm);
  REQUIRE(actual.snapshot == expected.snapshot);
  REQUIRE(actual.commitIndex == expected.commitIndex);
  REQUIRE(actual.log.size() == 1);
  REQUIRE(actual.log.front().term == 4);
  REQUIRE(actual.log.front().command.value == "value");
  REQUIRE(actual.log.front().command.requestId == 9);

  std::filesystem::remove(path);
}

TEST_CASE("Raft replays committed entries after restart",
          "[raft][persistence]") {
  auto storage = std::make_shared<MemoryRaftStorage>();
  {
    RaftCore original(1, {}, storage);
    original.startElection();
    REQUIRE(original.append(command("durable")) == 2);
    REQUIRE(original.commitIndex() == 2);
  }

  RaftCore restored(1, {}, storage);
  const auto committed = restored.takeCommitted();
  REQUIRE(committed.size() == 2);
  REQUIRE(committed.back().command.value == "durable");
}
