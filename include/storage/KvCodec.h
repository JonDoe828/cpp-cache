#pragma once

#include "storage/KvCommand.h"
#include "storage/KvStateMachine.h"

#include <string>
#include <string_view>

namespace cppcache::storage {

std::string encodeCommand(const KvCommand &command);
KvCommand decodeCommand(std::string_view bytes);

std::string encodeSnapshot(const KvSnapshot &snapshot);
KvSnapshot decodeSnapshot(std::string_view bytes);

} // namespace cppcache::storage
