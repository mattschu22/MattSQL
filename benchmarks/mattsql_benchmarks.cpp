#include "mattsql/binder/default_binder.hpp"
#include "mattsql/catalog/in_memory_catalog.hpp"
#include "mattsql/common/result_utils.hpp"
#include "mattsql/lexer/lexer.hpp"
#include "mattsql/optimizer/default_optimizer.hpp"
#include "mattsql/parser/parser.hpp"
#include "mattsql/planner/default_logical_planner.hpp"
#include "mattsql/planner/default_physical_planner.hpp"
#include "mattsql/runtime/hosted_platform_runtime.hpp"
#include "mattsql/sql_engine.hpp"
#include "mattsql/storage/heap/page_heap_table.hpp"
#include "mattsql/storage/page/slotted_page.hpp"
#include "mattsql/storage/tuple_codec.hpp"
#include "mattsql/txn/transaction.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

volatile std::uint64_t benchmark_sink = 0;

void Consume(std::uint64_t value) {
  benchmark_sink ^= value + std::uint64_t{0x9E3779B97F4A7C15};
}

template <typename T> T TakeOk(mattsql::Result<T> result, std::string_view action) {
  if (!result.ok()) {
    std::ostringstream message;
    message << action << " failed with " << mattsql::ErrorCodeName(result.status.code)
            << ": " << result.status.message;
    throw std::runtime_error(message.str());
  }
  if (!result.has_value()) {
    std::ostringstream message;
    message << action << " succeeded without a value";
    throw std::runtime_error(message.str());
  }
  return std::move(result).TakeValue();
}

void RequireOk(const mattsql::Status &status, std::string_view action) {
  if (!status_ok(status)) {
    std::ostringstream message;
    message << action << " failed with " << mattsql::ErrorCodeName(status.code)
            << ": " << status.message;
    throw std::runtime_error(message.str());
  }
}

void ExecuteOk(mattsql::DefaultSqlEngine &engine, std::string_view sql) {
  const auto result = engine.Execute(sql);
  if (!result.ok()) {
    std::ostringstream message;
    message << "SQL failed with " << mattsql::ErrorCodeName(result.status.code)
            << ": " << result.status.message << "\nSQL: " << sql;
    throw std::runtime_error(message.str());
  }
}

mattsql::QueryResult ExecuteQuery(mattsql::DefaultSqlEngine &engine,
                                  std::string_view sql) {
  auto result = engine.Execute(sql);
  if (!result.ok()) {
    std::ostringstream message;
    message << "query failed with " << mattsql::ErrorCodeName(result.status.code)
            << ": " << result.status.message << "\nSQL: " << sql;
    throw std::runtime_error(message.str());
  }
  if (!result.has_value()) {
    std::ostringstream message;
    message << "query succeeded without a result\nSQL: " << sql;
    throw std::runtime_error(message.str());
  }
  return std::move(result).TakeValue();
}

std::string QuoteSqlString(std::string_view value) {
  std::string output = "'";
  for (const char character : value) {
    if (character == '\'') {
      output += "''";
    } else {
      output.push_back(character);
    }
  }
  output.push_back('\'');
  return output;
}

std::string BoolSql(bool value) { return value ? "true" : "false"; }

mattsql::TableSchema FuzzSchema() {
  mattsql::TableSchema schema;
  schema.columns.push_back({"id", mattsql::SqlType::Integer, false});
  schema.columns.push_back({"name", mattsql::SqlType::Text});
  schema.columns.push_back({"active", mattsql::SqlType::Boolean, false});
  schema.columns.push_back({"score", mattsql::SqlType::Integer, false});
  return schema;
}

mattsql::InMemoryCatalog MakeFuzzCatalog() {
  mattsql::CreateTableRequest request;
  request.name = "fuzz";
  request.schema = FuzzSchema();

  mattsql::InMemoryCatalog catalog;
  auto table = TakeOk(catalog.CreateTable(request), "create benchmark catalog table");
  RequireOk(catalog.SetTableHeapRoot(table.id, mattsql::PageId{1}),
            "set benchmark catalog heap root");
  return catalog;
}

