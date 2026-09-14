#pragma once

#include "LevelEditorDocument.h"

namespace editor {

Asura_Quat decode_legacy_animation_quaternion(uint32_t word);
bool decode_model_animations(const asura::level::ChunkList &chunks,
                             const std::vector<uint32_t> &animation_ids, EntityModel *model,
                             asura::Error *err);
const ModelAnimation *entity_model_animation(const Entity &entity, const EntityModel &model);
// Skin into a separate vertex buffer; never mutate the shared bind-pose mesh.
bool sample_entity_model(const Entity &entity, const EntityModel &model, double seconds,
                         std::vector<EntityModelVertex> *vertices);

} // namespace editor
