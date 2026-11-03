#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace duckdb {

// Decode numeric_send without float, fixed DECIMAL capacity or exponent
// expansion beyond PostgreSQL's own limits. Preserve the source dscale.
inline std::string PostgresNumericText(const char *bytes, size_t length) {
	if (length < 8) { throw std::invalid_argument("Short PostgreSQL numeric value"); }
	auto word = [&](size_t offset) -> uint16_t {
		return (uint16_t(static_cast<unsigned char>(bytes[offset])) << 8) |
		       static_cast<unsigned char>(bytes[offset + 1]);
	};
	const auto count = word(0);
	const int32_t weight = static_cast<int16_t>(word(2));
	const auto sign = word(4), scale = word(6);
	if (length != 8 + size_t(count) * 2 || scale > 16383) {
		throw std::invalid_argument("Invalid PostgreSQL numeric length or scale");
	}
	if (sign == 0xc000 || sign == 0xd000 || sign == 0xf000) {
		if (count != 0) { throw std::invalid_argument("Invalid PostgreSQL special numeric"); }
		return sign == 0xc000 ? "NaN" : sign == 0xd000 ? "Infinity" : "-Infinity";
	}
	if (sign != 0 && sign != 0x4000) { throw std::invalid_argument("Invalid PostgreSQL numeric sign"); }
	bool nonzero = false;
	for (size_t i = 0; i < count; i++) {
		const auto digit = word(8 + i * 2);
		if (digit > 9999) { throw std::invalid_argument("Invalid PostgreSQL numeric digit"); }
		nonzero = nonzero || digit != 0;
		// Never silently truncate nonzero digits beyond the declared scale.
		const int32_t allowed = int32_t(scale) + (weight - int32_t(i) + 1) * 4;
		if (allowed < 4) {
			uint16_t divisor = 1;
			for (int j = 0; j < 4 - (allowed < 0 ? 0 : allowed); j++) { divisor *= 10; }
			if (digit % divisor != 0) { throw std::invalid_argument("PostgreSQL numeric exceeds its scale"); }
		}
	}
	auto group = [&](int32_t exponent) -> uint16_t {
		const auto index = weight - exponent;
		return index >= 0 && index < count ? word(8 + size_t(index) * 2) : 0;
	};
	auto padded = [](uint16_t digit) {
		auto result = std::to_string(digit);
		return std::string(4 - result.size(), '0') + result;
	};
	std::string result;
	if (sign == 0x4000 && nonzero) { result += '-'; }
	if (weight < 0 || !nonzero) {
		result += '0';
	} else {
		result += std::to_string(group(weight));
		for (int32_t exponent = weight - 1; exponent >= 0; exponent--) { result += padded(group(exponent)); }
	}
	if (scale > 0) {
		result += '.';
		for (int32_t offset = 0; offset < scale; offset += 4) {
			const auto part = padded(group(-1 - offset / 4));
			result.append(part, 0, scale - offset < 4 ? scale - offset : 4);
		}
	}
	return result;
}

} // namespace duckdb
