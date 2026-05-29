#pragma once

#include "mattsql/catalog/catalog.hpp"
#include "mattsql/execution/executor.hpp"
#include "mattsql/storage/table_storage.hpp"
#include "mattsql/storage/tuple_codec.hpp"

namespace mattsql {

class DefaultExecutor final : public Executor {
public:
  /// Creates an executor using the default binary tuple codec.
  DefaultExecutor(Catalog &catalog, TableStorageManager &storage);

  /// Creates an executor with an injected tuple codec for tests or alternate
  /// storage formats.
  DefaultExecutor(Catalog &catalog, TableStorageManager &storage,
                  const TupleCodec &tuple_codec);

  /// Executes a physical plan against catalog metadata and table storage.
  Result<QueryResult> Execute(const PhysicalPlan &plan,
                              Transaction &transaction) override;

private:
  Catalog *catalog_;
  TableStorageManager *storage_;
  BinaryTupleCodec default_tuple_codec_;
  const TupleCodec *tuple_codec_;
};

} // namespace mattsql
