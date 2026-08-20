#include "storage/KvStateMachine.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace cppcache::storage {

KvStateMachine::KvStateMachine(std::unique_ptr<IKvStore> store)
    : store_(std::move(store)) {
  if (!store_)
    throw std::invalid_argument("state machine store must not be null");
}

ApplyResult KvStateMachine::apply(const KvCommand &command) {
  if (command.operation == KvOperation::NoOp)
    return {true, std::nullopt};
  if (command.clientId.empty())
    throw std::invalid_argument("client id must not be empty");
  if (command.requestId == 0)
    throw std::invalid_argument("request id must be greater than zero");

  std::lock_guard<std::mutex> lock(mutex_);
  auto requestIt = clients_.find(command.clientId);
  if (requestIt != clients_.end() &&
      command.requestId <= requestIt->second.requestId) {
    return {false, store_->get(command.key)};
  }
  if (lastApplySequence_ == std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("client apply sequence exhausted");

  std::optional<std::string> result;
  switch (command.operation) {
  case KvOperation::Put:
    store_->put(command.key, command.value);
    result = command.value;
    break;
  case KvOperation::Append: {
    std::string value = store_->get(command.key).value_or("");
    value += command.value;
    store_->put(command.key, value);
    result = std::move(value);
    break;
  }
  case KvOperation::Erase:
    result = store_->get(command.key);
    (void)store_->erase(command.key);
    break;
  case KvOperation::NoOp:
    break;
  }

  if (requestIt == clients_.end() && clients_.size() >= kMaxTrackedClients) {
    const auto leastRecent = clientsBySequence_.begin();
    clients_.erase(leastRecent->second);
    clientsBySequence_.erase(leastRecent);
  }
  if (requestIt != clients_.end())
    clientsBySequence_.erase(requestIt->second.applySequence);
  const std::uint64_t sequence = ++lastApplySequence_;
  clients_[command.clientId] = {command.requestId, sequence};
  clientsBySequence_[sequence] = command.clientId;
  return {true, std::move(result)};
}

std::optional<std::string> KvStateMachine::get(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);
  return store_->get(key);
}

std::size_t KvStateMachine::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return store_->size();
}

KvSnapshot KvStateMachine::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {store_->snapshot(), clients_, lastApplySequence_};
}

void KvStateMachine::restore(const KvSnapshot &snapshot) {
  if (snapshot.clients.size() > kMaxTrackedClients)
    throw std::invalid_argument("snapshot has too many client records");

  std::map<std::uint64_t, std::string> clientsBySequence;
  for (const auto &[clientId, record] : snapshot.clients) {
    if (clientId.empty() || record.requestId == 0 || record.applySequence == 0 ||
        record.applySequence > snapshot.lastApplySequence ||
        !clientsBySequence.emplace(record.applySequence, clientId).second)
      throw std::invalid_argument("snapshot has invalid client records");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  store_->restore(snapshot.entries);
  clients_ = snapshot.clients;
  clientsBySequence_ = std::move(clientsBySequence);
  lastApplySequence_ = snapshot.lastApplySequence;
}

} // namespace cppcache::storage
