#pragma once

#include "LevelEditorAppState.h"

#include <DirectXMath.h>

#include <string>

namespace editor {

struct SkyboxTextureScan {
    // Physical DDS payload selected for each SKYB slot.
    std::array<std::string, ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT> source_paths{};
    // Extensionless target resource paths derived from the discovered files.
    std::array<std::string, ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT> texture_paths{};
    uint32_t file_count = 0;
};

bool gpu_ready();
bool gpu_has_skybox_cloud();
bool gpu_has_rain_texture();
void gpu_set_skybox_tint(const SkyboxSettings& skybox);
void gpu_set_environment_wet_weather(bool enabled);

bool gpu_reload_environment_textures(std::string* why = nullptr,
                                     uint32_t* loaded_count = nullptr,
                                     uint32_t* missing_count = nullptr);
bool scan_skybox_texture_folder(const std::string& directory, SkyboxTextureScan* scan,
                                std::string* why);
bool gpu_load_skybox(const std::string& directory, std::string* why);
bool gpu_load_pc_skybox(const std::string& pc_path, const SkyboxSettings& settings,
                        std::string* why);
bool gpu_rebuild_skybox_vertices(float orientation, bool back_texture_is_front_upside_down,
                                 bool right_texture_is_left_upside_down, std::string* why);
bool gpu_init(HWND viewport);
bool gpu_resize(uint32_t width, uint32_t height);
void gpu_shutdown();
bool gpu_upload_mesh();
void gpu_render();

void append_oriented_bounds_gizmo(const Entity& entity,
                                  std::vector<LightGizmoLine>* lines);

} // namespace editor
