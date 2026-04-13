#include "catch.hpp"
#include "duckdb.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

TEST_CASE("ScanDuckFileInt64Field", "[storage][persistence]") {
	// Compression on write is selected automatically by DuckDB default strategy.
	// This test verifies that values can be restored in-order when scanning compressed data through storage-layer APIs.
	const auto db_path = TestCreatePath("scan_duck_file_int64_field.duckdb");

	vector<int64_t> expected_values;
	expected_values.reserve(5000);
	for (idx_t i = 0; i < 3000; i++) {
		expected_values.push_back(7777777);
	}
	for (idx_t i = 0; i < 1500; i++) {
		expected_values.push_back(100 + UnsafeNumericCast<int64_t>(i % 9));
	}
	for (idx_t i = 0; i < 500; i++) {
		if (i % 4 == 0) {
			expected_values.push_back(NumericLimits<int64_t>::Maximum() - UnsafeNumericCast<int64_t>(i));
		} else if (i % 4 == 1) {
			expected_values.push_back(NumericLimits<int64_t>::Minimum() + UnsafeNumericCast<int64_t>(i));
		} else if (i % 4 == 2) {
			expected_values.push_back(1LL << 40);
		} else {
			expected_values.push_back(-(1LL << 40));
		}
	}

	{
		DuckDB db(db_path);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE scan_duck_file_int64_field(field0 BIGINT)"));

		Appender appender(con, "scan_duck_file_int64_field");
		for (auto value : expected_values) {
			appender.Append<int64_t>(value);
			appender.EndRow();
		}
		appender.Close();
	}

	vector<int64_t> actual_values;
	{
		DuckDB db(db_path);
		Connection con(db);
		auto &context = *con.context;
		auto &catalog = Catalog::GetSystemCatalog(context);
		auto &table = catalog.GetEntry<TableCatalogEntry>(context, DEFAULT_SCHEMA, "scan_duck_file_int64_field")
		                  .Cast<DuckTableEntry>();
		auto &columns = table.GetColumns();
		REQUIRE(columns.LogicalColumnCount() == 1);
		REQUIRE(columns.GetColumn(LogicalIndex(0)).Name() == "field0");

		auto &storage = table.GetStorage();
		auto &transaction = DuckTransaction::Get(context, table.ParentCatalog());
		TableScanState table_scan_state;
		vector<StorageIndex> column_ids {StorageIndex(0)};
		storage.InitializeScan(context, transaction, table_scan_state, column_ids);

		auto &scan_state = table_scan_state.table_state;
		DataChunk result_chunk;
		result_chunk.Initialize(Allocator::DefaultAllocator(), vector<LogicalType> {LogicalType::BIGINT});
		while (true) {
			result_chunk.Reset();
			scan_state.Scan(transaction, result_chunk);
			if (result_chunk.size() == 0) {
				break;
			}
			auto &result = result_chunk.data[0];
			UnifiedVectorFormat vector_data;
			result.ToUnifiedFormat(result_chunk.size(), vector_data);
			auto values = UnifiedVectorFormat::GetData<int64_t>(vector_data);
			for (idx_t i = 0; i < result_chunk.size(); i++) {
				auto row_idx = vector_data.sel->get_index(i);
				REQUIRE(vector_data.validity.RowIsValid(row_idx));
				actual_values.push_back(values[row_idx]);
			}
		}

		REQUIRE(actual_values.size() == storage.GetTotalRows());
	}

	REQUIRE(actual_values.size() == expected_values.size());
	for (idx_t i = 0; i < expected_values.size(); i++) {
		REQUIRE(actual_values[i] == expected_values[i]);
	}
}
