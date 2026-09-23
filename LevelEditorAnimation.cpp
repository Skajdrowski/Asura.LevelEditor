#include "LevelEditorAnimation.h"
#include "LevelEditorGeometry.h"
#include "LevelEditorImport.h"

#include <algorithm>
#include <cmath>

namespace editor {
using namespace asura;
using namespace asura::level;
namespace {
Asura_Vector_3 plus(Asura_Vector_3 a, Asura_Vector_3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Asura_Vector_3 times(Asura_Vector_3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
bool finite(Asura_Vector_3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool finite(const ModelBoneTransform &t) {
    const auto q = t.orientation;
    return finite(t.position) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
           std::isfinite(q.w) && q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w > .000001f;
}
Asura_Quat normalize(Asura_Quat q) {
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1.e-8f ? Asura_Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Asura_Quat{0, 0, 0, 1};
}
ModelBoneTransform compose(const ModelBoneTransform &a, const ModelBoneTransform &b) {
    const auto p = a.orientation, q = b.orientation;
    return {plus(a.position, rotate_by_quaternion(b.position, p)),
            normalize({p.w * q.x + p.x * q.w + p.y * q.z - p.z * q.y,
                       p.w * q.y - p.x * q.z + p.y * q.w + p.z * q.x,
                       p.w * q.z + p.x * q.y - p.y * q.x + p.z * q.w,
                       p.w * q.w - p.x * q.x - p.y * q.y - p.z * q.z})};
}
ModelBoneTransform inverse(const ModelBoneTransform &t) {
    const Asura_Quat q{-t.orientation.x, -t.orientation.y, -t.orientation.z, t.orientation.w};
    return {rotate_by_quaternion(times(t.position, -1), q), q};
}
ModelBoneTransform interpolate(const ModelBoneTransform &a, const ModelBoneTransform &b, float t) {
    Asura_Quat q = a.orientation, r = b.orientation;
    float dot = q.x * r.x + q.y * r.y + q.z * r.z + q.w * r.w;
    if (dot < 0) {
        r = {-r.x, -r.y, -r.z, -r.w};
        dot = -dot;
    }
    float s = 1 - t, u = t;
    if (dot < .9995f) {
        float angle = acosf(std::clamp(dot, -1.f, 1.f));
        s = sinf((1 - t) * angle) / sinf(angle);
        u = sinf(t * angle) / sinf(angle);
    }
    return {plus(times(a.position, 1 - t), times(b.position, t)),
            normalize({q.x * s + r.x * u, q.y * s + r.y * u, q.z * s + r.z * u, q.w * s + r.w * u})};
}
bool decode_skin(const ChunkList &chunks, const ModelAnimationChunkIndex &index,
                 EntityModel *model, Error *err) {
    for (uint32_t i : index.skin_chunks) {
        const auto &c = chunks.chunks[i];
        const Str name = padded_string_at(c.data, c.size, 24);
        if (!name.data ||
            !str_ieq(name, {model->resource_name.data(), static_cast<uint32_t>(model->resource_name.size())}))
            continue;
        if (c.version < 4 || c.version > 6 || c.size < 28)
            return fail(err, "Unsupported HSKN for '%s'", model->resource_name.c_str());
        const uint32_t count = read_u32(c.data + 20), vertices = read_u32(c.data + 16);
        const uint64_t at = align_up(24ull + name.size + 1, 4) + uint64_t(vertices) * 72;
        if (!count || count > 65535 || at + uint64_t(count) * 32 > c.size)
            return fail(err, "Invalid HSKN bone table for '%s'", model->resource_name.c_str());
        model->bones.resize(count);
        for (uint32_t b = 0; b < count; ++b) {
            auto &bone = model->bones[b];
            bone.parent = read_u32(c.data + at + b * 4);
            memcpy(&bone.bind, c.data + at + count * 4 + b * 28, 28);
            if ((b && bone.parent >= b) || !finite(bone.bind))
                return fail(err, "Invalid HSKN bind pose for '%s'", model->resource_name.c_str());
            bone.bind.orientation = normalize(bone.bind.orientation);
        }
        return true;
    }
    // Shared game assets can supply a skeleton absent from this donor.
    return true;
}
bool decode_animation(const ChunkRef &c, ModelAnimation *animation, Error *err) {
    if (c.version > 4 || c.size < 48)
        return fail(err, "Unsupported HCAN animation");
    const auto name = padded_string_at(c.data, c.size, 44);
    const uint32_t bone_count = read_u32(c.data + 16), keys = read_u32(c.data + 32),
                   offsets = read_u32(c.data + 36);
    if (!name.data || !bone_count || bone_count > 65535 || keys > 65535 || offsets > 65535)
        return fail(err, "Invalid HCAN header");
    animation->id = asura_lower_name_hash(name);
    animation->duration = read_f32(c.data + 20);
    if (!std::isfinite(animation->duration) || animation->duration <= 0)
        return fail(err, "Invalid HCAN duration");
    const uint32_t records = bone_count + ((c.flags & 16) ? 1 : 0), stride = (c.flags & 32) ? 8 : 16;
    const uint64_t at = align_up(44ull + name.size + 1, 4), quat_at = at + uint64_t(records) * stride,
                   time_at = quat_at + uint64_t(keys) * 4, pos_at = time_at + uint64_t(keys) * 2;
    uint64_t tail = pos_at + uint64_t(offsets) * 12;
    if (tail > c.size)
        return fail(err, "Truncated HCAN animation arrays");
    animation->tracks.resize(bone_count);
    for (uint32_t b = 0; b < bone_count; ++b) {
        const uint8_t *p = c.data + at + uint64_t(b) * stride;
        const uint32_t n = stride == 8 ? read_u16(p) : read_u32(p),
                       first = stride == 8 ? read_u16(p + 2) : read_u32(p + 4),
                       count = stride == 8 ? read_u16(p + 4) : read_u32(p + 8),
                       offset = stride == 8 ? read_u16(p + 6) : read_u32(p + 12);
        if (!n)
            continue;
        if (uint64_t(first) + n > keys || uint64_t(offset) + count > offsets || (count != 1 && count != n))
            return fail(err, "Invalid HCAN bone track");
        auto &track = animation->tracks[b];
        track.reserve(n);
        for (uint32_t k = 0; k < n; ++k) {
            ModelAnimationKey key;
            key.time = read_u16(c.data + time_at + (first + k) * 2) / 65535.f;
            key.transform.orientation =
                decode_legacy_animation_quaternion(read_u32(c.data + quat_at + (first + k) * 4));
            memcpy(&key.transform.position, c.data + pos_at + (offset + (count == 1 ? 0 : k)) * 12, 12);
            if (!finite(key.transform) || (!track.empty() && key.time < track.back().time))
                return fail(err, "Invalid HCAN keyframe");
            if (!track.empty() && key.time == track.back().time)
                track.back() = key;
            else
                track.push_back(key);
        }
    }
    if (c.flags & 1)
        tail += uint64_t(bone_count) * 4;
    tail += uint64_t(read_u32(c.data + 40)) * 12;
    if (c.version >= 1 && (c.flags & 8))
        tail += 24;
    if (c.version >= 2) {
        if (tail + 4 > c.size)
            return fail(err, "Truncated HCAN loop point");
        animation->loop_point = read_f32(c.data + tail);
        tail += 4;
        if (!std::isfinite(animation->loop_point) || animation->loop_point < 0 || animation->loop_point > 1)
            return fail(err, "Invalid HCAN loop point");
    }
    if (c.version >= 3) {
        if (tail + 4 > c.size)
            return fail(err, "Truncated HCAN sound table");
        tail += 4ull + uint64_t(read_u32(c.data + tail)) * 8;
    }
    if (tail > c.size)
        return fail(err, "Truncated HCAN trailing data");
    return true;
}
} // namespace

void build_model_animation_chunk_index(const ChunkList &chunks, ModelAnimationChunkIndex *index) {
    index->skin_chunks.clear();
    index->animation_chunks.clear();
    index->skin_chunks.reserve(chunks.count / 16 + 1);
    index->animation_chunks.reserve(chunks.count / 16 + 1);
    for (uint32_t i = 0; i < chunks.count; ++i) {
        const uint32_t cid = chunks.chunks[i].cid;
        if (cid == ASURA_CHUNK_HIERARCHY_SKIN)
            index->skin_chunks.push_back(i);
        else if (cid == ASURA_CHUNK_HIERARCHY_COMPRESSEDANIM)
            index->animation_chunks.push_back(i);
    }
}

Asura_Quat decode_legacy_animation_quaternion(uint32_t word) {
    // MCP1 Asura_Comp_Quat::ToQuat, 0x821FA080; MCP2 0x435F60. HCAN <=v4 uses
    // this legacy axis-angle codec; the later fast quaternion format differs.
    constexpr float step = 3.14159265358979323846f / 512;
    constexpr float angle_limit = 1.5707999467849731f; // exact target bits: 0x3FC90FF9
    const float w = (word & 2047) / 1024.f;
    if (w >= 1)
        return {0, 0, 0, 1};
    const float a = std::min(angle_limit, ((word >> 20) & 511) * step),
                b = std::min(angle_limit, ((word >> 11) & 511) * step);
    const float cosine_b = cosf(b);
    float x = sinf(a) * cosine_b, y = sinf(b), z = cosf(a) * cosine_b;
    if (word & 0x80000000) x = -x;
    if (word & 0x40000000) y = -y;
    if (word & 0x20000000) z = -z;
    const float s = sqrtf(1 - w * w);
    return {x * s, y * s, z * s, w};
}

bool decode_model_animations(const ChunkList &chunks, const ModelAnimationChunkIndex *chunk_index,
                             const std::vector<uint32_t> &ids, EntityModel *model, Error *err) {
    if (model->weights.empty() || ids.empty())
        return true;
    ModelAnimationChunkIndex local_index;
    if (!chunk_index) {
        build_model_animation_chunk_index(chunks, &local_index);
        chunk_index = &local_index;
    }
    if (!decode_skin(chunks, *chunk_index, model, err))
        return false;
    if (model->bones.empty())
        return true;
    for (auto &weights : model->weights) {
        float sum = 0;
        if (model->resource_subtype == ASURA_RESOURCEFILE_TYPE_PC_CHARACTER &&
            weights.bones[0] >= model->bones.size())
            return fail(err, "Invalid normal bone in '%s'", model->resource_name.c_str());
        for (size_t j = 0; j < 4; ++j) {
            const float w = weights.weights[j];
            if (!std::isfinite(w) || w < 0 || (w > 0 && weights.bones[j] >= model->bones.size()))
                return fail(err,
                            "Invalid vertex skin weights for '%s' (bone %u of %zu, weight %g, resource %u)",
                            model->resource_name.c_str(), weights.bones[j], model->bones.size(), w,
                            model->resource_subtype);
            sum += w;
        }
        if (sum <= .00001f)
            return fail(err, "Unweighted vertex in '%s'", model->resource_name.c_str());
        for (float &w : weights.weights)
            w /= sum;
    }
    for (uint32_t i : chunk_index->animation_chunks) {
        const auto &c = chunks.chunks[i];
        const auto name = padded_string_at(c.data, c.size, 44);
        if (!name.data)
            continue;
        const uint32_t id = asura_lower_name_hash(name);
        if (std::find(ids.begin(), ids.end(), id) == ids.end())
            continue;
        // Delta and movement animations require controllers not represented by a static object.
        // Preserve the original resource on export; leave those previews in bind pose.
        if (c.flags & (2 | 16))
            continue;
        ModelAnimation animation;
        if (!decode_animation(c, &animation, err))
            return false;
        if (animation.tracks.size() != model->bones.size())
            return fail(err, "Animation bone count does not match '%s'", model->resource_name.c_str());
        bool duplicate = false;
        for (const auto &existing : model->animations)
            duplicate |= existing.id == id;
        if (!duplicate)
            model->animations.push_back(std::move(animation));
    }
    return true;
}

const ModelAnimation *entity_model_animation(const Entity &entity, const EntityModel &model) {
    if (model.bones.empty() || model.weights.size() != model.vertices.size())
        return nullptr;
    for (const auto &animation : model.animations)
        if (animation.id == entity.pickup_anim_id)
            return &animation;
    return nullptr;
}

bool sample_entity_model(const Entity &entity, const EntityModel &model, const ModelAnimation &animation,
                         double seconds,
                         std::vector<EntityModelVertex> *vertices,
                         std::vector<ModelBoneTransform> *transform_scratch) {
    if (!transform_scratch)
        return false;
    Asura_ServerEntity_PhysicalObject_ChunkDataV7 physical{};
    if (entity.kind == EntityKind::StaticObject && entity.static_object_has_template)
        memcpy(&physical,
               entity.static_object_body.data() +
                   offsetof(Snipe_ServerEntity_StaticObject_ChunkDataV0, m_xPhysicalObject),
               sizeof(physical));
    else if (entity.kind == EntityKind::Pickup && entity.pickup_has_template)
        memcpy(&physical,
               entity.pickup_body.data() + offsetof(Snipe_ServerEntity_Pickup_ChunkDataV0, m_xPhysicalObject),
               sizeof(physical));
    else
        physical.m_iAnimFlags = 1;
    double time =
        seconds / animation.duration + (std::isfinite(physical.m_fAnimTimer) ? physical.m_fAnimTimer : 0);
    if (time > 1 && physical.m_iAnimFlags & 1) {
        const double loop = animation.loop_point;
        time = loop < 1 ? loop + fmod(time - loop, 1 - loop) : 1;
    }
    const float t = static_cast<float>(std::clamp(time, 0., 1.));
    const size_t bone_count = model.bones.size();
    transform_scratch->resize(bone_count * 2);
    ModelBoneTransform* bind = transform_scratch->data();
    ModelBoneTransform* pose = bind + bone_count;
    for (size_t b = 0; b < model.bones.size(); ++b) {
        const auto &bone = model.bones[b];
        auto local = bone.bind;
        const auto &track = animation.tracks[b];
        if (!track.empty()) {
            auto next =
                std::upper_bound(track.begin(), track.end(), t,
                                 [](float time, const ModelAnimationKey &k) { return time < k.time; });
            if (next == track.begin())
                local = next->transform;
            else if (next == track.end())
                local = track.back().transform;
            else {
                const auto &prev = *(next - 1);
                local =
                    interpolate(prev.transform, next->transform, (t - prev.time) / (next->time - prev.time));
            }
        }
        bind[b] = b ? compose(bind[bone.parent], bone.bind) : bone.bind;
        pose[b] = b ? compose(pose[bone.parent], local) : local;
    }
    // MCP2 0x49DCD0 builds the current-global * inverse-bind palette.
    for (size_t b = 0; b < bone_count; ++b)
        pose[b] = compose(pose[b], inverse(bind[b]));
    *vertices = model.vertices;
    for (size_t i = 0; i < vertices->size(); ++i) {
        auto &v = (*vertices)[i];
        Asura_Vector_3 position{}, normal{};
        for (size_t j = 0; j < 4; ++j) {
            const float w = model.weights[i].weights[j];
            if (w <= 0)
                continue;
            const auto &bone = pose[model.weights[i].bones[j]];
            position = plus(position, times(plus(rotate_by_quaternion(v.position, bone.orientation), bone.position), w));
            normal = plus(normal, times(rotate_by_quaternion(v.normal, bone.orientation), w));
        }
        v.position = position;
        // The retail Character shader skins its normal with the first bone
        // only (MCP2 shader at 0x6FD038), while blending four position weights.
        v.normal = model.resource_subtype == ASURA_RESOURCEFILE_TYPE_PC_CHARACTER
                       ? rotate_by_quaternion(v.normal, pose[model.weights[i].bones[0]].orientation)
                       : normal;
    }
    return true;
}
} // namespace editor
