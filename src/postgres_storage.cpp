#include "duckdb.hpp"

#include "postgres_storage.hpp"
#include "storage/postgres_catalog.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "storage/postgres_transaction_manager.hpp"

namespace duckdb {

static unique_ptr<Catalog> PostgresAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                          AttachedDatabase &db, const string &name, AttachInfo &info,
                                          AttachOptions &attach_options) {
	auto &config = DBConfig::GetConfig(context);
	if (!Settings::Get<EnableExternalAccessSetting>(config)) {
		throw PermissionException("Attaching Postgres databases is disabled through configuration");
	}
	string attach_path = info.path;

	string secret_name;
	string schema_to_load;
	PostgresIsolationLevel isolation_level = PostgresIsolationLevel::REPEATABLE_READ;
	bool use_text_protocol = false;
	bool use_cursor = true;
	for (auto &entry : attach_options.options) {
		auto lower_name = StringUtil::Lower(entry.first);
		if (lower_name == "secret") {
			secret_name = entry.second.ToString();
		} else if (lower_name == "schema") {
			schema_to_load = entry.second.ToString();
		} else if (lower_name == "isolation_level") {
			auto param = entry.second.ToString();
			auto lparam = StringUtil::Lower(param);
			if (lparam == "read committed") {
				isolation_level = PostgresIsolationLevel::READ_COMMITTED;
			} else if (lparam == "repeatable read") {
				isolation_level = PostgresIsolationLevel::REPEATABLE_READ;
			} else if (lparam == "serializable") {
				isolation_level = PostgresIsolationLevel::SERIALIZABLE;
			} else {
				throw InvalidInputException("Invalid value \"%s\" for isolation_level, expected READ COMMITTED, "
				                            "REPEATABLE READ or SERIALIZABLE",
				                            param);
			}
		} else if (lower_name == "use_text_protocol") {
			use_text_protocol = BooleanValue::Get(entry.second);
		} else if (lower_name == "use_cursor") {
			use_cursor = BooleanValue::Get(entry.second);
		} else {
			throw BinderException("Unrecognized option for Postgres attach: %s", entry.first);
		}
	}
	auto connection_string = PostgresCatalog::GetConnectionString(context, attach_path, secret_name);
	return make_uniq<PostgresCatalog>(db, std::move(connection_string), std::move(attach_path),
	                                  attach_options.access_mode, std::move(schema_to_load), isolation_level,
	                                  use_text_protocol, use_cursor);
}

static unique_ptr<TransactionManager> PostgresCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                       AttachedDatabase &db, Catalog &catalog) {
	auto &postgres_catalog = catalog.Cast<PostgresCatalog>();
	return make_uniq<PostgresTransactionManager>(db, postgres_catalog);
}

PostgresStorageExtension::PostgresStorageExtension() {
	attach = PostgresAttach;
	create_transaction_manager = PostgresCreateTransactionManager;
}

} // namespace duckdb
