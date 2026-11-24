#include "Rage.hpp"
#include "Pattern.hpp"

namespace spooner {
RageApi ResolveLegacy(const Image& image) {
    // Read-only anchors from FiveM's GamePrimitives_MapData.cpp. A match alone does not
    // establish the calling convention or the fields on the selected building.
    constexpr struct {
        const char* name;
        const char* bytes;
    } anchors[] = {
        {"constructMapEntity call",
         "4D 8B C6 41 8B D7 48 8B CF E8 ? ? ? ? 49 8B 4D"},
        {"mapDataContents create", "F7 42 24 A0 05 00 00 49 8B D8"},
        {"mapDataContents remove", "49 8B 47 08 0F B6 0C 28"},
        {"entity to script GUID",
         "48 F7 F9 49 8B 48 08 48 63 D0 C1 E0 08 0F B6 1C 11 03 D8"},
    };
    for (const auto& anchor : anchors) {
        const auto match = image.Unique(anchor.bytes);
        LogDiagnostic("RAGE anchor %s: %s at %p", anchor.name,
                      match ? "unique" : "missing/ambiguous", reinterpret_cast<void*>(match));
    }
    LogDiagnostic("Static RAGE writes disabled: building ownership, LOD and physics still unverified.");
    return {};
}
}
