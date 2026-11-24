#include "Rage.hpp"
#include "Pattern.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <vector>

namespace spooner {
namespace {
constexpr std::uint32_t Stamp3889 = 0x6a4f9ed3;
constexpr std::size_t ImageSize3889 = 0x3e5bc00;
constexpr std::size_t BuildingVtableRva = 0x1a98b60;
constexpr std::size_t UpdateTransformSlot = 0xe0;
constexpr std::size_t MapSlotSize = 40;

struct Pool {
    std::uintptr_t slots{}, generations{};
    std::uint32_t count{}, stride{};
};
struct MapSlot {
    std::uintptr_t contents{};
    std::uint32_t flags{}, name{};
    std::byte padding[24]{};
};
static_assert(sizeof(MapSlot) == MapSlotSize);
struct Contents {
    std::uintptr_t vtable{}, scene{}, entities{};
    std::uint32_t count{};
};
struct Snapshot {
    std::uintptr_t vtable{};
    std::byte unknown[0x20 - 8]{};
    std::uintptr_t archetype{};
    std::uint8_t type{};
    std::byte unknown2[0x60 - 0x29]{};
    Matrix34 matrix;
    std::uintptr_t lodLink{};
};
static_assert(offsetof(Snapshot, matrix) == 0x60 && offsetof(Snapshot, lodLink) == 0xa0);

std::uintptr_t poolAddress{}, transformAddress{}, buildingVtable{};

bool Read(std::uintptr_t address, void* out, std::size_t size) {
    if (!address || !size) return false;
    SIZE_T done{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                             out, size, &done) && done == size;
}
template<class T> bool Read(std::uintptr_t address, T& out) {
    return Read(address, &out, sizeof(out));
}

float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool FitsFirstFace(const Ray& ray, const Hit& hit, const Snapshot& entity,
                   const reference::FwArchetype& model) {
    const Vec3 origin = entity.matrix.Position();
    const Vec3 fromPivot = hit.point - origin;
    if (!(Dot(fromPivot, fromPivot) < 100000.f)) return false;
    const Vec3 relative = ray.start - origin;
    const Vec3 heading = ray.end - ray.start;
    const float length = std::sqrt(Dot(heading, heading));
    if (!(length > 0.01f)) return false;
    float entryDistance = 0.f, exitDistance = length;
    const Vec3 direction = heading * (1.f / length);
    for (int i = 0; i < 3; ++i) {
        const auto& row = entity.matrix.row[i];
        const Vec3 axis{row[0], row[1], row[2]};
        const float scaleSquared = Dot(axis, axis);
        const float lower = model.localMin[i], upper = model.localMax[i];
        if (!(scaleSquared > 0.80f && scaleSquared < 1.25f &&
              lower >= -160.f && upper <= 160.f && upper > lower)) return false;
        const float localStart = Dot(axis, relative) / scaleSquared;
        const float localStep = Dot(axis, direction) / scaleSquared;
        if (std::abs(localStep) < 0.0001f) {
            if (localStart < lower || localStart > upper) return false;
            continue;
        }
        const float a = (lower - localStart) / localStep, b = (upper - localStart) / localStep;
        entryDistance = std::max(entryDistance, std::min(a, b));
        exitDistance = std::min(exitDistance, std::max(a, b));
        if (entryDistance > exitDistance) return false;
    }
    const float actual = Dot(hit.point - ray.start, direction);
    return entryDistance > 0.f && std::abs(entryDistance - actual) <= 0.18f;
}

bool ReadPool(Pool& pool) {
    return Read(poolAddress, pool) && pool.slots && pool.generations &&
           pool.count > 0 && pool.count <= 30000 && pool.stride == MapSlotSize;
}

bool FindTrial(const Ray& ray, const Hit& hit, BuildingTrial& result) {
    if (hit.handle != 0) return false;
    Pool pool{};
    if (!ReadPool(pool)) return false;
    std::vector<MapSlot> slots(pool.count);
    std::vector<std::uint8_t> generations(pool.count);
    if (!Read(pool.slots, slots.data(), pool.count * sizeof(MapSlot)) ||
        !Read(pool.generations, generations.data(), pool.count)) return false;

    BuildingTrial candidate{};
    std::vector<std::uintptr_t> linkedParents;
    std::vector<std::uintptr_t> entities;
    for (std::uint32_t slot = 0; slot < pool.count; ++slot) {
        const auto& map = slots[slot];
        Contents data{};
        if ((generations[slot] & 0x80) || !Read(map.contents, data) ||
            !data.count || data.count > 5000) continue;
        entities.resize(data.count);
        if (!Read(data.entities, entities.data(), entities.size() * sizeof(std::uintptr_t))) continue;
        for (std::uint32_t index = 0; index < data.count; ++index) {
            const auto address = entities[index];
            Snapshot entity{};
            if (!Read(address, entity)) continue;
            if (entity.lodLink) linkedParents.push_back(entity.lodLink);
            if (entity.vtable != buildingVtable || entity.type != 1) continue;
            const Vec3 distance = entity.matrix.Position() - hit.point;
            if (!(Dot(distance, distance) < 100000.f)) continue;
            reference::FwArchetype model{};
            if (!Read(entity.archetype, model) || !model.hash || !FitsFirstFace(ray, hit, entity, model))
                continue;
            if (candidate.entity || entity.lodLink) return false;
            candidate = {address, map.contents, entity.archetype, slot, index, map.name,
                         model.hash, generations[slot], entity.matrix};
        }
    }
    if (!candidate.entity || std::find(linkedParents.begin(), linkedParents.end(), candidate.entity) !=
                             linkedParents.end()) return false;
    result = candidate;
    LogDiagnostic("Static candidate ymap=%08X index=%u model=%08X", result.mapHash,
                  result.index, result.model);
    return true;
}

bool SetTrialPosition(const BuildingTrial& trial, Vec3 offset) {
    Pool pool{};
    MapSlot slot{};
    Contents contents{};
    Snapshot entity{};
    std::uint8_t generation{};
    std::uintptr_t current{}, method{};
    if (!ReadPool(pool) || trial.slot >= pool.count ||
        !Read(pool.generations + trial.slot, generation) || generation != trial.generation ||
        !Read(pool.slots + trial.slot * sizeof(MapSlot), slot) ||
        slot.contents != trial.contents || slot.name != trial.mapHash ||
        !Read(slot.contents, contents) || trial.index >= contents.count ||
        !Read(contents.entities + trial.index * sizeof(std::uintptr_t), current) ||
        current != trial.entity || !Read(current, entity) ||
        entity.vtable != buildingVtable || entity.archetype != trial.archetype || entity.lodLink ||
        !Read(entity.vtable + UpdateTransformSlot, method) || method != transformAddress) return false;

    auto target = trial.original;
    target.Translate(offset);
    for (int i = 0; i < 3; ++i) target.row[i][3] = 0.f;
    target.row[3][3] = 1.f;
    using UpdateTransform = void (*)(void*, const Matrix34&, bool, bool, bool);
    reinterpret_cast<UpdateTransform>(method)(reinterpret_cast<void*>(current), target, true, false, false);
    Matrix34 actual{};
    return Read(current + offsetof(Snapshot, matrix), actual) &&
           Dot(actual.Position() - target.Position(), actual.Position() - target.Position()) < 0.0001f;
}
}

RageApi ResolveLegacy(const Image& image) {
    RageApi api;
    if (image.Timestamp() != Stamp3889 || image.Size() != ImageSize3889) {
        LogDiagnostic("Static trial unavailable outside verified Legacy 3889 image");
        return api;
    }

    const auto anchor = image.Unique("74 15 48 8D 50 01 48 8D 0D ? ? ? ? E8");
    const auto manager = anchor ? image.Relative(anchor + 6, 3, 7) : 0;
    transformAddress = image.Unique(
        "48 83 EC 28 0F 28 02 66 0F 70 59 70 FF 66 0F 70 A1 80 00 00 00 FF 0F 29 41 60");
    buildingVtable = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + BuildingVtableRva;

    std::uintptr_t modules{}, ymap{}, method{};
    std::uint16_t moduleCount{};
    std::uint32_t moduleIndex{};
    Pool pool{};
    if (!manager || !transformAddress || !Read(manager + 24, modules) ||
        !Read(manager + 32, moduleCount) || moduleCount <= 8 ||
        !Read(modules + 8 * sizeof(std::uintptr_t), ymap) || !ymap ||
        !Read(ymap + 12, moduleIndex) || moduleIndex != 8 ||
        !Read(buildingVtable + UpdateTransformSlot, method) || method != transformAddress) {
        LogDiagnostic("Static trial: transform or YMAP anchor did not validate");
        return api;
    }
    poolAddress = ymap + 56;
    if (!ReadPool(pool)) {
        LogDiagnostic("Static trial: invalid YMAP pool");
        return api;
    }
    api.findTrial = FindTrial;
    api.setTrialPosition = SetTrialPosition;
    LogDiagnostic("Static trial ready on Legacy 3889");
    return api;
}
}
