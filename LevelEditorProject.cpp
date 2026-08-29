#include "LevelEditorProject.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

using namespace asura;

namespace editor {
namespace {

constexpr char kProjectMagic[8] = {'A', 'L', 'E', 'V', '2', '0', '0', '5'};
constexpr uint32_t kProjectVersion = 15;

struct BinaryWriter {
    std::vector<uint8_t> bytes;

    void raw(const void* data, size_t size) {
        const size_t offset = bytes.size();
        bytes.resize(offset + size);
        if (size)
            memcpy(bytes.data() + offset, data, size);
    }

    void u32(uint32_t value) { raw(&value, sizeof(value)); }
    void f32(float value) { raw(&value, sizeof(value)); }

    void str(const std::string& value) {
        u32(static_cast<uint32_t>(value.size()));
        raw(value.data(), value.size());
    }
};

struct BinaryReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t at = 0;
    bool ok = true;

    bool raw(void* output, size_t byte_count) {
        if (!ok || byte_count > size - at) {
            ok = false;
            return false;
        }
        if (byte_count)
            memcpy(output, data + at, byte_count);
        at += byte_count;
        return true;
    }

    uint32_t u32() {
        uint32_t value = 0;
        raw(&value, sizeof(value));
        return value;
    }

    float f32() {
        float value = 0;
        raw(&value, sizeof(value));
        return value;
    }

