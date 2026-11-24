#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace spooner {
struct Vec3 {
    float x{}, y{}, z{};
    Vec3 operator+(Vec3 b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vec3 operator-(Vec3 b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

struct Quaternion { float x{}, y{}, z{}, w{1.f}; };

// Four padded 3D rows. Use only when the build confirms this representation.
struct alignas(16) Matrix34 {
    float row[4][4]{};
    Vec3 Position() const { return {row[3][0], row[3][1], row[3][2]}; }
    void Translate(Vec3 delta) {
        row[3][0] += delta.x;
        row[3][1] += delta.y;
        row[3][2] += delta.z;
    }
};
static_assert(sizeof(Matrix34) == 64 && alignof(Matrix34) == 16);

struct Sphere { Vec3 center; float radius{}; };
struct Aabb { Vec3 min, max; };

// Opaque RAGE types. Build-specific fields are read by the adapter.
// FiveM's fwEntity exposes an archetype pointer and an embedded transform.
// phInst is the physics instance, and may belong to a compound world bound.
struct CEntity;
struct fwArchetype;
struct phInst;

// Partial byte views of the public FiveM header for comparison in a disassembler.
// Legacy's adapter must verify the same offsets before using either view.
namespace reference {
struct alignas(16) FwEntity {
    std::byte header[0x20];
    fwArchetype* archetype;
    std::uint8_t type;
    std::byte unknown[0x60 - 0x29];
    Matrix34 transform;
};
struct FwArchetype {
    std::byte header[0x18];
    std::uint32_t hash;
    std::byte unknown[0x2C - 0x1C];
    float localRadius;
    float localMin[4], localMax[4];
};
static_assert(offsetof(FwEntity, archetype) == 0x20);
static_assert(offsetof(FwEntity, transform) == 0x60);
static_assert(offsetof(FwArchetype, hash) == 0x18);
static_assert(offsetof(FwArchetype, localMin) == 0x30);
}

struct Ray { Vec3 start, end; int ignore{}; };
struct Hit { bool hit{}; Vec3 point, normal; int handle{}; };

enum class Lod { Hd, Lod, Slod1, Slod2OrHigher, Unknown };

// The adapter must include a generation/streaming identity, even if two
// different instances can reuse the same address.
struct EntityId {
    CEntity* address{};
    std::uint64_t generation{};
    bool operator==(const EntityId&) const = default;
    explicit operator bool() const { return address != nullptr; }
};

struct Building {
    EntityId id, parent;
    const fwArchetype* archetype{};
    const phInst* physics{};
    std::uint32_t modelHash{};
    Lod lod{Lod::Unknown};
    bool sharedOrMerged{};
    bool independentCollision{};
    bool proxyMatchesTransform{}; // Unit scale and equivalent collision confirmed.
    Matrix34 matrix;
    Sphere worldSphere;
    Aabb worldStreamingBounds;
    Quaternion rotation;
};

struct Chain {
    std::array<Building, 3> entry{};
    std::size_t count{};
    bool editable{};
};

class Image;
void LogDiagnostic(const char* format, ...);
struct RageApi {
    // Custom adapter ABIs. Do not cast arbitrary game function addresses to these.
    bool (*pick)(const Ray&, const Hit&, EntityId&){};
    bool (*inspect)(EntityId, Building&){};
    // A successful acquisition protects the whole chain until release.
    bool (*acquire)(const Chain&, std::uint64_t& lease){};
    // Synchronous all-or-nothing operation at a verified engine update point.
    // Apply writes matrices and refreshes spatial/streaming registration. On the
    // first call it also detaches the original isolated collider. Restore must
    // be synchronous and guaranteed after a successful acquire.
    bool (*apply)(std::uint64_t lease, const Chain& target){};
    void (*restoreAndRelease)(std::uint64_t lease){};
};

RageApi ResolveLegacy(const Image& image);

inline Chain CollectChain(const RageApi& api, EntityId id) {
    Chain chain;
    bool complete = true;
    for (; id && chain.count < chain.entry.size();) {
        Building item;
        if (!api.inspect || !api.inspect(id, item) || item.id != id) {
            complete = false;
            break;
        }

        if (item.lod == Lod::Slod2OrHigher) break;
        if (item.lod == Lod::Unknown ||
            (chain.count && static_cast<int>(item.lod) <=
                                static_cast<int>(chain.entry[chain.count - 1].lod))) {
            complete = false;
            break;
        }

        chain.entry[chain.count++] = item;
        if (item.lod == Lod::Slod1) break;
        id = item.parent;
    }

    if (!complete || !chain.count) return chain;
    const auto& root = chain.entry[0];
    chain.editable = root.lod == Lod::Hd && root.independentCollision &&
                     root.proxyMatchesTransform && root.modelHash != 0;
    for (std::size_t i = 0; i < chain.count; ++i) {
        const auto& item = chain.entry[i];
        if (item.sharedOrMerged || (i && item.physics != nullptr)) chain.editable = false;
    }
    return chain;
}

inline Chain MakeTarget(const Chain& original, Vec3 absoluteDelta) {
    Chain target = original;
    for (std::size_t i = 0; i < target.count; ++i) {
        auto& entry = target.entry[i];
        entry.matrix.Translate(absoluteDelta);
        entry.worldSphere.center = entry.worldSphere.center + absoluteDelta;
        entry.worldStreamingBounds.min = entry.worldStreamingBounds.min + absoluteDelta;
        entry.worldStreamingBounds.max = entry.worldStreamingBounds.max + absoluteDelta;
    }
    return target;
}
}
