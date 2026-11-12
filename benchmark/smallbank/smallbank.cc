#include "smallbank.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include "txn_cc.h"
#include "util/os.h"

namespace smallbank {

using namespace felis;

size_t Client::g_customer_count = 1000;
std::vector<Input> Client::g_inputs;
std::string Client::g_input_path;
std::string Client::g_initial_state_path;

struct AccountBalances {
  int64_t savings;
  int64_t checking;
};

static AccountBalances ReadBalances(BaseTxn::BaseTxnRow &row) {
  auto raw = row.ReadVarStr();
  abort_if(raw == nullptr || raw->length() != sizeof(AccountBalances),
           "invalid SmallBank value: ptr {} length {} expected {}",
           static_cast<void *>(raw), raw == nullptr ? 0 : raw->length(),
           sizeof(AccountBalances));
  AccountBalances value;
  std::memcpy(&value, raw->data(), sizeof(value));
  return value;
}

static void WriteBalances(BaseTxn::BaseTxnRow &row, const AccountBalances &value) {
  auto raw = VarStr::New(sizeof(value));
  std::memcpy(raw->data(), &value, sizeof(value));
  row.WriteVarStr(raw);
}

struct State {
  VHandle *first;
  VHandle *second;
  TxnType type;

  struct LookupCompletion : public TxnStateCompletion<smallbank::State> {
    static bool writes_first(TxnType type) {
      return type != TxnType::Balance;
    }

    static bool writes_second(TxnType type) {
      return type == TxnType::Amalgamate || type == TxnType::SendPayment;
    }

    void operator()(int id, BaseTxn::LookupRowResult rows) {
      abort_if(rows[0] == nullptr, "SmallBank lookup failed for lookup id {}", id);
      auto row_addr = reinterpret_cast<uintptr_t>(rows[0]);
      abort_if(row_addr < 4096,
               "SmallBank lookup returned invalid row pointer {} for lookup id {}",
               row_addr, id);

      if (id == 0) {
        state->first = rows[0];
        if (writes_first(state->type)) {
          handle(rows[0]).AppendNewVersion(1);
        }
      } else if (id == 1) {
        state->second = rows[0];
        if (writes_second(state->type)) {
          handle(rows[0]).AppendNewVersion(1);
        }
      } else {
        abort_if(true, "unexpected SmallBank lookup id {}", id);
      }
    }
  };
};

class SmallBankSliceRouter {
 public:
  static int SliceToNodeId(int16_t slice_id) { return slice_id + 1; }
};

class SmallBankTxn : public felis::Txn<smallbank::State> {
  Input input;

  bool writes_first() const { return input.type != TxnType::Balance; }
  bool writes_second() const {
    return input.type == TxnType::Amalgamate || input.type == TxnType::SendPayment;
  }
  bool uses_second() const { return writes_second(); }

 public:
  SmallBankTxn(uint64_t serial_id, Input input)
      : felis::Txn<smallbank::State>(serial_id), input(input) {}

  void PrepareInsert() override final {}

  void Prepare() override final {
    INIT_ROUTINE_BRK(4096);

    state->first = nullptr;
    state->second = nullptr;
    state->type = input.type;
    Accounts::Key first_key;
    first_key.sb_customer_id = input.customer1;

    TxnIndexLookup<SmallBankSliceRouter,
             smallbank::State::LookupCompletion,
             void>(
      nullptr,
        KeyParam<Accounts>(first_key));

    if (uses_second()) {
      Accounts::Key second_key;
      second_key.sb_customer_id = input.customer2;

          TxnIndexLookup<SmallBankSliceRouter,
                 smallbank::State::LookupCompletion,
                 void>(
            nullptr,
          PlaceholderParam(),
          KeyParam<Accounts>(second_key));
    }
  }