std::string FuzzName(std::size_t index) {
  constexpr std::string_view bases[] = {"Ada", "Grace", "Lin", "Turing", "Kay"};
  const auto base_count = sizeof(bases) / sizeof(bases[0]);

  std::string name(bases[index % base_count]);
  name += '_';
  name += std::to_string(index);
  name += '_';
  for (std::size_t suffix = 0; suffix < 18U; ++suffix) {
    name.push_back(static_cast<char>('a' + ((index + suffix * 7U) % 26U)));
  }
  return name;
}

std::string InsertSql(std::size_t row_index) {
  const auto id = row_index + 1U;
  const auto active = (row_index % 3U) != 0U;
  const auto signed_index = static_cast<std::int64_t>(row_index);
  const auto score = (signed_index * 17) % 101 - 50;

  std::ostringstream sql;
  sql << "INSERT INTO fuzz VALUES (" << id << ", "
      << QuoteSqlString(FuzzName(row_index)) << ", " << BoolSql(active) << ", "
      << score << ");";
  return sql.str();
}

std::shared_ptr<mattsql::DefaultSqlEngine> MakeLoadedEngine(std::size_t row_count) {
  auto engine = std::make_shared<mattsql::DefaultSqlEngine>();
  ExecuteOk(*engine, "CREATE TABLE fuzz (id INT, name TEXT, active BOOL, score INT);");
  for (std::size_t row_index = 0; row_index < row_count; ++row_index) {
    ExecuteOk(*engine, InsertSql(row_index));
  }
  return engine;
}

class BenchmarkTransaction final : public mattsql::Transaction {
public:
  mattsql::TransactionId Id() const override { return 1; }
  mattsql::TransactionMode Mode() const override {
    return mattsql::TransactionMode::ReadWrite;
  }
  mattsql::TransactionState State() const override {
    return mattsql::TransactionState::Active;
  }
  mattsql::LogSequenceNumber BeginLsn() const override {
    return mattsql::LogSequenceNumber{0};
  }
};

struct BenchmarkDefinition {
  std::string name;
  std::string description;
  std::size_t iterations = 1;
  std::function<void()> body;
};

struct BenchmarkResult {
  std::string name;
  std::string description;
  std::size_t iterations = 0;
  std::uint64_t min_ns_per_iteration = 0;
  std::uint64_t median_ns_per_iteration = 0;
  std::uint64_t max_ns_per_iteration = 0;
};

struct BaselineEntry {
  std::string name;
  std::uint64_t baseline_ns_per_iteration = 0;
  double max_regression_ratio = 0.0;
};

struct Options {
  std::optional<std::string> filter;
  std::optional<std::string> baseline_path;
  std::optional<std::string> trace_json_path;
  bool quiet = false;
  bool json = false;
  std::optional<std::size_t> iteration_override;
};

std::uint64_t DurationNanos(Clock::time_point start, Clock::time_point end) {
  const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end -
                                                                            start);
  return static_cast<std::uint64_t>(duration.count());
}

std::string JsonEscape(std::string_view value);

class TraceRecorder {
public:
  void Record(std::string_view name, std::string_view category,
              Clock::time_point start, Clock::time_point end) {
    Event event;
    event.name = std::string(name);
    event.category = std::string(category);
    event.start_ns = DurationNanos(origin_, start);
    event.duration_ns = DurationNanos(start, end);
    events_.push_back(std::move(event));
  }

  void WriteJson(const std::string &path) const {
    std::ofstream output(path);
    if (!output.is_open()) {
      throw std::runtime_error("could not open trace output: " + path);
    }

    output << "{\n  \"traceEvents\": [\n";
    for (std::size_t index = 0; index < events_.size(); ++index) {
      const auto &event = events_[index];
      output << "    {"
             << "\"name\":\"" << JsonEscape(event.name) << "\","
             << "\"cat\":\"" << JsonEscape(event.category) << "\","
             << "\"ph\":\"X\","
             << "\"ts\":" << std::fixed << std::setprecision(3)
             << static_cast<double>(event.start_ns) / 1000.0 << ','
             << "\"dur\":" << static_cast<double>(event.duration_ns) / 1000.0
             << ','
             << "\"pid\":1,"
             << "\"tid\":0"
             << '}';
      if (index + 1U != events_.size()) {
        output << ',';
      }
      output << '\n';
    }
    output << "  ],\n  \"displayTimeUnit\": \"ns\"\n}\n";
  }

private:
  struct Event {
    std::string name;
    std::string category;
    std::uint64_t start_ns = 0;
    std::uint64_t duration_ns = 0;
  };

