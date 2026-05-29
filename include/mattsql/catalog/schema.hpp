#pragma once

#include "mattsql/common/types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace mattsql {

struct ColumnSchema {
    std::string name;
    SqlType type = SqlType::Null;
    bool nullable = true;
    ColumnId id = 0;
};

struct TableSchema {
    std::vector<ColumnSchema> columns;
};

struct IndexSchema {
    std::string name;
    std::vector<ColumnId> key_columns;
    bool unique = false;
};

struct CreateTableRequest {
    std::string name;
    TableSchema schema;
};

struct CreateIndexRequest {
    std::string table_name;
    IndexSchema schema;
};

} // namespace mattsql
