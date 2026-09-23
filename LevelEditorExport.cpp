#include "LevelEditorExport.h"
#include "LevelEditorGeometry.h"
#include "LevelEditorImport.h"
#include "LevelEditorSoundTriggers.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <unordered_map>

using namespace asura;
using namespace asura::level;

namespace editor {

// The target loads Snipe physical flag bit 0 into the client visibility gate
// (MCP2 0x576A00); Render and LOSTest skip the instance when it is set
// (0x575D70, 0x576100). The editor shows these objects, and does not export
// the original mission scripts that could reveal them later.
constexpr uint32_t kSnipePhysicalHidden = 1u;

bool append_editor_lights(Buffer* out, const Document& doc, Error* err) {
    uint32_t count = 0;
    for (const Entity& e : doc.entities)
        count += e.kind == EntityKind::Light;
    // The target loads entity ambient RGB from LITE even when its light count
    // is zero (MCP2 0x43EB50). Omitting the chunk loses ambient illumination.
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_LIGHTS, 5, 0, err);
    append_u32(out, count, err);
    buffer_append(out, &doc.light_header_a, sizeof(doc.light_header_a), err);
    buffer_append(out, &doc.light_header_b, sizeof(doc.light_header_b), err);
    buffer_append(out, &doc.light_header_c, sizeof(doc.light_header_c), err);
    append_u32(out, doc.light_header_flag, err);
    for (const Entity& e : doc.entities) {
        if (e.kind != EntityKind::Light)
            continue;
        Asura_Light rec = e.light;
        rec.Position = e.position;
        buffer_append(out, &rec, sizeof(rec), err);
    }
    return end_chunk(out, ch, err);
}

bool append_editor_skybox(Buffer* out, const SkyboxSettings& skybox, Error* err) {
    if (!isfinite(skybox.red) || !isfinite(skybox.green) || !isfinite(skybox.blue) ||
        !isfinite(skybox.orientation_radians))
        return fail(err, "SKYB colour and orientation values must be finite");
    ChunkMark chunk = begin_chunk(out, ASURA_CHUNK_SKYBOX, 7, 0, err);
    const Asura_Chunk_SkyBox_PayloadPrefixV7 prefix{
        skybox.red, skybox.green, skybox.blue, skybox.orientation_radians};
    buffer_append(out, &prefix, sizeof(prefix), err);
    for (const std::string& texture_path : skybox.texture_paths) {
        if (texture_path.size() > 4096 || texture_path.find('\0') != std::string::npos)
            return fail(err, "SKYB texture paths must contain at most 4096 non-NUL bytes");
        append_padded_cstr(out, {texture_path.data(), static_cast<uint32_t>(texture_path.size())}, err);
    }
    const Asura_Chunk_SkyBox_TrailingFlagsV7 flags{
        skybox.draw_clouds ? 1u : 0u,
        skybox.back_texture_is_front_upside_down ? 1u : 0u,
        skybox.right_texture_is_left_upside_down ? 1u : 0u};
    return buffer_append(out, &flags, sizeof(flags), err) != ~0ull && end_chunk(out, chunk, err);
}

bool append_editor_weather(Buffer* out, const Document& document, Error* err) {
    const uint64_t start = out->size;
    if (!append_wthr(out, err))
        return false;
    const uint8_t enabled[2]{document.rain_enabled ? 1u : 0u,
                             document.rain_enabled ? 1u : 0u};
    return buffer_patch(out, start + 21, enabled, sizeof(enabled), err);
}

Asura_Bounding_Box sound_region_bounds(const Entity& entity, bool outer) {
    const auto& b = outer ? entity.ambience_outer_bounds : entity.ambience_inner_bounds;
    return {b.MinX + entity.position.x, b.MaxX + entity.position.x,
            b.MinY + entity.position.y, b.MaxY + entity.position.y,
            b.MinZ + entity.position.z, b.MaxZ + entity.position.z};
}

bool valid_sound_region(const Entity& entity, Error* err) {
    if (entity.sound_file.size() > 4096 || entity.sound_file.find('\0') != std::string::npos ||
        !isfinite(entity.value_a) || entity.value_a < 0 || entity.value_a > 1)
        return fail(err, "sound region '%s' needs a valid stream path and volume from 0 to 1", entity.name.c_str());
    const auto inner = sound_region_bounds(entity, false);
    const auto outer = sound_region_bounds(entity, true);
    float a[6], b[6];
    memcpy(a, &inner, sizeof(a));
    memcpy(b, &outer, sizeof(b));
    for (int axis = 0; axis < 6; axis += 2) {
        if (!isfinite(a[axis]) || !isfinite(a[axis + 1]) ||
            !isfinite(b[axis]) || !isfinite(b[axis + 1]) ||
            a[axis] > a[axis + 1] || b[axis] > a[axis] || b[axis + 1] < a[axis + 1] ||
            fabsf(b[axis]) > 1.0e6f || fabsf(b[axis + 1]) > 1.0e6f)
            return fail(err, "sound region '%s' needs finite inner bounds contained by its outer bounds", entity.name.c_str());
    }
    return true;
}

bool valid_ambience_settings(const Document& document, Error* err) {
    if (document.ambient_stream_path.size() > 4096 ||
        document.ambient_stream_path.find('\0') != std::string::npos)
        return fail(err, "the ambience stream path must contain at most 4096 non-NUL bytes");
    if (!isfinite(document.ambient_volume) || document.ambient_volume < 0.0f ||
        document.ambient_volume > 1.0f)
        return fail(err, "the ambience volume must be between 0 and 1");
    return true;
}

bool append_editor_ambience(Buffer* out, const Document& document, Error* err) {
    if (!valid_ambience_settings(document, err))
        return false;
    std::vector<const Entity*> regions;
    for (const Entity& entity : document.entities) {
        if (entity.kind != EntityKind::SoundRegion) continue;
        if (!valid_sound_region(entity, err)) return false;
        regions.push_back(&entity);
    }
    // MCP2 0x4836C0 shares 16 KiB between results and the traversal stack.
    // This cap also covers the worst case where every balanced-tree leaf overlaps.
    if (regions.size() > 4000)
        return fail(err, "SBSN supports at most 4000 overlapping sound regions");
    if (regions.empty() && document.ambient_stream_path.empty()) return true;
    ChunkMark chunk = begin_chunk(out, ASURA_CHUNK_STREAMINGBACKGROUNDSOUND, 1, 0, err);
    append_u32(out, static_cast<uint32_t>(regions.size()), err);
    append_u32(out, 0, err); // SBSN flags (unused by the 2005 target reader)
    append_f32(out, document.ambient_volume, err);
    if (!append_padded_cstr(out,
                            {document.ambient_stream_path.data(),
                             static_cast<uint32_t>(document.ambient_stream_path.size())},
                            err))
        return false;
    std::vector<std::array<float, 6>> boxes;
    std::vector<uint16_t> order;
    for (const Entity* entity : regions) {
        const auto inner = sound_region_bounds(*entity, false);
        const auto outer = sound_region_bounds(*entity, true);
        append_f32(out, entity->value_a, err);
        buffer_append(out, &inner, sizeof(inner), err);
        buffer_append(out, &outer, sizeof(outer), err);
        append_padded_cstr(out, {entity->sound_file.data(), static_cast<uint32_t>(entity->sound_file.size())}, err);
        std::array<float, 6> box;
        memcpy(box.data(), &outer, sizeof(outer));
        // Tree tests use strict intersections. Padding keeps exact region edges reachable.
        for (int axis = 0; axis < 6; axis += 2) { box[axis] -= .5f; box[axis + 1] += .5f; }
        order.push_back(static_cast<uint16_t>(boxes.size()));
        boxes.push_back(box);
    }
    if (!regions.empty()) {
        const auto bounds = [&](size_t first, size_t last) {
            auto box = boxes[order[first]];
            for (size_t i = first + 1; i < last; ++i)
                for (int axis = 0; axis < 6; axis += 2) {
                    box[axis] = std::min(box[axis], boxes[order[i]][axis]);
                    box[axis + 1] = std::max(box[axis + 1], boxes[order[i]][axis + 1]);
                }
            return box;
        };
        const auto root = bounds(0, order.size());
        buffer_append(out, root.data(), sizeof(root), err);
        std::vector<std::array<uint8_t, 12>> nodes(regions.size() - 1);
        uint16_t next_node = 0;
        const auto build = [&](auto&& self, size_t first, size_t last,
                               const std::array<float, 6>& parent) -> uint16_t {
            if (last - first == 1) return order[first];
            const uint16_t index = next_node++;
            auto& node = nodes[index];
            int split = 0;
            for (int axis = 2; axis < 6; axis += 2)
                if (parent[axis + 1] - parent[axis] > parent[split + 1] - parent[split]) split = axis;
            const size_t middle = first + (last - first) / 2;
            std::nth_element(order.begin() + first, order.begin() + middle, order.begin() + last,
                [&](uint16_t a, uint16_t b) {
                    const float ca = boxes[a][split] + boxes[a][split + 1];
                    const float cb = boxes[b][split] + boxes[b][split + 1];
                    return ca != cb ? ca < cb : a < b;
                });
            const auto left = bounds(first, middle), right = bounds(middle, last);
            auto decoded_left = parent, decoded_right = parent;
            // MCP2 0x402A00: six 1/128 offsets; each side tightens one child.
            for (int axis = 0; axis < 3; ++axis) {
                const float step = (parent[axis * 2 + 1] - parent[axis * 2]) / 128.0f;
                for (int side = 0; side < 2; ++side) {
                    const int field = axis * 2 + side, bit = axis + side * 3;
                    const bool choose_left = side ? left[field] < right[field] : left[field] > right[field];
                    const float value = choose_left ? left[field] : right[field];
                    const float distance = side ? parent[field] - value : value - parent[field];
                    int q = std::clamp(static_cast<int>(floorf(distance / step)), 0, 128);
                    // Leave a quantization step of slack for x87/SSE rounding differences.
                    if (q) --q;
                    node[bit] = static_cast<uint8_t>(q);
                    if (choose_left) node[10] |= 1u << bit;
                    (choose_left ? decoded_left : decoded_right)[field] =
                        parent[field] + (side ? -q : q) * step;
                }
            }
            if (middle - first == 1) node[10] |= 0x40;
            if (last - middle == 1) node[10] |= 0x80;
            const uint16_t children[]{self(self, first, middle, decoded_left),
                                      self(self, middle, last, decoded_right)};
            memcpy(node.data() + 6, children, sizeof(children));
            return index;
        };
        if (regions.size() > 1) build(build, 0, order.size(), root);
        buffer_append(out, nodes.data(), nodes.size() * 12, err);
    }
    return end_chunk(out, chunk, err);
}

bool append_editor_spawnpoints(Buffer* out, const Document& doc, Error* err) {
    uint32_t index = 0;
    for (const Entity& e : doc.entities) {
        if (e.kind != EntityKind::SpawnPoint)
            continue;
        Snipe_ServerEntity_SpawnPoint_ChunkDataV0 data{};
        data.m_xEntity.Guid = e.guid ? e.guid : kToolCreatedGuidFirst + index;
        data.m_xEntity.Classification = SnipeEntityClass_SpawnPoint;
        data.m_xEntity.m_usPadding = e.entity_padding;
        data.m_xPosition = e.position;
        data.m_xDirection = e.spawn_direction;
        data.m_iSpawnIndex = e.spawn_source_record ? e.spawn_index : index;
        data.m_iPosture = e.spawn_source_record ? e.spawn_posture : 0;
        data.m_uTeamMask = e.value_u32_a;
        data.m_uGameModeMask = e.value_u32_b;
        data.m_fSpawnTimer = e.spawn_source_record ? e.spawn_timer : 5.0f;
        ChunkMark ch = begin_chunk(out, ASURA_CHUNK_ENTITY, 0, 0, err);
        buffer_append(out, &data, sizeof(data), err);
        end_chunk(out, ch, err);
        index++;
    }
    return !err->set;
}

bool make_editor_sounds(const Document& doc, Sounds* sounds, Arena* arena, Error* err) {
    uint32_t count = 0;
    for (const Entity& e : doc.entities)
        count += e.kind == EntityKind::Sound;
    if (!count)
        return true;
    sounds->items = arena_array<SoundEntry>(arena, count, err);
    if (!sounds->items)
        return false;
    sounds->count = count;
    uint32_t next_resource_id = 1, next_phonon_guid = kToolCreatedGuidFirst;
    for (const Entity& e : doc.entities) {
        if (e.kind != EntityKind::Sound || !e.sound_source_record)
            continue;
        if (e.sound_phonon.m_uSoundResourceID >= next_resource_id && e.sound_phonon.m_uSoundResourceID != 0xffffffffu)
            next_resource_id = e.sound_phonon.m_uSoundResourceID + 1;
        if (e.sound_phonon.m_uGuid >= next_phonon_guid && e.sound_phonon.m_uGuid != 0xffffffffu)
            next_phonon_guid = e.sound_phonon.m_uGuid + 1;
    }
    uint32_t index = 0;
    for (const Entity& e : doc.entities) {
        if (e.kind != EntityKind::Sound)
            continue;
        SoundEntry& s = sounds->items[index];
        s.name = {e.sound_name.data(), static_cast<uint32_t>(e.sound_name.size())};
        s.file = e.sound_file.empty() ? nullptr : e.sound_file.c_str();
        s.position = e.position;
        s.inner_radius = e.value_a;
        s.outer_radius = e.value_b;
        if (e.sound_source_record) {
            memcpy(s.legacy_volume_parameters, e.sound_phonon.m_afLegacyVolumeParameters,
                   sizeof(s.legacy_volume_parameters));
            s.inner_cuboid_radius = e.sound_phonon.m_xInnerCuboidRadius;
            s.outer_cuboid_radius = e.sound_phonon.m_xOuterCuboidRadius;
            s.retrigger_bounding_box = e.sound_phonon.m_xRetriggerBoundingBox;
            s.orientation = e.sound_phonon.m_xOrient;
            s.sound_resource_id = e.sound_phonon.m_uSoundResourceID;
            s.phonon_guid = e.sound_phonon.m_uGuid;
            s.flags = e.sound_loop ? (e.sound_phonon.m_uFlags | 1u) : (e.sound_phonon.m_uFlags & ~1u);
            s.controller_guid = e.guid;
            s.controller_padding = e.sound_controller_padding;
            s.emit_enti = e.sound_has_controller || e.sound_trigger_enabled;
            s.active = !e.sound_trigger_enabled && e.sound_controller_active;
        } else {
            const float params[7] = {0, 0, 0, 1, 1, 1, 1};
            memcpy(s.legacy_volume_parameters, params, sizeof(params));
            s.inner_cuboid_radius = {5, 5, 5};
            s.outer_cuboid_radius = {10, 10, 10};
            s.orientation = euler_quaternion(e.rotation);
            s.sound_resource_id = next_resource_id++;
            s.controller_guid = e.guid ? e.guid : kSoundControllerGuidBase + index;
            s.phonon_guid = next_phonon_guid++;
            s.flags = e.sound_loop ? 3u : 2u;
            s.controller_padding = e.sound_controller_padding;
            s.emit_enti = true;
            s.active = !e.sound_trigger_enabled && e.sound_controller_active;
        }
        index++;
    }
    return true;
}

