//===----------------------------------------------------------------------===//
//                         DuckDB
//
// postgres_text_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "postgres_result_reader.hpp"
#include "postgres_connection.hpp"
#include "postgres_result.hpp"

namespace duckdb {

struct PostgresTextReader : public PostgresResultReader {
	static constexpr idx_t CURSOR_FETCH_SIZE = 10000;

	explicit PostgresTextReader(ClientContext &context, PostgresConnection &con, const vector<column_t> &column_ids,
	                            const PostgresBindData &bind_data);
	~PostgresTextReader() override;

public:
	void BeginCopy(const string &sql) override;
	PostgresReadResult Read(DataChunk &result) override;

private:
	void Reset();
	void CloseCursor();
	void FetchNextBatch();
	void ConvertVector(Vector &source, Vector &target, const PostgresType &postgres_type, idx_t count);
	void ConvertList(Vector &source, Vector &target, const PostgresType &postgres_type, idx_t count);
	void ConvertStruct(Vector &source, Vector &target, const PostgresType &postgres_type, idx_t count);
	void ConvertCTID(Vector &source, Vector &target, idx_t count);
	void ConvertBlob(Vector &source, Vector &target, idx_t count);

private:
	ClientContext &context;
	DataChunk scan_chunk;
	unique_ptr<PostgresResult> result;
	idx_t row_offset = 0;
	bool use_cursor = false;
	bool cursor_open = false;
	string cursor_name;
};

} // namespace duckdb
