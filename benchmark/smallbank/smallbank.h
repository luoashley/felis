#ifndef SMALLBANK_H
#define SMALLBANK_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "epoch.h"
#include "index.h"
#include "slice.h"
#include "table_decl.h"

namespace smallbank {

enum class TableType : int { SmallBankBase = 300, Accounts };

struct Accounts {
  static uint32_t HashKey(const felis::VarStrView &key) {
    uint64_t value = 0;
    auto length = std::min(sizeof(value), static_cast<size_t>(key.length()));
    std::memcpy(&value, key.data(), length);
    return static_cast<uint32_t>(value ^ (value >> 32));
  }
  static constexpr auto kTable = TableType::Accounts;
  // The matched comparison uses ten million replicated accounts. Keep the
  // hash table below a 0.60 load factor at that scale.
  static constexpr auto kIndexArgs = std::make_tuple(HashKey, 1 << 24, false);
  using IndexBackend = felis::HashtableIndex;
  using Key = sql::SmallBankKey;
  using Value = sql::SmallBankValue;
};

enum class TxnType : uint8_t {
  Amalgamate = 1,
  Balance = 2,
  DepositChecking = 3,
  SendPayment = 4,
  TransactSavings = 5,
  WriteCheck = 6,
};

struct Input {
  TxnType type;
  uint64_t customer1;
  uint64_t customer2;
  int64_t amount;
};

class Client : public felis::EpochClient {
 public:
  static size_t g_customer_count;
  static std::vector<Input> g_inputs;
  static std::string g_input_path;
  static std::string g_initial_state_path;


  unsigned int LoadPercentage() final override { return 100; }
  felis::BaseTxn *CreateTxn(uint64_t serial_id) final override;
  void OnWorkloadComplete() final override;
  static void LoadInputs(const std::string &path);
};

class Loader : public go::Routine {
  std::atomic_bool done{false};
 public:
  void Run() override final;
  void Wait() { while (!done.load()) sleep(1); }
};

}

namespace felis {
using namespace smallbank;
// SmallBank validation uses full-copy replicas: every node initializes every
// account and executes the same ordered stream locally.  Route each lookup to
// the current node rather than hard-coding node 1 or partitioning the table.
SHARD_TABLE(Accounts) {
  return util::Instance<NodeConfiguration>().node_id() - 1;
}
}

#endif