void make_pickup_body(const Entity& entity, std::array<uint8_t, kPickupBodySize>* body) {
    Snipe_ServerEntity_Pickup_ChunkDataV0 wire{};
    if (entity.source_entity_record) {
        memcpy(&wire, entity.pickup_body.data(), sizeof(wire));
    } else {
        const PickupTemplate canonical = pickup_template_from_entity(entity);
        memcpy(&wire, canonical.body.data(), sizeof(wire));
    }
    wire.m_uItemID = entity.value_u32_a;
    wire.m_xPhysicalObject.m_xPosition = entity.position;
    wire.m_xPhysicalObject.m_xOrientation = euler_quaternion(entity.rotation);
    wire.m_xPhysicalObject.m_fHealth = entity.value_a;
    wire.m_xPhysicalObject.m_uFileID = entity.value_u32_b;
    wire.m_xPhysicalObject.m_uSkinID = entity.pickup_skin_id;
    wire.m_xPhysicalObject.m_uAnimID = entity.pickup_anim_id;
    wire.m_xPhysicalObject.m_uAnimFileID = entity.pickup_anim_file_id;
    wire.m_uSnipePhysicalFlags &= ~kSnipePhysicalHidden;
    // Original navigation links are not part of the rebuilt level. The target
    // reader (MCP2 0x465B10) consumes this many trailing u32 records.
    wire.m_uNumLinksToBlock = 0;
    memcpy(body->data(), &wire, sizeof(wire));
}

bool append_editor_pickups(Buffer* out, const Document& doc, Error* err) {
    for (const Entity& entity : doc.entities) {
        if (entity.kind != EntityKind::Pickup)
            continue;
        if (!entity.pickup_has_template)
            return fail(err, "pickup '%s' has no resolved item asset profile", entity.name.c_str());
        ChunkMark chunk = begin_chunk(out, ASURA_CHUNK_ENTITY, 0, 0, err);
        Asura_Chunk_Entity_PayloadHeader header{};
        header.Guid = entity.guid;
        header.Classification = SnipeEntityClass_Pickup;
        header.m_usPadding = entity.entity_padding;
        std::array<uint8_t, kPickupBodySize> body{};
        make_pickup_body(entity, &body);
        buffer_append(out, &header, sizeof(header), err);
        buffer_append(out, body.data(), body.size(), err);
        if (!end_chunk(out, chunk, err))
            return false;
    }
    return true;
}

void make_static_object_body(const Entity& entity,
                             std::array<uint8_t, kStaticObjectBodySize>* body) {
    Snipe_ServerEntity_StaticObject_ChunkDataV0 wire{};
    if (entity.static_object_has_template)
        memcpy(&wire, entity.static_object_body.data(), sizeof(wire));
    else
        memcpy(&wire, make_canonical_static_object_template(entity.value_u32_b, {}, {}).body.data(),
               sizeof(wire));
    wire.m_xPhysicalObject.m_xPosition = entity.position;
    wire.m_xPhysicalObject.m_xOrientation = euler_quaternion(entity.rotation);
    wire.m_xPhysicalObject.m_fHealth = entity.value_a;
    wire.m_xPhysicalObject.m_uFileID = entity.value_u32_b;
    wire.m_xPhysicalObject.m_uSkinID = entity.pickup_skin_id;
    wire.m_xPhysicalObject.m_uAnimID = entity.pickup_anim_id;
    wire.m_xPhysicalObject.m_uAnimFileID = entity.pickup_anim_file_id;
    wire.m_uSnipePhysicalFlags &= ~kSnipePhysicalHidden;
    wire.m_uNumLinksToBlock = 0;
    memcpy(body->data(), &wire, sizeof(wire));
}

bool append_editor_static_objects(Buffer* out, const Document& doc, Error* err) {
    for (const Entity& entity : doc.entities) {
        if (entity.kind != EntityKind::StaticObject)
            continue;
        if (!entity.static_object_has_template)
            return fail(err, "Object '%s' has no resolved asset profile", entity.name.c_str());
        ChunkMark chunk = begin_chunk(out, ASURA_CHUNK_ENTITY, 0, 0, err);
        Asura_Chunk_Entity_PayloadHeader header{};
        header.Guid = entity.guid;
        header.Classification = SnipeEntityClass_StaticObject;
        header.m_usPadding = entity.entity_padding;
        std::array<uint8_t, kStaticObjectBodySize> body{};
        make_static_object_body(entity, &body);
        buffer_append(out, &header, sizeof(header), err);
        buffer_append(out, body.data(), body.size(), err);
        if (!end_chunk(out, chunk, err))
            return false;
    }
    return true;
}