  Clock::time_point origin_ = Clock::now();
  std::vector<Event> events_;
};

TraceRecorder *active_trace = nullptr;

template <typename Callable>
decltype(auto) WithTrace(std::string_view name, std::string_view category,
                         Callable &&callable) {
  if (active_trace == nullptr) {
    return std::forward<Callable>(callable)();
  }

  const auto start = Clock::now();
  if constexpr (std::is_void_v<std::invoke_result_t<Callable>>) {
    std::forward<Callable>(callable)();
    const auto end = Clock::now();
    active_trace->Record(name, category, start, end);
  } else {
    auto result = std::forward<Callable>(callable)();
    const auto end = Clock::now();
    active_trace->Record(name, category, start, end);
    return result;
  }
}

BenchmarkResult RunBenchmark(const BenchmarkDefinition &benchmark,
                             const Options &options) {
  const auto iterations = options.iteration_override.value_or(benchmark.iterations);
  const auto warmups = std::min<std::size_t>(3U, std::max<std::size_t>(1U, iterations / 8U));

  for (std::size_t index = 0; index < warmups; ++index) {
    const auto phase = benchmark.name + ".warmup";
    WithTrace(phase, "benchmark", [&] { benchmark.body(); });
  }

  std::vector<std::uint64_t> samples;
  samples.reserve(iterations);
  for (std::size_t index = 0; index < iterations; ++index) {
    const auto phase = benchmark.name + ".sample";
    const auto start = Clock::now();
    WithTrace(phase, "benchmark", [&] { benchmark.body(); });
    const auto end = Clock::now();
    samples.push_back(DurationNanos(start, end));
  }

  auto sorted = samples;
  std::sort(sorted.begin(), sorted.end());

  BenchmarkResult result;
  result.name = benchmark.name;
  result.description = benchmark.description;
  result.iterations = iterations;
  result.min_ns_per_iteration = sorted.front();
  result.median_ns_per_iteration = sorted[sorted.size() / 2U];
  result.max_ns_per_iteration = sorted.back();
  return result;
}

std::string Trim(std::string_view value) {
  std::size_t first = 0;
  while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
    ++first;
  }

  auto last = value.size();
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1U])) != 0) {
    --last;
  }

  return std::string(value.substr(first, last - first));
}

std::vector<BaselineEntry> LoadBaseline(const std::string &path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    throw std::runtime_error("could not open performance baseline: " + path);
  }

  std::vector<BaselineEntry> entries;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
      continue;
    }

    std::istringstream stream(trimmed);
    BaselineEntry entry;
    if (!(stream >> entry.name >> entry.baseline_ns_per_iteration >>
          entry.max_regression_ratio)) {
      std::ostringstream message;
      message << path << ':' << line_number
              << ": expected `<name> <baseline_ns> <max_regression_ratio>`";
      throw std::runtime_error(message.str());
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

const BaselineEntry *FindBaseline(const std::vector<BaselineEntry> &entries,
                                  const std::string &name) {
  for (const auto &entry : entries) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

const BenchmarkResult *FindResult(const std::vector<BenchmarkResult> &results,
                                  const std::string &name) {
  for (const auto &result : results) {
    if (result.name == name) {
      return &result;
    }
  }
  return nullptr;
}

bool CompareToBaseline(const std::vector<BenchmarkResult> &results,
                       const std::vector<BaselineEntry> &baseline,
                       bool require_all_baselines, bool quiet) {
  bool passed = true;

  for (const auto &result : results) {
    const auto *entry = FindBaseline(baseline, result.name);
    if (entry == nullptr) {
      std::cerr << "[PERF][FAIL] missing baseline for " << result.name << '\n';
      passed = false;
      continue;
    }

    const auto ratio = static_cast<double>(result.median_ns_per_iteration) /
                       static_cast<double>(entry->baseline_ns_per_iteration);
    if (!quiet) {
      std::cout << "[PERF] " << result.name << " median_ns="
                << result.median_ns_per_iteration << " baseline_ns="
                << entry->baseline_ns_per_iteration << " ratio=" << std::fixed
                << std::setprecision(2) << ratio << " max="
                << entry->max_regression_ratio << '\n';
    }

    if (ratio > entry->max_regression_ratio) {
      std::cerr << "[PERF][FAIL] " << result.name << " is " << std::fixed
                << std::setprecision(2) << ratio
                << "x slower than baseline; allowed "
                << entry->max_regression_ratio << "x\n";
      passed = false;
    }
  }

  if (require_all_baselines) {
    for (const auto &entry : baseline) {
      if (FindResult(results, entry.name) == nullptr) {
        std::cerr << "[PERF][FAIL] baseline benchmark did not run: " << entry.name
                  << '\n';
        passed = false;
      }
    }
  }

  return passed;
}

std::string JsonEscape(std::string_view value) {
  std::string output;
  output.reserve(value.size());
  for (const char character : value) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      output.push_back(character);
      break;
    }
  }
  return output;
}

