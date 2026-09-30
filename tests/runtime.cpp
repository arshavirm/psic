#include <cstdint>
#include <cstring>
#include <iostream>
extern "C" {
std::int32_t psi_add(std::int32_t, std::int32_t);
std::uint32_t psi_unsigned_shift(std::uint32_t);
std::int32_t psi_signed_shift(std::int32_t);
std::int32_t psi_sum(std::int32_t);
std::int32_t psi_array();
std::int32_t psi_repeated_array_storage();
std::int32_t psi_struct();
std::int32_t psi_zero_structure_padding();
std::int32_t psi_zero_local();
std::int32_t psi_memory();
std::int32_t psi_unaligned_memory();
std::int32_t psi_pointer_arithmetic();
std::int32_t psi_memory_intrinsics();
std::int32_t psi_zero_length_memory();
std::int32_t psi_explicit_casts(std::int32_t, std::uint8_t);
std::int32_t psi_float_to_int(double);
std::int32_t psi_overlapping_copy();
std::int32_t psi_atomic_cas();
float psi_atomic_fadd(float*, float);
double psi_atomic_fsub(double*, double);
float psi_subnormal_add(float, float);
float psi_subnormal_underflow(float);
std::int32_t psi_subnormal_compare(float);
std::uint32_t psi_subnormal_to_uint(float);
std::int32_t psi_external(std::int32_t);
std::int32_t psi_global();
double psi_float(double);
std::int32_t psi_popcount(std::int32_t);
std::int32_t psi_view(std::uint64_t, std::int32_t);
std::uint64_t psi_view_length();
std::int32_t psi_view_call();
std::int32_t psi_view_slice();
std::uint64_t psi_empty_view_slice();
std::int32_t psi_pointer_address_order();
std::int32_t host_increment(std::int32_t value) { return value + 1; }
}
int main()
{
    const std::uint32_t positiveSubnormalBits = 1;
    const std::uint32_t negativeSubnormalBits = 0x80000001u;
    float positiveSubnormal = 0.0f;
    float negativeSubnormal = 0.0f;
    std::memcpy(&positiveSubnormal, &positiveSubnormalBits, sizeof(positiveSubnormal));
    std::memcpy(&negativeSubnormal, &negativeSubnormalBits, sizeof(negativeSubnormal));
    const float minimumNormal = 0x1p-126f;

    float atomicFloat = 0.0f;
    const std::uint32_t oldFloatBits = 0x7fc12345u;
    std::memcpy(&atomicFloat, &oldFloatBits, sizeof(atomicFloat));
    const float returnedFloat = psi_atomic_fadd(&atomicFloat, 1.0f);
    std::uint32_t returnedFloatBits = 0;
    std::uint32_t storedFloatBits = 0;
    std::memcpy(&returnedFloatBits, &returnedFloat, sizeof(returnedFloat));
    std::memcpy(&storedFloatBits, &atomicFloat, sizeof(atomicFloat));

    double atomicDouble = 0.0;
    const std::uint64_t oldDoubleBits = 0x7ff8123456789abcull;
    std::memcpy(&atomicDouble, &oldDoubleBits, sizeof(atomicDouble));
    const double returnedDouble = psi_atomic_fsub(&atomicDouble, 1.0);
    std::uint64_t returnedDoubleBits = 0;
    std::uint64_t storedDoubleBits = 0;
    std::memcpy(&returnedDoubleBits, &returnedDouble, sizeof(returnedDouble));
    std::memcpy(&storedDoubleBits, &atomicDouble, sizeof(atomicDouble));

    float atomicSubnormal = 0.0f;
    psi_atomic_fadd(&atomicSubnormal, positiveSubnormal);
    std::uint32_t atomicSubnormalBits = 1;
    std::memcpy(&atomicSubnormalBits, &atomicSubnormal, sizeof(atomicSubnormal));

    if (psi_add(-13, 55) != 42 || psi_unsigned_shift(0x80000000u) != 0x40000000u
        || psi_signed_shift(-8) != -4 || psi_sum(10) != 45 || psi_sum(0) != 0
        || psi_array() != 10 || psi_repeated_array_storage() != 0
        || psi_struct() != 12 || psi_zero_structure_padding() != 0
        || psi_zero_local() != 0 || psi_memory() != 19
        || psi_unaligned_memory() != 16909060
        || psi_pointer_arithmetic() != 9
        || psi_memory_intrinsics() != 11 || psi_zero_length_memory() != 42
        || psi_explicit_casts(255, 255) != 268
        || psi_float_to_int(12.75) != 12
        || psi_overlapping_copy() != 3
        || psi_atomic_cas() != 13
        || returnedFloatBits != oldFloatBits || storedFloatBits != 0x7fc00000u
        || returnedDoubleBits != oldDoubleBits || storedDoubleBits != 0x7ff8000000000000ull
        || psi_subnormal_add(positiveSubnormal, 0.0f) != 0.0f
        || psi_subnormal_underflow(minimumNormal) != 0.0f
        || psi_subnormal_compare(positiveSubnormal) != 1
        || psi_subnormal_to_uint(negativeSubnormal) != 0
        || atomicSubnormalBits != 0
        || psi_external(41) != 42 || psi_global() != 11
        || psi_float(2.75) != 5.5 || psi_popcount(255) != 8
        || psi_view(0, 5) != 5 || psi_view(1, 9) != 9 || psi_view(2, 16) != 16
        || psi_view_length() != 3 || psi_view_call() != 42 || psi_view_slice() != 23
        || psi_empty_view_slice() != 0 || psi_pointer_address_order() != 1) {
        std::cerr << "compiled PSI program returned incorrect results\n";
        return 1;
    }
    std::cout << "Native arithmetic, control flow, memory, structs, globals and external calls passed.\n";
}