Asura_Vector_3 collision_sub(Asura_Vector_3 a, Asura_Vector_3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

float collision_dot(Asura_Vector_3 a, Asura_Vector_3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Asura_Vector_3 collision_cross(Asura_Vector_3 a, Asura_Vector_3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

bool triangle_overlaps_box_axis(Asura_Vector_3 a, Asura_Vector_3 b, Asura_Vector_3 c,
                                Asura_Vector_3 axis, Asura_Vector_3 half_size) {
    if (collision_dot(axis, axis) <= 1e-12f)
        return true;
    const float pa = collision_dot(a, axis), pb = collision_dot(b, axis), pc = collision_dot(c, axis);
    const float radius = half_size.x * fabsf(axis.x) + half_size.y * fabsf(axis.y) +
                         half_size.z * fabsf(axis.z);
    return fminf(pa, fminf(pb, pc)) <= radius && fmaxf(pa, fmaxf(pb, pc)) >= -radius;
}

bool triangle_intersects_box(Asura_Vector_3 a, Asura_Vector_3 b, Asura_Vector_3 c,
                             const Asura_Bounding_Box& box) {
    const Asura_Vector_3 center{(box.MinX + box.MaxX) * 0.5f, (box.MinY + box.MaxY) * 0.5f,
                                (box.MinZ + box.MaxZ) * 0.5f};
    const Asura_Vector_3 half_size{(box.MaxX - box.MinX) * 0.5f, (box.MaxY - box.MinY) * 0.5f,
                                   (box.MaxZ - box.MinZ) * 0.5f};
    a = collision_sub(a, center);
    b = collision_sub(b, center);
    c = collision_sub(c, center);
    if (fmaxf(a.x, fmaxf(b.x, c.x)) < -half_size.x || fminf(a.x, fminf(b.x, c.x)) > half_size.x ||
        fmaxf(a.y, fmaxf(b.y, c.y)) < -half_size.y || fminf(a.y, fminf(b.y, c.y)) > half_size.y ||
        fmaxf(a.z, fmaxf(b.z, c.z)) < -half_size.z || fminf(a.z, fminf(b.z, c.z)) > half_size.z)
        return false;

    const Asura_Vector_3 edges[3] = {collision_sub(b, a), collision_sub(c, b), collision_sub(a, c)};
    if (!triangle_overlaps_box_axis(a, b, c, collision_cross(edges[0], edges[1]), half_size))
        return false;
    for (Asura_Vector_3 edge : edges) {
        const Asura_Vector_3 axes[3] = {{0, edge.z, -edge.y}, {-edge.z, 0, edge.x}, {edge.y, -edge.x, 0}};
        for (Asura_Vector_3 axis : axes)
            if (!triangle_overlaps_box_axis(a, b, c, axis, half_size))
                return false;
    }
    return true;
}

struct SpawnCollisionProbe {
    const Entity* entity;
    Asura_Bounding_Box bounds;
    uint32_t index;
};

bool validate_spawn_clearance(const Document& doc, const ObjData& obj, const Config& cfg, Error* err) {
    std::vector<SpawnCollisionProbe> probes;
    probes.reserve(doc.entities.size());
    uint32_t spawn_index = 0;
    for (const Entity& entity : doc.entities) {
        if (entity.kind != EntityKind::SpawnPoint)
            continue;
        SpawnCollisionProbe probe{};
        probe.entity = &entity;
        probe.index = spawn_index++;
        probe.bounds = {entity.position.x - kSpawnCollisionHalfWidth,
                        entity.position.x + kSpawnCollisionHalfWidth,
                        entity.position.y - kSpawnCollisionVerticalOffset - kSpawnCollisionHeight,
                        entity.position.y - kSpawnCollisionVerticalOffset,
                        entity.position.z - kSpawnCollisionHalfWidth,
                        entity.position.z + kSpawnCollisionHalfWidth};
        probes.push_back(probe);
    }
    if (probes.empty())
        return true;

    for (uint32_t face_index = 0; face_index < obj.face_count; ++face_index) {
        VertexKey keys[3];
        if (!face_keys(obj, obj.faces[face_index], keys, err))
            return false;
        const Asura_Vector_3 a = transform_vec(obj.positions[keys[0].v], cfg);
        const Asura_Vector_3 b = transform_vec(obj.positions[keys[1].v], cfg);
        const Asura_Vector_3 c = transform_vec(obj.positions[keys[2].v], cfg);
        const Asura_Bounding_Box triangle_bounds{
            fminf(a.x, fminf(b.x, c.x)), fmaxf(a.x, fmaxf(b.x, c.x)),
            fminf(a.y, fminf(b.y, c.y)), fmaxf(a.y, fmaxf(b.y, c.y)),
            fminf(a.z, fminf(b.z, c.z)), fmaxf(a.z, fmaxf(b.z, c.z))};
        for (const SpawnCollisionProbe& probe : probes) {
            if (triangle_bounds.MaxX < probe.bounds.MinX || triangle_bounds.MinX > probe.bounds.MaxX ||
                triangle_bounds.MaxY < probe.bounds.MinY || triangle_bounds.MinY > probe.bounds.MaxY ||
                triangle_bounds.MaxZ < probe.bounds.MinZ || triangle_bounds.MinZ > probe.bounds.MaxZ)
                continue;
            if (!triangle_intersects_box(a, b, c, probe.bounds))
                continue;
            return fail(err,
                        "Spawn '%s' at (%.3f, %.3f, %.3f) clips through environment geometry.",
                        probe.entity->name.c_str(), probe.entity->position.x,
                        probe.entity->position.y, probe.entity->position.z);
        }
    }
    return true;
}

Asura_Vector_3 oriented_box_dimensions(const Entity& entity) {
    return {entity.source_bounds.MaxX - entity.source_bounds.MinX,
            entity.source_bounds.MaxY - entity.source_bounds.MinY,
            entity.source_bounds.MaxZ - entity.source_bounds.MinZ};
}

Asura_Bounding_Box centered_oriented_box_bounds(const Entity& entity) {
    const Asura_Vector_3 size = oriented_box_dimensions(entity);
    return {entity.position.x - size.x * .5f, entity.position.x + size.x * .5f,
            entity.position.y - size.y * .5f, entity.position.y + size.y * .5f,
            entity.position.z - size.z * .5f, entity.position.z + size.z * .5f};
}

bool valid_oriented_box_dimensions(const Entity& entity) {
    const Asura_Vector_3 size = oriented_box_dimensions(entity);
    return isfinite(size.x) && isfinite(size.y) && isfinite(size.z) &&
           size.x > 0.0f && size.y > 0.0f && size.z > 0.0f;
}

void quaternion_matrix(const Asura_Quat& q, float* m) {
    m[0] = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    m[1] = 2.0f * (q.x * q.y - q.z * q.w);
    m[2] = 2.0f * (q.x * q.z + q.y * q.w);
    m[3] = 2.0f * (q.x * q.y + q.z * q.w);
    m[4] = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    m[5] = 2.0f * (q.y * q.z - q.x * q.w);
    m[6] = 2.0f * (q.x * q.z - q.y * q.w);
    m[7] = 2.0f * (q.y * q.z + q.x * q.w);
    m[8] = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
}

bool append_editor_building_volumes(Buffer* out, const Document& doc, Error* err) {
    for (const Entity& entity : doc.entities) {
        if (entity.kind != EntityKind::BuildingVolume && entity.kind != EntityKind::PositionMarker)
            continue;
        if (!valid_oriented_box_dimensions(entity))
            return fail(err, "building volume '%s' has invalid bounds", entity.name.c_str());

        ChunkMark chunk = begin_chunk(out, ASURA_CHUNK_ENTITY, 0, 0, err);
        Asura_Chunk_Entity_PayloadHeader header{};
        header.Guid = entity.guid;
        header.Classification = entity.kind == EntityKind::BuildingVolume
                                    ? SnipeEntityClass_BuildingVolume : SnipeEntityClass_PositionMarker;
        header.m_usPadding = entity.entity_padding;
        std::array<uint8_t, 0x74> body{};
        float matrix[9]{}, transpose[9]{};
        quaternion_matrix(euler_quaternion(entity.rotation), matrix);
        for (uint32_t row = 0; row < 3; ++row)
            for (uint32_t column = 0; column < 3; ++column)
                transpose[row * 3 + column] = matrix[column * 3 + row];
        const Asura_Bounding_Box bounds = centered_oriented_box_bounds(entity);
        memcpy(body.data() + 4, matrix, sizeof(matrix));
        memcpy(body.data() + 0x28, transpose, sizeof(transpose));
        memcpy(body.data() + 0x4c, &bounds, sizeof(bounds));
        memcpy(body.data() + 0x64, &entity.position, sizeof(entity.position));
        memcpy(body.data() + 0x70, &entity.value_u32_a, sizeof(entity.value_u32_a));
        buffer_append(out, &header, sizeof(header), err);
        buffer_append(out, body.data(), body.size(), err);
        if (!end_chunk(out, chunk, err))
            return false;
    }
    return true;
}

bool contains_u32(const std::vector<uint32_t>& values, uint32_t value) {
    for (uint32_t existing : values)
        if (existing == value)
            return true;
    return false;
}

bool contains_string(const std::vector<std::string>& values, const std::string& value) {
    for (const std::string& existing : values)
        if (existing == value)
            return true;
    return false;
}

std::string normalized_resource_path(Str value) {
    std::string result;
    result.reserve(value.size + 1);
    for (uint32_t i = 0; i < value.size; ++i) {
        char c = value.data[i];
        if (c == '/')
            c = '\\';
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
        result.push_back(c);
    }
    return result;
}

bool static_text_references(const ChunkRef& chunk, Str resource_name) {
    if (chunk.cid != ASURA_CHUNK_TEXTURENAMES || chunk.size < sizeof(Asura_Chunk_TextureNames))
        return false;
    std::string resource = normalized_resource_path(resource_name);
    std::string relative = resource;
    constexpr const char* graphics = "\\graphics";
    if (relative.rfind(graphics, 0) == 0)
        relative.erase(0, strlen(graphics));
    while (!relative.empty() && relative.front() == '\\')
        relative.erase(relative.begin());
    const uint8_t* payload = chunk.data + sizeof(Asura_Chunk_Header);
    const uint32_t payload_size = chunk.size - sizeof(Asura_Chunk_Header);
    const uint32_t count = read_u32(payload);
    uint32_t at = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (at >= payload_size)
            return false;
        uint32_t end = at;
        while (end < payload_size && payload[end])
            ++end;
        if (end == payload_size)
            return false;
        std::string name = normalized_resource_path(
            {reinterpret_cast<const char*>(payload + at), end - at});
        while (!name.empty() && name.front() == '\\')
            name.erase(name.begin());
        if (name == relative || name == resource)
            return true;
        const uint64_t next = align_up(static_cast<uint64_t>(end) + 1, 4);
        if (next > payload_size)
            return false;
        at = static_cast<uint32_t>(next);
    }
    return false;
}

bool append_imported_hierarchy_support(Buffer*, const Document&, const ChunkList&, Error*);

bool append_static_object_chunks(Buffer* out, const Document& doc, const ChunkList& source,
                                 const std::vector<uint8_t>& wanted, Error* err) {
    std::unordered_map<uint32_t, const StaticObjectTemplate*> objects, skins;
    for (const auto& object : doc.static_object_templates) {
        if (object.imported) continue;
        bool referenced = object.properties.fields != 0;
        for (const auto& entity : doc.entities)
            referenced |= entity.kind == EntityKind::StaticObject && entity.value_u32_b == object.file_id;
        if (!referenced) continue;
        objects.emplace(object.file_id, &object);
        Snipe_ServerEntity_StaticObject_ChunkDataV0 body{};
        memcpy(&body, object.body.data(), sizeof(body));
        if (body.m_xPhysicalObject.m_uSkinID) {
            auto [entry, inserted] = skins.emplace(body.m_xPhysicalObject.m_uSkinID, &object);
            if (!inserted && object.properties.fields) {
                if (entry->second->properties.fields && entry->second->properties != object.properties)
                    return fail(err, "Objects sharing skin %08X have conflicting properties", body.m_xPhysicalObject.m_uSkinID);
                entry->second = &object;
            }
        }
    }
    // Follow LOD links, retaining each original resource and its geometry.
    bool added = true;
    while (added) {
        added = false;
        for (uint32_t i = 0; i < source.count; ++i) {
            const auto& chunk = source.chunks[i];
            RscfInfo r{};
            if (rscf_info(chunk, &r) && r.type == 0 && r.subtype == ASURA_RESOURCEFILE_TYPE_PC_OBJECT && r.payload_size >= 20) {
                const auto owner = objects.find(read_u32(r.payload));
                const uint64_t at = 20ull + uint64_t(read_u32(r.payload + 8)) * 32 + uint64_t(read_u32(r.payload + 12)) * 2;
                if (owner != objects.end() && chunk.version && at + 8 <= r.payload_size) {
                    const uint32_t next = read_u32(r.payload + at + 4);
                    if (next) added |= objects.emplace(next, owner->second).second;
                }
            } else if (chunk.cid == fourcc('H','S','K','L')) {
                const Str root = padded_string_at(chunk.data, chunk.size, 16);
                if (!root.data) continue;
                const auto owner = skins.find(asura_lower_name_hash(root));
                const Str next = padded_string_at(chunk.data, chunk.size, static_cast<uint32_t>(align_up(17ull + root.size, 4)));
                if (owner != skins.end() && next.data && next.size)
                    added |= skins.emplace(asura_lower_name_hash(next), owner->second).second;
            }
        }
    }
    auto owner_for = [&](const ChunkRef& chunk) -> const StaticObjectTemplate* {
        uint32_t id = 0; bool hierarchy = false;
        RscfInfo r{};
        if (rscf_info(chunk, &r) && r.type == 0) {
            if (r.subtype == ASURA_RESOURCEFILE_TYPE_PC_OBJECT && r.payload_size >= 20) id = read_u32(r.payload);
            else if (r.subtype == ASURA_RESOURCEFILE_TYPE_PC_OBJECTHIERARCHY || r.subtype == ASURA_RESOURCEFILE_TYPE_PC_CHARACTER) {
                id = asura_lower_name_hash(r.name); hierarchy = true;
            } else return nullptr;
        } else if (chunk.cid == ASURA_CHUNK_SHAPE && chunk.size >= 24) id = read_u32(chunk.data + 16);
        else if (chunk.cid == ASURA_CHUNK_HIERARCHY_SKIN) {
            id = asura_lower_name_hash(padded_string_at(chunk.data, chunk.size, 24)); hierarchy = true;
        } else return nullptr;
        const auto& table = hierarchy ? skins : objects;
        const auto found = table.find(id);
        return found == table.end() ? nullptr : found->second;
    };
    auto is_table = [](const ChunkRef& c) {
        return c.cid == ASURA_CHUNK_TEXTURENAMES || c.cid == ASURA_CHUNK_TEXTUREFLAGS || c.cid == ASURA_CHUNK_MATERIAL;
    };
    for (uint32_t begin = 0; begin < source.count;) {
        if (is_table(source.chunks[begin])) {
            if (wanted[begin] && !append_chunk_copy(out, source.chunks[begin], err)) return false;
            ++begin;
            continue;
        }
        uint32_t end = begin;
        while (end < source.count && !is_table(source.chunks[end])) ++end;
        std::vector<std::vector<uint8_t>> replacements(end - begin);
        std::vector<EntityModelMaterial> materials;
        std::unordered_map<uint64_t, uint32_t> remapped;
        uint32_t first_material_edit = end;
        for (uint32_t i = begin; i < end; ++i) {
            if (!wanted[i]) continue;
            const auto& c = source.chunks[i];
            const auto* owner = owner_for(c);
            if (!owner || !owner->properties.fields) continue;
            const auto& p = owner->properties;
            auto& bytes = replacements[i - begin];
            bytes.assign(c.data, c.data + c.size);
            auto material = [&](uint32_t at, uint32_t stride, bool collision) {
                if (!(p.fields & (collision ? 1u : 3u))) return true;
                if (uint64_t(at) + stride > bytes.size()) return fail(err, "Truncated Object material index");
                if (first_material_edit == end) {
                    if (!decode_pc_model_materials(source, i, &materials, err, false)) return false;
                    first_material_edit = i;
                }
                const uint32_t old = stride == 2 ? read_u16(bytes.data() + at) : read_u32(bytes.data() + at);
                const bool missing = old == (stride == 2 ? 0xffffu : UINT32_MAX);
                // SHAP may already contain runtime handles supplied by shared
                // game files. A surface override needs its own collision material,
                // not an interpretation of that handle as a local MTRL ordinal.
                const uint64_t key = (uint64_t(owner->file_id) << 32) | (collision ? UINT32_MAX : old);
                auto found = remapped.find(key);
                uint32_t index;
                if (found == remapped.end()) {
                    if (!collision && !missing && old >= materials.size()) return fail(err, "Object %08X references missing material %u", owner->file_id, old);
                    EntityModelMaterial copy = collision || missing ? EntityModelMaterial{} : materials[old];
                    if (p.fields & 1) copy.surface_type = p.surface_type;
                    if (p.fields & 2) copy.flags = p.blending_flags;
                    index = static_cast<uint32_t>(materials.size());
                    // Collision indices >=1000 are runtime handles in MCP2.
                    if (index >= 1000) return fail(err, "Object material table exceeds the target's collision index range");
                    materials.push_back(std::move(copy));
                    remapped.emplace(key, index);
                } else index = found->second;
                memcpy(bytes.data() + at, &index, stride);
                return true;
            };
            RscfInfo r{};
            if (rscf_info(c, &r)) {
                const uint32_t payload = static_cast<uint32_t>(r.payload - c.data);
                if (r.subtype == ASURA_RESOURCEFILE_TYPE_PC_OBJECT) {
                    if (!material(payload + 16, 4, false)) return false;
                } else {
                    const Str name = padded_string_at(r.payload, r.payload_size, 0);
                    if (!name.data) return fail(err, "Unterminated Object hierarchy name");
                    const uint32_t at = payload + static_cast<uint32_t>(align_up(uint64_t(name.size) + 1, 4));
                    if (uint64_t(at) + 16 > c.size) return fail(err, "Truncated Object hierarchy");
                    if (r.subtype == ASURA_RESOURCEFILE_TYPE_PC_CHARACTER) {
                        if (!material(at + 12, 4, false)) return false;
                    } else {
                        const uint32_t count = read_u32(c.data + at);
                        if (uint64_t(at) + 12 + uint64_t(count) * 20 > c.size) return fail(err, "Truncated Object hierarchy strips");
                        for (uint32_t strip = 0; strip < count; ++strip)
                            if (!material(at + 20 + strip * 20, 4, false)) return false;
                    }
                }
            } else {
                std::vector<ObjectCollisionMesh> meshes;
                if (!object_collision_meshes(c, &meshes, err)) return false;
                for (const auto& mesh : meshes) {
                    if (p.fields & 4) {
                        const uint32_t flags = p.collision_flags;
                        if (!mesh.flags && !mesh.flag_count) return fail(err, "Object %08X has a v0 collision mesh without editable flags", owner->file_id);
                        if (mesh.flags) memcpy(bytes.data() + mesh.flags, &flags, mesh.stride);
                        for (uint32_t f = 0; f < mesh.flag_count; ++f)
                            memcpy(bytes.data() + mesh.flag_table + f * mesh.stride, &flags, mesh.stride);
                    }
                    if (mesh.materials && !material(mesh.materials, mesh.stride, true)) return false;
                    for (uint32_t f = 0; f < mesh.material_count; ++f)
                        if (!material(mesh.material_table + f * mesh.stride, mesh.stride, true)) return false;
                }
            }
        }
        for (uint32_t i = begin; i < end; ++i) {
            if (!wanted[i]) continue;
            if (i == first_material_edit) {
                // Append private material records to this conversion table.
                // Original ordinals keep their meaning for every unedited object.
                const uint32_t count = static_cast<uint32_t>(materials.size());
                std::vector<Str> names(count);
                std::vector<int32_t> textures(count);
                std::vector<uint32_t> flags(count), surfaces(count);
                for (uint32_t m = 0; m < count; ++m) {
                    names[m] = str_from_c(materials[m].texture_name.c_str());
                    textures[m] = names[m].size ? static_cast<int32_t>(m) : -1;
                    flags[m] = materials[m].flags; surfaces[m] = materials[m].surface_type;
                }
                if (!append_text(out, names.data(), count, err)) return false;
                const auto txfl = begin_chunk(out, ASURA_CHUNK_TEXTUREFLAGS, 1, 0, err);
                append_u32(out, count, err);
                for (const auto& m : materials) append_u32(out, m.texture_flags, err);
                if (!end_chunk(out, txfl, err) || !append_mtrl(out, textures.data(), flags.data(), surfaces.data(), count, err)) return false;
            }
            const auto& bytes = replacements[i - begin];
            if (bytes.empty()) { if (!append_chunk_copy(out, source.chunks[i], err)) return false; }
            else if (buffer_append(out, bytes.data(), bytes.size(), err) == ~0ull) return false;
        }
        begin = end;
    }
    return !err->set;
}

bool append_obj_static_object(Buffer* out, const StaticObjectTemplate& object, Error* err) {
    const auto& asset = *object.imported;
    const auto& mesh = asset.mesh;
    auto material = mesh.materials.front();
    if (object.properties.fields & 1) material.surface_type = object.properties.surface_type;
    if (object.properties.fields & 2) material.flags = object.properties.blending_flags;
    const int32_t texture_index = material.texture_bytes.empty() ? -1 : 0;
    if (texture_index == 0) {
        Str name = str_from_c(material.texture_name.c_str());
        const std::string resource_name = "\\graphics" + material.texture_name;
        if (!append_text(out, &name, 1, err) || !append_txfl(out, 1, err) ||
            !append_rscf(out, str_from_c(resource_name.c_str()), ASURA_RESOURCEFILE_TYPE_TEXTURE, 0,
                         material.texture_bytes.data(), static_cast<uint32_t>(material.texture_bytes.size()), err))
            return false;
    }
    if (!append_mtrl(out, &texture_index, &material.flags, &material.surface_type, 1, err)) return false;

    // MCP2 0x49E810: PC_OBJECT v1 is a single triangle strip, 32-byte vertices,
    // a material ordinal, then a LOD distance and lower-strip ID.
    std::vector<uint16_t> indices;
    indices.reserve(mesh.faces.size() * 6);
    for (const auto& face : mesh.faces) {
        const uint16_t a = face[0], b = face[2], c = face[1]; // editor to game winding
        if (!indices.empty()) {
            indices.push_back(indices.back());
            indices.push_back(a);
            if (indices.size() & 1) indices.push_back(a);
        }
        indices.insert(indices.end(), {a, b, c});
    }
    Buffer payload{};
    const uint64_t payload_size = 28 + mesh.vertices.size() * 32 + indices.size() * 2;
    if (!buffer_init(&payload, payload_size, err)) return false;
    append_u32(&payload, object.file_id, err);
    append_u32(&payload, static_cast<uint32_t>(indices.size() - 2), err);
    append_u32(&payload, static_cast<uint32_t>(mesh.vertices.size()), err);
    append_u32(&payload, static_cast<uint32_t>(indices.size()), err);
    append_u32(&payload, 0, err);
    static_assert(sizeof(EntityModelVertex) == 32);
    buffer_append(&payload, mesh.vertices.data(), mesh.vertices.size() * sizeof(EntityModelVertex), err);
    buffer_append(&payload, indices.data(), indices.size() * sizeof(uint16_t), err);
    append_f32(&payload, 0, err);
    append_u32(&payload, 0, err);
    const bool written = !err->set && append_rscf(out, str_from_c(object.resource_name.c_str()),
        ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC, ASURA_RESOURCEFILE_TYPE_PC_OBJECT,
        payload.base, static_cast<uint32_t>(payload.size), err, 1);
    buffer_release(&payload);
    if (!written) return false;

    // MCP1 Asura_CollisionMesh::ReadFromChunkStream and MCP2 0x4367C0 agree
    // on v3: overall u16 flags/material, bounds/radius, vertices and quads
    // (fourth index 0xffff for triangles). Bind material 0 while this MTRL is active.
    const ChunkMark shape = begin_chunk(out, ASURA_CHUNK_SHAPE, 0, 0, err);
    append_u32(out, object.file_id, err);
    append_u32(out, 11, err); // LOS, render record, movement collision (0x47C020)
    const Asura_Bounding_Box bounds{mesh.min.x, mesh.max.x, mesh.min.y, mesh.max.y, mesh.min.z, mesh.max.z};
    float radius = 0;
    for (const auto& vertex : mesh.vertices) {
        const auto& p = vertex.position;
        radius = fmaxf(radius, sqrtf(p.x * p.x + p.y * p.y + p.z * p.z));
    }
    for (int collision = 0; collision < 2; ++collision) {
        if (collision) buffer_append(out, nullptr, 260, err); // target render record
        append_u32(out, 3, err);
        append_u32(out, static_cast<uint32_t>(mesh.vertices.size()), err);
        append_u32(out, static_cast<uint32_t>(mesh.faces.size()), err);
        append_u32(out, 0, err); // no per-polygon flags
        append_u32(out, 0, err); // no per-polygon materials
        append_u16(out, object.properties.fields & 4 ? object.properties.collision_flags : asset.collision_flags, err);
        append_u16(out, 0, err);
        buffer_append(out, &bounds, sizeof(bounds), err);
        append_f32(out, radius, err);
        for (const auto& vertex : mesh.vertices)
            buffer_append(out, &vertex.position, sizeof(vertex.position), err);
        for (const auto& face : mesh.faces) {
            append_u16(out, face[0], err);
            append_u16(out, face[2], err);
            append_u16(out, face[1], err);
            append_u16(out, 0xffff, err);
        }
    }
    return end_chunk(out, shape, err);
}

bool append_static_object_support(Buffer* out, const Document& document,
                                  Error* err) {
    std::vector<uint32_t> required_roots;
    for (const Entity& entity : document.entities) {
        if (entity.kind != EntityKind::StaticObject)
            continue;
        if (!contains_u32(required_roots, entity.value_u32_b))
            required_roots.push_back(entity.value_u32_b);
    }
    if (required_roots.empty())
        return true;

    std::vector<uint32_t> present_objects, present_shapes, present_hierarchies, present_fragments;
    std::vector<std::string> present_textures;
    Arena scratch{};
    if (!arena_init(&scratch, 128 * MiB, err))
        return false;
    std::vector<std::string> donors = document.object_donors;
    if (!document.source_pc_path.empty() && !contains_string(donors, document.source_pc_path))
        donors.push_back(document.source_pc_path);
    for (const std::string& donor_path : donors) {
        bool donor_needed = false;
        for (const StaticObjectTemplate& object : document.static_object_templates)
            donor_needed |= object.donor_path == donor_path && contains_u32(required_roots, object.file_id) &&
                            (!contains_u32(present_objects, object.file_id) ||
                             !contains_u32(present_shapes, object.file_id));
        if (!donor_needed)
            continue;
        ArenaMark mark = arena_mark(&scratch);
        ChunkList donor{};
        if (!parse_chunks(donor_path.c_str(), &donor, &scratch, err)) {
            arena_release(&scratch);
            return false;
        }
        std::vector<uint8_t> wanted(donor.count, 0);
        std::vector<uint32_t> donor_ids;
        for (const StaticObjectTemplate& object : document.static_object_templates)
            if (object.donor_path == donor_path && contains_u32(required_roots, object.file_id) &&
                !contains_u32(donor_ids, object.file_id))
                donor_ids.push_back(object.file_id);

        for (size_t id_index = 0; !err->set && id_index < donor_ids.size(); ++id_index) {
            const uint32_t id = donor_ids[id_index];
            for (uint32_t i = 0; i < donor.count; ++i) {
                RscfInfo resource{};
                if (!rscf_info(donor.chunks[i], &resource) ||
                    resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
                    resource.subtype != ASURA_RESOURCEFILE_TYPE_PC_OBJECT || resource.payload_size < 20 ||
                    read_u32(resource.payload) != id)
                    continue;
                if (!contains_u32(present_objects, id)) {
                    wanted[i] = 1;
                    if (!mark_material_support(donor, i, wanted.data(), err))
                        break;
                }
                const uint32_t vertex_count = read_u32(resource.payload + 8);
                const uint32_t index_count = read_u32(resource.payload + 12);
                const uint64_t lod_at = 20ull + static_cast<uint64_t>(vertex_count) * 32 +
                                        static_cast<uint64_t>(index_count) * 2;
                if (lod_at + 8 <= resource.payload_size) {
                    const uint32_t next_lod = read_u32(resource.payload + lod_at + 4);
                    if (next_lod && !contains_u32(donor_ids, next_lod))
                        donor_ids.push_back(next_lod);
                }
                break;
            }
            if (!contains_u32(present_shapes, id)) {
                for (uint32_t i = 0; i < donor.count; ++i) {
                    const ChunkRef& shape = donor.chunks[i];
                    if (shape.cid != ASURA_CHUNK_SHAPE ||
                        shape.size < sizeof(Asura_Chunk_Header) + 4 ||
                        read_u32(shape.data + sizeof(Asura_Chunk_Header)) != id)
                        continue;
                    wanted[i] = 1;
                    // SHAP's fragment ID is separate from its model/file ID.
                    if (!mark_material_support(donor, i, wanted.data(), err)) break;
                    // MCP2 0x47C020 reads it after the optional LOS mesh and
                    // 260-byte render record. 0x465910 uses FRAG's replacement
                    // entry on death; without it the intact model stays visible.
                    if (shape.version != 0 || shape.size < 24) {
                        fail(err, "Object %08X has an unsupported or truncated SHAP", id);
                        break;
                    }
                    const uint32_t components = read_u32(shape.data + 20);
                    if (components & 4u) {
                        uint64_t at = 24;
                        if (components & 1u) {
                            std::vector<ObjectCollisionMesh> meshes;
                            if (!object_collision_meshes(shape, &meshes, err)) break;
                            at += meshes.front().size;
                        }
                        if (components & 2u) at += 260;
                        if (at + 4 > shape.size) {
                            fail(err, "Object %08X has a truncated SHAP fragment reference", id);
                            break;
                        }
                        const uint32_t fragment_id = read_u32(shape.data + at);
                        for (uint32_t j = 0; fragment_id && j < donor.count; ++j) {
                            const ChunkRef& fragment = donor.chunks[j];
                            if (fragment.cid != ASURA_CHUNK_FRAGMENT || fragment.size < 20 ||
                                read_u32(fragment.data + 16) != fragment_id) continue;
                            // MCP2 0x43CD50 / MCP1 Asura_Chunk_Fragment::Process:
                            // ID, count, then {file ID, position, flags, quantity}.
                            if (fragment.version != 0 || fragment.size < 24 ||
                                24ull + 24ull * read_u32(fragment.data + 20) != fragment.size) {
                                fail(err, "Object %08X has an unsupported or truncated FRAG", id);
                                break;
                            }
                            wanted[j] = 1;
                            for (uint32_t k = 0; k < read_u32(fragment.data + 20); ++k) {
                                const uint32_t child = read_u32(fragment.data + 24 + k * 24);
                                if (child && !contains_u32(donor_ids, child)) donor_ids.push_back(child);
                            }
                            break;
                        }
                    }
                    break;
                }
            }
            // SHPD is keyed by shape ID, not adjacency to SHAP. In level05d,
            // shelving's SHPD precedes its model while SHAP is hundreds of
            // chunks later. MCP2 0x4415A0 reads ID, flags and optional bounds.
            for (uint32_t i = 0; i < donor.count; ++i) {
                const ChunkRef& data = donor.chunks[i];
                if (data.cid == ASURA_CHUNK_SHAPEDATA && data.size >= 24 &&
                    read_u32(data.data + 16) == id)
                    wanted[i] = 1;
            }
        }
        for (uint32_t i = 0; i < donor.count; ++i) {
            RscfInfo resource{};
            if (!rscf_info(donor.chunks[i], &resource) || resource.type != ASURA_RESOURCEFILE_TYPE_TEXTURE)
                continue;
            const std::string normalized = normalized_resource_path(resource.name);
            if (contains_string(present_textures, normalized))
                continue;
            for (uint32_t text_index = 0; text_index < donor.count; ++text_index) {
                if (wanted[text_index] && static_text_references(donor.chunks[text_index], resource.name)) {
                    wanted[i] = 1;
                    break;
                }
            }
        }
        for (uint32_t i = 0; i < donor.count && !err->set; ++i) {
            if (!wanted[i])
                continue;
            RscfInfo resource{};
            if (rscf_info(donor.chunks[i], &resource)) {
                if (resource.type == ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC &&
                    resource.subtype == ASURA_RESOURCEFILE_TYPE_PC_OBJECT && resource.payload_size >= 4) {
                    const uint32_t id = read_u32(resource.payload);
                    if (contains_u32(present_objects, id)) { wanted[i] = 0; continue; }
                    present_objects.push_back(id);
                } else if (resource.type == ASURA_RESOURCEFILE_TYPE_TEXTURE) {
                    const std::string normalized = normalized_resource_path(resource.name);
                    if (contains_string(present_textures, normalized)) { wanted[i] = 0; continue; }
                    present_textures.push_back(normalized);
                }
            } else if (donor.chunks[i].cid == ASURA_CHUNK_SHAPE &&
                       donor.chunks[i].size >= sizeof(Asura_Chunk_Header) + 4) {
                const uint32_t id = read_u32(donor.chunks[i].data + sizeof(Asura_Chunk_Header));
                if (contains_u32(present_shapes, id)) { wanted[i] = 0; continue; }
                present_shapes.push_back(id);
            } else if (donor.chunks[i].cid == ASURA_CHUNK_FRAGMENT) {
                const uint32_t id = read_u32(donor.chunks[i].data + 16);
                if (contains_u32(present_fragments, id)) { wanted[i] = 0; continue; }
                present_fragments.push_back(id);
            }
        }
        if (!err->set) append_static_object_chunks(out, document, donor, wanted, err);
        if (!err->set && append_imported_hierarchy_support(out, document, donor, err)) {
            for (const auto& object : document.static_object_templates) {
                if (object.donor_path != donor_path || !contains_u32(required_roots, object.file_id)) continue;
                Snipe_ServerEntity_StaticObject_ChunkDataV0 body{};
                memcpy(&body, object.body.data(), sizeof(body));
                const uint32_t skin = body.m_xPhysicalObject.m_uSkinID;
                if (!skin) continue;
                for (uint32_t i = 0; i < donor.count; ++i) {
                    RscfInfo resource{};
                    if (rscf_info(donor.chunks[i], &resource) && resource.type == 0 &&
                        (resource.subtype == ASURA_RESOURCEFILE_TYPE_PC_CHARACTER ||
                         resource.subtype == ASURA_RESOURCEFILE_TYPE_PC_OBJECTHIERARCHY) &&
                        asura_lower_name_hash(resource.name) == skin) {
                        present_hierarchies.push_back(object.file_id);
                        break;
                    }
                }
            }
        }
        unmap_file(&donor.file);
        arena_reset(&scratch, mark);
        if (err->set)
            break;
    }
    if (!err->set) {
        for (const auto& object : document.static_object_templates) {
            if (!object.imported || !contains_u32(required_roots, object.file_id)) continue;
            if (!append_obj_static_object(out, object, err)) break;
            present_objects.push_back(object.file_id);
            present_shapes.push_back(object.file_id);
        }
        for (uint32_t id : required_roots) {
            if (contains_u32(present_hierarchies, id)) continue;
            if (!contains_u32(present_objects, id) || !contains_u32(present_shapes, id)) {
                // Imported entities can reference assets supplied by shared
                // game files. Keep that existing reference; a newly selected
                // external donor must still supply its model and collision.
                bool source_reference = false;
                for (const StaticObjectTemplate& object : document.static_object_templates)
                    source_reference |= object.file_id == id && !document.source_pc_path.empty() &&
                                        object.donor_path == document.source_pc_path;
                if (source_reference) continue;
                fail(err, "Object %08X is missing %s from the selected Objects donors", id,
                     !contains_u32(present_objects, id) ? "its model" : "its collision shape");
                break;
            }
        }
    }
    arena_release(&scratch);
    return !err->set;
}

bool append_imported_hierarchy_support(Buffer* out, const Document& doc,
                                       const ChunkList& source, Error* err) {
    std::vector<uint32_t> required, present, required_anims, present_anims;
    std::vector<std::string> textures;
    for (const Entity& entity : doc.entities)
        if ((entity.kind == EntityKind::StaticObject || entity.kind == EntityKind::Pickup) &&
            entity.pickup_skin_id && !contains_u32(required, entity.pickup_skin_id))
            required.push_back(entity.pickup_skin_id);
    if (required.empty()) return true;
    for (const Entity& entity : doc.entities)
        if ((entity.kind == EntityKind::StaticObject || entity.kind == EntityKind::Pickup) &&
            entity.pickup_skin_id && entity.pickup_anim_id && !contains_u32(required_anims, entity.pickup_anim_id))
            required_anims.push_back(entity.pickup_anim_id);
    // Weapon support may already have emitted some of these hierarchies.
    for (uint64_t at = sizeof(kAsuraMagic); at + sizeof(Asura_Chunk_Header) <= out->size;) {
        const uint8_t* bytes = out->base + at;
        const uint32_t size = read_u32(bytes + 4);
        if (size < sizeof(Asura_Chunk_Header) || size > out->size - at)
            return fail(err, "invalid output while collecting hierarchy dependencies");
        const ChunkRef chunk{bytes, size, read_u32(bytes), read_u32(bytes + 8), read_u32(bytes + 12), 0};
        RscfInfo resource{};
        if (rscf_info(chunk, &resource)) {
            if (resource.type == ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC &&
                (resource.subtype == ASURA_RESOURCEFILE_TYPE_PC_OBJECTHIERARCHY ||
                 resource.subtype == ASURA_RESOURCEFILE_TYPE_PC_CHARACTER))
                present.push_back(asura_lower_name_hash(resource.name));
            else if (resource.type == ASURA_RESOURCEFILE_TYPE_TEXTURE)
                textures.push_back(normalized_resource_path(resource.name));
        }
        if (chunk.cid == ASURA_CHUNK_HIERARCHY_COMPRESSEDANIM && chunk.version <= 4) {
            const Str name = padded_string_at(chunk.data, chunk.size, 44);
            if (name.data) present_anims.push_back(asura_lower_name_hash(name));
        }
        at += size;
    }
    std::vector<uint8_t> wanted(source.count);
    // MCP2 0x43D6C0: HSKL names its root skin and LOD skin, followed by
    // optional weighted vertices and a distance. Keep the named LOD meshes.
    for (size_t root = 0; root < required.size(); ++root) {
        for (uint32_t i = 0; i < source.count; ++i) {
            const auto& lod = source.chunks[i];
            if (lod.cid != ASURA_CHUNK_HIERARCHY_SKINLOD || lod.version != 0) continue;
            const auto root_name = padded_string_at(lod.data, lod.size, 16);
            if (!root_name.data || asura_lower_name_hash(root_name) != required[root]) continue;
            const uint32_t name_at = static_cast<uint32_t>(align_up(17ull + root_name.size, 4));
            const auto lod_name = padded_string_at(lod.data, lod.size, name_at);
            if (!lod_name.data) return fail(err, "invalid HSKL name");
            const uint64_t data_at = align_up(static_cast<uint64_t>(name_at) + lod_name.size + 1, 4);
            if (data_at + 8 > lod.size || data_at + 8 + 72ull * read_u32(lod.data + data_at) > lod.size)
                return fail(err, "truncated HSKL data");
            const uint32_t id = asura_lower_name_hash(lod_name);
            if (!contains_u32(present, id)) wanted[i] = 1;
            if (!contains_u32(required, id)) required.push_back(id);
        }
    }
    for (uint32_t i = 0; i < source.count; ++i) {
        RscfInfo resource{};
        if (!rscf_info(source.chunks[i], &resource) ||
            resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
            (resource.subtype != ASURA_RESOURCEFILE_TYPE_PC_OBJECTHIERARCHY &&
             resource.subtype != ASURA_RESOURCEFILE_TYPE_PC_CHARACTER)) continue;
        const uint32_t skin = asura_lower_name_hash(resource.name);
        if (!contains_u32(required, skin) || contains_u32(present, skin)) continue;
        wanted[i] = 1;
        present.push_back(skin);
        if (!mark_material_support(source, i, wanted.data(), err)) return false;
        for (uint32_t scan = i; scan > 0; --scan) {
            const uint32_t h = scan - 1;
            const ChunkRef& chunk = source.chunks[h];
            if (chunk.cid != ASURA_CHUNK_HIERARCHY_SKIN ||
                !str_ieq(padded_string_at(chunk.data, chunk.size, 24), resource.name)) continue;
            wanted[h] = 1;
            if (!mark_material_support(source, h, wanted.data(), err)) return false;
            // Animation dependencies are resolved by name below, not adjacency.
            break;
        }
        for (uint32_t adjunct = 0; adjunct < source.count; ++adjunct) {
            const auto& data = source.chunks[adjunct];
            uint32_t name_at = 0;
            if (data.cid == fourcc('H','S','B','B')) name_at = 16;
            else if (data.cid == fourcc('H','M','P','T') || data.cid == fourcc('H','S','N','D')) name_at = 20;
            if (name_at && str_ieq(padded_string_at(data.data, data.size, name_at), resource.name))
                wanted[adjunct] = 1;
        }
    }
    // MCP2 sub_43D8E0 reads the HCAN name after its 44-byte header. Match
    // the entity's animation ID across the whole stream: 5dlava_rest follows
    // its Character resource, so the old backwards-only skin scan missed it.
    for (uint32_t i = 0; i < source.count; ++i) {
        const ChunkRef& chunk = source.chunks[i];
        if (chunk.cid != ASURA_CHUNK_HIERARCHY_COMPRESSEDANIM || chunk.version > 4) continue;
        const Str name = padded_string_at(chunk.data, chunk.size, 44);
        if (!name.data) continue;
        const uint32_t id = asura_lower_name_hash(name);
        if (contains_u32(required_anims, id) && !contains_u32(present_anims, id)) {
            wanted[i] = 1;
            present_anims.push_back(id);
        }
    }
    for (uint32_t i = 0; i < source.count; ++i) {
        RscfInfo resource{};
        if (!rscf_info(source.chunks[i], &resource) || resource.type != ASURA_RESOURCEFILE_TYPE_TEXTURE ||
            contains_string(textures, normalized_resource_path(resource.name))) continue;
        for (uint32_t j = 0; j < source.count; ++j)
            if (wanted[j] && static_text_references(source.chunks[j], resource.name)) {
                wanted[i] = 1;
                textures.push_back(normalized_resource_path(resource.name));
                break;
            }
    }
    return append_static_object_chunks(out, doc, source, wanted, err);
}

// Convert precisely the renderable triangles decoded by the importer to the
// shared environment builder's input. No source modules, visibility, navigation
// or collision payloads are copied into the new level.
bool make_pc_geometry(const ChunkList& source, ObjData* obj,
                      std::vector<PcEnvironmentMaterialBinding>* materials,
                      Arena* arena, Error* err) {
    RscfInfo resource{};
    Mesh mesh;
    if (!find_pc_environment(source, &resource))
        return fail(err, "the .PC contains no PC environment RSCF");
    if (!decode_pc_environment(resource, &mesh, arena, err) ||
        !pc_environment_material_bindings(source, materials, err))
        return false;
    if (mesh.positions.size() > INT32_MAX || mesh.faces.size() > UINT32_MAX)
        return fail(err, "the PC geometry exceeds the shared builder's limits");
    obj->position_count = obj->normal_count = obj->texcoord_count =
        static_cast<uint32_t>(mesh.positions.size());
    obj->face_count = static_cast<uint32_t>(mesh.faces.size());
    obj->positions = arena_array<Asura_Vector_3>(arena, obj->position_count, err);
    obj->normals = arena_array<Asura_Vector_3>(arena, obj->normal_count, err);
    obj->texcoords = arena_array<Asura_Vector_2>(arena, obj->texcoord_count, err);
    obj->colors = arena_array<uint32_t>(arena, obj->position_count, err);
    obj->has_color = arena_array<uint8_t>(arena, obj->position_count, err);
    obj->faces = arena_array<ObjFace>(arena, obj->face_count, err);
    if (err->set) return false;
    for (uint32_t i = 0; i < obj->position_count; ++i) {
        obj->positions[i] = mesh.positions[i];
        obj->normals[i] = mesh.normals[i];
        obj->texcoords[i] = {mesh.texcoords[i].x, 1.0f - mesh.texcoords[i].y};
        obj->colors[i] = mesh.diffuse_abgr[i];
        obj->has_color[i] = 1;
    }
    // Untextured geometry still needs an explicit material table on re-export.
    if (materials->empty()) materials->resize(1);
    const uint32_t untextured = static_cast<uint32_t>(materials->size());
    std::vector<Str> names(materials->size() + 1);
    for (uint32_t i = 0; i < obj->face_count; ++i) {
        const int32_t original = mesh.face_materials[i];
        const uint32_t material = original < 0 ? untextured
                                   : original >= 1000 ? original - 1000 : original;
        if (original >= 0 && material >= untextured)
            return fail(err, "PC geometry references missing material %u", material);
        if (material == untextured && materials->size() == untextured)
            materials->push_back({});
        if (!names[material].data) {
            char* name = arena_array<char>(arena, 32, err);
            if (!name) return false;
            snprintf(name, 32, "mat_%u", material);
            names[material] = str_from_c(name);
        }
        auto index = [](uint32_t vertex) {
            const int32_t one_based = static_cast<int32_t>(vertex + 1);
            return ObjIndex{one_based, one_based, one_based};
        };
        const auto& face = mesh.faces[i];
        obj->faces[i] = {index(face[0]), index(face[1]), index(face[2]), names[material], i};
    }
    return true;
}

bool append_pc_texture(Buffer* out, const ChunkList& source, Str name, Error* err) {
    if (!name.size) return true;
    for (uint64_t at = sizeof(kAsuraMagic); at + sizeof(Asura_Chunk_Header) <= out->size;) {
        const uint8_t* bytes = out->base + at;
        const uint32_t size = read_u32(bytes + 4);
        if (size < sizeof(Asura_Chunk_Header) || size > out->size - at)
            return fail(err, "invalid output while checking PC textures");
        const ChunkRef chunk{bytes, size, read_u32(bytes), read_u32(bytes + 8), read_u32(bytes + 12), 0};
        RscfInfo existing{};
        if (rscf_info(chunk, &existing) && existing.type == ASURA_RESOURCEFILE_TYPE_TEXTURE &&
            text_name_matches_resource(name, existing.name))
            return true;
        at += size;
    }
    for (uint32_t i = 0; i < source.count; ++i) {
        RscfInfo resource{};
        if (rscf_info(source.chunks[i], &resource) &&
            resource.type == ASURA_RESOURCEFILE_TYPE_TEXTURE &&
            text_name_matches_resource(name, resource.name)) {
            if (resource.payload_size < 4 || memcmp(resource.payload, "DDS ", 4))
                return fail(err, "PC texture '%.*s' is not a DDS payload", name.size, name.data);
            return append_rscf(out, resource.name, resource.type, resource.subtype,
                               resource.payload, resource.payload_size, err);
        }
    }
    // Stock levels may reference textures supplied by the game's shared files.
    return true;
}

bool append_pc_materials(Buffer* out, const ChunkList& source, const EnvView& env,
                         const MaterialMap& map,
                         const std::vector<PcEnvironmentMaterialBinding>& materials,
                         const std::vector<CollisionPolygon>& barriers,
                         Error* err) {
    std::vector<uint8_t> used(materials.size());
    for (uint32_t i = 0; i < env.strip_count; ++i) {
        const int32_t original = env.strips[i].m_iOriginalMaterialIndex;
        if (original < 0) continue;
        const uint32_t ordinal = original >= 1000 ? original - 1000 : original;
        if (ordinal >= materials.size()) return fail(err, "rebuilt PC material index is invalid");
        used[ordinal] = 1;
    }
    for (const CollisionPolygon& face : barriers) {
        if (face.material == 0xffff) continue; // Source collision has no material binding.
        if (face.material >= materials.size())
            return fail(err, "collision barrier material %u is missing", face.material);
        used[face.material] = 1;
    }
    std::vector<Str> names(materials.size());
    std::vector<std::string> emitted;
    for (uint32_t i = 0; i < materials.size(); ++i) {
        if (!used[i]) continue;
        const Str mapped = material_texture_name(map, i);
        names[i] = mapped.size ? mapped : materials[i].texture_name;
        const std::string key = normalized_resource_path(names[i]);
        if (!contains_string(emitted, key)) {
            if (!append_pc_texture(out, source, names[i], err)) return false;
            emitted.push_back(key);
        }
    }
    // MCP2 0x440440 recreates the original-index material conversion array;
    // write this table immediately before Env so object/weapon tables cannot
    // change the meaning of its strips (target consumer at 0x49E6AD).
    const uint32_t count = static_cast<uint32_t>(materials.size());
    ChunkMark text = begin_chunk(out, ASURA_CHUNK_TEXTURENAMES, 3, 0, err);
    append_u32(out, count, err);
    for (Str name : names) append_padded_cstr(out, name, err);
    if (!end_chunk(out, text, err)) return false;
    ChunkMark txfl = begin_chunk(out, ASURA_CHUNK_TEXTUREFLAGS, 1, 0, err);
    append_u32(out, count, err);
    for (uint32_t i = 0; i < count; ++i)
        append_u32(out, used[i] ? materials[i].texture_flags : 0, err);
    if (!end_chunk(out, txfl, err)) return false;
    ChunkMark mtrl = begin_chunk(out, ASURA_CHUNK_MATERIAL, 1, 0, err);
    append_u32(out, count, err);
    for (uint32_t i = 0; i < count; ++i) {
        append_u32(out, names[i].size ? i : UINT32_MAX, err);
        append_u32(out, used[i] ? material_override(map, "transparency_flag_by_material_index",
                                                   i, materials[i].flags) : 0, err);
        append_u32(out, used[i] ? material_override(map, "surface_type_by_material_index",
                                                   i, materials[i].surface_type) : 0, err);
    }
    return end_chunk(out, mtrl, err);
}

bool append_pc_sky_textures(Buffer* out, const ChunkList& source, const Document& doc, Error* err) {
    if (!doc.sky_texture_dir.empty()) return true;
    std::vector<std::string> emitted;
    for (const std::string& path : doc.skybox.texture_paths) {
        const Str name{path.data(), static_cast<uint32_t>(path.size())};
        const std::string key = normalized_resource_path(name);
        if (contains_string(emitted, key)) continue;
        if (!append_pc_texture(out, source, name, err)) return false;
        emitted.push_back(key);
    }
    return true;
}

bool append_pc_sound_resources(Buffer* out, const ChunkList& source, const Sounds& sounds, Error* err) {
    std::vector<uint32_t> emitted;
    for (uint32_t i = 0; i < sounds.count; ++i) {
        const SoundEntry& sound = sounds.items[i];
        if (sound.file || contains_u32(emitted, sound.sound_resource_id)) continue;
        for (uint32_t j = 0; j < source.count; ++j) {
            RscfInfo resource{};
            if (rscf_info(source.chunks[j], &resource) && resource.type == ASURA_RESOURCEFILE_TYPE_SOUND &&
                resource.subtype == sound.sound_resource_id) {
                if (!append_rscf(out, resource.name, resource.type, resource.subtype,
                                 resource.payload, resource.payload_size, err)) return false;
                break;
            }
        }
        emitted.push_back(sound.sound_resource_id);
    }
    return true;
}

bool append_editor_targets(Buffer* out, const ChunkList& source, const Document& doc, Error* err) {
    for (const Entity& entity : doc.entities) {
        if (entity.kind != EntityKind::AssassinationTarget) continue;
        const ChunkRef* record = nullptr;
        for (uint32_t i = 0; i < source.count; ++i) {
            const ChunkRef& chunk = source.chunks[i];
            if (chunk.cid == ASURA_CHUNK_ENTITY && chunk.version == 0 &&
                chunk.size >= sizeof(Asura_Chunk_Entity) + 0x80 &&
                read_u32(chunk.data + 16) == entity.guid &&
                read_u16(chunk.data + 20) == SnipeEntityClass_AssassinationTarget) {
                record = &chunk;
                break;
            }
        }
        if (!record) return fail(err, "Assassination target '%s' has no supported source record", entity.name.c_str());
        // Only this explicitly represented, version-checked entity is retained;
        // do not copy unknown extensions or other source entities around it.
        std::array<uint8_t, 0x80> body{};
        memcpy(body.data(), record->data + sizeof(Asura_Chunk_Entity), body.size());
        // Assassination targets inherit the same static/physical record after
        // their eight-byte class prefix.
        constexpr size_t flags_offset = 8 +
            offsetof(Snipe_ServerEntity_StaticObject_ChunkDataV0, m_uSnipePhysicalFlags);
        const uint32_t flags = read_u32(body.data() + flags_offset) & ~kSnipePhysicalHidden;
        memcpy(body.data() + flags_offset, &flags, sizeof(flags));
        const Asura_Quat orientation = euler_quaternion(entity.rotation);
        memcpy(body.data() + 0x38, &entity.position, sizeof(entity.position));
        memcpy(body.data() + 0x44, &orientation, sizeof(orientation));
        memcpy(body.data() + 0x54, &entity.value_a, sizeof(entity.value_a));
        ChunkMark chunk = begin_chunk(out, ASURA_CHUNK_ENTITY, 0, 0, err);
        buffer_append(out, record->data + sizeof(Asura_Chunk_Header),
                      sizeof(Asura_Chunk_Entity_PayloadHeader), err);
        buffer_append(out, body.data(), body.size(), err);
        if (!end_chunk(out, chunk, err)) return false;
    }
    return true;
}

struct CollisionVertexCell {
    int64_t x, y, z;
    uint32_t material;
    bool operator==(const CollisionVertexCell&) const = default;
};

struct CollisionVertexCellHash {
    size_t operator()(const CollisionVertexCell& cell) const {
        uint64_t h = static_cast<uint64_t>(cell.x) * 0x9e3779b185ebca87ull;
        h ^= static_cast<uint64_t>(cell.y) * 0xc2b2ae3d27d4eb4full;
        h ^= static_cast<uint64_t>(cell.z) * 0x165667b19e3779f9ull;
        return static_cast<size_t>(h ^ (static_cast<uint64_t>(cell.material) * 0x85ebca77));
    }
};

// Match decoded collision properties to visible faces, without copying source
// collision geometry. Quads may be triangulated along either diagonal by the
// renderer. A millimetre tolerance allows for local/world float round-off.
struct PcCollisionFlagLookup {
    std::vector<PcCollisionPolygon> polygons;
    std::unordered_multimap<CollisionVertexCell, uint32_t, CollisionVertexCellHash> cells;

    static CollisionVertexCell cell(Asura_Vector_3 p, uint32_t material) {
        return {static_cast<int64_t>(floor(static_cast<double>(p.x) * 100)),
                static_cast<int64_t>(floor(static_cast<double>(p.y) * 100)),
                static_cast<int64_t>(floor(static_cast<double>(p.z) * 100)), material};
    }
    void build() {
        cells.reserve(polygons.size() * 4);
        for (uint32_t i = 0; i < polygons.size(); ++i)
            for (uint32_t j = 0; j < polygons[i].vertex_count; ++j)
                cells.emplace(cell(polygons[i].vertices[j], polygons[i].material), i);
    }
    static bool vertices_close(Asura_Vector_3 a, Asura_Vector_3 b) {
        return fabsf(a.x - b.x) <= 0.001f && fabsf(a.y - b.y) <= 0.001f && fabsf(a.z - b.z) <= 0.001f;
    }
    static uint32_t resolve(const void* context, uint32_t material,
                            const Asura_Vector_3* corners, uint32_t fallback) {
        const auto& lookup = *static_cast<const PcCollisionFlagLookup*>(context);
        const CollisionVertexCell origin = cell(corners[0], material);
        uint32_t best = 0x10000;
        for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) for (int z = -1; z <= 1; ++z) {
            const auto range = lookup.cells.equal_range({origin.x + x, origin.y + y, origin.z + z, material});
            for (auto it = range.first; it != range.second; ++it) {
                const auto& polygon = lookup.polygons[it->second];
                uint32_t used = 0;
                for (uint32_t corner = 0; corner < 3; ++corner) {
                    bool found = false;
                    for (uint32_t vertex = 0; vertex < polygon.vertex_count; ++vertex) {
                        if (!(used & (1u << vertex)) && vertices_close(corners[corner], polygon.vertices[vertex])) {
                            used |= 1u << vertex; found = true; break;
                        }
                    }
                    if (!found) { used = 0; break; }
                }
                if (used) best = std::min(best, static_cast<uint32_t>(polygon.flags));
            }
        }
        return best <= 0xffff ? best : fallback;
    }
};

bool collect_collision_barriers(const Document& doc, const EnvView& env,
                                std::vector<CollisionPolygon>* output, Error* err) {
    output->clear();
    struct Bounds { Asura_Vector_3 min, max; };
    std::vector<Bounds> module_bounds(env.module_count);
    for (uint32_t module = 0; module < env.module_count; ++module) {
        const uint32_t block_index = env.modules[module].m_uBufferIndex;
        if (block_index >= env.block_count) return fail(err, "collision barrier module is invalid");
        const uint8_t* block = env.blocks[block_index];
        const uint32_t count = read_u32(block);
        Bounds& bounds = module_bounds[module];
        bounds.min = {FLT_MAX, FLT_MAX, FLT_MAX};
        bounds.max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* v = block + 8 + static_cast<uint64_t>(i) * 36;
            const Asura_Vector_3 p{read_f32(v), read_f32(v + 4), read_f32(v + 8)};
            bounds.min.x = fminf(bounds.min.x, p.x); bounds.max.x = fmaxf(bounds.max.x, p.x);
            bounds.min.y = fminf(bounds.min.y, p.y); bounds.max.y = fmaxf(bounds.max.y, p.y);
            bounds.min.z = fminf(bounds.min.z, p.z); bounds.max.z = fmaxf(bounds.max.z, p.z);
        }
    }
    const auto assign_module = [&](CollisionPolygon* face) {
        Asura_Vector_3 center{};
        for (uint32_t i = 0; i < face->vertex_count; ++i) {
            center.x += face->vertices[i].x; center.y += face->vertices[i].y;
            center.z += face->vertices[i].z;
        }
        const float inv = 1.0f / face->vertex_count;
        center.x *= inv; center.y *= inv; center.z *= inv;
        float best = FLT_MAX;
        for (uint32_t module = 0; module < env.module_count; ++module) {
            const Bounds& b = module_bounds[module];
            if (b.min.x == FLT_MAX) continue;
            const float dx = fmaxf(fmaxf(b.min.x - center.x, center.x - b.max.x), 0.0f);
            const float dy = fmaxf(fmaxf(b.min.y - center.y, center.y - b.max.y), 0.0f);
            const float dz = fmaxf(fmaxf(b.min.z - center.z, center.z - b.max.z), 0.0f);
            const float distance = dx * dx + dy * dy + dz * dz;
            if (distance < best) { best = distance; face->module_index = module; }
        }
        return best != FLT_MAX;
    };
    constexpr uint8_t sides[6][4] = {
        {0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4},
        {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}
    };
    for (const Entity& entity : doc.entities) {
        if (entity.kind != EntityKind::CollisionBarrier) continue;
        if (!entity.collision_faces.empty()) {
            for (CollisionPolygon face : entity.collision_faces) {
                if (face.vertex_count < 3 || face.vertex_count > 4 || !assign_module(&face))
                    return fail(err, "imported collision barrier has invalid geometry");
                output->push_back(face);
            }
            continue;
        }
        const Asura_Bounding_Box& b = entity.source_bounds;
        if (!isfinite(b.MinX) || !isfinite(b.MaxX) || !isfinite(b.MinY) ||
            !isfinite(b.MaxY) || !isfinite(b.MinZ) || !isfinite(b.MaxZ) ||
            b.MinX >= b.MaxX || b.MinY >= b.MaxY || b.MinZ >= b.MaxZ)
            return fail(err, "collision barrier '%s' needs positive bounds on all axes", entity.name.c_str());
        Asura_Vector_3 corners[8];
        const Asura_Vector_3 center{(b.MinX + b.MaxX) * .5f,
                                    (b.MinY + b.MaxY) * .5f,
                                    (b.MinZ + b.MaxZ) * .5f};
        float rotation[9]{};
        quaternion_matrix(euler_quaternion(entity.rotation), rotation);
        for (uint32_t i = 0; i < 8; ++i) {
            const Asura_Vector_3 local{(i & 1 ? b.MaxX : b.MinX) - center.x,
                                        (i & 2 ? b.MaxY : b.MinY) - center.y,
                                        (i & 4 ? b.MaxZ : b.MinZ) - center.z};
            corners[i] = {entity.position.x + rotation[0] * local.x + rotation[1] * local.y + rotation[2] * local.z,
                          entity.position.y + rotation[3] * local.x + rotation[4] * local.y + rotation[5] * local.z,
                          entity.position.z + rotation[6] * local.x + rotation[7] * local.y + rotation[8] * local.z};
        }
        for (const auto& side : sides) {
            CollisionPolygon face;
            face.vertex_count = 4;
            face.material = 8;
            face.flags = 0x200;
            for (uint32_t i = 0; i < 4; ++i) face.vertices[i] = corners[side[i]];
            if (!assign_module(&face)) return fail(err, "collision barrier has no environment module");
            output->push_back(face);
        }
    }
    return true;
}

