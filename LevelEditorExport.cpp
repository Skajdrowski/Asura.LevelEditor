#include "LevelEditorInternal.h"

#include <iomanip>
#include <unordered_map>

using namespace asura;
using namespace asura::level;

namespace editor {

bool append_editor_lights(Buffer* out, const Document& doc, Error* err) {
    uint32_t count = 0;
    for (const Entity& e : doc.entities)
        count += e.kind == EntityKind::Light;
    if (!count)
        return true;
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
    if (skybox.chunk_version < 6 || skybox.chunk_version > 7)
        return fail(err, "SKYB chunk version must be 6 or 7");
    if (skybox.chunk_version < 7 && skybox.right_texture_is_left_upside_down)
        return fail(err, "the right-face compatibility flag requires SKYB version 7");
    if (!isfinite(skybox.red) || !isfinite(skybox.green) || !isfinite(skybox.blue) ||
        !isfinite(skybox.orientation_radians))
        return fail(err, "SKYB colour and orientation values must be finite");
    ChunkMark chunk = begin_chunk(out, ASURA_CHUNK_SKYBOX, skybox.chunk_version, 0, err);
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
    const size_t flag_bytes = skybox.chunk_version == 7 ? sizeof(flags) : sizeof(flags) - sizeof(uint32_t);
    return buffer_append(out, &flags, flag_bytes, err) != ~0ull && end_chunk(out, chunk, err);
}

bool append_editor_weather(Buffer* out, const Document& document, Error* err) {
    const uint64_t start = out->size;
    if (!append_wthr(out, err))
        return false;
    const uint8_t enabled[2]{document.rain_enabled ? 1u : 0u,
                             document.rain_enabled ? 1u : 0u};
    return buffer_patch(out, start + 21, enabled, sizeof(enabled), err);
}

bool append_editor_weather_copy(Buffer* out, const ChunkRef& chunk, const Document& document,
                                Error* err) {
    if (chunk.size <= 22)
        return fail(err, "the source WTHR rain state is truncated");
    const uint64_t start = out->size;
    if (!append_chunk_copy(out, chunk, err))
        return false;
    // SniperElite.exe's WTHR v5/v6 reader loads the adjacent bytes at +21 and
    // +22 into the two runtime rain gates. Retail rainy levels set both and
    // retail dry levels clear both. The byte at +20 is a separate legacy
    // weather flag (it is set in some dry levels), so preserve it verbatim.
    const uint8_t enabled[2]{document.rain_enabled ? 1u : 0u,
                             document.rain_enabled ? 1u : 0u};
    return buffer_patch(out, start + 21, enabled, sizeof(enabled), err);
}

// AEPR v0 stores the extended rain system's two runtime enable bits in the
// low bits of the dword at chunk +0x1c. Retail rainy levels set both bits,
// while dry levels clear both. WTHR controls wet-material response, but an
// imported level's preserved AEPR overrides whether its rain particles run.
constexpr uint32_t kExtendedRainFlagsOffset = 0x1cu;
constexpr uint32_t kExtendedRainEnabledMask = 0x3u;
constexpr uint32_t kStreamingBackgroundSoundDefaultVolumeOffset = 0x18u;
constexpr uint32_t kStreamingBackgroundSoundDefaultPathOffset = 0x1cu;

bool append_editor_extended_rain_copy(Buffer* out, const ChunkRef& chunk,
                                      const Document& document, Error* err) {
    if (chunk.version != 0 || chunk.size < kExtendedRainFlagsOffset + sizeof(uint32_t))
        return fail(err, "the source AEPR rain flags are truncated or unsupported");
    const uint64_t start = out->size;
    if (!append_chunk_copy(out, chunk, err))
        return false;
    uint32_t flags = read_u32(chunk.data + kExtendedRainFlagsOffset);
    flags = (flags & ~kExtendedRainEnabledMask) |
            (document.rain_enabled ? kExtendedRainEnabledMask : 0u);
    return buffer_patch(out, start + kExtendedRainFlagsOffset, &flags, sizeof(flags), err);
}

bool source_ambience_info(const ChunkRef& chunk, std::string* path, float* volume,
                          uint32_t* tail_offset, Error* err) {
    if (chunk.cid != ASURA_CHUNK_STREAMINGBACKGROUNDSOUND || chunk.version > 1 ||
        chunk.size <= kStreamingBackgroundSoundDefaultPathOffset)
        return fail(err, "the source SBSN default sound is truncated or unsupported");
    const Str source_path = padded_string_at(chunk.data, chunk.size,
                                             kStreamingBackgroundSoundDefaultPathOffset);
    if (!source_path.data)
        return fail(err, "the source SBSN default sound path is truncated");
    const uint64_t tail = align_up(
        static_cast<uint64_t>(kStreamingBackgroundSoundDefaultPathOffset) + source_path.size + 1, 4);
    if (tail > chunk.size)
        return fail(err, "the source SBSN default sound path is invalid");
    path->assign(source_path.data, source_path.size);
    memcpy(volume, chunk.data + kStreamingBackgroundSoundDefaultVolumeOffset, sizeof(*volume));
    if (tail_offset)
        *tail_offset = static_cast<uint32_t>(tail);
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
    ChunkMark chunk = begin_chunk(out, ASURA_CHUNK_STREAMINGBACKGROUNDSOUND, 1, 0, err);
    append_u32(out, 0, err); // regional sound count
    append_u32(out, 0, err); // SBSN flags (unused by the 2005 target reader)
    append_f32(out, document.ambient_volume, err);
    if (!append_padded_cstr(out,
                            {document.ambient_stream_path.data(),
                             static_cast<uint32_t>(document.ambient_stream_path.size())},
                            err))
        return false;
    return end_chunk(out, chunk, err);
}

bool append_editor_ambience_copy(Buffer* out, const ChunkRef& chunk,
                                 const Document& document, Error* err) {
    if (!valid_ambience_settings(document, err))
        return false;
    std::string source_path;
    float source_volume = 0.0f;
    uint32_t tail_offset = 0;
    if (!source_ambience_info(chunk, &source_path, &source_volume, &tail_offset, err))
        return false;
    if (source_path == document.ambient_stream_path &&
        memcmp(&source_volume, &document.ambient_volume, sizeof(source_volume)) == 0)
        return append_chunk_copy(out, chunk, err);

    const uint64_t start = out->size;
    if (buffer_append(out, chunk.data, kStreamingBackgroundSoundDefaultPathOffset, err) == ~0ull ||
        !append_padded_cstr(out,
                            {document.ambient_stream_path.data(),
                             static_cast<uint32_t>(document.ambient_stream_path.size())},
                            err) ||
        buffer_append(out, chunk.data + tail_offset, chunk.size - tail_offset, err) == ~0ull)
        return false;
    const uint64_t rebuilt_size = out->size - start;
    if (rebuilt_size > UINT32_MAX)
        return fail(err, "the edited SBSN chunk is too large");
    const uint32_t rebuilt_size_32 = static_cast<uint32_t>(rebuilt_size);
    return buffer_patch(out, start + offsetof(Asura_Chunk_Header, Size), &rebuilt_size_32,
                        sizeof(rebuilt_size_32), err) &&
           buffer_patch(out, start + kStreamingBackgroundSoundDefaultVolumeOffset,
                        &document.ambient_volume, sizeof(document.ambient_volume), err);
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
            s.emit_enti = e.sound_has_controller;
            s.active = e.sound_controller_active;
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
            s.active = true;
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
    memcpy(body->data(), &wire, sizeof(wire));
}

bool append_editor_pickups(Buffer* out, const Document& doc, Error* err) {
    const size_t source_dot = doc.source_pc_path.find_last_of('.');
    const bool ps2_source = source_dot != std::string::npos &&
                            _stricmp(doc.source_pc_path.c_str() + source_dot, ".ps2") == 0;
    for (const Entity& entity : doc.entities) {
        if (entity.kind != EntityKind::Pickup || (entity.source_entity_record && !ps2_source))
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
    memcpy(body->data(), &wire, sizeof(wire));
}

bool append_editor_static_objects(Buffer* out, const Document& doc, Error* err) {
    const size_t source_dot = doc.source_pc_path.find_last_of('.');
    const bool ps2_source = source_dot != std::string::npos &&
                            _stricmp(doc.source_pc_path.c_str() + source_dot, ".ps2") == 0;
    for (const Entity& entity : doc.entities) {
        if (entity.kind != EntityKind::StaticObject || (entity.source_entity_record && !ps2_source))
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

bool editable_pc_entity_chunk(const ChunkRef& chunk) {
    if (chunk.cid != ASURA_CHUNK_ENTITY || chunk.size < sizeof(Asura_Chunk_Entity))
        return false;
    const uint16_t classification = read_u16(
        chunk.data + sizeof(Asura_Chunk_Header) + offsetof(Asura_Chunk_Entity_PayloadHeader, Classification));
    return classification == SnipeEntityClass_SpawnPoint || classification == AsuraEntityClass_SoundController;
}

bool nearly_equal(float a, float b) {
    return fabsf(a - b) <= 1.0e-5f;
}

bool nearly_equal(const Asura_Vector_3& a, const Asura_Vector_3& b) {
    return nearly_equal(a.x, b.x) && nearly_equal(a.y, b.y) && nearly_equal(a.z, b.z);
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

bool nearly_equal_rotation(const Asura_Vector_3& a, const Asura_Vector_3& b) {
    auto equal_angle = [](float x, float y) {
        float difference = fmodf(fabsf(x - y), 360.0f);
        difference = fminf(difference, 360.0f - difference);
        return difference <= 1.0e-3f;
    };
    return equal_angle(a.x, b.x) && equal_angle(a.y, b.y) && equal_angle(a.z, b.z);
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

const Entity* find_source_entity(const Document& doc, uint32_t guid, uint16_t classification) {
    for (const Entity& entity : doc.entities)
        if (entity.source_entity_record && entity.guid == guid &&
            entity.source_entity_classification == classification)
            return &entity;
    return nullptr;
}

bool append_source_entity_copy(Buffer* out, const ChunkRef& chunk, const Document& doc, Error* err) {
    if (chunk.cid != ASURA_CHUNK_ENTITY || chunk.size < sizeof(Asura_Chunk_Entity))
        return append_chunk_copy(out, chunk, err);
    const uint8_t* payload = chunk.data + sizeof(Asura_Chunk_Header);
    const uint32_t guid = read_u32(payload);
    const uint16_t classification = read_u16(
        payload + offsetof(Asura_Chunk_Entity_PayloadHeader, Classification));
    const Entity* entity = find_source_entity(doc, guid, classification);
    if (!entity) {
        if (classification == SnipeEntityClass_Pickup && doc.source_pickup_inventory_complete)
            return true;
        if (classification == SnipeEntityClass_StaticObject && doc.source_static_object_inventory_complete)
            return true;
        return append_chunk_copy(out, chunk, err);
    }

    const uint8_t* body = payload + sizeof(Asura_Chunk_Entity_PayloadHeader);
    if (classification == SnipeEntityClass_Pickup && entity->pickup_has_template) {
        if (chunk.size < sizeof(Asura_Chunk_Entity) + kPickupBodySize)
            return fail(err, "source physical-object ENTI is truncated");
        Asura_Vector_3 source_position{};
        Asura_Quat source_orientation{};
        memcpy(&source_position, body + 0x4c, sizeof(source_position));
        memcpy(&source_orientation, body + 0x58, sizeof(source_orientation));
        const bool template_changed = memcmp(entity->pickup_body.data(), body, entity->pickup_body.size()) != 0 ||
                                      entity->value_u32_a != read_u32(body + 0x10) ||
                                      entity->value_u32_b != read_u32(body + 0x6c) ||
                                      entity->pickup_skin_id != read_u32(body + 0x70) ||
                                      entity->pickup_anim_id != read_u32(body + 0x74) ||
                                      entity->pickup_anim_file_id != read_u32(body + 0x78);
        if (!template_changed && nearly_equal(entity->position, source_position) &&
            nearly_equal_rotation(entity->rotation, quaternion_euler(source_orientation)))
            return append_chunk_copy(out, chunk, err);
        std::vector<uint8_t> patched(chunk.data, chunk.data + chunk.size);
        std::array<uint8_t, kPickupBodySize> patched_body{};
        make_pickup_body(*entity, &patched_body);
        memcpy(patched.data() + sizeof(Asura_Chunk_Entity), patched_body.data(), patched_body.size());
        return buffer_append(out, patched.data(), patched.size(), err) != ~0ull;
    }
    if (classification == SnipeEntityClass_StaticObject && entity->static_object_has_template) {
        if (chunk.size < sizeof(Asura_Chunk_Entity) + kStaticObjectBodySize)
            return fail(err, "source static-object ENTI is truncated");
        Asura_Vector_3 source_position{};
        Asura_Quat source_orientation{};
        memcpy(&source_position, body + 0x30, sizeof(source_position));
        memcpy(&source_orientation, body + 0x3c, sizeof(source_orientation));
        const bool template_changed =
            memcmp(entity->static_object_body.data(), body, entity->static_object_body.size()) != 0 ||
            entity->value_u32_b != read_u32(body + 0x50) ||
            entity->pickup_skin_id != read_u32(body + 0x54) ||
            entity->pickup_anim_id != read_u32(body + 0x58) ||
            entity->pickup_anim_file_id != read_u32(body + 0x5c);
        if (!template_changed && nearly_equal(entity->position, source_position) &&
            nearly_equal_rotation(entity->rotation, quaternion_euler(source_orientation)))
            return append_chunk_copy(out, chunk, err);
        std::vector<uint8_t> patched(chunk.data, chunk.data + chunk.size);
        std::array<uint8_t, kStaticObjectBodySize> patched_body{};
        make_static_object_body(*entity, &patched_body);
        memcpy(patched.data() + sizeof(Asura_Chunk_Entity), patched_body.data(), patched_body.size());
        return buffer_append(out, patched.data(), patched.size(), err) != ~0ull;
    }

    const bool oriented_box = classification == SnipeEntityClass_PositionMarker ||
                              classification == SnipeEntityClass_BuildingVolume;
    uint32_t position_offset = 0, orientation_offset = 0;
    if (classification == SnipeEntityClass_StaticObject) {
        position_offset = 0x30;
        orientation_offset = 0x3c;
    } else if (classification == SnipeEntityClass_Pickup) {
        position_offset = 0x4c;
        orientation_offset = 0x58;
    } else if (classification == SnipeEntityClass_AssassinationTarget) {
        position_offset = 0x38;
        orientation_offset = 0x44;
    } else if (oriented_box) {
        position_offset = 0x64;
    } else {
        return append_chunk_copy(out, chunk, err);
    }
    if (sizeof(Asura_Chunk_Entity) + position_offset + sizeof(Asura_Vector_3) > chunk.size)
        return fail(err, "source ENTI 0x%04X is truncated", classification);

    Asura_Vector_3 source_position{};
    memcpy(&source_position, body + position_offset, sizeof(source_position));
    Asura_Vector_3 source_rotation{};
    if (oriented_box) {
        if (sizeof(Asura_Chunk_Entity) + 0x70 + sizeof(uint32_t) > chunk.size)
            return fail(err, "source oriented-box ENTI 0x%04X is truncated", classification);
        float source_matrix[9]{};
        memcpy(source_matrix, body + 4, sizeof(source_matrix));
        source_rotation = matrix_euler(source_matrix);
    } else {
        if (sizeof(Asura_Chunk_Entity) + orientation_offset + sizeof(Asura_Quat) > chunk.size)
            return fail(err, "source physical-object ENTI is truncated");
        Asura_Quat source_orientation{};
        memcpy(&source_orientation, body + orientation_offset, sizeof(source_orientation));
        source_rotation = quaternion_euler(source_orientation);
    }

    const bool position_changed = !nearly_equal(entity->position, source_position);
    const bool rotation_changed = !nearly_equal_rotation(entity->rotation, source_rotation);
    bool bounds_changed = false, flags_changed = false;
    Asura_Bounding_Box source_bounds{}, desired_bounds{};
    if (oriented_box) {
        memcpy(&source_bounds, body + 0x4c, sizeof(source_bounds));
        if (!valid_oriented_box_dimensions(*entity))
            return fail(err, "oriented-box ENTI 0x%04X has invalid bounds", classification);
        const Asura_Vector_3 source_size{source_bounds.MaxX - source_bounds.MinX,
                                         source_bounds.MaxY - source_bounds.MinY,
                                         source_bounds.MaxZ - source_bounds.MinZ};
        const Asura_Vector_3 edited_size = oriented_box_dimensions(*entity);
        bounds_changed = position_changed || !nearly_equal(source_size, edited_size);
        desired_bounds = centered_oriented_box_bounds(*entity);
        flags_changed = entity->value_u32_a != read_u32(body + 0x70);
    }
    if (!position_changed && !rotation_changed && !bounds_changed && !flags_changed)
        return append_chunk_copy(out, chunk, err);

    std::vector<uint8_t> patched(chunk.data, chunk.data + chunk.size);
    uint8_t* patched_body = patched.data() + sizeof(Asura_Chunk_Entity);
    if (bounds_changed)
        memcpy(patched_body + 0x4c, &desired_bounds, sizeof(desired_bounds));
    if (position_changed || bounds_changed)
        memcpy(patched_body + position_offset, &entity->position, sizeof(entity->position));
    if (rotation_changed) {
        const Asura_Quat orientation = euler_quaternion(entity->rotation);
        if (oriented_box) {
            float matrix[9]{}, transpose[9]{};
            quaternion_matrix(orientation, matrix);
            for (uint32_t row = 0; row < 3; ++row)
                for (uint32_t column = 0; column < 3; ++column)
                    transpose[row * 3 + column] = matrix[column * 3 + row];
            memcpy(patched_body + 4, matrix, sizeof(matrix));
            memcpy(patched_body + 0x28, transpose, sizeof(transpose));
        } else {
            memcpy(patched_body + orientation_offset, &orientation, sizeof(orientation));
        }
    }
    if (flags_changed)
        memcpy(patched_body + 0x70, &entity->value_u32_a, sizeof(entity->value_u32_a));
    return buffer_append(out, patched.data(), patched.size(), err) != ~0ull;
}

bool append_editor_building_volumes(Buffer* out, const Document& doc, Error* err) {
    for (const Entity& entity : doc.entities) {
        if (entity.kind != EntityKind::BuildingVolume || entity.source_entity_record)
            continue;
        if (!valid_oriented_box_dimensions(entity))
            return fail(err, "building volume '%s' has invalid bounds", entity.name.c_str());

        ChunkMark chunk = begin_chunk(out, ASURA_CHUNK_ENTITY, 0, 0, err);
        Asura_Chunk_Entity_PayloadHeader header{};
        header.Guid = entity.guid;
        header.Classification = SnipeEntityClass_BuildingVolume;
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

bool replaced_pc_sound_resource(const Document& doc, const RscfInfo& resource) {
    if (resource.type != ASURA_RESOURCEFILE_TYPE_SOUND)
        return false;
    for (const Entity& entity : doc.entities)
        if (entity.kind == EntityKind::Sound && entity.sound_source_record && !entity.sound_file.empty() &&
            entity.sound_phonon.m_uSoundResourceID == resource.subtype)
            return true;
    return false;
}

bool append_pc_material_map_override(Buffer* out, const ChunkList& source,
                                     const char* material_map_path, Arena* arena, Error* err);
bool pc_environment_material_chunk_indices(const ChunkList& source, uint32_t* text_chunk_index,
                                           uint32_t* material_chunk_index, Error* err);
bool append_ps2_static_object_support(Buffer* out, const Document& document,
                                      const ChunkList& source,
                                      std::vector<uint32_t>* object_ids,
                                      std::vector<uint32_t>* shape_ids, Error* err);

bool contains_u32(const std::vector<uint32_t>& values, uint32_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
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

bool append_static_object_support(Buffer* out, const Document& document,
                                  const ChunkList* existing,
                                  const std::vector<uint32_t>* provided_objects,
                                  const std::vector<uint32_t>* provided_shapes, Error* err) {
    std::vector<uint32_t> required_roots;
    for (const Entity& entity : document.entities) {
        if (entity.kind != EntityKind::StaticObject)
            continue;
        bool needs_donor = !entity.source_entity_record;
        if (entity.source_entity_record) {
            needs_donor = false;
            for (const StaticObjectTemplate& object : document.static_object_templates)
                if (object.file_id == entity.value_u32_b && !object.donor_path.empty() &&
                    object.donor_path != document.source_pc_path) {
                    needs_donor = true;
                    break;
                }
        }
        if (needs_donor && !contains_u32(required_roots, entity.value_u32_b))
            required_roots.push_back(entity.value_u32_b);
    }
    if (required_roots.empty())
        return true;

    std::vector<uint32_t> present_objects, present_shapes;
    std::vector<std::string> present_textures;
    if (provided_objects)
        present_objects = *provided_objects;
    if (provided_shapes)
        present_shapes = *provided_shapes;
    if (existing) {
        for (uint32_t i = 0; i < existing->count; ++i) {
            const ChunkRef& chunk = existing->chunks[i];
            RscfInfo resource{};
            if (rscf_info(chunk, &resource)) {
                if (resource.type == ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC &&
                    resource.subtype == ASURA_RESOURCEFILE_TYPE_PC_OBJECT && resource.payload_size >= 4)
                    present_objects.push_back(read_u32(resource.payload));
                else if (resource.type == ASURA_RESOURCEFILE_TYPE_TEXTURE)
                    present_textures.push_back(normalized_resource_path(resource.name));
            } else if (chunk.cid == ASURA_CHUNK_SHAPE &&
                       chunk.size >= sizeof(Asura_Chunk_Header) + 4) {
                present_shapes.push_back(read_u32(chunk.data + sizeof(Asura_Chunk_Header)));
            }
        }
    }

    Arena scratch{};
    if (!arena_init(&scratch, 128 * MiB, err))
        return false;
    for (const std::string& donor_path : document.object_donors) {
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

        const uint32_t support_ids[] = {ASURA_CHUNK_TEXTURENAMES, ASURA_CHUNK_TEXTUREFLAGS,
                                        ASURA_CHUNK_MATERIAL, ASURA_CHUNK_MATERIALNAMES};
        auto is_material_support = [&](uint32_t cid) {
            for (uint32_t support : support_ids)
                if (cid == support)
                    return true;
            return false;
        };
        for (size_t id_index = 0; id_index < donor_ids.size(); ++id_index) {
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
                    for (int32_t before = static_cast<int32_t>(i) - 1;
                         before >= 0 && is_material_support(donor.chunks[before].cid); --before)
                        wanted[before] = 1;
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
                    if (i + 1 < donor.count && donor.chunks[i + 1].cid == ASURA_CHUNK_SHAPEDATA)
                        wanted[i + 1] = 1;
                    break;
                }
            }
        }
        for (uint32_t i = 0; i < donor.count; ++i) {
            RscfInfo resource{};
            if (!rscf_info(donor.chunks[i], &resource) || resource.type != ASURA_RESOURCEFILE_TYPE_TEXTURE)
                continue;
            const std::string normalized = normalized_resource_path(resource.name);
            if (std::find(present_textures.begin(), present_textures.end(), normalized) != present_textures.end())
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
                    if (contains_u32(present_objects, id))
                        continue;
                    present_objects.push_back(id);
                } else if (resource.type == ASURA_RESOURCEFILE_TYPE_TEXTURE) {
                    const std::string normalized = normalized_resource_path(resource.name);
                    if (std::find(present_textures.begin(), present_textures.end(), normalized) !=
                        present_textures.end())
                        continue;
                    present_textures.push_back(normalized);
                }
            } else if (donor.chunks[i].cid == ASURA_CHUNK_SHAPE &&
                       donor.chunks[i].size >= sizeof(Asura_Chunk_Header) + 4) {
                const uint32_t id = read_u32(donor.chunks[i].data + sizeof(Asura_Chunk_Header));
                if (contains_u32(present_shapes, id))
                    continue;
                present_shapes.push_back(id);
            }
            append_chunk_copy(out, donor.chunks[i], err);
        }
        unmap_file(&donor.file);
        arena_reset(&scratch, mark);
        if (err->set)
            break;
    }
    if (!err->set) {
        for (uint32_t id : required_roots) {
            if (!contains_u32(present_objects, id) || !contains_u32(present_shapes, id)) {
                fail(err, "Object %08X is not available from the selected Objects donors", id);
                break;
            }
        }
    }
    arena_release(&scratch);
    return !err->set;
}

bool pack_pc_document(const Document& doc, const char* output_path, std::string* why) {
    Error err{};
    Arena arena{};
    ChunkList source{};
    Buffer output{};
    Sounds sounds{};
    bool ok = arena_init(&arena, 64 * MiB, &err) &&
              parse_chunks(doc.source_pc_path.c_str(), &source, &arena, &err);
    uint32_t material_text_chunk = 0xffffffffu;
    uint32_t material_chunk = 0xffffffffu;
    if (ok && !doc.material_map.empty())
        ok = pc_environment_material_chunk_indices(source, &material_text_chunk,
                                                   &material_chunk, &err);
    uint64_t reserve = 0;
    if (ok) {
        if (source.file.size > UINT64_MAX - 64 * MiB)
            ok = fail(&err, "source .PC is too large");
        else
            reserve = source.file.size + 64 * MiB;
    }
    if (ok && sizeof(void*) == 4 && reserve > 512 * MiB)
        ok = fail(&err, "source .PC is too large for the 32-bit editor; use the x64 build");
    if (ok)
        ok = buffer_init(&output, reserve, &err) && make_editor_sounds(doc, &sounds, &arena, &err) &&
             buffer_append(&output, kAsuraMagic, sizeof(kAsuraMagic), &err) != ~0ull;

    bool source_has_lights = false, source_has_phonons = false, source_has_editable_entities = false;
    bool source_has_ambience = false;
    if (ok) {
        for (uint32_t i = 0; i < source.count; ++i) {
            source_has_lights |= source.chunks[i].cid == ASURA_CHUNK_LIGHTS;
            source_has_phonons |= source.chunks[i].cid == ASURA_CHUNK_PHONONS;
            source_has_editable_entities |= editable_pc_entity_chunk(source.chunks[i]);
            source_has_ambience |=
                source.chunks[i].cid == ASURA_CHUNK_STREAMINGBACKGROUNDSOUND;
        }
    }
    bool wrote_lights = false, wrote_phonons = false, wrote_entities = false, wrote_sound_resources = false;
    bool wrote_object_support = false;
    bool wrote_skybox = false;
    auto write_lights = [&]() {
        if (!wrote_lights) {
            wrote_lights = true;
            ok = ok && append_editor_lights(&output, doc, &err);
        }
    };
    auto write_phonons = [&]() {
        if (!wrote_sound_resources) {
            wrote_sound_resources = true;
            ok = ok && append_sound_resources(&output, sounds, &arena, &err);
        }
        if (!wrote_phonons) {
            wrote_phonons = true;
            ok = ok && append_phon(&output, sounds, &err);
        }
    };
    auto write_object_support = [&]() {
        if (!wrote_object_support) {
            wrote_object_support = true;
            ok = ok && append_static_object_support(&output, doc, &source, nullptr, nullptr, &err);
        }
    };
    auto write_entities = [&]() {
        if (!wrote_entities) {
            write_object_support();
            wrote_entities = true;
            ok = ok && append_sound_entities(&output, sounds, &err) && append_editor_spawnpoints(&output, doc, &err) &&
                 append_editor_pickups(&output, doc, &err) && append_editor_static_objects(&output, doc, &err) &&
                 append_editor_building_volumes(&output, doc, &err);
        }
    };

    for (uint32_t i = 0; ok && i < source.count; ++i) {
        const ChunkRef& chunk = source.chunks[i];
        // MCP2 MTRL processing at 0x440440 calls sub_405AD0, which frees and
        // recreates the original-index conversion array. The Env reader then
        // resolves every strip through that array at 0x49E6AD. Replace the
        // source table at its original stream position so the imported map is
        // the one active for Env without leaving a duplicate material state.
        if (!doc.material_map.empty() && i == material_text_chunk) {
            ok = append_pc_material_map_override(&output, source, doc.material_map.c_str(),
                                                 &arena, &err);
            if (!ok)
                break;
            continue;
        }
        if (!doc.material_map.empty() &&
            (i == material_chunk ||
             (material_chunk != 0xffffffffu && i > material_text_chunk && i < material_chunk &&
              chunk.cid == ASURA_CHUNK_TEXTUREFLAGS)))
            continue;
        if (chunk.cid == ASURA_CHUNK_SKYBOX && doc.skybox.source_record) {
            if (!wrote_skybox) {
                wrote_skybox = true;
                ok = append_editor_skybox(&output, doc.skybox, &err);
            }
            continue;
        }
        // The target updates its global weather state whenever it encounters a
        // supported WTHR. Keep every source record consistent so a duplicate
        // or reordered chunk cannot restore the level's authored rain value.
        if (chunk.cid == ASURA_CHUNK_WEATHERSYSTEM && doc.weather_source_record &&
            chunk.version >= 5 && chunk.version <= 6 && chunk.size > 21) {
            ok = append_editor_weather_copy(&output, chunk, doc, &err);
            continue;
        }
        if (chunk.cid == ASURA_CHUNK_EXTENDED_PARTICLE_RAIN_SYSTEM && doc.weather_source_record &&
            chunk.version == 0 && chunk.size >= kExtendedRainFlagsOffset + sizeof(uint32_t)) {
            ok = append_editor_extended_rain_copy(&output, chunk, doc, &err);
            continue;
        }
        if (chunk.cid == ASURA_CHUNK_STREAMINGBACKGROUNDSOUND &&
            doc.ambient_source_record) {
            ok = append_editor_ambience_copy(&output, chunk, doc, &err);
            continue;
        }
        if (chunk.cid == ASURA_CHUNK_RESOURCEFILE) {
            RscfInfo resource{};
            if (rscf_info(chunk, &resource) && replaced_pc_sound_resource(doc, resource))
                continue;
        }
        if (chunk.cid == ASURA_CHUNK_LIGHTS) {
            write_lights();
            continue;
        }
        if (chunk.cid == ASURA_CHUNK_PHONONS) {
            if (!source_has_lights)
                write_lights();
            write_phonons();
            continue;
        }
        if (chunk.cid == ASURA_CHUNK_ENTITY) {
            write_object_support();
            if (!source_has_lights)
                write_lights();
            if (!source_has_phonons)
                write_phonons();
            const bool editable = editable_pc_entity_chunk(chunk);
            if (!wrote_entities && (editable || !source_has_editable_entities))
                write_entities();
            if (editable)
                continue;
            ok = append_source_entity_copy(&output, chunk, doc, &err);
            continue;
        }
        ok = ok && append_chunk_copy(&output, chunk, &err);
    }
    if (ok) {
        write_lights();
        write_phonons();
        write_entities();
        if (!source_has_ambience && !doc.ambient_stream_path.empty())
            ok = append_editor_ambience(&output, doc, &err);
        ok = ok && buffer_append(&output, nullptr, sizeof(Asura_Chunk_Header), &err) != ~0ull;
    }
    if (ok)
        ok = patch_fnfo_file_size(&output, &err);

    // The source may also be the explicitly selected destination. Release its
    // read mapping before opening the output with CREATE_ALWAYS.
    unmap_file(&source.file);
    if (ok)
        ok = write_entire_file(output_path, output.base, output.size, &err);
    if (!ok && why)
        *why = err.set ? err.message : "Packing the imported PC level failed.";
    buffer_release(&output);
    arena_release(&arena);
    return ok;
}

struct Ps2ConvertedDdsPixelFormat {
    uint32_t size;
    uint32_t flags;
    uint32_t four_cc;
    uint32_t rgb_bit_count;
    uint32_t r_mask;
    uint32_t g_mask;
    uint32_t b_mask;
    uint32_t a_mask;
};

struct Ps2ConvertedDdsHeader {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitch_or_linear_size;
    uint32_t depth;
    uint32_t mip_count;
    uint32_t reserved[11];
    Ps2ConvertedDdsPixelFormat pixel_format;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};

static_assert(sizeof(Ps2ConvertedDdsHeader) == 124);

uint16_t dds_rgb565(const uint8_t* colour) {
    return static_cast<uint16_t>(((colour[0] * 31u + 127u) / 255u) << 11u |
                                 ((colour[1] * 63u + 127u) / 255u) << 5u |
                                 ((colour[2] * 31u + 127u) / 255u));
}

void dds_expand_rgb565(uint16_t packed, uint8_t* colour) {
    colour[0] = static_cast<uint8_t>(((packed >> 11u) & 31u) * 255u / 31u);
    colour[1] = static_cast<uint8_t>(((packed >> 5u) & 63u) * 255u / 63u);
    colour[2] = static_cast<uint8_t>((packed & 31u) * 255u / 31u);
}

uint32_t dds_colour_distance(const uint8_t* a, const uint8_t* b) {
    const int32_t red = static_cast<int32_t>(a[0]) - b[0];
    const int32_t green = static_cast<int32_t>(a[1]) - b[1];
    const int32_t blue = static_cast<int32_t>(a[2]) - b[2];
    return static_cast<uint32_t>(red * red + green * green + blue * blue);
}

void dds_compress_colour_block(const uint8_t pixels[16][4], bool allow_transparency,
                               uint8_t* output) {
    bool transparent = false;
    uint32_t opaque_count = 0;
    for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        transparent |= allow_transparency && pixels[pixel][3] < 128u;
        opaque_count += !allow_transparency || pixels[pixel][3] >= 128u;
    }

    uint32_t endpoint_a = 0;
    uint32_t endpoint_b = 0;
    uint32_t greatest_distance = 0;
    for (uint32_t a = 0; a < 16; ++a) {
        if (allow_transparency && pixels[a][3] < 128u)
            continue;
        for (uint32_t b = a + 1; b < 16; ++b) {
            if (allow_transparency && pixels[b][3] < 128u)
                continue;
            const uint32_t distance = dds_colour_distance(pixels[a], pixels[b]);
            if (distance > greatest_distance) {
                greatest_distance = distance;
                endpoint_a = a;
                endpoint_b = b;
            }
        }
    }

    uint16_t colour0 = opaque_count ? dds_rgb565(pixels[endpoint_a]) : 0u;
    uint16_t colour1 = opaque_count ? dds_rgb565(pixels[endpoint_b]) : 0u;
    if (transparent) {
        if (colour0 > colour1)
            std::swap(colour0, colour1);
    } else {
        if (colour0 < colour1)
            std::swap(colour0, colour1);
        if (colour0 == colour1) {
            if (colour0 < 0xffffu)
                ++colour0;
            else
                --colour1;
        }
    }

    uint8_t palette[4][3]{};
    dds_expand_rgb565(colour0, palette[0]);
    dds_expand_rgb565(colour1, palette[1]);
    if (colour0 > colour1) {
        for (uint32_t channel = 0; channel < 3; ++channel) {
            palette[2][channel] = static_cast<uint8_t>((2u * palette[0][channel] + palette[1][channel]) / 3u);
            palette[3][channel] = static_cast<uint8_t>((palette[0][channel] + 2u * palette[1][channel]) / 3u);
        }
    } else {
        for (uint32_t channel = 0; channel < 3; ++channel)
            palette[2][channel] = static_cast<uint8_t>((palette[0][channel] + palette[1][channel]) / 2u);
    }

    uint32_t indices = 0;
    for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        uint32_t best = transparent && pixels[pixel][3] < 128u ? 3u : 0u;
        if (best != 3u) {
            uint32_t best_distance = ~0u;
            const uint32_t colour_count = colour0 > colour1 ? 4u : 3u;
            for (uint32_t candidate = 0; candidate < colour_count; ++candidate) {
                const uint32_t distance = dds_colour_distance(pixels[pixel], palette[candidate]);
                if (distance < best_distance) {
                    best_distance = distance;
                    best = candidate;
                }
            }
        }
        indices |= best << (pixel * 2u);
    }
    output[0] = static_cast<uint8_t>(colour0);
    output[1] = static_cast<uint8_t>(colour0 >> 8u);
    output[2] = static_cast<uint8_t>(colour1);
    output[3] = static_cast<uint8_t>(colour1 >> 8u);
    for (uint32_t byte = 0; byte < 4; ++byte)
        output[4 + byte] = static_cast<uint8_t>(indices >> (byte * 8u));
}

void dds_compress_alpha_block(const uint8_t pixels[16][4], uint8_t* output) {
    uint8_t alpha0 = 0;
    uint8_t alpha1 = 255;
    for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        alpha0 = std::max(alpha0, pixels[pixel][3]);
        alpha1 = std::min(alpha1, pixels[pixel][3]);
    }
    if (alpha0 == alpha1) {
        if (alpha0)
            --alpha1;
        else
            ++alpha0;
    }
    uint8_t palette[8]{alpha0, alpha1};
    if (alpha0 > alpha1) {
        for (uint32_t i = 1; i <= 6; ++i)
            palette[i + 1] = static_cast<uint8_t>(((7u - i) * alpha0 + i * alpha1) / 7u);
    } else {
        for (uint32_t i = 1; i <= 4; ++i)
            palette[i + 1] = static_cast<uint8_t>(((5u - i) * alpha0 + i * alpha1) / 5u);
        palette[6] = 0;
        palette[7] = 255;
    }
    uint64_t indices = 0;
    for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        uint32_t best = 0;
        uint32_t best_distance = ~0u;
        for (uint32_t candidate = 0; candidate < 8; ++candidate) {
            const int32_t delta = static_cast<int32_t>(pixels[pixel][3]) - palette[candidate];
            const uint32_t distance = static_cast<uint32_t>(delta * delta);
            if (distance < best_distance) {
                best_distance = distance;
                best = candidate;
            }
        }
        indices |= static_cast<uint64_t>(best) << (pixel * 3u);
    }
    output[0] = alpha0;
    output[1] = alpha1;
    for (uint32_t byte = 0; byte < 6; ++byte)
        output[2 + byte] = static_cast<uint8_t>(indices >> (byte * 8u));
}

void dds_compress_mip(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                      bool dxt5, std::vector<uint8_t>* output) {
    const uint32_t blocks_x = std::max(1u, (width + 3u) / 4u);
    const uint32_t blocks_y = std::max(1u, (height + 3u) / 4u);
    const uint32_t block_size = dxt5 ? 16u : 8u;
    const size_t start = output->size();
    output->resize(start + static_cast<size_t>(blocks_x) * blocks_y * block_size);
    for (uint32_t block_y = 0; block_y < blocks_y; ++block_y) {
        for (uint32_t block_x = 0; block_x < blocks_x; ++block_x) {
            uint8_t pixels[16][4]{};
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t source_x = std::min(width - 1u, block_x * 4u + x);
                    const uint32_t source_y = std::min(height - 1u, block_y * 4u + y);
                    memcpy(pixels[y * 4u + x],
                           rgba.data() + (static_cast<size_t>(source_y) * width + source_x) * 4u,
                           4);
                }
            }
            uint8_t* block = output->data() + start +
                             (static_cast<size_t>(block_y) * blocks_x + block_x) * block_size;
            if (dxt5) {
                dds_compress_alpha_block(pixels, block);
                dds_compress_colour_block(pixels, false, block + 8);
            } else {
                dds_compress_colour_block(pixels, true, block);
            }
        }
    }
}

