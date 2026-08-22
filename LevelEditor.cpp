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
constexpr uint32_t kProjectVersion = 6;
constexpr float kSpawnCollisionHalfWidth = 0.3f;
constexpr float kSpawnCollisionHeight = 1.8f;
constexpr float kSpawnCollisionVerticalOffset = 0.1f;

enum class EntityKind : uint32_t { SpawnPoint, Light, Sound };

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
};

struct Document {
    std::string project_path;
    std::string obj_path;
    std::string output_path;
    std::string material_map;
    std::string texture_dir;
    std::string weapons_donor;
    std::string sky_texture_dir;
    std::vector<Entity> entities;
    uint32_t next_guid = 0x500000;
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

bool save_project(const Document& doc, const char* path, std::string* why) {
    BinaryWriter w;
    w.raw(kProjectMagic, sizeof(kProjectMagic));
    w.u32(kProjectVersion);
    w.str(doc.obj_path);
    w.str(doc.output_path);
    w.str(doc.material_map);
    w.str(doc.texture_dir);
    w.str(doc.weapons_donor);
    w.str(doc.sky_texture_dir);
    w.u32(doc.next_guid);
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
    next.output_path = r.str();
    next.material_map = r.str();
    next.texture_dir = r.str();
    if (project_version >= 4)
        next.weapons_donor = r.str();
    if (project_version >= 6)
        next.sky_texture_dir = r.str();
    next.next_guid = r.u32();
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
        const uint32_t maximum_kind = project_version <= 4 ? 3u : static_cast<uint32_t>(EntityKind::Sound);
        if (kind > maximum_kind)
            r.ok = false;
        const bool supported = kind <= static_cast<uint32_t>(EntityKind::Sound);
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
        if (!next.positions.empty()) {
            next.min = next.max = next.positions[0];
            for (const Asura_Vector_3& p : next.positions) {
                next.min.x = fminf(next.min.x, p.x);
                next.min.y = fminf(next.min.y, p.y);
                next.min.z = fminf(next.min.z, p.z);
                next.max.x = fmaxf(next.max.x, p.x);
                next.max.y = fmaxf(next.max.y, p.y);
                next.max.z = fmaxf(next.max.z, p.z);
            }
            next.center = {(next.min.x + next.max.x) * .5f, (next.min.y + next.max.y) * .5f,
                           (next.min.z + next.max.z) * .5f};
            const float dx = next.max.x - next.min.x, dy = next.max.y - next.min.y, dz = next.max.z - next.min.z;
            next.radius = fmaxf(5.0f, sqrtf(dx * dx + dy * dy + dz * dz) * .5f);
        }
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

bool append_editor_lights(Buffer* out, const Document& doc, Error* err) {
    uint32_t count = 0;
    for (const Entity& e : doc.entities)
        count += e.kind == EntityKind::Light;
    if (!count)
        return true;
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_LIGHTS, 5, 0, err);
    append_u32(out, count, err);
    const Asura_Vector_3 hdr_a{80, 80, 80}, hdr_b{.3f, .3f, .3f}, hdr_c{130, 100, 50};
    buffer_append(out, &hdr_a, sizeof(hdr_a), err);
    buffer_append(out, &hdr_b, sizeof(hdr_b), err);
    buffer_append(out, &hdr_c, sizeof(hdr_c), err);
    append_u32(out, 1, err);
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
        data.m_xEntity.Guid = kSpawnGuidBase + index;
        data.m_xEntity.Classification = SnipeEntityClass_SpawnPoint;
        data.m_xPosition = e.position;
        const float yaw = e.rotation.y * 3.14159265358979323846f / 180.0f;
        const float pitch = e.rotation.x * 3.14159265358979323846f / 180.0f;
        data.m_xDirection = {cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw)};
        data.m_iSpawnIndex = index;
        data.m_iPosture = 0;
        data.m_uTeamMask = e.value_u32_a;
        data.m_uGameModeMask = e.value_u32_b;
        data.m_fSpawnTimer = 5.0f;
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
    uint32_t index = 0, resource_id = 1;
    for (const Entity& e : doc.entities) {
        if (e.kind != EntityKind::Sound)
            continue;
        SoundEntry& s = sounds->items[index];
        s.name = {e.sound_name.data(), static_cast<uint32_t>(e.sound_name.size())};
        s.file = e.sound_file.empty() ? nullptr : e.sound_file.c_str();
        s.position = e.position;
        s.inner_radius = e.value_a;
        s.outer_radius = e.value_b;
        const float params[7] = {0, 0, 0, 1, 1, 1, 1};
        memcpy(s.legacy_volume_parameters, params, sizeof(params));
        s.inner_cuboid_radius = {5, 5, 5};
        s.outer_cuboid_radius = {10, 10, 10};
        s.orientation = euler_quaternion(e.rotation);
        s.sound_resource_id = resource_id++;
        s.controller_guid = kSoundControllerGuidBase + index;
        s.phonon_guid = kPhononGuidBase + index;
        s.flags = e.sound_loop ? 3u : 2u;
        s.controller_padding = 0x4974;
        s.emit_enti = true;
        s.active = true;
        index++;
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

bool pack_document(const Document& doc, const char* output_path, std::string* why) {
    if (doc.obj_path.empty()) {
        if (why)
            *why = "Open an OBJ before exporting.";
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
    std::array<SpawnPuppet, 2> spawn_puppets;
    std::string spawn_puppet_source;
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
    if (team_mask == 5 && !g.spawn_puppets[0].faces.empty())
        return &g.spawn_puppets[0]; // Russian team + Deathmatch
    if (team_mask == 3 && !g.spawn_puppets[1].faces.empty())
        return &g.spawn_puppets[1]; // German team + Deathmatch
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

static_assert(sizeof(DdsPixelFormat) == 32);
static_assert(sizeof(DdsHeader) == 124);
static_assert(sizeof(DdsHeaderDx10) == 20);

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

bool gpu_create_dds_view(const char* path, ID3D11ShaderResourceView** output, std::string* why) {
    *output = nullptr;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        if (why)
            *why = std::string("Could not open skybox texture: ") + path;
        return false;
    }
    const std::streamoff end = file.tellg();
    if (end < static_cast<std::streamoff>(4 + sizeof(DdsHeader)) || end > static_cast<std::streamoff>(512 * MiB)) {
        if (why)
            *why = std::string("Skybox texture is not a valid DDS file: ") + path;
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || memcmp(bytes.data(), "DDS ", 4) != 0) {
        if (why)
            *why = std::string("Skybox texture does not contain DDS data: ") + path;
        return false;
    }
    DdsHeader header{};
    memcpy(&header, bytes.data() + 4, sizeof(header));
    if (header.size != sizeof(DdsHeader) || header.pixel_format.size != sizeof(DdsPixelFormat) || !header.width ||
        !header.height || header.width > 16384 || header.height > 16384) {
        if (why)
            *why = std::string("Skybox texture has an unsupported DDS header: ") + path;
        return false;
    }

    constexpr uint32_t dds_four_cc = 0x4;
    constexpr uint32_t dds_rgb = 0x40;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    size_t data_at = 4 + sizeof(DdsHeader);
    if ((header.pixel_format.flags & dds_four_cc) && header.pixel_format.four_cc == fourcc('D', 'X', '1', '0')) {
        if (bytes.size() < data_at + sizeof(DdsHeaderDx10)) {
            if (why)
                *why = std::string("Skybox DDS DX10 header is truncated: ") + path;
            return false;
        }
        DdsHeaderDx10 dx10{};
        memcpy(&dx10, bytes.data() + data_at, sizeof(dx10));
        data_at += sizeof(dx10);
        if (dx10.resource_dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D || dx10.array_size != 1 ||
            (dx10.misc_flag & D3D11_RESOURCE_MISC_TEXTURECUBE)) {
            if (why)
                *why = std::string("Skybox DDS must contain one 2D texture: ") + path;
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
            *why = std::string("Skybox DDS pixel format is unsupported: ") + path;
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
        if (row_pitch > 0xffffffffu || size > bytes.size() - data_at) {
            if (why)
                *why = std::string("Skybox DDS mip data is truncated: ") + path;
            return false;
        }
        initial[mip].pSysMem = bytes.data() + data_at;
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
            *why = std::string("Direct3D could not create the skybox texture: ") + path;
        return false;
    }
    return true;
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
    return float4(skyboxTexture.Sample(skyboxSampler, input.uv).rgb, 1.0);
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

    const Asura_Vector_3 target_corners[] = {{-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1},
                                             {-1, -1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, -1, -1}};
    const uint32_t target_faces[6][4] = {{1, 0, 3, 2}, {0, 4, 7, 3}, {3, 7, 6, 2},
                                         {2, 6, 5, 1}, {1, 5, 4, 0}, {4, 5, 6, 7}};
    // SniperElite.exe sub_49D000 assigns these UVs to the four
    // vertices of every face. The later PDB build's base-face order is
    // different and rotates the 2005 PC sky textures in this viewport.
    const DirectX::XMFLOAT2 uv[] = {{1, 1}, {1, 0}, {0, 0}, {0, 1}};
    const uint32_t triangles[] = {0, 1, 2, 0, 2, 3};
    constexpr float skybox_orientation = 3.107175588607788f;
    const float orientation_sin = sinf(skybox_orientation), orientation_cos = cosf(skybox_orientation);
    SkyboxVertex skybox_vertices[36]{};
    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t vertex = 0; vertex < 6; ++vertex) {
            const uint32_t corner_in_face = triangles[vertex];
            const Asura_Vector_3 source = target_corners[target_faces[face][corner_in_face]];
            const Asura_Vector_3 oriented{source.x * orientation_cos + source.z * orientation_sin,
                                          -source.y,
                                          source.z * orientation_cos - source.x * orientation_sin};
            skybox_vertices[face * 6 + vertex] = {{oriented.x, oriented.y, oriented.z}, uv[corner_in_face]};
        }
    }
    D3D11_BUFFER_DESC skybox_vertex_desc{};
    skybox_vertex_desc.ByteWidth = sizeof(skybox_vertices);
    skybox_vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
    skybox_vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA skybox_vertex_data{skybox_vertices};
    if (FAILED(gpu.device->CreateBuffer(&skybox_vertex_desc, &skybox_vertex_data, &gpu.skybox_vertices))) {
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
                                              {animation_time / 64.0f, animation_time / 128.0f}};
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
    DirectX::XMFLOAT4 color = entity.value_u32_a == 5 ? DirectX::XMFLOAT4{.43f, .52f, .24f, 0}
                                                      : DirectX::XMFLOAT4{.25f, .42f, .18f, 0};
    if (selected)
        color = entity.value_u32_a == 5 ? DirectX::XMFLOAT4{.72f, .82f, .34f, 0}
                                        : DirectX::XMFLOAT4{.40f, .58f, .24f, 0};
    for (const auto& face : puppet->faces) {
        for (uint16_t index : face) {
            const SpawnPuppetVertex& source = puppet->vertices[index];
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
        if (entity.kind == EntityKind::SpawnPoint) {
            const SpawnPuppet* puppet = spawn_puppet_for_team(entity.value_u32_a);
            if (puppet && puppet->faces.size() <= (SIZE_MAX - puppet_vertex_count) / 3)
                puppet_vertex_count += puppet->faces.size() * 3;
        }
    }
    puppet_vertices.reserve(puppet_vertex_count);
    for (int i = 0; i < static_cast<int>(g.document.entities.size()); ++i) {
        const Entity& entity = g.document.entities[i];
        if (entity.kind == EntityKind::SpawnPoint)
            append_gpu_spawn_puppet(entity, i == g.selected, &puppet_vertices);
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
        if (entity.kind == EntityKind::SpawnPoint && spawn_puppet_for_team(entity.value_u32_a))
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
    if (!g.document.project_path.empty()) {
        const size_t slash = g.document.project_path.find_last_of("\\/");
        title += " - " + g.document.project_path.substr(slash == std::string::npos ? 0 : slash + 1);
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
        std::array<SpawnPuppet, 2> puppets;
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
            }
        }
        unmap_file(&chunks.file);
        arena_release(&arena);
        if (parsed && !puppets[0].faces.empty() && !puppets[1].faces.empty()) {
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
    snprintf(text, sizeof(text), "%.4g", value);
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

void refresh_list() {
    SendMessageA(g.list, LB_RESETCONTENT, 0, 0);
    for (const Entity& e : g.document.entities) {
        const char* prefix = "Spawn";
        if (e.kind == EntityKind::Light)
            prefix = "Light";
        else if (e.kind == EntityKind::Sound)
            prefix = "Sound";
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
    HWND fields[] = {g.name, g.pos[0], g.pos[1], g.pos[2], g.rot[0], g.rot[1], g.rot[2], g.value[0], g.value[1]};
    for (HWND h : fields)
        EnableWindow(h, enabled);
    EnableWindow(GetDlgItem(g.window, ID_APPLY_INSPECTOR), enabled);
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
    } else if (e.kind == EntityKind::Light) {
        e.value_a = get_float(g.value[0], e.light.Brightness);
        e.value_b = get_float(g.value[1], e.light.Range);
        e.light.Brightness = e.value_a;
        e.light.Range = e.value_b;
    } else if (e.kind == EntityKind::Sound) {
        e.value_a = get_float(g.value[0], e.value_a);
        e.value_b = get_float(g.value[1], e.value_b);
        e.sound_loop = SendMessageA(g.sound_loop, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
    e.kind = kind;
    e.position = p;
    e.guid = g.document.next_guid++;
    char name[160];
    if (kind == EntityKind::SpawnPoint) {
        snprintf(name, sizeof(name), "Spawn %zu", g.document.entities.size() + 1);
        e.value_u32_a = 5;
        e.value_u32_b = 24;
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
    if (g.document.obj_path.empty()) {
        MessageBoxA(g.window, "Open an environment OBJ first.", "Level Editor", MB_ICONINFORMATION);
        return;
    }
    g.pending_kind = static_cast<int>(kind);
    set_status("Click the viewport to place the entity on the Y=0 ground plane. Right-drag orbits; wheel zooms.");
}

bool spawn_puppet_screen_bounds(const Entity& entity, RECT* bounds, float* nearest_depth = nullptr) {
    const SpawnPuppet* puppet = spawn_puppet_for_team(entity.value_u32_a);
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
        if (entity.kind == EntityKind::SpawnPoint && spawn_puppet_for_team(entity.value_u32_a)) {
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
    const SpawnPuppet* puppet = spawn_puppet_for_team(entity.value_u32_a);
    if (!puppet)
        return false;
    COLORREF color = entity.value_u32_a == 5 ? RGB(135, 165, 67) : RGB(112, 128, 138);
    if (selected)
        color = entity.value_u32_a == 5 ? RGB(220, 240, 105) : RGB(190, 218, 232);
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
        if (e.kind == EntityKind::SpawnPoint && draw_spawn_puppet(dc, e, i == g.selected)) {
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
    const struct { int id, x, w; } top[] = {{ID_OPEN_OBJ, 8, 84},         {ID_OPEN_PROJECT, 96, 84},
                                            {ID_SAVE_PROJECT, 184, 84},   {ID_EXPORT_PC, 272, 90},
                                            {ID_MATERIAL_MAP, 366, 104},  {ID_TEXTURE_DIR, 474, 100},
                                            {ID_WEAPONS_DONOR, 578, 112}, {ID_SKYBOX_TEXTURES, 694, 116}};
    for (auto c : top)
        MoveWindow(GetDlgItem(g.window, c.id), c.x, top_y, c.w, 28, TRUE);
    MoveWindow(g.list, 8, 48, 220, std::max(80, static_cast<int>(r.bottom) - 270), TRUE);
    int y = std::max(140, static_cast<int>(r.bottom) - 214);
    const int bw = 106;
    MoveWindow(GetDlgItem(g.window, ID_ADD_SPAWN), 8, y, bw, 27, TRUE);
    MoveWindow(GetDlgItem(g.window, ID_ADD_LIGHT), 120, y, bw, 27, TRUE);
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
    g.status = make_control("STATIC", "Open a Blender OBJ to begin.", SS_LEFT, ID_STATUS);
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
    std::string path = g.document.obj_path;
    if (choose_path(g.window, false, "Open environment OBJ", "Wavefront OBJ\0*.obj\0All files\0*.*\0", "obj", &path))
        open_obj_path(path);
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
    const bool loaded = gpu_load_skybox(g.document.sky_texture_dir, &why);
    if (!loaded && show_warning && !g.document.sky_texture_dir.empty())
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
    if (!doc.obj_path.empty() && !load_preview_mesh(doc.obj_path, &mesh, &why)) {
        MessageBoxA(g.window, why.c_str(), "Project OBJ is unavailable", MB_ICONWARNING);
    } else {
        g.mesh = std::move(mesh);
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
    set_status("Export complete: the .PC contains the environment and editor-authored entities.");
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
    g.document.weapons_donor = path;
    mark_dirty();
    set_status(g.document.weapons_donor.c_str());
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
        if (_stricmp(ext.c_str(), ".obj") == 0)
            open_obj_path(p);
        else if (_stricmp(ext.c_str(), ".alev") == 0) {
            std::string why;
            Document doc;
            if (load_project(&doc, p.c_str(), &why)) {
                g.document = std::move(doc);
                const bool skybox_loaded = reload_skybox_preview(true);
                if (!g.document.obj_path.empty())
                    load_preview_mesh(g.document.obj_path, &g.mesh, &why);
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
    const bool gpu_smoke = (__argc == 3 || __argc == 4) && strcmp(__argv[1], "--gpu-smoke") == 0;
    if (__argc == 4 && strcmp(__argv[1], "--pack") == 0) {
        Document doc;
        std::string why;
        return load_project(&doc, __argv[2], &why) && pack_document(doc, __argv[3], &why) ? 0 : 2;
    }
    if (__argc == 4 && strcmp(__argv[1], "--smoke-pack") == 0) {
        Document doc;
        doc.obj_path = __argv[2];
        Entity spawn;
        spawn.kind = EntityKind::SpawnPoint;
        spawn.name = "Smoke Spawn";
        spawn.guid = doc.next_guid++;
        spawn.value_u32_a = 5;
        spawn.value_u32_b = 24;
        doc.entities.push_back(spawn);
        Entity light;
        light.kind = EntityKind::Light;
        light.name = "Smoke Light";
        light.guid = doc.next_guid++;
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
        else if (_stricmp(ext.c_str(), ".alev") == 0) {
            std::string why;
            Document doc;
            if (load_project(&doc, path.c_str(), &why)) {
                g.document = std::move(doc);
                const bool skybox_loaded = reload_skybox_preview(true);
                if (!g.document.obj_path.empty())
                    load_preview_mesh(g.document.obj_path, &g.mesh, &why);
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