    std::string str() {
        const uint32_t byte_count = u32();
        if (!ok || byte_count > 64 * MiB || byte_count > size - at) {
            ok = false;
            return {};
        }
        std::string value(reinterpret_cast<const char*>(data + at), byte_count);
        at += byte_count;
        return value;
    }
};

void write_vec3(BinaryWriter& writer, const Asura_Vector_3& value) {
    writer.f32(value.x);
    writer.f32(value.y);
    writer.f32(value.z);
}

Asura_Vector_3 read_vec3(BinaryReader& reader) {
    return {reader.f32(), reader.f32(), reader.f32()};
}

void write_light(BinaryWriter& writer, const Asura_Light& light) {
    write_vec3(writer, light.Position);
    write_vec3(writer, light.Direction);
    writer.f32(light.R);
    writer.f32(light.G);
    writer.f32(light.B);
    writer.f32(light.Brightness);
    writer.f32(light.Range);
    writer.f32(light.m_fInnerRange);
    writer.f32(light.Angle);
    writer.f32(light.ShadowStrength);
    writer.f32(light.m_xBoundingBox.MinX);
    writer.f32(light.m_xBoundingBox.MaxX);
    writer.f32(light.m_xBoundingBox.MinY);
    writer.f32(light.m_xBoundingBox.MaxY);
    writer.f32(light.m_xBoundingBox.MinZ);
    writer.f32(light.m_xBoundingBox.MaxZ);
    writer.u32(light.m_uFlags);
    writer.f32(light.BrightnessOverRange);
    write_vec3(writer, light.OldPosition);
    writer.f32(light.OldRange);
    writer.u32(light.HasChanged ? 1u : 0u);
}

Asura_Light read_light(BinaryReader& reader) {
    Asura_Light light{};
    light.Position = read_vec3(reader);
    light.Direction = read_vec3(reader);
    light.R = reader.f32();
    light.G = reader.f32();
    light.B = reader.f32();
    light.Brightness = reader.f32();
    light.Range = reader.f32();
    light.m_fInnerRange = reader.f32();
    light.Angle = reader.f32();
    light.ShadowStrength = reader.f32();
    light.m_xBoundingBox.MinX = reader.f32();
    light.m_xBoundingBox.MaxX = reader.f32();
    light.m_xBoundingBox.MinY = reader.f32();
    light.m_xBoundingBox.MaxY = reader.f32();
    light.m_xBoundingBox.MinZ = reader.f32();
    light.m_xBoundingBox.MaxZ = reader.f32();
    light.m_uFlags = reader.u32();
    light.BrightnessOverRange = reader.f32();
    light.OldPosition = read_vec3(reader);
    light.OldRange = reader.f32();
    light.HasChanged = reader.u32() != 0;
    return light;
}

void write_phonon(BinaryWriter& writer, const Asura_Chunk_Phonons_PhononDataV9& phonon) {
    writer.u32(phonon.m_uSoundResourceID);
    write_vec3(writer, phonon.m_xPosition);
    writer.f32(phonon.m_fInnerRadius);
    writer.f32(phonon.m_fOuterRadius);
    for (float value : phonon.m_afLegacyVolumeParameters)
        writer.f32(value);
    writer.u32(phonon.m_uFlags);
    write_vec3(writer, phonon.m_xInnerCuboidRadius);
    write_vec3(writer, phonon.m_xOuterCuboidRadius);
    writer.u32(phonon.m_uGuid);
    writer.f32(phonon.m_xRetriggerBoundingBox.MinX);
    writer.f32(phonon.m_xRetriggerBoundingBox.MaxX);
    writer.f32(phonon.m_xRetriggerBoundingBox.MinY);
    writer.f32(phonon.m_xRetriggerBoundingBox.MaxY);
    writer.f32(phonon.m_xRetriggerBoundingBox.MinZ);
    writer.f32(phonon.m_xRetriggerBoundingBox.MaxZ);
    writer.f32(phonon.m_xOrient.x);
    writer.f32(phonon.m_xOrient.y);
    writer.f32(phonon.m_xOrient.z);
    writer.f32(phonon.m_xOrient.w);
}

Asura_Chunk_Phonons_PhononDataV9 read_phonon(BinaryReader& reader) {
    Asura_Chunk_Phonons_PhononDataV9 phonon{};
    phonon.m_uSoundResourceID = reader.u32();
    phonon.m_xPosition = read_vec3(reader);
    phonon.m_fInnerRadius = reader.f32();
    phonon.m_fOuterRadius = reader.f32();
    for (float& value : phonon.m_afLegacyVolumeParameters)
        value = reader.f32();
    phonon.m_uFlags = reader.u32();
    phonon.m_xInnerCuboidRadius = read_vec3(reader);
    phonon.m_xOuterCuboidRadius = read_vec3(reader);
    phonon.m_uGuid = reader.u32();
    phonon.m_xRetriggerBoundingBox.MinX = reader.f32();
    phonon.m_xRetriggerBoundingBox.MaxX = reader.f32();
    phonon.m_xRetriggerBoundingBox.MinY = reader.f32();
    phonon.m_xRetriggerBoundingBox.MaxY = reader.f32();
    phonon.m_xRetriggerBoundingBox.MinZ = reader.f32();
    phonon.m_xRetriggerBoundingBox.MaxZ = reader.f32();
    phonon.m_xOrient = {reader.f32(), reader.f32(), reader.f32(), reader.f32()};
    return phonon;
}

void skip_legacy_project_records(BinaryReader& reader) {
    const uint32_t count = reader.u32();
    if (count > 4096) {
        reader.ok = false;
        return;
    }
    for (uint32_t index = 0; index < count && reader.ok; ++index) {
        reader.str();
        reader.str();
        reader.u32();
        reader.u32();
        reader.u32();
        reader.u32();
        const uint32_t size = reader.u32();
        if (!reader.ok || size > 32 * MiB || size > reader.size - reader.at) {
            reader.ok = false;
            return;
        }
        reader.at += size;
    }
}

} // namespace

Asura_Light legacy_editor_light(const Entity& entity) {
    Asura_Light light{};
    light.Position = light.OldPosition = entity.position;
    // The 2005 PC runtime evaluates LITE records as omnidirectional point
    // sources. Keep the serialized legacy direction at its target default;
    // editor rotation must not imply a cone or direction transform.
    light.Direction = {0.0f, 0.0f, 1.0f};
    light.R = 255.0f;
    light.G = 244.0f;
    light.B = 220.0f;
    light.Brightness = entity.value_a;
    light.Range = light.OldRange = entity.value_b;
    light.Angle = 360.0f;
    light.m_uFlags = ASURA_LIGHT_FLAG_AFFECTS_ENTITIES;
    return light;
}

