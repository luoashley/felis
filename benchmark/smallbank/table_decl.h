// SmallBank schema used by the broker-package integration workload.
#ifndef SMALLBANK_TABLE_DECL_H
#define SMALLBANK_TABLE_DECL_H

#include "sqltypes.h"

namespace sql {

FIELD(uint64_t, sb_customer_id);
KEYS(SmallBankKey);

FIELD(int64_t, sb_savings);
FIELD(int64_t, sb_checking);
VALUES(SmallBankValue);

}

#endif
