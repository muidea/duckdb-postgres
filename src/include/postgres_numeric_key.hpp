#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace duckdb {

// Source numeric_out uses fixed-point text. Normalize only the comparison
// key: returned values retain their original scale. Keys have PostgreSQL's
// ordering -Infinity < finite values < Infinity < NaN, without float casts or
// padding each small value to PostgreSQL's maximum numeric width.
inline std::string PostgresNumericKey(const std::string &text) {
	if (text == "NaN") { return "5"; }
	if (text == "Infinity" || text == "+Infinity") { return "4"; }
	if (text == "-Infinity") { return "0"; }
	const bool negative = !text.empty() && text[0] == '-';
	const size_t start = !text.empty() && (text[0] == '-' || text[0] == '+') ? 1 : 0;
	std::string digits;
	int32_t whole = 0, scale = 0;
	bool dot = false;
	for (size_t i = start; i < text.size(); i++) {
		const auto c = text[i];
		if (c == '.' && !dot) { dot = true; continue; }
		if (c < '0' || c > '9') { throw std::invalid_argument("Invalid exact PostgreSQL numeric"); }
		if (dot) { scale++; } else { whole++; }
		if (whole > 131072 || scale > 16383) {
			throw std::invalid_argument("Exact PostgreSQL numeric exceeds source limits");
		}
		digits += c;
	}
	if (digits.empty()) { throw std::invalid_argument("Invalid exact PostgreSQL numeric"); }
	const auto first = digits.find_first_not_of('0');
	if (first == std::string::npos) { return "2"; }
	const auto last = digits.find_last_not_of('0');
	const int32_t exponent = whole - static_cast<int32_t>(first);
	uint32_t ordered = static_cast<uint32_t>(exponent + 16384);
	if (negative) { ordered = UINT32_MAX - ordered; }
	std::string key(1, negative ? '1' : '3');
	const char *hex = "0123456789abcdef";
	for (int shift = 28; shift >= 0; shift -= 4) { key += hex[(ordered >> shift) & 15]; }
	for (size_t i = first; i <= last; i++) {
		key += negative ? '9' - (digits[i] - '0') : digits[i];
	}
	// A negative prefix reverses the positive prefix ordering, too.
	key += negative ? ':' : '/';
	return key;
}

} // namespace duckdb