bool save_project(const Document& document, const char* path, std::string* why) {
    BinaryWriter writer;
    writer.raw(kProjectMagic, sizeof(kProjectMagic));
    writer.u32(kProjectVersion);
    writer.str(document.obj_path);
    writer.str(document.source_pc_path);
    writer.str(document.output_path);
    writer.str(document.material_map);
    writer.str(document.texture_dir);
    writer.str(document.weapons_donor);
    writer.str(document.sky_texture_dir);
    writer.u32(document.next_guid);
    write_vec3(writer, document.light_header_a);
    write_vec3(writer, document.light_header_b);
    write_vec3(writer, document.light_header_c);
    writer.u32(document.light_header_flag);
    writer.u32(document.source_pickup_inventory_complete ? 1u : 0u);
    writer.u32(static_cast<uint32_t>(document.pickup_templates.size()));
    for (const PickupTemplate& pickup : document.pickup_templates) {
        writer.u32(pickup.item_id);
        writer.f32(pickup.health);
        writer.u32(pickup.file_id);
        writer.u32(pickup.skin_id);
        writer.u32(pickup.anim_id);
        writer.u32(pickup.anim_file_id);
        writer.u32(pickup.entity_padding);
        writer.raw(pickup.body.data(), pickup.body.size());
    }
    writer.u32(document.skybox.source_record ? 1u : 0u);
    writer.f32(document.skybox.red);
    writer.f32(document.skybox.green);
    writer.f32(document.skybox.blue);
    writer.f32(document.skybox.orientation_radians);
    for (const std::string& texture_path : document.skybox.texture_paths)
        writer.str(texture_path);
    writer.u32(document.skybox.draw_clouds ? 1u : 0u);
    writer.u32(document.skybox.back_texture_is_front_upside_down ? 1u : 0u);
    writer.u32(document.skybox.right_texture_is_left_upside_down ? 1u : 0u);
    writer.u32(document.skybox.chunk_version);
    writer.u32(document.rain_enabled ? 1u : 0u);
    writer.u32(document.weather_source_record ? 1u : 0u);
    writer.str(document.ambient_stream_path);
    writer.f32(document.ambient_volume);
    writer.u32(document.ambient_source_record ? 1u : 0u);
    writer.u32(document.source_static_object_inventory_complete ? 1u : 0u);
    writer.u32(document.source_barrier_inventory_complete ? 1u : 0u);
    writer.u32(static_cast<uint32_t>(document.object_donors.size()));
    for (const std::string& donor : document.object_donors)
        writer.str(donor);
    writer.u32(static_cast<uint32_t>(document.static_object_templates.size()));
    for (const StaticObjectTemplate& object : document.static_object_templates) {
        writer.u32(object.file_id);
        writer.str(object.resource_name);
        writer.str(object.donor_path);
        writer.u32(object.entity_padding);
        writer.raw(object.body.data(), object.body.size());
    }
    writer.u32(static_cast<uint32_t>(document.entities.size()));
    for (const Entity& entity : document.entities) {
        writer.u32(static_cast<uint32_t>(entity.kind));
        writer.str(entity.name);
        write_vec3(writer, entity.position);
        write_vec3(writer, entity.rotation);
        writer.u32(entity.guid);
        writer.f32(entity.value_a);
        writer.f32(entity.value_b);
        writer.u32(entity.value_u32_a);
        writer.u32(entity.value_u32_b);
        writer.str(entity.sound_name);
        writer.str(entity.sound_file);
        if (entity.kind == EntityKind::Sound)
            writer.u32(entity.sound_loop ? 1u : 0u);
        if (entity.kind == EntityKind::Light) {
            Asura_Light light = entity.light;
            light.Position = entity.position;
            write_light(writer, light);
        }
        if (entity.kind == EntityKind::SpawnPoint) {
            writer.u32(entity.spawn_source_record ? 1u : 0u);
            writer.u32(static_cast<uint32_t>(entity.spawn_index));
            writer.u32(static_cast<uint32_t>(entity.spawn_posture));
            writer.f32(entity.spawn_timer);
            write_vec3(writer, entity.spawn_direction);
            writer.u32(entity.entity_padding);
        }
        if (entity.kind == EntityKind::Sound) {
            writer.u32(entity.sound_source_record ? 1u : 0u);
            writer.u32(entity.sound_has_controller ? 1u : 0u);
            writer.u32(entity.sound_controller_active ? 1u : 0u);
            writer.u32(entity.sound_controller_padding);
            write_phonon(writer, entity.sound_phonon);
        }
        if (entity.kind == EntityKind::Pickup || entity.kind == EntityKind::AssassinationTarget ||
            entity.kind == EntityKind::PositionMarker || entity.kind == EntityKind::StaticObject ||
            entity.kind == EntityKind::BuildingVolume || entity.kind == EntityKind::InvisibleBarrier) {
            writer.u32(entity.source_entity_record ? 1u : 0u);
            writer.u32(entity.source_entity_classification);
            writer.f32(entity.source_bounds.MinX);
            writer.f32(entity.source_bounds.MaxX);
            writer.f32(entity.source_bounds.MinY);
            writer.f32(entity.source_bounds.MaxY);
            writer.f32(entity.source_bounds.MinZ);
            writer.f32(entity.source_bounds.MaxZ);
        }
        if (entity.kind == EntityKind::Pickup) {
            writer.u32(entity.pickup_has_template ? 1u : 0u);
            writer.u32(entity.pickup_skin_id);
            writer.u32(entity.pickup_anim_id);
            writer.u32(entity.pickup_anim_file_id);
            writer.raw(entity.pickup_body.data(), entity.pickup_body.size());
        }
        if (entity.kind == EntityKind::StaticObject) {
            writer.u32(entity.static_object_has_template ? 1u : 0u);
            writer.raw(entity.static_object_body.data(), entity.static_object_body.size());
        }
        if (entity.kind == EntityKind::InvisibleBarrier) {
            writer.u32(entity.barrier_source_record ? 1u : 0u);
            writer.u32(entity.barrier_source_chunk);
            writer.u32(entity.barrier_source_module);
            writer.u32(entity.barrier_source_component);
            writer.u32(entity.barrier_collision_flags);
            writer.u32(entity.barrier_collision_material);
        }
    }

    Error error{};
    if (!write_entire_file(path, writer.bytes.data(), writer.bytes.size(), &error)) {
        if (why)
            *why = error.message;
        return false;
    }
    return true;
}

