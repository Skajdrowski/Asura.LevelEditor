#define ASURA_CONSTRUCT_NO_MAIN
#include "Construct.cpp"

#include <commdlg.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

namespace editor {

constexpr char kProjectMagic[8] = {'A', 'L', 'E', 'V', '2', '0', '0', '5'};
constexpr uint32_t kProjectVersion = 9;
constexpr size_t kPhysicalObjectBodySize = sizeof(Snipe_ServerEntity_PhysicalPickup_ChunkDataV0);
constexpr float kSpawnCollisionHalfWidth = 0.3f;
constexpr float kSpawnCollisionHeight = 1.8f;
constexpr float kSpawnCollisionVerticalOffset = 0.1f;

enum class EntityKind : uint32_t {
    SpawnPoint,
    Light,
    Sound,
    PhysicalObject,
    AssassinationTarget,
    PositionMarker,
};

struct Entity {
    EntityKind kind = EntityKind::SpawnPoint;
    std::string name;
    Asura_Vector_3 position{};
    Asura_Vector_3 rotation{}; // pitch, yaw, roll in degrees
    uint32_t guid = 0;
    float value_a = 0;
    float value_b = 0;
    uint32_t value_u32_a = 0;
    uint32_t value_u32_b = 0;
    std::string sound_name;
    std::string sound_file;
    bool sound_loop = true;
    Asura_Light light{};
    bool spawn_source_record = false;
    int32_t spawn_index = 0;
    int32_t spawn_posture = 0;
    float spawn_timer = 5.0f;
    Asura_Vector_3 spawn_direction{0, 0, 1};
    uint16_t entity_padding = 0;
    bool sound_source_record = false;
    bool sound_has_controller = false;
    bool sound_controller_active = true;
    uint16_t sound_controller_padding = 0x4974;
    Asura_Chunk_Phonons_PhononDataV9 sound_phonon{};
    // Source-backed ENTI classes whose transforms are patched in-place during
    // export. Their unreversed fields remain byte-for-byte from the source PC.
    bool source_entity_record = false;
    uint16_t source_entity_classification = 0;
    Asura_Bounding_Box source_bounds{};
    // Physical-object ENTI payload used both to resolve its embedded model and
    // as a byte-exact template when the editor creates another pickup.
    bool pickup_has_template = false;
    uint32_t pickup_skin_id = 0;
    uint32_t pickup_anim_id = 0;
    uint32_t pickup_anim_file_id = 0;
    std::array<uint8_t, kPhysicalObjectBodySize> pickup_body{};
};

struct PickupTemplate {
    uint32_t item_id = 0;
    float health = 0;
    uint32_t file_id = 0;
    uint32_t skin_id = 0;
    uint32_t anim_id = 0;
    uint32_t anim_file_id = 0;
    uint16_t entity_padding = 0;
    std::array<uint8_t, kPhysicalObjectBodySize> body{};
};

struct Document {
    std::string project_path;
    std::string obj_path;
    std::string source_pc_path;
    std::string output_path;
    std::string material_map;
    std::string texture_dir;
    std::string weapons_donor;
    std::string sky_texture_dir;
    std::vector<Entity> entities;
    std::vector<PickupTemplate> pickup_templates;
    uint32_t next_guid = kToolCreatedGuidFirst;
    Asura_Vector_3 light_header_a{80, 80, 80};
    Asura_Vector_3 light_header_b{.3f, .3f, .3f};
    Asura_Vector_3 light_header_c{130, 100, 50};
    uint32_t light_header_flag = 1;
    // True only when every source physical-object ENTI was imported. This
    // makes absence from entities an intentional deletion during export.
    bool source_pickup_inventory_complete = false;
    bool dirty = false;
};

struct Mesh {
    std::vector<Asura_Vector_3> positions;
    std::vector<std::array<uint32_t, 3>> faces;
    Asura_Vector_3 min{}, max{}, center{};
    float radius = 25.0f;
};

struct SpawnPuppetVertex {
    Asura_Vector_3 position{};
    Asura_Vector_3 normal{};
};

struct SpawnPuppet {
    std::string resource_name;
    std::vector<SpawnPuppetVertex> vertices;
    std::vector<std::array<uint16_t, 3>> faces;
    Asura_Vector_3 min{}, max{};
};

struct PickupModel {
    uint32_t skin_id = 0;
    SpawnPuppet mesh;
};

struct BinaryWriter {
    std::vector<uint8_t> bytes;
    void raw(const void* p, size_t n) {
        const size_t at = bytes.size();
        bytes.resize(at + n);
        if (n)
            memcpy(bytes.data() + at, p, n);
    }
    void u32(uint32_t v) { raw(&v, sizeof(v)); }
    void f32(float v) { raw(&v, sizeof(v)); }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
};

struct BinaryReader {
    const uint8_t* data = nullptr;
    size_t size = 0, at = 0;
    bool ok = true;
    bool raw(void* out, size_t n) {
        if (!ok || n > size - at) {
            ok = false;
            return false;
        }
        if (n)
            memcpy(out, data + at, n);
        at += n;
        return true;
    }
    uint32_t u32() {
        uint32_t v = 0;
        raw(&v, sizeof(v));
        return v;
    }
    float f32() {
        float v = 0;
        raw(&v, sizeof(v));
        return v;
    }
    std::string str() {
        const uint32_t n = u32();
        if (!ok || n > 64 * MiB || n > size - at) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data + at), n);
        at += n;
        return s;
    }
};

void write_vec3(BinaryWriter& w, const Asura_Vector_3& v) {
    w.f32(v.x);
    w.f32(v.y);
    w.f32(v.z);
}

Asura_Vector_3 read_vec3(BinaryReader& r) { return {r.f32(), r.f32(), r.f32()}; }

Asura_Vector_3 light_direction_from_rotation(const Asura_Vector_3& degrees) {
    constexpr float d2r = 3.14159265358979323846f / 180.0f;
    const float yaw = degrees.y * d2r;
    const float pitch = degrees.x * d2r;
    return {cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw)};
}

Asura_Light legacy_editor_light(const Entity& e) {
    Asura_Light light{};
    light.Position = light.OldPosition = e.position;
    light.Direction = light_direction_from_rotation(e.rotation);
    light.R = 255.0f;
    light.G = 244.0f;
    light.B = 220.0f;
    light.Brightness = e.value_a;
    light.Range = light.OldRange = e.value_b;
    light.Angle = 360.0f;
    light.m_uFlags = ASURA_LIGHT_FLAG_AFFECTS_ENTITIES;
    return light;
}

void write_light(BinaryWriter& w, const Asura_Light& light) {
    write_vec3(w, light.Position);
    write_vec3(w, light.Direction);
    w.f32(light.R);
    w.f32(light.G);
    w.f32(light.B);
    w.f32(light.Brightness);
    w.f32(light.Range);
    w.f32(light.m_fInnerRange);
    w.f32(light.Angle);
    w.f32(light.ShadowStrength);
    w.f32(light.m_xBoundingBox.MinX);
    w.f32(light.m_xBoundingBox.MaxX);
    w.f32(light.m_xBoundingBox.MinY);
    w.f32(light.m_xBoundingBox.MaxY);
    w.f32(light.m_xBoundingBox.MinZ);
    w.f32(light.m_xBoundingBox.MaxZ);
    w.u32(light.m_uFlags);
    w.f32(light.BrightnessOverRange);
    write_vec3(w, light.OldPosition);
    w.f32(light.OldRange);
    w.u32(light.HasChanged ? 1u : 0u);
}

Asura_Light read_light(BinaryReader& r) {
    Asura_Light light{};
    light.Position = read_vec3(r);
    light.Direction = read_vec3(r);
    light.R = r.f32();
    light.G = r.f32();
    light.B = r.f32();
    light.Brightness = r.f32();
    light.Range = r.f32();
    light.m_fInnerRange = r.f32();
    light.Angle = r.f32();
    light.ShadowStrength = r.f32();
    light.m_xBoundingBox.MinX = r.f32();
    light.m_xBoundingBox.MaxX = r.f32();
    light.m_xBoundingBox.MinY = r.f32();
    light.m_xBoundingBox.MaxY = r.f32();
    light.m_xBoundingBox.MinZ = r.f32();
    light.m_xBoundingBox.MaxZ = r.f32();
    light.m_uFlags = r.u32();
    light.BrightnessOverRange = r.f32();
    light.OldPosition = read_vec3(r);
    light.OldRange = r.f32();
    light.HasChanged = r.u32() != 0;
    return light;
}

void write_phonon(BinaryWriter& w, const Asura_Chunk_Phonons_PhononDataV9& phonon) {
    w.u32(phonon.m_uSoundResourceID);
    write_vec3(w, phonon.m_xPosition);
    w.f32(phonon.m_fInnerRadius);
    w.f32(phonon.m_fOuterRadius);
    for (float value : phonon.m_afLegacyVolumeParameters)
        w.f32(value);
    w.u32(phonon.m_uFlags);
    write_vec3(w, phonon.m_xInnerCuboidRadius);
    write_vec3(w, phonon.m_xOuterCuboidRadius);
    w.u32(phonon.m_uGuid);
    w.f32(phonon.m_xRetriggerBoundingBox.MinX);
    w.f32(phonon.m_xRetriggerBoundingBox.MaxX);
    w.f32(phonon.m_xRetriggerBoundingBox.MinY);
    w.f32(phonon.m_xRetriggerBoundingBox.MaxY);
    w.f32(phonon.m_xRetriggerBoundingBox.MinZ);
    w.f32(phonon.m_xRetriggerBoundingBox.MaxZ);
    w.f32(phonon.m_xOrient.x);
    w.f32(phonon.m_xOrient.y);
    w.f32(phonon.m_xOrient.z);
    w.f32(phonon.m_xOrient.w);
}

Asura_Chunk_Phonons_PhononDataV9 read_phonon(BinaryReader& r) {
    Asura_Chunk_Phonons_PhononDataV9 phonon{};
    phonon.m_uSoundResourceID = r.u32();
    phonon.m_xPosition = read_vec3(r);
    phonon.m_fInnerRadius = r.f32();
    phonon.m_fOuterRadius = r.f32();
    for (float& value : phonon.m_afLegacyVolumeParameters)
        value = r.f32();
    phonon.m_uFlags = r.u32();
    phonon.m_xInnerCuboidRadius = read_vec3(r);
    phonon.m_xOuterCuboidRadius = read_vec3(r);
    phonon.m_uGuid = r.u32();
    phonon.m_xRetriggerBoundingBox.MinX = r.f32();
    phonon.m_xRetriggerBoundingBox.MaxX = r.f32();
    phonon.m_xRetriggerBoundingBox.MinY = r.f32();
    phonon.m_xRetriggerBoundingBox.MaxY = r.f32();
    phonon.m_xRetriggerBoundingBox.MinZ = r.f32();
    phonon.m_xRetriggerBoundingBox.MaxZ = r.f32();
    phonon.m_xOrient = {r.f32(), r.f32(), r.f32(), r.f32()};
    return phonon;
}

bool save_project(const Document& doc, const char* path, std::string* why) {
    BinaryWriter w;
    w.raw(kProjectMagic, sizeof(kProjectMagic));
    w.u32(kProjectVersion);
    w.str(doc.obj_path);
    w.str(doc.source_pc_path);
    w.str(doc.output_path);
    w.str(doc.material_map);
    w.str(doc.texture_dir);
    w.str(doc.weapons_donor);
    w.str(doc.sky_texture_dir);
    w.u32(doc.next_guid);
    write_vec3(w, doc.light_header_a);
    write_vec3(w, doc.light_header_b);
    write_vec3(w, doc.light_header_c);
    w.u32(doc.light_header_flag);
    w.u32(doc.source_pickup_inventory_complete ? 1u : 0u);
    w.u32(static_cast<uint32_t>(doc.pickup_templates.size()));
    for (const PickupTemplate& pickup : doc.pickup_templates) {
        w.u32(pickup.item_id);
        w.f32(pickup.health);
        w.u32(pickup.file_id);
        w.u32(pickup.skin_id);
        w.u32(pickup.anim_id);
        w.u32(pickup.anim_file_id);
        w.u32(pickup.entity_padding);
        w.raw(pickup.body.data(), pickup.body.size());
    }
    w.u32(static_cast<uint32_t>(doc.entities.size()));
    for (const Entity& e : doc.entities) {
        w.u32(static_cast<uint32_t>(e.kind));
        w.str(e.name);
        write_vec3(w, e.position);
        write_vec3(w, e.rotation);
        w.u32(e.guid);
        w.f32(e.value_a);
        w.f32(e.value_b);
        w.u32(e.value_u32_a);
        w.u32(e.value_u32_b);
        w.str(e.sound_name);
        w.str(e.sound_file);
        if (e.kind == EntityKind::Sound)
            w.u32(e.sound_loop ? 1u : 0u);
        if (e.kind == EntityKind::Light) {
            Asura_Light light = e.light;
            light.Position = e.position;
            write_light(w, light);
        }
        if (e.kind == EntityKind::SpawnPoint) {
            w.u32(e.spawn_source_record ? 1u : 0u);
            w.u32(static_cast<uint32_t>(e.spawn_index));
            w.u32(static_cast<uint32_t>(e.spawn_posture));
            w.f32(e.spawn_timer);
            write_vec3(w, e.spawn_direction);
            w.u32(e.entity_padding);
        }
        if (e.kind == EntityKind::Sound) {
            w.u32(e.sound_source_record ? 1u : 0u);
            w.u32(e.sound_has_controller ? 1u : 0u);
            w.u32(e.sound_controller_active ? 1u : 0u);
            w.u32(e.sound_controller_padding);
            write_phonon(w, e.sound_phonon);
        }
        if (e.kind == EntityKind::PhysicalObject || e.kind == EntityKind::AssassinationTarget ||
            e.kind == EntityKind::PositionMarker) {
            w.u32(e.source_entity_record ? 1u : 0u);
            w.u32(e.source_entity_classification);
            w.f32(e.source_bounds.MinX);
            w.f32(e.source_bounds.MaxX);
            w.f32(e.source_bounds.MinY);
            w.f32(e.source_bounds.MaxY);
            w.f32(e.source_bounds.MinZ);
            w.f32(e.source_bounds.MaxZ);
        }
        if (e.kind == EntityKind::PhysicalObject) {
            w.u32(e.pickup_has_template ? 1u : 0u);
            w.u32(e.pickup_skin_id);
            w.u32(e.pickup_anim_id);
            w.u32(e.pickup_anim_file_id);
            w.raw(e.pickup_body.data(), e.pickup_body.size());
        }
    }
    Error err{};
    if (!write_entire_file(path, w.bytes.data(), w.bytes.size(), &err)) {
        if (why)
            *why = err.message;
        return false;
    }
    return true;
}

void skip_legacy_project_records(BinaryReader& r) {
    const uint32_t count = r.u32();
    if (count > 4096) {
        r.ok = false;
        return;
    }
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        r.str();
        r.str();
        r.u32();
        r.u32();
        r.u32();
        r.u32();
        const uint32_t size = r.u32();
        if (!r.ok || size > 32 * MiB || size > r.size - r.at) {
            r.ok = false;
            return;
        }
        r.at += size;
    }
}

bool load_project(Document* doc, const char* path, std::string* why) {
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
    BinaryReader r{bytes.data(), bytes.size()};
    char magic[8]{};
    r.raw(magic, sizeof(magic));
    const uint32_t project_version = r.u32();
    if (memcmp(magic, kProjectMagic, sizeof(magic)) != 0 || project_version < 1 ||
        project_version > kProjectVersion) {
        if (why)
            *why = "This is not a supported Asura Level Editor project.";
        return false;
    }
    Document next;
    next.project_path = path;
    next.obj_path = r.str();
    if (project_version >= 7)
        next.source_pc_path = r.str();
    next.output_path = r.str();
    next.material_map = r.str();
    next.texture_dir = r.str();
    if (project_version >= 4)
        next.weapons_donor = r.str();
    if (project_version >= 6)
        next.sky_texture_dir = r.str();
    next.next_guid = r.u32();
    if (project_version >= 7) {
        next.light_header_a = read_vec3(r);
        next.light_header_b = read_vec3(r);
        next.light_header_c = read_vec3(r);
        next.light_header_flag = r.u32();
        if (project_version >= 9) {
            next.source_pickup_inventory_complete = r.u32() != 0;
            const uint32_t pickup_template_count = r.u32();
            if (pickup_template_count > 256)
                r.ok = false;
            next.pickup_templates.reserve(r.ok ? pickup_template_count : 0);
            for (uint32_t i = 0; i < pickup_template_count && r.ok; ++i) {
                PickupTemplate pickup;
                pickup.item_id = r.u32();
                pickup.health = r.f32();
                pickup.file_id = r.u32();
                pickup.skin_id = r.u32();
                pickup.anim_id = r.u32();
                pickup.anim_file_id = r.u32();
                pickup.entity_padding = static_cast<uint16_t>(r.u32());
                r.raw(pickup.body.data(), pickup.body.size());
                next.pickup_templates.push_back(std::move(pickup));
            }
        }
    }
    if (project_version <= 4)
        skip_legacy_project_records(r);
    const uint32_t entity_count = r.u32();
    if (entity_count > 100000)
        r.ok = false;
    next.entities.reserve(r.ok ? entity_count : 0);
    bool skipped_legacy_entities = false;
    for (uint32_t i = 0; i < entity_count && r.ok; ++i) {
        Entity e;
        const uint32_t kind = r.u32();
        const uint32_t maximum_kind = project_version <= 4
                                          ? 3u
                                          : project_version <= 7
                                                ? static_cast<uint32_t>(EntityKind::Sound)
                                                : static_cast<uint32_t>(EntityKind::PositionMarker);
        if (kind > maximum_kind)
            r.ok = false;
        const bool supported = kind <= (project_version <= 7 ? static_cast<uint32_t>(EntityKind::Sound)
                                                             : static_cast<uint32_t>(EntityKind::PositionMarker));
        if (supported)
            e.kind = static_cast<EntityKind>(kind);
        e.name = r.str();
        e.position = read_vec3(r);
        e.rotation = read_vec3(r);
        e.guid = r.u32();
        if (project_version <= 4)
            r.u32();
        e.value_a = r.f32();
        e.value_b = r.f32();
        e.value_u32_a = r.u32();
        e.value_u32_b = r.u32();
        e.sound_name = r.str();
        e.sound_file = r.str();
        if (kind == static_cast<uint32_t>(EntityKind::Sound))
            e.sound_loop = project_version >= 3 ? r.u32() != 0 : true;
        if (kind == static_cast<uint32_t>(EntityKind::Light))
            e.light = project_version >= 2 ? read_light(r) : legacy_editor_light(e);
        if (project_version >= 7 && kind == static_cast<uint32_t>(EntityKind::SpawnPoint)) {
            e.spawn_source_record = r.u32() != 0;
            e.spawn_index = static_cast<int32_t>(r.u32());
            e.spawn_posture = static_cast<int32_t>(r.u32());
            e.spawn_timer = r.f32();
            e.spawn_direction = read_vec3(r);
            e.entity_padding = static_cast<uint16_t>(r.u32());
        }
        if (project_version >= 7 && kind == static_cast<uint32_t>(EntityKind::Sound)) {
            e.sound_source_record = r.u32() != 0;
            e.sound_has_controller = r.u32() != 0;
            e.sound_controller_active = r.u32() != 0;
            e.sound_controller_padding = static_cast<uint16_t>(r.u32());
            e.sound_phonon = read_phonon(r);
        }
        if (project_version >= 8 && kind >= static_cast<uint32_t>(EntityKind::PhysicalObject)) {
            e.source_entity_record = r.u32() != 0;
            e.source_entity_classification = static_cast<uint16_t>(r.u32());
            e.source_bounds.MinX = r.f32();
            e.source_bounds.MaxX = r.f32();
            e.source_bounds.MinY = r.f32();
            e.source_bounds.MaxY = r.f32();
            e.source_bounds.MinZ = r.f32();
            e.source_bounds.MaxZ = r.f32();
        }
        if (project_version >= 9 && kind == static_cast<uint32_t>(EntityKind::PhysicalObject)) {
            e.pickup_has_template = r.u32() != 0;
            e.pickup_skin_id = r.u32();
            e.pickup_anim_id = r.u32();
            e.pickup_anim_file_id = r.u32();
            r.raw(e.pickup_body.data(), e.pickup_body.size());
        }
        if (supported)
            next.entities.push_back(std::move(e));
        else
            skipped_legacy_entities = true;
    }
    if (!r.ok || r.at != r.size) {
        if (why)
            *why = "The editor project is truncated or corrupt.";
        return false;
    }
    next.dirty = skipped_legacy_entities;
    *doc = std::move(next);
    return true;
}

void finish_mesh_bounds(Mesh* mesh) {
    if (mesh->positions.empty()) {
        mesh->min = mesh->max = mesh->center = {};
        mesh->radius = 25.0f;
        return;
    }
    mesh->min = mesh->max = mesh->positions[0];
    for (const Asura_Vector_3& p : mesh->positions) {
        mesh->min.x = fminf(mesh->min.x, p.x);
        mesh->min.y = fminf(mesh->min.y, p.y);
        mesh->min.z = fminf(mesh->min.z, p.z);
        mesh->max.x = fmaxf(mesh->max.x, p.x);
        mesh->max.y = fmaxf(mesh->max.y, p.y);
        mesh->max.z = fmaxf(mesh->max.z, p.z);
    }
    mesh->center = {(mesh->min.x + mesh->max.x) * .5f, (mesh->min.y + mesh->max.y) * .5f,
                    (mesh->min.z + mesh->max.z) * .5f};
    const float dx = mesh->max.x - mesh->min.x, dy = mesh->max.y - mesh->min.y,
                dz = mesh->max.z - mesh->min.z;
    mesh->radius = fmaxf(5.0f, sqrtf(dx * dx + dy * dy + dz * dz) * .5f);
}

bool load_preview_mesh(const std::string& path, Mesh* mesh, std::string* why) {
    Error err{};
    Arena arena{};
    MappedFile file{};
    ObjData obj{};
    Config cfg{};
    char editor_arg[] = "LevelEditor";
    char out_arg[] = "preview.pc";
    char* args[] = {editor_arg, const_cast<char*>(path.c_str()), out_arg};
    bool ok = arena_init(&arena, sizeof(void*) == 4 ? 256 * MiB : 2 * GiB, &err) &&
               parse_cli(3, args, &cfg, &err) && map_file(path.c_str(), &file, &err) &&
               parse_obj(&file, &obj, &arena, &err);
    // Game Env vertices are Y/Z-flipped. Keep the authored Blender Y axis
    // upright in the viewport; pack_document still uses the game default.
    cfg.flip_y = false;
    Mesh next;
    if (ok) {
        next.positions.reserve(obj.position_count);
        for (uint32_t i = 0; i < obj.position_count; ++i)
            next.positions.push_back(transform_vec(obj.positions[i], cfg));
        next.faces.reserve(obj.face_count);
        for (uint32_t i = 0; i < obj.face_count; ++i) {
            const ObjFace& f = obj.faces[i];
            const int32_t a = resolve_obj_index(f.a.v, obj.position_count);
            const int32_t b = resolve_obj_index(f.b.v, obj.position_count);
            const int32_t c = resolve_obj_index(f.c.v, obj.position_count);
            if (a >= 0 && b >= 0 && c >= 0) {
                if (transform_reverses_winding(cfg))
                    next.faces.push_back(
                        {static_cast<uint32_t>(a), static_cast<uint32_t>(c), static_cast<uint32_t>(b)});
                else
                    next.faces.push_back(
                        {static_cast<uint32_t>(a), static_cast<uint32_t>(b), static_cast<uint32_t>(c)});
            }
        }
        finish_mesh_bounds(&next);
    }
    if (!ok && why)
        *why = err.set ? err.message : "Could not load the OBJ preview.";
    unmap_file(&file);
    arena_release(&arena);
    if (ok)
        *mesh = std::move(next);
    return ok;
}

Asura_Quat euler_quaternion(const Asura_Vector_3& degrees) {
    constexpr float d2r = 3.14159265358979323846f / 180.0f;
    const float hx = degrees.x * d2r * .5f, hy = degrees.y * d2r * .5f, hz = degrees.z * d2r * .5f;
    const float sx = sinf(hx), cx = cosf(hx), sy = sinf(hy), cy = cosf(hy), sz = sinf(hz), cz = cosf(hz);
    return {sx * cy * cz + cx * sy * sz, cx * sy * cz - sx * cy * sz,
            cx * cy * sz + sx * sy * cz, cx * cy * cz - sx * sy * sz};
}

Asura_Vector_3 quaternion_euler(const Asura_Quat& q) {
    constexpr float r2d = 180.0f / 3.14159265358979323846f;
    const float m11 = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    const float m12 = 2.0f * (q.x * q.y - q.z * q.w);
    const float m13 = 2.0f * (q.x * q.z + q.y * q.w);
    const float m22 = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    const float m23 = 2.0f * (q.y * q.z - q.x * q.w);
    const float m32 = 2.0f * (q.y * q.z + q.x * q.w);
    const float m33 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    Asura_Vector_3 degrees{};
    degrees.y = asinf(std::clamp(m13, -1.0f, 1.0f));
    if (fabsf(m13) < .9999999f) {
        degrees.x = atan2f(-m23, m33);
        degrees.z = atan2f(-m12, m11);
    } else {
        degrees.x = atan2f(m32, m22);
    }
    degrees.x *= r2d;
    degrees.y *= r2d;
    degrees.z *= r2d;
    return degrees;
}

Asura_Vector_3 matrix_euler(const float* m) {
    constexpr float r2d = 180.0f / 3.14159265358979323846f;
    Asura_Vector_3 degrees{};
    degrees.y = asinf(std::clamp(m[2], -1.0f, 1.0f));
    if (fabsf(m[2]) < .9999999f) {
        degrees.x = atan2f(-m[5], m[8]);
        degrees.z = atan2f(-m[1], m[0]);
    } else {
        degrees.x = atan2f(m[7], m[4]);
    }
    degrees.x *= r2d;
    degrees.y *= r2d;
    degrees.z *= r2d;
    return degrees;
}

