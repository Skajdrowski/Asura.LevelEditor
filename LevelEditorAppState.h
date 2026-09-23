#pragma once

#include "LevelEditorDocument.h"
#include "LevelEditorHistory.h"
#include "LevelEditorRaycast.h"

#include <windows.h>

#include <array>
#include <string>
#include <vector>

namespace editor {

struct Camera {
    Asura_Vector_3 target{};
    float yaw = .65f;
    float pitch = .42f;
    float distance = 80;
};

struct AppState {
    HWND window = nullptr;
    HWND list = nullptr;
    HWND name = nullptr;
    HWND pos[3]{};
    HWND rot[3]{};
    HWND value[3]{};
    HWND value_label[3]{};
    HWND pickup_item = nullptr;
    HWND rain_toggle = nullptr;
    HWND backface_cull_toggle = nullptr;
    HWND ambience_properties = nullptr;
    HWND sound_browse = nullptr;
    HWND sound_loop = nullptr;
    HWND sound_preview = nullptr;
    HWND sound_properties = nullptr;
    HWND spawn_team_label = nullptr;
    HWND spawn_game_mode_label = nullptr;
    HWND spawn_team_checks[4]{};
    HWND spawn_game_mode_checks[6]{};
    HWND light_properties = nullptr;
    HWND status = nullptr;
    HWND viewport = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    Document document;
    LevelEditorHistory history;
    Mesh mesh;
    EnvironmentRaycast environment_raycast;
    std::array<SpawnPuppet, 3> spawn_puppets;
    bool spawn_puppets_loaded = false;
    std::vector<PickupModel> pickup_models;
    std::vector<StaticObjectModel> static_object_models;
    std::vector<uint8_t> sound_preview_bytes;
    int sound_preview_entity = -1;
    Camera camera;
    std::vector<int> selected_entities;
    int selected = -1;
    int pending_kind = -1;
    uint32_t pending_object_file = 0;
    bool orbiting = false;
    bool panning = false;
    bool moving_entity = false;
    bool refreshing_inspector = false;
    bool backface_culling = true;
    uint32_t inspector_dirty = 0;
    bool fast_preview = false;
    HBITMAP environment_cache = nullptr;
    int environment_cache_width = 0;
    int environment_cache_height = 0;
    bool environment_cache_valid = false;
    bool environment_cache_fast = false;
    POINT last_mouse{};
    POINT entity_drag_last_mouse{};
};

extern AppState g;

struct LightGizmoLine {
    Asura_Vector_3 a{};
    Asura_Vector_3 b{};
};

inline constexpr int kLightRangeSegments = 48;
inline constexpr size_t kLightBoundingBoxLines = 12;
inline constexpr size_t kCameraSpawnArrowLines = 5;
inline constexpr size_t kMaximumLightGizmoLines =
    kLightRangeSegments * 3 + kLightBoundingBoxLines;
inline constexpr float kMaximumLightGizmoRange = 10000000.0f;

bool valid_entity_index(int index);
bool entity_is_selected(int index);
const SpawnPuppet* spawn_puppet_for_team(uint32_t team_mask);
const EntityModel* entity_render_model(const Entity& entity);
Asura_Vector_3 add(Asura_Vector_3 a, Asura_Vector_3 b);
Asura_Vector_3 sub(Asura_Vector_3 a, Asura_Vector_3 b);
Asura_Vector_3 mul(Asura_Vector_3 a, float b);
Asura_Vector_3 entity_view_position(Asura_Vector_3 game_position);
float dot(Asura_Vector_3 a, Asura_Vector_3 b);
Asura_Vector_3 cross(Asura_Vector_3 a, Asura_Vector_3 b);
Asura_Vector_3 normalized(Asura_Vector_3 a);
Asura_Vector_3 rotate_by_quaternion(Asura_Vector_3 value, const Asura_Quat& rotation);
Asura_Vector_3 entity_model_view_vector(Asura_Vector_3 value, const Entity& entity);
Asura_Vector_3 entity_model_view_position(Asura_Vector_3 local_position,
                                          const Entity& entity);
void append_light_gizmo_line(std::vector<LightGizmoLine>* lines, Asura_Vector_3 a,
                             Asura_Vector_3 b);
void append_camera_spawn_arrow(const Entity& entity, float marker,
                               std::vector<LightGizmoLine>* lines);
void append_sound_gizmo(const Entity& entity, std::vector<LightGizmoLine>* lines);
void append_collision_barrier_gizmo(const Entity& entity, std::vector<LightGizmoLine>* lines);
void append_light_gizmo(const Entity& entity, std::vector<LightGizmoLine>* lines);
void camera_axes(Asura_Vector_3* position, Asura_Vector_3* right, Asura_Vector_3* up,
                 Asura_Vector_3* forward);

} // namespace editor