std::vector<uint8_t> dds_next_mip(const std::vector<uint8_t>& source, uint32_t width,
                                  uint32_t height) {
    const uint32_t next_width = std::max(1u, width / 2u);
    const uint32_t next_height = std::max(1u, height / 2u);
    std::vector<uint8_t> next(static_cast<size_t>(next_width) * next_height * 4u);
    for (uint32_t y = 0; y < next_height; ++y) {
        for (uint32_t x = 0; x < next_width; ++x) {
            uint32_t sum[4]{};
            uint32_t samples = 0;
            for (uint32_t source_y = y * 2u; source_y < std::min(height, y * 2u + 2u); ++source_y) {
                for (uint32_t source_x = x * 2u; source_x < std::min(width, x * 2u + 2u); ++source_x) {
                    const uint8_t* pixel = source.data() +
                                           (static_cast<size_t>(source_y) * width + source_x) * 4u;
                    for (uint32_t channel = 0; channel < 4; ++channel)
                        sum[channel] += pixel[channel];
                    ++samples;
                }
            }
            uint8_t* target = next.data() + (static_cast<size_t>(y) * next_width + x) * 4u;
            for (uint32_t channel = 0; channel < 4; ++channel)
                target[channel] = static_cast<uint8_t>((sum[channel] + samples / 2u) / samples);
        }
    }
    return next;
}

