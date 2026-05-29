#pragma once

#include "mattsql/catalog/catalog.hpp"
#include "mattsql/common/status.hpp"
#include "mattsql/engine.hpp"
#include "mattsql/runtime/platform.hpp"
#include "mattsql/txn/transaction.hpp"

#include <string_view>

namespace mattsql {

class SqlEngine {
public:
    /// Destroys a SQL engine through the interface pointer.
    virtual ~SqlEngine() = default;

    /// Executes one SQL statement using the engine's default transaction policy.
    virtual Result<QueryResult> Execute(std::string_view sql) = 0;

    /// Executes one SQL statement inside an existing transaction.
    virtual Result<QueryResult> Execute(std::string_view sql,
                                        Transaction& transaction) = 0;

    /// Returns the catalog interface owned by this engine.
    virtual Catalog& GetCatalog() = 0;

    /// Returns the runtime boundary used by this engine.
    virtual PlatformRuntime& GetRuntime() = 0;
};

} // namespace mattsql
