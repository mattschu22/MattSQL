#include "sql_logic_support.hpp"

#include "mattsql/common/result_utils.hpp"
#include "mattsql/sql_engine.hpp"

#include "sql_logic_format.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sql_logic {
namespace {

class FuzzRng {
public:
  explicit FuzzRng(std::uint64_t seed) : state_(seed) {}

  std::uint32_t NextU32() {
    state_ = state_ * std::uint64_t{6364136223846793005} +
             std::uint64_t{1442695040888963407};
    return static_cast<std::uint32_t>(state_ >> 32U);
  }

  std::size_t NextIndex(std::size_t bound) {
    return static_cast<std::size_t>(NextU32() % static_cast<std::uint32_t>(bound));
  }

  int Between(int minimum, int maximum) {
    const auto span = static_cast<std::uint32_t>(maximum - minimum + 1);
    return minimum + static_cast<int>(NextU32() % span);
  }

  bool Coin() { return (NextU32() & 1U) == 1U; }

private:
  std::uint64_t state_;
};

struct FuzzRow {
  int id = 0;
  std::string name;
  bool active = false;
  int score = 0;
};

enum class FuzzPredicateKind {
  ScoreAtLeast,
  IdAtMost,
  ActiveEquals,
  NameEquals,
  ActiveAndScore,
  ActiveOrId,
  ScoreRangeOrName,
  NotActive,
  IdNotEqual
};

struct FuzzPredicate {
  FuzzPredicateKind kind = FuzzPredicateKind::ScoreAtLeast;
  int first = 0;
  int second = 0;
  bool flag = false;
  std::string name;
  std::string sql;
};

struct FuzzQuery {
  std::string sql;
  mattsql::QueryResult expected;
  bool sort_rows = false;
};

std::string make_fuzz_name(FuzzRng &rng, std::size_t row_index) {
  constexpr std::string_view bases[] = {"Ada",    "Grace",  "Lin",
                                        "O'Neil", "Turing", "Kay"};
  constexpr std::size_t base_count = sizeof(bases) / sizeof(bases[0]);

  std::string name(bases[rng.NextIndex(base_count)]);
  name += '_';
  name += std::to_string(row_index);
  name += '_';

  const auto suffix_length = rng.Between(8, 36);
  for (int index = 0; index < suffix_length; ++index) {
    name.push_back(static_cast<char>('a' + rng.Between(0, 25)));
  }

  return name;
}

bool matches_predicate(const FuzzRow &row, const FuzzPredicate &predicate) {
  switch (predicate.kind) {
  case FuzzPredicateKind::ScoreAtLeast:
    return row.score >= predicate.first;
  case FuzzPredicateKind::IdAtMost:
    return row.id <= predicate.first;
  case FuzzPredicateKind::ActiveEquals:
    return row.active == predicate.flag;
  case FuzzPredicateKind::NameEquals:
    return row.name == predicate.name;
  case FuzzPredicateKind::ActiveAndScore:
    return row.active == predicate.flag && row.score >= predicate.first;
  case FuzzPredicateKind::ActiveOrId:
    return row.active == predicate.flag || row.id == predicate.first;
  case FuzzPredicateKind::ScoreRangeOrName:
    return (row.score >= predicate.first && row.score <= predicate.second) ||
           row.name == predicate.name;
  case FuzzPredicateKind::NotActive:
    return !row.active;
  case FuzzPredicateKind::IdNotEqual:
    return row.id != predicate.first;
  }
  return false;
}

FuzzPredicate make_predicate(FuzzRng &rng, const std::vector<FuzzRow> &rows) {
  FuzzPredicate predicate;
  predicate.kind = static_cast<FuzzPredicateKind>(rng.NextIndex(9));
  const auto &sample = rows[rng.NextIndex(rows.size())];

  switch (predicate.kind) {
  case FuzzPredicateKind::ScoreAtLeast:
    predicate.first = rng.Between(-55, 55);
    predicate.sql = "score >= " + std::to_string(predicate.first);
    break;
  case FuzzPredicateKind::IdAtMost:
    predicate.first = rng.Between(1, static_cast<int>(rows.size()));
    predicate.sql = "id <= " + std::to_string(predicate.first);
    break;
  case FuzzPredicateKind::ActiveEquals:
    predicate.flag = rng.Coin();
    predicate.sql = "active = " + SqlBool(predicate.flag);
    break;
  case FuzzPredicateKind::NameEquals:
    predicate.name = sample.name;
    predicate.sql = "name = " + QuoteSqlString(predicate.name);
    break;
  case FuzzPredicateKind::ActiveAndScore:
    predicate.flag = rng.Coin();
    predicate.first = rng.Between(-55, 55);
    predicate.sql = "active = " + SqlBool(predicate.flag) +
                    " AND score >= " + std::to_string(predicate.first);
    break;
  case FuzzPredicateKind::ActiveOrId:
    predicate.flag = rng.Coin();
    predicate.first = sample.id;
    predicate.sql = "active = " + SqlBool(predicate.flag) +
                    " OR id = " + std::to_string(predicate.first);
    break;
  case FuzzPredicateKind::ScoreRangeOrName: {
    const auto left = rng.Between(-55, 55);
    const auto right = rng.Between(-55, 55);
    predicate.first = std::min(left, right);
    predicate.second = std::max(left, right);
    predicate.name = sample.name;
    predicate.sql = "(score >= " + std::to_string(predicate.first) +
                    " AND score <= " + std::to_string(predicate.second) +
                    ") OR name = " + QuoteSqlString(predicate.name);
    break;
  }
  case FuzzPredicateKind::NotActive:
    predicate.sql = "NOT active";
    break;
  case FuzzPredicateKind::IdNotEqual:
    predicate.first = sample.id;
    predicate.sql = "id <> " + std::to_string(predicate.first);
    break;
  }

  return predicate;
}

std::string add_integer_expression(std::string_view expression, int delta) {
  if (delta < 0) {
    return std::string(expression) + " - " + std::to_string(-delta);
  }
  return std::string(expression) + " + " + std::to_string(delta);
}

void add_expected_row(mattsql::QueryResult &result, const FuzzRow &row, int query_kind,
                      int delta) {
  switch (query_kind) {
  case 0:
    result.rows.push_back(
        {std::int64_t{row.id}, row.name, row.active, std::int64_t{row.score}});
    break;
  case 1:
    result.rows.push_back({std::int64_t{row.id}, std::int64_t{row.score + delta}});
    break;
  case 2:
    result.rows.push_back({row.name, row.active});
    break;
  case 3:
    result.rows.push_back(
        {std::int64_t{row.id}, std::int64_t{row.score * 2 + row.id}, row.active});
    break;
  case 4:
    result.rows.push_back({std::int64_t{row.id}, row.name});
    break;
  default:
    break;
  }
}

FuzzQuery make_table_query(FuzzRng &rng, const std::vector<FuzzRow> &rows) {
  const auto predicate = make_predicate(rng, rows);
  const auto query_kind = static_cast<int>(rng.NextIndex(5));
  const auto delta = rng.Between(-12, 12);

  FuzzQuery query;
  query.sort_rows = true;
  switch (query_kind) {
  case 0:
    query.sql = "SELECT * FROM fuzz WHERE " + predicate.sql + ';';
    query.expected.columns = {"id", "name", "active", "score"};
    break;
  case 1:
    query.sql = "SELECT id AS row_id, " + add_integer_expression("score", delta) +
                " AS shifted FROM fuzz WHERE " + predicate.sql + ';';
    query.expected.columns = {"row_id", "shifted"};
    break;
  case 2:
    query.sql = "SELECT name, active FROM fuzz WHERE " + predicate.sql + ';';
    query.expected.columns = {"name", "active"};
    break;
  case 3:
    query.sql = "SELECT id AS row_id, score * 2 + id AS metric, active = true AS "
                "is_active FROM fuzz WHERE " +
                predicate.sql + ';';
    query.expected.columns = {"row_id", "metric", "is_active"};
    break;
  case 4:
    query.sql = "SELECT fuzz.id AS id, fuzz.name AS name FROM fuzz WHERE " +
                predicate.sql + ';';
    query.expected.columns = {"id", "name"};
    break;
  default:
    break;
  }

  for (const auto &row : rows) {
    if (matches_predicate(row, predicate)) {
      add_expected_row(query.expected, row, query_kind, delta);
    }
  }

  return query;
}

FuzzQuery make_scalar_query(FuzzRng &rng) {
  const auto left = rng.Between(-20, 20);
  const auto right = rng.Between(-20, 20);
  const auto multiplier = rng.Between(-5, 5);

  FuzzQuery query;
  query.sql = "SELECT (" + std::to_string(left) + " + " + std::to_string(right) +
              ") * " + std::to_string(multiplier) + " AS value, NOT false AS truth;";
  query.expected.columns = {"value", "truth"};
  query.expected.rows.push_back({std::int64_t{(left + right) * multiplier}, true});
  return query;
}

[[noreturn]] void fail_fuzz(std::uint64_t seed, const std::string &sql,
                            const std::string &message) {
  std::ostringstream output;
  output << "SQL fuzz seed " << seed << " failed: " << message << "\nSQL:\n" << sql;
  throw std::runtime_error(output.str());
}

void execute_ok(mattsql::DefaultSqlEngine &engine, std::uint64_t seed,
                const std::string &sql) {
  const auto result = engine.Execute(sql);
  if (!result.ok()) {
    fail_fuzz(seed, sql,
              "expected success, got " + StatusName(result.status.code) + ": " +
                  result.status.message);
  }
}

void expect_query(mattsql::DefaultSqlEngine &engine, std::uint64_t seed,
                  const FuzzQuery &query) {
  const auto result = engine.Execute(query.sql);
  if (!result.ok()) {
    fail_fuzz(seed, query.sql,
              "expected query success, got " + StatusName(result.status.code) + ": " +
                  result.status.message);
  }
  if (!result.has_value()) {
    fail_fuzz(seed, query.sql, "query succeeded without a QueryResult");
  }

  auto actual_output = FormatQueryResult(result.Value());
  auto expected_output = FormatQueryResult(query.expected);
  if (query.sort_rows) {
    SortQueryRows(actual_output);
    SortQueryRows(expected_output);
  }

  if (actual_output != expected_output) {
    fail_fuzz(seed, query.sql,
              "query output mismatch\nexpected:\n" + JoinLines(expected_output) +
                  "actual:\n" + JoinLines(actual_output));
  }
}

void run_sql_logic_fuzzer_seed(std::uint64_t seed) {
  constexpr std::size_t row_count = 96;
  constexpr std::size_t query_count = 32;

  FuzzRng rng(seed);
  mattsql::DefaultSqlEngine engine;
  execute_ok(engine, seed,
             "CREATE TABLE fuzz (id INT, name TEXT, active BOOL, score INT);");

  std::vector<FuzzRow> rows;
  rows.reserve(row_count);
  for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
    FuzzRow row;
    row.id = static_cast<int>(row_index + 1U);
    row.name = make_fuzz_name(rng, row_index);
    row.active = rng.Coin();
    row.score = rng.Between(-50, 50);

    const auto insert_sql = "INSERT INTO fuzz VALUES (" + std::to_string(row.id) +
                            ", " + QuoteSqlString(row.name) + ", " +
                            SqlBool(row.active) + ", " + std::to_string(row.score) +
                            ");";
    execute_ok(engine, seed, insert_sql);
    rows.push_back(std::move(row));
  }

  for (std::size_t query_index = 0; query_index < query_count; ++query_index) {
    expect_query(engine, seed, make_table_query(rng, rows));
    expect_query(engine, seed, make_scalar_query(rng));
  }
}

} // namespace

void RunDeterministicFuzzerSeeds(std::uint64_t first_seed, std::size_t count) {
  for (std::uint64_t seed_index = 0; seed_index < count; ++seed_index) {
    run_sql_logic_fuzzer_seed(first_seed + seed_index);
  }
}

} // namespace sql_logic