Asura_Vector_3 direction_euler(const Asura_Vector_3& direction) {
    constexpr float r2d = 180.0f / 3.14159265358979323846f;
    const float length = sqrtf(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (length <= 1e-8f)
        return {};
    return {asinf(std::clamp(direction.y / length, -1.0f, 1.0f)) * r2d,
            atan2f(direction.x, direction.z) * r2d, 0};
}

std::string edited_pc_path(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    const size_t dot = path.find_last_of('.');
    const size_t stem_end = dot == std::string::npos || (slash != std::string::npos && dot < slash) ? path.size() : dot;
    return path.substr(0, stem_end) + "_edited.PC";
}

bool decode_pc_environment(const RscfInfo& resource, Mesh* mesh, Arena* arena, Error* err) {
    Buffer payload{};
    payload.base = const_cast<uint8_t*>(resource.payload);
    payload.size = payload.committed = payload.reserved = resource.payload_size;
    EnvView env{};
    if (!env_view(payload, &env, arena, err))
        return false;
    if (!env.module_count || !env.block_count)
        return fail(err, "PC environment contains no renderable modules");

    Mesh next;
    std::vector<uint32_t> block_vertex_base(env.block_count, 0xffffffffu);
    uint64_t total_vertices = 0, total_triangles = 0;
    for (uint32_t block_index = 0; block_index < env.block_count; ++block_index) {
        const uint32_t vertex_count = read_u32(env.blocks[block_index]);
        if (total_vertices + vertex_count > 0xffffffffull)
            return fail(err, "PC environment has too many vertices for the editor");
        block_vertex_base[block_index] = static_cast<uint32_t>(total_vertices);
        total_vertices += vertex_count;
    }
    for (uint32_t module_index = 0; module_index < env.module_count; ++module_index) {
        const Asura_PC_EnvironmentRenderer_Module& module = env.modules[module_index];
        if (module.m_uBufferIndex >= env.block_count ||
            static_cast<uint64_t>(module.m_uFirstStrip) + module.m_uNumberOfStrips > env.strip_count)
            return fail(err, "PC environment module %u has invalid strip or buffer references", module_index);
        for (uint32_t i = 0; i < module.m_uNumberOfStrips; ++i)
            total_triangles += env.strips[module.m_uFirstStrip + i].m_uNumberOfTriangles;
    }
    if (total_triangles > static_cast<uint64_t>(SIZE_MAX))
        return fail(err, "PC environment has too many triangles for the editor");
    next.positions.reserve(static_cast<size_t>(total_vertices));
    next.faces.reserve(static_cast<size_t>(total_triangles));
    for (uint32_t block_index = 0; block_index < env.block_count; ++block_index) {
        const uint8_t* block = env.blocks[block_index];
        const uint32_t vertex_count = read_u32(block);
        const uint8_t* vertices = block + 8;
        for (uint32_t vertex_index = 0; vertex_index < vertex_count; ++vertex_index) {
            const uint8_t* source = vertices + static_cast<uint64_t>(vertex_index) * sizeof(Asura_PC_EnvironmentRenderer_Vertex);
            const Asura_Vector_3 position{read_f32(source), -read_f32(source + 4), read_f32(source + 8)};
            if (!isfinite(position.x) || !isfinite(position.y) || !isfinite(position.z))
                return fail(err, "PC environment contains a non-finite vertex");
            next.positions.push_back(position);
        }
    }
    for (uint32_t module_index = 0; module_index < env.module_count; ++module_index) {
        const Asura_PC_EnvironmentRenderer_Module& module = env.modules[module_index];
        const uint8_t* block = env.blocks[module.m_uBufferIndex];
        const uint32_t vertex_count = read_u32(block), index_count = read_u32(block + 4);
        const uint8_t* indices = block + 8 + static_cast<uint64_t>(vertex_count) * sizeof(Asura_PC_EnvironmentRenderer_Vertex);
        const uint32_t vertex_base = block_vertex_base[module.m_uBufferIndex];
        for (uint32_t strip_index = 0; strip_index < module.m_uNumberOfStrips; ++strip_index) {
            const Asura_PC_EnvironmentRenderer_Strip& strip = env.strips[module.m_uFirstStrip + strip_index];
            if (static_cast<uint64_t>(strip.m_uStartIndex) + strip.m_uNumberOfTriangles + 2 > index_count)
                return fail(err, "PC environment strip exceeds its index buffer");
            for (uint32_t triangle = 0; triangle < strip.m_uNumberOfTriangles; ++triangle) {
                uint16_t a = read_u16(indices + static_cast<uint64_t>(strip.m_uStartIndex + triangle) * 2);
                uint16_t b = read_u16(indices + static_cast<uint64_t>(strip.m_uStartIndex + triangle + 1) * 2);
                uint16_t c = read_u16(indices + static_cast<uint64_t>(strip.m_uStartIndex + triangle + 2) * 2);
                if (triangle & 1)
                    std::swap(a, b);
                if (a == 0xffff || b == 0xffff || c == 0xffff || a >= vertex_count || b >= vertex_count ||
                    c >= vertex_count || a == b || b == c || a == c)
                    continue;
                // Negating the target's negative-up Y axis reverses handedness.
                next.faces.push_back({vertex_base + a, vertex_base + c, vertex_base + b});
            }
        }
    }
    if (next.faces.empty())
        return fail(err, "PC environment contains no renderable triangles");
    finish_mesh_bounds(&next);
    *mesh = std::move(next);
    return true;
}

const RscfInfo* find_pc_environment(const ChunkList& chunks, RscfInfo* storage) {
    bool have_fallback = false;
    RscfInfo fallback{};
    for (uint32_t i = 0; i < chunks.count; ++i) {
        RscfInfo resource{};
        if (!rscf_info(chunks.chunks[i], &resource) ||
            resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
            resource.subtype != ASURA_RESOURCEFILE_TYPE_PC_ENVIRONMENT)
            continue;
        if (str_ieq_c(resource.name, "Env")) {
            *storage = resource;
            return storage;
        }
        if (!have_fallback) {
            fallback = resource;
            have_fallback = true;
        }
    }
    if (!have_fallback)
        return nullptr;
    *storage = fallback;
    return storage;
}

std::string pc_sound_name(const ChunkList& chunks, uint32_t resource_id) {
    for (uint32_t i = 0; i < chunks.count; ++i) {
        RscfInfo resource{};
        if (rscf_info(chunks.chunks[i], &resource) && resource.type == ASURA_RESOURCEFILE_TYPE_SOUND &&
            resource.subtype == resource_id)
            return std::string(resource.name.data, resource.name.size);
    }
    return {};
}

const char* snipe_item_name(uint32_t item_id) {
    switch (item_id) {
    case SnipeItem_PistolAmmo: return "Pistol Ammo";
    case SnipeItem_RifleAmmo: return "Rifle Ammo";
    case SnipeItem_PPSHAmmo: return "PPSh Ammo";
    case SnipeItem_MP40Ammo: return "MP 40 Ammo";
    case SnipeItem_MG42Ammo: return "MG42 Ammo";
    case SnipeItem_DP28Ammo: return "DP 28 Ammo";
    case SnipeItem_Panzerfaust: return "Panzerfaust";
    case SnipeItem_StickGrenade: return "Stick Grenade";
    case SnipeItem_FragGrenade: return "Frag Grenade";
    case SnipeItem_SmokeGrenade: return "Smoke Grenade";
    case SnipeItem_Knife: return "Knife";
    case SnipeItem_MedKit: return "MedKit";
    case SnipeItem_Bandage: return "Bandage";
    case SnipeItem_TnT: return "TnT";
    case SnipeItem_Binoculars: return "Binoculars";
    case SnipeItem_Gewehr43: return "Gewehr 43";
    case SnipeItem_Mosin91: return "Mosin 91";
    case SnipeItem_SVT40: return "SVT-40";
    case SnipeItem_Luger: return "Luger";
    case SnipeItem_P38: return "P-38";
    case SnipeItem_PPSH: return "PPSh";
    case SnipeItem_MP40: return "MP 40";
    case SnipeItem_MG42: return "MG42";
    case SnipeItem_DP28: return "DP 28";
    case SnipeItem_TimeBomb: return "Time Bomb";
    case SnipeItem_Panzerschreck: return "Panzerschreck";
    case SnipeItem_TripWire: return "Trip Wire";
    case SnipeItem_PanzerschreckAmmo: return "Panzerschreck Ammo";
    default: return nullptr;
    }
}

std::string snipe_item_label(uint32_t item_id) {
    char label[96]{};
    const char* name = snipe_item_name(item_id);
    if (name)
        snprintf(label, sizeof(label), "%s (0x%02X)", name, item_id);
    else
        snprintf(label, sizeof(label), "Unknown item (0x%02X)", item_id);
    return label;
}

PickupTemplate make_canonical_pickup_template(uint32_t item_id, float health, uint32_t file_id,
                                              uint32_t skin_id, uint32_t anim_id, uint32_t anim_file_id,
                                              uint32_t state_bits = 0xc0u) {
    PickupTemplate pickup;
    pickup.item_id = item_id;
    pickup.health = health > 0.0f ? health : 100.0f;
    pickup.file_id = file_id;
    pickup.skin_id = skin_id;
    pickup.anim_id = anim_id;
    pickup.anim_file_id = anim_file_id;

    Snipe_ServerEntity_PhysicalPickup_ChunkDataV0 body{};
    body.m_iPickupVersion = 2;
    body.m_uPickupClassID = 999;
    body.m_uPickupFlags = 2;
    body.m_iAsuraPickupVersion = 2;
    body.m_uItemID = pickup.item_id;
    body.m_iStaticObjectVersion = 3;
    body.m_iAsuraStaticObjectVersion = 0;
    body.m_iPhysicalObjectVersion = 7;
    body.m_uTeam = 0;
    body.m_uSnipePhysicalFlags = 0;
    body.m_uSnipePhysicalPropertyA = 999;
    body.m_uSnipePhysicalPropertyB = 0;
    body.m_uSnipePhysicalPropertyC = 999;
    body.m_uSnipePhysicalPropertyD = 999;
    body.m_uSnipePhysicalPropertyE = 0;
    body.m_iAsuraPhysicalObjectVersion = 7;
    body.m_xPhysicalObject.m_xOrientation.w = 1.0f;
    body.m_xPhysicalObject.m_fHealth = pickup.health;
    body.m_xPhysicalObject.m_uFileID = pickup.file_id;
    body.m_xPhysicalObject.m_uSkinID = pickup.skin_id;
    body.m_xPhysicalObject.m_uAnimID = pickup.anim_id;
    body.m_xPhysicalObject.m_uAnimFileID = pickup.anim_file_id;
    body.m_xPhysicalObject.m_iAnimFlags = 1;
    body.m_xPhysicalObject.m_iBBIndex = -1;
    // Only the low ten state bits are initialized by the 2005 constructor.
    // Masking discards the uninitialized high bits found in a few retail files.
    body.m_xPhysicalObject.m_uStateBits = state_bits & 0x3ffu;
    if (!body.m_xPhysicalObject.m_uStateBits)
        body.m_xPhysicalObject.m_uStateBits = 0xc0u;
    body.m_xPhysicalObject.m_uPhysicalObjectFlags = 2;
    body.m_xPhysicalObject.m_fAnimTimer = 0.0f;
    body.m_uNumLinksToBlock = 0;
    memcpy(pickup.body.data(), &body, sizeof(body));
    return pickup;
}

PickupTemplate pickup_template_from_entity(const Entity& entity) {
    const uint32_t state_bits = entity.pickup_has_template
                                    ? read_u32(entity.pickup_body.data() +
                                               offsetof(Snipe_ServerEntity_PhysicalPickup_ChunkDataV0,
                                                        m_xPhysicalObject) +
                                               offsetof(Asura_ServerEntity_PhysicalObject_ChunkDataV7,
                                                        m_uStateBits))
                                    : 0xc0u;
    return make_canonical_pickup_template(entity.value_u32_a, entity.value_a, entity.value_u32_b,
                                          entity.pickup_skin_id, entity.pickup_anim_id,
                                          entity.pickup_anim_file_id, state_bits);
}

void note_pickup_template(Document* document, const Entity& entity) {
    for (const PickupTemplate& pickup : document->pickup_templates)
        if (pickup.item_id == entity.value_u32_a)
            return;
    document->pickup_templates.push_back(pickup_template_from_entity(entity));
}

uint32_t asura_lower_name_hash(Str name) {
    uint32_t hash = 0;
    for (uint32_t i = 0; i < name.size; ++i) {
        uint8_t c = static_cast<uint8_t>(name.data[i]);
        if (c >= 'A' && c <= 'Z')
            c = static_cast<uint8_t>(c + ('a' - 'A'));
        else if (c == '\\')
            c = '/';
        hash = hash * 31u + c;
    }
    return hash;
}

uint32_t asura_lower_name_hash(const std::string& name) {
    return asura_lower_name_hash(Str{name.data(), static_cast<uint32_t>(name.size())});
}

struct PickupResourceDefinition {
    uint32_t item_id;
    const char* model_name;
};

// Snipe's item-to-attachment conversion (MCP2 sub_50F840), supplemented by
// the retail mp_01a RSFL names for pickups which bypass that conversion.
constexpr PickupResourceDefinition kPickupResourceDefinitions[] = {
    {SnipeItem_PistolAmmo, "ammo_pistol"},
    {SnipeItem_RifleAmmo, "ammo_rifle"},
    {SnipeItem_PPSHAmmo, "ammo_drum"},
    {SnipeItem_MP40Ammo, "ammo_mp40"},
    {SnipeItem_MG42Ammo, "ammo_belt"},
    {SnipeItem_DP28Ammo, "ammo_dp28"},
    {SnipeItem_Panzerfaust, "Panzerfaust"},
    {SnipeItem_StickGrenade, "stickgrenade"},
    {SnipeItem_FragGrenade, "Pineapple"},
    {SnipeItem_SmokeGrenade, "smokegrenade"},
    {SnipeItem_Knife, "Knife"},
    {SnipeItem_MedKit, "smallmedkit"},
    {SnipeItem_Bandage, "bandage"},
    {SnipeItem_TnT, "tnt"},
    {SnipeItem_Binoculars, "Binoculars"},
    {SnipeItem_Gewehr43, "springfield"},
    {SnipeItem_Mosin91, "nagan_scope"},
    {SnipeItem_SVT40, "mauser_scope"},
    {SnipeItem_Luger, "Luger"},
    {SnipeItem_P38, "p38"},
    {SnipeItem_PPSH, "machgun"},
    {SnipeItem_MP40, "mp40"},
    {SnipeItem_MG42, "MG42"},
    {SnipeItem_DP28, "dp28"},
    {SnipeItem_TimeBomb, "tbomb"},
    {SnipeItem_Panzerschreck, "panzerschreck"},
    {SnipeItem_TripWire, "tripbomb"},
    {SnipeItem_PanzerschreckAmmo, "schreckrocket"},
};

uint32_t pickup_initial_state_bits(uint32_t item_id) {
    switch (item_id) {
    case SnipeItem_Panzerfaust:
    case SnipeItem_StickGrenade:
    case SnipeItem_FragGrenade:
    case SnipeItem_SmokeGrenade:
    case SnipeItem_TnT:
    case SnipeItem_TimeBomb:
    case SnipeItem_TripWire:
    case SnipeItem_PanzerschreckAmmo:
        return 0xc1u;
    default:
        return 0xc0u;
    }
}

bool donor_has_pickup_model(const ChunkList& chunks, uint32_t skin_id) {
    for (uint32_t i = 0; i < chunks.count; ++i) {
        RscfInfo resource{};
        if (rscf_info(chunks.chunks[i], &resource) &&
            resource.type == ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC &&
            resource.subtype == ASURA_RESOURCEFILE_TYPE_PC_OBJECTHIERARCHY &&
            asura_lower_name_hash(resource.name) == skin_id)
            return true;
    }
    return false;
}

PickupTemplate pickup_template_from_resource(const PickupResourceDefinition& definition) {
    const std::string model_name = definition.model_name;
    const std::string rest_name = model_name + "_rest";
    const std::string model_file = "Characters/" + model_name + "/" + model_name + ".asr";
    const std::string anim_file = "Characters/" + model_name + "/Anims/" + rest_name + ".asr";
    return make_canonical_pickup_template(
        definition.item_id, 100.0f, asura_lower_name_hash(model_file), asura_lower_name_hash(model_name),
        asura_lower_name_hash(rest_name), asura_lower_name_hash(anim_file),
        pickup_initial_state_bits(definition.item_id));
}

void add_resource_backed_pickup_templates(const ChunkList& chunks, Document* catalog) {
    for (const PickupResourceDefinition& definition : kPickupResourceDefinitions) {
        bool already_present = false;
        for (const PickupTemplate& existing : catalog->pickup_templates)
            already_present |= existing.item_id == definition.item_id;
        if (already_present)
            continue;

        PickupTemplate generated = pickup_template_from_resource(definition);
        if (donor_has_pickup_model(chunks, generated.skin_id))
            catalog->pickup_templates.push_back(std::move(generated));
    }
}

bool pickup_skin_is_referenced(const Document& document, uint32_t skin_id) {
    for (const Entity& entity : document.entities)
        if (entity.kind == EntityKind::PhysicalObject && entity.pickup_skin_id == skin_id)
            return true;
    for (const PickupTemplate& pickup : document.pickup_templates)
        if (pickup.skin_id == skin_id)
            return true;
    return false;
}

bool decode_pc_pickup_model(const RscfInfo& resource, uint32_t skin_id, PickupModel* output, Error* err) {
    if (resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
        resource.subtype != ASURA_RESOURCEFILE_TYPE_PC_OBJECTHIERARCHY ||
        asura_lower_name_hash(resource.name) != skin_id)
        return false;
    const Str embedded_name = padded_string_at(resource.payload, resource.payload_size, 0);
    if (!embedded_name.data || asura_lower_name_hash(embedded_name) != skin_id)
        return fail(err, "pickup ObjectHierarchy '%.*s' has a mismatched embedded name",
                    resource.name.size, resource.name.data);
    const uint64_t counts_at = align_up(static_cast<uint64_t>(embedded_name.size) + 1, 4);
    if (counts_at + 12 > resource.payload_size)
        return fail(err, "pickup ObjectHierarchy '%.*s' is truncated", resource.name.size, resource.name.data);
    const uint32_t strip_count = read_u32(resource.payload + counts_at);
    const uint32_t vertex_count = read_u32(resource.payload + counts_at + 4);
    const uint32_t index_count = read_u32(resource.payload + counts_at + 8);
    constexpr uint32_t strip_stride = 20;
    constexpr uint32_t vertex_stride = 32;
    const uint64_t strips_at = counts_at + 12;
    const uint64_t vertices_at = strips_at + static_cast<uint64_t>(strip_count) * strip_stride;
    const uint64_t indices_at = vertices_at + static_cast<uint64_t>(vertex_count) * vertex_stride;
    const uint64_t required = indices_at + static_cast<uint64_t>(index_count) * sizeof(uint16_t);
    if (!strip_count || strip_count > 65535 || !vertex_count || vertex_count > 65535 || index_count < 3 ||
        required > resource.payload_size)
        return fail(err, "pickup ObjectHierarchy '%.*s' has invalid counts", resource.name.size,
                    resource.name.data);

    PickupModel next;
    next.skin_id = skin_id;
    next.mesh.resource_name.assign(resource.name.data, resource.name.size);
    next.mesh.vertices.resize(vertex_count);
    for (uint32_t i = 0; i < vertex_count; ++i) {
        const uint8_t* source = resource.payload + vertices_at + static_cast<uint64_t>(i) * vertex_stride;
        SpawnPuppetVertex& vertex = next.mesh.vertices[i];
        vertex.position = {read_f32(source), read_f32(source + 4), read_f32(source + 8)};
        vertex.normal = {read_f32(source + 12), read_f32(source + 16), read_f32(source + 20)};
        if (!isfinite(vertex.position.x) || !isfinite(vertex.position.y) || !isfinite(vertex.position.z) ||
            !isfinite(vertex.normal.x) || !isfinite(vertex.normal.y) || !isfinite(vertex.normal.z))
            return fail(err, "pickup ObjectHierarchy '%.*s' contains non-finite vertices",
                        resource.name.size, resource.name.data);
        const float normal_length = sqrtf(vertex.normal.x * vertex.normal.x + vertex.normal.y * vertex.normal.y +
                                          vertex.normal.z * vertex.normal.z);
        if (normal_length > 1.0e-5f) {
            vertex.normal.x /= normal_length;
            vertex.normal.y /= normal_length;
            vertex.normal.z /= normal_length;
        } else {
            vertex.normal = {0, -1, 0};
        }
        if (i == 0) {
            next.mesh.min = next.mesh.max = vertex.position;
        } else {
            next.mesh.min.x = fminf(next.mesh.min.x, vertex.position.x);
            next.mesh.min.y = fminf(next.mesh.min.y, vertex.position.y);
            next.mesh.min.z = fminf(next.mesh.min.z, vertex.position.z);
            next.mesh.max.x = fmaxf(next.mesh.max.x, vertex.position.x);
            next.mesh.max.y = fmaxf(next.mesh.max.y, vertex.position.y);
            next.mesh.max.z = fmaxf(next.mesh.max.z, vertex.position.z);
        }
    }

    const uint8_t* indices = resource.payload + indices_at;
    uint64_t triangle_capacity = 0;
    for (uint32_t strip_index = 0; strip_index < strip_count; ++strip_index)
        triangle_capacity += read_u32(resource.payload + strips_at + static_cast<uint64_t>(strip_index) * strip_stride);
    if (triangle_capacity > SIZE_MAX)
        return fail(err, "pickup ObjectHierarchy '%.*s' has too many triangles", resource.name.size,
                    resource.name.data);
    next.mesh.faces.reserve(static_cast<size_t>(triangle_capacity));
    for (uint32_t strip_index = 0; strip_index < strip_count; ++strip_index) {
        const uint8_t* strip = resource.payload + strips_at + static_cast<uint64_t>(strip_index) * strip_stride;
        const uint32_t triangle_count = read_u32(strip);
        const uint32_t start_index = read_u32(strip + 4);
        const uint32_t lowest_vertex = read_u32(strip + 12);
        const uint32_t number_vertices = read_u32(strip + 16);
        if (static_cast<uint64_t>(start_index) + triangle_count + 2 > index_count ||
            static_cast<uint64_t>(lowest_vertex) + number_vertices > vertex_count)
            return fail(err, "pickup ObjectHierarchy '%.*s' has an invalid strip", resource.name.size,
                        resource.name.data);
        for (uint32_t triangle = 0; triangle < triangle_count; ++triangle) {
            uint16_t a = read_u16(indices + static_cast<uint64_t>(start_index + triangle) * 2);
            uint16_t b = read_u16(indices + static_cast<uint64_t>(start_index + triangle + 1) * 2);
            uint16_t c = read_u16(indices + static_cast<uint64_t>(start_index + triangle + 2) * 2);
            if (triangle & 1)
                std::swap(a, b);
            if (a == 0xffff || b == 0xffff || c == 0xffff)
                continue;
            if (a >= vertex_count || b >= vertex_count || c >= vertex_count)
                return fail(err, "pickup ObjectHierarchy '%.*s' has an out-of-range index",
                            resource.name.size, resource.name.data);
            if (a != b && b != c && a != c)
                next.mesh.faces.push_back({a, b, c});
        }
    }
    if (next.mesh.faces.empty())
        return fail(err, "pickup ObjectHierarchy '%.*s' has no renderable triangles",
                    resource.name.size, resource.name.data);
    *output = std::move(next);
    return true;
}

bool decode_pc_pickup_models(const ChunkList& chunks, const Document& document,
                             std::vector<PickupModel>* models, Error* err) {
    models->clear();
    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        RscfInfo resource{};
        if (!rscf_info(chunks.chunks[chunk_index], &resource) ||
            resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
            resource.subtype != ASURA_RESOURCEFILE_TYPE_PC_OBJECTHIERARCHY)
            continue;
        const uint32_t skin_id = asura_lower_name_hash(resource.name);
        if (!pickup_skin_is_referenced(document, skin_id))
            continue;
        bool duplicate = false;
        for (const PickupModel& model : *models)
            duplicate |= model.skin_id == skin_id;
        if (duplicate)
            continue;
        PickupModel model;
        if (!decode_pc_pickup_model(resource, skin_id, &model, err))
            return false;
        models->push_back(std::move(model));
    }
    return true;
}

bool valid_physical_pickup_body(const Snipe_ServerEntity_PhysicalPickup_ChunkDataV0& body) {
    return body.m_iPickupVersion == 2 && body.m_iAsuraPickupVersion == 2 &&
           body.m_iStaticObjectVersion == 3 && body.m_iAsuraStaticObjectVersion == 0 &&
           body.m_iPhysicalObjectVersion == 7 && body.m_iAsuraPhysicalObjectVersion == 7;
}

bool load_pickup_donor(const std::string& path, std::vector<PickupTemplate>* templates,
                       std::vector<PickupModel>* models, std::string* why) {
    Error err{};
    Arena arena{};
    ChunkList chunks{};
    Document catalog;
    std::vector<PickupModel> next_models;
    bool ok = arena_init(&arena, 64 * MiB, &err) && parse_chunks(path.c_str(), &chunks, &arena, &err);
    for (uint32_t chunk_index = 0; ok && chunk_index < chunks.count; ++chunk_index) {
        const ChunkRef& chunk = chunks.chunks[chunk_index];
        if (chunk.cid != ASURA_CHUNK_ENTITY || chunk.size < sizeof(Asura_Chunk_Entity))
            continue;
        const uint8_t* payload = chunk.data + sizeof(Asura_Chunk_Header);
        const uint16_t classification =
            read_u16(payload + offsetof(Asura_Chunk_Entity_PayloadHeader, Classification));
        if (classification != SnipeEntityClass_PhysicalObject)
            continue;
        if (chunk.version != 0 ||
            chunk.size < sizeof(Asura_Chunk_Entity) + sizeof(Snipe_ServerEntity_PhysicalPickup_ChunkDataV0)) {
            ok = fail(&err, "physical-pickup ENTI chunk %u is truncated or unsupported", chunk_index);
            break;
        }
        Snipe_ServerEntity_PhysicalPickup_ChunkDataV0 body{};
        memcpy(&body, payload + sizeof(Asura_Chunk_Entity_PayloadHeader), sizeof(body));
        if (!valid_physical_pickup_body(body)) {
            ok = fail(&err, "physical-pickup ENTI chunk %u uses unsupported payload versions", chunk_index);
            break;
        }
        bool duplicate = false;
        for (const PickupTemplate& existing : catalog.pickup_templates)
            duplicate |= existing.item_id == body.m_uItemID;
        if (duplicate)
            continue;
        catalog.pickup_templates.push_back(make_canonical_pickup_template(
            body.m_uItemID, body.m_xPhysicalObject.m_fHealth, body.m_xPhysicalObject.m_uFileID,
            body.m_xPhysicalObject.m_uSkinID, body.m_xPhysicalObject.m_uAnimID,
            body.m_xPhysicalObject.m_uAnimFileID, body.m_xPhysicalObject.m_uStateBits));
    }
    if (ok)
        add_resource_backed_pickup_templates(chunks, &catalog);
    if (ok && catalog.pickup_templates.empty())
        ok = fail(&err, "the weapons donor contains no renderable 0x0008 pickup resources");
    if (ok)
        ok = decode_pc_pickup_models(chunks, catalog, &next_models, &err);
    if (ok) {
        for (const PickupTemplate& pickup : catalog.pickup_templates) {
            bool resolved = false;
            for (const PickupModel& model : next_models)
                resolved |= model.skin_id == pickup.skin_id;
            if (!resolved) {
                ok = fail(&err, "the weapons donor has no ObjectHierarchy model for item 0x%02X (skin %08X)",
                          pickup.item_id, pickup.skin_id);
                break;
            }
        }
    }
    if (ok) {
        *templates = std::move(catalog.pickup_templates);
        if (models)
            *models = std::move(next_models);
    } else if (why) {
        *why = err.set ? err.message : "Could not load pickup definitions from the weapons donor.";
    }
    unmap_file(&chunks.file);
    arena_release(&arena);
    return ok;
}

void note_document_guid(Document* document, uint32_t guid) {
    if (guid >= kToolCreatedGuidFirst && guid <= kToolCreatedGuidLast && guid >= document->next_guid)
        document->next_guid = guid + 1;
}

uint32_t allocate_editor_guid(Document* document) {
    uint32_t candidate = document->next_guid;
    if (candidate < kToolCreatedGuidFirst || candidate > kToolCreatedGuidLast)
        candidate = kToolCreatedGuidFirst;
    const uint32_t first_candidate = candidate;
    do {
        bool used = false;
        for (const Entity& entity : document->entities) {
            if (entity.guid == candidate) {
                used = true;
                break;
            }
        }
        if (!used) {
            document->next_guid = candidate == kToolCreatedGuidLast ? kToolCreatedGuidFirst : candidate + 1;
            return candidate;
        }
        candidate = candidate == kToolCreatedGuidLast ? kToolCreatedGuidFirst : candidate + 1;
    } while (candidate != first_candidate);
    return 0;
}

bool normalise_editor_guids(Document* document, std::string* why) {
    for (size_t index = 0; index < document->entities.size(); ++index) {
        Entity& entity = document->entities[index];
        // Lights have no ENTI GUID on disk. Source-backed records retain their
        // original IDs exactly; only editor-authored ENTI records are migrated.
        const bool has_enti_guid = entity.kind == EntityKind::SpawnPoint || entity.kind == EntityKind::Sound ||
                                   entity.kind == EntityKind::PhysicalObject;
        if (!has_enti_guid || entity.source_entity_record || entity.sound_source_record)
            continue;
        bool duplicate = false;
        for (size_t earlier = 0; earlier < index; ++earlier)
            duplicate |= document->entities[earlier].guid == entity.guid;
        if (!duplicate && entity.guid >= kToolCreatedGuidFirst && entity.guid <= kToolCreatedGuidLast)
            continue;
        const uint32_t replacement = allocate_editor_guid(document);
        if (!replacement) {
            if (why)
                *why = "No free target-valid GUIDs remain for editor-authored entities.";
            return false;
        }
        entity.guid = replacement;
        document->dirty = true;
    }
    return true;
}

bool import_pc_entities(const ChunkList& chunks, Document* document, Error* err) {
    uint32_t light_number = 0, sound_number = 0, spawn_number = 0, object_number = 0;
    uint32_t target_number = 0, marker_number = 0;
    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        const ChunkRef& chunk = chunks.chunks[chunk_index];
        if (chunk.cid != ASURA_CHUNK_LIGHTS)
            continue;
        if (chunk.version < 3 || chunk.version > 5 || chunk.size < 60)
            return fail(err, "LITE chunk %u has an unsupported version or size", chunk_index);
        const uint8_t* payload = chunk.data + sizeof(Asura_Chunk_Header);
        const uint32_t count = read_u32(payload);
        const uint64_t required = 60ull + static_cast<uint64_t>(count) * sizeof(Asura_Light);
        if (required > chunk.size)
            return fail(err, "LITE chunk %u is truncated", chunk_index);
        memcpy(&document->light_header_a, payload + 4, sizeof(Asura_Vector_3));
        memcpy(&document->light_header_b, payload + 16, sizeof(Asura_Vector_3));
        memcpy(&document->light_header_c, payload + 28, sizeof(Asura_Vector_3));
        document->light_header_flag = read_u32(payload + 40);
        const uint8_t* records = payload + 44;
        for (uint32_t i = 0; i < count; ++i) {
            Entity entity;
            entity.kind = EntityKind::Light;
            entity.name = "Light " + std::to_string(++light_number);
            memcpy(&entity.light, records + static_cast<uint64_t>(i) * sizeof(Asura_Light), sizeof(Asura_Light));
            entity.position = entity.light.Position;
            entity.value_a = entity.light.Brightness;
            entity.value_b = entity.light.Range;
            document->entities.push_back(std::move(entity));
        }
        break;
    }

    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        const ChunkRef& chunk = chunks.chunks[chunk_index];
        if (chunk.cid != ASURA_CHUNK_PHONONS)
            continue;
        if (chunk.version != 9 || chunk.size < 20)
            return fail(err, "PHON chunk %u has an unsupported version or size", chunk_index);
        const uint8_t* payload = chunk.data + sizeof(Asura_Chunk_Header);
        const uint32_t count = read_u32(payload);
        const uint64_t required = 20ull + static_cast<uint64_t>(count) * sizeof(Asura_Chunk_Phonons_PhononDataV9);
        if (required > chunk.size)
            return fail(err, "PHON chunk %u is truncated", chunk_index);
        const uint8_t* records = payload + 4;
        for (uint32_t i = 0; i < count; ++i) {
            Entity entity;
            entity.kind = EntityKind::Sound;
            entity.sound_source_record = true;
            memcpy(&entity.sound_phonon,
                   records + static_cast<uint64_t>(i) * sizeof(Asura_Chunk_Phonons_PhononDataV9),
                   sizeof(Asura_Chunk_Phonons_PhononDataV9));
            entity.position = entity.sound_phonon.m_xPosition;
            entity.rotation = quaternion_euler(entity.sound_phonon.m_xOrient);
            entity.value_a = entity.sound_phonon.m_fInnerRadius;
            entity.value_b = entity.sound_phonon.m_fOuterRadius;
            entity.sound_loop = (entity.sound_phonon.m_uFlags & 1u) != 0;
            entity.sound_name = pc_sound_name(chunks, entity.sound_phonon.m_uSoundResourceID);
            entity.name = entity.sound_name.empty() ? "Sound " + std::to_string(++sound_number) : entity.sound_name;
            document->entities.push_back(std::move(entity));
        }
        break;
    }

    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        const ChunkRef& chunk = chunks.chunks[chunk_index];
        if (chunk.cid != ASURA_CHUNK_ENTITY || chunk.size < sizeof(Asura_Chunk_Entity))
            continue;
        const uint8_t* payload = chunk.data + sizeof(Asura_Chunk_Header);
        note_document_guid(document, read_u32(payload));
        const uint16_t classification = read_u16(payload + offsetof(Asura_Chunk_Entity_PayloadHeader, Classification));
        if (classification == SnipeEntityClass_SpawnPoint) {
            if (chunk.version != 0 || chunk.size < sizeof(Asura_Chunk_Header) + sizeof(Snipe_ServerEntity_SpawnPoint_ChunkDataV0))
                return fail(err, "spawnpoint ENTI chunk %u is truncated or unsupported", chunk_index);
            Snipe_ServerEntity_SpawnPoint_ChunkDataV0 source{};
            memcpy(&source, payload, sizeof(source));
            if (source.m_iVersion != 0)
                return fail(err, "spawnpoint ENTI chunk %u has unsupported payload version %d", chunk_index,
                            source.m_iVersion);
            Entity entity;
            entity.kind = EntityKind::SpawnPoint;
            entity.name = "Spawn " + std::to_string(++spawn_number);
            entity.guid = source.m_xEntity.Guid;
            entity.entity_padding = source.m_xEntity.m_usPadding;
            entity.position = source.m_xPosition;
            entity.rotation = direction_euler(source.m_xDirection);
            entity.spawn_direction = source.m_xDirection;
            entity.value_u32_a = source.m_uTeamMask;
            entity.value_u32_b = source.m_uGameModeMask;
            entity.spawn_source_record = true;
            entity.spawn_index = source.m_iSpawnIndex;
            entity.spawn_posture = source.m_iPosture;
            entity.spawn_timer = source.m_fSpawnTimer;
            note_document_guid(document, entity.guid);
            document->entities.push_back(std::move(entity));
        } else if (classification == AsuraEntityClass_SoundController) {
            if (chunk.version != 0 ||
                chunk.size < sizeof(Asura_Chunk_Header) + sizeof(Asura_ServerEntity_SoundController_ChunkDataV0))
                return fail(err, "sound-controller ENTI chunk %u is truncated or unsupported", chunk_index);
            Asura_ServerEntity_SoundController_ChunkDataV0 source{};
            memcpy(&source, payload, sizeof(source));
            if (source.m_iActivatableVersion != 2 || source.m_iVersion != 0)
                return fail(err, "sound-controller ENTI chunk %u has unsupported payload versions", chunk_index);
            for (Entity& entity : document->entities) {
                if (entity.kind != EntityKind::Sound || !entity.sound_source_record ||
                    entity.sound_phonon.m_uGuid != source.m_uPhononGuid || entity.sound_has_controller)
                    continue;
                entity.guid = source.m_xEntity.Guid;
                entity.sound_has_controller = true;
                entity.sound_controller_active = source.m_bActive != 0;
                entity.sound_controller_padding = source.m_xEntity.m_usPadding;
                note_document_guid(document, entity.guid);
                break;
            }
        } else if (classification == SnipeEntityClass_PhysicalObject) {
            if (chunk.version != 0 || chunk.size < sizeof(Asura_Chunk_Entity) + kPhysicalObjectBodySize)
                return fail(err, "physical-object ENTI chunk %u is truncated or unsupported", chunk_index);
            const uint8_t* body = payload + sizeof(Asura_Chunk_Entity_PayloadHeader);
            Snipe_ServerEntity_PhysicalPickup_ChunkDataV0 pickup{};
            memcpy(&pickup, body, sizeof(pickup));
            if (!valid_physical_pickup_body(pickup))
                return fail(err, "physical-object ENTI chunk %u has unsupported payload versions", chunk_index);
            Entity entity;
            entity.kind = EntityKind::PhysicalObject;
            entity.guid = read_u32(payload);
            entity.source_entity_record = true;
            entity.source_entity_classification = classification;
            entity.entity_padding = read_u16(payload + offsetof(Asura_Chunk_Entity_PayloadHeader, m_usPadding));
            entity.value_u32_a = pickup.m_uItemID;
            entity.value_u32_b = pickup.m_xPhysicalObject.m_uFileID;
            entity.value_a = pickup.m_xPhysicalObject.m_fHealth;
            entity.pickup_skin_id = pickup.m_xPhysicalObject.m_uSkinID;
            entity.pickup_anim_id = pickup.m_xPhysicalObject.m_uAnimID;
            entity.pickup_anim_file_id = pickup.m_xPhysicalObject.m_uAnimFileID;
            memcpy(entity.pickup_body.data(), body, entity.pickup_body.size());
            entity.pickup_has_template = true;
            entity.position = pickup.m_xPhysicalObject.m_xPosition;
            entity.rotation = quaternion_euler(pickup.m_xPhysicalObject.m_xOrientation);
            const char* item_name = snipe_item_name(entity.value_u32_a);
            ++object_number;
            if (item_name) {
                entity.name = std::string(item_name) + " " + std::to_string(object_number);
            } else {
                char name[96]{};
                snprintf(name, sizeof(name), "Unknown item %u (0x%02X)", object_number, entity.value_u32_a);
                entity.name = name;
            }
            note_pickup_template(document, entity);
            document->entities.push_back(std::move(entity));
        } else if (classification == SnipeEntityClass_AssassinationTarget) {
            constexpr uint32_t body_size = 0x80;
            if (chunk.version != 0 || chunk.size < sizeof(Asura_Chunk_Entity) + body_size)
                return fail(err, "assassination-target ENTI chunk %u is truncated or unsupported", chunk_index);
            const uint8_t* body = payload + sizeof(Asura_Chunk_Entity_PayloadHeader);
            if (read_u32(body) != 0 || read_u32(body + 8) != 3 || read_u32(body + 0x10) != 0 ||
                read_u32(body + 0x14) != 7 || read_u32(body + 0x34) != 7)
                return fail(err, "assassination-target ENTI chunk %u has unsupported payload versions", chunk_index);
            Entity entity;
            entity.kind = EntityKind::AssassinationTarget;
            entity.guid = read_u32(payload);
            entity.source_entity_record = true;
            entity.source_entity_classification = classification;
            entity.value_a = read_f32(body + 0x54);
            entity.value_u32_a = classification;
            memcpy(&entity.position, body + 0x38, sizeof(entity.position));
            Asura_Quat orientation{};
            memcpy(&orientation, body + 0x44, sizeof(orientation));
            entity.rotation = quaternion_euler(orientation);
            entity.name = "Assassination target " + std::to_string(++target_number);
            document->entities.push_back(std::move(entity));
        } else if (classification == SnipeEntityClass_PositionMarker) {
            constexpr uint32_t body_size = 0x74;
            if (chunk.version != 0 || chunk.size < sizeof(Asura_Chunk_Entity) + body_size)
                return fail(err, "position-marker ENTI chunk %u is truncated or unsupported", chunk_index);
            const uint8_t* body = payload + sizeof(Asura_Chunk_Entity_PayloadHeader);
            if (read_u32(body) != 0)
                return fail(err, "position-marker ENTI chunk %u has unsupported payload version", chunk_index);
            Entity entity;
            entity.kind = EntityKind::PositionMarker;
            entity.guid = read_u32(payload);
            entity.source_entity_record = true;
            entity.source_entity_classification = classification;
            memcpy(&entity.source_bounds, body + 0x4c, sizeof(entity.source_bounds));
            memcpy(&entity.position, body + 0x64, sizeof(entity.position));
            float orientation[9]{};
            memcpy(orientation, body + 4, sizeof(orientation));
            entity.rotation = matrix_euler(orientation);
            entity.value_a = entity.source_bounds.MaxX - entity.source_bounds.MinX;
            entity.value_b = entity.source_bounds.MaxZ - entity.source_bounds.MinZ;
            entity.value_u32_a = read_u32(body + 0x70);
            entity.name = "Position marker " + std::to_string(++marker_number);
            document->entities.push_back(std::move(entity));
        }
    }
    document->source_pickup_inventory_complete = true;
    return true;
}