bool pack_document(Document& doc, const char* output_path, std::string* why) {
    if (!normalise_editor_guids(&doc, why))
        return false;
    const bool from_pc = !doc.source_pc_path.empty();
    if (!from_pc && doc.obj_path.empty()) {
        if (why)
            *why = "Open an OBJ or PC level before exporting.";
        return false;
    }
    // Equipped weapons need their resources even without placed pickups.
    if (!from_pc && doc.weapons_donor.empty()) {
        if (why)
            *why = "Choose a Weapons donor .PC before exporting a custom level.";
        return false;
    }
    Error err{};
    Config cfg{};
    if (!initialize_editor_config(from_pc ? doc.source_pc_path.c_str() : doc.obj_path.c_str(), &cfg, &err)) {
        if (why)
            *why = err.message;
        return false;
    }
    cfg.material_map = doc.material_map.empty() ? nullptr : doc.material_map.c_str();
    cfg.texture_dir = doc.texture_dir.empty() ? nullptr : doc.texture_dir.c_str();
    cfg.weapon_from_pc = doc.weapons_donor.empty() ? nullptr : doc.weapons_donor.c_str();
    if (from_pc && !cfg.weapon_from_pc)
        cfg.weapon_from_pc = doc.source_pc_path.c_str();
    if (from_pc) cfg.flip_z = false; // The PC importer has already converted Y to editor space.
    cfg.sky_texture_dir = doc.sky_texture_dir.empty() ? nullptr : doc.sky_texture_dir.c_str();
    for (uint32_t slot = 0; slot < ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT; ++slot)
        cfg.sky_texture_paths[slot] = doc.skybox.texture_paths[slot].empty()
                                           ? nullptr
                                           : doc.skybox.texture_paths[slot].c_str();
    cfg.allow_unknown_materials = doc.material_map.empty();
    for (const Entity& entity : doc.entities)
        if (entity.kind == EntityKind::CollisionBarrier && entity.collision_faces.empty())
            cfg.min_material_count = 9;

    Arena arena{}, scratch{};
    Buffer output{}, env_payload{};
    MappedFile obj_file{};
    ChunkList source{};
    std::vector<PcEnvironmentMaterialBinding> pc_materials;
    std::vector<uint32_t> pc_collision_flags;
    PcCollisionFlagLookup pc_face_collision;
    MaterialMap material_map{};
    ObjData obj{};
    EnvBuild env{};
    EnvView view{};
    std::vector<CollisionPolygon> collision_barriers;
    Sounds sounds{};
    SoundTriggerExport sound_triggers;
    TextureSet textures{};
    ModuleMetric* metrics = nullptr;
    bool ok = arena_init(&arena, cfg.arena_reserve, &err) && arena_init(&scratch, cfg.arena_reserve, &err) &&
              load_material_map(cfg, &material_map, &arena, &err);
    if (ok && from_pc) {
        ok = parse_chunks(doc.source_pc_path.c_str(), &source, &arena, &err) &&
             make_pc_geometry(source, &obj, &pc_materials, &arena, &err);
        // Collision is rebuilt from visible geometry. Retain face/material masks
        // only when the source collision format is understood; an unsupported
        // collision chunk must not prevent exporting the decoded render mesh.
        Error collision_error{};
        if (ok && !pc_environment_collision_flags(source, static_cast<uint32_t>(pc_materials.size()),
                                                   &pc_collision_flags, &collision_error,
                                                   &pc_face_collision.polygons)) {
            pc_collision_flags.assign(pc_materials.size(), 0);
            pc_face_collision.polygons.clear();
        }
        if (ok && !pc_face_collision.polygons.empty()) {
            pc_face_collision.build();
            material_map.collision_flag_context = &pc_face_collision;
            material_map.resolve_collision_flags = PcCollisionFlagLookup::resolve;
        }
        if (ok && pc_materials.size() < cfg.min_material_count)
            pc_materials.resize(cfg.min_material_count);
        material_map.default_collision_flags = pc_collision_flags.data();
        material_map.default_collision_flag_count = static_cast<uint32_t>(pc_collision_flags.size());
    } else if (ok) {
        ok = map_file(cfg.obj, &obj_file, &err) && parse_obj(&obj_file, &obj, &arena, &err);
    }
    if (ok && !from_pc)
        ok = validate_spawn_clearance(doc, obj, cfg, &err);
    if (ok)
        ok = buffer_init(&output, cfg.output_reserve, &err) &&
             build_env(cfg, obj, material_map, &arena, &scratch, &env, &err);
    if (ok) {
        env_payload = env.payload;
        ok = env_view(env_payload, &view, &arena, &err) && view.module_count &&
             view.module_count <= kMaxAabbTreeObjects;
        if (!ok && !err.set)
            fail(&err, "generated environment has an invalid module count");
        if (ok)
            ok = collect_collision_barriers(doc, view, &collision_barriers, &err);
    }
    if (ok) {
        metrics = arena_array<ModuleMetric>(&arena, view.module_count, &err);
        ok = metrics && make_editor_sounds(doc, &sounds, &arena, &err) &&
             prepare_sound_triggers(doc, &sound_triggers, &err);
    }
    if (ok) {
        buffer_append(&output, kAsuraMagic, sizeof(kAsuraMagic), &err);
        ok = append_fnfo(&output, &err) && append_rsfl(&output, &err) &&
             append_weapon_support(&output, cfg, &scratch, &err) &&
             append_static_object_support(&output, doc, &err) &&
             (!from_pc || append_imported_hierarchy_support(&output, doc, source, &err)) &&
             append_sky_resources(&output, cfg, &scratch, &err) &&
             (!from_pc || append_pc_sky_textures(&output, source, doc, &err)) &&
             (from_pc ? append_pc_materials(&output, source, view, material_map, pc_materials,
                                            collision_barriers, &err)
                      : append_textures(&output, cfg, view, material_map, &arena, &scratch, &textures, &err)) &&
             append_rscf(&output, str_from_c(cfg.env_name), ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC,
                         ASURA_RESOURCEFILE_TYPE_PC_ENVIRONMENT, env_payload.base,
                         static_cast<uint32_t>(env_payload.size), &err) &&
             append_sound_resources(&output, sounds, &scratch, &err) &&
             (!from_pc || append_pc_sound_resources(&output, source, sounds, &err)) &&
             append_editor_ambience(&output, doc, &err) &&
             append_editor_lights(&output, doc, &err) &&
             append_phon(&output, sounds, &err) &&
             append_emod(&output, view, view.module_count, cfg, material_map,
                         collision_barriers.data(), static_cast<uint32_t>(collision_barriers.size()),
                         &scratch, metrics, &err) &&
             append_mlin(&output, metrics, view.module_count, &err) &&
             append_mrvb(&output, view.module_count, &err) && append_nav1(&output, view.module_count, &err) &&
             append_sound_entities(&output, sounds, &err) && append_editor_spawnpoints(&output, doc, &err) &&
             append_editor_pickups(&output, doc, &err) && append_editor_static_objects(&output, doc, &err) &&
             append_editor_building_volumes(&output, doc, &err) &&
             append_editor_targets(&output, source, doc, &err) &&
             append_sound_trigger_export(&output, sound_triggers, &err) &&
             append_editor_skybox(&output, doc.skybox, &err) &&
             append_fog(&output, &err) && append_editor_weather(&output, doc, &err) &&
             buffer_append(&output, nullptr, sizeof(Asura_Chunk_Header), &err) != ~0ull &&
             patch_fnfo_file_size(&output, &err);
    }
    // Allow the selected output to overwrite its backing PC after all required
    // data has been consumed. Every later export also rebuilds from Document.
    unmap_file(&source.file);
    if (ok) ok = write_entire_file(output_path, output.base, output.size, &err);
    if (!ok && why)
        *why = err.set ? err.message : "Packing failed.";
    unmap_file(&material_map.file);
    unmap_file(&obj_file);
    buffer_release(&env_payload);
    buffer_release(&output);
    arena_release(&scratch);
    arena_release(&arena);
    return ok;
}


