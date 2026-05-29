#pragma once

#include "mattsql/catalog/schema.hpp"
#include "mattsql/common/status.hpp"
#include "mattsql/common/types.hpp"

#include <vector>

namespace mattsql {

class TupleCodec {
public:
  /// Destroys a tuple codec through the interface pointer.
  virtual ~TupleCodec() = default;

  /// Encodes logical SQL values into the storage record format for a schema.
  virtual Result<Tuple> Encode(const TableSchema &schema,
                               const std::vector<Value> &values) const = 0;

  /// Decodes a storage record back into logical SQL values for execution.
  virtual Result<std::vector<Value>> Decode(const TableSchema &schema,
                                            ConstBufferView record) const = 0;
};

class BinaryTupleCodec final : public TupleCodec {
public:
  /// Encodes values using a compact schema-driven binary format.
  Result<Tuple> Encode(const TableSchema &schema,
                       const std::vector<Value> &values) const override;

  /// Decodes values written by BinaryTupleCodec::Encode.
  Result<std::vector<Value>> Decode(const TableSchema &schema,
                                    ConstBufferView record) const override;
};

} // namespace mattsql