void PrintJson(const std::vector<BenchmarkResult> &results) {
  std::cout << "{\n  \"version\": 1,\n  \"benchmarks\": [\n";
  for (std::size_t index = 0; index < results.size(); ++index) {
    const auto &result = results[index];
    std::cout << "    {\n"
              << "      \"name\": \"" << JsonEscape(result.name) << "\",\n"
              << "      \"description\": \"" << JsonEscape(result.description)
              << "\",\n"
              << "      \"iterations\": " << result.iterations << ",\n"
              << "      \"min_ns_per_iteration\": " << result.min_ns_per_iteration
              << ",\n"
              << "      \"median_ns_per_iteration\": "
              << result.median_ns_per_iteration << ",\n"
              << "      \"max_ns_per_iteration\": " << result.max_ns_per_iteration
              << "\n"
              << "    }";
    if (index + 1U != results.size()) {
      std::cout << ',';
    }
    std::cout << '\n';
  }
  std::cout << "  ]\n}\n";
}

void PrintTextResult(const BenchmarkResult &result) {
  std::cout << std::left << std::setw(42) << result.name << std::right
            << " median=" << std::setw(10) << result.median_ns_per_iteration
            << " ns"
            << " min=" << std::setw(10) << result.min_ns_per_iteration << " ns"
            << " max=" << std::setw(10) << result.max_ns_per_iteration << " ns"
            << " iterations=" << result.iterations << '\n';
}

std::vector<std::string> PipelineSqlCorpus() {
  return {
      "SELECT (1 + 2) * 3 AS value, NOT false AS truth;",
      "SELECT id, name, active, score FROM fuzz WHERE active = true;",
      "SELECT id AS row_id, score * 2 + id AS metric FROM fuzz WHERE score >= 10;",
      "SELECT fuzz.id AS id, fuzz.name AS name FROM fuzz WHERE "
      "(score >= -5 AND score <= 25) OR name = 'Ada_0_ahovcjqxelszgnubip';",
      "INSERT INTO fuzz VALUES (1001, 'profiled', true, 42);",
  };
}

BenchmarkDefinition MakeParserBenchmark() {
  const auto corpus = std::make_shared<std::vector<std::string>>(PipelineSqlCorpus());
  return BenchmarkDefinition{
      "frontend_parse_corpus",
      "lexes and parses representative scalar, table, predicate, and insert SQL",
      256U,
      [corpus] {
        std::uint64_t parsed = 0;
        for (const auto &sql : *corpus) {
          mattsql::Lexer lexer(sql);
          auto tokens = WithTrace("lexer_tokenize", "frontend",
                                  [&] { return lexer.Tokenize(); });
          mattsql::Parser parser(std::move(tokens));
          auto statement = WithTrace("parse_statement", "frontend",
                                     [&] { return parser.ParseStatement(); });
          Consume(reinterpret_cast<std::uintptr_t>(statement.get()));
          ++parsed;
        }
        Consume(parsed);
      }};
}