bool load_pc_level(const std::string& path, Document* document, Mesh* mesh, std::string* why,
                   std::vector<PickupModel>* pickup_models = nullptr) {
    Error err{};
    Arena arena{};
    ChunkList chunks{};
    Document next_document;
    Mesh next_mesh;
    std::vector<PickupModel> next_pickup_models;
    bool ok = arena_init(&arena, 64 * MiB, &err) && parse_chunks(path.c_str(), &chunks, &arena, &err);
    RscfInfo environment{};
    if (ok && !find_pc_environment(chunks, &environment))
        ok = fail(&err, "the .PC contains no PC environment RSCF");
    if (ok)
        ok = decode_pc_environment(environment, &next_mesh, &arena, &err) &&
             import_pc_entities(chunks, &next_document, &err);
    if (ok)
        add_resource_backed_pickup_templates(chunks, &next_document);
    if (ok && pickup_models)
        ok = decode_pc_pickup_models(chunks, next_document, &next_pickup_models, &err);
    if (ok) {
        next_document.source_pc_path = path;
        next_document.output_path = edited_pc_path(path);
        next_document.dirty = false;
        *document = std::move(next_document);
        *mesh = std::move(next_mesh);
        if (pickup_models)
            *pickup_models = std::move(next_pickup_models);
    } else if (why) {
        *why = err.set ? err.message : "Could not load the PC level.";
    }
    unmap_file(&chunks.file);
    arena_release(&arena);
    return ok;
}

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
        if (e.spawn_source_record) {
            data.m_xDirection = e.spawn_direction;
        } else {
            const float yaw = e.rotation.y * 3.14159265358979323846f / 180.0f;
            const float pitch = e.rotation.x * 3.14159265358979323846f / 180.0f;
            data.m_xDirection = {cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw)};
        }
        data.m_iSpawnIndex = e.spawn_source_record ? e.spawn_index : static_cast<int32_t>(index);
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

void make_pickup_body(const Entity& entity, std::array<uint8_t, kPhysicalObjectBodySize>* body) {
    Snipe_ServerEntity_PhysicalPickup_ChunkDataV0 wire{};
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
    for (const Entity& entity : doc.entities) {
        if (entity.kind != EntityKind::PhysicalObject || entity.source_entity_record)
            continue;
        if (!entity.pickup_has_template)
            return fail(err, "pickup '%s' has no resolved item asset profile", entity.name.c_str());
        ChunkMark chunk = begin_chunk(out, ASURA_CHUNK_ENTITY, 0, 0, err);
        Asura_Chunk_Entity_PayloadHeader header{};
        header.Guid = entity.guid;
        header.Classification = SnipeEntityClass_PhysicalObject;
        header.m_usPadding = entity.entity_padding;
        std::array<uint8_t, kPhysicalObjectBodySize> body{};
        make_pickup_body(entity, &body);
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
        if (classification == SnipeEntityClass_PhysicalObject && doc.source_pickup_inventory_complete)
            return true;
        return append_chunk_copy(out, chunk, err);
    }

    const uint8_t* body = payload + sizeof(Asura_Chunk_Entity_PayloadHeader);
    if (classification == SnipeEntityClass_PhysicalObject && entity->pickup_has_template) {
        if (chunk.size < sizeof(Asura_Chunk_Entity) + kPhysicalObjectBodySize)
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
        std::array<uint8_t, kPhysicalObjectBodySize> patched_body{};
        make_pickup_body(*entity, &patched_body);
        memcpy(patched.data() + sizeof(Asura_Chunk_Entity), patched_body.data(), patched_body.size());
        return buffer_append(out, patched.data(), patched.size(), err) != ~0ull;
    }

    uint32_t position_offset = 0, orientation_offset = 0;
    if (classification == SnipeEntityClass_PhysicalObject) {
        position_offset = 0x4c;
        orientation_offset = 0x58;
    } else if (classification == SnipeEntityClass_AssassinationTarget) {
        position_offset = 0x38;
        orientation_offset = 0x44;
    } else if (classification == SnipeEntityClass_PositionMarker) {
        position_offset = 0x64;
    } else {
        return append_chunk_copy(out, chunk, err);
    }
    if (sizeof(Asura_Chunk_Entity) + position_offset + sizeof(Asura_Vector_3) > chunk.size)
        return fail(err, "source ENTI 0x%04X is truncated", classification);

    Asura_Vector_3 source_position{};
    memcpy(&source_position, body + position_offset, sizeof(source_position));
    Asura_Vector_3 source_rotation{};
    if (classification == SnipeEntityClass_PositionMarker) {
        if (sizeof(Asura_Chunk_Entity) + 0x70 + sizeof(uint32_t) > chunk.size)
            return fail(err, "source position-marker ENTI is truncated");
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
    if (!position_changed && !rotation_changed)
        return append_chunk_copy(out, chunk, err);

    std::vector<uint8_t> patched(chunk.data, chunk.data + chunk.size);
    uint8_t* patched_body = patched.data() + sizeof(Asura_Chunk_Entity);
    if (position_changed) {
        if (classification == SnipeEntityClass_PositionMarker) {
            const Asura_Vector_3 delta{entity->position.x - source_position.x,
                                      entity->position.y - source_position.y,
                                      entity->position.z - source_position.z};
            Asura_Bounding_Box bounds{};
            memcpy(&bounds, body + 0x4c, sizeof(bounds));
            bounds.MinX += delta.x;
            bounds.MaxX += delta.x;
            bounds.MinY += delta.y;
            bounds.MaxY += delta.y;
            bounds.MinZ += delta.z;
            bounds.MaxZ += delta.z;
            memcpy(patched_body + 0x4c, &bounds, sizeof(bounds));
        }
        memcpy(patched_body + position_offset, &entity->position, sizeof(entity->position));
    }
    if (rotation_changed) {
        const Asura_Quat orientation = euler_quaternion(entity->rotation);
        if (classification == SnipeEntityClass_PositionMarker) {
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
    return buffer_append(out, patched.data(), patched.size(), err) != ~0ull;
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

bool pack_pc_document(const Document& doc, const char* output_path, std::string* why) {
    Error err{};
    Arena arena{};
    ChunkList source{};
    Buffer output{};
    Sounds sounds{};
    bool ok = arena_init(&arena, 64 * MiB, &err) &&
              parse_chunks(doc.source_pc_path.c_str(), &source, &arena, &err);
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
    if (ok) {
        for (uint32_t i = 0; i < source.count; ++i) {
            source_has_lights |= source.chunks[i].cid == ASURA_CHUNK_LIGHTS;
            source_has_phonons |= source.chunks[i].cid == ASURA_CHUNK_PHONONS;
            source_has_editable_entities |= editable_pc_entity_chunk(source.chunks[i]);
        }
    }
    bool wrote_lights = false, wrote_phonons = false, wrote_entities = false, wrote_sound_resources = false;
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
    auto write_entities = [&]() {
        if (!wrote_entities) {
            wrote_entities = true;
            ok = ok && append_sound_entities(&output, sounds, &err) && append_editor_spawnpoints(&output, doc, &err) &&
                 append_editor_pickups(&output, doc, &err);
        }
    };

    for (uint32_t i = 0; ok && i < source.count; ++i) {
        const ChunkRef& chunk = source.chunks[i];
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
        ok = ok && buffer_append(&output, nullptr, sizeof(Asura_Chunk_Header), &err) != ~0ull;
    }

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

bool pack_document(Document& doc, const char* output_path, std::string* why) {
    if (!normalise_editor_guids(&doc, why))
        return false;
    if (!doc.source_pc_path.empty())
        return pack_pc_document(doc, output_path, why);
    if (doc.obj_path.empty()) {
        if (why)
            *why = "Open an OBJ before exporting.";
        return false;
    }
    bool has_pickups = false;
    for (const Entity& entity : doc.entities)
        has_pickups |= entity.kind == EntityKind::PhysicalObject;
    if (has_pickups && doc.weapons_donor.empty()) {
        if (why)
            *why = "Choose a Weapons donor .PC before exporting pickups from a custom level.";
        return false;
    }
    Error err{};
    Config cfg{};
    char editor_arg[] = "LevelEditor";
    char* args[] = {editor_arg, const_cast<char*>(doc.obj_path.c_str()), const_cast<char*>(output_path)};
    if (!parse_cli(3, args, &cfg, &err)) {
        if (why)
            *why = err.message;
        return false;
    }
    cfg.material_map = doc.material_map.empty() ? nullptr : doc.material_map.c_str();
    cfg.texture_dir = doc.texture_dir.empty() ? nullptr : doc.texture_dir.c_str();
    cfg.weapon_from_pc = doc.weapons_donor.empty() ? nullptr : doc.weapons_donor.c_str();
    cfg.sky_texture_dir = doc.sky_texture_dir.empty() ? nullptr : doc.sky_texture_dir.c_str();
    cfg.allow_unknown_materials = doc.material_map.empty();

    Arena arena{}, scratch{};
    Buffer output{}, env_payload{};
    MappedFile obj_file{};
    MaterialMap material_map{};
    ObjData obj{};
    EnvBuild env{};
    EnvView view{};
    ShadeSource shade{};
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
             load_shade_source(cfg, obj, &shade, &arena, &scratch, &err) &&
             build_env(cfg, obj, material_map, shade.count ? &shade : nullptr, &arena, &scratch, &env, &err);
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
        ok = append_fnfo(&output, &err) && append_rsfl(&output, cfg, &scratch, &err) &&
             append_weapon_support(&output, cfg, &scratch, &err) &&
             append_sky_resources(&output, cfg, &scratch, &err) &&
             append_textures(&output, cfg, view, material_map, &arena, &scratch, &textures, &err) &&
             append_rscf(&output, str_from_c(cfg.env_name), ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC,
                         ASURA_RESOURCEFILE_TYPE_PC_ENVIRONMENT, env_payload.base,
                         static_cast<uint32_t>(env_payload.size), &err) &&
             append_sound_resources(&output, sounds, &scratch, &err) && append_editor_lights(&output, doc, &err) &&
             append_phon(&output, sounds, &err) &&
             append_emod(&output, view, view.module_count, cfg, material_map, &scratch, metrics, &err) &&
             append_mlin(&output, metrics, view.module_count, &err) &&
             append_mrvb(&output, view.module_count, &err) && append_nav1(&output, view.module_count, &err) &&
             append_sound_entities(&output, sounds, &err) && append_editor_spawnpoints(&output, doc, &err) &&
             append_editor_pickups(&output, doc, &err) &&
             append_skyb(&output, &err) &&
             append_fog(&output, &err) && append_wthr(&output, &err) &&
             buffer_append(&output, nullptr, sizeof(Asura_Chunk_Header), &err) != ~0ull &&
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

} // namespace editor

namespace editor {

enum ControlId : int {
    ID_OPEN_OBJ = 100,
    ID_OPEN_PC,
    ID_OPEN_PROJECT,
    ID_SAVE_PROJECT,
    ID_EXPORT_PC,
    ID_MATERIAL_MAP,
    ID_TEXTURE_DIR,
    ID_WEAPONS_DONOR,
    ID_SKYBOX_TEXTURES,
    ID_ENTITY_LIST,
    ID_ADD_SPAWN,
    ID_ADD_LIGHT,
    ID_ADD_SOUND,
    ID_ADD_PICKUP,
    ID_DELETE_ENTITY,
    ID_APPLY_INSPECTOR,
    ID_BROWSE_SOUND,
    ID_NAME,
    ID_POS_X,
    ID_POS_Y,
    ID_POS_Z,
    ID_ROT_X,
    ID_ROT_Y,
    ID_ROT_Z,
    ID_VALUE_A,
    ID_VALUE_B,
    ID_LIGHT_PROPERTIES,
    ID_SOUND_LOOP,
    ID_STATUS,
    ID_VIEWPORT,
};

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
    HWND value[2]{};
    HWND value_label[2]{};
    HWND sound_browse = nullptr;
    HWND sound_loop = nullptr;
    HWND light_properties = nullptr;
    HWND status = nullptr;
    HWND viewport = nullptr;
    HFONT font = nullptr;
    Document document;
    Mesh mesh;
    std::array<SpawnPuppet, 3> spawn_puppets;
    std::string spawn_puppet_source;
    std::vector<PickupModel> pickup_models;
    Camera camera;
    int selected = -1;
    int pending_kind = -1;
    bool orbiting = false;
    bool panning = false;
    bool moving_entity = false;
    bool fast_preview = false;
    HBITMAP environment_cache = nullptr;
    int environment_cache_width = 0;
    int environment_cache_height = 0;
    bool environment_cache_valid = false;
    bool environment_cache_fast = false;
    POINT last_mouse{};
};

AppState g;

void invalidate_environment_cache() { g.environment_cache_valid = false; }

void release_environment_cache() {
    if (g.environment_cache)
        DeleteObject(g.environment_cache);
    g.environment_cache = nullptr;
    g.environment_cache_width = 0;
    g.environment_cache_height = 0;
    g.environment_cache_valid = false;
}

void request_redraw() {
    if (g.window)
        InvalidateRect(g.window, nullptr, FALSE);
    if (g.viewport)
        InvalidateRect(g.viewport, nullptr, FALSE);
}

Asura_Vector_3 add(Asura_Vector_3 a, Asura_Vector_3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Asura_Vector_3 sub(Asura_Vector_3 a, Asura_Vector_3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Asura_Vector_3 mul(Asura_Vector_3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
Asura_Vector_3 entity_view_position(Asura_Vector_3 game_position) {
    game_position.y = -game_position.y;
    return game_position;
}
Asura_Vector_3 entity_view_direction(Asura_Vector_3 game_direction) {
    game_direction.y = -game_direction.y;
    return game_direction;
}
float dot(Asura_Vector_3 a, Asura_Vector_3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Asura_Vector_3 cross(Asura_Vector_3 a, Asura_Vector_3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Asura_Vector_3 normalized(Asura_Vector_3 a) {
    const float n = sqrtf(dot(a, a));
    return n > .00001f ? mul(a, 1.0f / n) : Asura_Vector_3{};
}

const SpawnPuppet* spawn_puppet_for_team(uint32_t team_mask) {
    if ((team_mask == 4 || team_mask == 5) && !g.spawn_puppets[0].faces.empty())
        return &g.spawn_puppets[0]; // Russian team
    if ((team_mask == 2 || team_mask == 3) && !g.spawn_puppets[1].faces.empty())
        return &g.spawn_puppets[1]; // German team
    if (team_mask == 1 && !g.spawn_puppets[2].faces.empty())
        return &g.spawn_puppets[2]; // Deathmatch
    return nullptr;
}

const SpawnPuppet* pickup_model_for_skin(uint32_t skin_id) {
    for (const PickupModel& model : g.pickup_models)
        if (model.skin_id == skin_id && !model.mesh.faces.empty())
            return &model.mesh;
    return nullptr;
}

const SpawnPuppet* entity_render_model(const Entity& entity) {
    if (entity.kind == EntityKind::SpawnPoint)
        return spawn_puppet_for_team(entity.value_u32_a);
    if (entity.kind == EntityKind::PhysicalObject)
        return pickup_model_for_skin(entity.pickup_skin_id);
    return nullptr;
}

Asura_Vector_3 rotate_by_quaternion(Asura_Vector_3 value, const Asura_Quat& rotation) {
    const Asura_Vector_3 q{rotation.x, rotation.y, rotation.z};
    const Asura_Vector_3 twice_cross = mul(cross(q, value), 2.0f);
    return add(value, add(mul(twice_cross, rotation.w), cross(q, twice_cross)));
}

Asura_Vector_3 spawn_puppet_view_vector(Asura_Vector_3 value, const Entity& entity) {
    value = rotate_by_quaternion(value, euler_quaternion(entity.rotation));
    value.y = -value.y;
    return value;
}

Asura_Vector_3 spawn_puppet_view_position(Asura_Vector_3 local_position, const Entity& entity) {
    return add(entity_view_position(entity.position), spawn_puppet_view_vector(local_position, entity));
}

enum class LightGizmoPart { Range, Direction };

struct LightGizmoLine {
    Asura_Vector_3 a{};
    Asura_Vector_3 b{};
    LightGizmoPart part = LightGizmoPart::Range;
};

constexpr int kLightRangeSegments = 48;
constexpr int kLightConeMeridians = 8;
constexpr size_t kMaximumLightGizmoLines = kLightRangeSegments * 12 + kLightConeMeridians + 5;
constexpr float kMaximumLightGizmoRange = 10000000.0f;

void append_light_gizmo_line(std::vector<LightGizmoLine>* lines, Asura_Vector_3 a, Asura_Vector_3 b,
                             LightGizmoPart part) {
    lines->push_back({a, b, part});
}

void append_range_globe(Asura_Vector_3 center, float range, std::vector<LightGizmoLine>* lines) {
    constexpr float tau = 6.28318530717958647692f;
    for (int ring = 0; ring < 3; ++ring) {
        for (int segment = 0; segment < kLightRangeSegments; ++segment) {
            const float a0 = tau * segment / kLightRangeSegments;
            const float a1 = tau * (segment + 1) / kLightRangeSegments;
            const float c0 = cosf(a0) * range, s0 = sinf(a0) * range;
            const float c1 = cosf(a1) * range, s1 = sinf(a1) * range;
            Asura_Vector_3 p0{}, p1{};
            if (ring == 0) {
                p0 = {c0, s0, 0};
                p1 = {c1, s1, 0};
            } else if (ring == 1) {
                p0 = {c0, 0, s0};
                p1 = {c1, 0, s1};
            } else {
                p0 = {0, c0, s0};
                p1 = {0, c1, s1};
            }
            append_light_gizmo_line(lines, add(center, p0), add(center, p1), LightGizmoPart::Range);
        }
    }
}

void append_sound_gizmo(const Entity& entity, std::vector<LightGizmoLine>* lines) {
    if (entity.kind != EntityKind::Sound)
        return;
    const float range = fabsf(entity.value_b);
    if (isfinite(range) && range > .00001f && range <= kMaximumLightGizmoRange)
        append_range_globe(entity_view_position(entity.position), range, lines);
}

void append_light_gizmo(const Entity& entity, float marker_size, std::vector<LightGizmoLine>* lines) {
    if (entity.kind != EntityKind::Light)
        return;
    const Asura_Vector_3 center = entity_view_position(entity.position);
    const float range = fabsf(entity.light.Range);
    const Asura_Vector_3 direction = normalized(entity_view_direction(entity.light.Direction));
    const bool has_direction = dot(direction, direction) > .5f;
    const float angle = isfinite(entity.light.Angle) ? std::clamp(entity.light.Angle, 0.0f, 360.0f) : 360.0f;
    if (isfinite(range) && range > .00001f && range <= kMaximumLightGizmoRange) {
        constexpr float tau = 6.28318530717958647692f;
        if (angle >= 359.999f) {
            append_range_globe(center, range, lines);
        } else if (has_direction && angle > .001f) {
            Asura_Vector_3 side = cross(direction, {0, 1, 0});
            if (dot(side, side) < .01f)
                side = cross(direction, {1, 0, 0});
            side = normalized(side);
            const Asura_Vector_3 cap_up = normalized(cross(direction, side));
            constexpr float d2r = 3.14159265358979323846f / 180.0f;
            const float half_angle = angle * .5f * d2r;
            const int latitude_rings = std::max(1, static_cast<int>(ceilf(angle / 45.0f)));
            const int arc_segments = std::max(2, static_cast<int>(ceilf(kLightRangeSegments * angle / 720.0f)));

            for (int latitude = 1; latitude <= latitude_rings; ++latitude) {
                const float theta = half_angle * latitude / latitude_rings;
                const float axial = cosf(theta) * range;
                const float radial = sinf(theta) * range;
                const Asura_Vector_3 ring_center = add(center, mul(direction, axial));
                for (int segment = 0; segment < kLightRangeSegments; ++segment) {
                    const float a0 = tau * segment / kLightRangeSegments;
                    const float a1 = tau * (segment + 1) / kLightRangeSegments;
                    const Asura_Vector_3 radial0 =
                        add(mul(side, cosf(a0) * radial), mul(cap_up, sinf(a0) * radial));
                    const Asura_Vector_3 radial1 =
                        add(mul(side, cosf(a1) * radial), mul(cap_up, sinf(a1) * radial));
                    append_light_gizmo_line(lines, add(ring_center, radial0), add(ring_center, radial1),
                                            LightGizmoPart::Range);
                }
            }

            for (int meridian = 0; meridian < kLightConeMeridians; ++meridian) {
                const float around = tau * meridian / kLightConeMeridians;
                const Asura_Vector_3 radial_axis =
                    add(mul(side, cosf(around)), mul(cap_up, sinf(around)));
                Asura_Vector_3 previous = add(center, mul(direction, range));
                for (int segment = 1; segment <= arc_segments; ++segment) {
                    const float theta = half_angle * segment / arc_segments;
                    const Asura_Vector_3 point =
                        add(center, add(mul(direction, cosf(theta) * range),
                                        mul(radial_axis, sinf(theta) * range)));
                    append_light_gizmo_line(lines, previous, point, LightGizmoPart::Range);
                    previous = point;
                }
                append_light_gizmo_line(lines, center, previous, LightGizmoPart::Range);
            }
        } else if (has_direction) {
            append_light_gizmo_line(lines, center, add(center, mul(direction, range)), LightGizmoPart::Range);
        }
    }

    if (!has_direction)
        return;
    const float maximum_length = fmaxf(marker_size * 8.0f, g.mesh.radius * .2f);
    const float arrow_length = isfinite(range) && range > 0
                                   ? std::clamp(range * .2f, marker_size * 5.0f, maximum_length)
                                   : maximum_length;
    const Asura_Vector_3 tip = add(center, mul(direction, arrow_length));
    append_light_gizmo_line(lines, center, tip, LightGizmoPart::Direction);

    Asura_Vector_3 side = cross(direction, {0, 1, 0});
    if (dot(side, side) < .01f)
        side = cross(direction, {1, 0, 0});
    side = normalized(side);
    const Asura_Vector_3 arrow_up = normalized(cross(direction, side));
    const float head_length = arrow_length * .22f;
    const float head_width = head_length * .45f;
    const Asura_Vector_3 head_base = sub(tip, mul(direction, head_length));
    append_light_gizmo_line(lines, tip, add(head_base, mul(side, head_width)), LightGizmoPart::Direction);
    append_light_gizmo_line(lines, tip, sub(head_base, mul(side, head_width)), LightGizmoPart::Direction);
    append_light_gizmo_line(lines, tip, add(head_base, mul(arrow_up, head_width)), LightGizmoPart::Direction);
    append_light_gizmo_line(lines, tip, sub(head_base, mul(arrow_up, head_width)), LightGizmoPart::Direction);
}

RECT viewport_rect() {
    RECT r{};
    GetClientRect(g.window, &r);
    r.left = 238;
    r.top = 44;
    r.right -= 272;
    r.bottom -= 25;
    if (r.right < r.left + 40)
        r.right = r.left + 40;
    if (r.bottom < r.top + 40)
        r.bottom = r.top + 40;
    return r;
}

void camera_axes(Asura_Vector_3* position, Asura_Vector_3* right, Asura_Vector_3* up, Asura_Vector_3* forward) {
    const float cp = cosf(g.camera.pitch), sp = sinf(g.camera.pitch);
    *position = add(g.camera.target,
                    {sinf(g.camera.yaw) * cp * g.camera.distance, sp * g.camera.distance,
                     cosf(g.camera.yaw) * cp * g.camera.distance});
    *forward = normalized(sub(g.camera.target, *position));
    *right = normalized(cross({0, 1, 0}, *forward));
    *up = normalized(cross(*forward, *right));
}

bool project_point(const Asura_Vector_3& p, POINT* screen, float* depth = nullptr) {
    const RECT vr = viewport_rect();
    Asura_Vector_3 cam, right, up, forward;
    camera_axes(&cam, &right, &up, &forward);
    const Asura_Vector_3 rel = sub(p, cam);
    const float z = dot(rel, forward);
    if (z <= .05f)
        return false;
    const float f = .85f * static_cast<float>(std::min(vr.right - vr.left, vr.bottom - vr.top));
    screen->x = static_cast<LONG>((vr.left + vr.right) * .5f + dot(rel, right) * f / z);
    screen->y = static_cast<LONG>((vr.top + vr.bottom) * .5f - dot(rel, up) * f / z);
    if (depth)
        *depth = z;
    return true;
}

bool ground_point_from_screen(int x, int y, Asura_Vector_3* point) {
    const RECT vr = viewport_rect();
    Asura_Vector_3 cam, right, up, forward;
    camera_axes(&cam, &right, &up, &forward);
    const float f = .85f * static_cast<float>(std::min(vr.right - vr.left, vr.bottom - vr.top));
    const float sx = (x - (vr.left + vr.right) * .5f) / f;
    const float sy = -(y - (vr.top + vr.bottom) * .5f) / f;
    const Asura_Vector_3 ray = normalized(add(forward, add(mul(right, sx), mul(up, sy))));
    if (fabsf(ray.y) < .00001f)
        return false;
    const float t = -cam.y / ray.y;
    if (t <= 0)
        return false;
    *point = add(cam, mul(ray, t));
    return true;
}

struct GpuVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT4 color;
};

struct SkyboxVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT2 uv;
};

struct SkyboxAnimationConstants {
    DirectX::XMFLOAT2 offset_a;
    DirectX::XMFLOAT2 offset_b;
    DirectX::XMFLOAT4 tint;
};

struct DdsPixelFormat {
    uint32_t size;
    uint32_t flags;
    uint32_t four_cc;
    uint32_t rgb_bit_count;
    uint32_t r_mask;
    uint32_t g_mask;
    uint32_t b_mask;
    uint32_t a_mask;
};

struct DdsHeader {
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitch_or_linear_size;
    uint32_t depth;
    uint32_t mip_count;
    uint32_t reserved[11];
    DdsPixelFormat pixel_format;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};

struct DdsHeaderDx10 {
    uint32_t format;
    uint32_t resource_dimension;
    uint32_t misc_flag;
    uint32_t array_size;
    uint32_t misc_flags2;
};

struct GpuRenderer {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* render_target = nullptr;
    ID3D11Texture2D* depth_texture = nullptr;
    ID3D11DepthStencilView* depth_view = nullptr;
    ID3D11VertexShader* vertex_shader = nullptr;
    ID3D11PixelShader* pixel_shader = nullptr;
    ID3D11InputLayout* input_layout = nullptr;
    ID3D11VertexShader* skybox_vertex_shader = nullptr;
    ID3D11PixelShader* skybox_pixel_shader = nullptr;
    ID3D11PixelShader* skybox_cloud_pixel_shader = nullptr;
    ID3D11InputLayout* skybox_input_layout = nullptr;
    ID3D11Buffer* camera_buffer = nullptr;
    ID3D11Buffer* skybox_animation_buffer = nullptr;
    ID3D11Buffer* skybox_vertices = nullptr;
    ID3D11Buffer* skybox_cloud_vertices = nullptr;
    ID3D11Buffer* mesh_vertices = nullptr;
    ID3D11Buffer* mesh_indices = nullptr;
    ID3D11Buffer* puppet_vertices = nullptr;
    ID3D11Buffer* overlay_vertices = nullptr;
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11DepthStencilState* depth_enabled = nullptr;
    ID3D11DepthStencilState* depth_disabled = nullptr;
    ID3D11DepthStencilState* skybox_depth = nullptr;
    ID3D11BlendState* skybox_cloud_blend = nullptr;
    ID3D11SamplerState* skybox_sampler = nullptr;
    ID3D11SamplerState* skybox_cloud_sampler = nullptr;
    ID3D11ShaderResourceView* skybox_faces[6]{};
    ID3D11ShaderResourceView* skybox_cloud = nullptr;
    ID3D11ShaderResourceView* white_texture = nullptr;
    uint32_t mesh_index_count = 0;
    uint32_t skybox_cloud_vertex_count = 0;
    uint32_t puppet_capacity = 0;
    uint32_t overlay_capacity = 0;
    uint32_t width = 0, height = 0;
    DirectX::XMFLOAT4 skybox_tint{1, 1, 1, 1};
    bool skybox_active = false;
    bool ready = false;
};

GpuRenderer gpu;

template <typename T> void gpu_release(T*& object) {
    if (object)
        object->Release();
    object = nullptr;
}

void gpu_release_skybox_textures() {
    for (ID3D11ShaderResourceView*& face : gpu.skybox_faces)
        gpu_release(face);
    gpu_release(gpu.skybox_cloud);
    gpu.skybox_active = false;
}

bool dds_format_layout(DXGI_FORMAT format, uint32_t* block_bytes, uint32_t* bytes_per_pixel) {
    *block_bytes = 0;
    *bytes_per_pixel = 0;
    switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM: *block_bytes = 8; return true;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM: *block_bytes = 16; return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: *bytes_per_pixel = 4; return true;
    default: return false;
    }
}

bool gpu_create_dds_view_from_memory(const uint8_t* bytes, size_t byte_count, const char* label,
                                     ID3D11ShaderResourceView** output, std::string* why) {
    *output = nullptr;
    const char* source = label ? label : "embedded .PC skybox texture";
    if (!bytes || byte_count < 4 + sizeof(DdsHeader) || byte_count > 512 * MiB) {
        if (why)
            *why = std::string("Skybox texture is not a valid DDS file: ") + source;
        return false;
    }
    if (memcmp(bytes, "DDS ", 4) != 0) {
        if (why)
            *why = std::string("Skybox texture does not contain DDS data: ") + source;
        return false;
    }
    DdsHeader header{};
    memcpy(&header, bytes + 4, sizeof(header));
    if (header.size != sizeof(DdsHeader) || header.pixel_format.size != sizeof(DdsPixelFormat) || !header.width ||
        !header.height || header.width > 16384 || header.height > 16384) {
        if (why)
            *why = std::string("Skybox texture has an unsupported DDS header: ") + source;
        return false;
    }

    constexpr uint32_t dds_four_cc = 0x4;
    constexpr uint32_t dds_rgb = 0x40;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    size_t data_at = 4 + sizeof(DdsHeader);
    if ((header.pixel_format.flags & dds_four_cc) && header.pixel_format.four_cc == fourcc('D', 'X', '1', '0')) {
        if (byte_count < data_at + sizeof(DdsHeaderDx10)) {
            if (why)
                *why = std::string("Skybox DDS DX10 header is truncated: ") + source;
            return false;
        }
        DdsHeaderDx10 dx10{};
        memcpy(&dx10, bytes + data_at, sizeof(dx10));
        data_at += sizeof(dx10);
        if (dx10.resource_dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D || dx10.array_size != 1 ||
            (dx10.misc_flag & D3D11_RESOURCE_MISC_TEXTURECUBE)) {
            if (why)
                *why = std::string("Skybox DDS must contain one 2D texture: ") + source;
            return false;
        }
        format = static_cast<DXGI_FORMAT>(dx10.format);
    } else if (header.pixel_format.flags & dds_four_cc) {
        const uint32_t code = header.pixel_format.four_cc;
        if (code == fourcc('D', 'X', 'T', '1'))
            format = DXGI_FORMAT_BC1_UNORM;
        else if (code == fourcc('D', 'X', 'T', '2') || code == fourcc('D', 'X', 'T', '3'))
            format = DXGI_FORMAT_BC2_UNORM;
        else if (code == fourcc('D', 'X', 'T', '4') || code == fourcc('D', 'X', 'T', '5'))
            format = DXGI_FORMAT_BC3_UNORM;
        else if (code == fourcc('A', 'T', 'I', '1') || code == fourcc('B', 'C', '4', 'U'))
            format = DXGI_FORMAT_BC4_UNORM;
        else if (code == fourcc('A', 'T', 'I', '2') || code == fourcc('B', 'C', '5', 'U'))
            format = DXGI_FORMAT_BC5_UNORM;
    } else if ((header.pixel_format.flags & dds_rgb) && header.pixel_format.rgb_bit_count == 32) {
        if (header.pixel_format.r_mask == 0x000000ff && header.pixel_format.g_mask == 0x0000ff00 &&
            header.pixel_format.b_mask == 0x00ff0000)
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (header.pixel_format.r_mask == 0x00ff0000 && header.pixel_format.g_mask == 0x0000ff00 &&
                 header.pixel_format.b_mask == 0x000000ff)
            format = header.pixel_format.a_mask == 0xff000000 ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                               : DXGI_FORMAT_B8G8R8X8_UNORM;
    }

    uint32_t block_bytes = 0, bytes_per_pixel = 0;
    if (!dds_format_layout(format, &block_bytes, &bytes_per_pixel)) {
        if (why)
            *why = std::string("Skybox DDS pixel format is unsupported: ") + source;
        return false;
    }
    const uint32_t mip_count = std::clamp(header.mip_count ? header.mip_count : 1u, 1u, 15u);
    std::vector<D3D11_SUBRESOURCE_DATA> initial(mip_count);
    uint32_t width = header.width, height = header.height;
    for (uint32_t mip = 0; mip < mip_count; ++mip) {
        const uint64_t row_pitch = block_bytes ? static_cast<uint64_t>(std::max(1u, (width + 3) / 4)) * block_bytes
                                               : static_cast<uint64_t>(width) * bytes_per_pixel;
        const uint64_t rows = block_bytes ? std::max(1u, (height + 3) / 4) : height;
        const uint64_t size = row_pitch * rows;
        if (data_at > byte_count || row_pitch > 0xffffffffu || size > byte_count - data_at) {
            if (why)
                *why = std::string("Skybox DDS mip data is truncated: ") + source;
            return false;
        }
        initial[mip].pSysMem = bytes + data_at;
        initial[mip].SysMemPitch = static_cast<UINT>(row_pitch);
        initial[mip].SysMemSlicePitch = static_cast<UINT>(size);
        data_at += static_cast<size_t>(size);
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = header.width;
    desc.Height = header.height;
    desc.MipLevels = mip_count;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* texture = nullptr;
    const HRESULT texture_result = gpu.device->CreateTexture2D(&desc, initial.data(), &texture);
    const HRESULT view_result = SUCCEEDED(texture_result)
                                    ? gpu.device->CreateShaderResourceView(texture, nullptr, output)
                                    : texture_result;
    gpu_release(texture);
    if (FAILED(view_result)) {
        if (why)
            *why = std::string("Direct3D could not create the skybox texture: ") + source;
        return false;
    }
    return true;
}

bool gpu_create_dds_view(const char* path, ID3D11ShaderResourceView** output, std::string* why) {
    *output = nullptr;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        if (why)
            *why = std::string("Could not open skybox texture: ") + path;
        return false;
    }
    const std::streamoff end = file.tellg();
    if (end < 0 || end > static_cast<std::streamoff>(512 * MiB)) {
        if (why)
            *why = std::string("Skybox texture is not a valid DDS file: ") + path;
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        if (why)
            *why = std::string("Could not read skybox texture: ") + path;
        return false;
    }
    return gpu_create_dds_view_from_memory(bytes.data(), bytes.size(), path, output, why);
}

bool find_skybox_texture(const std::string& directory, const char* stem, char* output, uint32_t output_size) {
    char search[MAX_PATH * 4]{};
    if (!join_path(search, sizeof(search), directory.c_str(), str_from_c("*")))
        return false;
    WIN32_FIND_DATAA entry{};
    HANDLE find = FindFirstFileA(search, &entry);
    if (find == INVALID_HANDLE_VALUE)
        return false;
    char first_matching_file[MAX_PATH * 4]{};
    bool found = false;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        std::string name = entry.cFileName;
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name.resize(dot);
        if (_stricmp(name.c_str(), stem) != 0)
            continue;
        char candidate[MAX_PATH * 4]{};
        if (!join_path(candidate, sizeof(candidate), directory.c_str(), str_from_c(entry.cFileName)))
            continue;
        if (!first_matching_file[0])
            strncpy_s(first_matching_file, candidate, _TRUNCATE);
        std::ifstream file(candidate, std::ios::binary);
        char magic[4]{};
        file.read(magic, sizeof(magic));
        if (file.gcount() == sizeof(magic) && memcmp(magic, "DDS ", sizeof(magic)) == 0) {
            strncpy_s(output, output_size, candidate, _TRUNCATE);
            found = true;
            break;
        }
    } while (FindNextFileA(find, &entry));
    FindClose(find);
    if (!found && first_matching_file[0]) {
        strncpy_s(output, output_size, first_matching_file, _TRUNCATE);
        found = true;
    }
    return found;
}

bool gpu_rebuild_skybox_vertices(float orientation, bool back_uses_front_upside_down,
                                 bool right_uses_left_upside_down, std::string* why);

bool gpu_load_skybox(const std::string& directory, std::string* why) {
    if (g.window)
        KillTimer(g.window, 2);
    gpu_release_skybox_textures();
    if (directory.empty())
        return true;
    if (!gpu.ready) {
        if (why)
            *why = "The Direct3D viewport is unavailable.";
        return false;
    }
    if (!gpu_rebuild_skybox_vertices(3.107175588607788f, false, false, why))
        return false;
    gpu.skybox_tint = {1, 1, 1, 1};
    // SKYB v7 slots: 0 is the empty lower face, 1..5 are the five
    // static images, and 6..7 are the animated cloud textures.
    const char* stems[] = {"fr", "lf", "bk", "rt", "up"};
    ID3D11ShaderResourceView* next[6]{};
    ID3D11ShaderResourceView* next_cloud = nullptr;
    uint32_t found = 0;
    for (uint32_t i = 0; i < _countof(stems); ++i) {
        char path[MAX_PATH * 4]{};
        if (!find_skybox_texture(directory, stems[i], path, sizeof(path)))
            continue;
        ++found;
        if (!gpu_create_dds_view(path, &next[i + 1], why)) {
            for (ID3D11ShaderResourceView*& face : next)
                gpu_release(face);
            return false;
        }
    }
    char cloud_path[MAX_PATH * 4]{};
    if (find_skybox_texture(directory, "ch_04_sky", cloud_path, sizeof(cloud_path))) {
        ++found;
        if (!gpu_create_dds_view(cloud_path, &next_cloud, why)) {
            for (ID3D11ShaderResourceView*& face : next)
                gpu_release(face);
            return false;
        }
    }
    if (!found) {
        if (why)
            *why = "The selected folder contains none of the expected DDS skybox textures "
                   "(fr, lf, bk, rt, up, ch_04_sky; filename extensions are ignored).";
        return false;
    }
    for (uint32_t i = 0; i < _countof(next); ++i)
        gpu.skybox_faces[i] = next[i];
    gpu.skybox_cloud = next_cloud;
    gpu.skybox_active = true;
    if (gpu.skybox_cloud && g.window)
        SetTimer(g.window, 2, 33, nullptr);
    return true;
}

struct PcSkyboxInfo {
    float red = 255.0f;
    float green = 255.0f;
    float blue = 255.0f;
    float orientation = 0.0f;
    Str names[8]{};
    bool draw_clouds = false;
    bool back_uses_front_upside_down = false;
    bool right_uses_left_upside_down = false;
};

bool pc_skybox_info(const ChunkList& chunks, PcSkyboxInfo* info, Error* err) {
    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        const ChunkRef& chunk = chunks.chunks[chunk_index];
        if (chunk.cid != ASURA_CHUNK_SKYBOX)
            continue;
        if (chunk.version != 7 || chunk.size < sizeof(Asura_Chunk_Header) + 16 + 8 * 4 + 12)
            return fail(err, "the .PC SKYB chunk has an unsupported version or size");
        const uint8_t* payload = chunk.data + sizeof(Asura_Chunk_Header);
        const uint32_t payload_size = chunk.size - sizeof(Asura_Chunk_Header);
        memcpy(&info->red, payload, sizeof(float));
        memcpy(&info->green, payload + 4, sizeof(float));
        memcpy(&info->blue, payload + 8, sizeof(float));
        memcpy(&info->orientation, payload + 12, sizeof(float));
        if (!isfinite(info->red) || !isfinite(info->green) || !isfinite(info->blue) ||
            !isfinite(info->orientation))
            return fail(err, "the .PC SKYB colour or orientation is invalid");
        uint64_t at = 16;
        for (uint32_t slot = 0; slot < 8; ++slot) {
            if (at > 0xffffffffull)
                return fail(err, "the .PC SKYB texture table is invalid");
            info->names[slot] = padded_string_at(payload, payload_size, static_cast<uint32_t>(at));
            if (!info->names[slot].data)
                return fail(err, "the .PC SKYB texture table is truncated");
            at = align_up(at + info->names[slot].size + 1, 4);
            if (at > payload_size)
                return fail(err, "the .PC SKYB texture table is truncated");
        }
        if (at + 12 > payload_size)
            return fail(err, "the .PC SKYB flags are truncated");
        uint32_t flags[3]{};
        memcpy(flags, payload + at, sizeof(flags));
        info->draw_clouds = flags[0] != 0;
        info->back_uses_front_upside_down = flags[1] != 0;
        info->right_uses_left_upside_down = flags[2] != 0;
        return true;
    }
    return fail(err, "the .PC contains no SKYB chunk");
}

bool pc_texture_resource(const ChunkList& chunks, Str skybox_name, RscfInfo* output) {
    if (!skybox_name.size)
        return false;
    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        RscfInfo resource{};
        if (rscf_info(chunks.chunks[chunk_index], &resource) &&
            resource.type == ASURA_RESOURCEFILE_TYPE_TEXTURE &&
            text_name_matches_resource(skybox_name, resource.name)) {
            *output = resource;
            return true;
        }
    }
    return false;
}

bool gpu_load_pc_skybox(const std::string& pc_path, std::string* why) {
    if (g.window)
        KillTimer(g.window, 2);
    gpu_release_skybox_textures();
    if (!gpu.ready) {
        if (why)
            *why = "The Direct3D viewport is unavailable.";
        return false;
    }
    Error err{};
    Arena arena{};
    ChunkList chunks{};
    bool ok = arena_init(&arena, 8 * MiB, &err) && parse_chunks(pc_path.c_str(), &chunks, &arena, &err);
    PcSkyboxInfo info{};
    if (ok)
        ok = pc_skybox_info(chunks, &info, &err);
    if (ok)
        ok = gpu_rebuild_skybox_vertices(info.orientation, info.back_uses_front_upside_down,
                                         info.right_uses_left_upside_down, why);

    ID3D11ShaderResourceView* next_faces[6]{};
    ID3D11ShaderResourceView* next_cloud = nullptr;
    uint32_t loaded = 0;
    for (uint32_t slot = 0; ok && slot < 6; ++slot) {
        RscfInfo resource{};
        if (!pc_texture_resource(chunks, info.names[slot], &resource))
            continue;
        const std::string label(info.names[slot].data, info.names[slot].size);
        ok = gpu_create_dds_view_from_memory(resource.payload, resource.payload_size, label.c_str(),
                                             &next_faces[slot], why);
        loaded += ok;
    }
    if (ok && info.draw_clouds) {
        RscfInfo resource{};
        uint32_t cloud_slot = 6;
        bool have_cloud = pc_texture_resource(chunks, info.names[cloud_slot], &resource);
        if (!have_cloud) {
            cloud_slot = 7;
            have_cloud = pc_texture_resource(chunks, info.names[cloud_slot], &resource);
        }
        if (have_cloud) {
            const std::string label(info.names[cloud_slot].data, info.names[cloud_slot].size);
            ok = gpu_create_dds_view_from_memory(resource.payload, resource.payload_size, label.c_str(),
                                                 &next_cloud, why);
            loaded += ok;
        }
    }
    if (ok && !loaded) {
        ok = false;
        fail(&err, "the .PC SKYB textures do not have matching embedded texture resources");
    }
    if (ok) {
        for (uint32_t face = 0; face < 6; ++face)
            gpu.skybox_faces[face] = next_faces[face];
        gpu.skybox_cloud = next_cloud;
        gpu.skybox_tint = {std::clamp(info.red / 255.0f, 0.0f, 1.0f),
                           std::clamp(info.green / 255.0f, 0.0f, 1.0f),
                           std::clamp(info.blue / 255.0f, 0.0f, 1.0f), 1.0f};
        gpu.skybox_active = true;
        if (gpu.skybox_cloud && g.window)
            SetTimer(g.window, 2, 33, nullptr);
    } else {
        for (ID3D11ShaderResourceView*& face : next_faces)
            gpu_release(face);
        gpu_release(next_cloud);
        if (why && why->empty())
            *why = err.set ? err.message : "Could not load embedded .PC skybox textures.";
    }
    unmap_file(&chunks.file);
    arena_release(&arena);
    return ok;
}

void gpu_release_targets() {
    if (gpu.context)
        gpu.context->OMSetRenderTargets(0, nullptr, nullptr);
    gpu_release(gpu.depth_view);
    gpu_release(gpu.depth_texture);
    gpu_release(gpu.render_target);
}

void gpu_shutdown() {
    if (gpu.context)
        gpu.context->ClearState();
    gpu_release_targets();
    gpu_release_skybox_textures();
    gpu_release(gpu.white_texture);
    gpu_release(gpu.skybox_cloud_sampler);
    gpu_release(gpu.skybox_sampler);
    gpu_release(gpu.skybox_cloud_blend);
    gpu_release(gpu.skybox_depth);
    gpu_release(gpu.skybox_cloud_vertices);
    gpu_release(gpu.skybox_vertices);
    gpu_release(gpu.skybox_animation_buffer);
    gpu_release(gpu.skybox_input_layout);
    gpu_release(gpu.skybox_cloud_pixel_shader);
    gpu_release(gpu.skybox_pixel_shader);
    gpu_release(gpu.skybox_vertex_shader);
    gpu_release(gpu.overlay_vertices);
    gpu_release(gpu.puppet_vertices);
    gpu_release(gpu.mesh_indices);
    gpu_release(gpu.mesh_vertices);
    gpu_release(gpu.depth_disabled);
    gpu_release(gpu.depth_enabled);
    gpu_release(gpu.rasterizer);
    gpu_release(gpu.camera_buffer);
    gpu_release(gpu.input_layout);
    gpu_release(gpu.pixel_shader);
    gpu_release(gpu.vertex_shader);
    gpu_release(gpu.swap_chain);
    gpu_release(gpu.context);
    gpu_release(gpu.device);
    gpu = {};
}

bool gpu_resize(uint32_t width, uint32_t height) {
    if (!gpu.device || !gpu.swap_chain || !width || !height)
        return false;
    if (gpu.width == width && gpu.height == height && gpu.render_target)
        return true;
    gpu_release_targets();
    if (FAILED(gpu.swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)))
        return false;
    ID3D11Texture2D* back_buffer = nullptr;
    if (FAILED(gpu.swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer))))
        return false;
    const HRESULT target_result = gpu.device->CreateRenderTargetView(back_buffer, nullptr, &gpu.render_target);
    back_buffer->Release();
    if (FAILED(target_result))
        return false;
    D3D11_TEXTURE2D_DESC depth_desc{};
    depth_desc.Width = width;
    depth_desc.Height = height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(gpu.device->CreateTexture2D(&depth_desc, nullptr, &gpu.depth_texture)) ||
        FAILED(gpu.device->CreateDepthStencilView(gpu.depth_texture, nullptr, &gpu.depth_view)))
        return false;
    gpu.width = width;
    gpu.height = height;
    return true;
}