bool convert_ps2_tim2_to_dds(const RscfInfo& texture, std::vector<uint8_t>* dds,
                             Error* err) {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    Error decode_error{};
    if (!decode_ps2_tim2(texture.payload, texture.payload_size, &width, &height, &rgba,
                         &decode_error))
        return fail(err, "%s: %.*s",
                    decode_error.set ? decode_error.message : "could not decode PS2 TIM2 texture",
                    texture.name.size, texture.name.data);

    bool has_fractional_alpha = false;
    for (size_t pixel = 3; pixel < rgba.size(); pixel += 4)
        has_fractional_alpha |= rgba[pixel] > 0u && rgba[pixel] < 250u;
    const bool dxt5 = str_ends_i(texture.name, str_lit(".tga")) || has_fractional_alpha;

    uint32_t mip_count = 1;
    for (uint32_t mip_width = width, mip_height = height;
         mip_width > 1u || mip_height > 1u;
         mip_width = std::max(1u, mip_width / 2u),
         mip_height = std::max(1u, mip_height / 2u))
        ++mip_count;
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> mip = std::move(rgba);
    uint32_t mip_width = width;
    uint32_t mip_height = height;
    for (uint32_t level = 0; level < mip_count; ++level) {
        dds_compress_mip(mip, mip_width, mip_height, dxt5, &compressed);
        if (level + 1u < mip_count) {
            mip = dds_next_mip(mip, mip_width, mip_height);
            mip_width = std::max(1u, mip_width / 2u);
            mip_height = std::max(1u, mip_height / 2u);
        }
    }

    Ps2ConvertedDdsHeader header{};
    header.size = sizeof(header);
    header.flags = 0x000a1007u; // CAPS | HEIGHT | WIDTH | PIXELFORMAT | LINEARSIZE | MIPMAPCOUNT
    header.height = height;
    header.width = width;
    header.pitch_or_linear_size = std::max(1u, (width + 3u) / 4u) *
                                  std::max(1u, (height + 3u) / 4u) * (dxt5 ? 16u : 8u);
    header.mip_count = mip_count;
    header.pixel_format.size = sizeof(header.pixel_format);
    header.pixel_format.flags = 0x4u; // FOURCC
    header.pixel_format.four_cc = dxt5 ? fourcc('D', 'X', 'T', '5')
                                       : fourcc('D', 'X', 'T', '1');
    header.caps = 0x00401008u; // TEXTURE | COMPLEX | MIPMAP
    dds->resize(4 + sizeof(header) + compressed.size());
    memcpy(dds->data(), "DDS ", 4);
    memcpy(dds->data() + 4, &header, sizeof(header));
    memcpy(dds->data() + 4 + sizeof(header), compressed.data(), compressed.size());
    return true;
}