BenchmarkDefinition MakeFrontendPipelineBenchmark() {
  const auto corpus = std::make_shared<std::vector<std::string>>(PipelineSqlCorpus());
  const auto catalog = std::make_shared<mattsql::InMemoryCatalog>(MakeFuzzCatalog());
  return BenchmarkDefinition{
      "frontend_bind_plan_optimize_corpus",
      "runs parse, bind, logical plan, optimizer, and physical plan over the SQL corpus",
      160U,
      [corpus, catalog] {
        std::uint64_t planned = 0;
        for (const auto &sql : *corpus) {
          auto statement = WithTrace("parse_statement", "frontend", [&] {
            mattsql::Lexer lexer(sql);
            mattsql::Parser parser(lexer.Tokenize());
            return parser.ParseStatement();
          });

          mattsql::DefaultBinder binder;
          auto bound = WithTrace("bind_statement", "frontend", [&] {
            return TakeOk(binder.Bind(*statement, *catalog), "bind benchmark SQL");
          });

          mattsql::DefaultLogicalPlanner logical_planner;
          auto logical = WithTrace("logical_plan", "frontend", [&] {
            return TakeOk(logical_planner.Plan(*bound),
                          "logical plan benchmark SQL");
          });

          mattsql::DefaultOptimizer optimizer;
          auto optimized = WithTrace("optimize_logical_plan", "frontend", [&] {
            return TakeOk(optimizer.Optimize(std::move(logical)),
                          "optimize benchmark SQL");
          });

          mattsql::DefaultPhysicalPlanner physical_planner;
          auto physical = WithTrace("physical_plan", "frontend", [&] {
            return TakeOk(physical_planner.Plan(*optimized),
                          "physical plan benchmark SQL");
          });

          Consume(static_cast<std::uint64_t>(physical->Kind()));
          ++planned;
        }
        Consume(planned);
      }};
}

BenchmarkDefinition MakeEngineSelectBenchmark() {
  const auto engine = MakeLoadedEngine(1024U);
  const auto queries = std::make_shared<std::vector<std::string>>(std::vector<std::string>{
      "SELECT id, name, active, score FROM fuzz WHERE active = true;",
      "SELECT id AS row_id, score * 2 + id AS metric, active = true AS is_active "
      "FROM fuzz WHERE score >= 10;",
      "SELECT id, name FROM fuzz WHERE score >= 999;",
      "SELECT id, name, score FROM fuzz WHERE id <> 512;",
  });

  return BenchmarkDefinition{
      "engine_select_scan_projection_1024",
      "executes read-only scans, filters, and projections against a 1024-row table",
      64U,
      [engine, queries] {
        std::uint64_t rows = 0;
        for (const auto &sql : *queries) {
          auto result = WithTrace("execute_select_query", "engine", [&] {
            return ExecuteQuery(*engine, sql);
          });
          rows += result.rows.size();
          rows += result.columns.size();
        }
        Consume(rows);
      }};
}

BenchmarkDefinition MakeEngineInsertBenchmark() {
  auto inserts = std::make_shared<std::vector<std::string>>();
  inserts->reserve(128U);
  for (std::size_t row_index = 0; row_index < 128U; ++row_index) {
    inserts->push_back(InsertSql(row_index));
  }

  return BenchmarkDefinition{
      "engine_insert_values_128",
      "creates a table and inserts 128 rows through the SQL engine",
      12U,
      [inserts] {
        mattsql::DefaultSqlEngine engine;
        WithTrace("create_insert_table", "engine", [&] {
          ExecuteOk(engine,
                    "CREATE TABLE fuzz (id INT, name TEXT, active BOOL, score INT);");
        });
        WithTrace("insert_values_rows", "engine", [&] {
          for (const auto &sql : *inserts) {
            ExecuteOk(engine, sql);
          }
        });
        Consume(inserts->size());
      }};
}

BenchmarkDefinition MakeTupleCodecBenchmark() {
  const auto schema = std::make_shared<mattsql::TableSchema>(FuzzSchema());
  auto rows = std::make_shared<std::vector<std::vector<mattsql::Value>>>();
  rows->reserve(256U);
  for (std::size_t row_index = 0; row_index < 256U; ++row_index) {
    const auto signed_index = static_cast<std::int64_t>(row_index);
    rows->push_back({signed_index + 1, FuzzName(row_index), (row_index % 2U) == 0U,
                     (signed_index * 13) % 97 - 48});
  }

  return BenchmarkDefinition{
      "tuple_codec_encode_decode_256",
      "encodes and decodes 256 mixed integer/text/boolean tuples",
      96U,
      [schema, rows] {
        mattsql::BinaryTupleCodec codec;
        std::uint64_t value_count = 0;
        WithTrace("encode_decode_tuples", "storage", [&] {
          for (const auto &row : *rows) {
            auto tuple = TakeOk(codec.Encode(*schema, row), "encode benchmark tuple");
            auto decoded = TakeOk(
                codec.Decode(*schema,
                             mattsql::ConstBufferView{std::span<const std::byte>(
                                 tuple.bytes.data(), tuple.bytes.size())}),
                "decode benchmark tuple");
            value_count += decoded.size();
          }
        });
        Consume(value_count);
      }};
}

