#pragma once

#include "mattsql/binder/binder.hpp"

namespace mattsql {

class DefaultBinder final : public Binder {
public:
  /// Resolves parsed statements against the catalog and annotates expressions
  /// with stable table/column identifiers and SQL result types.
  Result<BoundStatementPtr> Bind(const Statement &statement, Catalog &catalog) override;
};

} // namespace mattsql
