#include "Pattern.hpp"
#include "Rage.hpp"

#include <windows.h>
#include <main.h>
#include <types.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <type_traits>

namespace spooner {
namespace {
std::atomic_bool toggleRequested{};
enum class LogLocation { Unavailable, Game, Temp };
LogLocation logLocation{};

bool AppendLog(wchar_t (&path)[MAX_PATH], std::size_t directoryLength, const char* line, DWORD size) {
    constexpr wchar_t name[] = L"MapSpooner.log";
    if (!directoryLength || directoryLength + std::size(name) > MAX_PATH) return false;
    std::memcpy(path + directoryLength, name, sizeof(name));
    const HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written{};
    const bool ok = WriteFile(file, line, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}

void OnKeyboard(DWORD key, WORD, BYTE, BOOL, BOOL, BOOL wasDown, BOOL isUp) {
    if (key == VK_F12 && !wasDown && !isUp) {
        toggleRequested.store(true, std::memory_order_relaxed);
    }
}
}

void LogDiagnostic(const char* format, ...) {
    char line[512]{};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof(line) - 2, _TRUNCATE, format, args);
    va_end(args);
    const auto end = std::strlen(line);
    line[end] = '\r';
    line[end + 1] = '\n';
    const DWORD size = static_cast<DWORD>(end + 2);
    wchar_t path[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    const auto* separator = length && length < MAX_PATH ? std::wcsrchr(path, L'\\') : nullptr;
    if (separator && AppendLog(path, static_cast<std::size_t>(separator - path + 1), line, size)) {
        logLocation = LogLocation::Game;
        return;
    }
    const auto tempLength = GetTempPathW(MAX_PATH, path);
    if (tempLength && tempLength < MAX_PATH && AppendLog(path, tempLength, line, size)) {
        logLocation = LogLocation::Temp;
        return;
    }
    logLocation = LogLocation::Unavailable;
    OutputDebugStringA(line);
}

namespace {
// Vector arguments and results use the padding defined by ScriptHookV.
static_assert(sizeof(Vector3) == 24 && offsetof(Vector3, y) == 8 && offsetof(Vector3, z) == 16);
Vec3 Unpack(Vector3 v) { return {v.x, v.y, v.z}; }

bool Finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

template<class T> std::uint64_t Slot(T value) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<std::uintptr_t>(value);
    } else if constexpr (std::is_same_v<T, float>) {
        return std::bit_cast<std::uint32_t>(value);
    } else {
        return static_cast<std::uint64_t>(value);
    }
}

template<class R = void, class... A> R Call(std::uint64_t hash, A... args) {
    nativeInit(hash);
    (nativePush64(Slot(args)), ...);
    const auto* result = nativeCall();
    if constexpr (!std::is_void_v<R>) {
        R value{};
        if (result) std::memcpy(&value, result, sizeof(value));
        return value;
    }
}

constexpr auto DoesEntityExist = 0x7239B21A38F536BAull;
constexpr auto GetEntityType = 0x8ACD366038D14505ull;
constexpr auto GetEntityModel = 0x9F47B058362C84B5ull;
constexpr auto GetEntityCoords = 0x3FEF770D40960D5Aull;
constexpr auto IsEntityAMissionEntity = 0x0A7B270912999B3Cull;
constexpr auto SetEntityCollision = 0x1A9205C1B9EE827Full;
constexpr auto SetEntityCoordsNoOffset = 0x239A3351AC1DA385ull;
constexpr auto RemoveModel = 0xE532F5D78798DAABull;
constexpr auto IsPauseMenuActive = 0xB0034A223497FFCBull;

bool Down(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

Vec3 Forward(Vec3 rotation) {
    constexpr float rad = 0.01745329252f;
    const float pitch = rotation.x * rad, yaw = rotation.z * rad;
    const float horizontal = std::cos(pitch);
    return {-std::sin(yaw) * horizontal, std::cos(yaw) * horizontal, std::sin(pitch)};
}

int StartRaycast(const Ray& ray) {
    const auto a = ray.start, b = ray.end;
    // World, vehicles, peds, ragdolls, objects and glass.
    constexpr int flags = 1 | 2 | 4 | 8 | 16 | 64;
    return Call<int>(0x7EE9F5D83DD4F90Eull, a.x, a.y, a.z,
                     b.x, b.y, b.z, flags, ray.ignore, 4); // START_SHAPE_TEST_LOS_PROBE
}

int ReadRaycast(int probe, Hit& result) {
    BOOL hit{};
    alignas(16) Vector3 point{}, normal{};
    Entity entity{};
    const int status = Call<int>(0x3D87450E15D98694ull, probe, &hit, &point, &normal, &entity);
    if (status == 2) result = {hit != 0, Unpack(point), Unpack(normal), entity};
    return status;
}

void TextAt(float x, float y, const char* message, float size = 0.30f) {
    Call(0x07C837F9A01C34C9ull, 0.f, size);              // SET_TEXT_SCALE
    Call(0xBE6B23FFA53FB442ull, 255, 255, 255, 255);       // SET_TEXT_COLOUR
    Call(0x2513DFB0FB8400FEull);                          // SET_TEXT_OUTLINE
    Call(0x25FBB336DF1804CBull, "STRING");                // BEGIN_TEXT_COMMAND_DISPLAY_TEXT
    Call(0x6C188BE134E074AAull, message);                 // ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME
    Call(0xCD015E5BB0D96A57ull, x, y, 0);                // END_TEXT_COMMAND_DISPLAY_TEXT
}

void Text(float y, const char* message) { TextAt(0.022f, y, message); }

constexpr std::array<Vec3, 3> Axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
constexpr int Colors[3][3]{{255, 70, 70}, {70, 255, 100}, {70, 145, 255}};
bool Project(Vec3 p, float& x, float& y) {
    return Call<BOOL>(0x34E82F05DF2974F5ull, p.x, p.y, p.z, &x, &y) != 0;
}

struct Gizmo {
    int hovered{-1};
    float screenX{}, screenY{}, length{}, aspect{1.f};
};

float Magnitude(Vec3 v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

void Triangle(Vec3 a, Vec3 b, Vec3 c, const int (&color)[3]) {
    constexpr auto DrawPoly = 0xAC26716048436851ull;
    Call(DrawPoly, a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z, color[0], color[1], color[2], 255);
    Call(DrawPoly, c.x, c.y, c.z, b.x, b.y, b.z, a.x, a.y, a.z, color[0], color[1], color[2], 255);
}

void DrawArrow(Vec3 pivot, Vec3 towardCamera, Vec3 axis, float length, const int (&color)[3]) {
    const auto transverse = Cross(axis, towardCamera);
    const float width = Magnitude(transverse);
    if (width < 0.0001f) return;
    const auto sideways = transverse * (length / width);
    const auto shaft = sideways * 0.026f;
    const auto head = sideways * 0.075f;
    const auto shoulder = pivot + axis * (length * 0.73f);
    Triangle(pivot + shaft, pivot - shaft, shoulder + shaft, color);
    Triangle(pivot - shaft, shoulder - shaft, shoulder + shaft, color);
    Triangle(shoulder + head, shoulder - head, pivot + axis * length, color);
}

Gizmo DrawGizmo(Vec3 pivot, Vec3 camera, int activeAxis) {
    const auto towardCamera = camera - pivot;
    const float distance = Magnitude(towardCamera);
    Gizmo result;
    if (distance < 0.01f || !std::isfinite(distance)) return result;
    pivot = pivot + towardCamera * (std::clamp(distance * 0.008f, 0.025f, 0.4f) / distance);
    result.length = std::clamp(distance * 0.15f, 0.8f, 25.f);
    result.aspect = std::clamp(Call<float>(0xF1307EF624A80D87ull, false), 1.f, 4.f);
    float x{}, y{};
    const bool projected = Project(pivot, x, y);
    float nearest = 0.022f * 0.022f;
    float screenEnd[3][2]{};
    bool endVisible[3]{};
    for (int i = 0; i < 3; ++i) {
        const auto end = pivot + Axes[i] * result.length;
        float ex{}, ey{};
        if (projected && Project(end, ex, ey)) {
            screenEnd[i][0] = ex;
            screenEnd[i][1] = ey;
            endVisible[i] = true;
            const float dx = (ex - x) * result.aspect, dy = ey - y;
            const float lengthSquared = dx * dx + dy * dy;
            if (lengthSquared > 0.0001f) {
                const float aimX = (0.5f - x) * result.aspect, aimY = 0.5f - y;
                const float t = std::clamp((aimX * dx + aimY * dy) / lengthSquared, 0.20f, 1.f);
                const float fromX = dx * t - aimX, fromY = dy * t - aimY;
                const float d2 = fromX * fromX + fromY * fromY;
                if (d2 < nearest) {
                    nearest = d2;
                    result.hovered = i;
                    const float size = std::sqrt(lengthSquared);
                    result.screenX = dx / size;
                    result.screenY = dy / size;
                }
            }
        }
    }

    constexpr int highlighted[3]{255, 226, 90};
    constexpr const char* names[3]{"X", "Y", "Z"};
    const int highlightedAxis = activeAxis >= 0 ? activeAxis : result.hovered;
    for (int i = 0; i < 3; ++i) {
        DrawArrow(pivot, towardCamera, Axes[i], result.length,
                  i == highlightedAxis ? highlighted : Colors[i]);
        if (endVisible[i]) TextAt(screenEnd[i][0] + 0.004f, screenEnd[i][1] - 0.01f, names[i], 0.25f);
    }
    return result;
}

class Edit {
public:
    explicit Edit(RageApi api) : api_(api) {}

    void BeginPick(const Ray& ray) {
        if (probe_) return;
        ResetSelection();
        if (!Finite(ray.start) || !Finite(ray.end)) {
            LogDiagnostic("E: invalid camera coordinates");
            status_ = "Coordinate camera non valide";
            return;
        }
        ray_ = ray;
        LogDiagnostic("E: START_SHAPE_TEST_LOS_PROBE ignore=%d", ray.ignore);
        probe_ = StartRaycast(ray);
        discardProbe_ = false;
        loggedFirstPoll_ = false;
        probeStarted_ = GetTickCount64();
        LogDiagnostic("E: shape test handle=%d", probe_);
        status_ = probe_ ? "Raycast in corso..." : "Raycast rifiutato dal motore";
    }

    void PollPick() {
        if (!probe_) return;
        if (!loggedFirstPoll_) {
            LogDiagnostic("E: GET_SHAPE_TEST_RESULT handle=%d", probe_);
            loggedFirstPoll_ = true;
        }
        Hit hit{};
        const int status = ReadRaycast(probe_, hit);
        if (status == 1 && GetTickCount64() - probeStarted_ < 3000) return;
        LogDiagnostic("E: shape test status=%d hit=%d entity=%d point=(%.3f, %.3f, %.3f)",
                      status, int(hit.hit), hit.handle, hit.point.x, hit.point.y, hit.point.z);
        probe_ = 0;
        if (discardProbe_) return;
        status_ = "";
        if (status != 2) {
            status_ = status == 1 ? "Raycast scaduto" : "Risultato raycast non valido";
            return;
        }
        ShowHit(ray_, hit);
    }

    void ShowHit(const Ray& ray, const Hit& hit) {
        if (!hit.hit || !Finite(hit.point) || !Finite(hit.normal)) {
            std::snprintf(label_, sizeof(label_), "Nessuna superficie colpita");
            return;
        }
        EntityId id;
        if (api_.pick && api_.inspect && api_.pick(ray, hit, id) && id) {
            chain_ = CollectChain(api_, id);
            if (chain_.count) {
                selected_ = true;
                pivot_ = chain_.entry[0].matrix.Position();
                std::snprintf(label_, sizeof(label_), "CBuilding %08X | %zu livelli | %s",
                              chain_.entry[0].modelHash, chain_.count,
                              CanEdit() ? "editabile" : "ispezione");
                return;
            }
        }
        pivot_ = hit.point;
        selected_ = true;
        if (!hit.handle) {
            if (api_.findTrial && api_.setTrialPosition && api_.findTrial(ray, hit, trial_)) {
                trialHit_ = hit;
                std::snprintf(label_, sizeof(label_), "Edificio %08X | T prova spostamento di 5 m",
                              trial_.model);
                return;
            }
            std::snprintf(label_, sizeof(label_), "Superficie mappa | sola ispezione");
            return;
        }

        // GTA's mission/script objects have a real script handle. Static map
        // drawables and ambient objects with temporary handles stay read-only.
        if (Call<BOOL>(DoesEntityExist, hit.handle) && Call<int>(GetEntityType, hit.handle) == 3 &&
            Call<BOOL>(IsEntityAMissionEntity, hit.handle)) {
            const auto origin = Unpack(Call<Vector3>(GetEntityCoords, hit.handle, false));
            const auto model = Call<std::uint32_t>(GetEntityModel, hit.handle);
            if (Finite(origin) && model) {
                prop_ = hit.handle;
                propModel_ = model;
                propOrigin_ = origin;
                std::snprintf(label_, sizeof(label_), "Prop %08X | spostabile", model);
                LogDiagnostic("Editable script prop handle=%d model=%08X", prop_, propModel_);
                return;
            }
        }
        std::snprintf(label_, sizeof(label_), "Entita' %d | sola ispezione", hit.handle);
    }

    void StartTrial() {
        if (!trial_.entity || trialPhase_ != TrialPhase::Idle || trialMoved_) return;
        const float normalLength = Magnitude(trialHit_.normal);
        if (!(normalLength > 0.9f && normalLength < 1.1f)) return;
        trialNormal_ = trialHit_.normal * (1.f / normalLength);
        trialOffset_ = trialNormal_ * 5.f;
        StartTrialProbe(TrialPhase::OriginBefore, trialHit_.point);
    }

    void PollTrial() {
        if (trialPhase_ == TrialPhase::Idle) return;
        Hit hit{};
        const int result = ReadRaycast(trialProbe_, hit);
        if (result == 1 && GetTickCount64() - trialStarted_ < 2500) return;
        if (result != 2) {
            FinishTrial("Prova annullata: raycast di verifica fallito");
            return;
        }
        const auto phase = trialPhase_;
        if (phase == TrialPhase::OriginBefore && hit.hit && !hit.handle &&
            Magnitude(hit.point - trialHit_.point) < 0.18f) {
            StartTrialProbe(TrialPhase::DestinationBefore, trialHit_.point + trialOffset_);
        } else if (phase == TrialPhase::DestinationBefore && !hit.hit) {
            // After the engine call, a failed readback can still mean the
            // matrix changed. Keep rollback armed before making the call.
            trialMoved_ = true;
            if (!api_.setTrialPosition(trial_, trialOffset_)) {
                FinishTrial("Prova annullata: aggiornamento edificio fallito");
                return;
            }
            offset_ = trialOffset_;
            StartTrialProbe(TrialPhase::OriginAfter, trialHit_.point);
        } else if (phase == TrialPhase::OriginAfter && !hit.hit) {
            StartTrialProbe(TrialPhase::DestinationAfter, trialHit_.point + trialOffset_);
        } else if (phase == TrialPhase::DestinationAfter && hit.hit && !hit.handle &&
                   Magnitude(hit.point - trialHit_.point - trialOffset_) < 0.25f) {
            trialPhase_ = TrialPhase::Idle;
            status_ = "Prova riuscita sulle due superfici campionate | R Ripristina";
            LogDiagnostic("Static trial passed both collision samples");
        } else {
            const char* reason = "Prova annullata: collisione non segue l'edificio";
            if (phase == TrialPhase::OriginBefore) reason = "Prova annullata: origine non confermata";
            if (phase == TrialPhase::DestinationBefore) reason = "Prova annullata: destinazione occupata";
            FinishTrial(reason);
        }
    }

    bool SameProp() const {
        return prop_ && Call<BOOL>(DoesEntityExist, prop_) && Call<int>(GetEntityType, prop_) == 3 &&
               Call<std::uint32_t>(GetEntityModel, prop_) == propModel_;
    }

    void MoveProp() {
        if (!SameProp()) {
            ResetSelection();
            status_ = "Prop non piu' disponibile";
            return;
        }
        const auto p = propOrigin_ + requestedOffset_;
        Call(SetEntityCoordsNoOffset, prop_, p.x, p.y, p.z, true, true, true);
        offset_ = requestedOffset_;
        propMoved_ = true;
        pending_ = false;
    }

    bool CanEdit() const {
        return prop_ || (chain_.editable && api_.acquire && api_.apply && api_.restoreAndRelease);
    }
    bool Selected() const { return selected_; }
    Vec3 Pivot() const { return pivot_ + offset_; }
    const char* Label() const { return label_; }
    const char* Status() const { return status_; }

    void Move(Vec3 delta) {
        if (!CanEdit()) {
            if (selected_) status_ = "Selezione in sola ispezione";
            return;
        }
        if (delta.x == 0.f && delta.y == 0.f && delta.z == 0.f) return;
        const auto proposed = (pending_ ? requestedOffset_ : offset_) + delta;
        if (!std::isfinite(proposed.x) || !std::isfinite(proposed.y) || !std::isfinite(proposed.z)) return;
        requestedOffset_ = proposed;
        pending_ = true;
    }

    void Update() {
        if (proxy_ && !Call<BOOL>(DoesEntityExist, proxy_)) {
            Reset();
            status_ = "Proxy perso | ripristino eseguito";
            return;
        }
        if (!pending_) return;
        if (prop_) {
            MoveProp();
            return;
        }
        const auto& root = chain_.entry[0];
        if (!proxy_) {
            if (!loading_) {
                if (!Call<BOOL>(0xC0296A2EDF545E92ull, root.modelHash)) { // IS_MODEL_VALID
                    CancelPending("Modello non utilizzabile come CObject");
                    return;
                }
                started_ = GetTickCount64();
                loading_ = true;
            }
            Call(0x963D27A58DF860ACull, root.modelHash); // REQUEST_MODEL
            Call(0x923CB32A3B874FCBull, root.modelHash); // REQUEST_COLLISION_FOR_MODEL
            if (!Call<BOOL>(0x98A4EB5D89A0C952ull, root.modelHash) ||
                !Call<BOOL>(0x22CCA434E368F03Aull, root.modelHash)) {
                status_ = "Caricamento modello collisione...";
                if (GetTickCount64() - started_ > 3500) CancelPending("Timeout caricamento modello");
                return;
            }
        }

        const auto target = MakeTarget(chain_, requestedOffset_);
        const auto p = target.entry[0].matrix.Position();
        if (!proxy_) {
            // Acquire validates all identities against the picked snapshots before writes.
            if (!api_.acquire(chain_, lease_)) {
                CancelPending("La catena LOD e' cambiata o non e' modificabile");
                return;
            }
            leased_ = true;
            proxy_ = Call<int>(0x9A294B2138ABB884ull, root.modelHash, p.x, p.y, p.z,
                               false, false, true); // CREATE_OBJECT_NO_OFFSET
            if (!proxy_ || !Call<BOOL>(DoesEntityExist, proxy_)) {
                Reset();
                status_ = "Creazione proxy fallita";
                return;
            }
            Call(SetEntityCollision, proxy_, false, false);
            Call(0xAD738C3085FE7E11ull, proxy_, true, true); // SET_ENTITY_AS_MISSION_ENTITY
            Call(0xEA1C610A04DB6BBBull, proxy_, false, false); // SET_ENTITY_VISIBLE
            Call(0x428CA6DBD1094446ull, proxy_, true);        // FREEZE_ENTITY_POSITION
            const auto q = root.rotation;
            Call(0x77B21BE7AC540F07ull, proxy_, q.x, q.y, q.z, q.w);
            Call(RemoveModel, root.modelHash);
            loading_ = false;
        }

        if (!api_.apply(lease_, target)) {
            Reset();
            status_ = "Aggiornamento rifiutato | ripristino eseguito";
            return;
        }
        Call(SetEntityCoordsNoOffset, proxy_, p.x, p.y, p.z, false, false, false);
        Call(SetEntityCollision, proxy_, true, true);
        offset_ = requestedOffset_;
        pending_ = false;
        status_ = "";
    }

    void Reset() {
        discardProbe_ = true;
        ResetSelection();
    }

private:
    enum class TrialPhase { Idle, OriginBefore, DestinationBefore, OriginAfter, DestinationAfter };

    void StartTrialProbe(TrialPhase next, Vec3 center) {
        const auto half = trialNormal_ * 0.55f;
        trialProbe_ = StartRaycast({center + half, center - half, ray_.ignore});
        if (!trialProbe_) {
            FinishTrial("Prova annullata: raycast di verifica rifiutato");
            return;
        }
        trialPhase_ = next;
        trialStarted_ = GetTickCount64();
        LogDiagnostic("Static trial phase=%d", int(next));
        status_ = "Verifica collisione in corso...";
    }

    void FinishTrial(const char* message) {
        const bool restored = !trialMoved_ || api_.setTrialPosition(trial_, {});
        LogDiagnostic("Static trial finish: %s (rollback=%d)", message, int(restored));
        trialMoved_ = !restored;
        trialPhase_ = TrialPhase::Idle;
        if (restored) offset_ = {};
        status_ = restored ? message : "Ripristino rifiutato: istanza non piu' valida";
    }

    void ResetSelection() {
        if (trialMoved_ || trialPhase_ != TrialPhase::Idle) FinishTrial("Static trial reset");
        trial_ = {};
        trialMoved_ = false;
        if (propMoved_ && SameProp()) {
            Call(SetEntityCoordsNoOffset, prop_, propOrigin_.x, propOrigin_.y, propOrigin_.z,
                 true, true, true);
        }
        prop_ = 0;
        propMoved_ = false;
        if (proxy_ && Call<BOOL>(DoesEntityExist, proxy_)) {
            Call(SetEntityCollision, proxy_, false, false);
            Call(0xAE3CBE5BF394C9C9ull, &proxy_); // DELETE_ENTITY
        }
        if (leased_) api_.restoreAndRelease(lease_);
        CancelPending("");
        proxy_ = 0;
        leased_ = false;
        selected_ = false;
        offset_ = {};
        chain_ = {};
        std::snprintf(label_, sizeof(label_), "E  Seleziona davanti al mirino");
    }
    void CancelPending(const char* message) {
        if (loading_) Call(RemoveModel, chain_.entry[0].modelHash);
        loading_ = false;
        pending_ = false;
        status_ = message;
    }

    RageApi api_;
    Chain chain_;
    BuildingTrial trial_{};
    Hit trialHit_{};
    TrialPhase trialPhase_{};
    Vec3 trialNormal_{}, trialOffset_{};
    int trialProbe_{};
    std::uint64_t trialStarted_{};
    bool trialMoved_{};
    Ray ray_{};
    Vec3 pivot_, offset_, requestedOffset_;
    Vec3 propOrigin_{};
    int prop_{};
    std::uint32_t propModel_{};
    bool propMoved_{};
    bool selected_{}, pending_{}, loading_{}, leased_{};
    std::uint64_t lease_{}, started_{}, probeStarted_{};
    int proxy_{};
    int probe_{};
    bool discardProbe_{}, loggedFirstPoll_{};
    char label_[99]{"E  Seleziona davanti al mirino"};
    const char* status_{""};
};

void ScriptMain() {
    LogDiagnostic("MapSpooner v0.1.4 ScriptMain started");
    wchar_t executable[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto* name = length < MAX_PATH ? std::wcsrchr(executable, L'\\') : nullptr;
    if (!length || length >= MAX_PATH || !name || _wcsicmp(name + 1, L"GTA5.exe") != 0) {
        LogDiagnostic("Expected the GTA V Legacy GTA5.exe process.");
        return;
    }

    const Image image(GetModuleHandleW(nullptr));
    LogDiagnostic("Legacy PE timestamp=%08X size=%zu", image.Timestamp(), image.Size());
    const auto api = ResolveLegacy(image);
    Edit edit(api);
    bool selectWasDown{}, trialWasDown{}, resetWasDown{}, mouseWasDown{}, checkRendering{};
    int camera{}, dragAxis{-1};
    Vec3 position, rotation;
    float dragScreenX{}, dragScreenY{}, dragLength{}, dragAspect{1.f};

    for (;;) {
        DWORD foregroundProcess{};
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
        const bool focused = foregroundProcess == GetCurrentProcessId();
        const bool toggle = toggleRequested.exchange(false, std::memory_order_relaxed);
        const bool select = Down('E'), trial = Down('T'), reset = Down('R');
        const bool mouse = Down(VK_LBUTTON);
        const bool paused = (toggle || camera) && Call<BOOL>(IsPauseMenuActive) != 0;
        if (checkRendering) {
            LogDiagnostic("Camera rendering=%d", int(Call<BOOL>(0x02EC0AF5C5A49B7Aull, camera) != 0));
            checkRendering = false;
        }
        if (toggle) LogDiagnostic("F12 received; paused=%d", int(paused));
        if (toggle && !paused) {
            if (camera) {
                edit.Reset();
                dragAxis = -1;
                Call(0x07E5B515DB0636FCull, false, false, 0, true, false); // RENDER_SCRIPT_CAMS
                Call(0x865908C81A2C22E9ull, camera, false); // DESTROY_CAM
                Call(0x31B73D1EA9F01DA2ull); // CLEAR_FOCUS
                camera = 0;
                LogDiagnostic("Camera closed");
            } else {
                position = Unpack(Call<Vector3>(0x14D6F5678D8F1B37ull));
                rotation = Unpack(Call<Vector3>(0x837765A25378F0BBull, 2));
                const float fov = Call<float>(0x65019750A0324133ull); // GET_GAMEPLAY_CAM_FOV
                camera = Call<int>(0xB51194800B257161ull, "DEFAULT_SCRIPTED_CAMERA",
                                   position.x, position.y, position.z,
                                   rotation.x, rotation.y, rotation.z, fov, true, 2);
                LogDiagnostic("Create camera handle=%d", camera);
                if (camera && Call<BOOL>(0xA7A932170592B50Eull, camera)) {
                    Call(0x07E5B515DB0636FCull, true, false, 0, true, false);
                    checkRendering = true;
                } else {
                    camera = 0;
                }
            }
        }

        if (camera && !paused) {
            Call(0x5F4B6931816E599Bull, 0); // DISABLE_ALL_CONTROL_ACTIONS
            Call(0x719FF505F097FD20ull); // HIDE_HUD_AND_RADAR_THIS_FRAME
        }

        // Consume the pending result on a later script tick, even after closing F12.
        edit.PollPick();
        edit.PollTrial();
        if (camera && focused && !paused) {
            const float lookX = Call<float>(0x11E65974A982637Cull, 0, 1);
            const float lookY = Call<float>(0x11E65974A982637Cull, 0, 2);
            const float dt = std::clamp(Call<float>(0x15C40837039FFAF7ull), 0.f, 0.05f);
            if (!mouse) dragAxis = -1;
            if (reset && !resetWasDown) {
                edit.Reset();
                dragAxis = -1;
            }
            if (trial && !trialWasDown) edit.StartTrial();
            if (dragAxis < 0) {
                // Freeze camera rotation on the press frame so the gizmo stays under the reticle.
                if (!mouse || mouseWasDown) {
                    rotation.x = std::clamp(rotation.x - lookY * 8.f, -88.f, 88.f);
                    rotation.z = std::remainder(rotation.z - lookX * 8.f, 360.f);
                    rotation.y = 0.f;
                }
                const Vec3 forward = Forward(rotation);
                const Vec3 right = Forward({0.f, 0.f, rotation.z - 90.f});
                const Vec3 move = forward * static_cast<float>(int(Down('W')) - int(Down('S'))) +
                                  right * static_cast<float>(int(Down('D')) - int(Down('A'))) +
                                  Vec3{0, 0, static_cast<float>(int(Down(VK_SPACE)) - int(Down(VK_CONTROL)))};
                position = position + move * (dt * (Down(VK_SHIFT) ? 45.f : 12.f));
                if (select && !selectWasDown) {
                    LogDiagnostic("E received");
                    const int ped = Call<int>(0xD80958FC74E988A6ull); // PLAYER_PED_ID
                    edit.BeginPick({position, position + forward * 600.f, ped});
                }
            } else {
                edit.Move(Axes[dragAxis] * ((lookX * dragAspect * dragScreenX + lookY * dragScreenY) *
                                            dragLength));
            }

            if (edit.Selected()) {
                const Vec3 increment{
                    static_cast<float>(int(Down(VK_RIGHT)) - int(Down(VK_LEFT))),
                    static_cast<float>(int(Down(VK_UP)) - int(Down(VK_DOWN))),
                    static_cast<float>(int(Down(VK_PRIOR)) - int(Down(VK_NEXT)))
                };
                if (increment.x || increment.y || increment.z) edit.Move(increment * (5.f * dt));
            }
            edit.Update();
            Call(0x4D41783FB745E42Eull, camera, position.x, position.y, position.z);
            Call(0x85973643155D0B07ull, camera, rotation.x, rotation.y, rotation.z, 2);
            Call(0xBB7454BAFF08FE25ull, position.x, position.y, position.z, 0.f, 0.f, 0.f);
            if (edit.Selected()) {
                const auto gizmo = DrawGizmo(edit.Pivot(), position, dragAxis);
                if (mouse && !mouseWasDown && gizmo.hovered >= 0) {
                    if (edit.CanEdit()) {
                        dragAxis = gizmo.hovered;
                        dragScreenX = gizmo.screenX;
                        dragScreenY = gizmo.screenY;
                        dragLength = gizmo.length;
                        dragAspect = gizmo.aspect;
                    } else {
                        edit.Move({});
                    }
                }
            }

            Call(0x3A618A217E5154F0ull, 0.5f, 0.5f, 0.010f, 0.0018f, 255, 255, 255, 220, false);
            Call(0x3A618A217E5154F0ull, 0.5f, 0.5f, 0.0010f, 0.0170f, 255, 255, 255, 220, false);
            Text(0.055f, "MAP SPOONER 0.1.4 | F12 Esci | WASD Camera | Spazio/Ctrl Quota");
            Text(0.080f, edit.Label());
            if (edit.CanEdit()) Text(0.105f, "Frecce/PagSu/PagGiu o mouse sull'asse | R Ripristina");
            Text(0.155f, logLocation == LogLocation::Game ? "Log: cartella GTA V"
                         : logLocation == LogLocation::Temp ? "Log: %TEMP%/MapSpooner.log"
                         : "Log non scrivibile");
            if (*edit.Status()) Text(0.130f, edit.Status());
        } else {
            dragAxis = -1;
        }
        selectWasDown = select;
        trialWasDown = trial;
        resetWasDown = reset;
        mouseWasDown = mouse;
        scriptWait(0);
    }
}
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        keyboardHandlerRegister(spooner::OnKeyboard);
        scriptRegister(module, spooner::ScriptMain);
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        keyboardHandlerUnregister(spooner::OnKeyboard);
        scriptUnregister(module);
    }
    return TRUE;
}
