// Regression checks for instruction boundaries used by native ShellUI hooks.
#include "prologue.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

int main() {
    using namespace home_bridge;

    // Mono on PS5 13.60 exposes Update as FF 25 00 00 00 00 followed by an
    // inline 64-bit target. Preserve all 14 bytes as one patchable thunk.
    std::array<std::uint8_t, kAtomicPatchSize> thunk{};
    const std::uint8_t jump_prefix[] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(thunk.data(), jump_prefix, sizeof jump_prefix);
    const std::uint64_t target = 0x0000000812345678ULL;
    std::memcpy(thunk.data() + sizeof jump_prefix, &target, sizeof target);
    assert(native_patch_length(thunk.data()) == kAbsoluteJumpSize);

    // A null or non-user target must never be patched as a valid thunk.
    auto invalid = thunk;
    std::memset(invalid.data() + sizeof jump_prefix, 0, sizeof target);
    assert(native_patch_length(invalid.data()) == 0);
    const std::uint64_t kernel_target = 0xffff800000001000ULL;
    std::memcpy(invalid.data() + sizeof jump_prefix, &kernel_target, sizeof kernel_target);
    assert(native_patch_length(invalid.data()) == 0);

    // Only the exact RIP+0 form carries its 8-byte pointer within the 14-byte
    // stub; other FF /4 encodings remain unsupported and are refused.
    invalid = thunk;
    invalid[2] = 1;
    assert(native_patch_length(invalid.data()) == 0);
    invalid = thunk;
    invalid[1] = 0x15; // indirect CALL is not the recognized JMP thunk
    assert(native_patch_length(invalid.data()) == 0);

    // Ordinary supported prologues retain their original decoding behavior.
    const std::array<std::uint8_t, kAtomicPatchSize> prologue = {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
        0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0x7d, 0xf8
    };
    assert(native_patch_length(prologue.data()) == kAtomicPatchSize);

    std::cout << "Kabuki prologue checks passed\n";
}
