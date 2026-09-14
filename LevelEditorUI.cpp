#include "LevelEditorInternal.h"
#include "LevelEditorSoundTriggers.h"

using namespace asura;
using namespace asura::level;

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Winmm.lib")

namespace editor {

constexpr uint32_t kInspectorDirtyName = 1u << 0;
constexpr uint32_t kInspectorDirtyPosX = 1u << 1;
constexpr uint32_t kInspectorDirtyPosY = 1u << 2;
constexpr uint32_t kInspectorDirtyPosZ = 1u << 3;
constexpr uint32_t kInspectorDirtyRotX = 1u << 4;
constexpr uint32_t kInspectorDirtyRotY = 1u << 5;
constexpr uint32_t kInspectorDirtyRotZ = 1u << 6;
constexpr uint32_t kInspectorDirtyValueA = 1u << 7;
constexpr uint32_t kInspectorDirtyValueB = 1u << 8;
constexpr uint32_t kInspectorDirtyValueC = 1u << 21;
constexpr uint32_t kInspectorDirtyPickup = 1u << 9;
constexpr uint32_t kInspectorDirtySoundLoop = 1u << 10;
constexpr int kInspectorDirtySpawnTeamShift = 11;
constexpr int kInspectorDirtySpawnGameModeShift = 15;


AppState g;

int ui_px(int logical_pixels) {
    return MulDiv(logical_pixels, static_cast<int>(g.dpi), 96);
}

void rebuild_ui_font() {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
                                    g.dpi)) {
        if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            return;
        const UINT system_dpi = std::max<UINT>(96, GetDpiForSystem());
        metrics.lfMessageFont.lfHeight =
            MulDiv(metrics.lfMessageFont.lfHeight, static_cast<int>(g.dpi),
                   static_cast<int>(system_dpi));
    }
    HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
    if (!font)
        return;
    HFONT previous = g.font;
    g.font = font;
    if (g.window) {
        EnumChildWindows(
            g.window,
            [](HWND child, LPARAM value) -> BOOL {
                SendMessageA(child, WM_SETFONT, static_cast<WPARAM>(value), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(g.font));
    }
    if (previous)
        DeleteObject(previous);
}

bool valid_entity_index(int index) {
    return index >= 0 && index < static_cast<int>(g.document.entities.size());
}

size_t selection_index_of(const std::vector<int>& selection, int index) {
    for (size_t position = 0; position < selection.size(); ++position)
        if (selection[position] == index)
            return position;
    return selection.size();
}

bool entity_is_selected(int index) {
    return selection_index_of(g.selected_entities, index) != g.selected_entities.size();
}

void set_single_selection_state(int index) {
    g.selected_entities.clear();
    g.selected = valid_entity_index(index) ? index : -1;
    if (g.selected >= 0)
        g.selected_entities.push_back(g.selected);
}

void normalize_selection_state() {
    std::vector<int> normalized;
    normalized.reserve(g.selected_entities.size());
    for (int index : g.selected_entities) {
        if (valid_entity_index(index) &&
            selection_index_of(normalized, index) == normalized.size())
            normalized.push_back(index);
    }
    g.selected_entities = std::move(normalized);
    if (!entity_is_selected(g.selected))
        g.selected = g.selected_entities.empty() ? -1 : g.selected_entities.back();
}

void record_inspector_edit(int id, int notification) {
    if (g.refreshing_inspector)
        return;
    if (notification == EN_CHANGE) {
        switch (id) {
        case ID_NAME: g.inspector_dirty |= kInspectorDirtyName; break;
        case ID_POS_X: g.inspector_dirty |= kInspectorDirtyPosX; break;
        case ID_POS_Y: g.inspector_dirty |= kInspectorDirtyPosY; break;
        case ID_POS_Z: g.inspector_dirty |= kInspectorDirtyPosZ; break;
        case ID_ROT_X: g.inspector_dirty |= kInspectorDirtyRotX; break;
        case ID_ROT_Y: g.inspector_dirty |= kInspectorDirtyRotY; break;
        case ID_ROT_Z: g.inspector_dirty |= kInspectorDirtyRotZ; break;
        case ID_VALUE_A: g.inspector_dirty |= kInspectorDirtyValueA; break;
        case ID_VALUE_B: g.inspector_dirty |= kInspectorDirtyValueB; break;
        case ID_VALUE_C: g.inspector_dirty |= kInspectorDirtyValueC; break;
        }
    } else if (id == ID_PICKUP_ITEM && notification == CBN_SELCHANGE) {
        g.inspector_dirty |= kInspectorDirtyPickup;
    } else if (id == ID_SOUND_LOOP && notification == BN_CLICKED) {
        g.inspector_dirty |= kInspectorDirtySoundLoop;
    } else if (id >= ID_SPAWN_TEAM_FIRST && id <= ID_SPAWN_TEAM_LAST && notification == BN_CLICKED) {
        g.inspector_dirty |= 1u << (kInspectorDirtySpawnTeamShift + id - ID_SPAWN_TEAM_FIRST);
    } else if (id >= ID_SPAWN_GAME_MODE_FIRST && id <= ID_SPAWN_GAME_MODE_LAST &&
               notification == BN_CLICKED) {
        g.inspector_dirty |= 1u << (kInspectorDirtySpawnGameModeShift + id - ID_SPAWN_GAME_MODE_FIRST);
    }
}

void stop_sound_preview() {
    if (g.sound_preview_entity >= 0)
        PlaySoundA(nullptr, nullptr, 0);
    g.sound_preview_entity = -1;
    g.sound_preview_bytes.clear();
    if (g.sound_preview)
        SetWindowTextA(g.sound_preview, "Play preview");
}

void invalidate_environment_cache() { g.environment_cache_valid = false; }

void release_environment_cache() {
    if (g.environment_cache)
        DeleteObject(g.environment_cache);
    g.environment_cache = nullptr;
    g.environment_cache_width = 0;
    g.environment_cache_height = 0;
    g.environment_cache_valid = false;
}

void request_redraw() {
    if (g.window)
        InvalidateRect(g.window, nullptr, FALSE);
    if (g.viewport)
        InvalidateRect(g.viewport, nullptr, FALSE);
}

Asura_Vector_3 add(Asura_Vector_3 a, Asura_Vector_3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Asura_Vector_3 sub(Asura_Vector_3 a, Asura_Vector_3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Asura_Vector_3 mul(Asura_Vector_3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
Asura_Vector_3 entity_view_position(Asura_Vector_3 game_position) {
    game_position.y = -game_position.y;
    return game_position;
}
Asura_Vector_3 entity_view_direction(Asura_Vector_3 game_direction) {
    game_direction.y = -game_direction.y;
    return game_direction;
}
float dot(Asura_Vector_3 a, Asura_Vector_3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Asura_Vector_3 cross(Asura_Vector_3 a, Asura_Vector_3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Asura_Vector_3 normalized(Asura_Vector_3 a) {
    const float n = sqrtf(dot(a, a));
    return n > .00001f ? mul(a, 1.0f / n) : Asura_Vector_3{};
}

const SpawnPuppet* spawn_puppet_for_team(uint32_t team_mask) {
    if ((team_mask == 4 || team_mask == 5) && !g.spawn_puppets[0].faces.empty())
        return &g.spawn_puppets[0]; // Russian team
    if ((team_mask == 2 || team_mask == 3) && !g.spawn_puppets[1].faces.empty())
        return &g.spawn_puppets[1]; // German team
    if (team_mask == 1 && !g.spawn_puppets[2].faces.empty())
        return &g.spawn_puppets[2]; // Deathmatch
    return nullptr;
}

const EntityModel* pickup_model_for_skin(uint32_t skin_id) {
    for (const PickupModel& model : g.pickup_models)
        if (model.skin_id == skin_id && !model.mesh.faces.empty())
            return &model.mesh;
    return nullptr;
}

const EntityModel* static_object_model_for_file(uint32_t file_id) {
    for (const StaticObjectModel& model : g.static_object_models)
        if (model.file_id == file_id && !model.mesh.faces.empty())
            return &model.mesh;
    return nullptr;
}

const EntityModel* entity_render_model(const Entity& entity) {
    if (entity.kind == EntityKind::SpawnPoint)
        return spawn_puppet_for_team(entity.value_u32_a);
    if (entity.kind == EntityKind::Pickup)
        return pickup_model_for_skin(entity.pickup_skin_id);
    if (entity.kind == EntityKind::StaticObject) {
        if (const EntityModel* direct = static_object_model_for_file(entity.value_u32_b))
            return direct;
        return pickup_model_for_skin(entity.pickup_skin_id);
    }
    return nullptr;
}

Asura_Vector_3 rotate_by_quaternion(Asura_Vector_3 value, const Asura_Quat& rotation) {
    const Asura_Vector_3 q{rotation.x, rotation.y, rotation.z};
    const Asura_Vector_3 twice_cross = mul(cross(q, value), 2.0f);
    return add(value, add(mul(twice_cross, rotation.w), cross(q, twice_cross)));
}

Asura_Vector_3 entity_model_view_vector(Asura_Vector_3 value, const Entity& entity) {
    value = rotate_by_quaternion(value, euler_quaternion(entity.rotation));
    value.y = -value.y;
    return value;
}

Asura_Vector_3 entity_model_view_position(Asura_Vector_3 local_position, const Entity& entity) {
    return add(entity_view_position(entity.position), entity_model_view_vector(local_position, entity));
}

void append_light_gizmo_line(std::vector<LightGizmoLine>* lines, Asura_Vector_3 a, Asura_Vector_3 b) {
    lines->push_back({a, b});
}

void append_range_globe(Asura_Vector_3 center, float range, std::vector<LightGizmoLine>* lines) {
    constexpr float tau = 6.28318530717958647692f;
    for (int ring = 0; ring < 3; ++ring) {
        for (int segment = 0; segment < kLightRangeSegments; ++segment) {
            const float a0 = tau * segment / kLightRangeSegments;
            const float a1 = tau * (segment + 1) / kLightRangeSegments;
            const float c0 = cosf(a0) * range, s0 = sinf(a0) * range;
            const float c1 = cosf(a1) * range, s1 = sinf(a1) * range;
            Asura_Vector_3 p0{}, p1{};
            if (ring == 0) {
                p0 = {c0, s0, 0};
                p1 = {c1, s1, 0};
            } else if (ring == 1) {
                p0 = {c0, 0, s0};
                p1 = {c1, 0, s1};
            } else {
                p0 = {0, c0, s0};
                p1 = {0, c1, s1};
            }
            append_light_gizmo_line(lines, add(center, p0), add(center, p1));
        }
    }
}

void append_light_bounding_box(const Asura_Bounding_Box& bounds, std::vector<LightGizmoLine>* lines) {
    const float values[] = {bounds.MinX, bounds.MaxX, bounds.MinY,
                            bounds.MaxY, bounds.MinZ, bounds.MaxZ};
    for (float value : values)
        if (!isfinite(value))
            return;

    Asura_Vector_3 corners[8]{};
    for (int corner = 0; corner < 8; ++corner) {
        corners[corner] = entity_view_position({corner & 1 ? bounds.MaxX : bounds.MinX,
                                                corner & 2 ? bounds.MaxY : bounds.MinY,
                                                corner & 4 ? bounds.MaxZ : bounds.MinZ});
    }
    for (int corner = 0; corner < 8; ++corner) {
        for (int axis = 0; axis < 3; ++axis) {
            const int other = corner ^ (1 << axis);
            if (corner < other)
                append_light_gizmo_line(lines, corners[corner], corners[other]);
        }
    }
}

void append_camera_spawn_arrow(const Entity& entity, float marker, std::vector<LightGizmoLine>* lines) {
    if (entity.kind != EntityKind::SpawnPoint || !(entity.value_u32_a & SnipeSpawnTeam_Camera))
        return;
    Asura_Vector_3 direction = normalized(entity_view_direction(entity.spawn_direction));
    if (dot(direction, direction) <= .00001f)
        return;
    const Asura_Vector_3 start = entity_view_position(entity.position);
    const float length = fmaxf(1.0f, marker * 4.0f);
    const float head_length = length * .28f;
    const float head_width = length * .18f;
    const Asura_Vector_3 tip = add(start, mul(direction, length));
    const Asura_Vector_3 head_center = sub(tip, mul(direction, head_length));
    Asura_Vector_3 side = normalized(cross(direction, {0, 1, 0}));
    if (dot(side, side) <= .00001f)
        side = {1, 0, 0};
    const Asura_Vector_3 vertical = normalized(cross(direction, side));
    append_light_gizmo_line(lines, start, tip);
    append_light_gizmo_line(lines, tip, add(head_center, mul(side, head_width)));
    append_light_gizmo_line(lines, tip, sub(head_center, mul(side, head_width)));
    append_light_gizmo_line(lines, tip, add(head_center, mul(vertical, head_width)));
    append_light_gizmo_line(lines, tip, sub(head_center, mul(vertical, head_width)));
}

void append_sound_gizmo(const Entity& entity, std::vector<LightGizmoLine>* lines) {
    if (entity.kind != EntityKind::Sound)
        return;
    const float range = fabsf(entity.value_b);
    if (isfinite(range) && range > .00001f && range <= kMaximumLightGizmoRange)
        append_range_globe(entity_view_position(entity.position), range, lines);
    if (entity.sound_trigger_enabled)
        append_light_bounding_box(sound_trigger_bounds(entity), lines);
}

void append_light_gizmo(const Entity& entity, std::vector<LightGizmoLine>* lines) {
    if (entity.kind != EntityKind::Light)
        return;
    const Asura_Vector_3 center = entity_view_position(entity.position);
    const float range = fabsf(entity.light.Range);
    if (isfinite(range) && range > .00001f && range <= kMaximumLightGizmoRange)
        append_range_globe(center, range, lines);
    if (entity.light.m_uFlags & ASURA_LIGHT_FLAG_USE_BOUNDING_BOX)
        append_light_bounding_box(entity.light.m_xBoundingBox, lines);
}

RECT viewport_rect() {
    RECT r{};
    GetClientRect(g.window, &r);
    r.left = ui_px(238);
    r.top = ui_px(74);
    r.right -= ui_px(272);
    r.bottom -= ui_px(25);
    if (r.right < r.left + ui_px(40))
        r.right = r.left + ui_px(40);
    if (r.bottom < r.top + ui_px(40))
        r.bottom = r.top + ui_px(40);
    return r;
}

void camera_axes(Asura_Vector_3* position, Asura_Vector_3* right, Asura_Vector_3* up, Asura_Vector_3* forward) {
    const float cp = cosf(g.camera.pitch), sp = sinf(g.camera.pitch);
    *position = add(g.camera.target,
                    {sinf(g.camera.yaw) * cp * g.camera.distance, sp * g.camera.distance,
                     cosf(g.camera.yaw) * cp * g.camera.distance});
    *forward = normalized(sub(g.camera.target, *position));
    *right = normalized(cross({0, 1, 0}, *forward));
    *up = normalized(cross(*forward, *right));
}

bool project_point(const Asura_Vector_3& p, POINT* screen, float* depth = nullptr) {
    const RECT vr = viewport_rect();
    Asura_Vector_3 cam, right, up, forward;
    camera_axes(&cam, &right, &up, &forward);
    const Asura_Vector_3 rel = sub(p, cam);
    const float z = dot(rel, forward);
    if (z <= .05f)
        return false;
    const float f = .85f * static_cast<float>(std::min(vr.right - vr.left, vr.bottom - vr.top));
    screen->x = static_cast<LONG>((vr.left + vr.right) * .5f + dot(rel, right) * f / z);
    screen->y = static_cast<LONG>((vr.top + vr.bottom) * .5f - dot(rel, up) * f / z);
    if (depth)
        *depth = z;
    return true;
}

bool screen_ray(int x, int y, EnvironmentRay* ray) {
    if (!ray)
        return false;
    const RECT vr = viewport_rect();
    Asura_Vector_3 cam, right, up, forward;
    camera_axes(&cam, &right, &up, &forward);
    const float f = .85f * static_cast<float>(std::min(vr.right - vr.left, vr.bottom - vr.top));
    if (f <= 0.0f)
        return false;
    const float sx = (x - (vr.left + vr.right) * .5f) / f;
    const float sy = -(y - (vr.top + vr.bottom) * .5f) / f;
    ray->origin = cam;
    ray->direction = normalized(add(forward, add(mul(right, sx), mul(up, sy))));
    return dot(ray->direction, ray->direction) > .5f;
}

bool environment_point_from_screen(int x, int y, Asura_Vector_3* game_point) {
    EnvironmentRay ray{};
    EnvironmentRayHit hit{};
    if (!game_point || !screen_ray(x, y, &ray) || !g.environment_raycast.intersect(g.mesh, ray, &hit))
        return false;
    // Mesh vertices are in editor/view coordinates; gameplay entities retain
    // the target's negative-up Y convention.
    *game_point = entity_view_position(hit.position);
    return true;
}

bool environment_occludes_view_position(const Asura_Vector_3& view_position) {
    Asura_Vector_3 camera_position{}, right{}, up{}, forward{};
    camera_axes(&camera_position, &right, &up, &forward);
    const Asura_Vector_3 to_position = sub(view_position, camera_position);
    const float distance_squared = dot(to_position, to_position);
    if (distance_squared <= 1.0e-8f)
        return false;
    const float distance = sqrtf(distance_squared);
    EnvironmentRayHit hit{};
    const EnvironmentRay ray{camera_position, mul(to_position, 1.0f / distance)};
    if (!g.environment_raycast.intersect(g.mesh, ray, &hit))
        return false;
    const float surface_epsilon = fmaxf(.01f, distance * 1.0e-4f);
    return hit.distance + surface_epsilon < distance;
}


void set_status(const char* text) { SetWindowTextA(g.status, text ? text : ""); }

void update_title() {
    std::string title = "Asura 2005 Level Editor";
    const std::string& display_path = !g.document.project_path.empty() ? g.document.project_path
                                      : !g.document.source_pc_path.empty() ? g.document.source_pc_path
                                                                          : g.document.obj_path;
    if (!display_path.empty()) {
        const size_t slash = display_path.find_last_of("\\/");
        title += " - " + display_path.substr(slash == std::string::npos ? 0 : slash + 1);
    }
    if (g.document.dirty)
        title += " *";
    SetWindowTextA(g.window, title.c_str());
}

bool commit_history_transaction() {
    const bool changed = g.history.commit(&g.document, g.selected);
    update_title();
    return changed;
}

void reset_history(bool mark_as_saved) {
    g.history.reset(&g.document, g.selected, mark_as_saved);
    update_title();
}

bool choose_path(HWND owner, bool save, const char* title, const char* filter, const char* extension,
                 std::string* path) {
    char buffer[MAX_PATH * 4]{};
    if (!path->empty())
        strncpy_s(buffer, path->c_str(), _TRUNCATE);
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = sizeof(buffer);
    ofn.lpstrDefExt = extension;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn)))
        return false;
    *path = buffer;
    return true;
}

int CALLBACK initialize_directory_picker(HWND dialog, UINT message, LPARAM, LPARAM initial_path) {
    if (message == BFFM_INITIALIZED && initial_path)
        SendMessageA(dialog, BFFM_SETSELECTIONA, TRUE, initial_path);
    return 0;
}

bool choose_directory(HWND owner, const char* title, std::string* path) {
    char display_name[MAX_PATH]{};
    const HRESULT initialized = OleInitialize(nullptr);
    BROWSEINFOA info{};
    info.hwndOwner = owner;
    info.pszDisplayName = display_name;
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    info.lpfn = initialize_directory_picker;
    info.lParam = path->empty() ? 0 : reinterpret_cast<LPARAM>(path->c_str());
    PIDLIST_ABSOLUTE selected = SHBrowseForFolderA(&info);
    char selected_path[MAX_PATH]{};
    const bool accepted = selected && SHGetPathFromIDListA(selected, selected_path);
    CoTaskMemFree(selected);
    if (SUCCEEDED(initialized))
        OleUninitialize();
    if (!accepted)
        return false;
    *path = selected_path;
    return true;
}

std::string folder_from_path(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

std::string basename_without_extension(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    size_t end = path.find_last_of('.');
    if (end == std::string::npos || end < start)
        end = path.size();
    return path.substr(start, end - start);
}

bool decode_spawn_puppet(const RscfInfo& resource, const char* expected_name, SpawnPuppet* output) {
    if (resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
        resource.subtype != ASURA_RESOURCEFILE_TYPE_PC_CHARACTER || !str_ieq_c(resource.name, expected_name))
        return false;
    const Str embedded_name = padded_string_at(resource.payload, resource.payload_size, 0);
    if (!embedded_name.data || !str_ieq_c(embedded_name, expected_name))
        return false;
    const uint64_t counts_at = align_up(static_cast<uint64_t>(embedded_name.size) + 1, 4);
    if (counts_at + 16 > resource.payload_size)
        return false;
    const uint32_t triangle_windows = read_u32(resource.payload + counts_at);
    const uint32_t vertex_count = read_u32(resource.payload + counts_at + 4);
    const uint32_t index_count = read_u32(resource.payload + counts_at + 8);
    constexpr uint32_t vertex_stride = 64;
    const uint64_t vertices_at = counts_at + 16;
    const uint64_t indices_at = vertices_at + static_cast<uint64_t>(vertex_count) * vertex_stride;
    const uint64_t required = indices_at + static_cast<uint64_t>(index_count) * sizeof(uint16_t);
    if (!vertex_count || vertex_count > 65535 || index_count < 3 || triangle_windows != index_count - 2 ||
        required > resource.payload_size)
        return false;

    SpawnPuppet next;
    next.resource_name = expected_name;
    next.vertices.resize(vertex_count);
    for (uint32_t i = 0; i < vertex_count; ++i) {
        const uint8_t* source = resource.payload + vertices_at + static_cast<uint64_t>(i) * vertex_stride;
        EntityModelVertex& vertex = next.vertices[i];
        vertex.position = {read_f32(source), read_f32(source + 4), read_f32(source + 8)};
        vertex.normal = {read_f32(source + 12), read_f32(source + 16), read_f32(source + 20)};
        if (!isfinite(vertex.position.x) || !isfinite(vertex.position.y) || !isfinite(vertex.position.z) ||
            !isfinite(vertex.normal.x) || !isfinite(vertex.normal.y) || !isfinite(vertex.normal.z))
            return false;
        vertex.normal = normalized(vertex.normal);
        if (dot(vertex.normal, vertex.normal) < .5f)
            vertex.normal = {0, -1, 0};
        if (i == 0) {
            next.min = next.max = vertex.position;
        } else {
            next.min.x = fminf(next.min.x, vertex.position.x);
            next.min.y = fminf(next.min.y, vertex.position.y);
            next.min.z = fminf(next.min.z, vertex.position.z);
            next.max.x = fmaxf(next.max.x, vertex.position.x);
            next.max.y = fmaxf(next.max.y, vertex.position.y);
            next.max.z = fmaxf(next.max.z, vertex.position.z);
        }
    }

    const uint8_t* strip_data = resource.payload + indices_at;
    next.faces.reserve(index_count - 2);
    for (uint32_t window = 0; window + 2 < index_count; ++window) {
        uint16_t a = read_u16(strip_data + window * 2);
        uint16_t b = read_u16(strip_data + (window + 1) * 2);
        const uint16_t c = read_u16(strip_data + (window + 2) * 2);
        if (window & 1)
            std::swap(a, b);
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count)
            return false;
        if (a != b && b != c && a != c)
            next.faces.push_back({a, b, c});
    }
    if (next.faces.empty())
        return false;
    *output = std::move(next);
    return true;
}

bool load_spawn_puppets() {
    wchar_t path[MAX_PATH]{};
    const DWORD folder_length = executable_folder(path, static_cast<DWORD>(_countof(path)));
    if (!folder_length) {
        g.spawn_puppets = {};
        g.spawn_puppets_loaded = false;
        return false;
    }
    constexpr wchar_t suffixes[][32] = {
        L"\\MPChars.asr", L"\\Misc\\MPChars\\MPChars.asr"};
    for (const auto& suffix : suffixes) {
        size_t suffix_length = 0;
        while (suffix[suffix_length])
            ++suffix_length;
        if (folder_length + suffix_length >= _countof(path))
            continue;
        memcpy(path + folder_length, suffix, (suffix_length + 1) * sizeof(wchar_t));
        if (!file_exists(path))
            continue;
        Arena arena{};
        Error error{};
        ChunkList chunks{};
        std::array<SpawnPuppet, 3> puppets;
        bool parsed = arena_init(&arena, 16 * MiB, &error) && parse_chunks(path, &chunks, &arena, &error);
        if (parsed) {
            for (uint32_t i = 0; i < chunks.count; ++i) {
                RscfInfo resource{};
                if (!rscf_info(chunks.chunks[i], &resource))
                    continue;
                if (puppets[0].faces.empty())
                    decode_spawn_puppet(resource, "russian_soldier9", &puppets[0]);
                if (puppets[1].faces.empty())
                    decode_spawn_puppet(resource, "german_elite6", &puppets[1]);
                if (puppets[2].faces.empty())
                    decode_spawn_puppet(resource, "player", &puppets[2]);
            }
        }
        unmap_file(&chunks.file);
        arena_release(&arena);
        if (parsed && !puppets[0].faces.empty() && !puppets[1].faces.empty() && !puppets[2].faces.empty()) {
            g.spawn_puppets = std::move(puppets);
            g.spawn_puppets_loaded = true;
            return true;
        }
    }
    g.spawn_puppets = {};
    g.spawn_puppets_loaded = false;
    return false;
}

void set_control_text(HWND control, const char* text) { SetWindowTextA(control, text ? text : ""); }

void set_float(HWND control, float value) {
    char text[384]{};
    const std::to_chars_result result = std::to_chars(text, text + sizeof(text) - 1, value,
                                                       std::chars_format::fixed, 15);
    if (result.ec == std::errc{}) {
        char* end = result.ptr;
        while (end > text && end[-1] == '0')
            --end;
        if (end > text && end[-1] == '.')
            *end++ = '0';
        *end = 0;
        SetWindowTextA(control, text);
    }
}

float get_float(HWND control, float fallback) {
    char text[128]{};
    GetWindowTextA(control, text, sizeof(text));
    char* end = nullptr;
    const float v = strtof(text, &end);
    return end != text && isfinite(v) ? v : fallback;
}

void set_u32_hex(HWND control, uint32_t value) {
    char text[32];
    snprintf(text, sizeof(text), "0x%08X", value);
    SetWindowTextA(control, text);
}

uint32_t get_u32(HWND control, uint32_t fallback) {
    char text[128]{};
    GetWindowTextA(control, text, sizeof(text));
    char* end = nullptr;
    const unsigned long value = strtoul(text, &end, 0);
    while (end && *end == ' ')
        ++end;
    return end != text && end && !*end && value <= 0xfffffffful ? static_cast<uint32_t>(value) : fallback;
}

struct SpawnMaskControl {
    const char* label;
    uint32_t mask;
};

constexpr SpawnMaskControl kSpawnTeamControls[] = {
    {"Teamless", SnipeSpawnTeam_Deathmatch},
    {"Germany", SnipeSpawnTeam_German},
    {"Russia", SnipeSpawnTeam_Russian},
    {"Camera-only", SnipeSpawnTeam_Camera}
};

constexpr SpawnMaskControl kSpawnGameModeControls[] = {
    {"Single-Player", SnipeSpawnGameMode_SinglePlayer},
    {"Cooperative", SnipeSpawnGameMode_LocalCooperative},
    {"Deathmatch", SnipeSpawnGameMode_Deathmatch},
    {"TDM", SnipeSpawnGameMode_TeamDeathmatch},
    {"Manhunt", SnipeSpawnGameMode_Manhunt},
    {"Assassination", SnipeSpawnGameMode_Assassination}
};

enum LightPropertiesId : int {
    ID_LIGHT_POSITION_X = 3000,
    ID_LIGHT_POSITION_Y,
    ID_LIGHT_POSITION_Z,
    ID_LIGHT_R,
    ID_LIGHT_G,
    ID_LIGHT_B,
    ID_LIGHT_BRIGHTNESS,
    ID_LIGHT_RANGE,
    ID_LIGHT_INNER_RANGE,
    ID_LIGHT_SHADOW_STRENGTH,
    ID_LIGHT_BOUND_MIN_X,
    ID_LIGHT_BOUND_MAX_X,
    ID_LIGHT_BOUND_MIN_Y,
    ID_LIGHT_BOUND_MAX_Y,
    ID_LIGHT_BOUND_MIN_Z,
    ID_LIGHT_BOUND_MAX_Z,
    ID_LIGHT_FLAGS,
    ID_LIGHT_FLAG_FIRST,
    ID_LIGHT_FLAG_LAST = ID_LIGHT_FLAG_FIRST + 6,
    ID_LIGHT_OLD_POSITION_X,
    ID_LIGHT_OLD_POSITION_Y,
    ID_LIGHT_OLD_POSITION_Z,
    ID_LIGHT_OLD_RANGE,
    ID_LIGHT_HAS_CHANGED
};

struct LightFlagControl {
    const char* label;
    uint32_t mask;
};

constexpr LightFlagControl kLightFlagControls[] = {
    {"Has corona", ASURA_LIGHT_FLAG_HAS_CORONA},
    {"Infinite light", ASURA_LIGHT_FLAG_INFINITE_LIGHT},
    {"Affects entities", ASURA_LIGHT_FLAG_AFFECTS_ENTITIES},
    {"Affects environment", ASURA_LIGHT_FLAG_AFFECTS_ENVIRONMENT},
    {"Volumetric", ASURA_LIGHT_FLAG_IS_VOLUMETRIC},
    {"Use bounding box", ASURA_LIGHT_FLAG_USE_BOUNDING_BOX},
    {"Shadow volume", ASURA_LIGHT_FLAG_IS_SHADOW_VOLUME}
};

struct LightPropertiesState {
    HWND window = nullptr;
    HWND position[3]{};
    HWND colour[3]{};
    HWND brightness = nullptr;
    HWND range = nullptr;
    HWND inner_range = nullptr;
    HWND shadow_strength = nullptr;
    HWND bounds[6]{};
    HWND flags = nullptr;
    HWND flag_checks[_countof(kLightFlagControls)]{};
    Asura_Light value{};
    bool accepted = false;
};

void update_light_flag_dependent_controls(LightPropertiesState* state, uint32_t flags) {
    const BOOL shadow_volume = (flags & ASURA_LIGHT_FLAG_IS_SHADOW_VOLUME) != 0;
    const BOOL use_bounding_box = (flags & ASURA_LIGHT_FLAG_USE_BOUNDING_BOX) != 0;
    for (HWND field : state->colour)
        EnableWindow(field, !shadow_volume);
    EnableWindow(state->brightness, !shadow_volume);
    EnableWindow(state->shadow_strength, shadow_volume);
    for (HWND field : state->bounds)
        EnableWindow(field, use_bounding_box);
}

HWND make_dialog_control(HWND parent, const char* cls, const char* text, DWORD style, int id,
                         int x, int y, int width, int height) {
    HWND control = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                   ui_px(x), ui_px(y), ui_px(width), ui_px(height),
                                   parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandle(nullptr), nullptr);
    SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return control;
}

bool run_centered_modal(const char* window_class, const char* title,
                        int width_units, int height_units, void* create_param) {
    RECT owner{};
    GetWindowRect(g.window, &owner);
    const int width = ui_px(width_units), height = ui_px(height_units);
    const int x = owner.left + std::max(0L, (owner.right - owner.left - width) / 2);
    const int y = owner.top + std::max(0L, (owner.bottom - owner.top - height) / 2);
    HWND window = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, window_class, title,
                                  WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
                                  x, y, width, height, g.window, nullptr,
                                  GetModuleHandle(nullptr), create_param);
    if (!window)
        return false;
    EnableWindow(g.window, FALSE);
    MSG message{};
    bool quit = false;
    while (IsWindow(window)) {
        const BOOL result = GetMessageA(&message, nullptr, 0, 0);
        if (result <= 0) {
            quit = result == 0;
            break;
        }
        if (!IsDialogMessageA(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
    EnableWindow(g.window, TRUE);
    SetActiveWindow(g.window);
    if (quit)
        PostQuitMessage(static_cast<int>(message.wParam));
    return true;
}

enum SoundPropertiesId { ID_SOUND_START = 2100, ID_SOUND_TRIGGER, ID_SOUND_ONCE, ID_SOUND_EXIT,
                         ID_SOUND_TRIGGER_OFFSET, ID_SOUND_TRIGGER_SIZE = ID_SOUND_TRIGGER_OFFSET + 3 };
struct SoundPropertiesState {
    HWND window = nullptr, start = nullptr, trigger = nullptr, once = nullptr, stop = nullptr;
    HWND offset[3]{}, size[3]{};
    Entity value;
    bool accepted = false;
};

void update_sound_properties(SoundPropertiesState* state) {
    const bool enabled = SendMessageA(state->trigger, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool once = SendMessageA(state->once, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(state->start, !enabled);
    EnableWindow(state->once, enabled);
    EnableWindow(state->stop, enabled && !once);
    if (once) SendMessageA(state->stop, BM_SETCHECK, BST_UNCHECKED, 0);
    for (HWND field : state->offset) EnableWindow(field, enabled);
    for (HWND field : state->size) EnableWindow(field, enabled);
}

LRESULT CALLBACK sound_properties_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        SetWindowLongPtrA(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTA*>(lparam)->lpCreateParams));
        return TRUE;
    }
    auto* state = reinterpret_cast<SoundPropertiesState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (message == WM_CREATE) {
        state->window = hwnd;
        const DWORD check = BS_AUTOCHECKBOX | WS_TABSTOP;
        state->start = make_dialog_control(hwnd, "BUTTON", "Play when the level starts", check,
                                           ID_SOUND_START, 18, 16, 480, 24);
        state->trigger = make_dialog_control(hwnd, "BUTTON", "Use bounding box", check,
                                             ID_SOUND_TRIGGER, 18, 48, 480, 24);
        state->once = make_dialog_control(hwnd, "BUTTON", "Trigger once", check,
                                          ID_SOUND_ONCE, 38, 82, 240, 24);
        state->stop = make_dialog_control(hwnd, "BUTTON", "Stop when all players leave the box", check,
                                          ID_SOUND_EXIT, 38, 114, 460, 24);
        make_dialog_control(hwnd, "STATIC", "With a bounding box, playback starts when one of the players enter the box.",
                            SS_LEFT, 0, 18, 154, 510, 42);
        constexpr const char* axes[] = {"X", "Y", "Z"};
        for (int row = 0; row < 2; ++row) {
            make_dialog_control(hwnd, "STATIC", row ? "Box size" : "Offset from sound", SS_LEFT,
                                0, 18, 216 + row * 38, 132, 24);
            for (int axis = 0; axis < 3; ++axis) {
                const int x = 158 + axis * 118, y = 212 + row * 38;
                make_dialog_control(hwnd, "STATIC", axes[axis], SS_LEFT, 0, x, y + 3, 18, 24);
                HWND field = make_dialog_control(hwnd, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                    (row ? ID_SOUND_TRIGGER_SIZE : ID_SOUND_TRIGGER_OFFSET) + axis, x + 20, y, 84, 24);
                (row ? state->size : state->offset)[axis] = field;
            }
        }
        const auto& e = state->value;
        SendMessageA(state->start, BM_SETCHECK, e.sound_controller_active ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageA(state->trigger, BM_SETCHECK, e.sound_trigger_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageA(state->once, BM_SETCHECK, e.sound_trigger_once ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageA(state->stop, BM_SETCHECK, e.sound_stop_on_exit ? BST_CHECKED : BST_UNCHECKED, 0);
        const float offset[] = {e.sound_trigger_offset.x, e.sound_trigger_offset.y, e.sound_trigger_offset.z};
        const float size[] = {e.sound_trigger_size.x, e.sound_trigger_size.y, e.sound_trigger_size.z};
        for (int axis = 0; axis < 3; ++axis) {
            set_float(state->offset[axis], offset[axis]); set_float(state->size[axis], size[axis]);
        }
        make_dialog_control(hwnd, "BUTTON", "Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 310, 354, 104, 30);
        make_dialog_control(hwnd, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 422, 354, 104, 30);
        update_sound_properties(state);
        return 0;
    }
    if (message == WM_COMMAND) {
        const int id = LOWORD(wparam);
        if (id == ID_SOUND_TRIGGER || id == ID_SOUND_ONCE) update_sound_properties(state);
        if (id == IDOK) {
            Entity& e = state->value;
            e.sound_trigger_enabled = SendMessageA(state->trigger, BM_GETCHECK, 0, 0) == BST_CHECKED;
            e.sound_trigger_once = SendMessageA(state->once, BM_GETCHECK, 0, 0) == BST_CHECKED;
            e.sound_stop_on_exit = SendMessageA(state->stop, BM_GETCHECK, 0, 0) == BST_CHECKED;
            e.sound_controller_active = SendMessageA(state->start, BM_GETCHECK, 0, 0) == BST_CHECKED;
            float values[6]{};
            for (int i = 0; i < 6; ++i) {
                char text[128]{}; char* end = nullptr;
                GetWindowTextA(i < 3 ? state->offset[i] : state->size[i-3], text, sizeof(text));
                values[i] = strtof(text, &end);
                const bool parsed = end != text;
                while (*end == ' ' || *end == '\t') ++end;
                if (e.sound_trigger_enabled && (!parsed || *end || !isfinite(values[i]) ||
                                                 (i >= 3 && values[i] <= 0))) {
                    MessageBoxA(hwnd, "Enter finite box offsets and a positive size on every axis.",
                                "Sound playback", MB_OK | MB_ICONWARNING);
                    return 0;
                }
            }
            if (e.sound_trigger_enabled) {
                e.sound_trigger_offset = {values[0], values[1], values[2]};
                e.sound_trigger_size = {values[3], values[4], values[5]};
            }
            e.sound_has_controller = true;
            state->accepted = true;
            DestroyWindow(hwnd);
        } else if (id == IDCANCEL) DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

void command_sound_properties() {
    if (!valid_entity_index(g.selected) || g.document.entities[g.selected].kind != EntityKind::Sound) return;
    SoundPropertiesState state;
    state.value = g.document.entities[g.selected];
    if (!run_centered_modal("Asura2005SoundProperties", "Sound playback", 560, 438, &state) ||
        !state.accepted || !g.history.begin(g.document, g.selected)) return;
    g.document.entities[g.selected] = std::move(state.value);
    commit_history_transaction();
    refresh_inspector(); request_redraw();
}

void make_light_vector_row(LightPropertiesState* state, const char* label, int y, int first_id, HWND fields[3]) {
    make_dialog_control(state->window, "STATIC", label, SS_LEFT, 0, 14, y + 3, 94, 22);
    constexpr const char* axes[] = {"X", "Y", "Z"};
    constexpr int xs[] = {130, 334, 538};
    for (int i = 0; i < 3; ++i) {
        make_dialog_control(state->window, "STATIC", axes[i], SS_LEFT, 0, xs[i] - 18, y + 3, 16, 22);
        fields[i] = make_dialog_control(state->window, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                       first_id + i, xs[i], y, 142, 24);
    }
}

void make_light_scalar(LightPropertiesState* state, const char* label, int id, int x, int y, HWND* field) {
    make_dialog_control(state->window, "STATIC", label, SS_LEFT, 0, x, y + 3, 112, 22);
    *field = make_dialog_control(state->window, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, id,
                                x + 116, y, 104, 24);
}

void create_light_properties_controls(LightPropertiesState* state) {
    make_light_vector_row(state, "Position", 16, ID_LIGHT_POSITION_X, state->position);
    make_light_vector_row(state, "Colour (RGB255)", 50, ID_LIGHT_R, state->colour);

    constexpr const char* scalar_labels[] = {"Brightness", "Range", "Inner range", "Shadow strength"};
    constexpr int scalar_ids[] = {ID_LIGHT_BRIGHTNESS, ID_LIGHT_RANGE, ID_LIGHT_INNER_RANGE,
                                  ID_LIGHT_SHADOW_STRENGTH};
    constexpr int scalar_x[] = {14, 262, 510, 14};
    constexpr int scalar_y[] = {88, 88, 88, 122};
    HWND* scalar_outputs[] = {&state->brightness, &state->range, &state->inner_range,
                              &state->shadow_strength};
    for (size_t i = 0; i < _countof(scalar_ids); ++i)
        make_light_scalar(state, scalar_labels[i], scalar_ids[i], scalar_x[i], scalar_y[i], scalar_outputs[i]);

    make_dialog_control(state->window, "STATIC", "World-axis bounding box", SS_LEFT, 0, 14, 164, 150, 22);
    constexpr const char* bound_labels[] = {"Min X", "Max X", "Min Y", "Max Y", "Min Z", "Max Z"};
    constexpr int bound_ids[] = {ID_LIGHT_BOUND_MIN_X, ID_LIGHT_BOUND_MAX_X, ID_LIGHT_BOUND_MIN_Y,
                                 ID_LIGHT_BOUND_MAX_Y, ID_LIGHT_BOUND_MIN_Z, ID_LIGHT_BOUND_MAX_Z};
    for (int i = 0; i < 6; ++i) {
        const int column = i % 3, row = i / 3;
        const int x = 130 + column * 204, y = 160 + row * 34;
        make_dialog_control(state->window, "STATIC", bound_labels[i], SS_LEFT, 0, x, y + 3, 48, 22);
        state->bounds[i] = make_dialog_control(state->window, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                              bound_ids[i], x + 52, y, 90, 24);
    }

    make_dialog_control(state->window, "STATIC", "Raw flags", SS_LEFT, 0, 14, 236, 94, 22);
    state->flags = make_dialog_control(state->window, "EDIT", "", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER | WS_TABSTOP,
                                      ID_LIGHT_FLAGS, 130, 233, 142, 24);
    for (int i = 0; i < static_cast<int>(_countof(kLightFlagControls)); ++i) {
        const int column = i % 4, row = i / 4;
        state->flag_checks[i] = make_dialog_control(
            state->window, "BUTTON", kLightFlagControls[i].label, BS_AUTOCHECKBOX | WS_TABSTOP,
            ID_LIGHT_FLAG_FIRST + i, 14 + column * 185, 270 + row * 30, 178, 24);
    }

    // These legacy controls have no effect on the target's static entity
    // lighting path (0x4432B0 / 0x443810 / 0x487750). Preserve their imported values.
    EnableWindow(state->inner_range, FALSE);
    for (int index : {0, 1, 3, 4}) EnableWindow(state->flag_checks[index], FALSE);
    make_dialog_control(state->window, "STATIC",
        "'Affects entities' enables lighting\r\n'Use bounding box' lightens only entities inside of it.\r\n"
        "'Shadow volume' turns light into darkness; it does not cast shadows.\r\n"
        "Greyed options do not affect static entity lighting. Their values are preserved only.",
        SS_LEFT, 0, 14, 342, 728, 70);

    make_dialog_control(state->window, "BUTTON", "Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 526, 430, 104, 30);
    make_dialog_control(state->window, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 638, 430, 104, 30);

    const Asura_Light& light = state->value;
    const float position[] = {light.Position.x, light.Position.y, light.Position.z};
    const float colour[] = {light.R, light.G, light.B};
    const float bounds[] = {light.m_xBoundingBox.MinX, light.m_xBoundingBox.MaxX,
                            light.m_xBoundingBox.MinY, light.m_xBoundingBox.MaxY,
                            light.m_xBoundingBox.MinZ, light.m_xBoundingBox.MaxZ};
    for (int i = 0; i < 3; ++i) {
        set_float(state->position[i], position[i]);
        set_float(state->colour[i], colour[i]);
    }
    for (int i = 0; i < 6; ++i)
        set_float(state->bounds[i], bounds[i]);
    set_float(state->brightness, light.Brightness);
    set_float(state->range, light.Range);
    set_float(state->inner_range, light.m_fInnerRange);
    set_float(state->shadow_strength, light.ShadowStrength);
    set_u32_hex(state->flags, light.m_uFlags);
    for (int i = 0; i < static_cast<int>(_countof(kLightFlagControls)); ++i)
        SendMessageA(state->flag_checks[i], BM_SETCHECK,
                     light.m_uFlags & kLightFlagControls[i].mask ? BST_CHECKED : BST_UNCHECKED, 0);
    update_light_flag_dependent_controls(state, light.m_uFlags);
}

void apply_light_properties(LightPropertiesState* state) {
    Asura_Light& light = state->value;
    light.Position = {get_float(state->position[0], light.Position.x),
                      get_float(state->position[1], light.Position.y),
                      get_float(state->position[2], light.Position.z)};
    light.R = get_float(state->colour[0], light.R);
    light.G = get_float(state->colour[1], light.G);
    light.B = get_float(state->colour[2], light.B);
    light.Brightness = get_float(state->brightness, light.Brightness);
    light.Range = get_float(state->range, light.Range);
    light.ShadowStrength = get_float(state->shadow_strength, light.ShadowStrength);
    float* bounds[] = {&light.m_xBoundingBox.MinX, &light.m_xBoundingBox.MaxX,
                       &light.m_xBoundingBox.MinY, &light.m_xBoundingBox.MaxY,
                       &light.m_xBoundingBox.MinZ, &light.m_xBoundingBox.MaxZ};
    for (int i = 0; i < 6; ++i)
        *bounds[i] = get_float(state->bounds[i], *bounds[i]);
    light.m_uFlags = get_u32(state->flags, light.m_uFlags);
    for (int i = 0; i < static_cast<int>(_countof(kLightFlagControls)); ++i) {
        if (SendMessageA(state->flag_checks[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
            light.m_uFlags |= kLightFlagControls[i].mask;
        else
            light.m_uFlags &= ~kLightFlagControls[i].mask;
    }
}

LRESULT CALLBACK light_properties_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTA* create = reinterpret_cast<const CREATESTRUCTA*>(lparam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    LightPropertiesState* state =
        reinterpret_cast<LightPropertiesState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE:
        state->window = hwnd;
        create_light_properties_controls(state);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_LIGHT_FLAGS && HIWORD(wparam) == EN_CHANGE) {
            const uint32_t flags = get_u32(state->flags, state->value.m_uFlags);
            for (int i = 0; i < static_cast<int>(_countof(kLightFlagControls)); ++i)
                SendMessageA(state->flag_checks[i], BM_SETCHECK,
                             flags & kLightFlagControls[i].mask ? BST_CHECKED : BST_UNCHECKED, 0);
            update_light_flag_dependent_controls(state, flags);
        } else if (LOWORD(wparam) >= ID_LIGHT_FLAG_FIRST && LOWORD(wparam) <= ID_LIGHT_FLAG_LAST &&
                   HIWORD(wparam) == BN_CLICKED) {
            uint32_t flags = get_u32(state->flags, state->value.m_uFlags);
            const int index = LOWORD(wparam) - ID_LIGHT_FLAG_FIRST;
            if (SendMessageA(state->flag_checks[index], BM_GETCHECK, 0, 0) == BST_CHECKED)
                flags |= kLightFlagControls[index].mask;
            else
                flags &= ~kLightFlagControls[index].mask;
            set_u32_hex(state->flags, flags);
            update_light_flag_dependent_controls(state, flags);
        } else if (LOWORD(wparam) == IDOK) {
            apply_light_properties(state);
            const auto& light = state->value;
            const auto& bounds = light.m_xBoundingBox;
            if (((light.m_uFlags & ASURA_LIGHT_FLAG_AFFECTS_ENTITIES) && light.Range <= 0) ||
                ((light.m_uFlags & ASURA_LIGHT_FLAG_USE_BOUNDING_BOX) &&
                 (bounds.MinX >= bounds.MaxX || bounds.MinY >= bounds.MaxY || bounds.MinZ >= bounds.MaxZ)) ||
                ((light.m_uFlags & ASURA_LIGHT_FLAG_IS_SHADOW_VOLUME) &&
                 (light.ShadowStrength < 0 || light.ShadowStrength > 1))) {
                MessageBoxA(hwnd, "Use a positive range, ordered box bounds, and shadow strength from 0 to 1.",
                            "Light properties", MB_OK | MB_ICONWARNING);
                return 0;
            }
            state->accepted = true;
            DestroyWindow(hwnd);
        } else if (LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

void refresh_inspector();

void command_light_properties() {
    if (g.selected < 0 || g.selected >= static_cast<int>(g.document.entities.size()) ||
        g.document.entities[g.selected].kind != EntityKind::Light)
        return;
    Entity& entity = g.document.entities[g.selected];
    LightPropertiesState state{};
    state.value = entity.light;
    state.value.Position = entity.position;

    if (!run_centered_modal("Asura2005LightProperties", "Light properties", 780, 550, &state))
        return;
    if (!state.accepted)
        return;

    if (!g.history.begin(g.document, g.selected))
        return;
    for (int index : g.selected_entities) {
        if (!valid_entity_index(index) || g.document.entities[index].kind != EntityKind::Light)
            continue;
        Entity& target = g.document.entities[index];
        target.light = state.value;
        target.position = state.value.Position;
        target.value_a = state.value.Brightness;
        target.value_b = state.value.Range;
    }
    commit_history_transaction();
    refresh_inspector();
    request_redraw();
}

const PickupTemplate* find_pickup_template(const Document& document, uint32_t item_id, bool require_item) {
    const PickupTemplate* fallback = nullptr;
    for (const PickupTemplate& candidate : document.pickup_templates) {
        if (!fallback)
            fallback = &candidate;
        if (candidate.item_id == item_id)
            return &candidate;
    }
    return require_item ? nullptr : fallback;
}

void adopt_pickup_template(Entity* entity, const PickupTemplate& source) {
    entity->value_a = source.health;
    entity->value_u32_a = source.item_id;
    entity->value_u32_b = source.file_id;
    entity->pickup_skin_id = source.skin_id;
    entity->pickup_anim_id = source.anim_id;
    entity->pickup_anim_file_id = source.anim_file_id;
    entity->pickup_body = source.body;
    entity->pickup_has_template = true;
}

bool choose_paths(HWND owner, const char* title, const char* filter, const char* extension,
                  const std::vector<std::string>& current, std::vector<std::string>* paths) {
    std::vector<char> buffer(64 * 1024, 0);
    std::string initial_directory;
    if (!current.empty()) {
        const size_t slash = current.front().find_last_of("\\/");
        if (slash != std::string::npos)
            initial_directory = current.front().substr(0, slash);
    }
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrDefExt = extension;
    ofn.lpstrInitialDir = initial_directory.empty() ? nullptr : initial_directory.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameA(&ofn))
        return false;
    std::vector<std::string> selected;
    const std::string first = buffer.data();
    const char* next = buffer.data() + first.size() + 1;
    if (!*next) {
        selected.push_back(first);
    } else {
        while (*next) {
            std::string path = first;
            if (!path.empty() && path.back() != '\\' && path.back() != '/')
                path.push_back('\\');
            path += next;
            selected.push_back(std::move(path));
            next += strlen(next) + 1;
        }
    }
    *paths = std::move(selected);
    return true;
}

const StaticObjectTemplate* find_static_object_template(const Document& document, uint32_t file_id,
                                                        bool require_file) {
    const StaticObjectTemplate* fallback = nullptr;
    for (const StaticObjectTemplate& candidate : document.static_object_templates) {
        if (!fallback)
            fallback = &candidate;
        if (candidate.file_id == file_id)
            return &candidate;
    }
    return require_file ? nullptr : fallback;
}

void adopt_static_object_template(Entity* entity, const StaticObjectTemplate& source) {
    Snipe_ServerEntity_StaticObject_ChunkDataV0 body{};
    memcpy(&body, source.body.data(), sizeof(body));
    entity->value_a = body.m_xPhysicalObject.m_fHealth;
    entity->value_u32_b = source.file_id;
    entity->pickup_skin_id = body.m_xPhysicalObject.m_uSkinID;
    entity->pickup_anim_id = body.m_xPhysicalObject.m_uAnimID;
    entity->pickup_anim_file_id = body.m_xPhysicalObject.m_uAnimFileID;
    entity->entity_padding = source.entity_padding;
    entity->static_object_body = source.body;
    entity->static_object_has_template = true;
}

const char* entity_type_label(EntityKind kind) {
    switch (kind) {
    case EntityKind::SpawnPoint: return "Spawn";
    case EntityKind::Light: return "Light";
    case EntityKind::Sound: return "Sound";
    case EntityKind::Pickup: return "Pickup";
    case EntityKind::AssassinationTarget: return "Target";
    case EntityKind::PositionMarker: return "Marker";
    case EntityKind::StaticObject: return "Object";
    case EntityKind::BuildingVolume: return "Building volume";
    }
    return "Entity";
}

int entity_list_row(int document_index) {
    const int count = static_cast<int>(SendMessageA(g.list, LB_GETCOUNT, 0, 0));
    for (int row = 0; row < count; ++row)
        if (static_cast<int>(SendMessageA(g.list, LB_GETITEMDATA, row, 0)) == document_index)
            return row;
    return LB_ERR;
}

void sync_entity_list_selection() {
    if (!g.list)
        return;
    const int count = static_cast<int>(SendMessageA(g.list, LB_GETCOUNT, 0, 0));
    for (int row = 0; row < count; ++row) {
        const int index = static_cast<int>(SendMessageA(g.list, LB_GETITEMDATA, row, 0));
        SendMessageA(g.list, LB_SETSEL, entity_is_selected(index), row);
    }
    if (g.selected >= 0) {
        const int row = entity_list_row(g.selected);
        if (row != LB_ERR)
            SendMessageA(g.list, LB_SETCARETINDEX, row, FALSE);
    }
}

void select_entities_from_list() {
    const int old_primary = g.selected;
    const std::vector<int> old_selection = g.selected_entities;
    std::vector<int> selection;
    const int count = static_cast<int>(SendMessageA(g.list, LB_GETCOUNT, 0, 0));
    for (int row = 0; row < count; ++row) {
        if (SendMessageA(g.list, LB_GETSEL, row, 0) <= 0)
            continue;
        const int index = static_cast<int>(SendMessageA(g.list, LB_GETITEMDATA, row, 0));
        if (valid_entity_index(index))
            selection.push_back(index);
    }

    const LRESULT caret_row = SendMessageA(g.list, LB_GETCARETINDEX, 0, 0);
    const int caret_index = caret_row == LB_ERR
                                ? -1
                                : static_cast<int>(SendMessageA(g.list, LB_GETITEMDATA,
                                                                 static_cast<WPARAM>(caret_row), 0));
    bool restricted = false;
    if (selection.size() > 1) {
        EntityKind allowed_kind = g.document.entities[selection.front()].kind;
        bool mixed = false;
        for (int index : selection)
            mixed |= g.document.entities[index].kind != allowed_kind;
        if (mixed) {
            // When Ctrl/Shift adds another type, preserve the type that was
            // already selected. A plain click has only one item and switches
            // types normally.
            if (!old_selection.empty() && valid_entity_index(old_selection.front()))
                allowed_kind = g.document.entities[old_selection.front()].kind;
            else if (valid_entity_index(caret_index))
                allowed_kind = g.document.entities[caret_index].kind;
            size_t write = 0;
            for (int index : selection)
                if (g.document.entities[index].kind == allowed_kind)
                    selection[write++] = index;
            selection.resize(write);
            restricted = true;
        }
    }

    g.selected_entities = std::move(selection);
    if (entity_is_selected(caret_index))
        g.selected = caret_index;
    else if (entity_is_selected(old_primary))
        g.selected = old_primary;
    else
        g.selected = g.selected_entities.empty() ? -1 : g.selected_entities.back();
    if (restricted)
        sync_entity_list_selection();
    if (g.selected != old_primary || g.selected_entities != old_selection)
        stop_sound_preview();
    refresh_inspector();
    request_redraw();
    if (restricted)
        set_status("Multiple selection is limited to one entity type.");
}

void refresh_list() {
    SendMessageA(g.list, LB_RESETCONTENT, 0, 0);
    std::vector<uint32_t> order(g.document.entities.size());
    for (uint32_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) {
        const Entity& first = g.document.entities[a];
        const Entity& second = g.document.entities[b];
        const int names = _stricmp(first.name.c_str(), second.name.c_str());
        if (names != 0)
            return names < 0;
        const int types = _stricmp(entity_type_label(first.kind), entity_type_label(second.kind));
        if (types != 0)
            return types < 0;
        return first.guid != second.guid ? first.guid < second.guid : a < b;
    });
    for (uint32_t index : order) {
        const Entity& entity = g.document.entities[index];
        const std::string line = std::string(entity_type_label(entity.kind)) + "  " + entity.name;
        const LRESULT row = SendMessageA(g.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        if (row != LB_ERR && row != LB_ERRSPACE)
            SendMessageA(g.list, LB_SETITEMDATA, static_cast<WPARAM>(row), index);
    }
    normalize_selection_state();
    sync_entity_list_selection();
}

void refresh_pickup_choices(uint32_t selected_item) {
    SendMessageA(g.pickup_item, CB_RESETCONTENT, 0, 0);
    std::vector<const PickupTemplate*> choices;
    choices.reserve(g.document.pickup_templates.size());
    for (const PickupTemplate& item : g.document.pickup_templates)
        choices.push_back(&item);
    std::sort(choices.begin(), choices.end(), [](const PickupTemplate* a, const PickupTemplate* b) {
        const std::string first = snipe_item_label(a->item_id);
        const std::string second = snipe_item_label(b->item_id);
        const int names = _stricmp(first.c_str(), second.c_str());
        if (names != 0)
            return names < 0;
        return a->item_id != b->item_id ? a->item_id < b->item_id : a < b;
    });
    for (const PickupTemplate* item : choices) {
        const std::string label = snipe_item_label(item->item_id);
        const LRESULT row = SendMessageA(g.pickup_item, CB_ADDSTRING, 0,
                                         reinterpret_cast<LPARAM>(label.c_str()));
        if (row == CB_ERR || row == CB_ERRSPACE)
            continue;
        SendMessageA(g.pickup_item, CB_SETITEMDATA, static_cast<WPARAM>(row), item->item_id);
        if (item->item_id == selected_item)
            SendMessageA(g.pickup_item, CB_SETCURSEL, static_cast<WPARAM>(row), 0);
    }
}

void refresh_static_object_choices(uint32_t selected_file) {
    SendMessageA(g.pickup_item, CB_RESETCONTENT, 0, 0);
    std::vector<const StaticObjectTemplate*> choices;
    choices.reserve(g.document.static_object_templates.size());
    for (const StaticObjectTemplate& object : g.document.static_object_templates)
        choices.push_back(&object);
    std::sort(choices.begin(), choices.end(), [](const StaticObjectTemplate* a,
                                                 const StaticObjectTemplate* b) {
        const int names = _stricmp(a->resource_name.c_str(), b->resource_name.c_str());
        if (names != 0)
            return names < 0;
        return a->file_id != b->file_id ? a->file_id < b->file_id : a < b;
    });
    for (const StaticObjectTemplate* object : choices) {
        char label[640]{};
        snprintf(label, sizeof(label), "%s (%08X)",
                 object->resource_name.empty() ? "Unnamed Object" : object->resource_name.c_str(),
                 object->file_id);
        const LRESULT row = SendMessageA(g.pickup_item, CB_ADDSTRING, 0,
                                         reinterpret_cast<LPARAM>(label));
        if (row == CB_ERR || row == CB_ERRSPACE)
            continue;
        SendMessageA(g.pickup_item, CB_SETITEMDATA, static_cast<WPARAM>(row), object->file_id);
        if (object->file_id == selected_file)
            SendMessageA(g.pickup_item, CB_SETCURSEL, static_cast<WPARAM>(row), 0);
    }
}

void refresh_inspector() {
    g.refreshing_inspector = true;
    g.inspector_dirty = 0;
    const bool enabled = g.selected >= 0 && g.selected < static_cast<int>(g.document.entities.size());
    const bool source_entity = enabled && g.document.entities[g.selected].source_entity_record;
    const bool pickup = enabled && g.document.entities[g.selected].kind == EntityKind::Pickup;
    const bool static_object = enabled && g.document.entities[g.selected].kind == EntityKind::StaticObject;
    const bool oriented_bounds = enabled &&
                                 g.document.entities[g.selected].kind == EntityKind::BuildingVolume;
    bool selection_deletable = enabled;
    for (int index : g.selected_entities) {
        const Entity& entity = g.document.entities[index];
        selection_deletable &= !entity.source_entity_record || entity.kind == EntityKind::Pickup ||
                               entity.kind == EntityKind::StaticObject;
    }
    if (g.rain_toggle) {
        SendMessageA(g.rain_toggle, BM_SETCHECK,
                     g.document.rain_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(g.rain_toggle,
                     !g.document.source_pc_path.empty() || !g.document.obj_path.empty());
    }
    if (g.backface_cull_toggle)
        SendMessageA(g.backface_cull_toggle, BM_SETCHECK,
                     g.backface_culling ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g.ambience_properties)
        EnableWindow(g.ambience_properties,
                     !g.document.source_pc_path.empty() || !g.document.obj_path.empty());
    HWND fields[] = {g.name, g.pos[0], g.pos[1], g.pos[2], g.rot[0], g.rot[1], g.rot[2],
                     g.value[0], g.value[1], g.value[2]};
    for (HWND h : fields)
        EnableWindow(h, enabled);
    EnableWindow(g.value[0], enabled && (!source_entity || pickup || static_object || oriented_bounds));
    EnableWindow(g.value[1], enabled && ((!source_entity && !pickup && !static_object) || oriented_bounds));
    EnableWindow(g.value[2], oriented_bounds);
    EnableWindow(GetDlgItem(g.window, ID_APPLY_INSPECTOR), enabled);
    EnableWindow(GetDlgItem(g.window, ID_DELETE_ENTITY), selection_deletable);
    if (g.selected_entities.size() > 1) {
        char label[96]{};
        snprintf(label, sizeof(label), "Apply properties to %zu selected", g.selected_entities.size());
        SetWindowTextA(GetDlgItem(g.window, ID_APPLY_INSPECTOR), label);
    } else {
        SetWindowTextA(GetDlgItem(g.window, ID_APPLY_INSPECTOR), "Apply properties");
    }
    ShowWindow(g.sound_browse, SW_HIDE);
    ShowWindow(g.sound_loop, SW_HIDE);
    ShowWindow(g.sound_preview, SW_HIDE);
    ShowWindow(g.sound_properties, SW_HIDE);
    ShowWindow(g.light_properties, SW_HIDE);
    ShowWindow(g.pickup_item, SW_HIDE);
    ShowWindow(g.spawn_team_label, SW_HIDE);
    ShowWindow(g.spawn_game_mode_label, SW_HIDE);
    for (HWND check : g.spawn_team_checks)
        ShowWindow(check, SW_HIDE);
    for (HWND check : g.spawn_game_mode_checks)
        ShowWindow(check, SW_HIDE);
    for (int i = 0; i < 3; ++i) {
        ShowWindow(GetDlgItem(g.window, 914 + i), SW_SHOW);
        ShowWindow(g.rot[i], SW_SHOW);
    }
    for (int i = 0; i < 3; ++i) {
        ShowWindow(g.value_label[i], SW_HIDE);
        ShowWindow(g.value[i], SW_HIDE);
    }
    for (int i = 0; i < 2; ++i) {
        ShowWindow(g.value_label[i], SW_SHOW);
        ShowWindow(g.value[i], SW_SHOW);
    }
    if (!enabled) {
        for (HWND h : fields)
            SetWindowTextA(h, "");
        set_control_text(g.value_label[0], "Property A");
        set_control_text(g.value_label[1], "Property B");
        set_control_text(g.value_label[2], "Property C");
        set_control_text(GetDlgItem(g.window, 914), "Pitch");
        set_control_text(GetDlgItem(g.window, 915), "Yaw");
        set_control_text(GetDlgItem(g.window, 916), "Roll");
        g.refreshing_inspector = false;
        return;
    }
    const Entity& e = g.document.entities[g.selected];
    SetWindowTextA(g.name, e.name.c_str());
    set_float(g.pos[0], e.position.x);
    set_float(g.pos[1], e.position.y);
    set_float(g.pos[2], e.position.z);
    if (e.kind == EntityKind::Light) {
        for (int i = 0; i < 3; ++i) {
            ShowWindow(GetDlgItem(g.window, 914 + i), SW_HIDE);
            ShowWindow(g.rot[i], SW_HIDE);
        }
    } else {
        set_control_text(GetDlgItem(g.window, 914), "Pitch");
        set_control_text(GetDlgItem(g.window, 915), "Yaw");
        set_control_text(GetDlgItem(g.window, 916), "Roll");
        set_float(g.rot[0], e.rotation.x);
        set_float(g.rot[1], e.rotation.y);
        set_float(g.rot[2], e.rotation.z);
        if (e.kind == EntityKind::SpawnPoint) {
            set_control_text(GetDlgItem(g.window, 914), "Camera pitch");
            set_control_text(GetDlgItem(g.window, 915), "Camera yaw");
            ShowWindow(GetDlgItem(g.window, 916), SW_HIDE);
            ShowWindow(g.rot[2], SW_HIDE);
        }
    }
    if (e.kind == EntityKind::SpawnPoint) {
        for (int i = 0; i < 2; ++i) {
            ShowWindow(g.value_label[i], SW_HIDE);
            ShowWindow(g.value[i], SW_HIDE);
        }
        ShowWindow(g.spawn_team_label, SW_SHOW);
        ShowWindow(g.spawn_game_mode_label, SW_SHOW);
        for (int i = 0; i < static_cast<int>(_countof(kSpawnTeamControls)); ++i) {
            ShowWindow(g.spawn_team_checks[i], SW_SHOW);
            SendMessageA(g.spawn_team_checks[i], BM_SETCHECK,
                         e.value_u32_a & kSpawnTeamControls[i].mask ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        for (int i = 0; i < static_cast<int>(_countof(kSpawnGameModeControls)); ++i) {
            ShowWindow(g.spawn_game_mode_checks[i], SW_SHOW);
            SendMessageA(g.spawn_game_mode_checks[i], BM_SETCHECK,
                         e.value_u32_b & kSpawnGameModeControls[i].mask ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    } else if (e.kind == EntityKind::Light) {
        const bool shadow_volume = (e.light.m_uFlags & ASURA_LIGHT_FLAG_IS_SHADOW_VOLUME) != 0;
        set_control_text(g.value_label[0], shadow_volume ? "Shadow strength" : "Brightness");
        set_control_text(g.value_label[1], "Range");
        set_float(g.value[0], shadow_volume ? e.light.ShadowStrength : e.light.Brightness);
        set_float(g.value[1], e.light.Range);
        ShowWindow(g.light_properties, SW_SHOW);
    } else if (e.kind == EntityKind::Sound) {
        set_control_text(g.value_label[0], "Min radius");
        set_control_text(g.value_label[1], "Max radius");
        set_float(g.value[0], e.value_a);
        set_float(g.value[1], e.value_b);
        SendMessageA(g.sound_loop, BM_SETCHECK, e.sound_loop ? BST_CHECKED : BST_UNCHECKED, 0);
        ShowWindow(g.sound_loop, SW_SHOW);
        ShowWindow(g.sound_browse, SW_SHOW);
        ShowWindow(g.sound_preview, SW_SHOW);
        ShowWindow(g.sound_properties, SW_SHOW);
    } else if (e.kind == EntityKind::Pickup) {
        set_control_text(g.value_label[0], "Item type");
        set_control_text(g.value_label[1], "Object file ID");
        set_u32_hex(g.value[0], e.value_u32_a);
        set_u32_hex(g.value[1], e.value_u32_b);
        ShowWindow(g.value[0], SW_HIDE);
        refresh_pickup_choices(e.value_u32_a);
        EnableWindow(g.pickup_item,
                     SendMessageA(g.pickup_item, CB_GETCOUNT, 0, 0) > 0);
        ShowWindow(g.pickup_item, SW_SHOW);
    } else if (e.kind == EntityKind::StaticObject) {
        set_control_text(g.value_label[0], "Object type");
        set_control_text(g.value_label[1], "Object file ID");
        set_float(g.value[0], e.value_a);
        set_u32_hex(g.value[1], e.value_u32_b);
        ShowWindow(g.value[0], SW_HIDE);
        refresh_static_object_choices(e.value_u32_b);
        EnableWindow(g.pickup_item, SendMessageA(g.pickup_item, CB_GETCOUNT, 0, 0) > 0);
        ShowWindow(g.pickup_item, SW_SHOW);
    } else if (e.kind == EntityKind::AssassinationTarget) {
        set_control_text(g.value_label[0], "Health");
        set_control_text(g.value_label[1], "Class ID");
        set_float(g.value[0], e.value_a);
        set_u32_hex(g.value[1], e.source_entity_classification);
    } else if (e.kind == EntityKind::PositionMarker) {
        set_control_text(g.value_label[0], "Bounds width");
        set_control_text(g.value_label[1], "Bounds depth");
        set_float(g.value[0], e.value_a);
        set_float(g.value[1], e.value_b);
    } else if (e.kind == EntityKind::BuildingVolume) {
        const Asura_Vector_3 size = oriented_box_dimensions(e);
        set_control_text(g.value_label[0], "Bounds width");
        set_control_text(g.value_label[1], "Bounds height");
        set_control_text(g.value_label[2], "Bounds depth");
        set_float(g.value[0], size.x);
        set_float(g.value[1], size.y);
        set_float(g.value[2], size.z);
        ShowWindow(g.value_label[2], SW_SHOW);
        ShowWindow(g.value[2], SW_SHOW);
    }
    g.refreshing_inspector = false;
}

void select_entity(int index) {
    if (index != g.selected)
        stop_sound_preview();
    set_single_selection_state(index);
    sync_entity_list_selection();
    refresh_inspector();
    request_redraw();
}

void toggle_entity_selection(int index) {
    if (!valid_entity_index(index))
        return;
    if (!g.selected_entities.empty() && valid_entity_index(g.selected_entities.front()) &&
        g.document.entities[g.selected_entities.front()].kind != g.document.entities[index].kind) {
        set_status("Multiple selection is limited to one entity type.");
        return;
    }
    stop_sound_preview();
    const size_t position = selection_index_of(g.selected_entities, index);
    if (position == g.selected_entities.size()) {
        g.selected_entities.push_back(index);
        g.selected = index;
    } else {
        g.selected_entities.erase(g.selected_entities.begin() + static_cast<ptrdiff_t>(position));
        g.selected = g.selected_entities.empty() ? -1 : g.selected_entities.back();
    }
    sync_entity_list_selection();
    refresh_inspector();
    request_redraw();
}

void focus_camera_on_entity(int index) {
    if (index < 0 || index >= static_cast<int>(g.document.entities.size()))
        return;
    const Entity& entity = g.document.entities[index];
    g.camera.target = entity_view_position(entity.position);

    // Fit an actual spawn/pickup mesh when one is available. Rotation and the
    // target-to-editor Y conversion preserve the local bounding-sphere radius.
    float radius = fmaxf(.35f, g.mesh.radius * .008f) * 1.6f;
    if (const EntityModel* model = entity_render_model(entity)) {
        const Asura_Vector_3 local_center{(model->min.x + model->max.x) * .5f,
                                          (model->min.y + model->max.y) * .5f,
                                          (model->min.z + model->max.z) * .5f};
        g.camera.target = entity_model_view_position(local_center, entity);
        const float width = model->max.x - model->min.x;
        const float height = model->max.y - model->min.y;
        const float depth = model->max.z - model->min.z;
        const float model_radius = .5f * sqrtf(width * width + height * height + depth * depth);
        if (isfinite(model_radius) && model_radius > .01f)
            radius = model_radius;
    } else if (entity.kind == EntityKind::PositionMarker ||
               entity.kind == EntityKind::BuildingVolume) {
        const float width = entity.source_bounds.MaxX - entity.source_bounds.MinX;
        const float height = entity.source_bounds.MaxY - entity.source_bounds.MinY;
        const float depth = entity.source_bounds.MaxZ - entity.source_bounds.MinZ;
        const float marker_radius = .5f * sqrtf(width * width + height * height + depth * depth);
        if (isfinite(marker_radius) && marker_radius > .01f)
            radius = marker_radius;
    }

    g.camera.distance = std::clamp(radius * 2.8f, .5f, 1000000.0f);
    invalidate_environment_cache();
    request_redraw();
    set_status("Camera focused on the selected entity.");
}

void apply_inspector() {
    normalize_selection_state();
    if (!valid_entity_index(g.selected))
        return;
    if (!g.history.begin(g.document, g.selected))
        return;
    char name[512]{};
    GetWindowTextA(g.name, name, sizeof(name));
    const std::vector<int> targets = g.selected_entities;
    const bool apply_all = targets.size() == 1;
    const auto changed = [&](uint32_t flag) { return apply_all || (g.inspector_dirty & flag) != 0; };
    const uint32_t position_dirty = kInspectorDirtyPosX | kInspectorDirtyPosY | kInspectorDirtyPosZ;
    const uint32_t rotation_dirty = kInspectorDirtyRotX | kInspectorDirtyRotY | kInspectorDirtyRotZ;
    for (int index : targets) {
        Entity& e = g.document.entities[index];
        if (changed(kInspectorDirtyName))
            e.name = name;
        if (changed(kInspectorDirtyPosX))
            e.position.x = get_float(g.pos[0], e.position.x);
        if (changed(kInspectorDirtyPosY))
            e.position.y = get_float(g.pos[1], e.position.y);
        if (changed(kInspectorDirtyPosZ))
            e.position.z = get_float(g.pos[2], e.position.z);
        if (e.kind == EntityKind::Light) {
            if (apply_all || (g.inspector_dirty & position_dirty))
                e.light.Position = e.position;
        } else if (e.kind == EntityKind::SpawnPoint) {
            if (changed(kInspectorDirtyRotX))
                e.rotation.x = get_float(g.rot[0], e.rotation.x);
            if (changed(kInspectorDirtyRotY))
                e.rotation.y = get_float(g.rot[1], e.rotation.y);
            if (apply_all)
                e.rotation.z = 0.0f;
        } else {
            if (changed(kInspectorDirtyRotX))
                e.rotation.x = get_float(g.rot[0], e.rotation.x);
            if (changed(kInspectorDirtyRotY))
                e.rotation.y = get_float(g.rot[1], e.rotation.y);
            if (changed(kInspectorDirtyRotZ))
                e.rotation.z = get_float(g.rot[2], e.rotation.z);
        }
        if (e.kind == EntityKind::SpawnPoint) {
            for (int i = 0; i < static_cast<int>(_countof(kSpawnTeamControls)); ++i) {
                if (!changed(1u << (kInspectorDirtySpawnTeamShift + i)))
                    continue;
                if (SendMessageA(g.spawn_team_checks[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
                    e.value_u32_a |= kSpawnTeamControls[i].mask;
                else
                    e.value_u32_a &= ~kSpawnTeamControls[i].mask;
            }
            for (int i = 0; i < static_cast<int>(_countof(kSpawnGameModeControls)); ++i) {
                if (!changed(1u << (kInspectorDirtySpawnGameModeShift + i)))
                    continue;
                if (SendMessageA(g.spawn_game_mode_checks[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
                    e.value_u32_b |= kSpawnGameModeControls[i].mask;
                else
                    e.value_u32_b &= ~kSpawnGameModeControls[i].mask;
            }
            if (apply_all || (g.inspector_dirty & (kInspectorDirtyRotX | kInspectorDirtyRotY))) {
                const float yaw = e.rotation.y * 3.14159265358979323846f / 180.0f;
                const float pitch = e.rotation.x * 3.14159265358979323846f / 180.0f;
                e.spawn_direction = {cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw)};
            }
        } else if (e.kind == EntityKind::Light) {
            if (changed(kInspectorDirtyValueA)) {
                if (e.light.m_uFlags & ASURA_LIGHT_FLAG_IS_SHADOW_VOLUME)
                    e.light.ShadowStrength = get_float(g.value[0], e.light.ShadowStrength);
                else
                    e.light.Brightness = get_float(g.value[0], e.light.Brightness);
            }
            if (changed(kInspectorDirtyValueB))
                e.value_b = get_float(g.value[1], e.light.Range);
            e.value_a = e.light.Brightness;
            e.light.Range = e.value_b;
        } else if (e.kind == EntityKind::Sound) {
            if (changed(kInspectorDirtyValueA))
                e.value_a = get_float(g.value[0], e.value_a);
            if (changed(kInspectorDirtyValueB))
                e.value_b = get_float(g.value[1], e.value_b);
            if (changed(kInspectorDirtySoundLoop))
                e.sound_loop = SendMessageA(g.sound_loop, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (e.sound_source_record &&
                (apply_all || (g.inspector_dirty & (position_dirty | rotation_dirty |
                                                    kInspectorDirtyValueA | kInspectorDirtyValueB |
                                                    kInspectorDirtySoundLoop)))) {
                e.sound_phonon.m_xPosition = e.position;
                e.sound_phonon.m_fInnerRadius = e.value_a;
                e.sound_phonon.m_fOuterRadius = e.value_b;
                e.sound_phonon.m_xOrient = euler_quaternion(e.rotation);
                e.sound_phonon.m_uFlags = e.sound_loop ? (e.sound_phonon.m_uFlags | 1u)
                                                       : (e.sound_phonon.m_uFlags & ~1u);
            }
        } else if (e.kind == EntityKind::Pickup) {
            const LRESULT selected_item = SendMessageA(g.pickup_item, CB_GETCURSEL, 0, 0);
            const uint32_t requested_item = !changed(kInspectorDirtyPickup) || selected_item == CB_ERR
                                                ? e.value_u32_a
                                                : static_cast<uint32_t>(SendMessageA(
                                                      g.pickup_item, CB_GETITEMDATA,
                                                      static_cast<WPARAM>(selected_item), 0));
            if (requested_item != e.value_u32_a) {
                const PickupTemplate* item_template = find_pickup_template(g.document, requested_item, true);
                if (item_template) {
                    const char* old_item_name = snipe_item_name(e.value_u32_a);
                    const bool automatic_name = old_item_name && e.name.rfind(old_item_name, 0) == 0;
                    const std::string name_suffix = automatic_name ? e.name.substr(strlen(old_item_name)) : std::string{};
                    adopt_pickup_template(&e, *item_template);
                    if (automatic_name) {
                        const char* new_item_name = snipe_item_name(requested_item);
                        e.name = std::string(new_item_name ? new_item_name : "Pickup") + name_suffix;
                    }
                } else {
                    char message[192]{};
                    snprintf(message, sizeof(message),
                             "Item 0x%02X is not present in the loaded 0x0008 pickup catalog.",
                             requested_item);
                    set_status(message);
                }
            }
        } else if (e.kind == EntityKind::StaticObject) {
            const LRESULT selected_object = SendMessageA(g.pickup_item, CB_GETCURSEL, 0, 0);
            const uint32_t requested_file = !changed(kInspectorDirtyPickup) || selected_object == CB_ERR
                                                ? e.value_u32_b
                                                : static_cast<uint32_t>(SendMessageA(
                                                      g.pickup_item, CB_GETITEMDATA,
                                                      static_cast<WPARAM>(selected_object), 0));
            if (requested_file != e.value_u32_b) {
                const StaticObjectTemplate* object =
                    find_static_object_template(g.document, requested_file, true);
                if (object) {
                    const StaticObjectTemplate* old =
                        find_static_object_template(g.document, e.value_u32_b, true);
                    const bool automatic_name = old && e.name.rfind(old->resource_name, 0) == 0;
                    const std::string suffix = automatic_name ? e.name.substr(old->resource_name.size())
                                                              : std::string{};
                    adopt_static_object_template(&e, *object);
                    if (automatic_name)
                        e.name = object->resource_name + suffix;
                }
            }
        } else if (e.kind == EntityKind::BuildingVolume) {
            Asura_Vector_3 size = oriented_box_dimensions(e);
            if (changed(kInspectorDirtyValueA))
                size.x = get_float(g.value[0], size.x);
            if (changed(kInspectorDirtyValueB))
                size.y = get_float(g.value[1], size.y);
            if (changed(kInspectorDirtyValueC))
                size.z = get_float(g.value[2], size.z);
            if (isfinite(size.x) && isfinite(size.y) && isfinite(size.z) &&
                size.x > 0.0f && size.y > 0.0f && size.z > 0.0f) {
                e.source_bounds = {e.position.x - size.x * .5f, e.position.x + size.x * .5f,
                                   e.position.y - size.y * .5f, e.position.y + size.y * .5f,
                                   e.position.z - size.z * .5f, e.position.z + size.z * .5f};
                e.value_a = size.x;
                e.value_b = size.z;
            }
        }
    }
    commit_history_transaction();
    refresh_list();
    refresh_inspector();
    request_redraw();
    if (targets.size() > 1) {
        char status[128]{};
        snprintf(status, sizeof(status), "Properties applied to %zu selected %s entities.",
                 targets.size(), entity_type_label(g.document.entities[targets.front()].kind));
        set_status(status);
    }
}

void frame_mesh() {
    g.camera.target = g.mesh.center;
    g.camera.distance = fmaxf(10.0f, g.mesh.radius * 2.3f);
    g.environment_raycast.build(g.mesh);
    invalidate_environment_cache();
    if (gpu_ready())
        gpu_upload_mesh();
}

bool open_obj_path(const std::string& path) {
    stop_sound_preview();
    Mesh mesh;
    std::string why;
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const std::string preview_material_map = g.document.source_pc_path.empty() ? g.document.material_map : std::string{};
    const bool ok = load_preview_mesh(path, preview_material_map, &mesh, &why);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!ok) {
        MessageBoxA(g.window, why.c_str(), "Could not open OBJ", MB_ICONERROR);
        return false;
    }
    if (!g.document.source_pc_path.empty()) {
        g.document = Document{};
        g.pickup_models.clear();
        g.static_object_models.clear();
        set_single_selection_state(-1);
        g.pending_kind = -1;
        gpu_load_skybox({}, nullptr);
        refresh_list();
        refresh_inspector();
    }
    g.mesh = std::move(mesh);
    g.document.obj_path = path;
    if (g.document.output_path.empty()) {
        g.document.output_path = path;
        const size_t dot = g.document.output_path.find_last_of('.');
        if (dot != std::string::npos)
            g.document.output_path.resize(dot);
        g.document.output_path += ".PC";
    }
    frame_mesh();
    reset_history(false);
    char status[256];
    snprintf(status, sizeof(status), "%zu vertices, %zu triangles", g.mesh.positions.size(), g.mesh.faces.size());
    set_status(status);
    request_redraw();
    return true;
}

void add_entity_at(EntityKind kind, const Asura_Vector_3& p) {
    Entity e;
    if (kind == EntityKind::Pickup) {
        const PickupTemplate* pickup_template = find_pickup_template(g.document, SnipeItem_RifleAmmo, true);
        if (!pickup_template)
            pickup_template = find_pickup_template(g.document, 0, false);
        if (!pickup_template) {
            g.pending_kind = -1;
            set_status("No 0x0008 pickup catalog is loaded. Choose a Weapons donor .PC first.");
            return;
        }
        adopt_pickup_template(&e, *pickup_template);
        e.entity_padding = pickup_template->entity_padding;
        e.source_entity_record = false;
        e.source_entity_classification = SnipeEntityClass_Pickup;
    } else if (kind == EntityKind::StaticObject) {
        const StaticObjectTemplate* object = find_static_object_template(g.document, 0, false);
        if (!object) {
            g.pending_kind = -1;
            set_status("No Object catalog is loaded. Choose one or more Objects donor .PC levels first.");
            return;
        }
        adopt_static_object_template(&e, *object);
        e.source_entity_record = false;
        e.source_entity_classification = SnipeEntityClass_StaticObject;
    } else if (kind == EntityKind::BuildingVolume) {
        e.source_entity_record = false;
        e.source_entity_classification = SnipeEntityClass_BuildingVolume;
    }
    if (!g.history.begin(g.document, g.selected))
        return;
    e.kind = kind;
    e.position = p;
    e.guid = allocate_editor_guid(&g.document);
    if (!e.guid) {
        g.history.cancel_transaction();
        g.pending_kind = -1;
        set_status("No free authored entity GUIDs remain in the target's valid range.");
        return;
    }
    e.rotation = {};
    char name[160]{};
    if (kind == EntityKind::SpawnPoint) {
        snprintf(name, sizeof(name), "Spawn %zu", g.document.entities.size() + 1);
        e.value_u32_a = 5;
        e.value_u32_b = 24;
        e.spawn_timer = 5.0f;
    } else if (kind == EntityKind::Light) {
        snprintf(name, sizeof(name), "Light %zu", g.document.entities.size() + 1);
        e.value_a = 2.5f;
        e.value_b = 1500;
        e.light = legacy_editor_light(e);
    } else if (kind == EntityKind::Sound) {
        snprintf(name, sizeof(name), "Sound %zu", g.document.entities.size() + 1);
        e.value_a = 50;
        e.value_b = 250;
    } else if (kind == EntityKind::Pickup) {
        snprintf(name, sizeof(name), "%s %zu",
                 snipe_item_name(e.value_u32_a) ? snipe_item_name(e.value_u32_a) : "Pickup",
                 g.document.entities.size() + 1);
    } else if (kind == EntityKind::StaticObject) {
        const StaticObjectTemplate* object = find_static_object_template(g.document, e.value_u32_b, true);
        snprintf(name, sizeof(name), "%s %zu",
                  object && !object->resource_name.empty() ? object->resource_name.c_str() : "Object",
                  g.document.entities.size() + 1);
    } else if (kind == EntityKind::BuildingVolume) {
        snprintf(name, sizeof(name), "Building volume %zu", g.document.entities.size() + 1);
        constexpr Asura_Vector_3 size{10.0f, 5.0f, 10.0f};
        e.source_bounds = {p.x - size.x * .5f, p.x + size.x * .5f,
                           p.y - size.y * .5f, p.y + size.y * .5f,
                           p.z - size.z * .5f, p.z + size.z * .5f};
        e.value_a = size.x;
        e.value_b = size.z;
    }
    e.name = name;
    stop_sound_preview();
    g.document.entities.push_back(std::move(e));
    g.pending_kind = -1;
    set_single_selection_state(static_cast<int>(g.document.entities.size()) - 1);
    commit_history_transaction();
    refresh_list();
    refresh_inspector();
    set_status("Entity placed on the environment. Drag it across environment geometry or edit its transform.");
    request_redraw();
}

void begin_place(EntityKind kind) {
    if (g.mesh.positions.empty()) {
        MessageBoxA(g.window, "Open an environment OBJ or .PC first.", "Level Editor", MB_ICONINFORMATION);
        return;
    }
    if (kind == EntityKind::Pickup && !find_pickup_template(g.document, 0, false)) {
        MessageBoxA(g.window, "Choose a Weapons donor .PC containing 0x0008 pickup definitions first.",
                    "Cannot create pickup", MB_ICONINFORMATION);
        return;
    }
    if (kind == EntityKind::StaticObject && !find_static_object_template(g.document, 0, false)) {
        MessageBoxA(g.window, "Choose one or more Objects donor .PC levels first.",
                    "Cannot create Object", MB_ICONINFORMATION);
        return;
    }
    g.pending_kind = static_cast<int>(kind);
    set_status("Click visible environment geometry to place the entity. Right-drag orbits; wheel zooms.");
}

bool ray_hits_model_triangle(const EnvironmentRay& ray, const Asura_Vector_3& a,
                             const Asura_Vector_3& b, const Asura_Vector_3& c,
                             float maximum_distance, float* distance) {
    const Asura_Vector_3 ab = sub(b, a);
    const Asura_Vector_3 ac = sub(c, a);
    const Asura_Vector_3 p = cross(ray.direction, ac);
    const float determinant = dot(ab, p);
    if (fabsf(determinant) <= 1.0e-9f)
        return false;
    const float inverse_determinant = 1.0f / determinant;
    const Asura_Vector_3 from_a = sub(ray.origin, a);
    const float u = dot(from_a, p) * inverse_determinant;
    if (u < -1.0e-6f || u > 1.000001f)
        return false;
    const Asura_Vector_3 q = cross(from_a, ab);
    const float v = dot(ray.direction, q) * inverse_determinant;
    if (v < -1.0e-6f || u + v > 1.000001f)
        return false;
    const float hit_distance = dot(ac, q) * inverse_determinant;
    if (hit_distance <= 1.0e-5f || hit_distance >= maximum_distance)
        return false;
    *distance = hit_distance;
    return true;
}

bool entity_model_ray_distance(const Entity& entity, const EnvironmentRay& ray,
                               float* distance) {
    const EntityModel* model = entity_render_model(entity);
    if (!model || !distance)
        return false;
    float closest = FLT_MAX;
    bool found = false;
    for (const auto& face : model->faces) {
        if (face[0] >= model->vertices.size() || face[1] >= model->vertices.size() ||
            face[2] >= model->vertices.size())
            continue;
        const Asura_Vector_3 a = entity_model_view_position(model->vertices[face[0]].position, entity);
        const Asura_Vector_3 b = entity_model_view_position(model->vertices[face[1]].position, entity);
        const Asura_Vector_3 c = entity_model_view_position(model->vertices[face[2]].position, entity);
        float candidate = 0.0f;
        if (ray_hits_model_triangle(ray, a, b, c, closest, &candidate)) {
            closest = candidate;
            found = true;
        }
    }
    if (!found)
        return false;
    *distance = closest;
    return true;
}

int hit_entity(int x, int y) {
    int best = -1;
    int best_distance = 15 * 15;
    float best_depth = 1.0e30f;
    EnvironmentRay selection_ray{};
    const bool have_selection_ray = screen_ray(x, y, &selection_ray);
    EnvironmentRayHit environment_hit{};
    const bool have_environment_hit = have_selection_ray &&
                                      g.environment_raycast.intersect(g.mesh, selection_ray,
                                                                      &environment_hit);
    for (int i = 0; i < static_cast<int>(g.document.entities.size()); ++i) {
        const Entity& entity = g.document.entities[i];
        if (entity_render_model(entity)) {
            float model_distance = 0.0f;
            if (!have_selection_ray ||
                !entity_model_ray_distance(entity, selection_ray, &model_distance))
                continue;
            const float surface_epsilon = fmaxf(.01f, model_distance * 1.0e-4f);
            if (!entity_is_selected(i) && have_environment_hit &&
                environment_hit.distance + surface_epsilon < model_distance)
                continue;
            if (best_distance > 0 || model_distance < best_depth) {
                best_distance = 0;
                best_depth = model_distance;
                best = i;
            }
            continue;
        }
        if (entity.kind != EntityKind::BuildingVolume && !entity_is_selected(i) &&
            environment_occludes_view_position(entity_view_position(entity.position)))
            continue;
        if (entity.kind == EntityKind::BuildingVolume) {
            std::vector<LightGizmoLine> lines;
            lines.reserve(kLightBoundingBoxLines);
            append_oriented_bounds_gizmo(entity, &lines);
            const int edge_radius = 10;
            const int edge_distance_limit = edge_radius * edge_radius;
            for (const LightGizmoLine& line : lines) {
                POINT a{}, b{};
                float a_depth = 0, b_depth = 0;
                if (!project_point(line.a, &a, &a_depth) || !project_point(line.b, &b, &b_depth))
                    continue;
                const float vx = static_cast<float>(b.x - a.x);
                const float vy = static_cast<float>(b.y - a.y);
                const float length_squared = vx * vx + vy * vy;
                float t = length_squared > .001f
                              ? ((x - a.x) * vx + (y - a.y) * vy) / length_squared
                              : 0.0f;
                t = std::clamp(t, 0.0f, 1.0f);
                const float dx = x - (a.x + vx * t);
                const float dy = y - (a.y + vy * t);
                const int distance = static_cast<int>(dx * dx + dy * dy);
                if (distance >= edge_distance_limit)
                    continue;
                const Asura_Vector_3 edge_point = add(line.a, mul(sub(line.b, line.a), t));
                if (!entity_is_selected(i) && environment_occludes_view_position(edge_point))
                    continue;
                const float depth = fminf(a_depth, b_depth);
                if (distance < best_distance || (distance == best_distance && depth < best_depth)) {
                    best_distance = distance;
                    best_depth = depth;
                    best = i;
                }
            }
            continue;
        }
        POINT p{};
        float depth = 0;
        if (!project_point(entity_view_position(entity.position), &p, &depth))
            continue;
        const int dx = p.x - x, dy = p.y - y, d = dx * dx + dy * dy;
        if (d < best_distance || (d == best_distance && depth < best_depth)) {
            best_distance = d;
            best_depth = depth;
            best = i;
        }
    }
    return best;
}

COLORREF entity_color(EntityKind kind) {
    switch (kind) {
    case EntityKind::SpawnPoint: return RGB(80, 220, 120);
    case EntityKind::Light: return RGB(255, 220, 70);
    case EntityKind::Sound: return RGB(80, 190, 255);
    case EntityKind::Pickup: return RGB(255, 116, 46);
    case EntityKind::StaticObject: return RGB(80, 205, 175);
    case EntityKind::AssassinationTarget: return RGB(255, 38, 64);
    case EntityKind::PositionMarker: return RGB(190, 88, 255);
    case EntityKind::BuildingVolume: return RGB(38, 224, 255);
    default: return RGB(255, 120, 80);
    }
}

void draw_grid(HDC dc) {
    const float extent = fmaxf(50.0f, g.mesh.radius * 1.5f);
    float step = powf(10.0f, floorf(log10f(extent / 10.0f)));
    step = fmaxf(1.0f, step);
    HPEN minor = CreatePen(PS_SOLID, 1, RGB(48, 55, 62));
    HPEN major = CreatePen(PS_SOLID, 1, RGB(72, 82, 92));
    HGDIOBJ old = SelectObject(dc, minor);
    const int lines = static_cast<int>(extent / step);
    for (int i = -lines; i <= lines; ++i) {
        SelectObject(dc, i == 0 ? major : minor);
        POINT a{}, b{};
        if (project_point({i * step, 0, -extent}, &a) && project_point({i * step, 0, extent}, &b)) {
            MoveToEx(dc, a.x, a.y, nullptr);
            LineTo(dc, b.x, b.y);
        }
        if (project_point({-extent, 0, i * step}, &a) && project_point({extent, 0, i * step}, &b)) {
            MoveToEx(dc, a.x, a.y, nullptr);
            LineTo(dc, b.x, b.y);
        }
    }
    SelectObject(dc, old);
    DeleteObject(minor);
    DeleteObject(major);
}

struct ProjectedMeshVertex {
    POINT point{};
    float depth = 0;
    bool visible = false;
};

void project_mesh_vertices(std::vector<ProjectedMeshVertex>* projected) {
    const RECT vr = viewport_rect();
    Asura_Vector_3 cam, right, up, forward;
    camera_axes(&cam, &right, &up, &forward);
    const float scale = .85f * static_cast<float>(std::min(vr.right - vr.left, vr.bottom - vr.top));
    const float center_x = (vr.left + vr.right) * .5f, center_y = (vr.top + vr.bottom) * .5f;
    projected->resize(g.mesh.positions.size());
    for (size_t i = 0; i < g.mesh.positions.size(); ++i) {
        ProjectedMeshVertex& out = (*projected)[i];
        const Asura_Vector_3 rel = sub(g.mesh.positions[i], cam);
        out.depth = dot(rel, forward);
        out.visible = out.depth > .05f;
        if (!out.visible)
            continue;
        const float x = center_x + dot(rel, right) * scale / out.depth;
        const float y = center_y - dot(rel, up) * scale / out.depth;
        out.point.x = static_cast<LONG>(std::clamp(x, -1000000.0f, 1000000.0f));
        out.point.y = static_cast<LONG>(std::clamp(y, -1000000.0f, 1000000.0f));
    }
}

bool projected_face_visible(const std::array<uint32_t, 3>& face, const std::vector<ProjectedMeshVertex>& projected,
                            const RECT& vr) {
    const ProjectedMeshVertex& a = projected[face[0]];
    const ProjectedMeshVertex& b = projected[face[1]];
    const ProjectedMeshVertex& c = projected[face[2]];
    if (!a.visible || !b.visible || !c.visible)
        return false;
    const LONG min_x = std::min({a.point.x, b.point.x, c.point.x});
    const LONG max_x = std::max({a.point.x, b.point.x, c.point.x});
    const LONG min_y = std::min({a.point.y, b.point.y, c.point.y});
    const LONG max_y = std::max({a.point.y, b.point.y, c.point.y});
    return max_x >= vr.left && min_x < vr.right && max_y >= vr.top && min_y < vr.bottom;
}

void draw_fast_mesh(HDC dc, const RECT& vr, const std::vector<ProjectedMeshVertex>& projected) {
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(125, 148, 160));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    constexpr size_t kBatchFaces = 4096;
    std::vector<POINT> points;
    std::vector<DWORD> counts;
    points.reserve(kBatchFaces * 4);
    counts.reserve(kBatchFaces);
    auto flush = [&]() {
        if (!counts.empty())
            PolyPolyline(dc, points.data(), counts.data(), static_cast<DWORD>(counts.size()));
        points.clear();
        counts.clear();
    };
    for (const auto& face : g.mesh.faces) {
        if (!projected_face_visible(face, projected, vr))
            continue;
        points.push_back(projected[face[0]].point);
        points.push_back(projected[face[1]].point);
        points.push_back(projected[face[2]].point);
        points.push_back(projected[face[0]].point);
        counts.push_back(4);
        if (counts.size() == kBatchFaces)
            flush();
    }
    flush();
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

void draw_shaded_mesh(HDC dc, const RECT& vr, const std::vector<ProjectedMeshVertex>& projected) {
    struct PreviewTriangle {
        POINT points[3];
        float depth;
        uint8_t shade;
    };
    std::vector<PreviewTriangle> triangles;
    triangles.reserve(g.mesh.faces.size());
    const Asura_Vector_3 light = normalized({-.35f, .8f, -.45f});
    for (size_t i = 0; i < g.mesh.faces.size(); ++i) {
        const auto& f = g.mesh.faces[i];
        if (!projected_face_visible(f, projected, vr))
            continue;
        PreviewTriangle triangle{};
        triangle.points[0] = projected[f[0]].point;
        triangle.points[1] = projected[f[1]].point;
        triangle.points[2] = projected[f[2]].point;
        const Asura_Vector_3 ab = sub(g.mesh.positions[f[1]], g.mesh.positions[f[0]]);
        const Asura_Vector_3 ac = sub(g.mesh.positions[f[2]], g.mesh.positions[f[0]]);
        const Asura_Vector_3 normal = normalized(cross(ab, ac));
        const float illumination = .28f + .72f * fabsf(dot(normal, light));
        triangle.depth = (projected[f[0]].depth + projected[f[1]].depth + projected[f[2]].depth) / 3.0f;
        triangle.shade = static_cast<uint8_t>(std::clamp(static_cast<int>(illumination * 15.0f), 0, 15));
        triangles.push_back(triangle);
    }
    std::sort(triangles.begin(), triangles.end(), [](const PreviewTriangle& a, const PreviewTriangle& b) {
        return a.depth > b.depth;
    });
    HBRUSH brushes[16]{};
    HPEN pens[16]{};
    for (int i = 0; i < 16; ++i) {
        const int r = 48 + i * 6, green = 59 + i * 7, b = 65 + i * 8;
        brushes[i] = CreateSolidBrush(RGB(r, green, b));
        pens[i] = CreatePen(PS_SOLID, 1, RGB(std::max(28, r - 22), std::max(34, green - 22), std::max(38, b - 22)));
    }
    HGDIOBJ old_brush = GetCurrentObject(dc, OBJ_BRUSH);
    HGDIOBJ old_pen = GetCurrentObject(dc, OBJ_PEN);
    int selected_shade = -1;
    SetPolyFillMode(dc, WINDING);
    for (const PreviewTriangle& triangle : triangles) {
        if (selected_shade != triangle.shade) {
            selected_shade = triangle.shade;
            SelectObject(dc, brushes[selected_shade]);
            SelectObject(dc, pens[selected_shade]);
        }
        Polygon(dc, triangle.points, 3);
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    for (int i = 0; i < 16; ++i) {
        DeleteObject(brushes[i]);
        DeleteObject(pens[i]);
    }
}

void draw_environment(HDC dc) {
    const RECT vr = viewport_rect();
    HBRUSH bg = CreateSolidBrush(RGB(25, 30, 35));
    FillRect(dc, &vr, bg);
    DeleteObject(bg);
    HRGN clip = CreateRectRgn(vr.left, vr.top, vr.right, vr.bottom);
    SelectClipRgn(dc, clip);
    draw_grid(dc);
    std::vector<ProjectedMeshVertex> projected;
    project_mesh_vertices(&projected);
    if (g.fast_preview)
        draw_fast_mesh(dc, vr, projected);
    else
        draw_shaded_mesh(dc, vr, projected);
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    FrameRect(dc, &vr, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
}

void draw_gizmo_lines(HDC dc, const std::vector<LightGizmoLine>& lines, int width, COLORREF color) {
    if (lines.empty())
        return;
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    for (const LightGizmoLine& line : lines) {
        POINT a{}, b{};
        if (project_point(line.a, &a) && project_point(line.b, &b)) {
            MoveToEx(dc, a.x, a.y, nullptr);
            LineTo(dc, b.x, b.y);
        }
    }
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

void draw_light_gizmo(HDC dc, const Entity& entity, bool selected) {
    std::vector<LightGizmoLine> lines;
    lines.reserve(kMaximumLightGizmoLines);
    append_light_gizmo(entity, &lines);
    draw_gizmo_lines(dc, lines, selected ? 2 : 1,
                     selected ? RGB(255, 235, 90) : RGB(148, 120, 24));
}

void draw_sound_gizmo(HDC dc, const Entity& entity) {
    std::vector<LightGizmoLine> lines;
    lines.reserve(kLightRangeSegments * 3);
    append_sound_gizmo(entity, &lines);
    draw_gizmo_lines(dc, lines, 2, RGB(65, 205, 255));
}

bool draw_entity_model(HDC dc, const Entity& entity, bool selected) {
    const EntityModel* model = entity_render_model(entity);
    if (!model)
        return false;
    COLORREF color = entity.kind == EntityKind::Pickup
                         ? RGB(210, 92, 28)
                         : entity.kind == EntityKind::StaticObject ? RGB(45, 155, 128)
                         : entity.value_u32_a == 5 ? RGB(135, 165, 67) : RGB(112, 128, 138);
    if (selected) {
        color = entity.kind == EntityKind::Pickup
                    ? RGB(255, 188, 70)
                    : entity.kind == EntityKind::StaticObject ? RGB(110, 255, 220)
                    : entity.value_u32_a == 5 ? RGB(220, 240, 105) : RGB(190, 218, 232);
    }
    HPEN pen = CreatePen(PS_SOLID, selected ? 2 : 1, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    for (const auto& face : model->faces) {
        POINT points[4]{};
        bool visible = true;
        for (int corner = 0; corner < 3; ++corner) {
            const Asura_Vector_3 position =
                entity_model_view_position(model->vertices[face[corner]].position, entity);
            visible = visible && project_point(position, &points[corner]);
        }
        if (visible) {
            points[3] = points[0];
            Polyline(dc, points, 4);
        }
    }
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    return true;
}

void draw_entities(HDC dc) {
    const RECT vr = viewport_rect();
    HRGN clip = CreateRectRgn(vr.left, vr.top, vr.right, vr.bottom);
    SelectClipRgn(dc, clip);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(230, 235, 240));
    for (int i = 0; i < static_cast<int>(g.document.entities.size()); ++i) {
        const Entity& e = g.document.entities[i];
        const bool selected = entity_is_selected(i);
        if (e.kind != EntityKind::BuildingVolume && !selected &&
            environment_occludes_view_position(entity_view_position(e.position)))
            continue;
        POINT p{};
        if (!project_point(entity_view_position(e.position), &p))
            continue;
        const COLORREF color = entity_color(e.kind);
        if (e.kind == EntityKind::Light && selected)
            draw_light_gizmo(dc, e, true);
        else if (e.kind == EntityKind::Sound && selected)
            draw_sound_gizmo(dc, e);
        if (e.kind == EntityKind::BuildingVolume) {
            std::vector<LightGizmoLine> lines;
            lines.reserve(kLightBoundingBoxLines);
            append_oriented_bounds_gizmo(e, &lines);
            draw_gizmo_lines(dc, lines, selected ? 2 : 1,
                             selected ? RGB(255, 255, 255) : color);
            if (selected)
                TextOutA(dc, p.x + 10, p.y - 8, e.name.c_str(), static_cast<int>(e.name.size()));
            continue;
        }
        if (e.kind == EntityKind::SpawnPoint && (e.value_u32_a & SnipeSpawnTeam_Camera)) {
            std::vector<LightGizmoLine> lines;
            lines.reserve(kCameraSpawnArrowLines);
            const float marker = fmaxf(.35f, g.mesh.radius * .008f) * (selected ? 1.6f : 1.0f);
            append_camera_spawn_arrow(e, marker, &lines);
            draw_gizmo_lines(dc, lines, selected ? 2 : 1,
                             selected ? RGB(255, 255, 255) : color);
        }
        if ((e.kind == EntityKind::SpawnPoint || e.kind == EntityKind::Pickup ||
             e.kind == EntityKind::StaticObject) &&
            draw_entity_model(dc, e, selected)) {
            if (selected)
                TextOutA(dc, p.x + 10, p.y - 8, e.name.c_str(), static_cast<int>(e.name.size()));
            continue;
        }
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, selected ? 3 : 1, selected ? RGB(255, 255, 255) : color);
        HGDIOBJ ob = SelectObject(dc, brush), op = SelectObject(dc, pen);
        const int r = selected ? 7 : 5;
        Ellipse(dc, p.x - r, p.y - r, p.x + r + 1, p.y + r + 1);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(brush);
        DeleteObject(pen);
        if (selected)
            TextOutA(dc, p.x + 10, p.y - 8, e.name.c_str(), static_cast<int>(e.name.size()));
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
}

LRESULT CALLBACK viewport_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        gpu_render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (gpu_ready()) {
            gpu_resize(std::max(1, static_cast<int>(LOWORD(lparam))),
                       std::max(1, static_cast<int>(HIWORD(lparam))));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        MapWindowPoints(hwnd, g.window, &point, 1);
        return SendMessageA(g.window, message, wparam, MAKELPARAM(point.x, point.y));
    }
    case WM_MOUSEWHEEL:
        return SendMessageA(g.window, message, wparam, lparam);
    case WM_DESTROY:
        gpu_shutdown();
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

HWND make_control(const char* cls, const char* text, DWORD style, int id) {
    HWND h = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.window,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
    SendMessage(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return h;
}

void layout_controls() {
    RECT r{};
    GetClientRect(g.window, &r);
    const RECT vr = viewport_rect();
    if (g.viewport)
        MoveWindow(g.viewport, vr.left, vr.top, vr.right - vr.left, vr.bottom - vr.top, TRUE);
    const int right = r.right - ui_px(262);
    const struct { int id, x, y, w; } top[] = {
        {ID_OPEN_OBJ, 8, 6, 78},              {ID_OPEN_PC, 90, 6, 78},
        {ID_OPEN_PROJECT, 172, 6, 98},         {ID_SAVE_PROJECT, 274, 6, 96},
        {ID_EXPORT_PC, 374, 6, 88},            {ID_EXPORT_OBJ, 466, 6, 88},
        {ID_MATERIAL_MAP, 8, 38, 132},         {ID_EXPORT_MATERIAL_MAP, 144, 38, 132},
        {ID_TEXTURE_DIR, 280, 38, 102},        {ID_WEAPONS_DONOR, 386, 38, 112},
        {ID_OBJECT_DONOR, 502, 38, 104},       {ID_SKYBOX_TEXTURES, 610, 38, 120},
        {ID_TOGGLE_RAIN, 734, 38, 58}
    };
    for (auto c : top)
        MoveWindow(GetDlgItem(g.window, c.id), ui_px(c.x), ui_px(c.y), ui_px(c.w),
                   ui_px(28), TRUE);
    MoveWindow(g.ambience_properties, right, ui_px(6), ui_px(252), ui_px(28), TRUE);
    const int list_top = ui_px(78);
    int y = std::max(ui_px(172), static_cast<int>(r.bottom) - ui_px(245));
    MoveWindow(g.list, ui_px(8), list_top, ui_px(220),
               std::max(ui_px(80), y - list_top - ui_px(8)), TRUE);
    const int bw = ui_px(106);
    MoveWindow(GetDlgItem(g.window, ID_ADD_SPAWN), ui_px(8), y, bw, ui_px(27), TRUE);
    MoveWindow(GetDlgItem(g.window, ID_ADD_LIGHT), ui_px(120), y, bw, ui_px(27), TRUE);
    y += ui_px(31);
    MoveWindow(GetDlgItem(g.window, ID_ADD_PICKUP), ui_px(8), y, bw, ui_px(27), TRUE);
    MoveWindow(GetDlgItem(g.window, ID_ADD_STATIC_OBJECT), ui_px(120), y, bw, ui_px(27), TRUE);
    y += ui_px(31);
    MoveWindow(GetDlgItem(g.window, ID_ADD_SOUND), ui_px(8), y, bw, ui_px(27), TRUE);
    MoveWindow(GetDlgItem(g.window, ID_ADD_BUILDING_VOLUME), ui_px(120), y, bw, ui_px(27), TRUE);
    y += ui_px(31);
    MoveWindow(GetDlgItem(g.window, ID_DELETE_ENTITY), ui_px(8), y, ui_px(218), ui_px(27), TRUE);
    y += ui_px(38);
    HWND hint = GetDlgItem(g.window, 900);
    MoveWindow(hint, ui_px(8), y, ui_px(220), ui_px(60), TRUE);
    y += ui_px(64);
    MoveWindow(g.backface_cull_toggle, ui_px(8), y, ui_px(218), ui_px(22), TRUE);

    const int label_x = right, edit_x = right + ui_px(88), ew = ui_px(164);
    const int property_edit_x = right + ui_px(116), property_ew = ui_px(136);
    int iy = ui_px(52);
    MoveWindow(GetDlgItem(g.window, 910), label_x, iy + ui_px(3), ui_px(84), ui_px(22), TRUE);
    MoveWindow(g.name, edit_x, iy, ew, ui_px(24), TRUE);
    iy += ui_px(34);
    const int labels[] = {911, 912, 913, 914, 915, 916};
    HWND edits[] = {g.pos[0], g.pos[1], g.pos[2], g.rot[0], g.rot[1], g.rot[2]};
    for (int i = 0; i < 6; ++i, iy += ui_px(30)) {
        MoveWindow(GetDlgItem(g.window, labels[i]), label_x, iy + ui_px(3), ui_px(84), ui_px(22), TRUE);
        MoveWindow(edits[i], edit_x, iy, ew, ui_px(24), TRUE);
    }
    MoveWindow(g.value_label[0], label_x, iy + ui_px(3), ui_px(112), ui_px(22), TRUE);
    MoveWindow(g.value[0], property_edit_x, iy, property_ew, ui_px(24), TRUE);
    MoveWindow(g.pickup_item, property_edit_x, iy, property_ew, ui_px(240), TRUE);
    iy += ui_px(30);
    MoveWindow(g.value_label[1], label_x, iy + ui_px(3), ui_px(112), ui_px(22), TRUE);
    MoveWindow(g.value[1], property_edit_x, iy, property_ew, ui_px(24), TRUE);
    iy += ui_px(34);
    MoveWindow(g.value_label[2], label_x, iy + ui_px(3), ui_px(112), ui_px(22), TRUE);
    MoveWindow(g.value[2], property_edit_x, iy, property_ew, ui_px(24), TRUE);
    MoveWindow(g.sound_browse, edit_x, iy, ui_px(80), ui_px(26), TRUE);
    MoveWindow(g.sound_preview, edit_x + ui_px(84), iy, ui_px(80), ui_px(26), TRUE);
    MoveWindow(g.sound_loop, label_x, iy, ui_px(82), ui_px(26), TRUE);
    MoveWindow(g.sound_properties, label_x, iy + ui_px(34), ui_px(252), ui_px(28), TRUE);
    MoveWindow(g.light_properties, edit_x, iy, ew, ui_px(26), TRUE);
    MoveWindow(g.spawn_team_label, label_x, iy, ui_px(252), ui_px(22), TRUE);
    for (int i = 0; i < static_cast<int>(_countof(kSpawnTeamControls)); ++i) {
        const int column = i % 2, row = i / 2;
        MoveWindow(g.spawn_team_checks[i], label_x + ui_px(column * 126),
                   iy + ui_px(22 + row * 24), ui_px(126), ui_px(22), TRUE);
    }
    MoveWindow(g.spawn_game_mode_label, label_x, iy + ui_px(72), ui_px(252), ui_px(22), TRUE);
    for (int i = 0; i < static_cast<int>(_countof(kSpawnGameModeControls)); ++i) {
        const int column = i % 2, row = i / 2;
        MoveWindow(g.spawn_game_mode_checks[i], label_x + ui_px(column * 126),
                   iy + ui_px(94 + row * 24), ui_px(126), ui_px(22), TRUE);
    }
    iy += ui_px(172);
    MoveWindow(GetDlgItem(g.window, ID_APPLY_INSPECTOR), label_x, iy, ui_px(252), ui_px(30), TRUE);
    MoveWindow(g.status, ui_px(8), r.bottom - ui_px(21),
               std::max(ui_px(20), static_cast<int>(r.right) - ui_px(16)), ui_px(18), TRUE);
}

void create_controls() {
    const RECT vr = viewport_rect();
    g.viewport = CreateWindowExA(0, "Asura2005Viewport", "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                 vr.left, vr.top, vr.right - vr.left, vr.bottom - vr.top, g.window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VIEWPORT)), GetModuleHandle(nullptr),
                                 nullptr);
    if (!g.viewport || !gpu_init(g.viewport)) {
        if (g.viewport)
            DestroyWindow(g.viewport);
        g.viewport = nullptr;
    }
    make_control("BUTTON", "Open .OBJ", BS_DEFPUSHBUTTON, ID_OPEN_OBJ);
    constexpr const char* file_button_text[] = {
        "Open .PC", "Open project", "Save project", "Export .PC", "Export .OBJ",
        "Import material map", "Export material map", "Texture folder", "Weapons donor",
        "Objects donor", "Skybox properties"};
    constexpr int file_button_ids[] = {
        ID_OPEN_PC, ID_OPEN_PROJECT, ID_SAVE_PROJECT, ID_EXPORT_PC, ID_EXPORT_OBJ,
        ID_MATERIAL_MAP, ID_EXPORT_MATERIAL_MAP, ID_TEXTURE_DIR, ID_WEAPONS_DONOR,
        ID_OBJECT_DONOR, ID_SKYBOX_TEXTURES};
    for (size_t i = 0; i < _countof(file_button_ids); ++i)
        make_control("BUTTON", file_button_text[i], BS_PUSHBUTTON, file_button_ids[i]);
    g.rain_toggle = make_control("BUTTON", "Rain", BS_AUTOCHECKBOX, ID_TOGGLE_RAIN);
    g.backface_cull_toggle =
        make_control("BUTTON", "Viewport: Cull backfaces", BS_AUTOCHECKBOX,
                     ID_TOGGLE_BACKFACE_CULLING);
    g.ambience_properties = make_control("BUTTON", "Ambience sound", BS_PUSHBUTTON, ID_AMBIENCE_PROPERTIES);
    g.list = make_control("LISTBOX", "", LBS_NOTIFY | LBS_EXTENDEDSEL | WS_VSCROLL | WS_BORDER,
                          ID_ENTITY_LIST);
    constexpr const char* entity_button_text[] = {
        "+ Spawn", "+ Light", "+ Pickup", "+ Object", "+ Sound", "+ Indoor zone", "Delete selected"};
    constexpr int entity_button_ids[] = {
        ID_ADD_SPAWN, ID_ADD_LIGHT, ID_ADD_PICKUP, ID_ADD_STATIC_OBJECT, ID_ADD_SOUND,
        ID_ADD_BUILDING_VOLUME, ID_DELETE_ENTITY};
    for (size_t i = 0; i < _countof(entity_button_ids); ++i)
        make_control("BUTTON", entity_button_text[i], BS_PUSHBUTTON, entity_button_ids[i]);
    make_control("STATIC",
                 "Right: orbit; Middle: pan; Wheel: zoom\r\n"
                 "Ctrl/Shift: select same-type entities\r\n"
                 "Ctrl+Z/Y: undo/redo\r\n"
                 "Ctrl+C/V: copy/paste; Del: remove",
                 SS_LEFT, 900);
    constexpr const char* transform_label_text[] = {
        "Name", "Position X", "Position Y (-up)", "Position Z", "Pitch", "Yaw", "Roll"};
    for (size_t i = 0; i < _countof(transform_label_text); ++i)
        make_control("STATIC", transform_label_text[i], SS_LEFT, 910 + static_cast<int>(i));
    HWND* transform_outputs[] = {&g.name, &g.pos[0], &g.pos[1], &g.pos[2], &g.rot[0], &g.rot[1], &g.rot[2]};
    constexpr int transform_ids[] = {ID_NAME, ID_POS_X, ID_POS_Y, ID_POS_Z, ID_ROT_X, ID_ROT_Y, ID_ROT_Z};
    for (size_t i = 0; i < _countof(transform_ids); ++i)
        *transform_outputs[i] = make_control("EDIT", "", ES_AUTOHSCROLL | WS_BORDER, transform_ids[i]);
    constexpr const char* property_label_text[] = {"Property A", "Property B", "Property C"};
    constexpr int property_label_ids[] = {917, 918, 921};
    for (size_t i = 0; i < _countof(property_label_ids); ++i)
        g.value_label[i] = make_control("STATIC", property_label_text[i], SS_LEFT, property_label_ids[i]);
    constexpr int property_ids[] = {ID_VALUE_A, ID_VALUE_B, ID_VALUE_C};
    for (size_t i = 0; i < _countof(property_ids); ++i)
        g.value[i] = make_control("EDIT", "", ES_AUTOHSCROLL | WS_BORDER, property_ids[i]);
    g.pickup_item = make_control("COMBOBOX", "", CBS_DROPDOWNLIST | CBS_AUTOHSCROLL | WS_VSCROLL,
                                 ID_PICKUP_ITEM);
    g.sound_browse = make_control("BUTTON", "Choose .WAV", BS_PUSHBUTTON, ID_BROWSE_SOUND);
    g.sound_loop = make_control("BUTTON", "Loop", BS_AUTOCHECKBOX, ID_SOUND_LOOP);
    g.sound_preview = make_control("BUTTON", "Play preview", BS_PUSHBUTTON, ID_SOUND_PREVIEW);
    g.sound_properties = make_control("BUTTON", "Activation properties", BS_PUSHBUTTON, ID_SOUND_PROPERTIES);
    g.spawn_team_label = make_control("STATIC", "Teams", SS_LEFT, 919);
    g.spawn_game_mode_label = make_control("STATIC", "Game modes", SS_LEFT, 920);
    for (int i = 0; i < static_cast<int>(_countof(kSpawnTeamControls)); ++i)
        g.spawn_team_checks[i] = make_control("BUTTON", kSpawnTeamControls[i].label, BS_AUTOCHECKBOX,
                                             ID_SPAWN_TEAM_FIRST + i);
    for (int i = 0; i < static_cast<int>(_countof(kSpawnGameModeControls)); ++i)
        g.spawn_game_mode_checks[i] = make_control("BUTTON", kSpawnGameModeControls[i].label, BS_AUTOCHECKBOX,
                                                  ID_SPAWN_GAME_MODE_FIRST + i);
    g.light_properties =
        make_control("BUTTON", "All light properties...", BS_PUSHBUTTON, ID_LIGHT_PROPERTIES);
    make_control("BUTTON", "Apply properties", BS_PUSHBUTTON, ID_APPLY_INSPECTOR);
    g.status = make_control("STATIC", "Open a Blender OBJ or original .PC level to begin.", SS_LEFT, ID_STATUS);
    layout_controls();
    refresh_inspector();
}

bool confirm_discard() {
    if (!g.document.dirty)
        return true;
    return MessageBoxA(g.window, "Discard unsaved editor changes?", "Asura Level Editor",
                       MB_ICONQUESTION | MB_YESNO) == IDYES;
}

void command_open_obj() {
    if (!g.document.source_pc_path.empty() && !confirm_discard())
        return;
    std::string path = g.document.obj_path;
    if (choose_path(g.window, false, "Open environment OBJ", "Wavefront OBJ\0*.obj\0All files\0*.*\0", "obj", &path))
        open_obj_path(path);
}

void enrich_project_pickup_templates(Document* document, const Document& imported) {
    if (document->pickup_templates.empty())
        document->pickup_templates = imported.pickup_templates;
    size_t imported_pickups = 0, matched_pickups = 0;
    for (const Entity& source : imported.entities) {
        if (source.kind != EntityKind::Pickup || !source.source_entity_record)
            continue;
        ++imported_pickups;
        for (Entity& saved : document->entities) {
            if (saved.kind != EntityKind::Pickup || saved.guid != source.guid)
                continue;
            ++matched_pickups;
            if (!saved.pickup_has_template) {
                saved.entity_padding = source.entity_padding;
                saved.source_entity_classification = source.source_entity_classification;
                const PickupTemplate source_template = pickup_template_from_entity(source);
                adopt_pickup_template(&saved, source_template);
            }
            break;
        }
    }
    if (!document->source_pickup_inventory_complete && imported_pickups == matched_pickups)
        document->source_pickup_inventory_complete = true;
}

void merge_pickup_templates(Document* document, const std::vector<PickupTemplate>& incoming) {
    for (const PickupTemplate& pickup : incoming) {
        bool present = false;
        for (const PickupTemplate& existing : document->pickup_templates)
            present |= existing.item_id == pickup.item_id;
        if (!present)
            document->pickup_templates.push_back(pickup);
    }
}

void merge_static_object_templates(Document* document,
                                   const std::vector<StaticObjectTemplate>& incoming);

void enrich_project_static_object_templates(Document* document, const Document& imported) {
    if (document->static_object_templates.empty())
        document->static_object_templates = imported.static_object_templates;
    else
        merge_static_object_templates(document, imported.static_object_templates);
    size_t imported_objects = 0, matched_objects = 0;
    for (const Entity& source : imported.entities) {
        if (source.kind != EntityKind::StaticObject || !source.source_entity_record)
            continue;
        ++imported_objects;
        for (Entity& saved : document->entities) {
            if (saved.kind != EntityKind::StaticObject || saved.guid != source.guid)
                continue;
            ++matched_objects;
            if (!saved.static_object_has_template) {
                saved.entity_padding = source.entity_padding;
                saved.source_entity_classification = source.source_entity_classification;
                saved.static_object_body = source.static_object_body;
                saved.static_object_has_template = true;
            }
            break;
        }
    }
    if (!document->source_static_object_inventory_complete && imported_objects == matched_objects)
        document->source_static_object_inventory_complete = true;
}

bool load_document_preview(Document* document, Mesh* mesh, std::string* why,
                           std::vector<PickupModel>* pickup_models = nullptr,
                           std::vector<StaticObjectModel>* object_models = nullptr) {
    if (!document->obj_path.empty()) {
        if (!load_preview_mesh(document->obj_path, document->material_map, mesh, why))
            return false;
        if (pickup_models)
            pickup_models->clear();
        if (object_models)
            object_models->clear();
        if (!document->weapons_donor.empty()) {
            std::vector<PickupTemplate> donor_templates;
            std::vector<PickupModel> donor_models;
            if (!load_pickup_donor(document->weapons_donor, &donor_templates, &donor_models, why))
                return false;
            merge_pickup_templates(document, donor_templates);
            if (pickup_models)
                *pickup_models = std::move(donor_models);
        }
        if (!document->object_donors.empty()) {
            std::vector<StaticObjectTemplate> donor_templates;
            std::vector<StaticObjectModel> donor_models;
            if (!load_static_object_donors(document->object_donors, &donor_templates, &donor_models, why))
                return false;
            merge_static_object_templates(document, donor_templates);
            if (object_models)
                *object_models = std::move(donor_models);
        }
        return true;
    }
    if (!document->source_pc_path.empty()) {
        Document imported;
        if (!load_pc_level(document->source_pc_path, &imported, mesh, why, pickup_models, object_models))
            return false;
        if (!document->skybox.source_record && imported.skybox.source_record)
            document->skybox = imported.skybox;
        if (!document->weather_source_record && imported.weather_source_record) {
            document->weather_source_record = true;
            document->rain_enabled = imported.rain_enabled;
        }
        if (!document->ambient_source_record && imported.ambient_source_record) {
            document->ambient_source_record = true;
            document->ambient_stream_path = imported.ambient_stream_path;
            document->ambient_volume = imported.ambient_volume;
        }
        enrich_project_pickup_templates(document, imported);
        enrich_project_static_object_templates(document, imported);
        if (!document->object_donors.empty()) {
            std::vector<StaticObjectTemplate> donor_templates;
            std::vector<StaticObjectModel> donor_models;
            if (!load_static_object_donors(document->object_donors, &donor_templates, &donor_models, why))
                return false;
            merge_static_object_templates(document, donor_templates);
            if (object_models) {
                for (StaticObjectModel& model : donor_models) {
                    bool present = false;
                    for (const StaticObjectModel& existing : *object_models)
                        present |= existing.file_id == model.file_id;
                    if (!present)
                        object_models->push_back(std::move(model));
                }
            }
        }
        return true;
    }
    if (pickup_models)
        pickup_models->clear();
    if (object_models)
        object_models->clear();
    *mesh = {};
    return true;
}

bool open_pc_path(const std::string& path) {
    stop_sound_preview();
    set_status("Reading PC environment and supported entities...");
    UpdateWindow(g.window);
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    Document document;
    Mesh mesh;
    std::string why;
    std::vector<PickupModel> pickup_models;
    std::vector<StaticObjectModel> object_models;
    const bool ok = load_pc_level(path, &document, &mesh, &why, &pickup_models, &object_models);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!ok) {
        set_status("Could not open the .PC level.");
        MessageBoxA(g.window, why.c_str(), "Could not open .PC", MB_ICONERROR);
        return false;
    }
    g.document = std::move(document);
    g.mesh = std::move(mesh);
    g.pickup_models = std::move(pickup_models);
    g.static_object_models = std::move(object_models);
    set_single_selection_state(g.document.entities.empty() ? -1 : 0);
    g.pending_kind = -1;
    std::string skybox_why;
    const bool skybox_loaded = gpu_load_pc_skybox(path, g.document.skybox, &skybox_why);
    frame_mesh();
    reset_history(true);
    refresh_list();
    refresh_inspector();
    update_title();
    size_t source_object_count = 0;
    for (const Entity& entity : g.document.entities)
        source_object_count += entity.source_entity_record;
    const size_t authored_entity_count = g.document.entities.size() - source_object_count;
    char status[420];
    snprintf(status, sizeof(status),
             skybox_loaded
                 ? "Original .PC loaded with embedded skybox: %zu vertices, %zu triangles, %zu lights/spawns/sounds and %zu source objects/targets/markers."
                 : "Original .PC loaded: %zu vertices, %zu triangles, %zu lights/spawns/sounds and %zu source objects/targets/markers; embedded skybox unavailable.",
             g.mesh.positions.size(), g.mesh.faces.size(), authored_entity_count, source_object_count);
    set_status(status);
    request_redraw();
    return true;
}

void command_open_pc() {
    if (!confirm_discard())
        return;
    std::string path = g.document.source_pc_path;
    if (!choose_path(g.window, false, "Open original Sniper Elite PC level",
                     "Sniper Elite PC level\0*.PC\0All files\0*.*\0", "PC", &path))
        return;
    open_pc_path(path);
}

void command_save_project() {
    std::string path = g.document.project_path;
    if (path.empty() &&
        !choose_path(g.window, true, "Save editor project", "Asura Level Editor project\0*.alev\0", "alev", &path))
        return;
    std::string why;
    if (!save_project(g.document, path.c_str(), &why)) {
        MessageBoxA(g.window, why.c_str(), "Could not save project", MB_ICONERROR);
        return;
    }
    g.document.project_path = path;
    g.history.mark_saved(&g.document);
    update_title();
    set_status("Project saved.");
}

bool reload_skybox_preview(bool show_warning) {
    std::string why;
    const bool use_embedded = g.document.sky_texture_dir.empty() && !g.document.source_pc_path.empty();
    const bool loaded = use_embedded ? gpu_load_pc_skybox(g.document.source_pc_path, g.document.skybox, &why)
                                     : gpu_load_skybox(g.document.sky_texture_dir, &why);
    if (loaded) {
        gpu_rebuild_skybox_vertices(g.document.skybox.orientation_radians,
                                    g.document.skybox.back_texture_is_front_upside_down,
                                    g.document.skybox.right_texture_is_left_upside_down, nullptr);
        gpu_set_skybox_tint(g.document.skybox);
    }
    if (!loaded && show_warning && (use_embedded || !g.document.sky_texture_dir.empty()))
        MessageBoxA(g.window, why.c_str(), "Skybox preview unavailable", MB_ICONWARNING);
    request_redraw();
    return loaded;
}

bool same_skybox_settings(const SkyboxSettings& a, const SkyboxSettings& b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue &&
           a.orientation_radians == b.orientation_radians && a.texture_paths == b.texture_paths &&
           a.draw_clouds == b.draw_clouds &&
           a.back_texture_is_front_upside_down == b.back_texture_is_front_upside_down &&
           a.right_texture_is_left_upside_down == b.right_texture_is_left_upside_down &&
           a.source_record == b.source_record;
}

bool same_ambience_settings(const Document& a, const Document& b) {
    return a.ambient_stream_path == b.ambient_stream_path &&
           memcmp(&a.ambient_volume, &b.ambient_volume, sizeof(a.ambient_volume)) == 0 &&
           a.ambient_source_record == b.ambient_source_record;
}

bool refresh_history_derived_resources(const Document& previous, std::string* why) {
    const bool preview_changed = previous.obj_path != g.document.obj_path ||
                                 previous.source_pc_path != g.document.source_pc_path ||
                                 previous.material_map != g.document.material_map ||
                                 previous.weapons_donor != g.document.weapons_donor ||
                                 previous.object_donors != g.document.object_donors;
    const bool textures_changed = previous.texture_dir != g.document.texture_dir ||
                                  previous.material_map != g.document.material_map ||
                                  previous.rain_enabled != g.document.rain_enabled;
    const bool skybox_changed = previous.source_pc_path != g.document.source_pc_path ||
                                previous.sky_texture_dir != g.document.sky_texture_dir ||
                                !same_skybox_settings(previous.skybox, g.document.skybox);

    bool ok = true;
    if (preview_changed) {
        // Preview loading enriches recovered document metadata.  Use a copy so
        // derived-resource refresh never mutates the restored history state.
        Document preview_document = g.document;
        Mesh mesh;
        std::vector<PickupModel> pickup_models;
        std::vector<StaticObjectModel> object_models;
        std::string preview_why;
        bool preview_loaded = load_document_preview(&preview_document, &mesh, &preview_why,
                                                    &pickup_models, &object_models);
        if (preview_loaded && !g.document.source_pc_path.empty() &&
            !g.document.weapons_donor.empty()) {
            std::vector<PickupTemplate> donor_templates;
            std::vector<PickupModel> donor_models;
            preview_loaded = load_pickup_donor(g.document.weapons_donor, &donor_templates,
                                               &donor_models, &preview_why);
            if (preview_loaded) {
                for (PickupModel& model : donor_models) {
                    bool present = false;
                    for (const PickupModel& existing : pickup_models) {
                        if (existing.skin_id == model.skin_id) {
                            present = true;
                            break;
                        }
                    }
                    if (!present)
                        pickup_models.push_back(std::move(model));
                }
            }
        }
        if (preview_loaded) {
            g.mesh = std::move(mesh);
            g.pickup_models = std::move(pickup_models);
            g.static_object_models = std::move(object_models);
        } else {
            g.mesh = {};
            g.pickup_models.clear();
            g.static_object_models.clear();
            ok = false;
            if (why)
                *why = preview_why;
        }
        frame_mesh();
    } else if (textures_changed) {
        std::string texture_why;
        if (!gpu_reload_environment_textures(&texture_why)) {
            ok = false;
            if (why && why->empty())
                *why = texture_why;
        }
    }

    if (skybox_changed && !reload_skybox_preview(false)) {
        ok = false;
        if (why && why->empty())
            *why = "Skybox preview resources are unavailable for the restored state.";
    }
    return ok;
}

void finish_entity_drag_transaction() {
    if (!g.moving_entity)
        return;
    g.moving_entity = false;
    commit_history_transaction();
    if (GetCapture() == g.window)
        ReleaseCapture();
}

void refresh_after_history_restore(const Document& previous, const char* action) {
    stop_sound_preview();
    g.pending_kind = -1;
    set_single_selection_state(g.selected);
    std::string why;
    const bool resources_ready = refresh_history_derived_resources(previous, &why);
    refresh_list();
    refresh_inspector();
    update_title();
    request_redraw();
    if (resources_ready)
        set_status(action);
    else
        set_status((std::string(action) + " Preview refresh warning: " + why).c_str());
}

void command_undo() {
    finish_entity_drag_transaction();
    Document previous = g.document;
    if (!g.history.undo(&g.document, &g.selected)) {
        set_status("Nothing to undo.");
        return;
    }
    refresh_after_history_restore(previous, "Undo complete.");
}

void command_redo() {
    finish_entity_drag_transaction();
    Document previous = g.document;
    if (!g.history.redo(&g.document, &g.selected)) {
        set_status("Nothing to redo.");
        return;
    }
    refresh_after_history_restore(previous, "Redo complete.");
}

void command_copy_entity() {
    normalize_selection_state();
    std::string why;
    if (!g.history.copy(g.document, g.selected_entities, &why)) {
        set_status(why.c_str());
        return;
    }
    char status[160]{};
    snprintf(status, sizeof(status), "%zu %s copied. Paste creates authored clones with fresh target-valid GUIDs.",
             g.selected_entities.size(), g.selected_entities.size() == 1 ? "entity" : "entities");
    set_status(status);
}

void command_paste_entity() {
    stop_sound_preview();
    std::string why;
    std::vector<int> pasted;
    if (!g.history.paste(&g.document, &g.selected, &pasted, &why)) {
        set_status(why.c_str());
        return;
    }
    g.selected_entities = std::move(pasted);
    g.selected = g.selected_entities.empty() ? -1 : g.selected_entities.back();
    g.pending_kind = -1;
    refresh_list();
    refresh_inspector();
    update_title();
    request_redraw();
    char status[128]{};
    snprintf(status, sizeof(status), "%zu %s pasted as %s authored %s.",
             g.selected_entities.size(), g.selected_entities.size() == 1 ? "entity" : "entities",
             g.selected_entities.size() == 1 ? "a new" : "new",
             g.selected_entities.size() == 1 ? "record" : "records");
    set_status(status);
}

void command_open_project() {
    if (!confirm_discard())
        return;
    std::string path;
    if (!choose_path(g.window, false, "Open editor project", "Asura Level Editor project\0*.alev\0", "alev", &path))
        return;
    Document doc;
    std::string why;
    if (!load_project(&doc, path.c_str(), &why)) {
        MessageBoxA(g.window, why.c_str(), "Could not open project", MB_ICONERROR);
        return;
    }
    stop_sound_preview();
    Mesh mesh;
    std::vector<PickupModel> pickup_models;
    std::vector<StaticObjectModel> object_models;
    if (!load_document_preview(&doc, &mesh, &why, &pickup_models, &object_models)) {
        g.mesh = std::move(mesh);
        g.pickup_models.clear();
        g.static_object_models.clear();
        MessageBoxA(g.window, why.c_str(), "Project source level is unavailable", MB_ICONWARNING);
    } else {
        g.mesh = std::move(mesh);
        g.pickup_models = std::move(pickup_models);
        g.static_object_models = std::move(object_models);
    }
    g.document = std::move(doc);
    const bool skybox_loaded = reload_skybox_preview(true);
    set_single_selection_state(g.document.entities.empty() ? -1 : 0);
    frame_mesh();
    reset_history(!g.document.dirty);
    refresh_list();
    refresh_inspector();
    update_title();
    if (!skybox_loaded)
        set_status("Project loaded; skybox preview is unavailable.");
    else
        set_status(g.document.dirty ? "Project loaded; unsupported legacy entities were removed." : "Project loaded.");
    request_redraw();
}

void command_export() {
    apply_inspector();
    std::string path = g.document.output_path;
    if (path.empty() && !g.document.obj_path.empty()) {
        path = g.document.obj_path;
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos)
            path.resize(dot);
        path += ".PC";
    }
    if (path.empty() && !g.document.source_pc_path.empty())
        path = edited_pc_path(g.document.source_pc_path);
    if (!choose_path(g.window, true, "Export target-game level", "Sniper Elite PC level\0*.PC\0", "PC", &path))
        return;
    if (!g.history.begin(g.document, g.selected))
        return;
    set_status("Packing environment, resources, and entities...");
    UpdateWindow(g.window);
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::string why;
    const bool ok = pack_document(g.document, path.c_str(), &why);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!ok) {
        commit_history_transaction();
        set_status("Export failed.");
        MessageBoxA(g.window, why.c_str(), "Could not export .PC", MB_ICONERROR);
        return;
    }
    g.document.output_path = path;
    commit_history_transaction();
    set_status("Export complete.");
    MessageBoxA(g.window, path.c_str(), "Exported .PC", MB_ICONINFORMATION);
}

void command_export_obj() {
    normalize_selection_state();
    const bool selected_static_object =
        g.selected_entities.size() == 1 && valid_entity_index(g.selected_entities.front()) &&
        g.document.entities[g.selected_entities.front()].kind == EntityKind::StaticObject;
    if (selected_static_object) {
        const Entity& entity = g.document.entities[g.selected_entities.front()];
        const EntityModel* model = entity_render_model(entity);
        if (!model) {
            MessageBoxA(g.window, "The selected static Object has no loaded model geometry.",
                        "Could not export OBJ", MB_ICONERROR);
            return;
        }

        std::string folder_source = !g.document.source_pc_path.empty() ? g.document.source_pc_path
                                                                       : g.document.obj_path;
        const size_t folder_slash = folder_source.find_last_of("\\/");
        const std::string folder = folder_slash == std::string::npos
                                       ? std::string{}
                                       : folder_source.substr(0, folder_slash + 1);

        std::string object_type;
        if (const StaticObjectTemplate* object =
                find_static_object_template(g.document, entity.value_u32_b, true))
            object_type = object->resource_name;
        if (object_type.empty())
            object_type = model->resource_name;
        if (object_type.empty())
            object_type = entity.name;

        const size_t type_slash = object_type.find_last_of("\\/");
        if (type_slash != std::string::npos)
            object_type.erase(0, type_slash + 1);
        const size_t type_dot = object_type.find_last_of('.');
        if (type_dot != std::string::npos)
            object_type.resize(type_dot);
        for (char& c : object_type) {
            const unsigned char byte = static_cast<unsigned char>(c);
            if (byte < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
                c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
                c = '_';
        }
        while (!object_type.empty() && (object_type.back() == ' ' || object_type.back() == '.'))
            object_type.pop_back();
        if (object_type.empty())
            object_type = "object";

        std::string path = folder + object_type + ".obj";
        if (!choose_path(g.window, true, "Export selected static Object as Wavefront OBJ",
                         "Wavefront OBJ\0*.obj\0All files\0*.*\0", "obj", &path))
            return;

        set_status("Exporting selected Object geometry, materials, and embedded textures...");
        UpdateWindow(g.window);
        SetCursor(LoadCursor(nullptr, IDC_WAIT));
        std::string result;
        const bool ok = export_static_object_obj(entity, *model, path.c_str(), &result);
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        if (!ok) {
            set_status("OBJ export failed.");
            MessageBoxA(g.window, result.c_str(), "Could not export OBJ", MB_ICONERROR);
            return;
        }
        set_status(result.c_str());
        MessageBoxA(g.window, result.c_str(), "OBJ export complete", MB_ICONINFORMATION);
        return;
    }

    if (g.document.source_pc_path.empty()) {
        MessageBoxA(g.window, "Open an original .PC level before exporting its geometry.\nOr select a static object to export it's model.",
                    "Could not export OBJ", MB_ICONERROR);
        return;
    }
    std::string path = g.document.source_pc_path;
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("\\/");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        path.resize(dot);
    path += ".obj";
    if (!choose_path(g.window, true, "Export PC environment as Wavefront OBJ",
                     "Wavefront OBJ\0*.obj\0All files\0*.*\0", "obj", &path))
        return;

    set_status("Exporting Env geometry, mat_<index> materials, and embedded textures...");
    UpdateWindow(g.window);
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::string result;
    const bool ok = export_pc_environment_obj(g.document.source_pc_path, path.c_str(), &result);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!ok) {
        set_status("OBJ export failed.");
        MessageBoxA(g.window, result.c_str(), "Could not export OBJ", MB_ICONERROR);
        return;
    }
    set_status(result.c_str());
    MessageBoxA(g.window, result.c_str(), "OBJ export complete", MB_ICONINFORMATION);
}

bool valid_wave_bytes(const std::vector<uint8_t>& bytes) {
    return bytes.size() >= 12 && memcmp(bytes.data(), "RIFF", 4) == 0 &&
           memcmp(bytes.data() + 8, "WAVE", 4) == 0;
}

bool load_preview_wave_file(const std::string& path, std::vector<uint8_t>* bytes, std::string* why) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        if (why)
            *why = "Could not open the selected WAV file.";
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 12 || size > static_cast<std::streamoff>(512 * MiB)) {
        if (why)
            *why = "The selected WAV file has an invalid size.";
        return false;
    }
    std::vector<uint8_t> next(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(next.data()), static_cast<std::streamsize>(size)) ||
        !valid_wave_bytes(next)) {
        if (why)
            *why = "The selected sound is not a raw RIFF/WAVE resource.";
        return false;
    }
    *bytes = std::move(next);
    return true;
}

bool load_embedded_preview_wave(const Entity& entity, std::vector<uint8_t>* bytes, std::string* why) {
    if (g.document.source_pc_path.empty() || !entity.sound_source_record) {
        if (why)
            *why = "This sound has no local WAV or imported embedded resource.";
        return false;
    }
    Arena arena{};
    Error err{};
    ChunkList chunks{};
    bool ok = arena_init(&arena, 8 * MiB, &err) &&
              parse_chunks(g.document.source_pc_path.c_str(), &chunks, &arena, &err);
    if (ok) {
        ok = false;
        for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
            RscfInfo resource{};
            if (!rscf_info(chunks.chunks[chunk_index], &resource) ||
                resource.type != ASURA_RESOURCEFILE_TYPE_SOUND ||
                resource.subtype != entity.sound_phonon.m_uSoundResourceID)
                continue;
            bytes->assign(resource.payload, resource.payload + resource.payload_size);
            ok = valid_wave_bytes(*bytes);
            if (!ok)
                fail(&err, "embedded sound resource %u is not raw RIFF/WAVE data",
                     entity.sound_phonon.m_uSoundResourceID);
            break;
        }
        if (!ok && !err.set)
            fail(&err, "embedded sound resource %u was not found", entity.sound_phonon.m_uSoundResourceID);
    }
    unmap_file(&chunks.file);
    arena_release(&arena);
    if (!ok && why)
        *why = err.set ? err.message : "Could not read the embedded sound resource.";
    return ok;
}

void command_sound_preview() {
    if (g.selected < 0 || g.selected >= static_cast<int>(g.document.entities.size()) ||
        g.document.entities[g.selected].kind != EntityKind::Sound)
        return;
    if (g.sound_preview_entity == g.selected) {
        stop_sound_preview();
        set_status("Sound preview stopped.");
        return;
    }

    const Entity& entity = g.document.entities[g.selected];
    std::vector<uint8_t> bytes;
    std::string why;
    bool loaded = !entity.sound_file.empty() && load_preview_wave_file(entity.sound_file, &bytes, &why);
    bool using_embedded = false;
    if (!loaded && entity.sound_source_record) {
        loaded = load_embedded_preview_wave(entity, &bytes, &why);
        using_embedded = loaded;
    }
    if (!loaded) {
        MessageBoxA(g.window, why.c_str(), "Sound preview unavailable", MB_ICONWARNING);
        return;
    }

    stop_sound_preview();
    g.sound_preview_bytes = std::move(bytes);
    // Preview once even when the in-game repeat flag is set; the same button
    // remains available as an explicit stop control while playback is active.
    const DWORD flags = SND_MEMORY | SND_ASYNC | SND_NODEFAULT;
    if (!PlaySoundA(reinterpret_cast<LPCSTR>(g.sound_preview_bytes.data()), nullptr, flags)) {
        g.sound_preview_bytes.clear();
        MessageBoxA(g.window, "Windows could not play this WAV resource.", "Sound preview unavailable",
                    MB_ICONWARNING);
        return;
    }
    g.sound_preview_entity = g.selected;
    SetWindowTextA(g.sound_preview, "Stop preview");
    set_status(using_embedded ? "Playing embedded sound resource." : "Playing local WAV resource.");
}

void command_browse_sound() {
    if (g.selected < 0 || g.selected >= static_cast<int>(g.document.entities.size()))
        return;
    Entity& e = g.document.entities[g.selected];
    if (e.kind != EntityKind::Sound)
        return;
    stop_sound_preview();
    std::string path = e.sound_file;
    if (!choose_path(g.window, false, "Choose sound resource", "Wave audio\0*.wav\0All files\0*.*\0", "wav", &path))
        return;
    if (!g.history.begin(g.document, g.selected))
        return;
    e.sound_file = path;
    e.sound_name = "sounds\\" + basename_without_extension(path);
    e.name = basename_without_extension(path);
    commit_history_transaction();
    refresh_list();
    refresh_inspector();
    set_status(e.sound_name.c_str());
}

void command_export_material_map() {
    if (g.document.source_pc_path.empty()) {
        MessageBoxA(g.window, "You can't export material map out of a custom level", "Cannot export material map", MB_ICONERROR);
        return;
    }
    std::string path;
    if (!choose_path(g.window, true, "Save material map as", "Material map JSON\0*.json\0All files\0*.*\0", "json", &path))
        return;

    Error err{};
    Arena arena{};
    ChunkList chunks{};
    std::vector<PcEnvironmentMaterialBinding> materials;
    std::vector<uint32_t> collision_flags;
    bool ok = arena_init(&arena, 8 * MiB, &err) &&
              parse_chunks(g.document.source_pc_path.c_str(), &chunks, &arena, &err) &&
              pc_environment_material_bindings(chunks, &materials, &err) &&
              pc_environment_collision_flags(chunks, static_cast<uint32_t>(materials.size()),
                                             &collision_flags, &err);

    std::string out = "{\n";
    if (ok && !materials.empty()) {
        out += "  \"texture_by_material_index\": {\n";
        for (size_t i = 0; i < materials.size(); ++i) {
            std::string tname(materials[i].texture_name.data, materials[i].texture_name.size);
            std::string escaped;
            for (char c : tname) {
                if (c == '\\' || c == '"') escaped += '\\';
                escaped += c;
            }
            char line[1024];
            snprintf(line, sizeof(line), "    \"%zu\": \"%s\"%s\n", i, escaped.c_str(), i + 1 == materials.size() ? "" : ",");
            out += line;
        }
        out += "  },\n";

        out += "  \"transparency_flag_by_material_index\": {\n";
        for (size_t i = 0; i < materials.size(); ++i) {
            char line[256];
            snprintf(line, sizeof(line), "    \"%zu\": %u%s\n", i, materials[i].flags, i + 1 == materials.size() ? "" : ",");
            out += line;
        }
        out += "  },\n";

        out += "  \"surface_type_by_material_index\": {\n";
        for (size_t i = 0; i < materials.size(); ++i) {
            char line[256];
            snprintf(line, sizeof(line), "    \"%zu\": %u%s\n", i, materials[i].surface_type, i + 1 == materials.size() ? "" : ",");
            out += line;
        }
        out += "  },\n";

        out += "  \"collision_flags\": {\n";
        for (size_t i = 0; i < materials.size(); ++i) {
            char line[256];
            snprintf(line, sizeof(line), "    \"%zu\": %u%s\n", i, collision_flags[i],
                     i + 1 == materials.size() ? "" : ",");
            out += line;
        }
        out += "  }\n";
    }
    unmap_file(&chunks.file);
    arena_release(&arena);

    if (!ok) {
        MessageBoxA(g.window, err.set ? err.message : "Failed to export material map.",
                    "Error", MB_ICONERROR);
        return;
    }

    out += "}\n";

    if (!write_entire_file(path.c_str(), out.c_str(), out.size(), nullptr)) {
        MessageBoxA(g.window, "Failed to write file.", "Error", MB_ICONERROR);
        return;
    }

    if (!g.history.begin(g.document, g.selected))
        return;
    g.document.material_map = path;
    std::string why;
    if (!g.document.obj_path.empty()) {
        Mesh rebuilt;
        if (load_preview_mesh(g.document.obj_path, path, &rebuilt, &why)) {
            g.mesh = std::move(rebuilt);
            g.environment_raycast.build(g.mesh);
            invalidate_environment_cache();
            gpu_upload_mesh();
        }
    } else {
        gpu_reload_environment_textures(&why);
    }
    commit_history_transaction();
    request_redraw();
    set_status(why.empty() ? "Material map exported and applied." : why.c_str());
}

void command_material_map() {
    std::string path = g.document.material_map;
    if (!choose_path(g.window, false, "Choose material map", "Material map JSON\0*.json\0All files\0*.*\0", "json", &path))
        return;
    Mesh rebuilt;
    std::string why;
    if (!g.document.obj_path.empty() && !load_preview_mesh(g.document.obj_path, path, &rebuilt, &why)) {
        MessageBoxA(g.window, why.c_str(), "Could not apply material map", MB_ICONERROR);
        return;
    }
    if (!g.history.begin(g.document, g.selected))
        return;
    g.document.material_map = path;
    if (!g.document.obj_path.empty()) {
        g.mesh = std::move(rebuilt);
        g.environment_raycast.build(g.mesh);
        invalidate_environment_cache();
        gpu_upload_mesh();
    } else {
        gpu_reload_environment_textures(&why);
    }
    commit_history_transaction();
    request_redraw();
    set_status(why.empty() ? "Material map applied to the environment preview." : why.c_str());
}

void command_texture_dir() {
    std::string path = g.document.texture_dir.empty() ? folder_from_path(g.document.obj_path) : g.document.texture_dir;
    if (!choose_directory(g.window, "Choose texture folder", &path))
        return;
    if (!g.history.begin(g.document, g.selected))
        return;
    g.document.texture_dir = path;
    uint32_t loaded = 0, missing = 0;
    std::string why;
    gpu_reload_environment_textures(&why, &loaded, &missing);
    commit_history_transaction();
    request_redraw();
    char status[256]{};
    snprintf(status, sizeof(status), "Environment textures: %u loaded, %u using diffuse fallback.%s%s",
             loaded, missing, why.empty() ? "" : " ", why.c_str());
    set_status(status);
}

void command_toggle_rain() {
    if (g.document.source_pc_path.empty() && g.document.obj_path.empty())
        return;
    const bool enabled = SendMessageA(g.rain_toggle, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (enabled == g.document.rain_enabled)
        return;
    if (!g.history.begin(g.document, g.selected)) {
        SendMessageA(g.rain_toggle, BM_SETCHECK,
                     g.document.rain_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        return;
    }
    g.document.rain_enabled = enabled;
    std::string why;
    const bool preview_ready = gpu_reload_environment_textures(&why);
    if (g.document.source_pc_path.empty())
        gpu_set_environment_wet_weather(enabled);
    commit_history_transaction();
    invalidate_environment_cache();
    request_redraw();
    if (!preview_ready || !why.empty())
        set_status((std::string(enabled ? "Rain enabled. " : "Rain disabled. ") + why).c_str());
    else
        set_status(enabled ? "WTHR rain enabled." : "WTHR rain disabled.");
}

void command_toggle_backface_culling() {
    const bool enabled =
        SendMessageA(g.backface_cull_toggle, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (enabled == g.backface_culling)
        return;
    g.backface_culling = enabled;
    invalidate_environment_cache();
    request_redraw();
    set_status(enabled ? "Backface culling enabled." : "Backface culling disabled.");
}

enum AmbiencePropertiesId : int {
    ID_AMBIENCE_STREAM = 3400,
    ID_AMBIENCE_VOLUME,
};

struct AmbiencePropertiesState {
    HWND window = nullptr;
    HWND stream = nullptr;
    HWND volume = nullptr;
    std::vector<std::string> paths;
    std::string selected_path;
    float selected_volume = 1.0f;
    bool accepted = false;
};

constexpr const char* kAmbienceSoundNames[] = {
    "01_temp.wav",
    "02_temp.wav",
    "indoor_s\\m1_kar1i.wav",
    "indoor_s\\m1_roof.wav",
    "indoor_s\\m2_bra1i.wav",
    "indoor_s\\m2_bra1r.wav",
    "indoor_s\\m3_pla5i.wav",
    "indoor_s\\m4_anh3i.wav",
    "indoor_s\\m4_entr.wav",
    "indoor_s\\m4_roof.wav",
    "indoor_s\\m4_tube.wav",
    "indoor_s\\m5_bor3i.wav",
    "indoor_s\\m5_bor4i.wav",
    "indoor_s\\m5_roof.wav",
    "indoor_s\\m5_room.wav",
    "indoor_s\\m7_kei1i.wav",
    "indoor_s\\m8_air1i.wav",
    "indoor_s\\m8_contr.wav",
    "m1_karl1.wav",
    "m1_karl2.wav",
    "m10_Hofe.wav",
    "m11_bunk.wav",
    "m2_bran1.wav",
    "m2_bran2.wav",
    "m2_bran3.wav",
    "m2_bran4.wav",
    "m2_bran5.wav",
    "m3_play1.wav",
    "m3_play2.wav",
    "m3_play3.wav",
    "m3_play4.wav",
    "m3_play5.wav",
    "m4_anh1.wav",
    "m4_anh2.wav",
    "m4_anh3.wav",
    "m4_anh3o.wav",
    "m5_bor1.wav",
    "m5_bor2.wav",
    "m5_bor3o.wav",
    "m5_bor4o.wav",
    "m5_bor5.wav",
    "m5_bor6.wav",
    "m6_schl1.wav",
    "m6_schl2.wav",
    "m6_schl3.wav",
    "m6_schl4.wav",
    "m6_schl5.wav",
    "m7_kei1o.wav",
    "m7_kei2o.wav",
    "m7_kei3o.wav",
    "m8_air1o.wav",
    "m8_air2o.wav",
    "m8_air3o.wav",
    "m8_air4o.wav"
};

std::vector<std::string> available_ambience_streams() {
    std::vector<std::string> paths;
    paths.reserve(sizeof(kAmbienceSoundNames) / sizeof(kAmbienceSoundNames[0]));
    for (const char* name : kAmbienceSoundNames)
        paths.emplace_back(std::string("Sounds\\Streams\\") + name);
    return paths;
}

std::string ambience_stream_label(const std::string& path) {
    constexpr const char prefix[] = "Sounds\\Streams\\";
    size_t start = 0;
    while (start < path.size() && (path[start] == '\\' || path[start] == '/'))
        ++start;
    if (path.size() - start >= sizeof(prefix) - 1 &&
        _strnicmp(path.c_str() + start, prefix, sizeof(prefix) - 1) == 0)
        start += sizeof(prefix) - 1;
    size_t end = path.size();
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && dot >= start)
        end = dot;
    std::string label = path.substr(start, end - start);
    for (char& c : label)
        if (c == '/')
            c = '\\';
    return label;
}

void create_ambience_properties_controls(AmbiencePropertiesState* state) {
    make_dialog_control(state->window, "STATIC", "Default stream", SS_LEFT, -1, 18, 22, 104, 22);
    state->stream = make_dialog_control(state->window, "COMBOBOX", "",
                                          CBS_DROPDOWNLIST | CBS_AUTOHSCROLL | WS_VSCROLL,
                                          ID_AMBIENCE_STREAM, 126, 18, 380, 260);
    make_dialog_control(state->window, "STATIC", "Volume (0-1)", SS_LEFT, -1, 18, 62, 104, 22);
    state->volume = make_dialog_control(state->window, "EDIT", "",
                                          ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                          ID_AMBIENCE_VOLUME, 126, 58, 100, 24);
    make_dialog_control(
        state->window, "STATIC",
        "Ambience sound names come from game's root Sounds\\Streams",
        SS_LEFT, -1, 18, 98, 355, 17);
    make_dialog_control(state->window, "BUTTON", "OK", BS_DEFPUSHBUTTON | WS_TABSTOP,
                          IDOK, 290, 158, 104, 30);
    make_dialog_control(state->window, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP,
                          IDCANCEL, 402, 158, 104, 30);

    SendMessageA(state->stream, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>("(None - No ambience sound)"));
    int selected = state->selected_path.empty() ? 0 : -1;
    for (size_t index = 0; index < state->paths.size(); ++index) {
        const std::string label = ambience_stream_label(state->paths[index]);
        const LRESULT row = SendMessageA(state->stream, CB_ADDSTRING, 0,
                                         reinterpret_cast<LPARAM>(label.c_str()));
        if (row != CB_ERR && row != CB_ERRSPACE &&
            _stricmp(state->paths[index].c_str(), state->selected_path.c_str()) == 0)
            selected = static_cast<int>(row);
    }
    SendMessageA(state->stream, CB_SETCURSEL, selected >= 0 ? selected : 0, 0);
    set_float(state->volume, state->selected_volume);
}

LRESULT CALLBACK ambience_properties_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTA* create = reinterpret_cast<const CREATESTRUCTA*>(lparam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    AmbiencePropertiesState* state =
        reinterpret_cast<AmbiencePropertiesState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE:
        state->window = hwnd;
        create_ambience_properties_controls(state);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            const int selection = static_cast<int>(SendMessageA(state->stream, CB_GETCURSEL, 0, 0));
            const float volume = get_float(state->volume, state->selected_volume);
            if (selection < 0 || selection > static_cast<int>(state->paths.size()) ||
                !isfinite(volume) || volume < 0.0f || volume > 1.0f) {
                MessageBoxA(hwnd, "Choose a stream and enter a volume from 0 to 1.",
                            "Invalid ambience settings", MB_ICONWARNING);
                return 0;
            }
            state->selected_path = selection == 0 ? std::string{} : state->paths[selection - 1];
            state->selected_volume = volume;
            state->accepted = true;
            DestroyWindow(hwnd);
        } else if (LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

void command_ambience_properties() {
    if (g.document.source_pc_path.empty() && g.document.obj_path.empty())
        return;
    AmbiencePropertiesState state{};
    state.selected_path = g.document.ambient_stream_path;
    state.selected_volume = g.document.ambient_volume;
    state.paths = available_ambience_streams();
    bool current_present = state.selected_path.empty();
    for (const std::string& path : state.paths)
        current_present |= _stricmp(path.c_str(), state.selected_path.c_str()) == 0;
    if (!current_present)
        state.paths.push_back(state.selected_path);

    if (!run_centered_modal("Asura2005AmbienceProperties", "Streaming ambience", 540, 235, &state))
        return;
    if (!state.accepted)
        return;
    if (!g.history.begin(g.document, g.selected))
        return;
    g.document.ambient_stream_path = std::move(state.selected_path);
    g.document.ambient_volume = state.selected_volume;
    commit_history_transaction();
    if (g.document.ambient_stream_path.empty()) {
        set_status("Default streaming ambience disabled");
    } else {
        const std::string label = ambience_stream_label(g.document.ambient_stream_path);
        char status[320]{};
        snprintf(status, sizeof(status), "Streaming ambience: %s at %.3g",
                 label.c_str(), g.document.ambient_volume);
        set_status(status);
    }
}

void command_weapons_donor() {
    std::string path = g.document.weapons_donor;
    if (!choose_path(g.window, false, "Choose a target-game weapons donor .PC",
                     "Asura PC files\0*.PC\0All files\0*.*\0", "PC", &path))
        return;
    set_status("Reading 0x0008 pickup definitions and models from the weapons donor...");
    UpdateWindow(g.window);
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::vector<PickupTemplate> templates;
    std::vector<PickupModel> models;
    std::string why;
    const bool ok = load_pickup_donor(path, &templates, &models, &why);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!ok) {
        set_status("Weapons donor does not contain a usable pickup catalog.");
        MessageBoxA(g.window, why.c_str(), "Could not load Weapons donor", MB_ICONERROR);
        return;
    }
    for (const Entity& entity : g.document.entities) {
        if (entity.kind != EntityKind::Pickup || entity.source_entity_record)
            continue;
        bool supported = false;
        for (const PickupTemplate& pickup : templates)
            supported |= pickup.item_id == entity.value_u32_a;
        if (!supported) {
            char message[220]{};
            snprintf(message, sizeof(message),
                     "The selected donor has no 0x0008 definition for existing item 0x%02X.", entity.value_u32_a);
            set_status("Weapons donor is missing an item used by this level.");
            MessageBoxA(g.window, message, "Could not switch Weapons donor", MB_ICONERROR);
            return;
        }
    }
    if (!g.history.begin(g.document, g.selected))
        return;
    g.document.weapons_donor = path;
    if (g.document.source_pc_path.empty()) {
        g.document.pickup_templates = templates;
        for (Entity& entity : g.document.entities) {
            if (entity.kind != EntityKind::Pickup || entity.source_entity_record)
                continue;
            for (const PickupTemplate& pickup : templates)
                if (pickup.item_id == entity.value_u32_a) {
                    adopt_pickup_template(&entity, pickup);
                    break;
                }
        }
        g.pickup_models = std::move(models);
    } else {
        merge_pickup_templates(&g.document, templates);
        for (PickupModel& model : models) {
            bool present = false;
            for (const PickupModel& existing : g.pickup_models)
                present |= existing.skin_id == model.skin_id;
            if (!present)
                g.pickup_models.push_back(std::move(model));
        }
    }
    commit_history_transaction();
    char status[220]{};
    snprintf(status, sizeof(status), "Weapons donor loaded: %zu pickup item definitions, %zu rendered models.",
             templates.size(), g.pickup_models.size());
    set_status(status);
    refresh_list();
    refresh_inspector();
    request_redraw();
}

void merge_static_object_templates(Document* document,
                                   const std::vector<StaticObjectTemplate>& incoming) {
    for (const StaticObjectTemplate& object : incoming) {
        bool present = false;
        for (const StaticObjectTemplate& existing : document->static_object_templates)
            present |= existing.file_id == object.file_id;
        if (!present)
            document->static_object_templates.push_back(object);
    }
}

void command_object_donor() {
    std::vector<std::string> paths;
    if (!choose_paths(g.window, "Choose one or more target-game Objects donor .PC levels",
                      "Asura PC files\0*.PC\0All files\0*.*\0", "PC",
                      g.document.object_donors, &paths))
        return;
    set_status("Reading class-0x7 Object definitions and models from selected donors...");
    UpdateWindow(g.window);
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::vector<StaticObjectTemplate> templates;
    std::vector<StaticObjectModel> models;
    std::string why;
    const bool ok = load_static_object_donors(paths, &templates, &models, &why);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!ok) {
        set_status("The selected Objects donors do not contain a usable Object catalog.");
        MessageBoxA(g.window, why.c_str(), "Could not load Objects donors", MB_ICONERROR);
        return;
    }
    for (const Entity& entity : g.document.entities) {
        if (entity.kind != EntityKind::StaticObject)
            continue;
        bool requires_donor = !entity.source_entity_record;
        if (entity.source_entity_record) {
            for (const StaticObjectTemplate& current : g.document.static_object_templates)
                if (current.file_id == entity.value_u32_b &&
                    current.donor_path != g.document.source_pc_path) {
                    requires_donor = true;
                    break;
                }
        }
        if (!requires_donor)
            continue;
        bool supported = false;
        for (const StaticObjectTemplate& object : templates)
            supported |= object.file_id == entity.value_u32_b;
        if (!supported) {
            char message[220]{};
            snprintf(message, sizeof(message),
                     "The selected donors have no Object definition for existing file ID %08X.",
                     entity.value_u32_b);
            set_status("Objects donors are missing an Object used by this level.");
            MessageBoxA(g.window, message, "Could not switch Objects donors", MB_ICONERROR);
            return;
        }
    }
    if (!g.history.begin(g.document, g.selected))
        return;
    g.document.object_donors = paths;
    if (g.document.source_pc_path.empty()) {
        g.document.static_object_templates = templates;
        for (Entity& entity : g.document.entities) {
            if (entity.kind != EntityKind::StaticObject || entity.source_entity_record)
                continue;
            for (const StaticObjectTemplate& object : templates)
                if (object.file_id == entity.value_u32_b) {
                    adopt_static_object_template(&entity, object);
                    break;
                }
        }
        g.static_object_models = std::move(models);
    } else {
        std::vector<StaticObjectTemplate> source_templates;
        for (const StaticObjectTemplate& object : g.document.static_object_templates)
            if (object.donor_path == g.document.source_pc_path)
                source_templates.push_back(object);
        g.document.static_object_templates = std::move(source_templates);
        merge_static_object_templates(&g.document, templates);
        for (StaticObjectModel& model : models) {
            bool present = false;
            for (const StaticObjectModel& existing : g.static_object_models)
                present |= existing.file_id == model.file_id;
            if (!present)
                g.static_object_models.push_back(std::move(model));
        }
    }
    commit_history_transaction();
    char status[260]{};
    snprintf(status, sizeof(status),
             "%zu Objects donor levels loaded: %zu definitions, %zu rendered models.",
             paths.size(), templates.size(), g.static_object_models.size());
    set_status(status);
    refresh_list();
    refresh_inspector();
    request_redraw();
}

enum SkyboxPropertiesId : int {
    ID_SKYBOX_RED = 3300,
    ID_SKYBOX_GREEN,
    ID_SKYBOX_BLUE,
    ID_SKYBOX_ORIENTATION,
    ID_SKYBOX_PATH_FIRST,
    ID_SKYBOX_PATH_LAST = ID_SKYBOX_PATH_FIRST + ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT - 1,
    ID_SKYBOX_BACK_FLIPPED,
    ID_SKYBOX_RIGHT_FLIPPED,
    ID_SKYBOX_PREVIEW_FOLDER,
    ID_SKYBOX_USE_EMBEDDED,
};

struct SkyboxPropertiesState {
    HWND window = nullptr;
    HWND red = nullptr;
    HWND green = nullptr;
    HWND blue = nullptr;
    HWND orientation = nullptr;
    HWND paths[ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT]{};
    HWND back_flipped = nullptr;
    HWND right_flipped = nullptr;
    HWND preview_folder = nullptr;
    SkyboxSettings value;
    std::string texture_directory;
    bool accepted = false;
};

void refresh_skybox_folder_text(SkyboxPropertiesState* state) {
    SetWindowTextA(state->preview_folder,
                   state->texture_directory.empty() ? "Embedded/source resources" : state->texture_directory.c_str());
}

void refresh_skybox_path_text(SkyboxPropertiesState* state) {
    for (uint32_t slot = 0; slot < ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT; ++slot)
        SetWindowTextA(state->paths[slot], state->value.texture_paths[slot].c_str());
}

void create_skybox_properties_controls(SkyboxPropertiesState* state) {
    make_dialog_control(state->window, "STATIC", "RGB tint", SS_LEFT, 0, 14, 19, 82, 22);
    constexpr const char* colour_labels[] = {"R", "G", "B"};
    HWND* colours[] = {&state->red, &state->green, &state->blue};
    constexpr int colour_ids[] = {ID_SKYBOX_RED, ID_SKYBOX_GREEN, ID_SKYBOX_BLUE};
    for (int i = 0; i < 3; i++) {
        const int x = 100 + i * 150;
        make_dialog_control(state->window, "STATIC", colour_labels[i], SS_LEFT, 0, x, 19, 18, 22);
        *colours[i] = make_dialog_control(state->window, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                          colour_ids[i], x + 20, 16, 118, 24);
    }
    make_dialog_control(state->window, "STATIC", "Orientation (radians)", SS_LEFT, 0, 556, 19, 126, 22);
    state->orientation = make_dialog_control(state->window, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                             ID_SKYBOX_ORIENTATION, 682, 16, 112, 24);

    constexpr const char* path_labels[] = {"Path 0 (lower)", "Path 1 (front)", "Path 2 (left)",
                                            "Path 3 (back)", "Path 4 (right)", "Path 5 (upper)",
                                            "Path 6 (cloud A)", "Path 7 (cloud B)"};
    for (int i = 0; i < static_cast<int>(_countof(path_labels)); i++) {
        const int y = 58 + i * 36;
        make_dialog_control(state->window, "STATIC", path_labels[i], SS_LEFT, 0, 14, y + 3, 112, 22);
        state->paths[i] = make_dialog_control(state->window, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                              ID_SKYBOX_PATH_FIRST + i, 130, y, 664, 24);
        SendMessageA(state->paths[i], EM_SETLIMITTEXT, 4096, 0);
    }

    make_dialog_control(state->window, "STATIC", "Clearing cloud paths disables clouds animation.", SS_LEFT,
                        0, 14, 340, 332, 22);
    constexpr const char* option_text[] = {
        "Back texture is front upside down", "Right texture is left upside down"};
    constexpr DWORD option_style[] = {
        BS_AUTOCHECKBOX | WS_TABSTOP, BS_AUTOCHECKBOX | WS_TABSTOP};
    constexpr int option_ids[] = {ID_SKYBOX_BACK_FLIPPED, ID_SKYBOX_RIGHT_FLIPPED};
    constexpr int option_x[] = {366, 14};
    constexpr int option_width[] = {332, 346};
    HWND* option_outputs[] = {&state->back_flipped, &state->right_flipped};
    for (size_t i = 0; i < _countof(option_ids); i++)
        *option_outputs[i] = make_dialog_control(state->window, "BUTTON", option_text[i], option_style[i],
                                                 option_ids[i], option_x[i], 386, option_width[i], 24);
    make_dialog_control(state->window, "STATIC", "Preview resources", SS_LEFT, 0, 14, 416, 112, 22);
    state->preview_folder = make_dialog_control(state->window, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | ES_READONLY,
                                                0, 130, 413, 414, 24);
    constexpr const char* action_text[] = {"Choose folder...", "Use embedded", "Apply", "Cancel"};
    constexpr DWORD action_style[] = {BS_PUSHBUTTON | WS_TABSTOP, BS_PUSHBUTTON | WS_TABSTOP,
                                      BS_DEFPUSHBUTTON | WS_TABSTOP, BS_PUSHBUTTON | WS_TABSTOP};
    constexpr int action_ids[] = {ID_SKYBOX_PREVIEW_FOLDER, ID_SKYBOX_USE_EMBEDDED, IDOK, IDCANCEL};
    constexpr int action_x[] = {552, 676, 566, 682};
    constexpr int action_y[] = {412, 412, 451, 451};
    constexpr int action_width[] = {116, 118, 104, 112};
    constexpr int action_height[] = {27, 27, 30, 30};
    for (size_t i = 0; i < _countof(action_ids); i++)
        make_dialog_control(state->window, "BUTTON", action_text[i], action_style[i], action_ids[i],
                            action_x[i], action_y[i], action_width[i], action_height[i]);

    set_float(state->red, state->value.red);
    set_float(state->green, state->value.green);
    set_float(state->blue, state->value.blue);
    set_float(state->orientation, state->value.orientation_radians);
    refresh_skybox_path_text(state);
    SendMessageA(state->back_flipped, BM_SETCHECK,
                 state->value.back_texture_is_front_upside_down ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(state->right_flipped, BM_SETCHECK,
                 state->value.right_texture_is_left_upside_down ? BST_CHECKED : BST_UNCHECKED, 0);
    refresh_skybox_folder_text(state);
}

void apply_skybox_properties(SkyboxPropertiesState* state) {
    state->value.red = get_float(state->red, state->value.red);
    state->value.green = get_float(state->green, state->value.green);
    state->value.blue = get_float(state->blue, state->value.blue);
    state->value.orientation_radians = get_float(state->orientation, state->value.orientation_radians);
    for (int i = 0; i < static_cast<int>(_countof(state->paths)); i++) {
        char path[4097]{};
        GetWindowTextA(state->paths[i], path, sizeof(path));
        state->value.texture_paths[i] = path;
    }
    state->value.back_texture_is_front_upside_down =
        SendMessageA(state->back_flipped, BM_GETCHECK, 0, 0) == BST_CHECKED;
    state->value.right_texture_is_left_upside_down =
        SendMessageA(state->right_flipped, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

LRESULT CALLBACK skybox_properties_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTA* create = reinterpret_cast<const CREATESTRUCTA*>(lparam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    SkyboxPropertiesState* state =
        reinterpret_cast<SkyboxPropertiesState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE:
        state->window = hwnd;
        create_skybox_properties_controls(state);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_SKYBOX_PREVIEW_FOLDER) {
            std::string path = state->texture_directory;
            if (choose_directory(hwnd, "Choose skybox texture folder", &path)) {
                SkyboxTextureScan scan{};
                std::string why;
                if (!scan_skybox_texture_folder(path, &scan, &why)) {
                    MessageBoxA(hwnd, why.c_str(), "Could not scan skybox textures", MB_ICONWARNING);
                } else {
                    state->texture_directory = std::move(path);
                    state->value.texture_paths = std::move(scan.texture_paths);
                    refresh_skybox_folder_text(state);
                    refresh_skybox_path_text(state);
                }
            }
        } else if (LOWORD(wparam) == ID_SKYBOX_USE_EMBEDDED) {
            state->texture_directory.clear();
            refresh_skybox_folder_text(state);
        } else if (LOWORD(wparam) == IDOK) {
            apply_skybox_properties(state);
            state->accepted = true;
            DestroyWindow(hwnd);
        } else if (LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

void command_skybox_textures() {
    SkyboxPropertiesState state{};
    state.value = g.document.skybox;
    state.texture_directory = g.document.sky_texture_dir;
    if (!run_centered_modal("Asura2005SkyboxProperties", "Skybox properties", 830, 535, &state))
        return;
    if (!state.accepted)
        return;

    if (!g.history.begin(g.document, g.selected))
        return;
    g.document.skybox = std::move(state.value);
    g.document.sky_texture_dir = std::move(state.texture_directory);
    commit_history_transaction();
    const bool loaded = reload_skybox_preview(true);
    if (loaded) {
        gpu_rebuild_skybox_vertices(g.document.skybox.orientation_radians,
                                    g.document.skybox.back_texture_is_front_upside_down,
                                    g.document.skybox.right_texture_is_left_upside_down, nullptr);
        gpu_set_skybox_tint(g.document.skybox);
        request_redraw();
    }
    set_status(loaded ? "Skybox properties applied."
                      : "Skybox properties saved; preview unavailable.");
}

void delete_selected() {
    normalize_selection_state();
    if (g.selected_entities.empty())
        return;
    stop_sound_preview();
    if (!g.history.begin(g.document, g.selected))
        return;
    std::vector<int> targets = g.selected_entities;
    std::sort(targets.begin(), targets.end());
    const int next_index = targets.front();
    for (auto it = targets.rbegin(); it != targets.rend(); ++it)
        g.document.entities.erase(g.document.entities.begin() + *it);
    set_single_selection_state(std::min(next_index, static_cast<int>(g.document.entities.size()) - 1));
    commit_history_transaction();
    refresh_list();
    refresh_inspector();
    request_redraw();
    if (targets.size() > 1) {
        char status[96]{};
        snprintf(status, sizeof(status), "Deleted %zu selected entities.", targets.size());
        set_status(status);
    }
}

void paint_window() {
    PAINTSTRUCT ps{};
    HDC window_dc = BeginPaint(g.window, &ps);
    RECT client{};
    GetClientRect(g.window, &client);
    if (gpu_ready() && g.viewport) {
        HBRUSH panel = CreateSolidBrush(RGB(238, 241, 244));
        FillRect(window_dc, &client, panel);
        DeleteObject(panel);
        EndPaint(g.window, &ps);
        return;
    }
    const int width = std::max(1L, client.right), height = std::max(1L, client.bottom);
    if (!g.environment_cache || g.environment_cache_width != width || g.environment_cache_height != height) {
        release_environment_cache();
        g.environment_cache = CreateCompatibleBitmap(window_dc, width, height);
        g.environment_cache_width = width;
        g.environment_cache_height = height;
    }
    if (!g.environment_cache_valid || g.environment_cache_fast != g.fast_preview) {
        HDC cache_dc = CreateCompatibleDC(window_dc);
        HGDIOBJ old_cache_bitmap = SelectObject(cache_dc, g.environment_cache);
        HBRUSH panel = CreateSolidBrush(RGB(238, 241, 244));
        FillRect(cache_dc, &client, panel);
        DeleteObject(panel);
        SelectObject(cache_dc, g.font);
        draw_environment(cache_dc);
        SelectObject(cache_dc, old_cache_bitmap);
        DeleteDC(cache_dc);
        g.environment_cache_valid = true;
        g.environment_cache_fast = g.fast_preview;
    }
    HDC dc = CreateCompatibleDC(window_dc);
    HBITMAP bitmap = CreateCompatibleBitmap(window_dc, width, height);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    HDC cache_dc = CreateCompatibleDC(window_dc);
    HGDIOBJ old_cache_bitmap = SelectObject(cache_dc, g.environment_cache);
    BitBlt(dc, 0, 0, width, height, cache_dc, 0, 0, SRCCOPY);
    SelectObject(cache_dc, old_cache_bitmap);
    DeleteDC(cache_dc);
    SelectObject(dc, g.font);
    draw_entities(dc);
    BitBlt(window_dc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(g.window, &ps);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_GETMINMAXINFO: {
        MINMAXINFO* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        const UINT dpi = std::max<UINT>(96, GetDpiForWindow(hwnd));
        limits->ptMinTrackSize.x = MulDiv(1180, static_cast<int>(dpi), 96);
        limits->ptMinTrackSize.y = MulDiv(650, static_cast<int>(dpi), 96);
        return 0;
    }
    case WM_CREATE:
        g.window = hwnd;
        g.dpi = std::max<UINT>(96, GetDpiForWindow(hwnd));
        rebuild_ui_font();
        create_controls();
        reset_history(true);
        if (!g.spawn_puppets_loaded)
            set_status("MPChars.asr was not found or is incompatible; spawnpoints use fallback markers.");
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    case WM_DPICHANGED: {
        g.dpi = std::max<UINT>(96, LOWORD(wparam));
        rebuild_ui_font();
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        layout_controls();
        release_environment_cache();
        request_redraw();
        return 0;
    }
    case WM_SIZE:
        layout_controls();
        release_environment_cache();
        g.fast_preview = true;
        SetTimer(hwnd, 1, 140, nullptr);
        request_redraw();
        return 0;
    case WM_TIMER:
        if (wparam == 1 && !g.orbiting && !g.panning) {
            KillTimer(hwnd, 1);
            g.fast_preview = false;
            invalidate_environment_cache();
            request_redraw();
        } else if (wparam == 2 &&
                   ((g.document.skybox.draw_clouds && gpu_has_skybox_cloud()) ||
                    (g.document.rain_enabled && gpu_has_rain_texture()) || gpu_has_animated_models())) {
            request_redraw();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_window();
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        record_inspector_edit(id, HIWORD(wparam));
        if (id == ID_OPEN_OBJ)
            command_open_obj();
        else if (id == ID_OPEN_PC)
            command_open_pc();
        else if (id == ID_OPEN_PROJECT)
            command_open_project();
        else if (id == ID_SAVE_PROJECT)
            command_save_project();
        else if (id == ID_EXPORT_PC)
            command_export();
        else if (id == ID_EXPORT_OBJ)
            command_export_obj();
        else if (id == ID_MATERIAL_MAP)
            command_material_map();
        else if (id == ID_EXPORT_MATERIAL_MAP)
            command_export_material_map();
        else if (id == ID_TEXTURE_DIR)
            command_texture_dir();
        else if (id == ID_WEAPONS_DONOR)
            command_weapons_donor();
        else if (id == ID_OBJECT_DONOR)
            command_object_donor();
        else if (id == ID_SKYBOX_TEXTURES)
            command_skybox_textures();
        else if (id == ID_TOGGLE_RAIN)
            command_toggle_rain();
        else if (id == ID_TOGGLE_BACKFACE_CULLING)
            command_toggle_backface_culling();
        else if (id == ID_AMBIENCE_PROPERTIES)
            command_ambience_properties();
        else if (id == ID_ADD_SPAWN)
            begin_place(EntityKind::SpawnPoint);
        else if (id == ID_ADD_LIGHT)
            begin_place(EntityKind::Light);
        else if (id == ID_ADD_SOUND)
            begin_place(EntityKind::Sound);
        else if (id == ID_ADD_PICKUP)
            begin_place(EntityKind::Pickup);
        else if (id == ID_ADD_STATIC_OBJECT)
            begin_place(EntityKind::StaticObject);
        else if (id == ID_ADD_BUILDING_VOLUME)
            begin_place(EntityKind::BuildingVolume);
        else if (id == ID_UNDO)
            command_undo();
        else if (id == ID_REDO)
            command_redo();
        else if (id == ID_COPY_ENTITY)
            command_copy_entity();
        else if (id == ID_PASTE_ENTITY)
            command_paste_entity();
        else if (id == ID_DELETE_ENTITY)
            delete_selected();
        else if (id == ID_APPLY_INSPECTOR)
            apply_inspector();
        else if (id == ID_BROWSE_SOUND)
            command_browse_sound();
        else if (id == ID_SOUND_PREVIEW)
            command_sound_preview();
        else if (id == ID_SOUND_PROPERTIES)
            command_sound_properties();
        else if (id == ID_LIGHT_PROPERTIES)
            command_light_properties();
        else if (id == ID_ENTITY_LIST) {
            const int notification = HIWORD(wparam);
            if (notification == LBN_SELCHANGE)
                select_entities_from_list();
            else if (notification == LBN_DBLCLK) {
                select_entities_from_list();
                focus_camera_on_entity(g.selected);
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const RECT vr = viewport_rect();
        if (!PtInRect(&vr, p))
            break;
        SetFocus(hwnd);
        Asura_Vector_3 world{};
        if (g.pending_kind >= 0) {
            if (environment_point_from_screen(p.x, p.y, &world))
                add_entity_at(static_cast<EntityKind>(g.pending_kind), world);
            else
                set_status("No environment geometry is under the cursor; placement remains active.");
            return 0;
        }
        const int hit = hit_entity(p.x, p.y);
        if ((GetKeyState(VK_CONTROL) & 0x8000) && hit >= 0) {
            toggle_entity_selection(hit);
            return 0;
        }
        select_entity(hit);
        if (hit >= 0 && g.history.begin(g.document, g.selected)) {
            g.moving_entity = true;
            g.entity_drag_last_mouse = p;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        finish_entity_drag_transaction();
        return 0;
    case WM_RBUTTONDOWN:
        g.orbiting = true;
        g.fast_preview = true;
        invalidate_environment_cache();
        g.last_mouse = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
        return 0;
    case WM_RBUTTONUP:
        g.orbiting = false;
        g.fast_preview = g.panning;
        invalidate_environment_cache();
        request_redraw();
        ReleaseCapture();
        return 0;
    case WM_MBUTTONDOWN:
        g.panning = true;
        g.fast_preview = true;
        invalidate_environment_cache();
        g.last_mouse = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
        return 0;
    case WM_MBUTTONUP:
        g.panning = false;
        g.fast_preview = g.orbiting;
        invalidate_environment_cache();
        request_redraw();
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        // A modal dialog or another window can steal capture before a button-up
        // arrives. Never let that leave a latent drag that moves on the next
        // unrelated mouse event.
        if (reinterpret_cast<HWND>(lparam) != hwnd) {
            if (g.moving_entity)
                finish_entity_drag_transaction();
            g.moving_entity = false;
            g.orbiting = false;
            g.panning = false;
            g.fast_preview = false;
            invalidate_environment_cache();
            request_redraw();
        }
        return 0;
    case WM_MOUSEMOVE: {
        const POINT now{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (g.moving_entity && g.selected >= 0) {
            if (now.x == g.entity_drag_last_mouse.x && now.y == g.entity_drag_last_mouse.y)
                return 0;
            g.entity_drag_last_mouse = now;
            Asura_Vector_3 p{};
            if (environment_point_from_screen(now.x, now.y, &p)) {
                Entity& e = g.document.entities[g.selected];
                if (e.position.x == p.x && e.position.y == p.y && e.position.z == p.z)
                    return 0;
                e.position = p;
                if (e.kind == EntityKind::Light)
                    e.light.Position = e.position;
                // Dragging only changes position. A full inspector refresh is
                // surprisingly expensive here (it rebuilds pickup/object combo
                // contents and toggles many controls on every mouse event).
                const bool was_refreshing_inspector = g.refreshing_inspector;
                g.refreshing_inspector = true;
                set_float(g.pos[0], e.position.x);
                set_float(g.pos[1], e.position.y);
                set_float(g.pos[2], e.position.z);
                g.refreshing_inspector = was_refreshing_inspector;
                request_redraw();
            }
        } else if (g.orbiting) {
            g.camera.yaw -= (now.x - g.last_mouse.x) * .008f;
            g.camera.pitch += (now.y - g.last_mouse.y) * .008f;
            g.camera.pitch = std::clamp(g.camera.pitch, -1.45f, 1.45f);
            g.last_mouse = now;
            invalidate_environment_cache();
            request_redraw();
        } else if (g.panning) {
            Asura_Vector_3 cam, right, up, forward;
            camera_axes(&cam, &right, &up, &forward);
            const float scale = g.camera.distance * .0018f;
            g.camera.target = add(g.camera.target,
                                  add(mul(right, -(now.x - g.last_mouse.x) * scale),
                                      mul(up, (now.y - g.last_mouse.y) * scale)));
            g.last_mouse = now;
            invalidate_environment_cache();
            request_redraw();
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        g.camera.distance *= delta > 0 ? .86f : 1.16f;
        g.camera.distance = std::clamp(g.camera.distance, .5f, 1000000.0f);
        g.fast_preview = true;
        invalidate_environment_cache();
        SetTimer(hwnd, 1, 140, nullptr);
        request_redraw();
        return 0;
    }
    case WM_DROPFILES: {
        char path[MAX_PATH * 4]{};
        DragQueryFileA(reinterpret_cast<HDROP>(wparam), 0, path, sizeof(path));
        DragFinish(reinterpret_cast<HDROP>(wparam));
        const std::string p = path;
        const size_t dot = p.find_last_of('.');
        const std::string ext = dot == std::string::npos ? "" : p.substr(dot);
        if (_stricmp(ext.c_str(), ".obj") == 0) {
            if (g.document.source_pc_path.empty() || confirm_discard())
                open_obj_path(p);
        }
        else if (_stricmp(ext.c_str(), ".pc") == 0) {
            if (confirm_discard())
                open_pc_path(p);
        }
        else if (_stricmp(ext.c_str(), ".alev") == 0) {
            if (!confirm_discard())
                return 0;
            std::string why;
            Document doc;
            if (load_project(&doc, p.c_str(), &why)) {
                stop_sound_preview();
                g.document = std::move(doc);
                set_single_selection_state(g.document.entities.empty() ? -1 : 0);
                g.pending_kind = -1;
                const bool skybox_loaded = reload_skybox_preview(true);
                load_document_preview(&g.document, &g.mesh, &why, &g.pickup_models,
                                      &g.static_object_models);
                frame_mesh();
                reset_history(!g.document.dirty);
                refresh_list();
                refresh_inspector();
                update_title();
                set_status(skybox_loaded ? "Project loaded." : "Project loaded; skybox preview is unavailable.");
                request_redraw();
            } else
                MessageBoxA(hwnd, why.c_str(), "Could not open project", MB_ICONERROR);
        }
        return 0;
    }
    case WM_CLOSE:
        if (confirm_discard())
            DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        stop_sound_preview();
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        release_environment_cache();
        if (g.font)
            DeleteObject(g.font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}


bool focused_edit_owns_clipboard_shortcut(const MSG& message) {
    if (message.message != WM_KEYDOWN || !(GetKeyState(VK_CONTROL) & 0x8000) ||
        (message.wParam != 'C' && message.wParam != 'V'))
        return false;
    const HWND focused = GetFocus();
    if (!focused || (focused != g.window && !IsChild(g.window, focused)))
        return false;
    char class_name[32]{};
    return GetClassNameA(focused, class_name, static_cast<int>(sizeof(class_name))) > 0 &&
           _stricmp(class_name, "Edit") == 0;
}

} // namespace editor
