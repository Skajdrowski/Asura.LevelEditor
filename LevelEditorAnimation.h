#pragma once

#include "LevelEditorDocument.h"

namespace editor {

struct ModelAnimationChunkIndex {
    std::vector<uint32_t> skin_chunks;
    std::vector<uint32_t> animation_chunks;
};

void build_model_animation_chunk_index(const asura::level::ChunkList &chunks,
                                       ModelAnimationChunkIndex *index);
Asura_Quat decode_legacy_animation_quaternion(uint32_t word);
bool decode_model_animations(const asura::level::ChunkList &chunks,
                             const ModelAnimationChunkIndex *chunk_index,
                             const std::vector<uint32_t> &animation_ids, EntityModel *model,
                             asura::Error *err);
const ModelAnimation *entity_model_animation(const Entity &entity, const EntityModel &model);
// Skin into a separate vertex buffer; never mutate the shared bind-pose mesh.
bool sample_entity_model(const Entity &entity, const EntityModel &model, const ModelAnimation &animation,
                         double seconds,
                         std::vector<EntityModelVertex> *vertices,
                         std::vector<ModelBoneTransform> *transform_scratch);

} // namespace editor
