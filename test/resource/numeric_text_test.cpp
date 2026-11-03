#include "postgres_numeric_text.hpp"
#include <iostream>
#include <vector>

static std::string Pack(int16_t weight, uint16_t sign, uint16_t scale, std::vector<uint16_t> digits) {
	std::string bytes;
	auto append = [&](uint16_t word) { bytes += char(word >> 8); bytes += char(word & 255); };
	append(uint16_t(digits.size())); append(uint16_t(weight)); append(sign); append(scale);
	for (auto digit : digits) { append(digit); }
	return bytes;
}

int main() {
	using duckdb::PostgresNumericText;
	struct Case { int16_t weight; uint16_t sign, scale; std::vector<uint16_t> digits; std::string want; };
	for (const auto &test : std::vector<Case>{
	    {0, 0, 18, {1, 2345, 6789, 123, 4567, 8900}, "1.234567890123456789"},
	    {-1, 0x4000, 6, {12, 3000}, "-0.001230"},
	    {1, 0, 0, {1, 2000}, "12000"},
	    {0, 0, 5, {}, "0.00000"}, {0, 0x4000, 2, {}, "0.00"},
	    {0, 0xc000, 0, {}, "NaN"}, {0, 0xd000, 0, {}, "Infinity"}, {0, 0xf000, 0, {}, "-Infinity"},
	    {32767, 0, 0, {9999}, "9999" + std::string(131068, '0')},
	    {-4096, 0, 16383, {10}, "0." + std::string(16382, '0') + "1"}}) {
		auto bytes = Pack(test.weight, test.sign, test.scale, test.digits);
		if (PostgresNumericText(bytes.data(), bytes.size()) != test.want) {
			std::cerr << "PostgreSQL numeric binary decoder changed digits or scale\n";
			return 1;
		}
	}
	for (const auto &bytes : std::vector<std::string>{
	    "", std::string(7, '\0'), Pack(0, 0, 0, {10000}), Pack(0, 0x8000, 0, {}),
	    Pack(0, 0, 16384, {}), Pack(-1, 0, 2, {1234}), Pack(0, 0xc000, 0, {1}),
	    Pack(0, 0, 0, {1}) + "x"}) {
		try { PostgresNumericText(bytes.data(), bytes.size()); return 2; }
		catch (const std::invalid_argument &) { }
	}
	std::cout << "PostgreSQL exact numeric binary decoder passed\n";
}
