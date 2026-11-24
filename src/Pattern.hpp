#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace spooner {
// Scans executable sections of GTA5.exe in the current process.
class Image {
public:
    explicit Image(void* module);
    std::uint32_t Timestamp() const { return timestamp_; }
    std::size_t Size() const { return size_; }
    std::uintptr_t Unique(std::string_view idaPattern) const;
    // displacementOffset is measured from the instruction's first byte.
    std::uintptr_t Relative(std::uintptr_t instruction, std::size_t displacementOffset,
                            std::size_t instructionLength) const;
    bool Contains(std::uintptr_t address, std::size_t bytes = 1) const;
private:
    std::uintptr_t base_{};
    std::size_t size_{};
    std::uint32_t timestamp_{};
};
}
