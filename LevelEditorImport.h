#pragma once

#include "LevelEditorDocument.h"

#include <string>
#include <vector>

namespace editor {

inline constexpr float kSpawnCollisionHalfWidth = 0.3f;
inline constexpr float kSpawnCollisionHeight = 1.8f;
inline constexpr float kSpawnCollisionVerticalOffset = 0.1f;

Asura_Quat euler_quaternion(const Asura_Vector_3& degrees);
Asura_Vector_3 quaternion_euler(const Asura_Quat& quaternion);
Asura_Vector_3 matrix_euler(const float* matrix);

bool load_preview_mesh(const std::string& path, const std::string& material_map_path,
                       Mesh* mesh, std::string* why);
bool load_static_object_obj(const std::string& path, ImportedStaticObject* object, std::string* why);
bool prepare_imported_static_object(ImportedStaticObject* object, std::string* why);
bool decode_pc_model_materials(const asura::level::ChunkList& chunks, uint32_t before_chunk,
                               std::vector<EntityModelMaterial>* output, asura::Error* err,
                               bool load_textures = true);

struct ObjectCollisionMesh {
    uint32_t offset = 0, size = 0, version = 0;
    uint32_t flags = 0, materials = 0; // absolute offsets of overall values; zero if absent
    uint32_t flag_table = 0, flag_count = 0, material_table = 0, material_count = 0;
    uint32_t stride = 0;
};
bool object_collision_meshes(const asura::level::ChunkRef& chunk,
                             std::vector<ObjectCollisionMesh>* meshes, asura::Error* err);
bool load_static_object_collision_flags(const StaticObjectTemplate& object, uint16_t* flags,
                                        std::string* why);

std::string edited_pc_path(const std::string& path);

const char* snipe_item_name(uint32_t item_id);
std::string snipe_item_label(uint32_t item_id);
PickupTemplate pickup_template_from_entity(const Entity& entity);
StaticObjectTemplate make_canonical_static_object_template(
    uint32_t file_id, asura::Str resource_name, const std::string& donor_path);

uint32_t asura_lower_name_hash(asura::Str name);
uint32_t asura_lower_name_hash(const std::string& name);

const asura::level::RscfInfo* find_pc_environment(const asura::level::ChunkList& chunks,
                                                  asura::level::RscfInfo* storage);
bool decode_pc_environment(const asura::level::RscfInfo& resource, Mesh* mesh,
                           asura::Arena* arena, asura::Error* err);

bool load_static_object_donors(const std::vector<std::string>& paths,
                               std::vector<StaticObjectTemplate>* templates,
                               std::vector<StaticObjectModel>* models,
                               std::string* why);

bool load_pickup_donor(const std::string& path, std::vector<PickupTemplate>* templates,
                       std::vector<PickupModel>* models, std::string* why);

uint32_t allocate_editor_guid(Document* document);
bool normalise_editor_guids(Document* document, std::string* why);

struct PcSkyboxInfo {
    float red = 255.0f;
    float green = 255.0f;
    float blue = 255.0f;
    float orientation = 0.0f;
    asura::Str names[ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT]{};
    bool draw_clouds = false;
    bool back_texture_is_front_upside_down = false;
    bool right_texture_is_left_upside_down = false;
};

bool pc_skybox_info(const asura::level::ChunkList& chunks, PcSkyboxInfo* info,
                    asura::Error* err);

bool load_pc_level(const std::string& path, Document* document, Mesh* mesh,
                   std::string* why, std::vector<PickupModel>* pickup_models = nullptr,
                   std::vector<StaticObjectModel>* object_models = nullptr);

} // namespace editor