bool pc_environment_material_bindings(const ChunkList& chunks,
                                      std::vector<PcEnvironmentMaterialBinding>* output, Error* err) {
    output->clear();
    RscfInfo environment{};
    if (!find_pc_environment(chunks, &environment))
        return fail(err, "the .PC contains no PC environment RSCF");

    std::vector<Str> texture_names;
    std::vector<uint32_t> texture_flags;
    bool reached_environment = false;
    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        const ChunkRef& chunk = chunks.chunks[chunk_index];
        if (chunk.cid == ASURA_CHUNK_TEXTURENAMES) {
            if (chunk.version > 3)
                return fail(err, "the active PC TEXT chunk uses unsupported version %u", chunk.version);
            if (chunk.size < sizeof(Asura_Chunk_TextureNames))
                return fail(err, "the active PC TEXT chunk is truncated");
            const uint32_t count = read_u32(chunk.data + sizeof(Asura_Chunk_Header));
            if (count > chunk.size - sizeof(Asura_Chunk_TextureNames))
                return fail(err, "the active PC TEXT count exceeds its chunk");
            texture_names.clear();
            texture_names.reserve(count);
            texture_flags.assign(count, 0);
            uint64_t at = sizeof(Asura_Chunk_TextureNames);
            for (uint32_t texture_index = 0; texture_index < count; ++texture_index) {
                if (at > chunk.size)
                    return fail(err, "the active PC TEXT string table is truncated");
                const Str name = padded_string_at(chunk.data, chunk.size, static_cast<uint32_t>(at));
                if (!name.data)
                    return fail(err, "the active PC TEXT string table is unterminated");
                texture_names.push_back(name);
                at = align_up(at + name.size + 1, 4);
            }
            // MCP2 0x441CE0 creates one implicit material per TEXT entry for
            // v0-v2. TEXT v3 only replaces the texture conversion table and
            // relies on a following MTRL chunk.
            if (chunk.version < 3) {
                output->assign(count, {});
                for (uint32_t texture_index = 0; texture_index < count; ++texture_index) {
                    (*output)[texture_index].texture_index = static_cast<int32_t>(texture_index);
                    (*output)[texture_index].texture_name = texture_names[texture_index];
                }
            }
        } else if (chunk.cid == ASURA_CHUNK_TEXTUREFLAGS) {
            if (chunk.version > 1)
                return fail(err, "the active PC TXFL chunk uses unsupported version %u", chunk.version);
            const uint64_t values_at = sizeof(Asura_Chunk_Header) + sizeof(uint32_t);
            if (chunk.size < values_at)
                return fail(err, "the active PC TXFL chunk is truncated");
            const uint32_t count = read_u32(chunk.data + sizeof(Asura_Chunk_Header));
            if (count > (chunk.size - values_at) / sizeof(uint32_t) || count > texture_flags.size())
                return fail(err, "the active PC TXFL table exceeds the active TEXT table");
            for (uint32_t texture_index = 0; texture_index < count; ++texture_index) {
                uint32_t value = read_u32(chunk.data + values_at + texture_index * sizeof(uint32_t));
                if (!chunk.version) {
                    if (texture_index < output->size())
                        (*output)[texture_index].flags |= value & 0xDE87u;
                    value &= 0xFFFF2178u;
                }
                texture_flags[texture_index] |= value;
            }
            for (PcEnvironmentMaterialBinding& binding : *output)
                if (binding.texture_index >= 0 &&
                    static_cast<uint32_t>(binding.texture_index) < texture_flags.size())
                    binding.texture_flags = texture_flags[binding.texture_index];
        } else if (chunk.cid == ASURA_CHUNK_MATERIAL) {
            if (chunk.version > 1)
                return fail(err, "the active PC MTRL chunk uses unsupported version %u", chunk.version);
            if (chunk.size < sizeof(Asura_Chunk_Header) + sizeof(uint32_t))
                return fail(err, "the active PC MTRL chunk is truncated");
            const uint32_t count = read_u32(chunk.data + sizeof(Asura_Chunk_Header));
            const uint32_t stride = chunk.version ? sizeof(Asura_PC_Material_V1) : 8u;
            const uint64_t records_at = sizeof(Asura_Chunk_Header) + sizeof(uint32_t);
            if (count > (chunk.size - records_at) / stride)
                return fail(err, "the active PC MTRL record table is truncated");
            output->assign(count, {});
            for (uint32_t material_index = 0; material_index < count; ++material_index) {
                const uint8_t* record = chunk.data + records_at + static_cast<uint64_t>(material_index) * stride;
                PcEnvironmentMaterialBinding& binding = (*output)[material_index];
                const int32_t texture_index = static_cast<int32_t>(read_u32(record));
                binding.texture_index = texture_index;
                binding.flags = read_u32(record + 4);
                binding.surface_type = chunk.version ? read_u32(record + 8) : 0;
                if (texture_index >= 0 && static_cast<uint32_t>(texture_index) < texture_names.size()) {
                    binding.texture_name = texture_names[texture_index];
                    binding.texture_flags = texture_flags[texture_index];
                }
            }
        }

        RscfInfo resource{};
        if (rscf_info(chunk, &resource) && resource.payload == environment.payload) {
            reached_environment = true;
            break;
        }
    }
    if (!reached_environment)
        return fail(err, "the PC environment resource is absent from the chunk stream");
    return true;
}

