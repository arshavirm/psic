#include <cstdint>
#include <iostream>
extern "C" {
std::int32_t psi_add(std::int32_t, std::int32_t);
std::uint32_t psi_unsigned_shift(std::uint32_t);
std::int32_t psi_signed_shift(std::int32_t);
std::int32_t psi_sum(std::int32_t);
std::int32_t psi_array();
std::int32_t psi_struct();
std::int32_t psi_memory();
std::int32_t psi_external(std::int32_t);
std::int32_t psi_global();
double psi_float(double);
std::int32_t psi_popcount(std::int32_t);
std::int32_t host_increment(std::int32_t value) { return value + 1; }
}
int main()
{
    if (psi_add(-13, 55) != 42 || psi_unsigned_shift(0x80000000u) != 0x40000000u
        || psi_signed_shift(-8) != -4 || psi_sum(10) != 45 || psi_sum(0) != 0
        || psi_array() != 10 || psi_struct() != 12 || psi_memory() != 19
        || psi_external(41) != 42 || psi_global() != 11
        || psi_float(2.75) != 5.5 || psi_popcount(255) != 8) {
        std::cerr << "compiled PSI program returned incorrect results\n";
        return 1;
    }
    std::cout << "Native arithmetic, control flow, memory, structs, globals and external calls passed.\n";
}
