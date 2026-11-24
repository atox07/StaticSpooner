#include "Pattern.hpp"

#include <windows.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace spooner {
static const IMAGE_NT_HEADERS64* Headers(std::uintptr_t base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE &&
           nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? nt : nullptr;
}

Image::Image(void* module) : base_(reinterpret_cast<std::uintptr_t>(module)) {
    if (const auto* nt = Headers(base_)) {
        size_ = nt->OptionalHeader.SizeOfImage;
        timestamp_ = nt->FileHeader.TimeDateStamp;
    }
}

bool Image::Contains(std::uintptr_t address, std::size_t bytes) const {
    return size_ && address >= base_ && bytes <= size_ && address - base_ <= size_ - bytes;
}

std::uintptr_t Image::Unique(std::string_view idaPattern) const {
    if (!size_) return 0;
    std::istringstream input{std::string(idaPattern)};
    std::vector<int> pattern;
    bool hasLiteral = false;
    for (std::string token; input >> token;) {
        if (token == "?" || token == "??") {
            pattern.push_back(-1);
            continue;
        }
        unsigned byte{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), byte, 16);
        if (token.size() != 2 || parsed.ec != std::errc{} ||
            parsed.ptr != token.data() + token.size()) return 0;
        pattern.push_back(static_cast<int>(byte));
        hasLiteral = true;
    }
    if (!hasLiteral) return 0;

    const auto* nt = Headers(base_);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    std::uintptr_t match{};
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        constexpr DWORD flags = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
        if ((section->Characteristics & flags) != flags) continue;
        const auto start = base_ + section->VirtualAddress;
        const auto length = static_cast<std::size_t>(section->Misc.VirtualSize);
        if (!Contains(start, length) || length < pattern.size()) continue;
        const auto* bytes = reinterpret_cast<const unsigned char*>(start);
        for (std::size_t i = 0; i <= length - pattern.size(); ++i) {
            std::size_t j = 0;
            while (j < pattern.size() && (pattern[j] < 0 || pattern[j] == bytes[i + j])) ++j;
            if (j != pattern.size()) continue;
            if (match) return 0;
            match = start + i;
        }
    }
    return match;
}

std::uintptr_t Image::Relative(std::uintptr_t instruction, std::size_t displacementOffset,
                               std::size_t instructionLength) const {
    if (!Contains(instruction, instructionLength) || displacementOffset > instructionLength ||
        instructionLength - displacementOffset < sizeof(std::int32_t)) return 0;

    std::int32_t displacement{};
    std::memcpy(&displacement, reinterpret_cast<const void*>(instruction + displacementOffset),
                sizeof(displacement));
    const auto target = instruction + instructionLength + displacement;
    return Contains(target) ? target : 0;
}
}
