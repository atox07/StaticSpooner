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
#include <type_traits>

namespace spooner {
namespace {
HMODULE moduleHandle{};
std::atomic_bool toggleRequested{};

void OnKeyboard(DWORD key, WORD, BYTE, BOOL, BOOL, BOOL wasDown, BOOL isUp) {
    if (key == VK_F12 && !wasDown && !isUp) {
        toggleRequested.store(true, std::memory_order_relaxed);
    }
}
}

void LogDiagnostic(const char* format, ...) {
    wchar_t path[MAX_PATH]{};
    const auto length = GetModuleFileNameW(moduleHandle, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    auto* fileName = std::wcsrchr(path, L'\\');
    if (!fileName) return;
    ++fileName;
    if (wcscpy_s(fileName, MAX_PATH - (fileName - path), L"MapSpooner.log") != 0) return;
    FILE* file{};
    if (_wfopen_s(&file, path, L"a") != 0 || !file) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fclose(file);
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

void Text(float y, const char* message) {
    Call(0x07C837F9A01C34C9ull, 0.f, 0.30f);             // SET_TEXT_SCALE
    Call(0xBE6B23FFA53FB442ull, 255, 255, 255, 255);       // SET_TEXT_COLOUR
    Call(0x2513DFB0FB8400FEull);                          // SET_TEXT_OUTLINE
    Call(0x25FBB336DF1804CBull, "STRING");                // BEGIN_TEXT_COMMAND_DISPLAY_TEXT
    Call(0x6C188BE134E074AAull, message);                 // ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME
    Call(0xCD015E5BB0D96A57ull, 0.022f, y, 0);           // END_TEXT_COMMAND_DISPLAY_TEXT
}

constexpr std::array<Vec3, 3> Axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
constexpr int Colors[3][3]{{255, 70, 70}, {70, 255, 100}, {70, 145, 255}};
bool Project(Vec3 p, float& x, float& y) {
    return Call<BOOL>(0x34E82F05DF2974F5ull, p.x, p.y, p.z, &x, &y) != 0;
}

struct Gizmo {
    int hovered{-1};
    float screenX{}, screenY{}, length{};
};

Gizmo DrawGizmo(Vec3 pivot, Vec3 camera, int activeAxis) {
    const auto difference = pivot - camera;
    const float distance = std::sqrt(difference.x * difference.x +
                                     difference.y * difference.y +
                                     difference.z * difference.z);
    Gizmo result;
    result.length = std::clamp(distance * 0.10f, 0.7f, 18.f);
    float x{}, y{};
    const bool projected = Project(pivot, x, y);
    float nearest = 0.012f * 0.012f;
    for (int i = 0; i < 3; ++i) {
        const auto end = pivot + Axes[i] * result.length;
        float ex{}, ey{};
        if (projected && Project(end, ex, ey)) {
            const float dx = ex - x, dy = ey - y;
            const float lengthSquared = dx * dx + dy * dy;
            if (lengthSquared > 0.000025f) {
                const float t = std::clamp(((0.5f - x) * dx + (0.5f - y) * dy) /
                                          lengthSquared, 0.25f, 1.f);
                const float fromX = x + dx * t - 0.5f, fromY = y + dy * t - 0.5f;
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

    for (int i = 0; i < 3; ++i) {
        const auto end = pivot + Axes[i] * result.length;
        const bool highlight = i == activeAxis || (activeAxis < 0 && i == result.hovered);
        const auto& rgb = Colors[i];
        Call(0x6B7256074AE34680ull, pivot.x, pivot.y, pivot.z, end.x, end.y, end.z,
             highlight ? 255 : rgb[0], highlight ? 255 : rgb[1], highlight ? 255 : rgb[2], 255);
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
            std::snprintf(label_, sizeof(label_), "Hit mondo | CBuilding non risolta (handle 0)");
            return;
        }
        std::snprintf(label_, sizeof(label_), "Entita' colpita | handle %d", hit.handle);
    }

    bool CanEdit() const {
        return chain_.editable && api_.acquire && api_.apply && api_.restoreAndRelease;
    }
    bool Selected() const { return selected_; }
    Vec3 Pivot() const { return pivot_ + offset_; }
    const char* Label() const { return label_; }
    const char* Status() const { return status_; }

    void Move(Vec3 delta) {
        if (!CanEdit()) return;
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
    void ResetSelection() {
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
    Ray ray_{};
    Vec3 pivot_, offset_, requestedOffset_;
    bool selected_{}, pending_{}, loading_{}, leased_{};
    std::uint64_t lease_{}, started_{}, probeStarted_{};
    int proxy_{};
    int probe_{};
    bool discardProbe_{}, loggedFirstPoll_{};
    char label_[99]{"E  Seleziona davanti al mirino"};
    const char* status_{""};
};

void ScriptMain() {
    LogDiagnostic("MapSpooner v0.1.2 ScriptMain started");
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
    bool selectWasDown{}, resetWasDown{}, mouseWasDown{}, checkRendering{};
    int camera{}, dragAxis{-1};
    Vec3 position, rotation;
    float dragScreenX{}, dragScreenY{}, dragLength{};

    for (;;) {
        DWORD foregroundProcess{};
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
        const bool focused = foregroundProcess == GetCurrentProcessId();
        const bool toggle = toggleRequested.exchange(false, std::memory_order_relaxed);
        const bool select = Down('E'), reset = Down('R');
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
        if (camera && focused && !paused) {
            const float lookX = Call<float>(0x11E65974A982637Cull, 0, 1);
            const float lookY = Call<float>(0x11E65974A982637Cull, 0, 2);
            const float dt = std::clamp(Call<float>(0x15C40837039FFAF7ull), 0.f, 0.05f);
            if (!mouse) dragAxis = -1;
            if (reset && !resetWasDown) {
                edit.Reset();
                dragAxis = -1;
            }
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
                edit.Move(Axes[dragAxis] * ((lookX * dragScreenX + lookY * dragScreenY) * dragLength));
            }

            if (edit.CanEdit()) {
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
                if (edit.CanEdit() && mouse && !mouseWasDown && gizmo.hovered >= 0) {
                    dragAxis = gizmo.hovered;
                    dragScreenX = gizmo.screenX;
                    dragScreenY = gizmo.screenY;
                    dragLength = gizmo.length;
                }
            }

            Call(0x3A618A217E5154F0ull, 0.5f, 0.5f, 0.010f, 0.0018f, 255, 255, 255, 220, false);
            Call(0x3A618A217E5154F0ull, 0.5f, 0.5f, 0.0010f, 0.0170f, 255, 255, 255, 220, false);
            Text(0.055f, "MAP SPOONER | F12 Esci | WASD Camera | Spazio/Ctrl Quota");
            Text(0.080f, edit.Label());
            Text(0.105f, api.pick && api.inspect && api.acquire && api.apply && api.restoreAndRelease
                        ? "Frecce/PagSu/PagGiu o mouse sull'asse | R Ripristina"
                        : "Profilo RAGE assente | ispezione raycast");
            if (*edit.Status()) Text(0.130f, edit.Status());
        } else {
            dragAxis = -1;
        }
        selectWasDown = select;
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
        spooner::moduleHandle = module;
        keyboardHandlerRegister(spooner::OnKeyboard);
        scriptRegister(module, spooner::ScriptMain);
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        keyboardHandlerUnregister(spooner::OnKeyboard);
        scriptUnregister(module);
    }
    return TRUE;
}
