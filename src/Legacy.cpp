#include "Rage.hpp"
#include "Pattern.hpp"

namespace spooner {
RageApi ResolveLegacy(const Image&) {
    // No verified executable/ABI profile is shipped with this source.
    // Supply a build adapter via MAP_SPOONER_LEGACY_ADAPTER. The engine ABI
    // and the patterns to validate are documented in README.md.
    return {};
}
}