namespace {

std::string obj_export_folder(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

std::string obj_export_stem(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    const size_t begin = slash == std::string::npos ? 0 : slash + 1;
    size_t end = path.find_last_of('.');
    if (end == std::string::npos || end < begin)
        end = path.size();
    std::string stem = path.substr(begin, end - begin);
    for (char& c : stem) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!safe)
            c = '_';
    }
    return stem;
}

std::string obj_export_join(const std::string& folder, const std::string& name) {
    return folder.empty() ? name : folder + "\\" + name;
}

std::string obj_texture_identity(Str name) {
    if (!name.data || !name.size)
        return {};
    std::string identity(name.data, name.size);
    for (char& c : identity) {
        if (c == '/')
            c = '\\';
        else if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return identity;
}

std::string obj_texture_stem(Str name) {
    if (!name.data || !name.size)
        return "texture";

    const std::string path(name.data, name.size);
    const size_t slash = path.find_last_of("\\/");
    const size_t begin = slash == std::string::npos ? 0 : slash + 1;
    size_t end = path.find_last_of('.');
    if (end == std::string::npos || end <= begin)
        end = path.size();
    std::string stem = path.substr(begin, end - begin);

    // Preserve the resource stem, replacing only characters Windows cannot use.
    for (char& c : stem) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            c = '_';
    }
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.'))
        stem.pop_back();
    return stem.empty() ? "texture" : stem;
}