bool gpu_rebuild_skybox_vertices(float orientation, bool back_uses_front_upside_down,
                                 bool right_uses_left_upside_down, std::string* why) {
    if (!gpu.device || !isfinite(orientation)) {
        if (why)
            *why = "The .PC SKYB orientation is invalid.";
        return false;
    }

    // SniperElite.exe sub_49D000 uses these eight corners and six face
    // quads. The two version-7 compatibility flags permute face vertices
    // while retaining the fixed UVs; applying the equivalent UV flips here
    // preserves winding and reproduces the target mapping.
    const Asura_Vector_3 target_corners[] = {{-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1},
                                             {-1, -1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, -1, -1}};
    const uint32_t target_faces[6][4] = {{1, 0, 3, 2}, {0, 4, 7, 3}, {3, 7, 6, 2},
                                         {2, 6, 5, 1}, {1, 5, 4, 0}, {4, 5, 6, 7}};
    const DirectX::XMFLOAT2 base_uv[] = {{1, 1}, {1, 0}, {0, 0}, {0, 1}};
    const uint32_t triangles[] = {0, 1, 2, 0, 2, 3};
    const float orientation_sin = sinf(orientation), orientation_cos = cosf(orientation);
    SkyboxVertex vertices[36]{};
    for (uint32_t face = 0; face < 6; ++face) {
        DirectX::XMFLOAT2 uv[4]{};
        memcpy(uv, base_uv, sizeof(uv));
        if (right_uses_left_upside_down) {
            if (face == 2)
                for (DirectX::XMFLOAT2& item : uv)
                    item.x = 1.0f - item.x;
        } else if (back_uses_front_upside_down) {
            if (face == 0) {
                for (DirectX::XMFLOAT2& item : uv)
                    item.x = 1.0f - item.x;
            } else if (face == 2 || face == 3) {
                for (DirectX::XMFLOAT2& item : uv)
                    item.y = 1.0f - item.y;
            }
        }
        for (uint32_t vertex = 0; vertex < 6; ++vertex) {
            const uint32_t corner_in_face = triangles[vertex];
            const Asura_Vector_3 source = target_corners[target_faces[face][corner_in_face]];
            const Asura_Vector_3 oriented{source.x * orientation_cos + source.z * orientation_sin,
                                          -source.y,
                                          source.z * orientation_cos - source.x * orientation_sin};
            vertices[face * 6 + vertex] = {{oriented.x, oriented.y, oriented.z}, uv[corner_in_face]};
        }
    }

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(vertices);
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{vertices};
    ID3D11Buffer* next = nullptr;
    if (FAILED(gpu.device->CreateBuffer(&desc, &data, &next))) {
        if (why)
            *why = "Direct3D could not create the target skybox geometry.";
        return false;
    }
    gpu_release(gpu.skybox_vertices);
    gpu.skybox_vertices = next;
    return true;
}

bool gpu_init(HWND viewport) {
    RECT rect{};
    GetClientRect(viewport, &rect);
    DXGI_SWAP_CHAIN_DESC swap_desc{};
    swap_desc.BufferDesc.Width = std::max<LONG>(1, rect.right);
    swap_desc.BufferDesc.Height = std::max<LONG>(1, rect.bottom);
    swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 2;
    swap_desc.OutputWindow = viewport;
    swap_desc.Windowed = TRUE;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                                   D3D11_SDK_VERSION, &swap_desc, &gpu.swap_chain, &gpu.device,
                                                   &feature_level, &gpu.context);
    if (FAILED(result))
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &swap_desc, &gpu.swap_chain, &gpu.device,
                                               &feature_level, &gpu.context);
    if (FAILED(result)) {
        gpu_shutdown();
        return false;
    }
    static const char shader_source[] = R"(
cbuffer CameraBuffer : register(b0) { float4x4 viewProjection; };
struct VSInput { float3 position : POSITION; float3 normal : NORMAL; float4 color : COLOR; };
struct VSOutput { float4 position : SV_POSITION; float3 normal : NORMAL; float4 color : COLOR; };
VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.normal = input.normal;
    output.color = input.color;
    return output;
}
float4 PSMain(VSOutput input) : SV_TARGET {
    if (input.color.a > 0.5)
        return input.color;
    float light = 0.28 + 0.72 * abs(dot(normalize(input.normal), normalize(float3(-0.35, 0.8, -0.45))));
    float3 baseColor = dot(abs(input.color.rgb), float3(1.0, 1.0, 1.0)) > 0.001
                           ? input.color.rgb
                           : float3(0.32, 0.39, 0.43);
    return float4(baseColor * light, 1.0);
}
Texture2D skyboxTexture : register(t0);
Texture2D skyboxCloud : register(t1);
SamplerState skyboxSampler : register(s0);
SamplerState skyboxCloudSampler : register(s1);
cbuffer SkyboxAnimationBuffer : register(b1) {
    float2 cloudOffsetA;
    float2 cloudOffsetB;
    float4 skyboxTint;
};
struct SkyVSInput { float3 position : POSITION; float2 uv : TEXCOORD; };
struct SkyVSOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float3 direction : TEXCOORD1; };
SkyVSOutput SkyVSMain(SkyVSInput input) {
    SkyVSOutput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.position.z = output.position.w;
    output.uv = input.uv;
    output.direction = input.position;
    return output;
}
float4 SkyPSMain(SkyVSOutput input) : SV_TARGET {
    return float4(skyboxTexture.Sample(skyboxSampler, input.uv).rgb * skyboxTint.rgb, 1.0);
}
float4 SkyCloudPSMain(SkyVSOutput input) : SV_TARGET {
    float3 cloudA = skyboxCloud.Sample(skyboxCloudSampler, input.uv + cloudOffsetA).rgb;
    float3 cloudB = skyboxCloud.Sample(skyboxCloudSampler, input.uv * 2.0 + cloudOffsetB).rgb;
    float3 cloudColour = saturate((cloudA + cloudB) * 0.75 + float3(0.18, 0.19, 0.18));
    float elevation = normalize(input.direction).y;
    float horizonFade = smoothstep(-0.05, 0.20, elevation);
    return float4(cloudColour, horizonFade * 0.72);
}
)";
    ID3DBlob *vs_blob = nullptr, *ps_blob = nullptr, *errors = nullptr;
    result = D3DCompile(shader_source, sizeof(shader_source) - 1, nullptr, nullptr, nullptr, "VSMain", "vs_4_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_blob, &errors);
    gpu_release(errors);
    if (FAILED(result)) {
        gpu_release(vs_blob);
        gpu_shutdown();
        return false;
    }
    result = D3DCompile(shader_source, sizeof(shader_source) - 1, nullptr, nullptr, nullptr, "PSMain", "ps_4_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_blob, &errors);
    gpu_release(errors);
    if (FAILED(result) || FAILED(gpu.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                                                nullptr, &gpu.vertex_shader)) ||
        FAILED(gpu.device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr,
                                             &gpu.pixel_shader))) {
        gpu_release(vs_blob);
        gpu_release(ps_blob);
        gpu_shutdown();
        return false;
    }
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GpuVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GpuVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(GpuVertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    result = gpu.device->CreateInputLayout(elements, _countof(elements), vs_blob->GetBufferPointer(),
                                           vs_blob->GetBufferSize(), &gpu.input_layout);
    gpu_release(vs_blob);
    gpu_release(ps_blob);
    if (FAILED(result)) {
        gpu_shutdown();
        return false;
    }
    ID3DBlob *sky_vs_blob = nullptr, *sky_ps_blob = nullptr, *sky_cloud_ps_blob = nullptr;
    result = D3DCompile(shader_source, sizeof(shader_source) - 1, nullptr, nullptr, nullptr, "SkyVSMain", "vs_4_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &sky_vs_blob, &errors);
    gpu_release(errors);
    if (SUCCEEDED(result))
        result = D3DCompile(shader_source, sizeof(shader_source) - 1, nullptr, nullptr, nullptr, "SkyPSMain", "ps_4_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &sky_ps_blob, &errors);
    gpu_release(errors);
    if (SUCCEEDED(result))
        result = D3DCompile(shader_source, sizeof(shader_source) - 1, nullptr, nullptr, nullptr, "SkyCloudPSMain",
                            "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &sky_cloud_ps_blob, &errors);
    gpu_release(errors);
    if (FAILED(result) ||
        FAILED(gpu.device->CreateVertexShader(sky_vs_blob->GetBufferPointer(), sky_vs_blob->GetBufferSize(), nullptr,
                                               &gpu.skybox_vertex_shader)) ||
        FAILED(gpu.device->CreatePixelShader(sky_ps_blob->GetBufferPointer(), sky_ps_blob->GetBufferSize(), nullptr,
                                              &gpu.skybox_pixel_shader)) ||
        FAILED(gpu.device->CreatePixelShader(sky_cloud_ps_blob->GetBufferPointer(),
                                              sky_cloud_ps_blob->GetBufferSize(), nullptr,
                                              &gpu.skybox_cloud_pixel_shader))) {
        gpu_release(sky_vs_blob);
        gpu_release(sky_ps_blob);
        gpu_release(sky_cloud_ps_blob);
        gpu_shutdown();
        return false;
    }
    D3D11_INPUT_ELEMENT_DESC skybox_elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SkyboxVertex, position),
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(SkyboxVertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    result = gpu.device->CreateInputLayout(skybox_elements, _countof(skybox_elements),
                                           sky_vs_blob->GetBufferPointer(), sky_vs_blob->GetBufferSize(),
                                           &gpu.skybox_input_layout);
    gpu_release(sky_vs_blob);
    gpu_release(sky_ps_blob);
    gpu_release(sky_cloud_ps_blob);
    if (FAILED(result)) {
        gpu_shutdown();
        return false;
    }

    if (!gpu_rebuild_skybox_vertices(3.107175588607788f, false, false, nullptr)) {
        gpu_shutdown();
        return false;
    }

    // sub_423210/sub_49D600 build a tessellated sphere and flatten/offset
    // it to form a cloud ellipsoid around the camera. The target's angular
    // coordinates have a longitude singularity; use a continuous X/Z
    // projection in the preview so looking through the zenith cannot turn
    // the texture into radial wedges. Four repeats approximate the target's
    // texture density around the upper ellipsoid.
    constexpr uint32_t cloud_latitudes = 24, cloud_longitudes = 48;
    constexpr float pi = 3.14159265358979323846f, tau = pi * 2.0f;
    std::vector<SkyboxVertex> cloud_vertices;
    cloud_vertices.reserve(cloud_latitudes * cloud_longitudes * 6);
    const auto cloud_vertex = [](float theta, float phi) {
        const float radial = sinf(theta);
        const float source_x = cosf(phi) * radial;
        const float source_y = cosf(theta);
        const float source_z = sinf(phi) * radial;
        return SkyboxVertex{{source_x * 40.0f, (source_y - .75f) * 10.0f, source_z * 40.0f},
                            {source_x * 4.0f, source_z * 4.0f}};
    };
    for (uint32_t latitude = 0; latitude < cloud_latitudes; ++latitude) {
        const float theta0 = pi * latitude / cloud_latitudes;
        const float theta1 = pi * (latitude + 1) / cloud_latitudes;
        for (uint32_t longitude = 0; longitude < cloud_longitudes; ++longitude) {
            const float phi0 = tau * longitude / cloud_longitudes;
            const float phi1 = tau * (longitude + 1) / cloud_longitudes;
            const SkyboxVertex a = cloud_vertex(theta0, phi0), b = cloud_vertex(theta1, phi0);
            const SkyboxVertex c = cloud_vertex(theta1, phi1), d = cloud_vertex(theta0, phi1);
            cloud_vertices.insert(cloud_vertices.end(), {a, b, c, a, c, d});
        }
    }
    D3D11_BUFFER_DESC cloud_vertex_desc{};
    cloud_vertex_desc.ByteWidth = static_cast<UINT>(cloud_vertices.size() * sizeof(SkyboxVertex));
    cloud_vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
    cloud_vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA cloud_vertex_data{cloud_vertices.data()};
    if (FAILED(gpu.device->CreateBuffer(&cloud_vertex_desc, &cloud_vertex_data, &gpu.skybox_cloud_vertices))) {
        gpu_shutdown();
        return false;
    }
    gpu.skybox_cloud_vertex_count = static_cast<uint32_t>(cloud_vertices.size());

    const uint32_t white_pixel = 0xffffffffu;
    D3D11_TEXTURE2D_DESC white_desc{};
    white_desc.Width = white_desc.Height = white_desc.MipLevels = white_desc.ArraySize = 1;
    white_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    white_desc.SampleDesc.Count = 1;
    white_desc.Usage = D3D11_USAGE_IMMUTABLE;
    white_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA white_data{&white_pixel, sizeof(white_pixel), sizeof(white_pixel)};
    ID3D11Texture2D* white_texture = nullptr;
    result = gpu.device->CreateTexture2D(&white_desc, &white_data, &white_texture);
    if (SUCCEEDED(result))
        result = gpu.device->CreateShaderResourceView(white_texture, nullptr, &gpu.white_texture);
    gpu_release(white_texture);
    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(result) || FAILED(gpu.device->CreateSamplerState(&sampler_desc, &gpu.skybox_sampler))) {
        gpu_shutdown();
        return false;
    }
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    if (FAILED(gpu.device->CreateSamplerState(&sampler_desc, &gpu.skybox_cloud_sampler))) {
        gpu_shutdown();
        return false;
    }
    D3D11_BUFFER_DESC constant_desc{};
    constant_desc.ByteWidth = sizeof(DirectX::XMFLOAT4X4);
    constant_desc.Usage = D3D11_USAGE_DEFAULT;
    constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(gpu.device->CreateBuffer(&constant_desc, nullptr, &gpu.camera_buffer))) {
        gpu_shutdown();
        return false;
    }
    constant_desc.ByteWidth = sizeof(SkyboxAnimationConstants);
    if (FAILED(gpu.device->CreateBuffer(&constant_desc, nullptr, &gpu.skybox_animation_buffer))) {
        gpu_shutdown();
        return false;
    }
    D3D11_RASTERIZER_DESC raster_desc{};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = TRUE;
    raster_desc.MultisampleEnable = TRUE;
    if (FAILED(gpu.device->CreateRasterizerState(&raster_desc, &gpu.rasterizer))) {
        gpu_shutdown();
        return false;
    }
    D3D11_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(gpu.device->CreateDepthStencilState(&depth_desc, &gpu.depth_enabled))) {
        gpu_shutdown();
        return false;
    }
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(gpu.device->CreateDepthStencilState(&depth_desc, &gpu.skybox_depth))) {
        gpu_shutdown();
        return false;
    }
    D3D11_BLEND_DESC cloud_blend_desc{};
    cloud_blend_desc.RenderTarget[0].BlendEnable = TRUE;
    cloud_blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    cloud_blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    cloud_blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    cloud_blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    cloud_blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    cloud_blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    cloud_blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(gpu.device->CreateBlendState(&cloud_blend_desc, &gpu.skybox_cloud_blend))) {
        gpu_shutdown();
        return false;
    }
    depth_desc.DepthEnable = FALSE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(gpu.device->CreateDepthStencilState(&depth_desc, &gpu.depth_disabled)) ||
        !gpu_resize(swap_desc.BufferDesc.Width, swap_desc.BufferDesc.Height)) {
        gpu_shutdown();
        return false;
    }
    gpu.ready = true;
    return true;
}

