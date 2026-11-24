#include "Rage.hpp"

#include <cstdio>

using namespace spooner;

namespace {
int markers[4]{};
std::array<Building, 4> records{};

EntityId Id(std::size_t index) {
    return {reinterpret_cast<CEntity*>(&markers[index]), index + 1};
}

bool Inspect(EntityId id, Building& out) {
    for (const auto& item : records) {
        if (item.id == id) {
            out = item;
            return true;
        }
    }
    return false;
}

int failures{};
void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "%s\n", message);
    ++failures;
}
}

int main() {
    const auto levels = std::array{Lod::Hd, Lod::Lod, Lod::Slod1, Lod::Slod2OrHigher};
    for (std::size_t i = 0; i < records.size(); ++i) {
        records[i].id = Id(i);
        if (i + 1 < records.size()) records[i].parent = Id(i + 1);
        records[i].lod = levels[i];
        records[i].matrix.row[3][0] = static_cast<float>(i);
        records[i].matrix.row[3][3] = 1.f;
        records[i].matrix.row[0][1] = 0.5f;
        records[i].worldSphere = {{2, 4, 8}, 11};
        records[i].worldStreamingBounds = {{-1, -2, -3}, {1, 2, 3}};
    }
    records[0].modelHash = 123;
    records[0].independentCollision = records[0].proxyMatchesTransform = true;
    RageApi api{};
    api.inspect = Inspect;

    auto chain = CollectChain(api, Id(0));
    Check(chain.count == 3 && chain.editable, "HD -> LOD -> SLOD1 should be editable");
    auto target = MakeTarget(chain, {2, -3, 4});
    Check(target.count == 3 && target.entry[2].matrix.Position().x == 4,
          "Every LOD matrix must translate");
    Check(target.entry[1].matrix.row[0][1] == 0.5f && target.entry[1].matrix.row[3][3] == 1.f,
          "Basis and row padding should be preserved");
    Check(target.entry[2].worldSphere.center.y == 1 && target.entry[2].worldSphere.radius == 11,
          "Sphere center moves, radius is preserved");
    Check(target.entry[0].worldStreamingBounds.min.y == -5 &&
          target.entry[0].worldStreamingBounds.max.z == 7 &&
          chain.entry[0].matrix.Position().x == 0, "Bounds move from an unchanged snapshot");

    records[1].sharedOrMerged = true;
    chain = CollectChain(api, Id(0));
    Check(chain.count == 3 && !chain.editable, "Shared LOD stays inspectable");
    records[1].sharedOrMerged = false;
    records[1].parent = Id(0);
    Check(!CollectChain(api, Id(0)).editable, "Cycles must fail closed");
    records[1].parent = {Id(2).address, 99};
    Check(!CollectChain(api, Id(0)).editable, "Stale generation must fail closed");
    records[1].parent = Id(3);
    chain = CollectChain(api, Id(0));
    Check(chain.count == 2 && chain.editable, "SLOD2 must be excluded even without SLOD1");
    Check(CollectChain(api, Id(3)).count == 0, "SLOD2 itself must not be selected");
    return failures != 0;
}
