#pragma once

#include "mattsql/binder/bound_statement.hpp"
#include "mattsql/catalog/catalog.hpp"
#include "mattsql/common/status.hpp"
#include "mattsql/parser/ast.hpp"

namespace mattsql {

class Binder {
public:
    /// Destroys a binder through the interface pointer.
    virtual ~Binder() = default;

    /// Binds parsed AST names and types against the catalog.
    virtual Result<BoundStatementPtr> Bind(const Statement& statement,
                                           Catalog& catalog) = 0;
};

} // namespace mattsql