std::string obj_texture_filename_key(std::string filename) {
    for (char& c : filename) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return filename;
}

const std::string* obj_extracted_texture_name(
    const std::vector<std::string>& extracted,
    const std::string& source_key) {
    for (size_t i = 0; i + 1 < extracted.size(); i += 2)
        if (extracted[i] == source_key)
            return &extracted[i + 1];
    return nullptr;
}

bool obj_texture_filename_is_used(const std::vector<std::string>& filenames,
                                  const std::string& filename_key) {
    for (const std::string& existing : filenames)
        if (existing == filename_key)
            return true;
    return false;
}

std::string obj_material_name(int32_t material_index) {
    return "mat_" + std::to_string(material_index);
}

bool pc_environment_texture_resource(const ChunkList& chunks, Str texture_name,
                                     RscfInfo* output) {
    if (!texture_name.size)
        return false;
    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        RscfInfo resource{};
        if (rscf_info(chunks.chunks[chunk_index], &resource) &&
            resource.type == ASURA_RESOURCEFILE_TYPE_TEXTURE &&
            text_name_matches_resource(texture_name, resource.name)) {
            *output = resource;
            return true;
        }
    }
    return false;
}

bool write_obj_texture(const std::string& path, const uint8_t* bytes, size_t size,
                       const char* invalid_message, Error* err) {
    if (size < 4 || memcmp(bytes, "DDS ", 4) != 0)
        return fail(err, invalid_message, path.c_str());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return fail(err, "could not create OBJ texture: %s", path.c_str());
    file.write(reinterpret_cast<const char*>(bytes), size);
    if (!file)
        return fail(err, "could not write OBJ texture: %s", path.c_str());
    return true;
}

Asura_Vector_3 obj_static_object_vector(Asura_Vector_3 value, const Asura_Quat& rotation) {
    value = rotate_by_quaternion(value, rotation);
    // PC object geometry and entity transforms are in game coordinates. OBJ
    // authoring uses the same inverse Y/Z conversion as the environment export.
    value.y = -value.y;
    value.z = -value.z;
    return value;
}

Asura_Vector_3 obj_static_object_position(Asura_Vector_3 value, const Entity& entity,
                                          const Asura_Quat& rotation) {
    value = rotate_by_quaternion(value, rotation);
    value.x += entity.position.x;
    value.y += entity.position.y;
    value.z += entity.position.z;
    value.y = -value.y;
    value.z = -value.z;
    return value;
}

Asura_Vector_3 obj_normalized(Asura_Vector_3 value) {
    const float length = sqrtf(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length <= 1.0e-5f)
        return {0, 1, 0};
    const float inverse = 1.0f / length;
    return {value.x * inverse, value.y * inverse, value.z * inverse};
}

} // namespace