bool append_ps2_object_material_support(Buffer* out, const StaticObjectModel& model,
                                        Error* err) {
    if (model.mesh.materials.empty())
        return fail(err, "PS2 object %08X has no material binding", model.file_id);
    const SpawnPuppetMaterial& material = model.mesh.materials[0];
    const Str texture_name{material.texture_name.data(),
                           static_cast<uint32_t>(material.texture_name.size())};

    ChunkMark text = begin_chunk(out, ASURA_CHUNK_TEXTURENAMES, 3, 0, err);
    append_u32(out, 1, err);
    append_padded_cstr(out, texture_name, err);
    if (!end_chunk(out, text, err))
        return false;

    ChunkMark txfl = begin_chunk(out, ASURA_CHUNK_TEXTUREFLAGS, 1, 0, err);
    append_u32(out, 1, err);
    append_u32(out, material.texture_flags ? material.texture_flags : 0x2000u, err);
    if (!end_chunk(out, txfl, err))
        return false;

    ChunkMark mtrl = begin_chunk(out, ASURA_CHUNK_MATERIAL, 1, 0, err);
    append_u32(out, 1, err);
    append_u32(out, 0, err);
    append_u32(out, material.flags, err);
    append_u32(out, 4, err);
    if (!end_chunk(out, mtrl, err))
        return false;

    ChunkMark shpd = begin_chunk(out, ASURA_CHUNK_SHAPEDATA, 0, 0, err);
    append_u32(out, model.file_id, err);
    append_u32(out, 0, err);
    return end_chunk(out, shpd, err);
}

