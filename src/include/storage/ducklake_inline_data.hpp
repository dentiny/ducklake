//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_inline_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/index_vector.hpp"
#include "storage/ducklake_stats.hpp"
#include "common/ducklake_data_file.hpp"

namespace duckdb {
class DuckLakeInsert;

class DuckLakeInlineData : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

public:
	DuckLakeInlineData(PhysicalPlan &physical_plan, PhysicalOperator &child, idx_t inline_row_limit);

	idx_t inline_row_limit;
	optional_ptr<DuckLakeInsert> insert;
	//! NOT NULL nested columns: (physical column index, column name) pairs
	vector<pair<idx_t, string>> not_null_nested_columns;
	//! Table name for error messages
	string table_name;

public:
	unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const override;
	unique_ptr<GlobalOperatorState> GetGlobalOperatorState(ClientContext &context) const override;
	void VerifyNestedNotNull(DataChunk &chunk) const;
	OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                           GlobalOperatorState &gstate, OperatorState &state) const override;
	OperatorFinalizeResultType FinalExecute(ExecutionContext &context, DataChunk &chunk, GlobalOperatorState &gstate,
	                                        OperatorState &state) const override;
	OperatorFinalResultType OperatorFinalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                                         OperatorFinalizeInput &input) const override;

	bool RequiresFinalExecute() const override {
		return true;
	}
	bool RequiresOperatorFinalize() const override {
		return true;
	}
	bool ParallelOperator() const override {
		return true;
	}
	string GetName() const override;
};

} // namespace duckdb