bool gpu_upload_mesh() {
    gpu_release(gpu.mesh_indices);
    gpu_release(gpu.mesh_vertices);
    gpu.mesh_index_count = 0;
    if (!gpu.ready || g.mesh.positions.empty() || g.mesh.faces.empty())
        return gpu.ready;
    std::vector<Asura_Vector_3> normals(g.mesh.positions.size());
    std::vector<uint32_t> indices;
    indices.reserve(g.mesh.faces.size() * 3);
    for (const auto& face : g.mesh.faces) {
        const Asura_Vector_3 ab = sub(g.mesh.positions[face[1]], g.mesh.positions[face[0]]);
        const Asura_Vector_3 ac = sub(g.mesh.positions[face[2]], g.mesh.positions[face[0]]);
        const Asura_Vector_3 n = cross(ab, ac);
        normals[face[0]] = add(normals[face[0]], n);
        normals[face[1]] = add(normals[face[1]], n);
        normals[face[2]] = add(normals[face[2]], n);
        indices.push_back(face[0]);
        indices.push_back(face[1]);
        indices.push_back(face[2]);
    }
    std::vector<GpuVertex> vertices(g.mesh.positions.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        const Asura_Vector_3 n = normalized(normals[i]);
        vertices[i] = {{g.mesh.positions[i].x, g.mesh.positions[i].y, g.mesh.positions[i].z}, {n.x, n.y, n.z},
                       {0, 0, 0, 0}};
    }
    if (vertices.size() > 0xffffffffu / sizeof(GpuVertex) || indices.size() > 0xffffffffu / sizeof(uint32_t))
        return false;
    D3D11_BUFFER_DESC vertex_desc{};
    vertex_desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(GpuVertex));
    vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertex_data{vertices.data()};
    D3D11_BUFFER_DESC index_desc{};
    index_desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    index_desc.Usage = D3D11_USAGE_IMMUTABLE;
    index_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA index_data{indices.data()};
    if (FAILED(gpu.device->CreateBuffer(&vertex_desc, &vertex_data, &gpu.mesh_vertices)) ||
        FAILED(gpu.device->CreateBuffer(&index_desc, &index_data, &gpu.mesh_indices))) {
        gpu_release(gpu.mesh_indices);
        gpu_release(gpu.mesh_vertices);
        return false;
    }
    gpu.mesh_index_count = static_cast<uint32_t>(indices.size());
    return true;
}

GpuVertex gpu_line_vertex(Asura_Vector_3 position, DirectX::XMFLOAT4 color) {
    return {{position.x, position.y, position.z}, {0, 1, 0}, color};
}

bool gpu_update_dynamic_vertices(ID3D11Buffer** buffer, uint32_t* capacity,
                                 const std::vector<GpuVertex>& vertices) {
    if (vertices.empty())
        return true;
    if (vertices.size() > 0xffffffffu / sizeof(GpuVertex))
        return false;
    const uint32_t bytes = static_cast<uint32_t>(vertices.size() * sizeof(GpuVertex));
    if (!*buffer || bytes > *capacity) {
        gpu_release(*buffer);
        const uint64_t aligned = align_up(bytes, 65536);
        if (aligned > 0xffffffffu)
            return false;
        *capacity = static_cast<uint32_t>(aligned);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = *capacity;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(gpu.device->CreateBuffer(&desc, nullptr, buffer)))
            return false;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(gpu.context->Map(*buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    memcpy(mapped.pData, vertices.data(), bytes);
    gpu.context->Unmap(*buffer, 0);
    return true;
}

bool gpu_update_overlay(const std::vector<GpuVertex>& vertices) {
    return gpu_update_dynamic_vertices(&gpu.overlay_vertices, &gpu.overlay_capacity, vertices);
}

void gpu_render_skybox(const DirectX::XMFLOAT4X4& view_projection) {
    if (!gpu.skybox_active || !gpu.skybox_vertices || !gpu.skybox_sampler || !gpu.white_texture)
        return;
    gpu.context->UpdateSubresource(gpu.camera_buffer, 0, nullptr, &view_projection, 0, 0);
    gpu.context->VSSetConstantBuffers(0, 1, &gpu.camera_buffer);
    gpu.context->IASetInputLayout(gpu.skybox_input_layout);
    gpu.context->VSSetShader(gpu.skybox_vertex_shader, nullptr, 0);
    gpu.context->PSSetShader(gpu.skybox_pixel_shader, nullptr, 0);
    gpu.context->PSSetSamplers(0, 1, &gpu.skybox_sampler);
    gpu.context->OMSetDepthStencilState(gpu.skybox_depth, 0);
    const UINT stride = sizeof(SkyboxVertex), offset = 0;
    gpu.context->IASetVertexBuffers(0, 1, &gpu.skybox_vertices, &stride, &offset);
    gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float animation_time = static_cast<float>(fmod(GetTickCount64() * .001, 8192.0));
    const SkyboxAnimationConstants animation{{animation_time / 128.0f, -animation_time / 64.0f},
                                              {animation_time / 64.0f, animation_time / 128.0f},
                                              gpu.skybox_tint};
    gpu.context->UpdateSubresource(gpu.skybox_animation_buffer, 0, nullptr, &animation, 0, 0);
    gpu.context->PSSetConstantBuffers(1, 1, &gpu.skybox_animation_buffer);
    for (uint32_t face = 0; face < 6; ++face) {
        ID3D11ShaderResourceView* texture = gpu.skybox_faces[face] ? gpu.skybox_faces[face] : gpu.white_texture;
        gpu.context->PSSetShader(gpu.skybox_pixel_shader, nullptr, 0);
        gpu.context->PSSetShaderResources(0, 1, &texture);
        gpu.context->PSSetSamplers(0, 1, &gpu.skybox_sampler);
        gpu.context->Draw(6, face * 6);
    }
    if (gpu.skybox_cloud && gpu.skybox_cloud_pixel_shader && gpu.skybox_cloud_sampler &&
        gpu.skybox_cloud_vertices && gpu.skybox_cloud_vertex_count) {
        gpu.context->PSSetShader(gpu.skybox_cloud_pixel_shader, nullptr, 0);
        gpu.context->PSSetShaderResources(1, 1, &gpu.skybox_cloud);
        gpu.context->PSSetSamplers(1, 1, &gpu.skybox_cloud_sampler);
        gpu.context->OMSetBlendState(gpu.skybox_cloud_blend, nullptr, 0xffffffffu);
        gpu.context->IASetVertexBuffers(0, 1, &gpu.skybox_cloud_vertices, &stride, &offset);
        gpu.context->Draw(gpu.skybox_cloud_vertex_count, 0);
        gpu.context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    }
    ID3D11ShaderResourceView* none[] = {nullptr, nullptr};
    gpu.context->PSSetShaderResources(0, _countof(none), none);
}

void append_gpu_spawn_puppet(const Entity& entity, bool selected, std::vector<GpuVertex>* output) {
    const SpawnPuppet* puppet = spawn_puppet_for_team(entity.value_u32_a);
    if (!puppet)
        return;

    DirectX::XMFLOAT4 color;
    if (entity.value_u32_a == 4 || entity.value_u32_a == 5) {
        color = DirectX::XMFLOAT4{ .43f, .52f, .24f, 0 };
        if (selected)
            color = DirectX::XMFLOAT4{ .72f, .82f, .34f, 0 };
    }
    if (entity.value_u32_a == 2 || entity.value_u32_a == 3) {
        color = DirectX::XMFLOAT4{ .25f, .42f, .18f, 0 };
        if (selected)
            color = DirectX::XMFLOAT4{ .40f, .58f, .24f, 0 };
    }
    if (entity.value_u32_a == 1) {
        color = DirectX::XMFLOAT4{ .75f, .78f, .80f, 0 };
        if (selected)
            color = DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 0 };
    }

    for (const auto& face : puppet->faces) {
        for (uint16_t index : face) {
            const SpawnPuppetVertex& source = puppet->vertices[index];
            const Asura_Vector_3 position = spawn_puppet_view_position(source.position, entity);
            const Asura_Vector_3 normal = normalized(spawn_puppet_view_vector(source.normal, entity));
            output->push_back({{position.x, position.y, position.z}, {normal.x, normal.y, normal.z}, color});
        }
    }
}

void append_gpu_pickup_model(const Entity& entity, bool selected, std::vector<GpuVertex>* output) {
    const SpawnPuppet* model = pickup_model_for_skin(entity.pickup_skin_id);
    if (!model)
        return;
    DirectX::XMFLOAT4 color = selected ? DirectX::XMFLOAT4{1.0f, .68f, .22f, 0}
                                       : DirectX::XMFLOAT4{.72f, .31f, .08f, 0};
    for (const auto& face : model->faces) {
        for (uint16_t index : face) {
            const SpawnPuppetVertex& source = model->vertices[index];
            const Asura_Vector_3 position = spawn_puppet_view_position(source.position, entity);
            const Asura_Vector_3 normal = normalized(spawn_puppet_view_vector(source.normal, entity));
            output->push_back({{position.x, position.y, position.z}, {normal.x, normal.y, normal.z}, color});
        }
    }
}

void gpu_render() {
    if (!gpu.ready || !g.viewport)
        return;
    RECT rect{};
    GetClientRect(g.viewport, &rect);
    const uint32_t width = std::max<LONG>(1, rect.right), height = std::max<LONG>(1, rect.bottom);
    if (!gpu_resize(width, height))
        return;
    const float clear[4] = {.075f, .09f, .105f, 1};
    gpu.context->ClearRenderTargetView(gpu.render_target, clear);
    gpu.context->ClearDepthStencilView(gpu.depth_view, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
    gpu.context->OMSetRenderTargets(1, &gpu.render_target, gpu.depth_view);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    gpu.context->RSSetViewports(1, &viewport);
    gpu.context->RSSetState(gpu.rasterizer);
    Asura_Vector_3 camera_position, right, up, forward;
    camera_axes(&camera_position, &right, &up, &forward);
    using namespace DirectX;
    const XMVECTOR eye = XMVectorSet(camera_position.x, camera_position.y, camera_position.z, 1);
    const XMVECTOR target = XMVectorSet(g.camera.target.x, g.camera.target.y, g.camera.target.z, 1);
    const XMVECTOR world_up = XMVectorSet(0, 1, 0, 0);
    const float focal = .85f * static_cast<float>(std::min(width, height));
    const float fov_y = 2.0f * atanf(static_cast<float>(height) / (2.0f * focal));
    const float near_plane = fmaxf(.01f, g.camera.distance * .001f);
    float far_plane = fmaxf(1000.0f, g.camera.distance + g.mesh.radius * 8.0f);
    if (g.selected >= 0 && g.selected < static_cast<int>(g.document.entities.size()) &&
        (g.document.entities[g.selected].kind == EntityKind::Light ||
         g.document.entities[g.selected].kind == EntityKind::Sound)) {
        const Entity& entity = g.document.entities[g.selected];
        const float range = fabsf(entity.kind == EntityKind::Light ? entity.light.Range : entity.value_b);
        if (isfinite(range) && range <= kMaximumLightGizmoRange) {
            const Asura_Vector_3 offset = sub(entity_view_position(entity.position), camera_position);
            far_plane = fmaxf(far_plane, sqrtf(dot(offset, offset)) + range + 1.0f);
        }
    }
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(fov_y, static_cast<float>(width) / height, near_plane,
                                                          far_plane);
    XMFLOAT4X4 skybox_view_projection{};
    const XMVECTOR view_direction = XMVector3Normalize(XMVectorSubtract(target, eye));
    XMStoreFloat4x4(&skybox_view_projection,
                    XMMatrixTranspose(XMMatrixLookToLH(XMVectorZero(), view_direction, world_up) * projection));
    gpu_render_skybox(skybox_view_projection);

    XMFLOAT4X4 view_projection{};
    XMStoreFloat4x4(&view_projection, XMMatrixTranspose(XMMatrixLookAtLH(eye, target, world_up) * projection));
    gpu.context->IASetInputLayout(gpu.input_layout);
    gpu.context->VSSetShader(gpu.vertex_shader, nullptr, 0);
    gpu.context->PSSetShader(gpu.pixel_shader, nullptr, 0);
    gpu.context->UpdateSubresource(gpu.camera_buffer, 0, nullptr, &view_projection, 0, 0);
    gpu.context->VSSetConstantBuffers(0, 1, &gpu.camera_buffer);
    const UINT stride = sizeof(GpuVertex), offset = 0;
    if (gpu.mesh_vertices && gpu.mesh_indices && gpu.mesh_index_count) {
        gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
        gpu.context->IASetVertexBuffers(0, 1, &gpu.mesh_vertices, &stride, &offset);
        gpu.context->IASetIndexBuffer(gpu.mesh_indices, DXGI_FORMAT_R32_UINT, 0);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu.context->DrawIndexed(gpu.mesh_index_count, 0, 0);
    }
    std::vector<GpuVertex> puppet_vertices;
    size_t puppet_vertex_count = 0;
    for (const Entity& entity : g.document.entities) {
        const SpawnPuppet* model = entity_render_model(entity);
        if (model && model->faces.size() <= (SIZE_MAX - puppet_vertex_count) / 3)
            puppet_vertex_count += model->faces.size() * 3;
    }
    puppet_vertices.reserve(puppet_vertex_count);
    for (int i = 0; i < static_cast<int>(g.document.entities.size()); ++i) {
        const Entity& entity = g.document.entities[i];
        if (entity.kind == EntityKind::SpawnPoint)
            append_gpu_spawn_puppet(entity, i == g.selected, &puppet_vertices);
        else if (entity.kind == EntityKind::PhysicalObject)
            append_gpu_pickup_model(entity, i == g.selected, &puppet_vertices);
    }
    const bool puppets_ready =
        gpu_update_dynamic_vertices(&gpu.puppet_vertices, &gpu.puppet_capacity, puppet_vertices) &&
        gpu.puppet_vertices && !puppet_vertices.empty();
    std::vector<GpuVertex> overlay;
    const float extent = fmaxf(50.0f, g.mesh.radius * 1.5f);
    float step = fmaxf(1.0f, powf(10.0f, floorf(log10f(extent / 10.0f))));
    const int lines = static_cast<int>(extent / step);
    const DirectX::XMFLOAT4 minor{.18f, .22f, .25f, 1}, major{.32f, .38f, .42f, 1};
    size_t gizmo_line_capacity = 0;
    if (g.selected >= 0 && g.selected < static_cast<int>(g.document.entities.size())) {
        const EntityKind kind = g.document.entities[g.selected].kind;
        if (kind == EntityKind::Light)
            gizmo_line_capacity = kMaximumLightGizmoLines;
        else if (kind == EntityKind::Sound)
            gizmo_line_capacity = kLightRangeSegments * 3;
    }
    overlay.reserve((lines * 2 + 1) * 4 + g.document.entities.size() * 6 +
                    gizmo_line_capacity * 2);
    for (int i = -lines; i <= lines; ++i) {
        const auto color = i == 0 ? major : minor;
        overlay.push_back(gpu_line_vertex({i * step, 0, -extent}, color));
        overlay.push_back(gpu_line_vertex({i * step, 0, extent}, color));
        overlay.push_back(gpu_line_vertex({-extent, 0, i * step}, color));
        overlay.push_back(gpu_line_vertex({extent, 0, i * step}, color));
    }
    const uint32_t entity_start = static_cast<uint32_t>(overlay.size());
    const float marker = fmaxf(.35f, g.mesh.radius * .008f);
    std::vector<LightGizmoLine> entity_gizmo;
    entity_gizmo.reserve(kMaximumLightGizmoLines);
    for (int i = 0; i < static_cast<int>(g.document.entities.size()); ++i) {
        const Entity& entity = g.document.entities[i];
        const Asura_Vector_3 view_position = entity_view_position(entity.position);
        DirectX::XMFLOAT4 color{1, .45f, .25f, 1};
        if (entity.kind == EntityKind::SpawnPoint)
            color = {.25f, .9f, .45f, 1};
        else if (entity.kind == EntityKind::Light)
            color = {1, .86f, .2f, 1};
        else if (entity.kind == EntityKind::Sound)
            color = {.2f, .7f, 1, 1};
        else if (entity.kind == EntityKind::PhysicalObject)
            color = {1, .45f, .18f, 1};
        else if (entity.kind == EntityKind::AssassinationTarget)
            color = {1, .15f, .25f, 1};
        else if (entity.kind == EntityKind::PositionMarker)
            color = {.75f, .35f, 1, 1};
        const bool selected = i == g.selected;
        if (selected)
            color = {1, 1, 1, 1};
        if (selected && (entity.kind == EntityKind::Light || entity.kind == EntityKind::Sound)) {
            entity_gizmo.clear();
            if (entity.kind == EntityKind::Light)
                append_light_gizmo(entity, marker, &entity_gizmo);
            else
                append_sound_gizmo(entity, &entity_gizmo);
            const DirectX::XMFLOAT4 range_color = entity.kind == EntityKind::Light
                                                       ? DirectX::XMFLOAT4{1, .92f, .35f, 1}
                                                       : DirectX::XMFLOAT4{.25f, .8f, 1, 1};
            const DirectX::XMFLOAT4 direction_color{1, 1, 1, 1};
            for (const LightGizmoLine& line : entity_gizmo) {
                const DirectX::XMFLOAT4 line_color =
                    line.part == LightGizmoPart::Range ? range_color : direction_color;
                overlay.push_back(gpu_line_vertex(line.a, line_color));
                overlay.push_back(gpu_line_vertex(line.b, line_color));
            }
        }
        if (entity_render_model(entity))
            continue;
        const float size = selected ? marker * 1.6f : marker;
        overlay.push_back(gpu_line_vertex(add(view_position, {-size, 0, 0}), color));
        overlay.push_back(gpu_line_vertex(add(view_position, {size, 0, 0}), color));
        overlay.push_back(gpu_line_vertex(add(view_position, {0, -size, 0}), color));
        overlay.push_back(gpu_line_vertex(add(view_position, {0, size, 0}), color));
        overlay.push_back(gpu_line_vertex(add(view_position, {0, 0, -size}), color));
        overlay.push_back(gpu_line_vertex(add(view_position, {0, 0, size}), color));
    }
    if (gpu_update_overlay(overlay) && gpu.overlay_vertices) {
        gpu.context->IASetVertexBuffers(0, 1, &gpu.overlay_vertices, &stride, &offset);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
        if (entity_start)
            gpu.context->Draw(entity_start, 0);
        gpu.context->OMSetDepthStencilState(gpu.depth_disabled, 0);
        if (puppets_ready) {
            // Discard only the environment/grid depth, then let the puppets
            // depth-test and write against themselves and one another.
            gpu.context->ClearDepthStencilView(gpu.depth_view, D3D11_CLEAR_DEPTH, 1, 0);
            gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
            gpu.context->IASetVertexBuffers(0, 1, &gpu.puppet_vertices, &stride, &offset);
            gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            gpu.context->Draw(static_cast<UINT>(puppet_vertices.size()), 0);
            gpu.context->OMSetDepthStencilState(gpu.depth_disabled, 0);
            gpu.context->IASetVertexBuffers(0, 1, &gpu.overlay_vertices, &stride, &offset);
            gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        }
        if (overlay.size() > entity_start)
            gpu.context->Draw(static_cast<UINT>(overlay.size() - entity_start), entity_start);
    }
    gpu.swap_chain->Present(0, 0);
}

void set_status(const char* text) { SetWindowTextA(g.status, text ? text : ""); }

void update_title() {
    std::string title = "Asura 2005 Level Editor";
    const std::string& display_path = !g.document.project_path.empty() ? g.document.project_path
                                      : !g.document.source_pc_path.empty() ? g.document.source_pc_path
                                                                          : g.document.obj_path;
    if (!display_path.empty()) {
        const size_t slash = display_path.find_last_of("\\/");
        title += " - " + display_path.substr(slash == std::string::npos ? 0 : slash + 1);
    }
    if (g.document.dirty)
        title += " *";
    SetWindowTextA(g.window, title.c_str());
}

void mark_dirty() {
    g.document.dirty = true;
    update_title();
}

bool choose_path(HWND owner, bool save, const char* title, const char* filter, const char* extension,
                 std::string* path) {
    char buffer[MAX_PATH * 4]{};
    if (!path->empty())
        strncpy_s(buffer, path->c_str(), _TRUNCATE);
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = sizeof(buffer);
    ofn.lpstrDefExt = extension;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn)))
        return false;
    *path = buffer;
    return true;
}

int CALLBACK initialize_directory_picker(HWND dialog, UINT message, LPARAM, LPARAM initial_path) {
    if (message == BFFM_INITIALIZED && initial_path)
        SendMessageA(dialog, BFFM_SETSELECTIONA, TRUE, initial_path);
    return 0;
}

bool choose_directory(HWND owner, const char* title, std::string* path) {
    char display_name[MAX_PATH]{};
    const HRESULT initialized = OleInitialize(nullptr);
    BROWSEINFOA info{};
    info.hwndOwner = owner;
    info.pszDisplayName = display_name;
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    info.lpfn = initialize_directory_picker;
    info.lParam = path->empty() ? 0 : reinterpret_cast<LPARAM>(path->c_str());
    PIDLIST_ABSOLUTE selected = SHBrowseForFolderA(&info);
    char selected_path[MAX_PATH]{};
    const bool accepted = selected && SHGetPathFromIDListA(selected, selected_path);
    CoTaskMemFree(selected);
    if (SUCCEEDED(initialized))
        OleUninitialize();
    if (!accepted)
        return false;
    *path = selected_path;
    return true;
}

std::string folder_from_path(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

std::string basename_without_extension(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    const size_t start = slash == std::string::npos ? 0 : slash + 1;
    size_t end = path.find_last_of('.');
    if (end == std::string::npos || end < start)
        end = path.size();
    return path.substr(start, end - start);
}

bool decode_spawn_puppet(const RscfInfo& resource, const char* expected_name, SpawnPuppet* output) {
    if (resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
        resource.subtype != ASURA_RESOURCEFILE_TYPE_PC_CHARACTER || !str_ieq_c(resource.name, expected_name))
        return false;
    const Str embedded_name = padded_string_at(resource.payload, resource.payload_size, 0);
    if (!embedded_name.data || !str_ieq_c(embedded_name, expected_name))
        return false;
    const uint64_t counts_at = align_up(static_cast<uint64_t>(embedded_name.size) + 1, 4);
    if (counts_at + 16 > resource.payload_size)
        return false;
    const uint32_t triangle_windows = read_u32(resource.payload + counts_at);
    const uint32_t vertex_count = read_u32(resource.payload + counts_at + 4);
    const uint32_t index_count = read_u32(resource.payload + counts_at + 8);
    constexpr uint32_t vertex_stride = 64;
    const uint64_t vertices_at = counts_at + 16;
    const uint64_t indices_at = vertices_at + static_cast<uint64_t>(vertex_count) * vertex_stride;
    const uint64_t required = indices_at + static_cast<uint64_t>(index_count) * sizeof(uint16_t);
    if (!vertex_count || vertex_count > 65535 || index_count < 3 || triangle_windows != index_count - 2 ||
        required > resource.payload_size)
        return false;

    SpawnPuppet next;
    next.resource_name = expected_name;
    next.vertices.resize(vertex_count);
    for (uint32_t i = 0; i < vertex_count; ++i) {
        const uint8_t* source = resource.payload + vertices_at + static_cast<uint64_t>(i) * vertex_stride;
        SpawnPuppetVertex& vertex = next.vertices[i];
        vertex.position = {read_f32(source), read_f32(source + 4), read_f32(source + 8)};
        vertex.normal = {read_f32(source + 12), read_f32(source + 16), read_f32(source + 20)};
        if (!isfinite(vertex.position.x) || !isfinite(vertex.position.y) || !isfinite(vertex.position.z) ||
            !isfinite(vertex.normal.x) || !isfinite(vertex.normal.y) || !isfinite(vertex.normal.z))
            return false;
        vertex.normal = normalized(vertex.normal);
        if (dot(vertex.normal, vertex.normal) < .5f)
            vertex.normal = {0, -1, 0};
        if (i == 0) {
            next.min = next.max = vertex.position;
        } else {
            next.min.x = fminf(next.min.x, vertex.position.x);
            next.min.y = fminf(next.min.y, vertex.position.y);
            next.min.z = fminf(next.min.z, vertex.position.z);
            next.max.x = fmaxf(next.max.x, vertex.position.x);
            next.max.y = fmaxf(next.max.y, vertex.position.y);
            next.max.z = fmaxf(next.max.z, vertex.position.z);
        }
    }

    const uint8_t* strip_data = resource.payload + indices_at;
    next.faces.reserve(index_count - 2);
    for (uint32_t window = 0; window + 2 < index_count; ++window) {
        uint16_t a = read_u16(strip_data + window * 2);
        uint16_t b = read_u16(strip_data + (window + 1) * 2);
        const uint16_t c = read_u16(strip_data + (window + 2) * 2);
        if (window & 1)
            std::swap(a, b);
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count)
            return false;
        if (a != b && b != c && a != c)
            next.faces.push_back({a, b, c});
    }
    if (next.faces.empty())
        return false;
    *output = std::move(next);
    return true;
}

bool load_spawn_puppets() {
    char module_path[MAX_PATH * 4]{};
    GetModuleFileNameA(nullptr, module_path, static_cast<DWORD>(sizeof(module_path)));
    const std::string module_folder = folder_from_path(module_path);
    std::vector<std::string> candidates = {
        "MPChars.asr", module_folder + "\\MPChars.asr",
        folder_from_path(folder_from_path(module_folder)) + "\\MPChars.asr"};
    for (const std::string& path : candidates) {
        if (path.empty() || !file_exists(path.c_str()))
            continue;
        Arena arena{};
        Error error{};
        ChunkList chunks{};
        std::array<SpawnPuppet, 3> puppets;
        bool parsed = arena_init(&arena, 16 * MiB, &error) && parse_chunks(path.c_str(), &chunks, &arena, &error);
        if (parsed) {
            for (uint32_t i = 0; i < chunks.count; ++i) {
                RscfInfo resource{};
                if (!rscf_info(chunks.chunks[i], &resource))
                    continue;
                if (puppets[0].faces.empty())
                    decode_spawn_puppet(resource, "russian_soldier9", &puppets[0]);
                if (puppets[1].faces.empty())
                    decode_spawn_puppet(resource, "german_elite6", &puppets[1]);
                if (puppets[2].faces.empty())
                    decode_spawn_puppet(resource, "player", &puppets[2]);
            }
        }
        unmap_file(&chunks.file);
        arena_release(&arena);
        if (parsed && !puppets[0].faces.empty() && !puppets[1].faces.empty() && !puppets[2].faces.empty()) {
            g.spawn_puppets = std::move(puppets);
            g.spawn_puppet_source = path;
            return true;
        }
    }
    g.spawn_puppets = {};
    g.spawn_puppet_source.clear();
    return false;
}

void set_control_text(HWND control, const char* text) { SetWindowTextA(control, text ? text : ""); }

void set_float(HWND control, float value) {
    char text[64];
    snprintf(text, sizeof(text), "%.9g", value);
    SetWindowTextA(control, text);
}

float get_float(HWND control, float fallback) {
    char text[128]{};
    GetWindowTextA(control, text, sizeof(text));
    char* end = nullptr;
    const float v = strtof(text, &end);
    return end != text && isfinite(v) ? v : fallback;
}

void set_u32_hex(HWND control, uint32_t value) {
    char text[32];
    snprintf(text, sizeof(text), "0x%08X", value);
    SetWindowTextA(control, text);
}

uint32_t get_u32(HWND control, uint32_t fallback) {
    char text[128]{};
    GetWindowTextA(control, text, sizeof(text));
    char* end = nullptr;
    const unsigned long value = strtoul(text, &end, 0);
    while (end && *end == ' ')
        ++end;
    return end != text && end && !*end && value <= 0xfffffffful ? static_cast<uint32_t>(value) : fallback;
}