bool make_pc_object_payload(const StaticObjectModel& model, Buffer* payload, Error* err) {
    const SpawnPuppet& mesh = model.mesh;
    if (mesh.vertices.empty() || mesh.faces.empty() || mesh.vertices.size() > 65535)
        return fail(err, "PS2 object %08X has no PC-compatible geometry", model.file_id);
    if (mesh.faces.size() > ((UINT32_MAX - 3u) / 6u + 1u) / 2u)
        return fail(err, "PS2 object %08X has too many faces", model.file_id);

    // PS2 static objects are submitted without backface culling. PC Object
    // resources are culled by the target renderer, so preserve the PS2 result
    // explicitly by emitting each decoded face in both winding directions.
    const size_t output_face_count = mesh.faces.size() * 2u;
    std::vector<uint16_t> indices;
    indices.reserve(3 + (output_face_count - 1) * 6);
    auto append_face = [&indices](const std::array<uint16_t, 3>& face) {
        if (indices.empty()) {
            indices.insert(indices.end(), face.begin(), face.end());
            return;
        }
        // Reset a single PC triangle strip with six degenerate indices. The
        // even-sized bridge keeps every real face on the same strip parity.
        const uint16_t previous = indices.back();
        indices.push_back(previous);
        indices.push_back(previous);
        indices.push_back(face[0]);
        indices.push_back(face[0]);
        indices.push_back(face[1]);
        indices.push_back(face[2]);
    };
    for (size_t face_index = 0; face_index < mesh.faces.size(); ++face_index) {
        const std::array<uint16_t, 3>& face = mesh.faces[face_index];
        if (face[0] >= mesh.vertices.size() || face[1] >= mesh.vertices.size() ||
            face[2] >= mesh.vertices.size())
            return fail(err, "PS2 object %08X has an out-of-range face", model.file_id);
        append_face(face);
        append_face({face[0], face[2], face[1]});
    }
    if (indices.size() > UINT32_MAX)
        return fail(err, "PS2 object %08X has too many strip indices", model.file_id);

    const uint32_t index_count = static_cast<uint32_t>(indices.size());
    const uint32_t triangle_count = index_count - 2;
    const uint64_t reserve = 20ull + static_cast<uint64_t>(mesh.vertices.size()) * 32 +
                             static_cast<uint64_t>(index_count) * 2 + 8;
    if (!buffer_init(payload, reserve, err))
        return false;
    append_u32(payload, model.file_id, err);
    append_u32(payload, triangle_count, err);
    append_u32(payload, static_cast<uint32_t>(mesh.vertices.size()), err);
    append_u32(payload, index_count, err);
    append_u32(payload, 0, err);
    for (const SpawnPuppetVertex& vertex : mesh.vertices) {
        append_f32(payload, vertex.position.x, err);
        append_f32(payload, vertex.position.y, err);
        append_f32(payload, vertex.position.z, err);
        append_f32(payload, vertex.normal.x, err);
        append_f32(payload, vertex.normal.y, err);
        append_f32(payload, vertex.normal.z, err);
        append_f32(payload, vertex.texcoord.x, err);
        append_f32(payload, vertex.texcoord.y, err);
    }
    for (uint16_t index : indices)
        append_u16(payload, index, err);
    append_f32(payload, 0.0f, err);
    append_u32(payload, 0, err);
    return !err->set;
}

bool append_pc_object_rscf(Buffer* out, const StaticObjectModel& model, Error* err) {
    Buffer payload{};
    if (!make_pc_object_payload(model, &payload, err))
        return false;
    const Str resource_name{model.mesh.resource_name.data(),
                            static_cast<uint32_t>(model.mesh.resource_name.size())};
    ChunkMark chunk = begin_chunk(out, ASURA_CHUNK_RESOURCEFILE, 1, 0, err);
    append_u32(out, ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC, err);
    append_u32(out, ASURA_RESOURCEFILE_TYPE_PC_OBJECT, err);
    append_u32(out, static_cast<uint32_t>(payload.size), err);
    append_padded_cstr(out, resource_name, err);
    buffer_append(out, payload.base, payload.size, err);
    const bool ok = end_chunk(out, chunk, err);
    buffer_release(&payload);
    return ok;
}

bool append_ps2_static_object_support(Buffer* out, const Document& document,
                                      const ChunkList& source,
                                      std::vector<uint32_t>* object_ids,
                                      std::vector<uint32_t>* shape_ids, Error* err) {
    std::vector<StaticObjectModel> models;
    if (!decode_ps2_static_object_models(source, document, &models, err))
        return false;

    std::vector<std::string> emitted_textures;
    for (const StaticObjectModel& model : models) {
        if (model.mesh.materials.empty() || model.mesh.materials[0].texture_bytes.empty())
            return fail(err, "PS2 object %08X has no source TIM2 texture", model.file_id);
        const SpawnPuppetMaterial& material = model.mesh.materials[0];
        const std::string identity = normalized_resource_path(
            Str{material.texture_name.data(), static_cast<uint32_t>(material.texture_name.size())});
        if (std::find(emitted_textures.begin(), emitted_textures.end(), identity) !=
            emitted_textures.end())
            continue;
        RscfInfo texture{};
        texture.type = ASURA_RESOURCEFILE_TYPE_TEXTURE;
        texture.name = {material.texture_name.data(),
                        static_cast<uint32_t>(material.texture_name.size())};
        texture.payload = material.texture_bytes.data();
        texture.payload_size = static_cast<uint32_t>(material.texture_bytes.size());
        std::vector<uint8_t> dds;
        if (!convert_ps2_tim2_to_dds(texture, &dds, err))
            return false;
        std::string resource_name = material.texture_name;
        if (resource_name.rfind("\\graphics", 0) != 0 &&
            resource_name.rfind("/graphics", 0) != 0)
            resource_name = "\\graphics" + resource_name;
        if (!append_rscf(out, {resource_name.data(), static_cast<uint32_t>(resource_name.size())},
                         ASURA_RESOURCEFILE_TYPE_TEXTURE, 0, dds.data(),
                         static_cast<uint32_t>(dds.size()), err))
            return false;
        emitted_textures.push_back(identity);
    }

    for (const StaticObjectModel& model : models) {
        if (!append_ps2_object_material_support(out, model, err) ||
            !append_pc_object_rscf(out, model, err))
            return false;
        object_ids->push_back(model.file_id);
    }
    for (const StaticObjectModel& model : models) {
        for (uint32_t chunk_index = 0; chunk_index < source.count; ++chunk_index) {
            const ChunkRef& shape = source.chunks[chunk_index];
            if (shape.cid != ASURA_CHUNK_SHAPE ||
                shape.size < sizeof(Asura_Chunk_Header) + 4 ||
                read_u32(shape.data + sizeof(Asura_Chunk_Header)) != model.file_id)
                continue;
            if (!append_chunk_copy(out, shape, err))
                return false;
            if (chunk_index + 1 < source.count &&
                source.chunks[chunk_index + 1].cid == ASURA_CHUNK_SHAPEDATA &&
                !append_chunk_copy(out, source.chunks[chunk_index + 1], err))
                return false;
            shape_ids->push_back(model.file_id);
            break;
        }
    }
    return true;
}

bool append_ps2_passthrough_entities(Buffer* out, const ChunkList& source, Error* err) {
    for (uint32_t chunk_index = 0; chunk_index < source.count; ++chunk_index) {
        const ChunkRef& chunk = source.chunks[chunk_index];
        if (chunk.cid != ASURA_CHUNK_ENTITY || chunk.size < sizeof(Asura_Chunk_Entity))
            continue;
        const uint16_t classification = read_u16(
            chunk.data + sizeof(Asura_Chunk_Header) +
            offsetof(Asura_Chunk_Entity_PayloadHeader, Classification));
        // The cutscene controller is an existing engine classification and
        // contains only versioned scalar/string data. Unknown project-specific
        // classifications are deliberately not carried into the PC output.
        if (classification != AsuraEntityClass_CutsceneController)
            continue;
        if (!append_chunk_copy(out, chunk, err))
            return false;
    }
    return true;
}