bool export_static_object_obj(const Entity& entity, const EntityModel& model,
                              const char* output_path, std::string* why) {
    if (entity.kind != EntityKind::StaticObject || !output_path || !*output_path) {
        if (why)
            *why = "Select one static Object and choose an OBJ output path first.";
        return false;
    }
    if (model.vertices.empty() || model.faces.empty()) {
        if (why)
            *why = "The selected static Object has no loaded model geometry.";
        return false;
    }

    Error err{};
    bool ok = true;
    for (const auto& face : model.faces) {
        for (uint16_t index : face) {
            if (index >= model.vertices.size()) {
                ok = fail(&err, "the selected Object model contains an out-of-range vertex index");
                break;
            }
        }
        if (!ok)
            break;
    }

    std::vector<int32_t> material_indices;
    if (ok) {
        material_indices.reserve(model.faces.size());
        for (size_t face_index = 0; face_index < model.faces.size(); ++face_index) {
            const int32_t material_index = face_index < model.face_materials.size()
                                               ? model.face_materials[face_index]
                                               : -1;
            material_indices.push_back(material_index);
        }
        std::sort(material_indices.begin(), material_indices.end());
        material_indices.erase(std::unique(material_indices.begin(), material_indices.end()),
                               material_indices.end());
    }

    const std::string obj_path = output_path ? output_path : "";
    const std::string folder = obj_export_folder(obj_path);
    const std::string stem = obj_export_stem(obj_path);
    const std::string mtl_name = stem + ".mtl";
    const std::string material_map_name = stem + "_materials.json";
    const std::string texture_folder_name = "textures";
    const std::string mtl_path = obj_export_join(folder, mtl_name);
    const std::string texture_folder = obj_export_join(folder, texture_folder_name);
    if (ok && stem.empty())
        ok = fail(&err, "the OBJ output path has no file name");
    if (ok && !CreateDirectoryA(texture_folder.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        ok = fail(&err, "could not create OBJ texture folder '%s' (win32=%lu)",
                  texture_folder.c_str(), GetLastError());

    uint32_t texture_count = 0;
    uint32_t missing_texture_count = 0;
    std::vector<std::string> extracted_texture_names;
    std::vector<std::string> used_texture_filenames;
    std::ofstream mtl;
    if (ok) {
        mtl.open(mtl_path, std::ios::binary | std::ios::trunc);
        if (!mtl)
            ok = fail(&err, "could not create OBJ material library: %s", mtl_path.c_str());
    }
    if (ok) {
        mtl << "# Sniper Elite 2005 static Object materials\n";
        for (int32_t material_index : material_indices) {
            if (material_index < 0)
                continue;
            const std::string material_name = obj_material_name(material_index);
            mtl << "\nnewmtl " << material_name << "\n"
                << "Ka 1.000000 1.000000 1.000000\n"
                << "Kd 1.000000 1.000000 1.000000\n"
                << "Ks 0.000000 0.000000 0.000000\n"
                << "d 1.000000\n"
                << "illum 1\n";
            if (static_cast<uint32_t>(material_index) >= model.materials.size())
                continue;

            const EntityModelMaterial& material = model.materials[material_index];
            if (material.texture_bytes.empty()) {
                if (!material.texture_name.empty())
                    ++missing_texture_count;
                continue;
            }

            const Str texture_resource_name{material.texture_name.data(),
                                            static_cast<uint32_t>(material.texture_name.size())};
            std::string source_key = obj_texture_identity(texture_resource_name);
            if (source_key.empty())
                source_key = material_name;

            std::string texture_name;
            const std::string* extracted = obj_extracted_texture_name(extracted_texture_names, source_key);
            if (extracted) {
                texture_name = *extracted;
            } else {
                const std::string texture_stem = obj_texture_stem(texture_resource_name);
                texture_name = texture_stem + ".dds";
                uint32_t suffix = 2;
                while (obj_texture_filename_is_used(
                    used_texture_filenames, obj_texture_filename_key(texture_name))) {
                    texture_name = texture_stem + "_" + std::to_string(suffix++) + ".dds";
                }
                const std::string texture_path = obj_export_join(texture_folder, texture_name);
                if (!write_obj_texture(texture_path, material.texture_bytes.data(),
                                       material.texture_bytes.size(),
                                       "object texture resource is not DDS data: %s", &err)) {
                    ok = false;
                    break;
                }
                extracted_texture_names.push_back(source_key);
                extracted_texture_names.push_back(texture_name);
                used_texture_filenames.push_back(obj_texture_filename_key(texture_name));
                ++texture_count;
            }
            const std::string relative_texture_path = texture_folder_name + '/' + texture_name;
            mtl << "map_Kd " << relative_texture_path << "\n";
            if ((material.flags & 0x2u) != 0)
                mtl << "map_d " << relative_texture_path << "\n";
        }
        if (ok && !mtl)
            ok = fail(&err, "could not write OBJ material library: %s", mtl_path.c_str());
    }
    mtl.close();

    if (ok) {
        std::string out = "{\n";
        for (int32_t material_index : material_indices)
            if (material_index >= 0)
                out += "  \"" + obj_material_name(material_index) + "\": " +
                       std::to_string(material_index) + ",\n";
        const char* sections[] = {"texture_by_material_index",
                                  "transparency_flag_by_material_index",
                                  "surface_type_by_material_index"};
        for (size_t section = 0; section < std::size(sections); ++section) {
            out += "  \"" + std::string(sections[section]) + "\": {";
            bool first = true;
            for (int32_t material_index : material_indices) {
                if (material_index < 0 || static_cast<size_t>(material_index) >= model.materials.size())
                    continue;
                const EntityModelMaterial& material = model.materials[material_index];
                out += first ? "\n" : ",\n";
                first = false;
                out += "    \"" + std::to_string(material_index) + "\": ";
                if (section == 0) {
                    std::string source_key = obj_texture_identity(
                        {material.texture_name.data(), static_cast<uint32_t>(material.texture_name.size())});
                    if (source_key.empty())
                        source_key = obj_material_name(material_index);
                    const std::string* extracted = obj_extracted_texture_name(extracted_texture_names, source_key);
                    // Match the DDS filename, including any sanitizing or collision suffix.
                    const std::string& texture_name = extracted ? *extracted : material.texture_name;
                    out += '"';
                    for (unsigned char c : texture_name) {
                        if (c < 32) {
                            char escaped[7];
                            snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned int>(c));
                            out += escaped;
                        } else {
                            if (c == '\\' || c == '"')
                                out += '\\';
                            out += static_cast<char>(c);
                        }
                    }
                    out += '"';
                } else {
                    out += std::to_string(section == 1 ? material.flags : material.surface_type);
                }
            }
            out += "\n  }";
            out += section + 1 == std::size(sections) ? "\n" : ",\n";
        }
        out += "}\n";
        const std::string material_map_path = obj_export_join(folder, material_map_name);
        ok = write_entire_file(material_map_path.c_str(), out.data(), out.size(), &err);
    }

    std::ofstream obj;
    if (ok) {
        obj.open(obj_path, std::ios::binary | std::ios::trunc);
        if (!obj)
            ok = fail(&err, "could not create Wavefront OBJ: %s", obj_path.c_str());
    }
    if (ok) {
        const Asura_Quat rotation = euler_quaternion(entity.rotation);
        obj << "# Sniper Elite 2005 selected static Object\n"
            << "mtllib " << mtl_name << "\n"
            << "o " << stem << "\n" << std::setprecision(9);
        for (const EntityModelVertex& vertex : model.vertices) {
            const Asura_Vector_3 position = obj_static_object_position(vertex.position, entity, rotation);
            obj << "v " << position.x << ' ' << position.y << ' ' << position.z << '\n';
        }
        for (const EntityModelVertex& vertex : model.vertices)
            obj << "vt " << vertex.texcoord.x << ' ' << (1.0f - vertex.texcoord.y) << '\n';
        for (const EntityModelVertex& vertex : model.vertices) {
            const Asura_Vector_3 normal = obj_normalized(obj_static_object_vector(vertex.normal, rotation));
            obj << "vn " << normal.x << ' ' << normal.y << ' ' << normal.z << '\n';
        }
        obj << "s off\n";
        for (int32_t material_index : material_indices) {
            if (material_index >= 0)
                obj << "g " << obj_material_name(material_index) << "\nusemtl "
                    << obj_material_name(material_index) << "\n";
            else
                obj << "g unmaterialed\n";
            for (size_t face_index = 0; face_index < model.faces.size(); ++face_index) {
                const int32_t face_material = face_index < model.face_materials.size()
                                                  ? model.face_materials[face_index]
                                                  : -1;
                if (face_material != material_index)
                    continue;
                const auto& face = model.faces[face_index];
                obj << 'f';
                // The preview decoder reversed winding for its single Y reflection.
                // OBJ applies the inverse Y/Z game conversion, so undo that reversal.
                const uint16_t export_order[] = {face[0], face[2], face[1]};
                for (uint16_t vertex_index : export_order) {
                    const uint64_t obj_index = static_cast<uint64_t>(vertex_index) + 1;
                    obj << ' ' << obj_index << '/' << obj_index << '/' << obj_index;
                }
                obj << '\n';
            }
        }
        if (!obj)
            ok = fail(&err, "could not write Wavefront OBJ: %s", obj_path.c_str());
    }
    obj.close();

    if (ok && why) {
        *why = "Exported selected Object with " + std::to_string(model.vertices.size()) +
               " vertices, " + std::to_string(model.faces.size()) + " triangles, " +
               std::to_string(material_indices.size()) + " material groups, and " +
               std::to_string(texture_count) + " DDS textures to " + texture_folder_name + ".";
        *why += " Material map saved as " + material_map_name + ".";
        if (missing_texture_count)
            *why += " " + std::to_string(missing_texture_count) +
                    " referenced textures were not loaded with the Object model.";
    } else if (!ok && why) {
        *why = err.set ? err.message : "Could not export the selected static Object as Wavefront OBJ.";
    }
    return ok;
}

bool export_pc_environment_obj(const std::string& source_pc_path, const char* output_path,
                               std::string* why) {
    if (source_pc_path.empty() || !output_path || !*output_path) {
        if (why)
            *why = "Open a source .PC level and choose an OBJ output path first.";
        return false;
    }

    Error err{};
    Arena arena{};
    ChunkList chunks{};
    RscfInfo environment{};
    Mesh mesh;
    std::vector<PcEnvironmentMaterialBinding> materials;
    bool ok = arena_init(&arena, 64 * MiB, &err) &&
              parse_chunks(source_pc_path.c_str(), &chunks, &arena, &err) &&
              find_pc_environment(chunks, &environment) &&
              decode_pc_environment(environment, &mesh, &arena, &err) &&
              pc_environment_material_bindings(chunks, &materials, &err);
    if (ok && mesh.face_materials.size() != mesh.faces.size())
        ok = fail(&err, "PC environment face/material tables are inconsistent");
    if (ok && (mesh.normals.size() != mesh.positions.size() ||
               mesh.texcoords.size() != mesh.positions.size() ||
               mesh.diffuse_abgr.size() != mesh.positions.size()))
        ok = fail(&err, "PC environment vertex attributes are inconsistent");

    std::vector<int32_t> material_indices;
    if (ok) {
        material_indices = mesh.face_materials;
        std::sort(material_indices.begin(), material_indices.end());
        material_indices.erase(std::unique(material_indices.begin(), material_indices.end()),
                               material_indices.end());
    }

    const std::string obj_path = output_path ? output_path : "";
    const std::string folder = obj_export_folder(obj_path);
    const std::string stem = obj_export_stem(obj_path);
    const std::string mtl_name = stem + ".mtl";
    const std::string texture_folder_name = "textures";
    const std::string mtl_path = obj_export_join(folder, mtl_name);
    const std::string texture_folder = obj_export_join(folder, texture_folder_name);
    if (ok && stem.empty())
        ok = fail(&err, "the OBJ output path has no file name");
    if (ok && !CreateDirectoryA(texture_folder.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        ok = fail(&err, "could not create OBJ texture folder '%s' (win32=%lu)",
                  texture_folder.c_str(), GetLastError());

    uint32_t texture_count = 0;
    uint32_t missing_texture_count = 0;
    // Alternating source key / generated filename. Keeping this as the already
    // ubiquitous vector<string> avoids a one-off vector<pair<string,string>>
    // allocator/template family for this tiny insertion-ordered cache.
    std::vector<std::string> extracted_texture_names;
    std::vector<std::string> used_texture_filenames;
    std::ofstream mtl;
    if (ok) {
        mtl.open(mtl_path, std::ios::binary | std::ios::trunc);
        if (!mtl)
            ok = fail(&err, "could not create OBJ material library: %s", mtl_path.c_str());
    }
    if (ok) {
        mtl << "# Sniper Elite 2005 Env materials\n";
        for (int32_t material_index : material_indices) {
            const std::string material_name = obj_material_name(material_index);
            mtl << "\nnewmtl " << material_name << "\n"
                << "Ka 1.000000 1.000000 1.000000\n"
                << "Kd 1.000000 1.000000 1.000000\n"
                << "Ks 0.000000 0.000000 0.000000\n"
                << "d 1.000000\n"
                << "illum 1\n";
            if (material_index < 0 || static_cast<uint32_t>(material_index) >= materials.size())
                continue;
            const PcEnvironmentMaterialBinding& material = materials[material_index];
            if (!material.texture_name.size)
                continue;
            RscfInfo texture{};
            if (!pc_environment_texture_resource(chunks, material.texture_name, &texture)) {
                ++missing_texture_count;
                continue;
            }

            std::string source_key = obj_texture_identity(texture.name);
            if (source_key.empty())
                source_key = obj_texture_identity(material.texture_name);
            if (source_key.empty())
                source_key = material_name;

            std::string texture_name;
            const std::string* extracted = obj_extracted_texture_name(extracted_texture_names, source_key);
            if (extracted) {
                texture_name = *extracted;
            } else {
                const Str original_name = texture.name.size ? texture.name : material.texture_name;
                const std::string texture_stem = obj_texture_stem(original_name);
                texture_name = texture_stem + ".dds";
                uint32_t suffix = 2;
                while (obj_texture_filename_is_used(
                    used_texture_filenames, obj_texture_filename_key(texture_name))) {
                    texture_name = texture_stem + "_" + std::to_string(suffix++) + ".dds";
                }

                const std::string texture_path = obj_export_join(texture_folder, texture_name);
                if (!write_obj_texture(texture_path, texture.payload, texture.payload_size,
                                       "environment texture resource is not DDS data: %s", &err)) {
                    ok = false;
                    break;
                }
                extracted_texture_names.push_back(source_key);
                extracted_texture_names.push_back(texture_name);
                used_texture_filenames.push_back(obj_texture_filename_key(texture_name));
                ++texture_count;
            }
            const std::string relative_texture_path =
                texture_folder_name + '/' + texture_name;
            mtl << "map_Kd " << relative_texture_path << "\n";
            if ((material.flags & 0x2u) != 0)
                mtl << "map_d " << relative_texture_path << "\n";
        }
        if (ok && !mtl)
            ok = fail(&err, "could not write OBJ material library: %s", mtl_path.c_str());
    }
    mtl.close();

    std::ofstream obj;
    if (ok) {
        obj.open(obj_path, std::ios::binary | std::ios::trunc);
        if (!obj)
            ok = fail(&err, "could not create Wavefront OBJ: %s", obj_path.c_str());
    }
    if (ok) {
        obj << "# Sniper Elite 2005 PC Env\n"
            << "mtllib " << mtl_name << "\n"
            << "o Env\n" << std::setprecision(9);
        constexpr float inverse_byte = 1.0f / 255.0f;
        for (size_t vertex_index = 0; vertex_index < mesh.positions.size(); ++vertex_index) {
            const Asura_Vector_3& position = mesh.positions[vertex_index];
            const uint32_t diffuse = mesh.diffuse_abgr[vertex_index];
            const float red = ((diffuse >> 16) & 0xff) * inverse_byte;
            const float green = ((diffuse >> 8) & 0xff) * inverse_byte;
            const float blue = (diffuse & 0xff) * inverse_byte;
            // decode_pc_environment has already corrected the target's negative Y.
            // Negate Z here to finish the inverse of the Blender-to-game Y/Z flip.
            obj << "v " << position.x << ' ' << position.y << ' ' << -position.z << ' '
                << red << ' ' << green << ' ' << blue << '\n';
        }
        for (const Asura_Vector_2& texcoord : mesh.texcoords)
            obj << "vt " << texcoord.x << ' ' << (1.0f - texcoord.y) << '\n';
        for (const Asura_Vector_3& normal : mesh.normals)
            obj << "vn " << normal.x << ' ' << normal.y << ' ' << -normal.z << '\n';
        obj << "s off\n";
        for (int32_t material_index : material_indices) {
            const std::string material_name = obj_material_name(material_index);
            obj << "g " << material_name << "\nusemtl " << material_name << "\n";
            for (size_t face_index = 0; face_index < mesh.faces.size(); ++face_index) {
                if (mesh.face_materials[face_index] != material_index)
                    continue;
                const std::array<uint32_t, 3>& face = mesh.faces[face_index];
                obj << 'f';
                // The Z reflection reverses handedness, so restore the winding.
                const uint32_t export_order[] = {face[0], face[2], face[1]};
                for (uint32_t vertex_index : export_order) {
                    const uint64_t obj_index = static_cast<uint64_t>(vertex_index) + 1;
                    obj << ' ' << obj_index << '/' << obj_index << '/' << obj_index;
                }
                obj << '\n';
            }
        }
        if (!obj)
            ok = fail(&err, "could not write Wavefront OBJ: %s", obj_path.c_str());
    }
    obj.close();

    if (ok && why) {
        *why = "Exported " + std::to_string(mesh.positions.size()) + " vertices, " +
               std::to_string(mesh.faces.size()) + " triangles, " +
               std::to_string(material_indices.size()) + " mat_<index> materials, and " +
               std::to_string(texture_count) + " DDS textures to " + texture_folder_name + ".";
        if (missing_texture_count)
            *why += " " + std::to_string(missing_texture_count) +
                    " referenced texture resources were not embedded in the source .PC.";
    } else if (!ok && why) {
        *why = err.set ? err.message : "Could not export the PC environment as Wavefront OBJ.";
    }

    unmap_file(&chunks.file);
    arena_release(&arena);
    return ok;
}

bool advance_pc_padded_string(const ChunkRef& chunk, uint64_t* at, Error* err) {
    if (*at >= chunk.size)
        return fail(err, "the PC EMOD string table is truncated");
    const Str value = padded_string_at(chunk.data, chunk.size, static_cast<uint32_t>(*at));
    if (!value.data)
        return fail(err, "the PC EMOD string table is unterminated");
    const uint64_t padded_size = align_up(static_cast<uint64_t>(value.size) + 1, 4);
    if (padded_size > chunk.size - *at)
        return fail(err, "the PC EMOD string padding exceeds its chunk");
    *at += padded_size;
    return true;
}

bool pc_environment_collision_flags(const ChunkList& chunks, uint32_t material_count,
                                    std::vector<uint32_t>* output, Error* err,
                                    std::vector<PcCollisionPolygon>* polygons) {
    output->assign(material_count, 0);
    if (polygons) polygons->clear();
    const ChunkRef* module_list = nullptr;
    for (uint32_t i = 0; i < chunks.count; ++i) {
        if (chunks.chunks[i].cid == ASURA_CHUNK_ENVIRONMENT_MODULELIST) {
            module_list = &chunks.chunks[i];
            break;
        }
    }
    if (!module_list)
        return fail(err, "the .PC contains no EMOD collision data");

    // SniperElite.exe's Asura_Chunk_Environment_ModuleList::Process
    // (MCP2 0x43B5F0) embeds sized collision meshes starting with EMOD v6.
    if (module_list->version != 6)
        return fail(err, "the PC EMOD collision table uses unsupported version %u",
                    module_list->version);
    if (module_list->size < sizeof(Asura_Chunk_Environment_ModuleList))
        return fail(err, "the PC EMOD collision table is truncated");

    const uint32_t module_count =
        read_u32(module_list->data + offsetof(Asura_Chunk_Environment_ModuleList,
                                              m_iNumberOfModules));
    uint64_t at = sizeof(Asura_Chunk_Environment_ModuleList);
    if (!advance_pc_padded_string(*module_list, &at, err))
        return false;

    // Pack material/flag observations into one 32-bit value. Sorting groups
    // identical observations so the exact material-level mode can be recovered
    // without one hash table allocation per material. The source material is
    // uint16_t, so the upper/lower 16-bit split is lossless.
    std::vector<int32_t> frequencies;
    for (uint32_t module_index = 0; module_index < module_count; ++module_index) {
        if (!advance_pc_padded_string(*module_list, &at, err))
            return false;
        if (at > module_list->size ||
            sizeof(Asura_Chunk_Environment_ModuleList_EntryV6) > module_list->size - at)
            return fail(err, "the PC EMOD module %u is truncated", module_index);

        const uint8_t* entry = module_list->data + at;
        const Asura_Vector_3 translation{read_f32(entry), read_f32(entry + 4), read_f32(entry + 8)};
        const uint32_t collision_size = read_u32(
            entry + offsetof(Asura_Chunk_Environment_ModuleList_EntryV6,
                             m_uCollisionDataSize));
        at += sizeof(Asura_Chunk_Environment_ModuleList_EntryV6);
        if (collision_size > module_list->size - at)
            return fail(err, "the PC EMOD module %u collision mesh is truncated", module_index);
        const uint8_t* collision = module_list->data + at;
        at += collision_size;
        if (!collision_size)
            continue;
        if (collision_size < sizeof(uint32_t))
            return fail(err, "the PC EMOD module %u collision version is truncated", module_index);

        // MCP2's collision-mesh constructor at 0x4367C0 accepts versions 0-3.
        // Only v2 and v3 serialize material IDs, so older meshes cannot
        // contribute to a material-indexed collision_flags table.
        const uint32_t version = read_u32(collision);
        if (version > 3)
            return fail(err, "the PC EMOD module %u uses unsupported collision version %u",
                        module_index, version);
        if (version < 2)
            continue;

        const uint32_t fixed_size = version == 2 ? 56u : 52u;
        if (collision_size < fixed_size)
            return fail(err, "the PC EMOD module %u collision header is truncated", module_index);
        const uint32_t vertex_count = read_u32(collision + 4);
        const uint32_t polygon_count = read_u32(collision + 8);
        const bool has_polygon_flags = read_u32(collision + 12) != 0;
        const bool has_polygon_materials = read_u32(collision + 16) != 0;
        const uint16_t overall_flags = version == 2
                                           ? static_cast<uint16_t>(read_u32(collision + 20))
                                           : read_u16(collision + 20);
        const uint16_t overall_material = version == 2
                                              ? static_cast<uint16_t>(read_u32(collision + 24))
                                              : read_u16(collision + 22);
        const uint32_t item_size = version == 2 ? sizeof(uint32_t) : sizeof(uint16_t);

        uint64_t arrays_at = fixed_size;
        const uint64_t vertex_bytes = static_cast<uint64_t>(vertex_count) * 12;
        const uint64_t polygon_bytes = static_cast<uint64_t>(polygon_count) * 8;
        if (vertex_bytes > collision_size - arrays_at)
            return fail(err, "the PC EMOD module %u collision vertices are truncated", module_index);
        arrays_at += vertex_bytes;
        if (polygon_bytes > collision_size - arrays_at)
            return fail(err, "the PC EMOD module %u collision polygons are truncated", module_index);
        arrays_at += polygon_bytes;

        const uint64_t polygon_array_bytes = static_cast<uint64_t>(polygon_count) * item_size;
        const uint64_t flags_at = arrays_at;
        if (has_polygon_flags) {
            if (polygon_array_bytes > collision_size - arrays_at)
                return fail(err, "the PC EMOD module %u collision flags are truncated", module_index);
            arrays_at += polygon_array_bytes;
        }
        const uint64_t materials_at = arrays_at;
        if (has_polygon_materials && polygon_array_bytes > collision_size - arrays_at)
            return fail(err, "the PC EMOD module %u collision materials are truncated", module_index);

        for (uint32_t polygon = 0; polygon < polygon_count; ++polygon) {
            const uint16_t flags = !has_polygon_flags
                                       ? overall_flags
                                       : version == 2
                                             ? static_cast<uint16_t>(read_u32(
                                                   collision + flags_at +
                                                   static_cast<uint64_t>(polygon) * item_size))
                                             : read_u16(collision + flags_at +
                                                        static_cast<uint64_t>(polygon) * item_size);
            const uint16_t material = !has_polygon_materials
                                          ? overall_material
                                          : version == 2
                                                ? static_cast<uint16_t>(read_u32(
                                                      collision + materials_at +
                                                      static_cast<uint64_t>(polygon) * item_size))
                                                : read_u16(collision + materials_at +
                                                           static_cast<uint64_t>(polygon) * item_size);
            const uint32_t ordinal = material == 0xffff
                                         ? 0xffffu
                                         : (material >= 1000 ? static_cast<uint32_t>(material) - 1000
                                                             : material);
            if (ordinal == 0xffffu || ordinal < material_count) {
                if (ordinal != 0xffffu) {
                    const uint32_t packed = (ordinal << 16) | flags;
                    frequencies.push_back(static_cast<int32_t>(packed));
                }
                if (polygons) {
                    PcCollisionPolygon face;
                    face.material = ordinal; face.flags = flags;
                    face.module_index = module_index;
                    face.polygon_index = polygon;
                    const uint8_t* ids = collision + fixed_size + vertex_bytes + static_cast<uint64_t>(polygon) * 8;
                    for (uint32_t corner = 0; corner < 4; ++corner) {
                        const uint16_t index = read_u16(ids + corner * 2);
                        if (corner == 3 && index == 0xffff) break;
                        if (index >= vertex_count)
                            return fail(err, "the PC EMOD module %u has an invalid collision vertex", module_index);
                        const uint8_t* vertex = collision + fixed_size + static_cast<uint64_t>(index) * 12;
                        Asura_Vector_3 point{read_f32(vertex) + translation.x,
                                             read_f32(vertex + 4) + translation.y,
                                             read_f32(vertex + 8) + translation.z};
                        if (!isfinite(point.x) || !isfinite(point.y) || !isfinite(point.z) ||
                            fabsf(point.x) > 1e12f || fabsf(point.y) > 1e12f || fabsf(point.z) > 1e12f)
                            return fail(err, "the PC EMOD module %u has invalid collision coordinates", module_index);
                        face.vertices[face.vertex_count++] = point;
                    }
                    polygons->push_back(face);
                }
            }
        }
    }
    if (at != module_list->size)
        return fail(err, "the PC EMOD collision table has %llu trailing bytes",
                    static_cast<unsigned long long>(module_list->size - at));

    // Per-face records preserve matching visible geometry's masks. Material
    // modes remain a fallback for render faces absent from source collision,
    // and for the material-map export. Prefer the lower mask on a tie.
    std::sort(frequencies.begin(), frequencies.end());
    size_t sample = 0;
    uint32_t current_material = 0xffffffffu;
    uint32_t best_count = 0;
    uint16_t best_flags = 0;
    while (sample < frequencies.size()) {
        const uint32_t packed = static_cast<uint32_t>(frequencies[sample]);
        const uint32_t material = packed >> 16;
        const uint16_t flags = static_cast<uint16_t>(packed);
        size_t next = sample + 1;
        while (next < frequencies.size() && frequencies[next] == frequencies[sample])
            ++next;
        const uint32_t count = static_cast<uint32_t>(next - sample);
        if (material != current_material) {
            if (current_material != 0xffffffffu)
                (*output)[current_material] = best_flags;
            current_material = material;
            best_count = 0;
            best_flags = 0;
        }
        if (count > best_count || (count == best_count && flags < best_flags)) {
            best_count = count;
            best_flags = flags;
        }
        sample = next;
    }
    if (current_material != 0xffffffffu)
        (*output)[current_material] = best_flags;
    return true;
}

} // namespace editor
