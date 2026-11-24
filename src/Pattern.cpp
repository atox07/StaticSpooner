#include "Pattern.hpp"

#include <windows.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace spooner {
static bool Readable(const MEMORY_BASIC_INFORMATION& region) {
    if (region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD)) return false;
    const auto access = region.Protect & 0xff;
    return access == PAGE_READONLY || access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
           access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE ||
           access == PAGE_EXECUTE_WRITECOPY;
}

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
    std::size_t anchor = std::string::npos;
    for (std::string token; input >> token;) {
        if (token == "?" || token == "??") {
            pattern.push_back(-1);
            continue;
        }
        unsigned byte{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), byte, 16);
        if (token.size() != 2 || parsed.ec != std::errc{} ||
            parsed.ptr != token.data() + token.size()) return 0;
        if (anchor == std::string::npos) anchor = pattern.size();
        pattern.push_back(static_cast<int>(byte));
    }
    if (anchor == std::string::npos) return 0;

    const auto* nt = Headers(base_);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    std::uintptr_t match{};
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        constexpr DWORD flags = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
        if ((section->Characteristics & flags) != flags) continue;
        const auto start = base_ + section->VirtualAddress;
        const auto length = static_cast<std::size_t>(section->Misc.VirtualSize);
        if (!Contains(start, length) || length < pattern.size()) continue;
        for (auto cursor = start; cursor < start + length;) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region))) break;
            const auto end = std::min(start + length,
                                      reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize);
            if (end <= cursor) break;
            if (Readable(region) && end - cursor >= pattern.size()) {
                const auto* current = reinterpret_cast<const unsigned char*>(cursor + anchor);
                const auto* last = reinterpret_cast<const unsigned char*>(end - pattern.size() + anchor);
                while (current <= last) {
                    const auto* found = static_cast<const unsigned char*>(
                        std::memchr(current, pattern[anchor], static_cast<std::size_t>(last - current + 1)));
                    if (!found) break;
                    const auto* bytes = found - anchor;
                    std::size_t j = 0;
                    while (j < pattern.size() && (pattern[j] < 0 || pattern[j] == bytes[j])) ++j;
                    if (j == pattern.size()) {
                        if (match) return 0;
                        match = reinterpret_cast<std::uintptr_t>(bytes);
                    }
                    current = found + 1;
                }
            }
            cursor = end;
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