  void Run() override final {
    abort_if(state->first == nullptr,
             "SmallBank missing first handle for type {} customer1 {}",
             static_cast<int>(input.type), input.customer1);
    abort_if(reinterpret_cast<uintptr_t>(state->first) < 4096,
             "SmallBank invalid first handle {} for type {} customer1 {}",
             reinterpret_cast<uintptr_t>(state->first),
             static_cast<int>(input.type), input.customer1);
    if (uses_second()) {
      abort_if(state->second == nullptr,
               "SmallBank missing second handle for type {} customer2 {}",
               static_cast<int>(input.type), input.customer2);
      abort_if(reinterpret_cast<uintptr_t>(state->second) < 4096,
               "SmallBank invalid second handle {} for type {} customer2 {}",
               reinterpret_cast<uintptr_t>(state->second),
               static_cast<int>(input.type), input.customer2);
    }

    if (input.type == TxnType::Balance) {
      root->AttachRoutine(
          MakeContext(), 1,
          [](const auto &context) {
            auto &[state, handle] = context;
            auto first = handle(state->first);
            auto value = ReadBalances(first);
            volatile int64_t total = value.savings + value.checking;
            (void) total;
          });
      return;
    }

    root->AttachRoutine(
        // Keep the distributed routine context naturally aligned; all fields
        // that cross worker boundaries use the native 64-bit tuple layout.
        MakeContext(static_cast<int64_t>(input.type), input.amount), 1,
        [](const auto &context) {
          auto &[state, handle, raw_type, amount] = context;
          auto type = static_cast<TxnType>(raw_type);
          auto first = handle(state->first);
          auto value1 = ReadBalances(first);

          switch (type) {
            case TxnType::Balance:
              abort_if(true, "Balance must use its no-argument routine");
              break;
            case TxnType::DepositChecking:
              value1.checking += amount;
              WriteBalances(first, value1);
              break;
            case TxnType::TransactSavings:
              if (value1.savings >= amount) value1.savings -= amount;
              WriteBalances(first, value1);
              break;
            case TxnType::WriteCheck:
              value1.checking -=
                  (value1.savings + value1.checking < amount)
                      ? amount + 1 : amount;
              WriteBalances(first, value1);
              break;
            case TxnType::SendPayment: {
              auto second = handle(state->second);
              auto value2 = ReadBalances(second);
              if (value1.checking >= amount) {
                value1.checking -= amount;
                value2.checking += amount;
              }
              WriteBalances(first, value1);
              WriteBalances(second, value2);
              break;
            }
            case TxnType::Amalgamate: {
              auto second = handle(state->second);
              auto value2 = ReadBalances(second);
              auto total = value1.savings + value1.checking;
              value1.savings = 0;
              value1.checking = 0;
              value2.checking += total;
              WriteBalances(first, value1);
              WriteBalances(second, value2);
              break;
            }
          }
        });
  }
};

void Client::LoadInputs(const std::string &path) {
  g_input_path = path;
  std::ifstream stream(path);
  abort_if(!stream, "cannot open SmallBank input {}", path);
  std::string line;
  size_t line_number = 0;
  while (std::getline(stream, line)) {
    line_number++;
    if (line.empty() || line[0] == '#') continue;
    std::stringstream row(line);
    std::string field;
    uint64_t values[4] = {};
    for (int i = 0; i < 4; i++) {
      abort_if(!std::getline(row, field, ','),
               "invalid SmallBank input line {}", line_number);
      values[i] = std::stoull(field);
    }
    abort_if(values[0] < 1 || values[0] > 6,
             "invalid SmallBank type on line {}", line_number);
    abort_if(values[1] == 0 || values[1] > g_customer_count,
             "invalid first customer on line {}", line_number);
    auto type = static_cast<TxnType>(values[0]);
    if (type == TxnType::Amalgamate || type == TxnType::SendPayment) {
      abort_if(values[2] == 0 || values[2] > g_customer_count || values[1] == values[2],
               "invalid second customer on line {}", line_number);
    }
    g_inputs.push_back(Input{type, values[1], values[2], static_cast<int64_t>(values[3])});
  }
  auto expected = (EpochClient::g_max_epoch - 1) * EpochClient::g_txn_per_epoch;
  abort_if(g_inputs.size() != expected,
           "SmallBank input has {} rows; expected {}", g_inputs.size(), expected);
}

BaseTxn *Client::CreateTxn(uint64_t serial_id) {
  auto epoch = serial_id >> 32;
  auto sequence = (serial_id >> 8) & 0xFFFFFF;
  abort_if(epoch == 0 || sequence == 0, "invalid serial id {}", serial_id);
  auto index = (epoch - 1) * EpochClient::g_txn_per_epoch + sequence - 1;
  abort_if(index >= g_inputs.size(), "SmallBank input index {} out of range", index);
  return new SmallBankTxn(serial_id, g_inputs[index]);
}

void Client::OnWorkloadComplete() {
  const auto output_path = g_input_path + ".final.csv";
  std::ofstream output(output_path, std::ios::trunc);
  abort_if(!output, "cannot create SmallBank final-state output {}", output_path);
  output << "customer_id,savings,checking\n";

  auto &table = util::Instance<TableManager>().Get<Accounts>();
  auto &conf = util::Instance<NodeConfiguration>();
  auto node_index = static_cast<uint64_t>(conf.node_id() - 1);
  auto node_start = node_index * Client::g_customer_count / conf.nr_nodes();
  auto node_end = (node_index + 1) * Client::g_customer_count / conf.nr_nodes();
  void *buffer = alloca(128);
  uint64_t rows = 0;
  int64_t total_savings = 0;
  int64_t total_checking = 0;
  for (uint64_t customer = node_start + 1; customer <= node_end; customer++) {
    Accounts::Key key;
    key.sb_customer_id = customer;
    auto handle = table.Search(key.EncodeView(buffer));
    abort_if(handle == nullptr, "SmallBank final-state lookup failed for customer {}", customer);
    auto raw = handle->ReadWithVersion(std::numeric_limits<uint64_t>::max());
    abort_if(raw == nullptr || raw->length() != sizeof(AccountBalances),
             "invalid SmallBank final value for customer {}", customer);
    AccountBalances value;
    std::memcpy(&value, raw->data(), sizeof(value));
    output << customer << ',' << value.savings << ',' << value.checking << '\n';
    total_savings += value.savings;
    total_checking += value.checking;
    rows++;
  }
  output.close();
  logger->info("SmallBank final state rows {} savings {} checking {} path {}",
               rows, total_savings, total_checking, output_path);
}

void Loader::Run() {
  auto &manager = util::Instance<TableManager>();
  manager.Create<Accounts>();
  auto &table = manager.Get<Accounts>();
  void *buffer = alloca(128);

  auto &conf = util::Instance<NodeConfiguration>();
  auto node_index = static_cast<uint64_t>(conf.node_id() - 1);
  auto node_start = node_index * Client::g_customer_count / conf.nr_nodes();
  auto node_end = (node_index + 1) * Client::g_customer_count / conf.nr_nodes();

  auto insert = [&](uint64_t customer, int64_t savings, int64_t checking) {
    Accounts::Key key;
    key.sb_customer_id = customer;
    AccountBalances value{savings, checking};
    auto handle = table.SearchOrCreate(key.EncodeView(buffer));
    auto raw = VarStr::New(sizeof(value));
    std::memcpy(raw->data(), &value, sizeof(value));
    InitVersion(handle, raw);
  };

  if (Client::g_initial_state_path.empty()) {
    for (uint64_t customer = node_start + 1; customer <= node_end; customer++) {
      insert(customer, 1000, 1000);
    }
    done = true;
    return;
  }

  std::ifstream stream(Client::g_initial_state_path);
  abort_if(!stream, "cannot open SmallBank initial state {}",
           Client::g_initial_state_path);
  std::string line;
  uint64_t expected_customer = 1;
  while (std::getline(stream, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (line == "customer_id,savings,checking") continue;
    std::stringstream row(line);
    std::string customer_field, savings_field, checking_field, extra;
    abort_if(!std::getline(row, customer_field, ',') ||
                 !std::getline(row, savings_field, ',') ||
                 !std::getline(row, checking_field, ',') ||
                 std::getline(row, extra, ','),
             "invalid SmallBank initial-state row {}", line);
    auto customer = std::stoull(customer_field);
    auto savings = std::stoll(savings_field);
    auto checking = std::stoll(checking_field);
    abort_if(customer != expected_customer,
             "SmallBank initial-state customer {} expected {}",
             customer, expected_customer);
    abort_if(savings < 0 || checking < 0,
             "SmallBank initial state has negative balance for customer {}",
             customer);
    if (customer > node_start && customer <= node_end)
      insert(customer, savings, checking);
    expected_customer++;
  }
  abort_if(expected_customer != Client::g_customer_count + 1,
           "SmallBank initial state has {} rows; expected {}",
           expected_customer - 1, Client::g_customer_count);
  done = true;
}

}