enum LightPropertiesId : int {
    ID_LIGHT_POSITION_X = 3000,
    ID_LIGHT_POSITION_Y,
    ID_LIGHT_POSITION_Z,
    ID_LIGHT_DIRECTION_X,
    ID_LIGHT_DIRECTION_Y,
    ID_LIGHT_DIRECTION_Z,
    ID_LIGHT_R,
    ID_LIGHT_G,
    ID_LIGHT_B,
    ID_LIGHT_BRIGHTNESS,
    ID_LIGHT_RANGE,
    ID_LIGHT_INNER_RANGE,
    ID_LIGHT_ANGLE,
    ID_LIGHT_SHADOW_STRENGTH,
    ID_LIGHT_BRIGHTNESS_OVER_RANGE,
    ID_LIGHT_BOUND_MIN_X,
    ID_LIGHT_BOUND_MAX_X,
    ID_LIGHT_BOUND_MIN_Y,
    ID_LIGHT_BOUND_MAX_Y,
    ID_LIGHT_BOUND_MIN_Z,
    ID_LIGHT_BOUND_MAX_Z,
    ID_LIGHT_FLAGS,
    ID_LIGHT_FLAG_FIRST,
    ID_LIGHT_FLAG_LAST = ID_LIGHT_FLAG_FIRST + 6,
    ID_LIGHT_OLD_POSITION_X,
    ID_LIGHT_OLD_POSITION_Y,
    ID_LIGHT_OLD_POSITION_Z,
    ID_LIGHT_OLD_RANGE,
    ID_LIGHT_HAS_CHANGED,
};

struct LightFlagControl {
    const char* label;
    uint32_t mask;
};

constexpr LightFlagControl kLightFlagControls[] = {
    {"Has corona", ASURA_LIGHT_FLAG_HAS_CORONA},
    {"Infinite light", ASURA_LIGHT_FLAG_INFINITE_LIGHT},
    {"Affects entities", ASURA_LIGHT_FLAG_AFFECTS_ENTITIES},
    {"Affects environment", ASURA_LIGHT_FLAG_AFFECTS_ENVIRONMENT},
    {"Volumetric", ASURA_LIGHT_FLAG_IS_VOLUMETRIC},
    {"Use bounding box", ASURA_LIGHT_FLAG_USE_BOUNDING_BOX},
    {"Shadow volume", ASURA_LIGHT_FLAG_IS_SHADOW_VOLUME},
};

struct LightPropertiesState {
    HWND window = nullptr;
    HWND position[3]{};
    HWND direction[3]{};
    HWND colour[3]{};
    HWND brightness = nullptr;
    HWND range = nullptr;
    HWND inner_range = nullptr;
    HWND angle = nullptr;
    HWND shadow_strength = nullptr;
    HWND brightness_over_range = nullptr;
    HWND bounds[6]{};
    HWND flags = nullptr;
    HWND flag_checks[7]{};
    HWND old_position[3]{};
    HWND old_range = nullptr;
    HWND has_changed = nullptr;
    Asura_Light value{};
    bool accepted = false;
};

HWND make_light_control(LightPropertiesState* state, const char* cls, const char* text, DWORD style, int id,
                        int x, int y, int width, int height) {
    HWND control = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                                   state->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandle(nullptr), nullptr);
    SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return control;
}

void make_light_vector_row(LightPropertiesState* state, const char* label, int y, int first_id, HWND fields[3]) {
    make_light_control(state, "STATIC", label, SS_LEFT, 0, 14, y + 3, 94, 22);
    constexpr const char* axes[] = {"X", "Y", "Z"};
    constexpr int xs[] = {130, 334, 538};
    for (int i = 0; i < 3; ++i) {
        make_light_control(state, "STATIC", axes[i], SS_LEFT, 0, xs[i] - 18, y + 3, 16, 22);
        fields[i] = make_light_control(state, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                       first_id + i, xs[i], y, 142, 24);
    }
}

void make_light_scalar(LightPropertiesState* state, const char* label, int id, int x, int y, HWND* field) {
    make_light_control(state, "STATIC", label, SS_LEFT, 0, x, y + 3, 112, 22);
    *field = make_light_control(state, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, id,
                                x + 116, y, 104, 24);
}

