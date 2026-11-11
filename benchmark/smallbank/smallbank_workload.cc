#include "smallbank.h"

#include "module.h"
#include "opts.h"

namespace felis {

class SmallBankModule : public Module<WorkloadModule> {
 public:
  SmallBankModule() {
    info = {
      .name = "smallbank",
      .description = "SmallBank from externally ordered broker packages",
    };
  }

  void Init() override final {
    Module<CoreModule>::InitModule("node-server");
    Module<CoreModule>::InitModule("allocator");

    abort_if(!Options::kSmallBankInput,
             "smallbank requires -XSmallBankInputPATH");
    smallbank::Client::g_customer_count =
        Options::kSmallBankCustomers.ToLargeNumber("1000");
    smallbank::Client::g_initial_state_path =
        Options::kSmallBankInitialState
            ? Options::kSmallBankInitialState.Get()
            : "";
    smallbank::Client::LoadInputs(Options::kSmallBankInput.Get());

    auto loader = new smallbank::Loader();
    go::GetSchedulerFromPool(1)->WakeUp(loader);
    loader->Wait();

    EpochClient::g_workload_client = new smallbank::Client();
  }
};

static SmallBankModule smallbank_module;

}