bool load_project(Document* document, const char* path, std::string* why) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        if (why)
            *why = "Could not open the editor project.";
        return false;
    }
    const std::streamoff end = file.tellg();
    if (end < 12 || end > static_cast<std::streamoff>(512 * MiB)) {
        if (why)
            *why = "The editor project has an invalid size.";
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), end);
    BinaryReader reader{bytes.data(), bytes.size()};
    char magic[8]{};
    reader.raw(magic, sizeof(magic));
    const uint32_t project_version = reader.u32();
    if (memcmp(magic, kProjectMagic, sizeof(magic)) != 0 || project_version < 1 ||
        project_version > kProjectVersion) {
        if (why)
            *why = "This is not a supported Asura Level Editor project.";
        return false;
    }

    Document next;
    next.project_path = path;
    next.obj_path = reader.str();
    if (project_version >= 7)
        next.source_pc_path = reader.str();
    next.output_path = reader.str();
    next.material_map = reader.str();
    next.texture_dir = reader.str();
    if (project_version >= 4)
        next.weapons_donor = reader.str();
    if (project_version >= 6)
        next.sky_texture_dir = reader.str();
    next.next_guid = reader.u32();
    if (project_version >= 7) {
        next.light_header_a = read_vec3(reader);
        next.light_header_b = read_vec3(reader);
        next.light_header_c = read_vec3(reader);
        next.light_header_flag = reader.u32();
        if (project_version >= 9) {
            next.source_pickup_inventory_complete = reader.u32() != 0;
            const uint32_t pickup_template_count = reader.u32();
            if (pickup_template_count > 256)
                reader.ok = false;
            next.pickup_templates.reserve(reader.ok ? pickup_template_count : 0);
            for (uint32_t index = 0; index < pickup_template_count && reader.ok; ++index) {
                PickupTemplate pickup;
                pickup.item_id = reader.u32();
                pickup.health = reader.f32();
                pickup.file_id = reader.u32();
                pickup.skin_id = reader.u32();
                pickup.anim_id = reader.u32();
                pickup.anim_file_id = reader.u32();
                pickup.entity_padding = static_cast<uint16_t>(reader.u32());
                reader.raw(pickup.body.data(), pickup.body.size());
                next.pickup_templates.push_back(std::move(pickup));
            }
        }
        if (project_version >= 10) {
            next.skybox.source_record = reader.u32() != 0;
            next.skybox.red = reader.f32();
            next.skybox.green = reader.f32();
            next.skybox.blue = reader.f32();
            next.skybox.orientation_radians = reader.f32();
            for (std::string& texture_path : next.skybox.texture_paths)
                texture_path = reader.str();
            next.skybox.draw_clouds = reader.u32() != 0;
            next.skybox.back_texture_is_front_upside_down = reader.u32() != 0;
            next.skybox.right_texture_is_left_upside_down = reader.u32() != 0;
            if (project_version >= 11) {
                next.skybox.chunk_version = reader.u32();
                next.rain_enabled = reader.u32() != 0;
                next.weather_source_record = reader.u32() != 0;
                if (next.skybox.chunk_version < 6 || next.skybox.chunk_version > 7)
                    reader.ok = false;
                if (project_version >= 12) {
                    next.ambient_stream_path = reader.str();
                    next.ambient_volume = reader.f32();
                    next.ambient_source_record = reader.u32() != 0;
                    if (next.ambient_stream_path.size() > 4096 ||
                        next.ambient_stream_path.find('\0') != std::string::npos ||
                        !std::isfinite(next.ambient_volume) || next.ambient_volume < 0.0f ||
                        next.ambient_volume > 1.0f)
                        reader.ok = false;
                }
            }
        }
        if (project_version >= 13) {
            next.source_static_object_inventory_complete = reader.u32() != 0;
            if (project_version >= 15)
                next.source_barrier_inventory_complete = reader.u32() != 0;
            const uint32_t donor_count = reader.u32();
            if (donor_count > 256)
                reader.ok = false;
            next.object_donors.reserve(reader.ok ? donor_count : 0);
            for (uint32_t index = 0; index < donor_count && reader.ok; ++index)
                next.object_donors.push_back(reader.str());
            const uint32_t object_template_count = reader.u32();
            if (object_template_count > 100000)
                reader.ok = false;
            next.static_object_templates.reserve(reader.ok ? object_template_count : 0);
            for (uint32_t index = 0; index < object_template_count && reader.ok; ++index) {
                StaticObjectTemplate object;
                object.file_id = reader.u32();
                object.resource_name = reader.str();
                object.donor_path = reader.str();
                object.entity_padding = static_cast<uint16_t>(reader.u32());
                reader.raw(object.body.data(), object.body.size());
                next.static_object_templates.push_back(std::move(object));
            }
        }
    }
    if (project_version <= 4)
        skip_legacy_project_records(reader);

    const uint32_t entity_count = reader.u32();
    if (entity_count > 100000)
        reader.ok = false;
    next.entities.reserve(reader.ok ? entity_count : 0);
    bool skipped_legacy_entities = false;
    for (uint32_t index = 0; index < entity_count && reader.ok; ++index) {
        Entity entity;
        const uint32_t kind = reader.u32();
        const uint32_t maximum_kind = project_version <= 4
                                          ? 3u
                                          : project_version <= 7
                                                ? static_cast<uint32_t>(EntityKind::Sound)
                                                : project_version <= 12
                                                      ? static_cast<uint32_t>(EntityKind::PositionMarker)
                                                      : project_version <= 13
                                                            ? static_cast<uint32_t>(EntityKind::StaticObject)
                                                            : project_version <= 14
                                                                  ? static_cast<uint32_t>(EntityKind::BuildingVolume)
                                                                  : static_cast<uint32_t>(EntityKind::InvisibleBarrier);
        if (kind > maximum_kind)
            reader.ok = false;
        const bool supported = kind <= (project_version <= 7
                                             ? static_cast<uint32_t>(EntityKind::Sound)
                                             : project_version <= 12
                                                   ? static_cast<uint32_t>(EntityKind::PositionMarker)
                                                   : project_version <= 13
                                                         ? static_cast<uint32_t>(EntityKind::StaticObject)
                                                         : project_version <= 14
                                                               ? static_cast<uint32_t>(EntityKind::BuildingVolume)
                                                               : static_cast<uint32_t>(EntityKind::InvisibleBarrier));
        if (supported)
            entity.kind = static_cast<EntityKind>(kind);
        entity.name = reader.str();
        entity.position = read_vec3(reader);
        entity.rotation = read_vec3(reader);
        entity.guid = reader.u32();
        if (project_version <= 4)
            reader.u32();
        entity.value_a = reader.f32();
        entity.value_b = reader.f32();
        entity.value_u32_a = reader.u32();
        entity.value_u32_b = reader.u32();
        entity.sound_name = reader.str();
        entity.sound_file = reader.str();
        if (kind == static_cast<uint32_t>(EntityKind::Sound))
            entity.sound_loop = project_version >= 3 ? reader.u32() != 0 : true;
        if (kind == static_cast<uint32_t>(EntityKind::Light))
            entity.light = project_version >= 2 ? read_light(reader) : legacy_editor_light(entity);
        if (project_version >= 7 && kind == static_cast<uint32_t>(EntityKind::SpawnPoint)) {
            entity.spawn_source_record = reader.u32() != 0;
            entity.spawn_index = static_cast<int32_t>(reader.u32());
            entity.spawn_posture = static_cast<int32_t>(reader.u32());
            entity.spawn_timer = reader.f32();
            entity.spawn_direction = read_vec3(reader);
            entity.entity_padding = static_cast<uint16_t>(reader.u32());
        }
        if (project_version >= 7 && kind == static_cast<uint32_t>(EntityKind::Sound)) {
            entity.sound_source_record = reader.u32() != 0;
            entity.sound_has_controller = reader.u32() != 0;
            entity.sound_controller_active = reader.u32() != 0;
            entity.sound_controller_padding = static_cast<uint16_t>(reader.u32());
            entity.sound_phonon = read_phonon(reader);
        }
        if (project_version >= 8 && kind >= static_cast<uint32_t>(EntityKind::Pickup)) {
            entity.source_entity_record = reader.u32() != 0;
            entity.source_entity_classification = static_cast<uint16_t>(reader.u32());
            entity.source_bounds.MinX = reader.f32();
            entity.source_bounds.MaxX = reader.f32();
            entity.source_bounds.MinY = reader.f32();
            entity.source_bounds.MaxY = reader.f32();
            entity.source_bounds.MinZ = reader.f32();
            entity.source_bounds.MaxZ = reader.f32();
        }
        if (project_version >= 9 && kind == static_cast<uint32_t>(EntityKind::Pickup)) {
            entity.pickup_has_template = reader.u32() != 0;
            entity.pickup_skin_id = reader.u32();
            entity.pickup_anim_id = reader.u32();
            entity.pickup_anim_file_id = reader.u32();
            reader.raw(entity.pickup_body.data(), entity.pickup_body.size());
        }
        if (project_version >= 13 && kind == static_cast<uint32_t>(EntityKind::StaticObject)) {
            entity.static_object_has_template = reader.u32() != 0;
            reader.raw(entity.static_object_body.data(), entity.static_object_body.size());
        }
        if (project_version >= 15 && kind == static_cast<uint32_t>(EntityKind::InvisibleBarrier)) {
            entity.barrier_source_record = reader.u32() != 0;
            entity.barrier_source_chunk = reader.u32();
            entity.barrier_source_module = reader.u32();
            entity.barrier_source_component = reader.u32();
            entity.barrier_collision_flags = static_cast<uint16_t>(reader.u32());
            entity.barrier_collision_material = static_cast<uint16_t>(reader.u32());
        }
        if (supported)
            next.entities.push_back(std::move(entity));
        else
            skipped_legacy_entities = true;
    }
    if (!reader.ok || reader.at != reader.size) {
        if (why)
            *why = "The editor project is truncated or corrupt.";
        return false;
    }
    next.dirty = skipped_legacy_entities;
    *document = std::move(next);
    return true;
}

} // namespace editor