bool append_ps2_environment_textures_as_pc(
    Buffer* output, const ChunkList& source,
    const std::vector<PcEnvironmentMaterialBinding>& materials, Error* err) {
    std::vector<std::string> emitted;
    for (const PcEnvironmentMaterialBinding& material : materials) {
        if (material.texture_index < 0 || !material.texture_name.size)
            continue;
        const std::string identity = normalized_resource_path(str_trim(material.texture_name));
        if (std::find(emitted.begin(), emitted.end(), identity) != emitted.end())
            continue;

        RscfInfo texture{};
        bool found = false;
        for (uint32_t chunk_index = 0; chunk_index < source.count; ++chunk_index) {
            RscfInfo candidate{};
            if (rscf_info(source.chunks[chunk_index], &candidate) &&
                candidate.type == ASURA_RESOURCEFILE_TYPE_TEXTURE &&
                text_name_matches_resource(material.texture_name, candidate.name)) {
                texture = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            // Retail PS2 material tables contain a named Untextured! slot with
            // no texture RSCF. A path-like entry, however, must resolve or the
            // converted PC level would silently lose a real material.
            if (identity.find('\\') == std::string::npos &&
                identity.find('/') == std::string::npos) {
                emitted.push_back(identity);
                continue;
            }
            return fail(err, "PS2 environment texture is absent: %.*s",
                        material.texture_name.size, material.texture_name.data);
        }

        std::vector<uint8_t> dds;
        if (!convert_ps2_tim2_to_dds(texture, &dds, err))
            return false;
        const Str resource_name = str_trim(texture.name);
        if (!append_rscf(output, resource_name, ASURA_RESOURCEFILE_TYPE_TEXTURE, 0,
                         dds.data(), static_cast<uint32_t>(dds.size()), err))
            return false;
        emitted.push_back(identity);
    }

    RscfInfo environment{};
    if (!find_ps2_environment(source, &environment))
        return fail(err, "the .PS2 contains no PS2 environment RSCF");
    const ChunkRef* text = nullptr;
    const ChunkRef* txfl = nullptr;
    const ChunkRef* mtrl = nullptr;
    for (uint32_t chunk_index = 0; chunk_index < source.count; ++chunk_index) {
        const ChunkRef& chunk = source.chunks[chunk_index];
        RscfInfo resource{};
        if (rscf_info(chunk, &resource) && resource.payload == environment.payload)
            break;
        if (chunk.cid == ASURA_CHUNK_TEXTURENAMES)
            text = &chunk;
        else if (chunk.cid == ASURA_CHUNK_TEXTUREFLAGS)
            txfl = &chunk;
        else if (chunk.cid == ASURA_CHUNK_MATERIAL)
            mtrl = &chunk;
    }
    if (!text || !mtrl)
        return fail(err, "the PS2 environment has no active TEXT/MTRL material table");
    return append_chunk_copy(output, *text, err) &&
           (!txfl || append_chunk_copy(output, *txfl, err)) &&
           append_chunk_copy(output, *mtrl, err);
}

bool append_ps2_sky_resources_as_pc(Buffer* output, const Document& document,
                                    const ChunkList& source, Error* err) {
    // An external sky folder is an explicit replacement. Otherwise retain the
    // authored PS2 SKYB paths and transcode their matching TIM2 RSCFs to DDS.
    if (!document.sky_texture_dir.empty() || !document.skybox.source_record)
        return true;
    std::vector<std::string> emitted;
    for (const std::string& texture_path : document.skybox.texture_paths) {
        if (texture_path.empty())
            continue;
        const Str wanted{texture_path.data(), static_cast<uint32_t>(texture_path.size())};
        const std::string identity = normalized_resource_path(wanted);
        if (std::find(emitted.begin(), emitted.end(), identity) != emitted.end())
            continue;
        RscfInfo texture{};
        bool found = false;
        for (uint32_t chunk_index = 0; chunk_index < source.count; ++chunk_index) {
            RscfInfo candidate{};
            if (rscf_info(source.chunks[chunk_index], &candidate) &&
                candidate.type == ASURA_RESOURCEFILE_TYPE_TEXTURE &&
                text_name_matches_resource(wanted, candidate.name)) {
                texture = candidate;
                found = true;
                break;
            }
        }
        if (!found)
            return fail(err, "PS2 SKYB texture is absent: %s", texture_path.c_str());
        std::vector<uint8_t> dds;
        if (!convert_ps2_tim2_to_dds(texture, &dds, err))
            return false;
        const Str resource_name = str_trim(texture.name);
        if (!append_rscf(output, resource_name, ASURA_RESOURCEFILE_TYPE_TEXTURE, 0,
                         dds.data(), static_cast<uint32_t>(dds.size()), err))
            return false;
        emitted.push_back(identity);
    }
    return true;
}

bool obj_data_from_ps2_mesh(const Mesh& mesh, bool double_sided, ObjData* obj,
                            std::vector<std::string>* material_names,
                            Arena* arena, Error* err) {
    if (mesh.positions.empty() || mesh.faces.empty() ||
        mesh.positions.size() > UINT32_MAX || mesh.faces.size() > UINT32_MAX ||
        (double_sided && mesh.faces.size() > UINT32_MAX / 2u))
        return fail(err, "decoded PS2 environment geometry is empty or too large");
    const bool have_normals = mesh.normals.size() == mesh.positions.size();
    const bool have_texcoords = mesh.texcoords.size() == mesh.positions.size();
    const bool have_colors = mesh.diffuse_abgr.size() == mesh.positions.size();
    obj->position_count = static_cast<uint32_t>(mesh.positions.size());
    obj->normal_count = have_normals ? obj->position_count : 0;
    obj->texcoord_count = have_texcoords ? obj->position_count : 0;
    obj->face_count = static_cast<uint32_t>(mesh.faces.size()) * (double_sided ? 2u : 1u);
    obj->positions = arena_array<Asura_Vector_3>(arena, obj->position_count, err, false);
    obj->colors = arena_array<uint32_t>(arena, obj->position_count, err, false);
    obj->has_color = arena_array<uint8_t>(arena, obj->position_count, err);
    obj->normals = have_normals
                       ? arena_array<Asura_Vector_3>(arena, obj->normal_count, err, false)
                       : nullptr;
    obj->texcoords = have_texcoords
                         ? arena_array<Asura_Vector_2>(arena, obj->texcoord_count, err, false)
                         : nullptr;
    obj->faces = arena_array<ObjFace>(arena, obj->face_count, err, false);
    if (err->set)
        return false;
    memcpy(obj->positions, mesh.positions.data(), mesh.positions.size() * sizeof(mesh.positions[0]));
    if (have_normals)
        memcpy(obj->normals, mesh.normals.data(), mesh.normals.size() * sizeof(mesh.normals[0]));
    if (have_texcoords) {
        memcpy(obj->texcoords, mesh.texcoords.data(), mesh.texcoords.size() * sizeof(mesh.texcoords[0]));
        // build_env performs the OBJ-to-target V flip. Undo it in this
        // in-memory OBJ view so the PS2 target UV reaches the PC Env exactly.
        for (uint32_t i = 0; i < obj->texcoord_count; ++i)
            obj->texcoords[i].y = 1.0f - obj->texcoords[i].y;
    }
    for (uint32_t i = 0; i < obj->position_count; ++i) {
        obj->colors[i] = have_colors ? mesh.diffuse_abgr[i] : 0xffffffffu;
        obj->has_color[i] = 1;
    }

    uint32_t maximum_material = 0;
    for (int32_t material : mesh.face_materials)
        if (material > 0)
            maximum_material = std::max(maximum_material, static_cast<uint32_t>(material));
    material_names->resize(static_cast<size_t>(maximum_material) + 1);
    for (uint32_t i = 0; i <= maximum_material; ++i)
        (*material_names)[i] = "mat_" + std::to_string(i);

    for (uint32_t source_index = 0; source_index < mesh.faces.size(); ++source_index) {
        const std::array<uint32_t, 3>& source_face = mesh.faces[source_index];
        if (source_face[0] >= obj->position_count || source_face[1] >= obj->position_count ||
            source_face[2] >= obj->position_count)
            return fail(err, "decoded PS2 environment face %u has an invalid vertex", source_index);
        const uint32_t material = source_index < mesh.face_materials.size() &&
                                          mesh.face_materials[source_index] >= 0
                                      ? static_cast<uint32_t>(mesh.face_materials[source_index])
                                      : 0u;
        const auto index = [have_texcoords, have_normals](uint32_t vertex) {
            const int32_t one_based = static_cast<int32_t>(vertex + 1u);
            return ObjIndex{one_based, have_texcoords ? one_based : 0,
                            have_normals ? one_based : 0};
        };
        const uint32_t face_index = source_index * (double_sided ? 2u : 1u);
        ObjFace& face = obj->faces[face_index];
        face.a = index(source_face[0]);
        face.b = index(source_face[1]);
        face.c = index(source_face[2]);
        const std::string& name = (*material_names)[material];
        face.material = {name.data(), static_cast<uint32_t>(name.size())};
        face.order = face_index;
        if (double_sided) {
            ObjFace& reverse = obj->faces[face_index + 1];
            reverse.a = face.a;
            reverse.b = face.c;
            reverse.c = face.b;
            reverse.material = face.material;
            reverse.order = face_index + 1;
        }
    }
    return true;
}

bool pack_ps2_document(const Document& doc, const Mesh& mesh, const char* output_path,
                       std::string* why) {
    bool has_pickups = false;
    for (const Entity& entity : doc.entities) {
        has_pickups |= entity.kind == EntityKind::Pickup;
    }
    if (has_pickups && doc.weapons_donor.empty()) {
        if (why)
            *why = "Choose a Weapons donor .PC before exporting PS2 geometry with pickups.";
        return false;
    }
    Error err{};
    Config cfg{};
    if (!initialize_editor_config(doc.source_pc_path.c_str(), &cfg, &err)) {
        if (why)
            *why = err.message;
        return false;
    }
    cfg.flip_y = true;
    cfg.flip_z = false;
    cfg.diffuse_abgr = 0xffffffffu;
    cfg.allow_unknown_materials = true;
    cfg.weapon_from_pc = doc.weapons_donor.empty() ? nullptr : doc.weapons_donor.c_str();
    cfg.sky_texture_dir = doc.sky_texture_dir.empty() ? nullptr : doc.sky_texture_dir.c_str();
    for (uint32_t slot = 0; slot < ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT; ++slot)
        cfg.sky_texture_paths[slot] = doc.skybox.texture_paths[slot].empty()
                                           ? nullptr
                                           : doc.skybox.texture_paths[slot].c_str();

    Arena arena{}, scratch{};
    ChunkList source{};
    Buffer output{}, env_payload{}, collision_payload{};
    MaterialMap material_map{};
    ObjData render_obj{}, collision_obj{};
    EnvBuild env{}, collision_env{};
    EnvView view{}, collision_view{};
    Sounds sounds{};
    ModuleMetric* metrics = nullptr;
    std::vector<std::string> render_material_names, collision_material_names;
    std::vector<PcEnvironmentMaterialBinding> materials;
    std::vector<uint32_t> source_object_ids, source_shape_ids;
    bool ok = arena_init(&arena, cfg.arena_reserve, &err) &&
              arena_init(&scratch, cfg.arena_reserve, &err) &&
              parse_chunks(doc.source_pc_path.c_str(), &source, &arena, &err) &&
              ps2_environment_material_bindings(source, &materials, &err) &&
              obj_data_from_ps2_mesh(mesh, true, &render_obj, &render_material_names,
                                     &arena, &err) &&
              obj_data_from_ps2_mesh(mesh, true, &collision_obj,
                                     &collision_material_names, &arena, &err) &&
              validate_spawn_clearance(doc, collision_obj, cfg, &err) &&
              buffer_init(&output, cfg.output_reserve, &err);
    if (ok) {
        // PS2 environment packets are effectively two-sided. Their source
        // winding is therefore not a reliable indication of which side must
        // collide on PC either: keeping collision single-sided leaves the
        // oppositely-wound source triangles as walk-through holes. Duplicate
        // both render and collision faces, and keep the normal per-module cap
        // so the collision AABB query never sees twice its intended polygon
        // budget. Building both views with the same cap also preserves the
        // one-to-one renderer/EMOD module layout.
        ok = build_env(cfg, render_obj, material_map, &arena, &scratch, &env, &err);
        if (ok) {
            env_payload = env.payload;
            ok = build_env(cfg, collision_obj, material_map, &arena, &scratch,
                           &collision_env, &err);
        }
        if (ok)
            collision_payload = collision_env.payload;
    }
    if (ok) {
        ok = env_view(env_payload, &view, &arena, &err) && view.module_count &&
             view.module_count <= kMaxAabbTreeObjects &&
             env_view(collision_payload, &collision_view, &arena, &err) &&
             collision_view.module_count == view.module_count;
        if (!ok && !err.set)
            fail(&err, "two-sided render and collision module counts disagree");
    }
    if (ok) {
        metrics = arena_array<ModuleMetric>(&arena, view.module_count, &err);
        ok = metrics && make_editor_sounds(doc, &sounds, &arena, &err);
    }
    if (ok) {
        buffer_append(&output, kAsuraMagic, sizeof(kAsuraMagic), &err);
        ok = append_fnfo(&output, &err) && append_rsfl(&output, &err) &&
             append_weapon_support(&output, cfg, &scratch, &err) &&
             append_ps2_static_object_support(&output, doc, source,
                                              &source_object_ids, &source_shape_ids, &err) &&
             append_static_object_support(&output, doc, nullptr, &source_object_ids,
                                          &source_shape_ids, &err) &&
             append_sky_resources(&output, cfg, &scratch, &err) &&
             append_ps2_sky_resources_as_pc(&output, doc, source, &err) &&
             append_ps2_environment_textures_as_pc(&output, source, materials, &err) &&
             append_rscf(&output, str_from_c(cfg.env_name),
                         ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC,
                         ASURA_RESOURCEFILE_TYPE_PC_ENVIRONMENT, env_payload.base,
                         static_cast<uint32_t>(env_payload.size), &err) &&
             append_sound_resources(&output, sounds, &scratch, &err) &&
             (doc.ambient_stream_path.empty() || append_editor_ambience(&output, doc, &err)) &&
             append_editor_lights(&output, doc, &err) && append_phon(&output, sounds, &err) &&
             append_emod(&output, collision_view, collision_view.module_count, cfg,
                         material_map, &scratch, metrics, &err) &&
             append_mlin(&output, metrics, view.module_count, &err) &&
             append_mrvb(&output, view.module_count, &err) &&
             append_nav1(&output, view.module_count, &err) &&
             append_sound_entities(&output, sounds, &err) &&
             append_ps2_passthrough_entities(&output, source, &err) &&
             append_editor_spawnpoints(&output, doc, &err) &&
             append_editor_pickups(&output, doc, &err) &&
             append_editor_static_objects(&output, doc, &err) &&
             append_editor_building_volumes(&output, doc, &err) &&
             append_editor_skybox(&output, doc.skybox, &err) &&
             append_fog(&output, &err) && append_editor_weather(&output, doc, &err) &&
             buffer_append(&output, nullptr, sizeof(Asura_Chunk_Header), &err) != ~0ull &&
             patch_fnfo_file_size(&output, &err);
    }

    unmap_file(&source.file);
    if (ok)
        ok = write_entire_file(output_path, output.base, output.size, &err);
    if (!ok && why)
        *why = err.set ? err.message : "Converting the PS2 level to PC failed.";
    buffer_release(&collision_payload);
    buffer_release(&env_payload);
    buffer_release(&output);
    arena_release(&scratch);
    arena_release(&arena);
    return ok;
}

bool pack_document(Document& doc, const Mesh* source_mesh, const char* output_path,
                   std::string* why) {
    if (!normalise_editor_guids(&doc, why))
        return false;
    if (!doc.source_pc_path.empty()) {
        const size_t dot = doc.source_pc_path.find_last_of('.');
        const char* extension = dot == std::string::npos ? "" : doc.source_pc_path.c_str() + dot;
        if (_stricmp(extension, ".ps2") == 0) {
            if (!source_mesh) {
                if (why)
                    *why = "The decoded PS2 geometry is unavailable for PC conversion.";
                return false;
            }
            return pack_ps2_document(doc, *source_mesh, output_path, why);
        }
        return pack_pc_document(doc, output_path, why);
    }
    if (doc.obj_path.empty()) {
        if (why)
            *why = "Open an OBJ before exporting.";
        return false;
    }
    bool has_pickups = false, has_static_objects = false;
    for (const Entity& entity : doc.entities) {
        has_pickups |= entity.kind == EntityKind::Pickup;
        has_static_objects |= entity.kind == EntityKind::StaticObject;
    }
    if (has_pickups && doc.weapons_donor.empty()) {
        if (why)
            *why = "Choose a Weapons donor .PC before exporting pickups from a custom level.";
        return false;
    }
    if (has_static_objects && doc.object_donors.empty()) {
        if (why)
            *why = "Choose at least one Objects donor .PC before exporting Objects from a custom level.";
        return false;
    }
    Error err{};
    Config cfg{};
    if (!initialize_editor_config(doc.obj_path.c_str(), &cfg, &err)) {
        if (why)
            *why = err.message;
        return false;
    }
    cfg.material_map = doc.material_map.empty() ? nullptr : doc.material_map.c_str();
    cfg.texture_dir = doc.texture_dir.empty() ? nullptr : doc.texture_dir.c_str();
    cfg.weapon_from_pc = doc.weapons_donor.empty() ? nullptr : doc.weapons_donor.c_str();
    cfg.sky_texture_dir = doc.sky_texture_dir.empty() ? nullptr : doc.sky_texture_dir.c_str();
    for (uint32_t slot = 0; slot < ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT; ++slot)
        cfg.sky_texture_paths[slot] = doc.skybox.texture_paths[slot].empty()
                                           ? nullptr
                                           : doc.skybox.texture_paths[slot].c_str();
    cfg.allow_unknown_materials = doc.material_map.empty();

    Arena arena{}, scratch{};
    Buffer output{}, env_payload{};
    MappedFile obj_file{};
    MaterialMap material_map{};
    ObjData obj{};
    EnvBuild env{};
    EnvView view{};
    Sounds sounds{};
    TextureSet textures{};
    ModuleMetric* metrics = nullptr;
    bool ok = arena_init(&arena, cfg.arena_reserve, &err) && arena_init(&scratch, cfg.arena_reserve, &err) &&
              map_file(cfg.obj, &obj_file, &err) && parse_obj(&obj_file, &obj, &arena, &err);
    if (ok)
        ok = validate_spawn_clearance(doc, obj, cfg, &err);
    if (ok)
        ok = buffer_init(&output, cfg.output_reserve, &err) &&
             load_material_map(cfg, &material_map, &arena, &err) &&
             build_env(cfg, obj, material_map, &arena, &scratch, &env, &err);
    if (ok) {
        env_payload = env.payload;
        ok = env_view(env_payload, &view, &arena, &err) && view.module_count &&
             view.module_count <= kMaxAabbTreeObjects;
        if (!ok && !err.set)
            fail(&err, "generated environment has an invalid module count");
    }
    if (ok) {
        metrics = arena_array<ModuleMetric>(&arena, view.module_count, &err);
        ok = metrics && make_editor_sounds(doc, &sounds, &arena, &err);
    }
    if (ok) {
        buffer_append(&output, kAsuraMagic, sizeof(kAsuraMagic), &err);
        ok = append_fnfo(&output, &err) && append_rsfl(&output, &err) &&
             append_weapon_support(&output, cfg, &scratch, &err) &&
             append_static_object_support(&output, doc, nullptr, nullptr, nullptr, &err) &&
             append_sky_resources(&output, cfg, &scratch, &err) &&
             append_textures(&output, cfg, view, material_map, &arena, &scratch, &textures, &err) &&
             append_rscf(&output, str_from_c(cfg.env_name), ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC,
                         ASURA_RESOURCEFILE_TYPE_PC_ENVIRONMENT, env_payload.base,
                         static_cast<uint32_t>(env_payload.size), &err) &&
             append_sound_resources(&output, sounds, &scratch, &err) &&
             (doc.ambient_stream_path.empty() || append_editor_ambience(&output, doc, &err)) &&
             append_editor_lights(&output, doc, &err) &&
             append_phon(&output, sounds, &err) &&
             append_emod(&output, view, view.module_count, cfg, material_map,
                         &scratch, metrics, &err) &&
             append_mlin(&output, metrics, view.module_count, &err) &&
             append_mrvb(&output, view.module_count, &err) && append_nav1(&output, view.module_count, &err) &&
              append_sound_entities(&output, sounds, &err) && append_editor_spawnpoints(&output, doc, &err) &&
               append_editor_pickups(&output, doc, &err) && append_editor_static_objects(&output, doc, &err) &&
               append_editor_building_volumes(&output, doc, &err) &&
               append_editor_skybox(&output, doc.skybox, &err) &&
             append_fog(&output, &err) && append_editor_weather(&output, doc, &err) &&
             buffer_append(&output, nullptr, sizeof(Asura_Chunk_Header), &err) != ~0ull &&
             patch_fnfo_file_size(&output, &err) &&
             write_entire_file(output_path, output.base, output.size, &err);
    }
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


DirectX::XMFLOAT4 material_map_color(uint32_t key) {
    // Stable, high-contrast debug colours. Equal material/surface types keep
    // equal colours while unrelated types remain distinguishable without a
    // texture. The shader still multiplies this diagnostic tint by the exact
    // baked vertex diffuse so authored prelighting remains visible.
    uint32_t mixed = key + 0x9e3779b9u;
    mixed ^= mixed >> 16;
    mixed *= 0x7feb352du;
    mixed ^= mixed >> 15;
    mixed *= 0x846ca68bu;
    mixed ^= mixed >> 16;
    constexpr float scale = 0.45f / 255.0f;
    return {.45f + ((mixed >> 16) & 0xff) * scale,
            .45f + ((mixed >> 8) & 0xff) * scale,
            .45f + (mixed & 0xff) * scale, 1.0f};
}

static bool environment_material_bindings(const ChunkList& chunks,
                                          const RscfInfo& environment,
                                          const char* platform,
                                          std::vector<PcEnvironmentMaterialBinding>* output,
                                          Error* err) {
    output->clear();

    std::vector<Str> texture_names;
    std::vector<uint32_t> texture_flags;
    bool reached_environment = false;
    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        const ChunkRef& chunk = chunks.chunks[chunk_index];
        if (chunk.cid == ASURA_CHUNK_TEXTURENAMES) {
            if (chunk.version > 3)
                return fail(err, "the active %s TEXT chunk uses unsupported version %u", platform, chunk.version);
            if (chunk.size < sizeof(Asura_Chunk_TextureNames))
                return fail(err, "the active %s TEXT chunk is truncated", platform);
            const uint32_t count = read_u32(chunk.data + sizeof(Asura_Chunk_Header));
            if (count > chunk.size - sizeof(Asura_Chunk_TextureNames))
                return fail(err, "the active %s TEXT count exceeds its chunk", platform);
            texture_names.clear();
            texture_names.reserve(count);
            texture_flags.assign(count, 0);
            uint64_t at = sizeof(Asura_Chunk_TextureNames);
            for (uint32_t texture_index = 0; texture_index < count; ++texture_index) {
                if (at > chunk.size)
                    return fail(err, "the active %s TEXT string table is truncated", platform);
                const Str name = padded_string_at(chunk.data, chunk.size, static_cast<uint32_t>(at));
                if (!name.data)
                    return fail(err, "the active %s TEXT string table is unterminated", platform);
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
                return fail(err, "the active %s TXFL chunk uses unsupported version %u", platform, chunk.version);
            const uint64_t values_at = sizeof(Asura_Chunk_Header) + sizeof(uint32_t);
            if (chunk.size < values_at)
                return fail(err, "the active %s TXFL chunk is truncated", platform);
            const uint32_t count = read_u32(chunk.data + sizeof(Asura_Chunk_Header));
            if (count > (chunk.size - values_at) / sizeof(uint32_t) || count > texture_flags.size())
                return fail(err, "the active %s TXFL table exceeds the active TEXT table", platform);
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
                return fail(err, "the active %s MTRL chunk uses unsupported version %u", platform, chunk.version);
            if (chunk.size < sizeof(Asura_Chunk_Header) + sizeof(uint32_t))
                return fail(err, "the active %s MTRL chunk is truncated", platform);
            const uint32_t count = read_u32(chunk.data + sizeof(Asura_Chunk_Header));
            const uint32_t stride = chunk.version ? sizeof(Asura_PC_Material_V1) : 8u;
            const uint64_t records_at = sizeof(Asura_Chunk_Header) + sizeof(uint32_t);
            if (count > (chunk.size - records_at) / stride)
                return fail(err, "the active %s MTRL record table is truncated", platform);
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
        return fail(err, "the %s environment resource is absent from the chunk stream", platform);
    return true;
}

bool pc_environment_material_bindings(const ChunkList& chunks,
                                      std::vector<PcEnvironmentMaterialBinding>* output, Error* err) {
    RscfInfo environment{};
    if (!find_pc_environment(chunks, &environment))
        return fail(err, "the .PC contains no PC environment RSCF");
    return environment_material_bindings(chunks, environment, "PC", output, err);
}

bool ps2_environment_material_bindings(const ChunkList& chunks,
                                       std::vector<PcEnvironmentMaterialBinding>* output, Error* err) {
    RscfInfo environment{};
    if (!find_ps2_environment(chunks, &environment))
        return fail(err, "the .PS2 contains no PS2 environment RSCF");
    return environment_material_bindings(chunks, environment, "PS2", output, err);
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

bool write_obj_texture(const std::string& path, const RscfInfo& resource, Error* err) {
    if (resource.payload_size < 4 || memcmp(resource.payload, "DDS ", 4) != 0)
        return fail(err, "environment texture resource is not DDS data: %s", path.c_str());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return fail(err, "could not create OBJ texture: %s", path.c_str());
    file.write(reinterpret_cast<const char*>(resource.payload), resource.payload_size);
    if (!file)
        return fail(err, "could not write OBJ texture: %s", path.c_str());
    return true;
}

} // namespace

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
    std::unordered_map<std::string, std::string> extracted_texture_names;
    std::unordered_map<std::string, std::string> texture_filename_owners;
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
            const auto extracted = extracted_texture_names.find(source_key);
            if (extracted != extracted_texture_names.end()) {
                texture_name = extracted->second;
            } else {
                const Str original_name = texture.name.size ? texture.name : material.texture_name;
                const std::string texture_stem = obj_texture_stem(original_name);
                texture_name = texture_stem + ".dds";
                uint32_t suffix = 2;
                while (texture_filename_owners.count(
                           obj_texture_filename_key(texture_name)) != 0) {
                    texture_name = texture_stem + "_" + std::to_string(suffix++) + ".dds";
                }

                const std::string texture_path = obj_export_join(texture_folder, texture_name);
                if (!write_obj_texture(texture_path, texture, &err)) {
                    ok = false;
                    break;
                }
                extracted_texture_names.emplace(source_key, texture_name);
                texture_filename_owners.emplace(obj_texture_filename_key(texture_name),
                                                source_key);
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
                                    std::vector<uint32_t>* output, Error* err) {
    output->assign(material_count, 0);
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

    std::vector<std::unordered_map<uint16_t, uint32_t>> frequencies(material_count);
    for (uint32_t module_index = 0; module_index < module_count; ++module_index) {
        if (!advance_pc_padded_string(*module_list, &at, err))
            return false;
        if (at > module_list->size ||
            sizeof(Asura_Chunk_Environment_ModuleList_EntryV6) > module_list->size - at)
            return fail(err, "the PC EMOD module %u is truncated", module_index);

        const uint8_t* entry = module_list->data + at;
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
            if (material == 0xffff)
                continue;
            const uint32_t ordinal = material >= 1000
                                         ? static_cast<uint32_t>(material) - 1000
                                         : material;
            if (ordinal < material_count)
                ++frequencies[ordinal][flags];
        }
    }
    if (at != module_list->size)
        return fail(err, "the PC EMOD collision table has %llu trailing bytes",
                    static_cast<unsigned long long>(module_list->size - at));

    // Retail collision flags may vary polygon-by-polygon even when polygons
    // share a render material. collision_flags is material-level, so choose
    // the mode: it exactly preserves the largest possible number of source
    // polygons. Prefer the lower mask on a tie for deterministic output.
    for (uint32_t material = 0; material < material_count; ++material) {
        uint32_t best_count = 0;
        uint16_t best_flags = 0;
        for (const auto& [flags, count] : frequencies[material]) {
            if (count > best_count || (count == best_count && flags < best_flags)) {
                best_count = count;
                best_flags = flags;
            }
        }
        (*output)[material] = best_flags;
    }
    return true;
}

bool pc_environment_material_chunk_indices(const ChunkList& source, uint32_t* text_chunk_index,
                                           uint32_t* material_chunk_index, Error* err) {
    RscfInfo environment{};
    if (!find_pc_environment(source, &environment))
        return fail(err, "the .PC contains no PC environment RSCF");

    uint32_t last_text = 0xffffffffu;
    uint32_t active_text = 0xffffffffu;
    uint32_t active_material = 0xffffffffu;
    bool reached_environment = false;
    for (uint32_t i = 0; i < source.count; ++i) {
        const ChunkRef& chunk = source.chunks[i];
        RscfInfo resource{};
        if (rscf_info(chunk, &resource) && resource.payload == environment.payload) {
            reached_environment = true;
            break;
        }
        if (chunk.cid == ASURA_CHUNK_TEXTURENAMES) {
            last_text = i;
            if (chunk.version < 3) {
                active_text = i;
                active_material = 0xffffffffu;
            }
        } else if (chunk.cid == ASURA_CHUNK_MATERIAL) {
            if (last_text == 0xffffffffu)
                return fail(err, "the PC environment material table has no preceding TEXT chunk");
            active_text = last_text;
            active_material = i;
        }
    }
    if (!reached_environment)
        return fail(err, "the PC environment resource is absent from the chunk stream");
    if (active_text == 0xffffffffu)
        return fail(err, "the PC environment has no material table to override");
    *text_chunk_index = active_text;
    *material_chunk_index = active_material;
    return true;
}

bool append_pc_material_map_override(Buffer* out, const ChunkList& source,
                                     const char* material_map_path, Arena* arena, Error* err) {
    MaterialMap material_map{};
    Config config{};
    config.material_map = material_map_path;
    std::vector<PcEnvironmentMaterialBinding> source_materials;
    bool ok = load_material_map(config, &material_map, arena, err) &&
              pc_environment_material_bindings(source, &source_materials, err);
    if (ok && source_materials.empty())
        ok = fail(err, "the PC environment has no material bindings to override");

    std::vector<Str> texture_names;
    std::vector<int32_t> texture_indices;
    std::vector<uint32_t> material_flags;
    std::vector<uint32_t> texture_flags;
    std::vector<uint32_t> surface_types;
    if (ok) {
        texture_names.resize(source_materials.size());
        texture_indices.resize(source_materials.size());
        material_flags.resize(source_materials.size());
        texture_flags.resize(source_materials.size());
        surface_types.resize(source_materials.size());
        for (uint32_t material_index = 0;
             material_index < static_cast<uint32_t>(source_materials.size()); ++material_index) {
            const PcEnvironmentMaterialBinding& source_material = source_materials[material_index];
            const Str mapped_name = material_texture_name(material_map, material_index);
            texture_names[material_index] = mapped_name.size ? mapped_name : source_material.texture_name;
            texture_indices[material_index] = texture_names[material_index].size
                                                  ? static_cast<int32_t>(material_index)
                                                  : -1;
            material_flags[material_index] = material_override(
                material_map, "transparency_flag_by_material_index", material_index,
                source_material.flags);
            texture_flags[material_index] = source_material.texture_flags;
            surface_types[material_index] = material_override(
                material_map, "surface_type_by_material_index", material_index,
                source_material.surface_type);
        }
    }

    const uint32_t count = static_cast<uint32_t>(source_materials.size());
    if (ok) {
        ChunkMark text = begin_chunk(out, ASURA_CHUNK_TEXTURENAMES, 3, 0, err);
        ok = append_u32(out, count, err) != ~0ull;
        for (uint32_t i = 0; ok && i < count; ++i)
            ok = append_padded_cstr(out, texture_names[i], err);
        ok = ok && end_chunk(out, text, err);
    }
    if (ok) {
        ChunkMark txfl = begin_chunk(out, ASURA_CHUNK_TEXTUREFLAGS, 1, 0, err);
        ok = append_u32(out, count, err) != ~0ull;
        for (uint32_t i = 0; ok && i < count; ++i)
            ok = append_u32(out, texture_flags[i], err) != ~0ull;
        ok = ok && end_chunk(out, txfl, err);
    }
    if (ok) {
        ChunkMark mtrl = begin_chunk(out, ASURA_CHUNK_MATERIAL, 1, 0, err);
        ok = append_u32(out, count, err) != ~0ull;
        for (uint32_t i = 0; ok && i < count; ++i) {
            ok = append_u32(out, texture_indices[i] < 0
                                     ? 0xffffffffu
                                     : static_cast<uint32_t>(texture_indices[i]), err) != ~0ull &&
                 append_u32(out, material_flags[i], err) != ~0ull &&
                 append_u32(out, surface_types[i], err) != ~0ull;
        }
        ok = ok && end_chunk(out, mtrl, err);
    }
    unmap_file(&material_map.file);
    return ok;
}


} // namespace editor
