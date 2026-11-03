#include "postgres_numeric_key.hpp"
#include <iostream>
#include <vector>

int main() {
	using duckdb::PostgresNumericKey;
	const std::vector<std::string> ordered = {
	    "-Infinity", "-10000000000000000000000000000000000000000.0001", "-10", "-2", "-1.201", "-1.2",
	    "-0.00000000000000000000000000000000000000001", "0", "0.00000000000000000000000000000000000000001",
	    "1.2", "1.201", "1.234567890123456789", "2", "10",
	    "10000000000000000000000000000000000000000.0001", "Infinity", "NaN"};
	for (size_t i = 1; i < ordered.size(); i++) {
		if (!(PostgresNumericKey(ordered[i - 1]) < PostgresNumericKey(ordered[i]))) {
			std::cerr << "Incorrect numeric order: " << ordered[i - 1] << " / " << ordered[i] << '\n';
			return 1;
		}
	}
	for (const auto &text : {"1", "1.0", "01.000", "+1.0000"}) {
		if (PostgresNumericKey(text) != PostgresNumericKey("1")) { return 2; }
	}
	for (const auto &text : {"0", "-0.000", "+000.0"}) {
		if (PostgresNumericKey(text) != PostgresNumericKey("0")) { return 3; }
	}
	const auto large = std::string(131072, '9');
	const auto tiny = "0." + std::string(16382, '0') + "1";
	if (!(PostgresNumericKey(large) > PostgresNumericKey("10")) ||
	    !(PostgresNumericKey(tiny) < PostgresNumericKey("0.01"))) { return 4; }
	for (const auto &text : std::vector<std::string>{"", "+", ".", "1.2.3", "no", "1e2", "1 "}) {
		try { PostgresNumericKey(text); return 5; } catch (const std::invalid_argument &) { }
	}
	std::cout << "PostgreSQL exact numeric keys passed\n";
}
