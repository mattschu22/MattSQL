#pragma once

#include "mattsql/catalog/schema.hpp"
#include "mattsql/common/types.hpp"

#include <string>
#include <vector>

namespace mattsql {

struct IndexInfo {
    IndexId id = 0;
    std::string name;
    TableId table_id = 0;
    IndexSchema schema;
    PageId root_page_id = kInvalidPageId;
};

struct TableInfo {
    TableId id = 0;
    std::string name;
    TableSchema schema;
    PageId heap_root_page_id = kInvalidPageId;
    std::vector<IndexInfo> indexes;
};

} // namespace mattsql