void create_light_properties_controls(LightPropertiesState* state) {
    make_light_vector_row(state, "Position", 16, ID_LIGHT_POSITION_X, state->position);
    make_light_vector_row(state, "Direction", 50, ID_LIGHT_DIRECTION_X, state->direction);
    make_light_vector_row(state, "Colour (RGB255)", 84, ID_LIGHT_R, state->colour);

    make_light_scalar(state, "Brightness", ID_LIGHT_BRIGHTNESS, 14, 122, &state->brightness);
    make_light_scalar(state, "Range", ID_LIGHT_RANGE, 262, 122, &state->range);
    make_light_scalar(state, "Inner range", ID_LIGHT_INNER_RANGE, 510, 122, &state->inner_range);
    make_light_scalar(state, "Angle", ID_LIGHT_ANGLE, 14, 156, &state->angle);
    make_light_scalar(state, "Shadow strength", ID_LIGHT_SHADOW_STRENGTH, 262, 156,
                      &state->shadow_strength);
    make_light_scalar(state, "Brightness/range", ID_LIGHT_BRIGHTNESS_OVER_RANGE, 510, 156,
                      &state->brightness_over_range);

    make_light_control(state, "STATIC", "Bounding box", SS_LEFT, 0, 14, 198, 110, 22);
    constexpr const char* bound_labels[] = {"Min X", "Max X", "Min Y", "Max Y", "Min Z", "Max Z"};
    constexpr int bound_ids[] = {ID_LIGHT_BOUND_MIN_X, ID_LIGHT_BOUND_MAX_X, ID_LIGHT_BOUND_MIN_Y,
                                 ID_LIGHT_BOUND_MAX_Y, ID_LIGHT_BOUND_MIN_Z, ID_LIGHT_BOUND_MAX_Z};
    for (int i = 0; i < 6; ++i) {
        const int column = i % 3, row = i / 3;
        const int x = 130 + column * 204, y = 194 + row * 34;
        make_light_control(state, "STATIC", bound_labels[i], SS_LEFT, 0, x, y + 3, 48, 22);
        state->bounds[i] = make_light_control(state, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                              bound_ids[i], x + 52, y, 90, 24);
    }

    make_light_control(state, "STATIC", "Raw flags", SS_LEFT, 0, 14, 270, 94, 22);
    state->flags = make_light_control(state, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                      ID_LIGHT_FLAGS, 130, 267, 142, 24);
    for (int i = 0; i < 7; ++i) {
        const int column = i % 4, row = i / 4;
        state->flag_checks[i] = make_light_control(
            state, "BUTTON", kLightFlagControls[i].label, BS_AUTOCHECKBOX | WS_TABSTOP,
            ID_LIGHT_FLAG_FIRST + i, 14 + column * 185, 304 + row * 30, 178, 24);
    }

    make_light_vector_row(state, "Old position", 374, ID_LIGHT_OLD_POSITION_X, state->old_position);
    make_light_scalar(state, "Old range", ID_LIGHT_OLD_RANGE, 14, 412, &state->old_range);
    state->has_changed = make_light_control(state, "BUTTON", "Has changed", BS_AUTOCHECKBOX | WS_TABSTOP,
                                            ID_LIGHT_HAS_CHANGED, 280, 412, 150, 24);

    make_light_control(state, "BUTTON", "Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 526, 466, 104, 30);
    make_light_control(state, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 638, 466, 104, 30);

    const Asura_Light& light = state->value;
    const float position[] = {light.Position.x, light.Position.y, light.Position.z};
    const float direction[] = {light.Direction.x, light.Direction.y, light.Direction.z};
    const float colour[] = {light.R, light.G, light.B};
    const float bounds[] = {light.m_xBoundingBox.MinX, light.m_xBoundingBox.MaxX,
                            light.m_xBoundingBox.MinY, light.m_xBoundingBox.MaxY,
                            light.m_xBoundingBox.MinZ, light.m_xBoundingBox.MaxZ};
    const float old_position[] = {light.OldPosition.x, light.OldPosition.y, light.OldPosition.z};
    for (int i = 0; i < 3; ++i) {
        set_float(state->position[i], position[i]);
        set_float(state->direction[i], direction[i]);
        set_float(state->colour[i], colour[i]);
        set_float(state->old_position[i], old_position[i]);
    }
    for (int i = 0; i < 6; ++i)
        set_float(state->bounds[i], bounds[i]);
    set_float(state->brightness, light.Brightness);
    set_float(state->range, light.Range);
    set_float(state->inner_range, light.m_fInnerRange);
    set_float(state->angle, light.Angle);
    set_float(state->shadow_strength, light.ShadowStrength);
    set_float(state->brightness_over_range, light.BrightnessOverRange);
    set_u32_hex(state->flags, light.m_uFlags);
    for (int i = 0; i < 7; ++i)
        SendMessageA(state->flag_checks[i], BM_SETCHECK,
                     light.m_uFlags & kLightFlagControls[i].mask ? BST_CHECKED : BST_UNCHECKED, 0);
    set_float(state->old_range, light.OldRange);
    SendMessageA(state->has_changed, BM_SETCHECK, light.HasChanged ? BST_CHECKED : BST_UNCHECKED, 0);
}

void apply_light_properties(LightPropertiesState* state) {
    Asura_Light& light = state->value;
    light.Position = {get_float(state->position[0], light.Position.x),
                      get_float(state->position[1], light.Position.y),
                      get_float(state->position[2], light.Position.z)};
    light.Direction = {get_float(state->direction[0], light.Direction.x),
                       get_float(state->direction[1], light.Direction.y),
                       get_float(state->direction[2], light.Direction.z)};
    light.R = get_float(state->colour[0], light.R);
    light.G = get_float(state->colour[1], light.G);
    light.B = get_float(state->colour[2], light.B);
    light.Brightness = get_float(state->brightness, light.Brightness);
    light.Range = get_float(state->range, light.Range);
    light.m_fInnerRange = get_float(state->inner_range, light.m_fInnerRange);
    light.Angle = get_float(state->angle, light.Angle);
    light.ShadowStrength = get_float(state->shadow_strength, light.ShadowStrength);
    light.BrightnessOverRange = get_float(state->brightness_over_range, light.BrightnessOverRange);
    float* bounds[] = {&light.m_xBoundingBox.MinX, &light.m_xBoundingBox.MaxX,
                       &light.m_xBoundingBox.MinY, &light.m_xBoundingBox.MaxY,
                       &light.m_xBoundingBox.MinZ, &light.m_xBoundingBox.MaxZ};
    for (int i = 0; i < 6; ++i)
        *bounds[i] = get_float(state->bounds[i], *bounds[i]);
    light.m_uFlags = get_u32(state->flags, light.m_uFlags);
    for (int i = 0; i < 7; ++i) {
        if (SendMessageA(state->flag_checks[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
            light.m_uFlags |= kLightFlagControls[i].mask;
        else
            light.m_uFlags &= ~kLightFlagControls[i].mask;
    }
    light.OldPosition = {get_float(state->old_position[0], light.OldPosition.x),
                         get_float(state->old_position[1], light.OldPosition.y),
                         get_float(state->old_position[2], light.OldPosition.z)};
    light.OldRange = get_float(state->old_range, light.OldRange);
    light.HasChanged = SendMessageA(state->has_changed, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

LRESULT CALLBACK light_properties_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTA* create = reinterpret_cast<const CREATESTRUCTA*>(lparam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    LightPropertiesState* state =
        reinterpret_cast<LightPropertiesState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE:
        state->window = hwnd;
        create_light_properties_controls(state);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_LIGHT_FLAGS && HIWORD(wparam) == EN_CHANGE) {
            const uint32_t flags = get_u32(state->flags, state->value.m_uFlags);
            for (int i = 0; i < 7; ++i)
                SendMessageA(state->flag_checks[i], BM_SETCHECK,
                             flags & kLightFlagControls[i].mask ? BST_CHECKED : BST_UNCHECKED, 0);
        } else if (LOWORD(wparam) >= ID_LIGHT_FLAG_FIRST && LOWORD(wparam) <= ID_LIGHT_FLAG_LAST &&
                   HIWORD(wparam) == BN_CLICKED) {
            uint32_t flags = get_u32(state->flags, state->value.m_uFlags);
            const int index = LOWORD(wparam) - ID_LIGHT_FLAG_FIRST;
            if (SendMessageA(state->flag_checks[index], BM_GETCHECK, 0, 0) == BST_CHECKED)
                flags |= kLightFlagControls[index].mask;
            else
                flags &= ~kLightFlagControls[index].mask;
            set_u32_hex(state->flags, flags);
        } else if (LOWORD(wparam) == IDOK) {
            apply_light_properties(state);
            state->accepted = true;
            DestroyWindow(hwnd);
        } else if (LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

void refresh_inspector();

void command_light_properties() {
    if (g.selected < 0 || g.selected >= static_cast<int>(g.document.entities.size()) ||
        g.document.entities[g.selected].kind != EntityKind::Light)
        return;
    Entity& entity = g.document.entities[g.selected];
    LightPropertiesState state{};
    state.value = entity.light;
    state.value.Position = entity.position;

    RECT owner{};
    GetWindowRect(g.window, &owner);
    constexpr int width = 780, height = 550;
    const int x = owner.left + std::max(0L, (owner.right - owner.left - width) / 2);
    const int y = owner.top + std::max(0L, (owner.bottom - owner.top - height) / 2);
    HWND window = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, "Asura2005LightProperties",
                                  "Light properties", WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
                                  x, y, width, height, g.window, nullptr, GetModuleHandle(nullptr), &state);
    if (!window)
        return;
    EnableWindow(g.window, FALSE);
    MSG message{};
    bool quit = false;
    while (IsWindow(window)) {
        const BOOL result = GetMessageA(&message, nullptr, 0, 0);
        if (result <= 0) {
            quit = result == 0;
            break;
        }
        if (!IsDialogMessageA(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
    EnableWindow(g.window, TRUE);
    SetActiveWindow(g.window);
    if (quit)
        PostQuitMessage(static_cast<int>(message.wParam));
    if (!state.accepted)
        return;

    entity.light = state.value;
    entity.position = state.value.Position;
    entity.value_a = state.value.Brightness;
    entity.value_b = state.value.Range;
    mark_dirty();
    refresh_inspector();
    request_redraw();
}

const PickupTemplate* find_pickup_template(const Document& document, uint32_t item_id, bool require_item) {
    const PickupTemplate* fallback = nullptr;
    for (const PickupTemplate& candidate : document.pickup_templates) {
        if (!fallback)
            fallback = &candidate;
        if (candidate.item_id == item_id)
            return &candidate;
    }
    return require_item ? nullptr : fallback;
}

void adopt_pickup_template(Entity* entity, const PickupTemplate& source) {
    entity->value_a = source.health;
    entity->value_u32_a = source.item_id;
    entity->value_u32_b = source.file_id;
    entity->pickup_skin_id = source.skin_id;
    entity->pickup_anim_id = source.anim_id;
    entity->pickup_anim_file_id = source.anim_file_id;
    entity->pickup_body = source.body;
    entity->pickup_has_template = true;
}

void refresh_list() {
    SendMessageA(g.list, LB_RESETCONTENT, 0, 0);
    for (const Entity& e : g.document.entities) {
        const char* prefix = "Spawn";
        if (e.kind == EntityKind::Light)
            prefix = "Light";
        else if (e.kind == EntityKind::Sound)
            prefix = "Sound";
        else if (e.kind == EntityKind::PhysicalObject)
            prefix = "Pickup";
        else if (e.kind == EntityKind::AssassinationTarget)
            prefix = "Target";
        else if (e.kind == EntityKind::PositionMarker)
            prefix = "Marker";
        std::string line = std::string(prefix) + "  " + e.name;
        SendMessageA(g.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
    if (g.selected >= static_cast<int>(g.document.entities.size()))
        g.selected = static_cast<int>(g.document.entities.size()) - 1;
    if (g.selected >= 0)
        SendMessageA(g.list, LB_SETCURSEL, g.selected, 0);
}

void refresh_inspector() {
    const bool enabled = g.selected >= 0 && g.selected < static_cast<int>(g.document.entities.size());
    const bool source_entity = enabled && g.document.entities[g.selected].source_entity_record;
    const bool pickup = enabled && g.document.entities[g.selected].kind == EntityKind::PhysicalObject;
    HWND fields[] = {g.name, g.pos[0], g.pos[1], g.pos[2], g.rot[0], g.rot[1], g.rot[2], g.value[0], g.value[1]};
    for (HWND h : fields)
        EnableWindow(h, enabled);
    EnableWindow(g.value[0], enabled && (!source_entity || pickup));
    EnableWindow(g.value[1], enabled && !source_entity && !pickup);
    EnableWindow(GetDlgItem(g.window, ID_APPLY_INSPECTOR), enabled);
    EnableWindow(GetDlgItem(g.window, ID_DELETE_ENTITY), enabled && (!source_entity || pickup));
    ShowWindow(g.sound_browse, SW_HIDE);
    ShowWindow(g.sound_loop, SW_HIDE);
    ShowWindow(g.light_properties, SW_HIDE);
    if (!enabled) {
        for (HWND h : fields)
            SetWindowTextA(h, "");
        set_control_text(g.value_label[0], "Property A");
        set_control_text(g.value_label[1], "Property B");
        set_control_text(GetDlgItem(g.window, 914), "Pitch");
        set_control_text(GetDlgItem(g.window, 915), "Yaw");
        set_control_text(GetDlgItem(g.window, 916), "Roll");
        return;
    }
    const Entity& e = g.document.entities[g.selected];
    SetWindowTextA(g.name, e.name.c_str());
    set_float(g.pos[0], e.position.x);
    set_float(g.pos[1], e.position.y);
    set_float(g.pos[2], e.position.z);
    if (e.kind == EntityKind::Light) {
        set_control_text(GetDlgItem(g.window, 914), "Direction X");
        set_control_text(GetDlgItem(g.window, 915), "Direction Y");
        set_control_text(GetDlgItem(g.window, 916), "Direction Z");
        set_float(g.rot[0], e.light.Direction.x);
        set_float(g.rot[1], e.light.Direction.y);
        set_float(g.rot[2], e.light.Direction.z);
    } else {
        set_control_text(GetDlgItem(g.window, 914), "Pitch");
        set_control_text(GetDlgItem(g.window, 915), "Yaw");
        set_control_text(GetDlgItem(g.window, 916), "Roll");
        set_float(g.rot[0], e.rotation.x);
        set_float(g.rot[1], e.rotation.y);
        set_float(g.rot[2], e.rotation.z);
    }
    if (e.kind == EntityKind::SpawnPoint) {
        set_control_text(g.value_label[0], "Team mask");
        set_control_text(g.value_label[1], "Gamemode");
        set_float(g.value[0], static_cast<float>(e.value_u32_a));
        set_float(g.value[1], static_cast<float>(e.value_u32_b));
    } else if (e.kind == EntityKind::Light) {
        set_control_text(g.value_label[0], "Brightness");
        set_control_text(g.value_label[1], "Range");
        set_float(g.value[0], e.light.Brightness);
        set_float(g.value[1], e.light.Range);
        ShowWindow(g.light_properties, SW_SHOW);
    } else if (e.kind == EntityKind::Sound) {
        set_control_text(g.value_label[0], "Min radius");
        set_control_text(g.value_label[1], "Max radius");
        set_float(g.value[0], e.value_a);
        set_float(g.value[1], e.value_b);
        SendMessageA(g.sound_loop, BM_SETCHECK, e.sound_loop ? BST_CHECKED : BST_UNCHECKED, 0);
        ShowWindow(g.sound_loop, SW_SHOW);
        ShowWindow(g.sound_browse, SW_SHOW);
    } else if (e.kind == EntityKind::PhysicalObject) {
        set_control_text(g.value_label[0], "Item ID");
        set_control_text(g.value_label[1], "Object file ID");
        set_u32_hex(g.value[0], e.value_u32_a);
        set_u32_hex(g.value[1], e.value_u32_b);
    } else if (e.kind == EntityKind::AssassinationTarget) {
        set_control_text(g.value_label[0], "Health");
        set_control_text(g.value_label[1], "Class ID");
        set_float(g.value[0], e.value_a);
        set_u32_hex(g.value[1], e.source_entity_classification);
    } else if (e.kind == EntityKind::PositionMarker) {
        set_control_text(g.value_label[0], "Bounds width");
        set_control_text(g.value_label[1], "Bounds depth");
        set_float(g.value[0], e.value_a);
        set_float(g.value[1], e.value_b);
    }
}

void select_entity(int index) {
    g.selected = index;
    if (index >= 0)
        SendMessageA(g.list, LB_SETCURSEL, index, 0);
    refresh_inspector();
    request_redraw();
}

void apply_inspector() {
    if (g.selected < 0 || g.selected >= static_cast<int>(g.document.entities.size()))
        return;
    Entity& e = g.document.entities[g.selected];
    char name[512]{};
    GetWindowTextA(g.name, name, sizeof(name));
    e.name = name;
    e.position = {get_float(g.pos[0], e.position.x), get_float(g.pos[1], e.position.y),
                  get_float(g.pos[2], e.position.z)};
    if (e.kind == EntityKind::Light) {
        e.light.Position = e.position;
        e.light.Direction = {get_float(g.rot[0], e.light.Direction.x),
                             get_float(g.rot[1], e.light.Direction.y),
                             get_float(g.rot[2], e.light.Direction.z)};
    } else {
        e.rotation = {get_float(g.rot[0], e.rotation.x), get_float(g.rot[1], e.rotation.y),
                      get_float(g.rot[2], e.rotation.z)};
    }
    if (e.kind == EntityKind::SpawnPoint) {
        e.value_u32_a = static_cast<uint32_t>(fmaxf(0, get_float(g.value[0], static_cast<float>(e.value_u32_a))));
        e.value_u32_b = static_cast<uint32_t>(fmaxf(0, get_float(g.value[1], static_cast<float>(e.value_u32_b))));
        if (e.spawn_source_record) {
            const float yaw = e.rotation.y * 3.14159265358979323846f / 180.0f;
            const float pitch = e.rotation.x * 3.14159265358979323846f / 180.0f;
            e.spawn_direction = {cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw)};
        }
    } else if (e.kind == EntityKind::Light) {
        e.value_a = get_float(g.value[0], e.light.Brightness);
        e.value_b = get_float(g.value[1], e.light.Range);
        e.light.Brightness = e.value_a;
        e.light.Range = e.value_b;
    } else if (e.kind == EntityKind::Sound) {
        e.value_a = get_float(g.value[0], e.value_a);
        e.value_b = get_float(g.value[1], e.value_b);
        e.sound_loop = SendMessageA(g.sound_loop, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (e.sound_source_record) {
            e.sound_phonon.m_xPosition = e.position;
            e.sound_phonon.m_fInnerRadius = e.value_a;
            e.sound_phonon.m_fOuterRadius = e.value_b;
            e.sound_phonon.m_xOrient = euler_quaternion(e.rotation);
            e.sound_phonon.m_uFlags = e.sound_loop ? (e.sound_phonon.m_uFlags | 1u)
                                                   : (e.sound_phonon.m_uFlags & ~1u);
        }
    } else if (e.kind == EntityKind::PhysicalObject) {
        const uint32_t requested_item = get_u32(g.value[0], e.value_u32_a);
        if (requested_item != e.value_u32_a) {
            const PickupTemplate* item_template = find_pickup_template(g.document, requested_item, true);
            if (item_template) {
                const char* old_item_name = snipe_item_name(e.value_u32_a);
                const bool automatic_name = old_item_name && e.name.rfind(old_item_name, 0) == 0;
                const std::string name_suffix = automatic_name ? e.name.substr(strlen(old_item_name)) : std::string{};
                adopt_pickup_template(&e, *item_template);
                if (automatic_name) {
                    const char* new_item_name = snipe_item_name(requested_item);
                    e.name = std::string(new_item_name ? new_item_name : "Pickup") + name_suffix;
                }
                set_status(snipe_item_label(requested_item).c_str());
            } else {
                char message[192]{};
                snprintf(message, sizeof(message),
                         "Item 0x%02X is not present in the loaded 0x0008 pickup catalog.",
                         requested_item);
                set_status(message);
            }
        }
    }
    mark_dirty();
    refresh_list();
    refresh_inspector();
    request_redraw();
}

void frame_mesh() {
    g.camera.target = g.mesh.center;
    g.camera.distance = fmaxf(10.0f, g.mesh.radius * 2.3f);
    invalidate_environment_cache();
    if (gpu.ready)
        gpu_upload_mesh();
}

bool open_obj_path(const std::string& path) {
    Mesh mesh;
    std::string why;
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const bool ok = load_preview_mesh(path, &mesh, &why);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!ok) {
        MessageBoxA(g.window, why.c_str(), "Could not open OBJ", MB_ICONERROR);
        return false;
    }
    if (!g.document.source_pc_path.empty()) {
        g.document = Document{};
        g.pickup_models.clear();
        g.selected = -1;
        g.pending_kind = -1;
        gpu_load_skybox({}, nullptr);
        refresh_list();
        refresh_inspector();
    }
    g.mesh = std::move(mesh);
    g.document.obj_path = path;
    if (g.document.output_path.empty()) {
        g.document.output_path = path;
        const size_t dot = g.document.output_path.find_last_of('.');
        if (dot != std::string::npos)
            g.document.output_path.resize(dot);
        g.document.output_path += ".PC";
    }
    frame_mesh();
    mark_dirty();
    char status[256];
    snprintf(status, sizeof(status), "%zu vertices, %zu triangles", g.mesh.positions.size(), g.mesh.faces.size());
    set_status(status);
    request_redraw();
    return true;
}

void add_entity_at(EntityKind kind, const Asura_Vector_3& p) {
    Entity e;
    if (kind == EntityKind::PhysicalObject) {
        const PickupTemplate* pickup_template = find_pickup_template(g.document, SnipeItem_RifleAmmo, true);
        if (!pickup_template)
            pickup_template = find_pickup_template(g.document, 0, false);
        if (!pickup_template) {
            g.pending_kind = -1;
            set_status("No 0x0008 pickup catalog is loaded. Choose a Weapons donor .PC first.");
            return;
        }
        adopt_pickup_template(&e, *pickup_template);
        e.entity_padding = pickup_template->entity_padding;
        e.source_entity_record = false;
        e.source_entity_classification = SnipeEntityClass_PhysicalObject;
    }
    e.kind = kind;
    e.position = p;
    e.guid = allocate_editor_guid(&g.document);
    if (!e.guid) {
        g.pending_kind = -1;
        set_status("No free authored entity GUIDs remain in the target's valid range.");
        return;
    }
    e.rotation = {};
    char name[160]{};
    if (kind == EntityKind::SpawnPoint) {
        snprintf(name, sizeof(name), "Spawn %zu", g.document.entities.size() + 1);
        e.value_u32_a = 5;
        e.value_u32_b = 24;
        e.spawn_timer = 5.0f;
    } else if (kind == EntityKind::Light) {
        snprintf(name, sizeof(name), "Light %zu", g.document.entities.size() + 1);
        e.value_a = 2.5f;
        e.value_b = 1500;
        e.rotation.x = -35;
        e.light = legacy_editor_light(e);
    } else if (kind == EntityKind::Sound) {
        snprintf(name, sizeof(name), "Sound %zu", g.document.entities.size() + 1);
        e.value_a = 50;
        e.value_b = 250;
    } else if (kind == EntityKind::PhysicalObject) {
        snprintf(name, sizeof(name), "%s %zu",
                 snipe_item_name(e.value_u32_a) ? snipe_item_name(e.value_u32_a) : "Pickup",
                 g.document.entities.size() + 1);
    }
    e.name = name;
    g.document.entities.push_back(std::move(e));
    g.pending_kind = -1;
    g.selected = static_cast<int>(g.document.entities.size()) - 1;
    mark_dirty();
    refresh_list();
    refresh_inspector();
    set_status("Entity placed. Drag it on the ground plane or edit the numeric transform.");
    request_redraw();
}

void begin_place(EntityKind kind) {
    if (g.mesh.positions.empty()) {
        MessageBoxA(g.window, "Open an environment OBJ or .PC first.", "Level Editor", MB_ICONINFORMATION);
        return;
    }
    if (kind == EntityKind::PhysicalObject && !find_pickup_template(g.document, 0, false)) {
        MessageBoxA(g.window, "Choose a Weapons donor .PC containing 0x0008 pickup definitions first.",
                    "Cannot create pickup", MB_ICONINFORMATION);
        return;
    }
    g.pending_kind = static_cast<int>(kind);
    set_status("Click the viewport to place the entity on the Y=0 ground plane. Right-drag orbits; wheel zooms.");
}

bool spawn_puppet_screen_bounds(const Entity& entity, RECT* bounds, float* nearest_depth = nullptr) {
    const SpawnPuppet* puppet = entity_render_model(entity);
    if (!puppet)
        return false;
    bool projected = false;
    float nearest = 1.0e30f;
    RECT result{};
    for (int corner = 0; corner < 8; ++corner) {
        const Asura_Vector_3 local{
            corner & 1 ? puppet->max.x : puppet->min.x,
            corner & 2 ? puppet->max.y : puppet->min.y,
            corner & 4 ? puppet->max.z : puppet->min.z,
        };
        POINT point{};
        float depth = 0;
        if (!project_point(spawn_puppet_view_position(local, entity), &point, &depth))
            continue;
        if (!projected) {
            result = {point.x, point.y, point.x, point.y};
            projected = true;
        } else {
            result.left = std::min(result.left, point.x);
            result.top = std::min(result.top, point.y);
            result.right = std::max(result.right, point.x);
            result.bottom = std::max(result.bottom, point.y);
        }
        nearest = fminf(nearest, depth);
    }
    if (!projected)
        return false;
    *bounds = result;
    if (nearest_depth)
        *nearest_depth = nearest;
    return true;
}

int hit_entity(int x, int y) {
    int best = -1;
    int best_distance = 15 * 15;
    float best_depth = 1.0e30f;
    for (int i = 0; i < static_cast<int>(g.document.entities.size()); ++i) {
        const Entity& entity = g.document.entities[i];
        if (entity_render_model(entity)) {
            RECT bounds{};
            float depth = 0;
            if (!spawn_puppet_screen_bounds(entity, &bounds, &depth))
                continue;
            bounds.left -= 5;
            bounds.top -= 5;
            bounds.right += 5;
            bounds.bottom += 5;
            const int dx = x < bounds.left ? bounds.left - x : x > bounds.right ? x - bounds.right : 0;
            const int dy = y < bounds.top ? bounds.top - y : y > bounds.bottom ? y - bounds.bottom : 0;
            const int distance = dx * dx + dy * dy;
            if (distance < best_distance || (distance == best_distance && depth < best_depth)) {
                best_distance = distance;
                best_depth = depth;
                best = i;
            }
            continue;
        }
        POINT p{};
        float depth = 0;
        if (!project_point(entity_view_position(entity.position), &p, &depth))
            continue;
        const int dx = p.x - x, dy = p.y - y, d = dx * dx + dy * dy;
        if (d < best_distance || (d == best_distance && depth < best_depth)) {
            best_distance = d;
            best_depth = depth;
            best = i;
        }
    }
    return best;
}

COLORREF entity_color(EntityKind kind) {
    switch (kind) {
    case EntityKind::SpawnPoint: return RGB(80, 220, 120);
    case EntityKind::Light: return RGB(255, 220, 70);
    case EntityKind::Sound: return RGB(80, 190, 255);
    case EntityKind::PhysicalObject: return RGB(255, 116, 46);
    case EntityKind::AssassinationTarget: return RGB(255, 38, 64);
    case EntityKind::PositionMarker: return RGB(190, 88, 255);
    default: return RGB(255, 120, 80);
    }
}

void draw_grid(HDC dc) {
    const float extent = fmaxf(50.0f, g.mesh.radius * 1.5f);
    float step = powf(10.0f, floorf(log10f(extent / 10.0f)));
    step = fmaxf(1.0f, step);
    HPEN minor = CreatePen(PS_SOLID, 1, RGB(48, 55, 62));
    HPEN major = CreatePen(PS_SOLID, 1, RGB(72, 82, 92));
    HGDIOBJ old = SelectObject(dc, minor);
    const int lines = static_cast<int>(extent / step);
    for (int i = -lines; i <= lines; ++i) {
        SelectObject(dc, i == 0 ? major : minor);
        POINT a{}, b{};
        if (project_point({i * step, 0, -extent}, &a) && project_point({i * step, 0, extent}, &b)) {
            MoveToEx(dc, a.x, a.y, nullptr);
            LineTo(dc, b.x, b.y);
        }
        if (project_point({-extent, 0, i * step}, &a) && project_point({extent, 0, i * step}, &b)) {
            MoveToEx(dc, a.x, a.y, nullptr);
            LineTo(dc, b.x, b.y);
        }
    }
    SelectObject(dc, old);
    DeleteObject(minor);
    DeleteObject(major);
}

struct ProjectedMeshVertex {
    POINT point{};
    float depth = 0;
    bool visible = false;
};

void project_mesh_vertices(std::vector<ProjectedMeshVertex>* projected) {
    const RECT vr = viewport_rect();
    Asura_Vector_3 cam, right, up, forward;
    camera_axes(&cam, &right, &up, &forward);
    const float scale = .85f * static_cast<float>(std::min(vr.right - vr.left, vr.bottom - vr.top));
    const float center_x = (vr.left + vr.right) * .5f, center_y = (vr.top + vr.bottom) * .5f;
    projected->resize(g.mesh.positions.size());
    for (size_t i = 0; i < g.mesh.positions.size(); ++i) {
        ProjectedMeshVertex& out = (*projected)[i];
        const Asura_Vector_3 rel = sub(g.mesh.positions[i], cam);
        out.depth = dot(rel, forward);
        out.visible = out.depth > .05f;
        if (!out.visible)
            continue;
        const float x = center_x + dot(rel, right) * scale / out.depth;
        const float y = center_y - dot(rel, up) * scale / out.depth;
        out.point.x = static_cast<LONG>(std::clamp(x, -1000000.0f, 1000000.0f));
        out.point.y = static_cast<LONG>(std::clamp(y, -1000000.0f, 1000000.0f));
    }
}

bool projected_face_visible(const std::array<uint32_t, 3>& face, const std::vector<ProjectedMeshVertex>& projected,
                            const RECT& vr) {
    const ProjectedMeshVertex& a = projected[face[0]];
    const ProjectedMeshVertex& b = projected[face[1]];
    const ProjectedMeshVertex& c = projected[face[2]];
    if (!a.visible || !b.visible || !c.visible)
        return false;
    const LONG min_x = std::min({a.point.x, b.point.x, c.point.x});
    const LONG max_x = std::max({a.point.x, b.point.x, c.point.x});
    const LONG min_y = std::min({a.point.y, b.point.y, c.point.y});
    const LONG max_y = std::max({a.point.y, b.point.y, c.point.y});
    return max_x >= vr.left && min_x < vr.right && max_y >= vr.top && min_y < vr.bottom;
}

void draw_fast_mesh(HDC dc, const RECT& vr, const std::vector<ProjectedMeshVertex>& projected) {
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(125, 148, 160));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    constexpr size_t kBatchFaces = 4096;
    std::vector<POINT> points;
    std::vector<DWORD> counts;
    points.reserve(kBatchFaces * 4);
    counts.reserve(kBatchFaces);
    auto flush = [&]() {
        if (!counts.empty())
            PolyPolyline(dc, points.data(), counts.data(), static_cast<DWORD>(counts.size()));
        points.clear();
        counts.clear();
    };
    for (const auto& face : g.mesh.faces) {
        if (!projected_face_visible(face, projected, vr))
            continue;
        points.push_back(projected[face[0]].point);
        points.push_back(projected[face[1]].point);
        points.push_back(projected[face[2]].point);
        points.push_back(projected[face[0]].point);
        counts.push_back(4);
        if (counts.size() == kBatchFaces)
            flush();
    }
    flush();
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

void draw_shaded_mesh(HDC dc, const RECT& vr, const std::vector<ProjectedMeshVertex>& projected) {
    struct PreviewTriangle {
        POINT points[3];
        float depth;
        uint8_t shade;
    };
    std::vector<PreviewTriangle> triangles;
    triangles.reserve(g.mesh.faces.size());
    const Asura_Vector_3 light = normalized({-.35f, .8f, -.45f});
    for (size_t i = 0; i < g.mesh.faces.size(); ++i) {
        const auto& f = g.mesh.faces[i];
        if (!projected_face_visible(f, projected, vr))
            continue;
        PreviewTriangle triangle{};
        triangle.points[0] = projected[f[0]].point;
        triangle.points[1] = projected[f[1]].point;
        triangle.points[2] = projected[f[2]].point;
        const Asura_Vector_3 ab = sub(g.mesh.positions[f[1]], g.mesh.positions[f[0]]);
        const Asura_Vector_3 ac = sub(g.mesh.positions[f[2]], g.mesh.positions[f[0]]);
        const Asura_Vector_3 normal = normalized(cross(ab, ac));
        const float illumination = .28f + .72f * fabsf(dot(normal, light));
        triangle.depth = (projected[f[0]].depth + projected[f[1]].depth + projected[f[2]].depth) / 3.0f;
        triangle.shade = static_cast<uint8_t>(std::clamp(static_cast<int>(illumination * 15.0f), 0, 15));
        triangles.push_back(triangle);
    }
    std::sort(triangles.begin(), triangles.end(), [](const PreviewTriangle& a, const PreviewTriangle& b) {
        return a.depth > b.depth;
    });
    HBRUSH brushes[16]{};
    HPEN pens[16]{};
    for (int i = 0; i < 16; ++i) {
        const int r = 48 + i * 6, green = 59 + i * 7, b = 65 + i * 8;
        brushes[i] = CreateSolidBrush(RGB(r, green, b));
        pens[i] = CreatePen(PS_SOLID, 1, RGB(std::max(28, r - 22), std::max(34, green - 22), std::max(38, b - 22)));
    }
    HGDIOBJ old_brush = GetCurrentObject(dc, OBJ_BRUSH);
    HGDIOBJ old_pen = GetCurrentObject(dc, OBJ_PEN);
    int selected_shade = -1;
    SetPolyFillMode(dc, WINDING);
    for (const PreviewTriangle& triangle : triangles) {
        if (selected_shade != triangle.shade) {
            selected_shade = triangle.shade;
            SelectObject(dc, brushes[selected_shade]);
            SelectObject(dc, pens[selected_shade]);
        }
        Polygon(dc, triangle.points, 3);
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    for (int i = 0; i < 16; ++i) {
        DeleteObject(brushes[i]);
        DeleteObject(pens[i]);
    }
}

void draw_environment(HDC dc) {
    const RECT vr = viewport_rect();
    HBRUSH bg = CreateSolidBrush(RGB(25, 30, 35));
    FillRect(dc, &vr, bg);
    DeleteObject(bg);
    HRGN clip = CreateRectRgn(vr.left, vr.top, vr.right, vr.bottom);
    SelectClipRgn(dc, clip);
    draw_grid(dc);
    std::vector<ProjectedMeshVertex> projected;
    project_mesh_vertices(&projected);
    if (g.fast_preview)
        draw_fast_mesh(dc, vr, projected);
    else
        draw_shaded_mesh(dc, vr, projected);
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    FrameRect(dc, &vr, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
}

void draw_light_gizmo(HDC dc, const Entity& entity, bool selected) {
    const float marker = fmaxf(.35f, g.mesh.radius * .008f);
    std::vector<LightGizmoLine> lines;
    lines.reserve(kMaximumLightGizmoLines);
    append_light_gizmo(entity, marker, &lines);
    if (lines.empty())
        return;
    HPEN range_pen = CreatePen(PS_SOLID, selected ? 2 : 1,
                               selected ? RGB(255, 235, 90) : RGB(148, 120, 24));
    HPEN direction_pen = CreatePen(PS_SOLID, selected ? 2 : 1,
                                   selected ? RGB(255, 255, 255) : RGB(255, 155, 22));
    HGDIOBJ old_pen = SelectObject(dc, range_pen);
    LightGizmoPart selected_part = LightGizmoPart::Range;
    for (const LightGizmoLine& line : lines) {
        if (line.part != selected_part) {
            selected_part = line.part;
            SelectObject(dc, selected_part == LightGizmoPart::Range ? range_pen : direction_pen);
        }
        POINT a{}, b{};
        if (project_point(line.a, &a) && project_point(line.b, &b)) {
            MoveToEx(dc, a.x, a.y, nullptr);
            LineTo(dc, b.x, b.y);
        }
    }
    SelectObject(dc, old_pen);
    DeleteObject(range_pen);
    DeleteObject(direction_pen);
}

void draw_sound_gizmo(HDC dc, const Entity& entity) {
    std::vector<LightGizmoLine> lines;
    lines.reserve(kLightRangeSegments * 3);
    append_sound_gizmo(entity, &lines);
    if (lines.empty())
        return;
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(65, 205, 255));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    for (const LightGizmoLine& line : lines) {
        POINT a{}, b{};
        if (project_point(line.a, &a) && project_point(line.b, &b)) {
            MoveToEx(dc, a.x, a.y, nullptr);
            LineTo(dc, b.x, b.y);
        }
    }
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

bool draw_spawn_puppet(HDC dc, const Entity& entity, bool selected) {
    const SpawnPuppet* puppet = entity_render_model(entity);
    if (!puppet)
        return false;
    COLORREF color = entity.kind == EntityKind::PhysicalObject
                         ? RGB(210, 92, 28)
                         : entity.value_u32_a == 5 ? RGB(135, 165, 67) : RGB(112, 128, 138);
    if (selected) {
        color = entity.kind == EntityKind::PhysicalObject
                    ? RGB(255, 188, 70)
                    : entity.value_u32_a == 5 ? RGB(220, 240, 105) : RGB(190, 218, 232);
    }
    HPEN pen = CreatePen(PS_SOLID, selected ? 2 : 1, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    for (const auto& face : puppet->faces) {
        POINT points[4]{};
        bool visible = true;
        for (int corner = 0; corner < 3; ++corner) {
            const Asura_Vector_3 position =
                spawn_puppet_view_position(puppet->vertices[face[corner]].position, entity);
            visible = visible && project_point(position, &points[corner]);
        }
        if (visible) {
            points[3] = points[0];
            Polyline(dc, points, 4);
        }
    }
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    return true;
}

void draw_entities(HDC dc) {
    const RECT vr = viewport_rect();
    HRGN clip = CreateRectRgn(vr.left, vr.top, vr.right, vr.bottom);
    SelectClipRgn(dc, clip);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(230, 235, 240));
    for (int i = 0; i < static_cast<int>(g.document.entities.size()); ++i) {
        const Entity& e = g.document.entities[i];
        POINT p{};
        if (!project_point(entity_view_position(e.position), &p))
            continue;
        if (e.kind == EntityKind::Light && i == g.selected)
            draw_light_gizmo(dc, e, true);
        else if (e.kind == EntityKind::Sound && i == g.selected)
            draw_sound_gizmo(dc, e);
        if ((e.kind == EntityKind::SpawnPoint || e.kind == EntityKind::PhysicalObject) &&
            draw_spawn_puppet(dc, e, i == g.selected)) {
            if (i == g.selected)
                TextOutA(dc, p.x + 10, p.y - 8, e.name.c_str(), static_cast<int>(e.name.size()));
            continue;
        }
        const COLORREF color = entity_color(e.kind);
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, i == g.selected ? 3 : 1, i == g.selected ? RGB(255, 255, 255) : color);
        HGDIOBJ ob = SelectObject(dc, brush), op = SelectObject(dc, pen);
        const int r = i == g.selected ? 7 : 5;
        Ellipse(dc, p.x - r, p.y - r, p.x + r + 1, p.y + r + 1);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(brush);
        DeleteObject(pen);
        if (i == g.selected)
            TextOutA(dc, p.x + 10, p.y - 8, e.name.c_str(), static_cast<int>(e.name.size()));
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
}

LRESULT CALLBACK viewport_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        gpu_render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (gpu.ready) {
            gpu_resize(std::max(1, static_cast<int>(LOWORD(lparam))),
                       std::max(1, static_cast<int>(HIWORD(lparam))));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        MapWindowPoints(hwnd, g.window, &point, 1);
        return SendMessageA(g.window, message, wparam, MAKELPARAM(point.x, point.y));
    }
    case WM_MOUSEWHEEL:
        return SendMessageA(g.window, message, wparam, lparam);
    case WM_DESTROY:
        gpu_shutdown();
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

HWND make_control(const char* cls, const char* text, DWORD style, int id) {
    HWND h = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.window,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
    SendMessage(h, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return h;
}

void layout_controls() {
    RECT r{};
    GetClientRect(g.window, &r);
    if (g.viewport) {
        const RECT vr = viewport_rect();
        MoveWindow(g.viewport, vr.left, vr.top, vr.right - vr.left, vr.bottom - vr.top, TRUE);
    }
    const int right = r.right - 262;
    const int top_y = 7;
    const struct { int id, x, w; } top[] = {{ID_OPEN_OBJ, 8, 84},          {ID_OPEN_PC, 96, 84},
                                            {ID_OPEN_PROJECT, 184, 84},    {ID_SAVE_PROJECT, 272, 84},
                                            {ID_EXPORT_PC, 360, 90},       {ID_MATERIAL_MAP, 454, 104},
                                            {ID_TEXTURE_DIR, 562, 100},    {ID_WEAPONS_DONOR, 666, 112},
                                            {ID_SKYBOX_TEXTURES, 782, 116}};
    for (auto c : top)
        MoveWindow(GetDlgItem(g.window, c.id), c.x, top_y, c.w, 28, TRUE);
    MoveWindow(g.list, 8, 48, 220, std::max(80, static_cast<int>(r.bottom) - 301), TRUE);
    int y = std::max(140, static_cast<int>(r.bottom) - 245);
    const int bw = 106;
    MoveWindow(GetDlgItem(g.window, ID_ADD_SPAWN), 8, y, bw, 27, TRUE);
    MoveWindow(GetDlgItem(g.window, ID_ADD_LIGHT), 120, y, bw, 27, TRUE);
    y += 31;
    MoveWindow(GetDlgItem(g.window, ID_ADD_PICKUP), 8, y, 218, 27, TRUE);
    y += 31;
    MoveWindow(GetDlgItem(g.window, ID_ADD_SOUND), 8, y, 218, 27, TRUE);
    y += 31;
    MoveWindow(GetDlgItem(g.window, ID_DELETE_ENTITY), 8, y, 218, 27, TRUE);
    y += 38;
    HWND hint = GetDlgItem(g.window, 900);
    MoveWindow(hint, 8, y, 220, 60, TRUE);

    const int label_x = right, edit_x = right + 88, ew = 164;
    int iy = 52;
    MoveWindow(GetDlgItem(g.window, 910), label_x, iy + 3, 84, 22, TRUE);
    MoveWindow(g.name, edit_x, iy, ew, 24, TRUE);
    iy += 34;
    const int labels[] = {911, 912, 913, 914, 915, 916};
    HWND edits[] = {g.pos[0], g.pos[1], g.pos[2], g.rot[0], g.rot[1], g.rot[2]};
    for (int i = 0; i < 6; ++i, iy += 30) {
        MoveWindow(GetDlgItem(g.window, labels[i]), label_x, iy + 3, 84, 22, TRUE);
        MoveWindow(edits[i], edit_x, iy, ew, 24, TRUE);
    }
    MoveWindow(g.value_label[0], label_x, iy + 3, 84, 22, TRUE);
    MoveWindow(g.value[0], edit_x, iy, ew, 24, TRUE);
    iy += 30;
    MoveWindow(g.value_label[1], label_x, iy + 3, 84, 22, TRUE);
    MoveWindow(g.value[1], edit_x, iy, ew, 24, TRUE);
    iy += 34;
    MoveWindow(g.sound_browse, edit_x, iy, ew, 26, TRUE);
    MoveWindow(g.sound_loop, label_x, iy, 82, 26, TRUE);
    MoveWindow(g.light_properties, edit_x, iy, ew, 26, TRUE);
    iy += 34;
    MoveWindow(GetDlgItem(g.window, ID_APPLY_INSPECTOR), label_x, iy, 252, 30, TRUE);
    MoveWindow(g.status, 8, r.bottom - 21, std::max(20, static_cast<int>(r.right) - 16), 18, TRUE);
}

void create_controls() {
    NONCLIENTMETRICSA metrics{sizeof(metrics)};
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    g.font = CreateFontIndirectA(&metrics.lfMessageFont);
    const RECT vr = viewport_rect();
    g.viewport = CreateWindowExA(0, "Asura2005Viewport", "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                 vr.left, vr.top, vr.right - vr.left, vr.bottom - vr.top, g.window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VIEWPORT)), GetModuleHandle(nullptr),
                                 nullptr);
    if (!g.viewport || !gpu_init(g.viewport)) {
        if (g.viewport)
            DestroyWindow(g.viewport);
        g.viewport = nullptr;
    }
    make_control("BUTTON", "Open OBJ", BS_PUSHBUTTON, ID_OPEN_OBJ);
    make_control("BUTTON", "Open .PC", BS_PUSHBUTTON, ID_OPEN_PC);
    make_control("BUTTON", "Open project", BS_PUSHBUTTON, ID_OPEN_PROJECT);
    make_control("BUTTON", "Save project", BS_PUSHBUTTON, ID_SAVE_PROJECT);
    make_control("BUTTON", "Export .PC", BS_DEFPUSHBUTTON, ID_EXPORT_PC);
    make_control("BUTTON", "Material map", BS_PUSHBUTTON, ID_MATERIAL_MAP);
    make_control("BUTTON", "Texture folder", BS_PUSHBUTTON, ID_TEXTURE_DIR);
    make_control("BUTTON", "Weapons donor", BS_PUSHBUTTON, ID_WEAPONS_DONOR);
    make_control("BUTTON", "Skybox textures", BS_PUSHBUTTON, ID_SKYBOX_TEXTURES);
    g.list = make_control("LISTBOX", "", LBS_NOTIFY | WS_VSCROLL | WS_BORDER, ID_ENTITY_LIST);
    make_control("BUTTON", "+ Spawn", BS_PUSHBUTTON, ID_ADD_SPAWN);
    make_control("BUTTON", "+ Light", BS_PUSHBUTTON, ID_ADD_LIGHT);
    make_control("BUTTON", "+ Pickup", BS_PUSHBUTTON, ID_ADD_PICKUP);
    make_control("BUTTON", "+ Sound", BS_PUSHBUTTON, ID_ADD_SOUND);
    make_control("BUTTON", "Delete selected", BS_PUSHBUTTON, ID_DELETE_ENTITY);
    make_control("STATIC", "Right-drag: orbit\r\nMiddle-drag: pan\r\nWheel: zoom", SS_LEFT, 900);
    make_control("STATIC", "Name", SS_LEFT, 910);
    make_control("STATIC", "Position X", SS_LEFT, 911);
    make_control("STATIC", "Position Y (-up)", SS_LEFT, 912);
    make_control("STATIC", "Position Z", SS_LEFT, 913);
    make_control("STATIC", "Pitch", SS_LEFT, 914);
    make_control("STATIC", "Yaw", SS_LEFT, 915);
    make_control("STATIC", "Roll", SS_LEFT, 916);
    g.name = make_control("EDIT", "", ES_AUTOHSCROLL | WS_BORDER, ID_NAME);
    g.pos[0] = make_control("EDIT", "", ES_AUTOHSCROLL | WS_BORDER, ID_POS_X);
    g.pos[1] = make_control("EDIT", "", ES_AUTOHSCROLL | WS_BORDER, ID_POS_Y);
    g.pos[2] = make_control("EDIT", "", ES_AUTOHSCROLL | WS_BORDER, ID_POS_Z);
    g.rot[0] = make_control("EDIT", "", ES_AUTOHSCROLL | WS_BORDER, ID_ROT_X);
    g.rot[1] = make_control("EDIT", "", ES_AUTOHSCROLL | WS_BORDER, ID_ROT_Y);
    g.rot[2] = make_control("EDIT", "", ES_AUTOHSCROLL | WS_BORDER, ID_ROT_Z);
    g.value_label[0] = make_control("STATIC", "Property A", SS_LEFT, 917);
    g.value_label[1] = make_control("STATIC", "Property B", SS_LEFT, 918);
    g.value[0] = make_control("EDIT", "", ES_AUTOHSCROLL | WS_BORDER, ID_VALUE_A);
    g.value[1] = make_control("EDIT", "", ES_AUTOHSCROLL | WS_BORDER, ID_VALUE_B);
    g.sound_browse = make_control("BUTTON", "Choose WAV...", BS_PUSHBUTTON, ID_BROWSE_SOUND);
    g.sound_loop = make_control("BUTTON", "Loop", BS_AUTOCHECKBOX, ID_SOUND_LOOP);
    g.light_properties =
        make_control("BUTTON", "All light properties...", BS_PUSHBUTTON, ID_LIGHT_PROPERTIES);
    make_control("BUTTON", "Apply properties", BS_PUSHBUTTON, ID_APPLY_INSPECTOR);
    g.status = make_control("STATIC", "Open a Blender OBJ or original .PC level to begin.", SS_LEFT, ID_STATUS);
    layout_controls();
    refresh_inspector();
}

bool confirm_discard() {
    if (!g.document.dirty)
        return true;
    return MessageBoxA(g.window, "Discard unsaved editor changes?", "Asura Level Editor",
                       MB_ICONQUESTION | MB_YESNO) == IDYES;
}

void command_open_obj() {
    if (!g.document.source_pc_path.empty() && !confirm_discard())
        return;
    std::string path = g.document.obj_path;
    if (choose_path(g.window, false, "Open environment OBJ", "Wavefront OBJ\0*.obj\0All files\0*.*\0", "obj", &path))
        open_obj_path(path);
}

void enrich_project_pickup_templates(Document* document, const Document& imported) {
    if (document->pickup_templates.empty())
        document->pickup_templates = imported.pickup_templates;
    size_t imported_pickups = 0, matched_pickups = 0;
    for (const Entity& source : imported.entities) {
        if (source.kind != EntityKind::PhysicalObject || !source.source_entity_record)
            continue;
        ++imported_pickups;
        for (Entity& saved : document->entities) {
            if (saved.kind != EntityKind::PhysicalObject || saved.guid != source.guid)
                continue;
            ++matched_pickups;
            if (!saved.pickup_has_template) {
                saved.entity_padding = source.entity_padding;
                saved.source_entity_classification = source.source_entity_classification;
                const PickupTemplate source_template = pickup_template_from_entity(source);
                adopt_pickup_template(&saved, source_template);
            }
            break;
        }
    }
    if (!document->source_pickup_inventory_complete && imported_pickups == matched_pickups)
        document->source_pickup_inventory_complete = true;
}

void merge_pickup_templates(Document* document, const std::vector<PickupTemplate>& incoming) {
    for (const PickupTemplate& pickup : incoming) {
        bool present = false;
        for (const PickupTemplate& existing : document->pickup_templates)
            present |= existing.item_id == pickup.item_id;
        if (!present)
            document->pickup_templates.push_back(pickup);
    }
}

bool load_document_preview(Document* document, Mesh* mesh, std::string* why,
                           std::vector<PickupModel>* pickup_models = nullptr) {
    if (!document->obj_path.empty()) {
        if (!load_preview_mesh(document->obj_path, mesh, why))
            return false;
        if (document->weapons_donor.empty()) {
            if (pickup_models)
                pickup_models->clear();
            return true;
        }
        std::vector<PickupTemplate> donor_templates;
        std::vector<PickupModel> donor_models;
        if (!load_pickup_donor(document->weapons_donor, &donor_templates, &donor_models, why))
            return false;
        merge_pickup_templates(document, donor_templates);
        if (pickup_models)
            *pickup_models = std::move(donor_models);
        return true;
    }
    if (!document->source_pc_path.empty()) {
        Document imported;
        if (!load_pc_level(document->source_pc_path, &imported, mesh, why, pickup_models))
            return false;
        enrich_project_pickup_templates(document, imported);
        return true;
    }
    if (pickup_models)
        pickup_models->clear();
    *mesh = {};
    return true;
}

bool open_pc_path(const std::string& path) {
    set_status("Reading PC environment and supported entities...");
    UpdateWindow(g.window);
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    Document document;
    Mesh mesh;
    std::string why;
    std::vector<PickupModel> pickup_models;
    const bool ok = load_pc_level(path, &document, &mesh, &why, &pickup_models);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!ok) {
        set_status("Could not open the .PC level.");
        MessageBoxA(g.window, why.c_str(), "Could not open .PC", MB_ICONERROR);
        return false;
    }
    g.document = std::move(document);
    g.mesh = std::move(mesh);
    g.pickup_models = std::move(pickup_models);
    g.selected = g.document.entities.empty() ? -1 : 0;
    g.pending_kind = -1;
    std::string skybox_why;
    const bool skybox_loaded = gpu_load_pc_skybox(path, &skybox_why);
    frame_mesh();
    refresh_list();
    refresh_inspector();
    update_title();
    size_t source_object_count = 0;
    for (const Entity& entity : g.document.entities)
        source_object_count += entity.source_entity_record;
    const size_t authored_entity_count = g.document.entities.size() - source_object_count;
    char status[420];
    snprintf(status, sizeof(status),
             skybox_loaded
                 ? "Original .PC loaded with embedded skybox: %zu vertices, %zu triangles, %zu lights/spawns/sounds and %zu source objects/targets/markers."
                 : "Original .PC loaded: %zu vertices, %zu triangles, %zu lights/spawns/sounds and %zu source objects/targets/markers; embedded skybox unavailable.",
             g.mesh.positions.size(), g.mesh.faces.size(), authored_entity_count, source_object_count);
    set_status(status);
    request_redraw();
    return true;
}

void command_open_pc() {
    if (!confirm_discard())
        return;
    std::string path = g.document.source_pc_path;
    if (!choose_path(g.window, false, "Open original Sniper Elite PC level",
                     "Sniper Elite PC level\0*.PC\0All files\0*.*\0", "PC", &path))
        return;
    open_pc_path(path);
}

void command_save_project() {
    std::string path = g.document.project_path;
    if (path.empty() &&
        !choose_path(g.window, true, "Save editor project", "Asura Level Editor project\0*.alev\0", "alev", &path))
        return;
    std::string why;
    if (!save_project(g.document, path.c_str(), &why)) {
        MessageBoxA(g.window, why.c_str(), "Could not save project", MB_ICONERROR);
        return;
    }
    g.document.project_path = path;
    g.document.dirty = false;
    update_title();
    set_status("Project saved.");
}

bool reload_skybox_preview(bool show_warning) {
    std::string why;
    const bool use_embedded = g.document.sky_texture_dir.empty() && !g.document.source_pc_path.empty();
    const bool loaded = use_embedded ? gpu_load_pc_skybox(g.document.source_pc_path, &why)
                                     : gpu_load_skybox(g.document.sky_texture_dir, &why);
    if (!loaded && show_warning && (use_embedded || !g.document.sky_texture_dir.empty()))
        MessageBoxA(g.window, why.c_str(), "Skybox preview unavailable", MB_ICONWARNING);
    request_redraw();
    return loaded;
}

void command_open_project() {
    if (!confirm_discard())
        return;
    std::string path;
    if (!choose_path(g.window, false, "Open editor project", "Asura Level Editor project\0*.alev\0", "alev", &path))
        return;
    Document doc;
    std::string why;
    if (!load_project(&doc, path.c_str(), &why)) {
        MessageBoxA(g.window, why.c_str(), "Could not open project", MB_ICONERROR);
        return;
    }
    Mesh mesh;
    std::vector<PickupModel> pickup_models;
    if (!load_document_preview(&doc, &mesh, &why, &pickup_models)) {
        g.mesh = std::move(mesh);
        g.pickup_models.clear();
        MessageBoxA(g.window, why.c_str(), "Project source level is unavailable", MB_ICONWARNING);
    } else {
        g.mesh = std::move(mesh);
        g.pickup_models = std::move(pickup_models);
    }
    g.document = std::move(doc);
    const bool skybox_loaded = reload_skybox_preview(true);
    g.selected = g.document.entities.empty() ? -1 : 0;
    frame_mesh();
    refresh_list();
    refresh_inspector();
    update_title();
    if (!skybox_loaded)
        set_status("Project loaded; skybox preview is unavailable.");
    else
        set_status(g.document.dirty ? "Project loaded; unsupported legacy entities were removed." : "Project loaded.");
    request_redraw();
}

void command_export() {
    apply_inspector();
    std::string path = g.document.output_path;
    if (path.empty() && !g.document.obj_path.empty()) {
        path = g.document.obj_path;
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos)
            path.resize(dot);
        path += ".PC";
    }
    if (path.empty() && !g.document.source_pc_path.empty())
        path = edited_pc_path(g.document.source_pc_path);
    if (!choose_path(g.window, true, "Export target-game level", "Sniper Elite PC level\0*.PC\0", "PC", &path))
        return;
    set_status("Packing environment, resources, and entities...");
    UpdateWindow(g.window);
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::string why;
    const bool ok = pack_document(g.document, path.c_str(), &why);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!ok) {
        set_status("Export failed.");
        MessageBoxA(g.window, why.c_str(), "Could not export .PC", MB_ICONERROR);
        return;
    }
    g.document.output_path = path;
    mark_dirty();
    set_status(g.document.source_pc_path.empty()
                   ? "Export complete: the .PC contains the environment and editor-authored entities."
                   : "Export complete: source chunks were preserved and editable PC records were updated.");
    MessageBoxA(g.window, path.c_str(), "Exported .PC", MB_ICONINFORMATION);
}

void command_browse_sound() {
    if (g.selected < 0 || g.selected >= static_cast<int>(g.document.entities.size()))
        return;
    Entity& e = g.document.entities[g.selected];
    if (e.kind != EntityKind::Sound)
        return;
    std::string path = e.sound_file;
    if (!choose_path(g.window, false, "Choose sound resource", "Wave audio\0*.wav\0All files\0*.*\0", "wav", &path))
        return;
    e.sound_file = path;
    e.sound_name = "sounds\\" + basename_without_extension(path);
    e.name = basename_without_extension(path);
    mark_dirty();
    refresh_list();
    refresh_inspector();
    set_status(e.sound_name.c_str());
}

void command_material_map() {
    std::string path = g.document.material_map;
    if (choose_path(g.window, false, "Choose material map", "Material map JSON\0*.json\0All files\0*.*\0", "json", &path)) {
        g.document.material_map = path;
        mark_dirty();
        set_status(path.c_str());
    }
}

void command_texture_dir() {
    std::string path = g.document.texture_dir.empty() ? folder_from_path(g.document.obj_path) : g.document.texture_dir;
    if (!choose_directory(g.window, "Choose texture folder", &path))
        return;
    g.document.texture_dir = path;
    mark_dirty();
    set_status(g.document.texture_dir.c_str());
}

void command_weapons_donor() {
    std::string path = g.document.weapons_donor;
    if (!choose_path(g.window, false, "Choose a target-game weapons donor .PC",
                     "Asura PC files\0*.PC\0All files\0*.*\0", "PC", &path))
        return;
    set_status("Reading 0x0008 pickup definitions and models from the weapons donor...");
    UpdateWindow(g.window);
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::vector<PickupTemplate> templates;
    std::vector<PickupModel> models;
    std::string why;
    const bool ok = load_pickup_donor(path, &templates, &models, &why);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!ok) {
        set_status("Weapons donor does not contain a usable pickup catalog.");
        MessageBoxA(g.window, why.c_str(), "Could not load Weapons donor", MB_ICONERROR);
        return;
    }
    for (const Entity& entity : g.document.entities) {
        if (entity.kind != EntityKind::PhysicalObject || entity.source_entity_record)
            continue;
        bool supported = false;
        for (const PickupTemplate& pickup : templates)
            supported |= pickup.item_id == entity.value_u32_a;
        if (!supported) {
            char message[220]{};
            snprintf(message, sizeof(message),
                     "The selected donor has no 0x0008 definition for existing item 0x%02X.", entity.value_u32_a);
            set_status("Weapons donor is missing an item used by this level.");
            MessageBoxA(g.window, message, "Could not switch Weapons donor", MB_ICONERROR);
            return;
        }
    }
    g.document.weapons_donor = path;
    if (g.document.source_pc_path.empty()) {
        g.document.pickup_templates = templates;
        for (Entity& entity : g.document.entities) {
            if (entity.kind != EntityKind::PhysicalObject || entity.source_entity_record)
                continue;
            for (const PickupTemplate& pickup : templates)
                if (pickup.item_id == entity.value_u32_a) {
                    adopt_pickup_template(&entity, pickup);
                    break;
                }
        }
        g.pickup_models = std::move(models);
    } else {
        merge_pickup_templates(&g.document, templates);
        for (PickupModel& model : models) {
            bool present = false;
            for (const PickupModel& existing : g.pickup_models)
                present |= existing.skin_id == model.skin_id;
            if (!present)
                g.pickup_models.push_back(std::move(model));
        }
    }
    mark_dirty();
    char status[220]{};
    snprintf(status, sizeof(status), "Weapons donor loaded: %zu pickup item definitions, %zu rendered models.",
             templates.size(), g.pickup_models.size());
    set_status(status);
    refresh_list();
    refresh_inspector();
    request_redraw();
}

void command_skybox_textures() {
    std::string path = g.document.sky_texture_dir;
    if (!choose_directory(g.window, "Choose skybox texture folder", &path))
        return;
    g.document.sky_texture_dir = path;
    mark_dirty();
    if (reload_skybox_preview(true))
        set_status("Skybox DDS textures loaded into the viewport.");
    else
        set_status("Skybox texture folder saved; preview is unavailable.");
}

void delete_selected() {
    if (g.selected < 0 || g.selected >= static_cast<int>(g.document.entities.size()))
        return;
    if (g.document.entities[g.selected].source_entity_record &&
        g.document.entities[g.selected].kind != EntityKind::PhysicalObject) {
        set_status("Imported target/marker records remain source-preserved and cannot be deleted yet.");
        return;
    }
    g.document.entities.erase(g.document.entities.begin() + g.selected);
    if (g.selected >= static_cast<int>(g.document.entities.size()))
        --g.selected;
    mark_dirty();
    refresh_list();
    refresh_inspector();
    request_redraw();
}

void paint_window() {
    PAINTSTRUCT ps{};
    HDC window_dc = BeginPaint(g.window, &ps);
    RECT client{};
    GetClientRect(g.window, &client);
    if (gpu.ready && g.viewport) {
        HBRUSH panel = CreateSolidBrush(RGB(238, 241, 244));
        FillRect(window_dc, &client, panel);
        DeleteObject(panel);
        EndPaint(g.window, &ps);
        return;
    }
    const int width = std::max(1L, client.right), height = std::max(1L, client.bottom);
    if (!g.environment_cache || g.environment_cache_width != width || g.environment_cache_height != height) {
        release_environment_cache();
        g.environment_cache = CreateCompatibleBitmap(window_dc, width, height);
        g.environment_cache_width = width;
        g.environment_cache_height = height;
    }
    if (!g.environment_cache_valid || g.environment_cache_fast != g.fast_preview) {
        HDC cache_dc = CreateCompatibleDC(window_dc);
        HGDIOBJ old_cache_bitmap = SelectObject(cache_dc, g.environment_cache);
        HBRUSH panel = CreateSolidBrush(RGB(238, 241, 244));
        FillRect(cache_dc, &client, panel);
        DeleteObject(panel);
        SelectObject(cache_dc, g.font);
        draw_environment(cache_dc);
        SelectObject(cache_dc, old_cache_bitmap);
        DeleteDC(cache_dc);
        g.environment_cache_valid = true;
        g.environment_cache_fast = g.fast_preview;
    }
    HDC dc = CreateCompatibleDC(window_dc);
    HBITMAP bitmap = CreateCompatibleBitmap(window_dc, width, height);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    HDC cache_dc = CreateCompatibleDC(window_dc);
    HGDIOBJ old_cache_bitmap = SelectObject(cache_dc, g.environment_cache);
    BitBlt(dc, 0, 0, width, height, cache_dc, 0, 0, SRCCOPY);
    SelectObject(cache_dc, old_cache_bitmap);
    DeleteDC(cache_dc);
    SelectObject(dc, g.font);
    draw_entities(dc);
    BitBlt(window_dc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(g.window, &ps);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g.window = hwnd;
        create_controls();
        if (g.spawn_puppet_source.empty())
            set_status("MPChars.asr was not found or is incompatible; spawnpoints use fallback markers.");
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    case WM_SIZE:
        layout_controls();
        release_environment_cache();
        g.fast_preview = true;
        SetTimer(hwnd, 1, 140, nullptr);
        request_redraw();
        return 0;
    case WM_TIMER:
        if (wparam == 1 && !g.orbiting && !g.panning) {
            KillTimer(hwnd, 1);
            g.fast_preview = false;
            invalidate_environment_cache();
            request_redraw();
        } else if (wparam == 2 && gpu.skybox_cloud) {
            request_redraw();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_window();
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == ID_OPEN_OBJ)
            command_open_obj();
        else if (id == ID_OPEN_PC)
            command_open_pc();
        else if (id == ID_OPEN_PROJECT)
            command_open_project();
        else if (id == ID_SAVE_PROJECT)
            command_save_project();
        else if (id == ID_EXPORT_PC)
            command_export();
        else if (id == ID_MATERIAL_MAP)
            command_material_map();
        else if (id == ID_TEXTURE_DIR)
            command_texture_dir();
        else if (id == ID_WEAPONS_DONOR)
            command_weapons_donor();
        else if (id == ID_SKYBOX_TEXTURES)
            command_skybox_textures();
        else if (id == ID_ADD_SPAWN)
            begin_place(EntityKind::SpawnPoint);
        else if (id == ID_ADD_LIGHT)
            begin_place(EntityKind::Light);
        else if (id == ID_ADD_SOUND)
            begin_place(EntityKind::Sound);
        else if (id == ID_ADD_PICKUP)
            begin_place(EntityKind::PhysicalObject);
        else if (id == ID_DELETE_ENTITY)
            delete_selected();
        else if (id == ID_APPLY_INSPECTOR)
            apply_inspector();
        else if (id == ID_BROWSE_SOUND)
            command_browse_sound();
        else if (id == ID_LIGHT_PROPERTIES)
            command_light_properties();
        else if (id == ID_ENTITY_LIST && HIWORD(wparam) == LBN_SELCHANGE)
            select_entity(static_cast<int>(SendMessageA(g.list, LB_GETCURSEL, 0, 0)));
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const RECT vr = viewport_rect();
        if (!PtInRect(&vr, p))
            break;
        SetFocus(hwnd);
        Asura_Vector_3 world{};
        if (g.pending_kind >= 0 && ground_point_from_screen(p.x, p.y, &world)) {
            add_entity_at(static_cast<EntityKind>(g.pending_kind), world);
            return 0;
        }
        const int hit = hit_entity(p.x, p.y);
        select_entity(hit);
        if (hit >= 0) {
            g.moving_entity = true;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g.moving_entity) {
            g.moving_entity = false;
            ReleaseCapture();
        }
        return 0;
    case WM_RBUTTONDOWN:
        g.orbiting = true;
        g.fast_preview = true;
        invalidate_environment_cache();
        g.last_mouse = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
        return 0;
    case WM_RBUTTONUP:
        g.orbiting = false;
        g.fast_preview = g.panning;
        invalidate_environment_cache();
        request_redraw();
        ReleaseCapture();
        return 0;
    case WM_MBUTTONDOWN:
        g.panning = true;
        g.fast_preview = true;
        invalidate_environment_cache();
        g.last_mouse = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
        return 0;
    case WM_MBUTTONUP:
        g.panning = false;
        g.fast_preview = g.orbiting;
        invalidate_environment_cache();
        request_redraw();
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE: {
        const POINT now{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (g.moving_entity && g.selected >= 0) {
            Asura_Vector_3 p{};
            if (ground_point_from_screen(now.x, now.y, &p)) {
                Entity& e = g.document.entities[g.selected];
                e.position.x = p.x;
                e.position.z = p.z;
                if (e.kind == EntityKind::Light)
                    e.light.Position = e.position;
                mark_dirty();
                refresh_inspector();
                request_redraw();
            }
        } else if (g.orbiting) {
            g.camera.yaw -= (now.x - g.last_mouse.x) * .008f;
            g.camera.pitch += (now.y - g.last_mouse.y) * .008f;
            g.camera.pitch = std::clamp(g.camera.pitch, -1.45f, 1.45f);
            g.last_mouse = now;
            invalidate_environment_cache();
            request_redraw();
        } else if (g.panning) {
            Asura_Vector_3 cam, right, up, forward;
            camera_axes(&cam, &right, &up, &forward);
            const float scale = g.camera.distance * .0018f;
            g.camera.target = add(g.camera.target,
                                  add(mul(right, -(now.x - g.last_mouse.x) * scale),
                                      mul(up, (now.y - g.last_mouse.y) * scale)));
            g.last_mouse = now;
            invalidate_environment_cache();
            request_redraw();
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        g.camera.distance *= delta > 0 ? .86f : 1.16f;
        g.camera.distance = std::clamp(g.camera.distance, .5f, 1000000.0f);
        g.fast_preview = true;
        invalidate_environment_cache();
        SetTimer(hwnd, 1, 140, nullptr);
        request_redraw();
        return 0;
    }
    case WM_DROPFILES: {
        char path[MAX_PATH * 4]{};
        DragQueryFileA(reinterpret_cast<HDROP>(wparam), 0, path, sizeof(path));
        DragFinish(reinterpret_cast<HDROP>(wparam));
        const std::string p = path;
        const size_t dot = p.find_last_of('.');
        const std::string ext = dot == std::string::npos ? "" : p.substr(dot);
        if (_stricmp(ext.c_str(), ".obj") == 0) {
            if (g.document.source_pc_path.empty() || confirm_discard())
                open_obj_path(p);
        }
        else if (_stricmp(ext.c_str(), ".pc") == 0) {
            if (confirm_discard())
                open_pc_path(p);
        }
        else if (_stricmp(ext.c_str(), ".alev") == 0) {
            std::string why;
            Document doc;
            if (load_project(&doc, p.c_str(), &why)) {
                g.document = std::move(doc);
                const bool skybox_loaded = reload_skybox_preview(true);
                load_document_preview(&g.document, &g.mesh, &why, &g.pickup_models);
                frame_mesh();
                refresh_list();
                refresh_inspector();
                update_title();
                set_status(skybox_loaded ? "Project loaded." : "Project loaded; skybox preview is unavailable.");
                request_redraw();
            } else
                MessageBoxA(hwnd, why.c_str(), "Could not open project", MB_ICONERROR);
        }
        return 0;
    }
    case WM_CLOSE:
        if (confirm_discard())
            DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        release_environment_cache();
        if (g.font)
            DeleteObject(g.font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

} // namespace editor

int APIENTRY WinMain(HINSTANCE instance, HINSTANCE, LPSTR command_line, int show) {
    using namespace editor;
    if (__argc == 2 && strcmp(__argv[1], "--spawn-puppet-smoke") == 0) {
        if (!load_spawn_puppets())
            return 4;
        return g.spawn_puppets[0].vertices.size() == 2064 && g.spawn_puppets[0].faces.size() == 2743 &&
                       g.spawn_puppets[1].vertices.size() == 2136 && g.spawn_puppets[1].faces.size() == 2835
                   ? 0
                   : 5;
    }
    if (__argc == 3 && strcmp(__argv[1], "--pickup-donor-catalog-smoke") == 0) {
        std::vector<PickupTemplate> templates;
        std::vector<PickupModel> models;
        std::string why;
        if (!load_pickup_donor(__argv[2], &templates, &models, &why))
            return 23;
        constexpr uint32_t required_items[] = {
            SnipeItem_PistolAmmo, SnipeItem_RifleAmmo, SnipeItem_Panzerfaust, SnipeItem_StickGrenade,
            SnipeItem_FragGrenade, SnipeItem_SmokeGrenade, SnipeItem_Knife, SnipeItem_MedKit,
            SnipeItem_Bandage, SnipeItem_TnT, SnipeItem_Binoculars, SnipeItem_Gewehr43,
            SnipeItem_Mosin91, SnipeItem_SVT40, SnipeItem_Luger, SnipeItem_P38, SnipeItem_PPSH,
            SnipeItem_MP40, SnipeItem_MG42, SnipeItem_DP28, SnipeItem_TimeBomb,
            SnipeItem_Panzerschreck, SnipeItem_PanzerschreckAmmo,
        };
        for (uint32_t item_id : required_items) {
            const PickupTemplate* pickup = nullptr;
            for (const PickupTemplate& candidate : templates)
                if (candidate.item_id == item_id) {
                    pickup = &candidate;
                    break;
                }
            if (!pickup)
                return 24;
            bool model_resolved = false;
            for (const PickupModel& model : models)
                model_resolved |= model.skin_id == pickup->skin_id && !model.mesh.faces.empty();
            if (!model_resolved)
                return 25;
        }
        const PickupTemplate* mg42 = nullptr;
        for (const PickupTemplate& pickup : templates)
            if (pickup.item_id == SnipeItem_MG42)
                mg42 = &pickup;
        return mg42 && mg42->file_id == 0x5ad130dcu && mg42->skin_id == 0x00331598u &&
                       mg42->anim_id == 0x0642be1bu && mg42->anim_file_id == 0xd525ee6eu
                   ? 0
                   : 26;
    }
    if (__argc == 3 && strcmp(__argv[1], "--pc-pickup-model-smoke") == 0) {
        Document document;
        Mesh mesh;
        std::vector<PickupModel> models;
        std::string why;
        if (!load_pc_level(__argv[2], &document, &mesh, &why, &models) || models.empty())
            return 12;
        size_t pickup_count = 0;
        for (const Entity& entity : document.entities) {
            if (entity.kind != EntityKind::PhysicalObject)
                continue;
            ++pickup_count;
            bool resolved = false;
            for (const PickupModel& model : models)
                resolved |= model.skin_id == entity.pickup_skin_id && !model.mesh.vertices.empty() &&
                            !model.mesh.faces.empty();
            if (!resolved)
                return 13;
        }
        return pickup_count ? 0 : 14;
    }
    if (__argc == 4 && strcmp(__argv[1], "--pc-pickup-lifecycle-smoke") == 0) {
        Document document, restored;
        Mesh mesh, restored_mesh;
        std::vector<PickupModel> models, restored_models;
        std::string why;
        if (!load_pc_level(__argv[2], &document, &mesh, &why, &models))
            return 15;
        size_t pickup_count = 0;
        size_t delete_index = SIZE_MAX;
        uint32_t create_item_id = 0;
        for (size_t i = 0; i < document.entities.size(); ++i) {
            const Entity& entity = document.entities[i];
            if (entity.kind != EntityKind::PhysicalObject)
                continue;
            ++pickup_count;
            if (delete_index == SIZE_MAX) {
                delete_index = i;
                create_item_id = entity.value_u32_a;
            }
        }
        const PickupTemplate* creation_template = find_pickup_template(document, create_item_id, true);
        if (!pickup_count || delete_index == SIZE_MAX || !creation_template)
            return 16;
        const uint32_t deleted_guid = document.entities[delete_index].guid;
        document.entities.erase(document.entities.begin() + delete_index);
        Entity created;
        created.kind = EntityKind::PhysicalObject;
        created.source_entity_classification = SnipeEntityClass_PhysicalObject;
        created.entity_padding = creation_template->entity_padding;
        adopt_pickup_template(&created, *creation_template);
        created.source_entity_record = false;
        created.guid = allocate_editor_guid(&document);
        if (!created.guid)
            return 32;
        created.position.x += 3.25f;
        created.position.z -= 1.75f;
        created.name = "Lifecycle smoke pickup";
        const uint32_t created_guid = created.guid;
        document.entities.push_back(created);
        if (!pack_document(document, __argv[3], &why) ||
            !load_pc_level(__argv[3], &restored, &restored_mesh, &why, &restored_models))
            return 17;
        size_t restored_pickups = 0;
        bool deleted_absent = true, created_present = false;
        for (const Entity& entity : restored.entities) {
            if (entity.kind != EntityKind::PhysicalObject)
                continue;
            ++restored_pickups;
            deleted_absent &= entity.guid != deleted_guid;
            created_present |= entity.guid == created_guid && entity.value_u32_a == created.value_u32_a &&
                               entity.pickup_skin_id == created.pickup_skin_id &&
                               nearly_equal(entity.position, created.position);
        }
        return restored_pickups == pickup_count && deleted_absent && created_present &&
                       created_guid >= kToolCreatedGuidFirst && created_guid <= kToolCreatedGuidLast
                   ? 0
                   : 18;
    }
    if (__argc == 4 && strcmp(__argv[1], "--pc-pickup-item-swap-smoke") == 0) {
        Document document, restored;
        Mesh mesh, restored_mesh;
        std::vector<PickupModel> models, restored_models;
        std::string why;
        if (!load_pc_level(__argv[2], &document, &mesh, &why, &models))
            return 27;
        const PickupTemplate* mg42 = find_pickup_template(document, SnipeItem_MG42, true);
        Entity* changed = nullptr;
        for (Entity& entity : document.entities)
            if (entity.kind == EntityKind::PhysicalObject && entity.source_entity_record) {
                changed = &entity;
                break;
            }
        if (!mg42 || !changed)
            return 28;
        const uint32_t guid = changed->guid;
        const Asura_Vector_3 position = changed->position;
        adopt_pickup_template(changed, *mg42);
        if (!pack_document(document, __argv[3], &why) ||
            !load_pc_level(__argv[3], &restored, &restored_mesh, &why, &restored_models))
            return 29;
        for (const Entity& entity : restored.entities) {
            if (entity.kind != EntityKind::PhysicalObject || entity.guid != guid)
                continue;
            bool model_resolved = false;
            for (const PickupModel& model : restored_models)
                model_resolved |= model.skin_id == entity.pickup_skin_id && !model.mesh.faces.empty();
            return entity.value_u32_a == SnipeItem_MG42 && entity.value_u32_b == mg42->file_id &&
                           entity.pickup_skin_id == mg42->skin_id &&
                           entity.pickup_anim_id == mg42->anim_id &&
                           entity.pickup_anim_file_id == mg42->anim_file_id &&
                           nearly_equal(entity.position, position) && model_resolved
                       ? 0
                       : 30;
        }
        return 31;
    }
    const bool gpu_smoke = (__argc == 3 || __argc == 4) && strcmp(__argv[1], "--gpu-smoke") == 0;
    const bool pc_gpu_smoke = __argc == 3 && strcmp(__argv[1], "--pc-gpu-smoke") == 0;
    if (__argc == 5 && strcmp(__argv[1], "--obj-pickup-lifecycle-smoke") == 0) {
        Document document, project_document, restored;
        Mesh project_mesh, restored_mesh;
        std::vector<PickupTemplate> templates;
        std::vector<PickupModel> donor_models, project_models, restored_models;
        std::string why;
        if (!load_pickup_donor(__argv[3], &templates, &donor_models, &why) || templates.empty() ||
            donor_models.empty())
            return 19;
        document.obj_path = __argv[2];
        document.weapons_donor = __argv[3];
        document.pickup_templates = templates;
        const PickupTemplate* creation_template = find_pickup_template(document, SnipeItem_MG42, true);
        if (!creation_template)
            creation_template = &document.pickup_templates.front();
        Entity created;
        created.kind = EntityKind::PhysicalObject;
        created.name = "Custom OBJ pickup smoke";
        created.guid = allocate_editor_guid(&document);
        if (!created.guid)
            return 33;
        created.position = {2.5f, -1.0f, 4.25f};
        created.rotation = {0.0f, 37.0f, 0.0f};
        created.source_entity_classification = SnipeEntityClass_PhysicalObject;
        adopt_pickup_template(&created, *creation_template);
        const uint32_t created_guid = created.guid;
        const uint32_t created_item = created.value_u32_a;
        document.entities.push_back(created);
        const std::string project_path = std::string(__argv[4]) + ".alev";
        if (!save_project(document, project_path.c_str(), &why) ||
            !load_project(&project_document, project_path.c_str(), &why) ||
            !load_document_preview(&project_document, &project_mesh, &why, &project_models) ||
            project_document.pickup_templates.size() != document.pickup_templates.size() ||
            project_models.empty() || !pack_document(project_document, __argv[4], &why) ||
            !load_pc_level(__argv[4], &restored, &restored_mesh, &why, &restored_models))
            return 20;
        for (const Entity& entity : restored.entities) {
            if (entity.kind != EntityKind::PhysicalObject || entity.guid != created_guid)
                continue;
            Snipe_ServerEntity_PhysicalPickup_ChunkDataV0 wire{};
            memcpy(&wire, entity.pickup_body.data(), sizeof(wire));
            bool model_resolved = false;
            for (const PickupModel& model : restored_models)
                model_resolved |= model.skin_id == entity.pickup_skin_id && !model.mesh.faces.empty();
            return valid_physical_pickup_body(wire) && wire.m_uPickupClassID == 999 &&
                           wire.m_uPickupFlags == 2 && wire.m_uItemID == created_item &&
                           wire.m_xPhysicalObject.m_iAnimFlags == 1 &&
                           wire.m_xPhysicalObject.m_iBBIndex == -1 &&
                           (wire.m_xPhysicalObject.m_uStateBits & ~0x3ffu) == 0 &&
                           wire.m_xPhysicalObject.m_uPhysicalObjectFlags == 2 &&
                           wire.m_uNumLinksToBlock == 0 && created_guid >= kToolCreatedGuidFirst &&
                           created_guid <= kToolCreatedGuidLast && nearly_equal(entity.position, created.position) &&
                           nearly_equal_rotation(entity.rotation, created.rotation) && model_resolved
                       ? 0
                       : 21;
        }
        return 22;
    }
    if (__argc == 4 && strcmp(__argv[1], "--pack") == 0) {
        Document doc;
        std::string why;
        return load_project(&doc, __argv[2], &why) && pack_document(doc, __argv[3], &why) ? 0 : 2;
    }
    if (__argc == 4 && strcmp(__argv[1], "--pc-roundtrip") == 0) {
        Document doc;
        Mesh mesh;
        std::string why;
        return load_pc_level(__argv[2], &doc, &mesh, &why) && !mesh.positions.empty() && !mesh.faces.empty() &&
                       pack_document(doc, __argv[3], &why)
                   ? 0
                   : 6;
    }
    if (__argc == 4 && strcmp(__argv[1], "--pc-entity-transform-smoke") == 0) {
        Document doc, restored;
        Mesh mesh, restored_mesh;
        std::vector<Entity> expected;
        std::string why;
        if (!load_pc_level(__argv[2], &doc, &mesh, &why))
            return 9;
        bool moved_object = false, moved_target = false, moved_marker = false;
        for (Entity& entity : doc.entities) {
            bool* moved = nullptr;
            if (entity.kind == EntityKind::PhysicalObject)
                moved = &moved_object;
            else if (entity.kind == EntityKind::AssassinationTarget)
                moved = &moved_target;
            else if (entity.kind == EntityKind::PositionMarker)
                moved = &moved_marker;
            if (!moved || *moved)
                continue;
            entity.position = add(entity.position, {1.25f, -0.5f, 2.75f});
            entity.rotation.y += 7.5f;
            expected.push_back(entity);
            *moved = true;
        }
        if (!moved_object || !moved_target || !moved_marker || !pack_document(doc, __argv[3], &why) ||
            !load_pc_level(__argv[3], &restored, &restored_mesh, &why))
            return 10;
        for (const Entity& wanted : expected) {
            const Entity* actual = find_source_entity(restored, wanted.guid, wanted.source_entity_classification);
            if (!actual || !nearly_equal(actual->position, wanted.position) ||
                !nearly_equal_rotation(actual->rotation, wanted.rotation))
                return 11;
        }
        return 0;
    }
    if (__argc == 5 && strcmp(__argv[1], "--pc-project-roundtrip") == 0) {
        Document imported, restored, exported;
        Mesh mesh, exported_mesh;
        std::string why;
        return load_pc_level(__argv[2], &imported, &mesh, &why) &&
                       save_project(imported, __argv[3], &why) && load_project(&restored, __argv[3], &why) &&
                       restored.entities.size() == imported.entities.size() &&
                       restored.pickup_templates.size() == imported.pickup_templates.size() &&
                       restored.source_pickup_inventory_complete == imported.source_pickup_inventory_complete &&
                       pack_document(restored, __argv[4], &why) &&
                       load_pc_level(__argv[4], &exported, &exported_mesh, &why) &&
                       exported.entities.size() == imported.entities.size()
                   ? 0
                   : 8;
    }
    if (__argc == 4 && strcmp(__argv[1], "--smoke-pack") == 0) {
        Document doc;
        doc.obj_path = __argv[2];
        Entity spawn;
        spawn.kind = EntityKind::SpawnPoint;
        spawn.name = "Smoke Spawn";
        spawn.guid = allocate_editor_guid(&doc);
        spawn.value_u32_a = 5;
        spawn.value_u32_b = 24;
        doc.entities.push_back(spawn);
        Entity light;
        light.kind = EntityKind::Light;
        light.name = "Smoke Light";
        light.guid = allocate_editor_guid(&doc);
        light.position = {0, 4, 0};
        light.rotation.x = -45;
        light.value_a = 2.5f;
        light.value_b = 100;
        light.light = legacy_editor_light(light);
        doc.entities.push_back(light);
        std::string why;
        return pack_document(doc, __argv[3], &why) ? 0 : 2;
    }
    load_spawn_puppets();
    WNDCLASSEXA viewport_class{};
    viewport_class.cbSize = sizeof(viewport_class);
    viewport_class.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    viewport_class.lpfnWndProc = viewport_proc;
    viewport_class.hInstance = instance;
    viewport_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    viewport_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    viewport_class.lpszClassName = "Asura2005Viewport";
    if (!RegisterClassExA(&viewport_class))
        return 1;
    WNDCLASSEXA light_properties_class{};
    light_properties_class.cbSize = sizeof(light_properties_class);
    light_properties_class.lpfnWndProc = light_properties_proc;
    light_properties_class.hInstance = instance;
    light_properties_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    light_properties_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH));
    light_properties_class.lpszClassName = "Asura2005LightProperties";
    if (!RegisterClassExA(&light_properties_class))
        return 1;
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH));
    wc.lpszClassName = "Asura2005LevelEditor";
    if (!RegisterClassExA(&wc))
        return 1;
    HWND window = CreateWindowExA(0, wc.lpszClassName, "Asura 2005 Level Editor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1380, 840, nullptr, nullptr, instance, nullptr);
    if (!window)
        return 1;
    if (pc_gpu_smoke) {
        const bool loaded = open_pc_path(__argv[2]);
        gpu_render();
        bool pickups_resolved = !g.pickup_models.empty();
        for (const Entity& entity : g.document.entities)
            if (entity.kind == EntityKind::PhysicalObject)
                pickups_resolved &= pickup_model_for_skin(entity.pickup_skin_id) != nullptr;
        const bool rendered = loaded && gpu.ready && gpu.mesh_vertices && gpu.mesh_indices && gpu.mesh_index_count &&
                              gpu.skybox_active && gpu.puppet_vertices && gpu.puppet_capacity && pickups_resolved;
        DestroyWindow(window);
        return rendered ? 0 : 7;
    }
    if (gpu_smoke) {
        const bool loaded = open_obj_path(__argv[2]);
        std::string skybox_why;
        const bool skybox_loaded = __argc == 3 || gpu_load_skybox(__argv[3], &skybox_why);
        if (loaded) {
            Entity light;
            light.kind = EntityKind::Light;
            light.name = "GPU Smoke Light";
            light.position = {g.mesh.center.x, -g.mesh.center.y, g.mesh.center.z};
            light.rotation = {-25, 35, 0};
            light.value_a = 2.5f;
            light.value_b = fmaxf(1.0f, g.mesh.radius * .25f);
            light.light = legacy_editor_light(light);
            light.light.Angle = 180.0f;
            g.document.entities.push_back(light);
            Entity sound;
            sound.kind = EntityKind::Sound;
            sound.name = "GPU Smoke Sound";
            sound.position = light.position;
            sound.value_a = fmaxf(1.0f, g.mesh.radius * .1f);
            sound.value_b = fmaxf(2.0f, g.mesh.radius * .3f);
            g.document.entities.push_back(sound);
            Entity russian;
            russian.kind = EntityKind::SpawnPoint;
            russian.name = "GPU Smoke Russian Spawn";
            russian.position = light.position;
            russian.position.x -= 1.5f;
            russian.value_u32_a = 5;
            russian.value_u32_b = 24;
            g.document.entities.push_back(russian);
            Entity german = russian;
            german.name = "GPU Smoke German Spawn";
            german.position.x += 3.0f;
            german.rotation.y = 180.0f;
            german.value_u32_a = 3;
            g.document.entities.push_back(german);
            g.selected = 0;
        }
        gpu_render();
        if (loaded) {
            g.selected = 1;
            gpu_render();
        }
        bool all_skybox_faces = true;
        if (__argc == 4) {
            all_skybox_faces = gpu.skybox_faces[0] == nullptr && gpu.skybox_cloud != nullptr;
            for (uint32_t face = 1; face < 6; ++face)
                all_skybox_faces &= gpu.skybox_faces[face] != nullptr;
        }
        const bool rendered = loaded && skybox_loaded && all_skybox_faces && gpu.ready && gpu.mesh_vertices &&
                              gpu.mesh_indices && gpu.mesh_index_count && gpu.puppet_vertices && gpu.puppet_capacity &&
                              gpu.overlay_vertices && gpu.overlay_capacity;
        DestroyWindow(window);
        return rendered ? 0 : 3;
    }
    ShowWindow(window, show);
    UpdateWindow(window);
    if (command_line && *command_line) {
        std::string path = command_line;
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
            path = path.substr(1, path.size() - 2);
        const size_t dot = path.find_last_of('.');
        const std::string ext = dot == std::string::npos ? "" : path.substr(dot);
        if (_stricmp(ext.c_str(), ".obj") == 0)
            open_obj_path(path);
        else if (_stricmp(ext.c_str(), ".pc") == 0)
            open_pc_path(path);
        else if (_stricmp(ext.c_str(), ".alev") == 0) {
            std::string why;
            Document doc;
            if (load_project(&doc, path.c_str(), &why)) {
                g.document = std::move(doc);
                const bool skybox_loaded = reload_skybox_preview(true);
                load_document_preview(&g.document, &g.mesh, &why, &g.pickup_models);
                frame_mesh();
                refresh_list();
                refresh_inspector();
                update_title();
                set_status(skybox_loaded ? "Project loaded." : "Project loaded; skybox preview is unavailable.");
            }
        }
    }
    MSG message{};
    while (GetMessageA(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return static_cast<int>(message.wParam);
}
