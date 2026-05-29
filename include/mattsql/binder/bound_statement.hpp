#pragma once

#include "mattsql/binder/bound_expression.hpp"
#include "mattsql/catalog/table_info.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mattsql {

enum class BoundStatementKind {
    Select,
    Insert,
    CreateTable
};

struct BoundStatement {
    /// Destroys a bound statement through a base pointer.
    virtual ~BoundStatement() = default;

    BoundStatementKind kind = BoundStatementKind::Select;
};

using BoundStatementPtr = std::unique_ptr<BoundStatement>;

struct BoundSelectStatement final : BoundStatement {
    std::vector<BoundExpressionPtr> projections;
    TableInfo table;
    BoundExpressionPtr where;
};

struct BoundInsertStatement final : BoundStatement {
    TableInfo table;
    std::vector<BoundExpressionPtr> values;
};

struct BoundCreateTableStatement final : BoundStatement {
    CreateTableRequest request;
};

} // namespace mattsql