BenchmarkDefinition MakeSlottedPageBenchmark() {
  auto records = std::make_shared<std::vector<std::vector<std::byte>>>();
  records->reserve(80U);
  for (std::size_t record_index = 0; record_index < 80U; ++record_index) {
    std::vector<std::byte> record(32U);
    for (std::size_t byte_index = 0; byte_index < record.size(); ++byte_index) {
      record[byte_index] =
          static_cast<std::byte>((record_index * 31U + byte_index) % 251U);
    }
    records->push_back(std::move(record));
  }

  return BenchmarkDefinition{
      "slotted_page_insert_read_delete_80",
      "initializes one page, inserts 80 small records, reads them, and deletes every fourth",
      128U,
      [records] {
        mattsql::PageHeader header;
        header.page_id = mattsql::PageId{7};
        std::vector<std::byte> page_bytes(mattsql::kDefaultPageSize);
        mattsql::DefaultSlottedPage page;
        WithTrace("initialize_slotted_page", "storage", [&] {
          RequireOk(page.Initialize(
                        mattsql::PageView{
                            &header,
                            mattsql::BufferView{std::span<std::byte>(
                                page_bytes.data(), page_bytes.size())}},
                        mattsql::PageKind::Heap),
                    "initialize slotted page");
        });

        std::vector<mattsql::SlotId> slots;
        slots.reserve(records->size());
        WithTrace("slotted_page_insert_records", "storage", [&] {
          for (const auto &record : *records) {
            auto slot = TakeOk(
                page.Insert(mattsql::PageView{
                                &header,
                                mattsql::BufferView{std::span<std::byte>(
                                    page_bytes.data(), page_bytes.size())}},
                            mattsql::ConstBufferView{std::span<const std::byte>(
                                record.data(), record.size())}),
                "insert slotted-page record");
            slots.push_back(slot);
          }
        });

        std::uint64_t bytes_read = 0;
        WithTrace("slotted_page_read_records", "storage", [&] {
          for (const auto slot : slots) {
            auto record = TakeOk(
                page.Read(mattsql::ConstPageView{
                              &header,
                              mattsql::ConstBufferView{std::span<const std::byte>(
                                  page_bytes.data(), page_bytes.size())}},
                          slot),
                "read slotted-page record");
            bytes_read += record.bytes.bytes.size();
          }
        });

        WithTrace("slotted_page_delete_records", "storage", [&] {
          for (std::size_t index = 0; index < slots.size(); index += 4U) {
            RequireOk(page.Delete(
                          mattsql::PageView{
                              &header,
                              mattsql::BufferView{std::span<std::byte>(
                                  page_bytes.data(), page_bytes.size())}},
                          slots[index]),
                      "delete slotted-page record");
          }
        });

        Consume(bytes_read + page.FreeSpace(mattsql::ConstPageView{
                                 &header,
                                 mattsql::ConstBufferView{std::span<const std::byte>(
                                     page_bytes.data(), page_bytes.size())}}));
      }};
}

