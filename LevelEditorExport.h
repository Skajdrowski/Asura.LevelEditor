#pragma once

#include "LevelEditorDocument.h"

#include <string>
#include <vector>

namespace editor {

struct PcEnvironmentMaterialBinding {
    asura::Str texture_name{};
    int32_t texture_index = -1;
    uint32_t flags = 0;
    uint32_t texture_flags = 0;
    uint32_t surface_type = 0;
};

bool source_ambience_info(const asura::level::ChunkRef& chunk, std::string* path, float* volume,
                          uint32_t* tail_offset, asura::Error* err);

bool pc_environment_material_bindings(
    const asura::level::ChunkList& chunks,
    std::vector<PcEnvironmentMaterialBinding>* materials,
    asura::Error* err);

bool pc_environment_collision_flags(
    const asura::level::ChunkList& chunks,
    uint32_t material_count,
    std::vector<uint32_t>* flags,
    asura::Error* err);

Asura_Vector_3 oriented_box_dimensions(const Entity& entity);

bool pack_document(Document& document, const char* output_path, std::string* why);
bool export_static_object_obj(const Entity& entity, const EntityModel& model,
                              const char* output_path, std::string* why);
bool export_pc_environment_obj(const std::string& source_pc_path, const char* output_path,
                               std::string* why);

} // namespace editor