BenchmarkDefinition MakePageHeapBenchmark() {
  auto records = std::make_shared<std::vector<std::vector<std::byte>>>();
  records->reserve(256U);
  for (std::size_t record_index = 0; record_index < 256U; ++record_index) {
    std::vector<std::byte> record(48U);
    for (std::size_t byte_index = 0; byte_index < record.size(); ++byte_index) {
      record[byte_index] =
          static_cast<std::byte>((record_index * 17U + byte_index * 3U) % 253U);
    }
    records->push_back(std::move(record));
  }

  return BenchmarkDefinition{
      "page_heap_insert_scan_256",
      "inserts 256 serialized records into the page heap and scans the heap cursor",
      48U,
      [records] {
        BenchmarkTransaction transaction;
        mattsql::PageHeapTable heap(mattsql::PageId{10});

        WithTrace("page_heap_insert_records", "storage", [&] {
          for (const auto &record : *records) {
            (void)TakeOk(heap.Insert(transaction,
                                     mattsql::ConstBufferView{
                                         std::span<const std::byte>(record.data(),
                                                                    record.size())}),
                         "insert heap record");
          }
        });

        std::uint64_t scanned_bytes = 0;
        WithTrace("page_heap_scan_records", "storage", [&] {
          auto cursor = TakeOk(heap.Scan(transaction), "open heap scan");
          while (true) {
            auto record = cursor->Next();
            if (!record.ok()) {
              if (record.status.code == mattsql::ErrorCode::NotFound) {
                break;
              }
              (void)TakeOk(std::move(record), "scan heap record");
            }
            scanned_bytes += record.Value().bytes.bytes.size();
          }
        });
        Consume(scanned_bytes + heap.PageCount() + heap.RecordCount());
      }};
}

BenchmarkDefinition MakeRuntimeBenchmark() {
  return BenchmarkDefinition{
      "runtime_page_alloc_time_64",
      "allocates/frees hosted runtime page spans and reads monotonic time 64 times",
      192U,
      [] {
        mattsql::HostedPlatformRuntime runtime;
        std::uint64_t nanos = 0;
        WithTrace("runtime_allocate_pages_and_time", "runtime", [&] {
          for (std::size_t index = 0; index < 64U; ++index) {
            auto allocation = TakeOk(
                runtime.AllocatePageSpan(1U, mattsql::kDefaultPageSize,
                                         mattsql::kRuntimeMemoryZeroed),
                "allocate hosted runtime page span");
            nanos ^= runtime.MonotonicNanos();
            Consume(reinterpret_cast<std::uintptr_t>(allocation.data()));
          }
        });
        Consume(nanos);
      }};
}

BenchmarkDefinition MakeSqlLogicCorpusBenchmark() {
  auto queries = std::make_shared<std::vector<std::string>>(std::vector<std::string>{
      "SELECT id, name, active, score FROM fuzz WHERE active = true;",
      "SELECT id AS row_id, score + 7 AS shifted FROM fuzz WHERE score >= -10;",
      "SELECT name, active FROM fuzz WHERE name = 'Ada_0_ahovcjqxelszgnubip';",
      "SELECT id AS row_id, score * 2 + id AS metric, active = true AS is_active "
      "FROM fuzz WHERE active = false OR id = 17;",
      "SELECT fuzz.id AS id, fuzz.name AS name FROM fuzz WHERE "
      "(score >= -5 AND score <= 25) OR name = 'Turing_3_dkryfmtahovcjqxels';",
  });

  return BenchmarkDefinition{
      "sql_logic_corpus_96x20",
      "runs a deterministic end-to-end SQL logic-style workload over 96 inserted rows",
      20U,
      [queries] {
        mattsql::DefaultSqlEngine engine;
        WithTrace("sql_logic_setup_table", "sql_logic", [&] {
          ExecuteOk(engine,
                    "CREATE TABLE fuzz (id INT, name TEXT, active BOOL, score INT);");
        });
        WithTrace("sql_logic_insert_rows", "sql_logic", [&] {
          for (std::size_t row_index = 0; row_index < 96U; ++row_index) {
            ExecuteOk(engine, InsertSql(row_index));
          }
        });

        std::uint64_t row_total = 0;
        WithTrace("sql_logic_query_corpus", "sql_logic", [&] {
          for (std::size_t repeat = 0; repeat < 4U; ++repeat) {
            for (const auto &query : *queries) {
              auto result = ExecuteQuery(engine, query);
              row_total += result.rows.size();
            }
          }
        });
        Consume(row_total);
      }};
}

std::vector<BenchmarkDefinition> MakeBenchmarks() {
  std::vector<BenchmarkDefinition> benchmarks;
  benchmarks.push_back(MakeParserBenchmark());
  benchmarks.push_back(MakeFrontendPipelineBenchmark());
  benchmarks.push_back(MakeEngineSelectBenchmark());
  benchmarks.push_back(MakeEngineInsertBenchmark());
  benchmarks.push_back(MakeTupleCodecBenchmark());
  benchmarks.push_back(MakeSlottedPageBenchmark());
  benchmarks.push_back(MakePageHeapBenchmark());
  benchmarks.push_back(MakeRuntimeBenchmark());
  benchmarks.push_back(MakeSqlLogicCorpusBenchmark());
  return benchmarks;
}

bool MatchesFilter(const BenchmarkDefinition &benchmark,
                   const std::optional<std::string> &filter) {
  if (!filter.has_value()) {
    return true;
  }
  return benchmark.name.find(*filter) != std::string::npos;
}

Options ParseOptions(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--quiet") {
      options.quiet = true;
    } else if (argument == "--json") {
      options.json = true;
    } else if (argument == "--filter" || argument == "--benchmark_filter") {
      if (index + 1 >= argc) {
        throw std::runtime_error(argument + " requires a value");
      }
      options.filter = std::string(argv[++index]);
    } else if (argument.rfind("--filter=", 0) == 0) {
      options.filter = argument.substr(std::string("--filter=").size());
    } else if (argument.rfind("--benchmark_filter=", 0) == 0) {
      options.filter = argument.substr(std::string("--benchmark_filter=").size());
    } else if (argument == "--check-baseline") {
      if (index + 1 >= argc) {
        throw std::runtime_error("--check-baseline requires a path");
      }
      options.baseline_path = std::string(argv[++index]);
    } else if (argument.rfind("--check-baseline=", 0) == 0) {
      options.baseline_path =
          argument.substr(std::string("--check-baseline=").size());
    } else if (argument == "--trace-json") {
      if (index + 1 >= argc) {
        throw std::runtime_error("--trace-json requires a path");
      }
      options.trace_json_path = std::string(argv[++index]);
    } else if (argument.rfind("--trace-json=", 0) == 0) {
      options.trace_json_path = argument.substr(std::string("--trace-json=").size());
    } else if (argument == "--iterations") {
      if (index + 1 >= argc) {
        throw std::runtime_error("--iterations requires a positive integer");
      }
      options.iteration_override =
          static_cast<std::size_t>(std::stoull(argv[++index]));
    } else if (argument.rfind("--iterations=", 0) == 0) {
      options.iteration_override = static_cast<std::size_t>(
          std::stoull(argument.substr(std::string("--iterations=").size())));
    } else if (argument == "--help" || argument == "-h") {
      std::cout << "Usage: mattsql_benchmarks [options]\n"
                << "  --filter NAME              Run benchmarks containing NAME\n"
                << "  --benchmark_filter NAME    Alias for --filter\n"
                << "  --iterations N             Override per-benchmark iteration counts\n"
                << "  --check-baseline PATH      Compare current results to baseline TSV\n"
                << "  --trace-json PATH          Write Chrome/Perfetto trace JSON\n"
                << "  --json                     Emit JSON results\n"
                << "  --quiet                    Suppress text result lines\n";
      std::exit(0);
    } else {
      throw std::runtime_error("unknown benchmark option: " + argument);
    }
  }

  if (options.iteration_override.has_value() && *options.iteration_override == 0U) {
    throw std::runtime_error("--iterations must be positive");
  }

  return options;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto options = ParseOptions(argc, argv);
    const auto benchmarks = MakeBenchmarks();
    std::optional<TraceRecorder> trace;
    if (options.trace_json_path.has_value()) {
      trace.emplace();
      active_trace = &*trace;
    }

    std::vector<BenchmarkResult> results;
    for (const auto &benchmark : benchmarks) {
      if (!MatchesFilter(benchmark, options.filter)) {
        continue;
      }

      auto result = RunBenchmark(benchmark, options);
      if (!options.quiet && !options.json) {
        PrintTextResult(result);
      }
      results.push_back(std::move(result));
    }

    if (results.empty()) {
      std::cerr << "no benchmarks matched the requested filter\n";
      return 1;
    }

    bool passed = true;
    if (options.baseline_path.has_value()) {
      const auto baseline = LoadBaseline(*options.baseline_path);
      passed = CompareToBaseline(results, baseline, !options.filter.has_value(),
                                 options.quiet && !options.json);
    }

    if (options.json) {
      PrintJson(results);
    }

    if (options.trace_json_path.has_value()) {
      active_trace = nullptr;
      trace->WriteJson(*options.trace_json_path);
    }

    Consume(benchmark_sink);
    return passed ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << "benchmark error: " << error.what() << '\n';
    return 1;
  }
}
