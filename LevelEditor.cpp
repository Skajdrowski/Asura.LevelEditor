#include "LevelEditorGeometry.h"
#include "LevelEditorHistory.h"
#include "LevelEditorProject.h"
#include "LevelEditorRaycast.h"

#include <commdlg.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <string>
#include <vector>

using namespace asura;
using namespace asura::level;

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Winmm.lib")

namespace editor {

constexpr float kSpawnCollisionHalfWidth = 0.3f;
constexpr float kSpawnCollisionHeight = 1.8f;
constexpr float kSpawnCollisionVerticalOffset = 0.1f;

bool load_preview_mesh(const std::string& path, const std::string& material_map_path, Mesh* mesh, std::string* why) {
    Error err{};
    Arena arena{};
    MappedFile file{};
    ObjData obj{};
    MaterialMap materials{};
    Config cfg{};
    char editor_arg[] = "LevelEditor";
    char out_arg[] = "preview.pc";
    char* args[] = {editor_arg, const_cast<char*>(path.c_str()), out_arg};
    bool ok = arena_init(&arena, sizeof(void*) == 4 ? 256 * MiB : 2 * GiB, &err) &&
              parse_cli(3, args, &cfg, &err) && map_file(path.c_str(), &file, &err) &&
              parse_obj(&file, &obj, &arena, &err);
    cfg.material_map = material_map_path.empty() ? nullptr : material_map_path.c_str();
    if (ok)
        ok = load_material_map(cfg, &materials, &arena, &err);
    // Game Env vertices are Y/Z-flipped. Keep the authored Blender Y axis
    // upright in the viewport; pack_document still uses the game default.
    cfg.flip_y = false;
    Mesh next;
    if (ok) {
        const size_t maximum_vertices = static_cast<size_t>(obj.face_count) * 3;
        next.positions.reserve(maximum_vertices);
        next.normals.reserve(maximum_vertices);
        next.texcoords.reserve(maximum_vertices);
        next.diffuse_abgr.reserve(maximum_vertices);
        next.faces.reserve(obj.face_count);
        next.face_materials.reserve(obj.face_count);
        for (uint32_t i = 0; i < obj.face_count; ++i) {
            const ObjFace& f = obj.faces[i];
            VertexKey keys[3]{};
            uint32_t material = 0;
            if (!face_keys(obj, f, keys, &err) ||
                !resolve_material(materials, f.material, true, &material, &err)) {
                ok = false;
                break;
            }
            if (transform_reverses_winding(cfg))
                std::swap(keys[1], keys[2]);
            const uint32_t first = static_cast<uint32_t>(next.positions.size());
            for (const VertexKey& key : keys) {
                next.positions.push_back(transform_vec(obj.positions[key.v], cfg));
                Asura_Vector_3 normal{};
                if (key.vn != 0xffffffffu)
                    normal = transform_vec(obj.normals[key.vn], cfg);
                next.normals.push_back(normal);
                Asura_Vector_2 uv{};
                if (key.vt != 0xffffffffu) {
                    uv = obj.texcoords[key.vt];
                    uv.y = 1.0f - uv.y;
                }
                next.texcoords.push_back(uv);
                next.diffuse_abgr.push_back(obj.has_color[key.v] ? obj.colors[key.v] : cfg.diffuse_abgr);
            }
            next.faces.push_back({first, first + 1, first + 2});
            next.face_materials.push_back(material == 0xffffffffu ? 0u : material);
        }
        if (ok)
            finish_mesh_bounds(&next);
    }
    if (!ok && why)
        *why = err.set ? err.message : "Could not load the OBJ preview.";
    unmap_file(&file);
    unmap_file(&materials.file);
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
    next.normals.reserve(static_cast<size_t>(total_vertices));
    next.texcoords.reserve(static_cast<size_t>(total_vertices));
    next.diffuse_abgr.reserve(static_cast<size_t>(total_vertices));
    next.faces.reserve(static_cast<size_t>(total_triangles));
    next.face_materials.reserve(static_cast<size_t>(total_triangles));
    for (uint32_t block_index = 0; block_index < env.block_count; ++block_index) {
        const uint8_t* block = env.blocks[block_index];
        const uint32_t vertex_count = read_u32(block);
        const uint8_t* vertices = block + 8;
        for (uint32_t vertex_index = 0; vertex_index < vertex_count; ++vertex_index) {
            const uint8_t* source = vertices + static_cast<uint64_t>(vertex_index) * sizeof(Asura_PC_EnvironmentRenderer_Vertex);
            const Asura_Vector_3 position{read_f32(source), -read_f32(source + 4), read_f32(source + 8)};
            const Asura_Vector_3 normal{read_f32(source + 12), -read_f32(source + 16), read_f32(source + 20)};
            const Asura_Vector_2 uv{read_f32(source + 28), read_f32(source + 32)};
            if (!isfinite(position.x) || !isfinite(position.y) || !isfinite(position.z) ||
                !isfinite(normal.x) || !isfinite(normal.y) || !isfinite(normal.z) ||
                !isfinite(uv.x) || !isfinite(uv.y))
                return fail(err, "PC environment contains a non-finite vertex");
            next.positions.push_back(position);
            next.normals.push_back(normal);
            next.diffuse_abgr.push_back(read_u32(source + 24));
            next.texcoords.push_back(uv);
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
                next.face_materials.push_back(strip.m_iOriginalMaterialIndex);
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
    case SnipeItem_MedKit: return "MedKit";
    case SnipeItem_Bandage: return "Bandage";
    case SnipeItem_TnT: return "TnT";
    case SnipeItem_Gewehr43: return "Gewehr 43";
    case SnipeItem_Mosin91: return "Mosin 91";
    case SnipeItem_SVT40: return "SVT-40";
    case SnipeItem_PPSH: return "PPSh";
    case SnipeItem_MP40: return "MP 40";
    case SnipeItem_MG42: return "MG42";
    case SnipeItem_DP28: return "DP 28";
    case SnipeItem_TimeBomb: return "Time Bomb";
    case SnipeItem_Panzerschreck: return "Panzerschreck";
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
    {SnipeItem_MedKit, "smallmedkit"},
    {SnipeItem_Bandage, "bandage"},
    {SnipeItem_TnT, "tnt"},
    {SnipeItem_Gewehr43, "springfield"},
    {SnipeItem_Mosin91, "nagan_scope"},
    {SnipeItem_SVT40, "mauser_scope"},
    {SnipeItem_PPSH, "machgun"},
    {SnipeItem_MP40, "mp40"},
    {SnipeItem_MG42, "MG42"},
    {SnipeItem_DP28, "dp28"},
    {SnipeItem_TimeBomb, "tbomb"},
    {SnipeItem_Panzerschreck, "panzerschreck"},
    {SnipeItem_PanzerschreckAmmo, "schreckrocket"}
};

uint32_t pickup_initial_state_bits(uint32_t item_id) {
    switch (item_id) {
    case SnipeItem_Panzerfaust:
    case SnipeItem_StickGrenade:
    case SnipeItem_FragGrenade:
    case SnipeItem_SmokeGrenade:
    case SnipeItem_TnT:
    case SnipeItem_TimeBomb:
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

struct PcSkyboxInfo {
    uint32_t chunk_version = 7;
    float red = 255.0f;
    float green = 255.0f;
    float blue = 255.0f;
    float orientation = 0.0f;
    Str names[ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT]{};
    bool draw_clouds = false;
    bool back_texture_is_front_upside_down = false;
    bool right_texture_is_left_upside_down = false;
};

bool pc_skybox_info(const ChunkList& chunks, PcSkyboxInfo* info, Error* err) {
    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        const ChunkRef& chunk = chunks.chunks[chunk_index];
        if (chunk.cid != ASURA_CHUNK_SKYBOX)
            continue;
        if ((chunk.version != 6 && chunk.version != 7) ||
            chunk.size < sizeof(Asura_Chunk_Header) + sizeof(Asura_Chunk_SkyBox_PayloadPrefixV7) +
                             ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT * 4 +
                             (chunk.version == 7 ? 12u : 8u))
            return fail(err, "the .PC SKYB chunk has an unsupported version or size");
        const uint8_t* payload = chunk.data + sizeof(Asura_Chunk_Header);
        const uint32_t payload_size = chunk.size - sizeof(Asura_Chunk_Header);
        Asura_Chunk_SkyBox_PayloadPrefixV7 prefix{};
        memcpy(&prefix, payload, sizeof(prefix));
        info->chunk_version = chunk.version;
        info->red = prefix.m_fRed;
        info->green = prefix.m_fGreen;
        info->blue = prefix.m_fBlue;
        info->orientation = prefix.m_fOrientationAroundYAxis;
        if (!isfinite(info->red) || !isfinite(info->green) || !isfinite(info->blue) ||
            !isfinite(info->orientation))
            return fail(err, "the .PC SKYB colour or orientation is invalid");
        uint64_t at = sizeof(prefix);
        for (uint32_t slot = 0; slot < ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT; ++slot) {
            if (at > 0xffffffffull)
                return fail(err, "the .PC SKYB texture table is invalid");
            info->names[slot] = padded_string_at(payload, payload_size, static_cast<uint32_t>(at));
            if (!info->names[slot].data)
                return fail(err, "the .PC SKYB texture table is truncated");
            at = align_up(at + info->names[slot].size + 1, 4);
            if (at > payload_size)
                return fail(err, "the .PC SKYB texture table is truncated");
        }
        const uint32_t flag_bytes = chunk.version == 7 ? 12u : 8u;
        if (at + flag_bytes > payload_size)
            return fail(err, "the .PC SKYB flags are truncated");
        Asura_Chunk_SkyBox_TrailingFlagsV7 flags{};
        memcpy(&flags, payload + at, flag_bytes);
        info->draw_clouds = flags.m_bDrawClouds != 0;
        info->back_texture_is_front_upside_down = flags.m_bBackTextureIsFrontUpsideDown != 0;
        info->right_texture_is_left_upside_down = flags.m_bRightTextureIsLeftUpsideDown != 0;
        return true;
    }
    return fail(err, "the .PC contains no SKYB chunk");
}

void import_pc_skybox_settings(const PcSkyboxInfo& info, SkyboxSettings* skybox) {
    skybox->chunk_version = info.chunk_version;
    skybox->red = info.red;
    skybox->green = info.green;
    skybox->blue = info.blue;
    skybox->orientation_radians = info.orientation;
    for (uint32_t slot = 0; slot < ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT; ++slot)
        skybox->texture_paths[slot].assign(info.names[slot].data, info.names[slot].size);
    skybox->draw_clouds = info.draw_clouds;
    skybox->back_texture_is_front_upside_down = info.back_texture_is_front_upside_down;
    skybox->right_texture_is_left_upside_down = info.right_texture_is_left_upside_down;
    skybox->source_record = true;
}

bool source_ambience_info(const ChunkRef& chunk, std::string* path, float* volume,
                          uint32_t* tail_offset, Error* err);

bool load_pc_level(const std::string& path, Document* document, Mesh* mesh, std::string* why,
                   std::vector<PickupModel>* pickup_models = nullptr) {
    Error err{};
    Arena arena{};
    ChunkList chunks{};
    Document next_document;
    Mesh next_mesh;
    std::vector<PickupModel> next_pickup_models;
    bool ok = arena_init(&arena, 64 * MiB, &err) && parse_chunks(path.c_str(), &chunks, &arena, &err);
    bool source_has_skybox = false;
    for (uint32_t chunk_index = 0; ok && chunk_index < chunks.count; ++chunk_index)
        source_has_skybox |= chunks.chunks[chunk_index].cid == ASURA_CHUNK_SKYBOX;
    PcSkyboxInfo skybox_info{};
    if (ok && source_has_skybox)
        ok = pc_skybox_info(chunks, &skybox_info, &err);
    if (ok && source_has_skybox)
        import_pc_skybox_settings(skybox_info, &next_document.skybox);
    for (uint32_t chunk_index = 0; ok && chunk_index < chunks.count; ++chunk_index) {
        const ChunkRef& chunk = chunks.chunks[chunk_index];
        if (chunk.cid == ASURA_CHUNK_WEATHERSYSTEM && chunk.version >= 5 &&
            chunk.version <= 6 && chunk.size > 21) {
            next_document.weather_source_record = true;
            next_document.rain_enabled = chunk.data[21] != 0;
        } else if (chunk.cid == ASURA_CHUNK_STREAMINGBACKGROUNDSOUND &&
                   !next_document.ambient_source_record) {
            std::string stream_path;
            float volume = 1.0f;
            if (!source_ambience_info(chunk, &stream_path, &volume, nullptr, &err)) {
                ok = false;
            } else {
                next_document.ambient_source_record = true;
                next_document.ambient_stream_path = std::move(stream_path);
                next_document.ambient_volume = volume;
            }
        }
    }
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

bool append_pc_material_map_override(Buffer* out, const ChunkList& source,
                                     const char* material_map_path, Arena* arena, Error* err);
bool pc_environment_material_chunk_indices(const ChunkList& source, uint32_t* text_chunk_index,
                                           uint32_t* material_chunk_index, Error* err);

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
    auto write_entities = [&]() {
        if (!wrote_entities) {
            wrote_entities = true;
            ok = ok && append_sound_entities(&output, sounds, &err) && append_editor_spawnpoints(&output, doc, &err) &&
                 append_editor_pickups(&output, doc, &err);
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
             append_sound_resources(&output, sounds, &scratch, &err) &&
             (doc.ambient_stream_path.empty() || append_editor_ambience(&output, doc, &err)) &&
             append_editor_lights(&output, doc, &err) &&
             append_phon(&output, sounds, &err) &&
             append_emod(&output, view, view.module_count, cfg, material_map, &scratch, metrics, &err) &&
             append_mlin(&output, metrics, view.module_count, &err) &&
             append_mrvb(&output, view.module_count, &err) && append_nav1(&output, view.module_count, &err) &&
             append_sound_entities(&output, sounds, &err) && append_editor_spawnpoints(&output, doc, &err) &&
              append_editor_pickups(&output, doc, &err) &&
              append_editor_skybox(&output, doc.skybox, &err) &&
             append_fog(&output, &err) && append_editor_weather(&output, doc, &err) &&
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
    ID_EXPORT_MATERIAL_MAP,
    ID_TEXTURE_DIR,
    ID_WEAPONS_DONOR,
    ID_SKYBOX_TEXTURES,
    ID_TOGGLE_RAIN,
    ID_AMBIENCE_PROPERTIES,
    ID_ENTITY_LIST,
    ID_ADD_SPAWN,
    ID_ADD_LIGHT,
    ID_ADD_SOUND,
    ID_ADD_PICKUP,
    ID_DELETE_ENTITY,
    ID_UNDO,
    ID_REDO,
    ID_COPY_ENTITY,
    ID_PASTE_ENTITY,
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
    ID_PICKUP_ITEM,
    ID_LIGHT_PROPERTIES,
    ID_SOUND_LOOP,
    ID_SOUND_PREVIEW,
    ID_SPAWN_TEAM_FIRST,
    ID_SPAWN_TEAM_LAST = ID_SPAWN_TEAM_FIRST + 3,
    ID_SPAWN_GAME_MODE_FIRST,
    ID_SPAWN_GAME_MODE_LAST = ID_SPAWN_GAME_MODE_FIRST + 5,
    ID_STATUS,
    ID_VIEWPORT,
};

constexpr uint32_t kInspectorDirtyName = 1u << 0;
constexpr uint32_t kInspectorDirtyPosX = 1u << 1;
constexpr uint32_t kInspectorDirtyPosY = 1u << 2;
constexpr uint32_t kInspectorDirtyPosZ = 1u << 3;
constexpr uint32_t kInspectorDirtyRotX = 1u << 4;
constexpr uint32_t kInspectorDirtyRotY = 1u << 5;
constexpr uint32_t kInspectorDirtyRotZ = 1u << 6;
constexpr uint32_t kInspectorDirtyValueA = 1u << 7;
constexpr uint32_t kInspectorDirtyValueB = 1u << 8;
constexpr uint32_t kInspectorDirtyPickup = 1u << 9;
constexpr uint32_t kInspectorDirtySoundLoop = 1u << 10;
constexpr int kInspectorDirtySpawnTeamShift = 11;
constexpr int kInspectorDirtySpawnGameModeShift = 15;

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
    HWND pickup_item = nullptr;
    HWND rain_toggle = nullptr;
    HWND ambience_properties = nullptr;
    HWND sound_browse = nullptr;
    HWND sound_loop = nullptr;
    HWND sound_preview = nullptr;
    HWND spawn_team_label = nullptr;
    HWND spawn_game_mode_label = nullptr;
    HWND spawn_team_checks[4]{};
    HWND spawn_game_mode_checks[6]{};
    HWND light_properties = nullptr;
    HWND status = nullptr;
    HWND viewport = nullptr;
    HFONT font = nullptr;
    Document document;
    LevelEditorHistory history;
    Mesh mesh;
    EnvironmentRaycast environment_raycast;
    std::array<SpawnPuppet, 3> spawn_puppets;
    std::string spawn_puppet_source;
    std::vector<PickupModel> pickup_models;
    std::vector<uint8_t> sound_preview_bytes;
    int sound_preview_entity = -1;
    Camera camera;
    std::vector<int> selected_entities;
    int selected = -1;
    int pending_kind = -1;
    bool orbiting = false;
    bool panning = false;
    bool moving_entity = false;
    bool refreshing_inspector = false;
    uint32_t inspector_dirty = 0;
    bool fast_preview = false;
    HBITMAP environment_cache = nullptr;
    int environment_cache_width = 0;
    int environment_cache_height = 0;
    bool environment_cache_valid = false;
    bool environment_cache_fast = false;
    POINT last_mouse{};
    POINT entity_drag_last_mouse{};
};

AppState g;

bool valid_entity_index(int index) {
    return index >= 0 && index < static_cast<int>(g.document.entities.size());
}

bool entity_is_selected(int index) {
    return std::find(g.selected_entities.begin(), g.selected_entities.end(), index) !=
           g.selected_entities.end();
}

void set_single_selection_state(int index) {
    g.selected_entities.clear();
    g.selected = valid_entity_index(index) ? index : -1;
    if (g.selected >= 0)
        g.selected_entities.push_back(g.selected);
}

void normalize_selection_state() {
    std::vector<int> normalized;
    normalized.reserve(g.selected_entities.size());
    for (int index : g.selected_entities) {
        if (valid_entity_index(index) &&
            std::find(normalized.begin(), normalized.end(), index) == normalized.end())
            normalized.push_back(index);
    }
    g.selected_entities = std::move(normalized);
    if (!entity_is_selected(g.selected))
        g.selected = g.selected_entities.empty() ? -1 : g.selected_entities.back();
}

void record_inspector_edit(int id, int notification) {
    if (g.refreshing_inspector)
        return;
    if (notification == EN_CHANGE) {
        switch (id) {
        case ID_NAME: g.inspector_dirty |= kInspectorDirtyName; break;
        case ID_POS_X: g.inspector_dirty |= kInspectorDirtyPosX; break;
        case ID_POS_Y: g.inspector_dirty |= kInspectorDirtyPosY; break;
        case ID_POS_Z: g.inspector_dirty |= kInspectorDirtyPosZ; break;
        case ID_ROT_X: g.inspector_dirty |= kInspectorDirtyRotX; break;
        case ID_ROT_Y: g.inspector_dirty |= kInspectorDirtyRotY; break;
        case ID_ROT_Z: g.inspector_dirty |= kInspectorDirtyRotZ; break;
        case ID_VALUE_A: g.inspector_dirty |= kInspectorDirtyValueA; break;
        case ID_VALUE_B: g.inspector_dirty |= kInspectorDirtyValueB; break;
        }
    } else if (id == ID_PICKUP_ITEM && notification == CBN_SELCHANGE) {
        g.inspector_dirty |= kInspectorDirtyPickup;
    } else if (id == ID_SOUND_LOOP && notification == BN_CLICKED) {
        g.inspector_dirty |= kInspectorDirtySoundLoop;
    } else if (id >= ID_SPAWN_TEAM_FIRST && id <= ID_SPAWN_TEAM_LAST && notification == BN_CLICKED) {
        g.inspector_dirty |= 1u << (kInspectorDirtySpawnTeamShift + id - ID_SPAWN_TEAM_FIRST);
    } else if (id >= ID_SPAWN_GAME_MODE_FIRST && id <= ID_SPAWN_GAME_MODE_LAST &&
               notification == BN_CLICKED) {
        g.inspector_dirty |= 1u << (kInspectorDirtySpawnGameModeShift + id - ID_SPAWN_GAME_MODE_FIRST);
    }
}

void stop_sound_preview() {
    if (g.sound_preview_entity >= 0)
        PlaySoundA(nullptr, nullptr, 0);
    g.sound_preview_entity = -1;
    g.sound_preview_bytes.clear();
    if (g.sound_preview)
        SetWindowTextA(g.sound_preview, "Play preview");
}

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

struct LightGizmoLine {
    Asura_Vector_3 a{};
    Asura_Vector_3 b{};
};

constexpr int kLightRangeSegments = 48;
constexpr size_t kLightBoundingBoxLines = 12;
constexpr size_t kCameraSpawnArrowLines = 5;
constexpr size_t kMaximumLightGizmoLines = kLightRangeSegments * 3 + kLightBoundingBoxLines;
constexpr float kMaximumLightGizmoRange = 10000000.0f;

void append_light_gizmo_line(std::vector<LightGizmoLine>* lines, Asura_Vector_3 a, Asura_Vector_3 b) {
    lines->push_back({a, b});
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
            append_light_gizmo_line(lines, add(center, p0), add(center, p1));
        }
    }
}

void append_light_bounding_box(const Asura_Bounding_Box& bounds, std::vector<LightGizmoLine>* lines) {
    const float values[] = {bounds.MinX, bounds.MaxX, bounds.MinY,
                            bounds.MaxY, bounds.MinZ, bounds.MaxZ};
    for (float value : values)
        if (!isfinite(value))
            return;

    Asura_Vector_3 corners[8]{};
    for (int corner = 0; corner < 8; ++corner) {
        corners[corner] = entity_view_position({corner & 1 ? bounds.MaxX : bounds.MinX,
                                                corner & 2 ? bounds.MaxY : bounds.MinY,
                                                corner & 4 ? bounds.MaxZ : bounds.MinZ});
    }
    for (int corner = 0; corner < 8; ++corner) {
        for (int axis = 0; axis < 3; ++axis) {
            const int other = corner ^ (1 << axis);
            if (corner < other)
                append_light_gizmo_line(lines, corners[corner], corners[other]);
        }
    }
}

void append_camera_spawn_arrow(const Entity& entity, float marker, std::vector<LightGizmoLine>* lines) {
    if (entity.kind != EntityKind::SpawnPoint || !(entity.value_u32_a & SnipeSpawnTeam_Camera))
        return;
    Asura_Vector_3 direction = normalized(entity_view_direction(entity.spawn_direction));
    if (dot(direction, direction) <= .00001f)
        return;
    const Asura_Vector_3 start = entity_view_position(entity.position);
    const float length = fmaxf(1.0f, marker * 4.0f);
    const float head_length = length * .28f;
    const float head_width = length * .18f;
    const Asura_Vector_3 tip = add(start, mul(direction, length));
    const Asura_Vector_3 head_center = sub(tip, mul(direction, head_length));
    Asura_Vector_3 side = normalized(cross(direction, {0, 1, 0}));
    if (dot(side, side) <= .00001f)
        side = {1, 0, 0};
    const Asura_Vector_3 vertical = normalized(cross(direction, side));
    append_light_gizmo_line(lines, start, tip);
    append_light_gizmo_line(lines, tip, add(head_center, mul(side, head_width)));
    append_light_gizmo_line(lines, tip, sub(head_center, mul(side, head_width)));
    append_light_gizmo_line(lines, tip, add(head_center, mul(vertical, head_width)));
    append_light_gizmo_line(lines, tip, sub(head_center, mul(vertical, head_width)));
}

void append_sound_gizmo(const Entity& entity, std::vector<LightGizmoLine>* lines) {
    if (entity.kind != EntityKind::Sound)
        return;
    const float range = fabsf(entity.value_b);
    if (isfinite(range) && range > .00001f && range <= kMaximumLightGizmoRange)
        append_range_globe(entity_view_position(entity.position), range, lines);
}

void append_light_gizmo(const Entity& entity, std::vector<LightGizmoLine>* lines) {
    if (entity.kind != EntityKind::Light)
        return;
    const Asura_Vector_3 center = entity_view_position(entity.position);
    const float range = fabsf(entity.light.Range);
    if (isfinite(range) && range > .00001f && range <= kMaximumLightGizmoRange)
        append_range_globe(center, range, lines);
    if (entity.light.m_uFlags & ASURA_LIGHT_FLAG_USE_BOUNDING_BOX)
        append_light_bounding_box(entity.light.m_xBoundingBox, lines);
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

bool screen_ray(int x, int y, EnvironmentRay* ray) {
    if (!ray)
        return false;
    const RECT vr = viewport_rect();
    Asura_Vector_3 cam, right, up, forward;
    camera_axes(&cam, &right, &up, &forward);
    const float f = .85f * static_cast<float>(std::min(vr.right - vr.left, vr.bottom - vr.top));
    if (f <= 0.0f)
        return false;
    const float sx = (x - (vr.left + vr.right) * .5f) / f;
    const float sy = -(y - (vr.top + vr.bottom) * .5f) / f;
    ray->origin = cam;
    ray->direction = normalized(add(forward, add(mul(right, sx), mul(up, sy))));
    return dot(ray->direction, ray->direction) > .5f;
}

bool environment_point_from_screen(int x, int y, Asura_Vector_3* game_point) {
    EnvironmentRay ray{};
    EnvironmentRayHit hit{};
    if (!game_point || !screen_ray(x, y, &ray) || !g.environment_raycast.intersect(g.mesh, ray, &hit))
        return false;
    // Mesh vertices are in editor/view coordinates; gameplay entities retain
    // the target's negative-up Y convention.
    *game_point = entity_view_position(hit.position);
    return true;
}

bool environment_occludes_view_position(const Asura_Vector_3& view_position) {
    Asura_Vector_3 camera_position{}, right{}, up{}, forward{};
    camera_axes(&camera_position, &right, &up, &forward);
    const Asura_Vector_3 to_position = sub(view_position, camera_position);
    const float distance_squared = dot(to_position, to_position);
    if (distance_squared <= 1.0e-8f)
        return false;
    const float distance = sqrtf(distance_squared);
    EnvironmentRayHit hit{};
    const EnvironmentRay ray{camera_position, mul(to_position, 1.0f / distance)};
    if (!g.environment_raycast.intersect(g.mesh, ray, &hit))
        return false;
    const float surface_epsilon = fmaxf(.01f, distance * 1.0e-4f);
    return hit.distance + surface_epsilon < distance;
}

struct GpuVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT4 color;
    DirectX::XMFLOAT2 uv;
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

struct EnvironmentMaterialConstants {
    DirectX::XMFLOAT4 fallback_color;
    float has_texture;
    float has_material_color;
    float render_mode;
    float auxiliary_mode;
};

static_assert(sizeof(EnvironmentMaterialConstants) == 0x20,
              "environment material constants must preserve HLSL register packing");

EnvironmentMaterialConstants environment_material_constants(
    DirectX::XMFLOAT4 fallback_color, bool has_texture, bool has_material_color,
    float render_mode, float auxiliary_mode, bool alpha_test) {
    (void)alpha_test;
    return {fallback_color,
            has_texture ? 1.0f : 0.0f,
            has_material_color ? 1.0f : 0.0f,
            render_mode,
            auxiliary_mode};
}

struct EnvironmentViewConstants {
    DirectX::XMFLOAT4 camera_position;
    DirectX::XMFLOAT4 camera_right;
    DirectX::XMFLOAT4 camera_up;
    DirectX::XMFLOAT4 camera_forward;
};

static_assert(sizeof(EnvironmentViewConstants) == 0x40,
              "environment view constants must preserve HLSL register packing");

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

struct GpuMaterialRange {
    int32_t original_material_index = -1;
    uint32_t start_index = 0;
    uint32_t index_count = 0;
    ID3D11ShaderResourceView* texture = nullptr;
    uint32_t material_flags = 0;
    uint32_t texture_flags = 0;
    DirectX::XMFLOAT4 fallback_color{.32f, .39f, .43f, 1.0f};
    bool has_material_color = false;
    bool source_pc_material = false;
};

bool gpu_material_uses_alpha(const GpuMaterialRange& range) {
    return range.texture && (range.material_flags & 0x2u) != 0;
}

bool gpu_material_is_solid_cutout(const GpuMaterialRange& range) {
    // MCP2 ObjectHierarchy rendering uses TXFL bit 0x8 to distinguish a
    // background-blended material from an opaque alpha-tested cutout. Keep
    // target Env's genuinely composited passes out of this preview-only path:
    // wet roads, additive surfaces, and sphere-map surfaces still need their
    // original equations. Flag-0x4 solid fences stay cutouts; their target
    // detail multipass is precisely what makes them blow out in this viewport.
    constexpr uint32_t composited_material_flags = 0x1u | 0x80u | 0x4000u;
    return range.source_pc_material && gpu_material_uses_alpha(range) &&
           (range.texture_flags & 0x8u) == 0 &&
           (range.material_flags & composited_material_flags) == 0;
}

constexpr float kEnvironmentRenderModeAlphaPrelight = 2.0f;
constexpr float kEnvironmentRenderModeSolidCutout = 5.0f;

struct GpuRenderer {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* render_target = nullptr;
    ID3D11Texture2D* depth_texture = nullptr;
    ID3D11DepthStencilView* depth_view = nullptr;
    ID3D11VertexShader* vertex_shader = nullptr;
    ID3D11PixelShader* pixel_shader = nullptr;
    ID3D11PixelShader* environment_pixel_shader = nullptr;
    ID3D11PixelShader* rain_pixel_shader = nullptr;
    ID3D11InputLayout* input_layout = nullptr;
    ID3D11VertexShader* skybox_vertex_shader = nullptr;
    ID3D11PixelShader* skybox_pixel_shader = nullptr;
    ID3D11PixelShader* skybox_cloud_pixel_shader = nullptr;
    ID3D11InputLayout* skybox_input_layout = nullptr;
    ID3D11Buffer* camera_buffer = nullptr;
    ID3D11Buffer* skybox_animation_buffer = nullptr;
    ID3D11Buffer* environment_material_buffer = nullptr;
    ID3D11Buffer* environment_view_buffer = nullptr;
    ID3D11Buffer* skybox_vertices = nullptr;
    ID3D11Buffer* skybox_cloud_vertices = nullptr;
    ID3D11Buffer* mesh_vertices = nullptr;
    ID3D11Buffer* mesh_indices = nullptr;
    ID3D11Buffer* puppet_vertices = nullptr;
    ID3D11Buffer* overlay_vertices = nullptr;
    ID3D11Buffer* rain_vertices = nullptr;
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11DepthStencilState* depth_enabled = nullptr;
    ID3D11DepthStencilState* depth_disabled = nullptr;
    ID3D11DepthStencilState* depth_equal = nullptr;
    ID3D11DepthStencilState* skybox_depth = nullptr;
    ID3D11BlendState* alpha_blend = nullptr;
    ID3D11BlendState* modulate2x_blend = nullptr;
    ID3D11BlendState* additive_blend = nullptr;
    ID3D11BlendState* reflection_blend = nullptr;
    ID3D11SamplerState* skybox_sampler = nullptr;
    ID3D11SamplerState* skybox_cloud_sampler = nullptr;
    ID3D11SamplerState* environment_sampler = nullptr;
    ID3D11ShaderResourceView* skybox_faces[6]{};
    ID3D11ShaderResourceView* skybox_cloud = nullptr;
    ID3D11ShaderResourceView* white_texture = nullptr;
    ID3D11ShaderResourceView* environment_splash = nullptr;
    ID3D11ShaderResourceView* environment_detail = nullptr;
    ID3D11ShaderResourceView* environment_spheremap = nullptr;
    ID3D11ShaderResourceView* rain_texture = nullptr;
    uint32_t mesh_index_count = 0;
    uint32_t skybox_cloud_vertex_count = 0;
    uint32_t puppet_capacity = 0;
    uint32_t overlay_capacity = 0;
    uint32_t rain_capacity = 0;
    uint32_t rain_vertex_count = 0;
    std::vector<GpuMaterialRange> material_ranges;
    uint32_t width = 0, height = 0;
    DirectX::XMFLOAT4 skybox_tint{1, 1, 1, 1};
    bool environment_wet_weather = false;
    bool alpha_tested_prelight_drawn = false;
    bool solid_cutout_prelight_drawn = false;
    bool composited_alpha_prelight_drawn = false;
    bool wet_splash_drawn = false;
    bool rain_frame_drawn = false;
    bool skybox_active = false;
    bool ready = false;
};

GpuRenderer gpu;

void refresh_scene_animation_timer() {
    if (!g.window)
        return;
    const bool animated_clouds = g.document.skybox.draw_clouds && gpu.skybox_cloud;
    const bool animated_rain = g.document.rain_enabled && gpu.rain_texture && gpu.rain_pixel_shader;
    if (animated_clouds || animated_rain)
        SetTimer(g.window, 2, 33, nullptr);
    else
        KillTimer(g.window, 2);
}

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

void gpu_release_environment_textures() {
    for (GpuMaterialRange& range : gpu.material_ranges)
        gpu_release(range.texture);
    gpu_release(gpu.environment_splash);
    gpu_release(gpu.environment_detail);
    gpu_release(gpu.environment_spheremap);
    gpu_release(gpu.rain_texture);
    gpu.environment_wet_weather = false;
    gpu.rain_vertex_count = 0;
    gpu.rain_frame_drawn = false;
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
    const char* source = label ? label : "embedded .PC texture";
    if (!bytes || byte_count < 4 + sizeof(DdsHeader) || byte_count > 512 * MiB) {
        if (why)
            *why = std::string("Texture is not a valid DDS file: ") + source;
        return false;
    }
    if (memcmp(bytes, "DDS ", 4) != 0) {
        if (why)
            *why = std::string("Texture does not contain DDS data: ") + source;
        return false;
    }
    DdsHeader header{};
    memcpy(&header, bytes + 4, sizeof(header));
    if (header.size != sizeof(DdsHeader) || header.pixel_format.size != sizeof(DdsPixelFormat) || !header.width ||
        !header.height || header.width > 16384 || header.height > 16384) {
        if (why)
            *why = std::string("Texture has an unsupported DDS header: ") + source;
        return false;
    }

    constexpr uint32_t dds_four_cc = 0x4;
    constexpr uint32_t dds_rgb = 0x40;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    size_t data_at = 4 + sizeof(DdsHeader);
    if ((header.pixel_format.flags & dds_four_cc) && header.pixel_format.four_cc == fourcc('D', 'X', '1', '0')) {
        if (byte_count < data_at + sizeof(DdsHeaderDx10)) {
            if (why)
                *why = std::string("DDS DX10 header is truncated: ") + source;
            return false;
        }
        DdsHeaderDx10 dx10{};
        memcpy(&dx10, bytes + data_at, sizeof(dx10));
        data_at += sizeof(dx10);
        if (dx10.resource_dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D || dx10.array_size != 1 ||
            (dx10.misc_flag & D3D11_RESOURCE_MISC_TEXTURECUBE)) {
            if (why)
                *why = std::string("DDS must contain one 2D texture: ") + source;
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
            *why = std::string("DDS pixel format is unsupported: ") + source;
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
                *why = std::string("DDS mip data is truncated: ") + source;
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
            *why = std::string("Direct3D could not create the texture: ") + source;
        return false;
    }
    return true;
}

bool gpu_create_dds_view(const char* path, ID3D11ShaderResourceView** output, std::string* why) {
    *output = nullptr;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        if (why)
            *why = std::string("Could not open DDS texture: ") + path;
        return false;
    }
    const std::streamoff end = file.tellg();
    if (end < 0 || end > static_cast<std::streamoff>(512 * MiB)) {
        if (why)
            *why = std::string("Texture is not a valid DDS file: ") + path;
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        if (why)
            *why = std::string("Could not read DDS texture: ") + path;
        return false;
    }
    return gpu_create_dds_view_from_memory(bytes.data(), bytes.size(), path, output, why);
}

struct PcEnvironmentMaterialBinding {
    Str texture_name{};
    int32_t texture_index = -1;
    uint32_t flags = 0;
    uint32_t texture_flags = 0;
    uint32_t surface_type = 0;
};

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

bool pc_texture_resource(const ChunkList& chunks, Str texture_name, RscfInfo* output);

std::string parent_folder_of(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

bool gpu_load_global_texture(const char* relative_path, const std::string& level_path,
                             ID3D11ShaderResourceView** output, std::string* why) {
    if (*output)
        return true;
    char module_path[MAX_PATH * 4]{};
    GetModuleFileNameA(nullptr, module_path, static_cast<DWORD>(sizeof(module_path)));
    const std::string module_folder = parent_folder_of(module_path);
    const std::string editor_root = parent_folder_of(parent_folder_of(module_folder));
    const std::string level_folder = parent_folder_of(level_path);
    const std::string level_parent = parent_folder_of(level_folder);
    const std::string roots[] = {"", module_folder, editor_root, level_folder, level_parent};
    std::string last_error;
    for (const std::string& root : roots) {
        const std::string candidate = root.empty() ? relative_path : root + "\\" + relative_path;
        if (!file_exists(candidate.c_str()))
            continue;
        if (gpu_create_dds_view(candidate.c_str(), output, &last_error))
            return true;
    }
    if (why)
        *why = last_error.empty() ? std::string("Required target texture was not found: ") + relative_path
                                  : last_error;
    return false;
}

bool gpu_load_rain_sprite(const ChunkList* chunks, const std::string& level_path, std::string* why) {
    const auto parent_folder = [](const std::string& path) {
        const size_t slash = path.find_last_of("\\/");
        return slash == std::string::npos ? std::string{} : path.substr(0, slash);
    };
    const size_t slash = level_path.find_last_of("\\/");
    const std::string basename =
        slash == std::string::npos ? level_path : level_path.substr(slash + 1);
    std::vector<std::string> stems;
    const auto append_stem = [&stems](std::string stem) {
        if (!stem.empty() && std::find(stems.begin(), stems.end(), stem) == stems.end())
            stems.push_back(std::move(stem));
    };
    for (size_t at = 0; at + 1 < basename.size(); ++at) {
        const char first = basename[at], second = basename[at + 1];
        if (first < '0' || first > '9' || second < '0' || second > '9')
            continue;
        std::string prefix = "rn_p" + basename.substr(at, 2);
        if (at + 2 < basename.size()) {
            char suffix = basename[at + 2];
            if (suffix >= 'A' && suffix <= 'Z')
                suffix += 'a' - 'A';
            if (suffix >= 'a' && suffix <= 'z')
                append_stem(prefix + suffix);
        }
        append_stem(prefix + 'a');
        break;
    }
    append_stem("rn_p01a");
    append_stem("droplet1");

    char module_path[MAX_PATH * 4]{};
    GetModuleFileNameA(nullptr, module_path, static_cast<DWORD>(sizeof(module_path)));
    const std::string module_folder = parent_folder(module_path);
    const std::string editor_root = parent_folder(parent_folder(module_folder));
    const std::string level_folder = parent_folder(level_path);
    const std::string level_parent = parent_folder(level_folder);
    const std::string roots[] = {"", module_folder, editor_root, level_folder, level_parent};
    std::string last_error;
    for (const std::string& stem : stems) {
        if (chunks) {
            for (const char* extension : {".tga", ".dds"}) {
                const std::string resource_name = "\\specialfx\\" + stem + extension;
                RscfInfo resource{};
                if (!pc_texture_resource(*chunks, str_from_c(resource_name.c_str()), &resource))
                    continue;
                if (gpu_create_dds_view_from_memory(resource.payload, resource.payload_size,
                                                    resource_name.c_str(), &gpu.rain_texture,
                                                    &last_error))
                    return true;
            }
        }
        for (const std::string& root : roots) {
            const std::string candidate =
                root.empty() ? "SpecialFX\\" + stem + ".dds"
                             : root + "\\SpecialFX\\" + stem + ".dds";
            if (file_exists(candidate.c_str()) &&
                gpu_create_dds_view(candidate.c_str(), &gpu.rain_texture, &last_error))
                return true;
        }
    }
    if (why && why->empty())
        *why = last_error.empty() ? "Rain is enabled, but no SpecialFX\\rn_p*.dds sprite was found."
                                  : last_error;
    return false;
}

bool gpu_load_pc_environment_textures(const std::string& pc_path, uint32_t* loaded_count,
                                      uint32_t* missing_count, std::string* why) {
    Error err{};
    Arena arena{};
    ChunkList chunks{};
    std::vector<PcEnvironmentMaterialBinding> materials;
    bool ok = arena_init(&arena, 8 * MiB, &err) && parse_chunks(pc_path.c_str(), &chunks, &arena, &err) &&
              pc_environment_material_bindings(chunks, &materials, &err);
    uint32_t loaded = 0, missing = 0;
    std::string last_texture_error;
    if (ok) {
        // MCP2 0x442077 takes rain/wet weather from WTHR +0x15. Import
        // resolves the last supported chunk; the editable document may now
        // intentionally override its original value before export.
        gpu.environment_wet_weather = g.document.rain_enabled;
        if (gpu.environment_wet_weather) {
            gpu_load_rain_sprite(&chunks, pc_path, &last_texture_error);
            RscfInfo splash{};
            if (pc_texture_resource(chunks, str_lit("\\specialfx\\splash.bmp"), &splash) ||
                pc_texture_resource(chunks, str_lit("specialfx\\splash.bmp"), &splash)) {
                std::string texture_error;
                if (!gpu_create_dds_view_from_memory(splash.payload, splash.payload_size,
                                                     "\\specialfx\\splash.bmp", &gpu.environment_splash,
                                                     &texture_error))
                    last_texture_error = std::move(texture_error);
            }
            // splash.bmp is a globally registered game-root asset and need not
            // be packed into an individual level. The copied SpecialFX asset is
            // DDS data, just like target type-2 texture resources, despite the
            // original engine path retaining its .bmp extension.
            if (!gpu.environment_splash) {
                const auto parent_folder = [](const std::string& path) {
                    const size_t slash = path.find_last_of("\\/");
                    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
                };
                char module_path[MAX_PATH * 4]{};
                GetModuleFileNameA(nullptr, module_path, static_cast<DWORD>(sizeof(module_path)));
                const std::string module_folder = parent_folder(module_path);
                const std::string editor_root = parent_folder(parent_folder(module_folder));
                const std::string pc_folder = parent_folder(pc_path);
                const std::string pc_parent = parent_folder(pc_folder);
                const std::string candidates[] = {
                    "SpecialFX\\splash.dds",
                    "SpecialFX\\splash.bmp",
                    module_folder + "\\SpecialFX\\splash.dds",
                    editor_root + "\\SpecialFX\\splash.dds",
                    pc_folder + "\\SpecialFX\\splash.dds",
                    pc_parent + "\\SpecialFX\\splash.dds",
                };
                for (const std::string& candidate : candidates) {
                    if (candidate.empty() || !file_exists(candidate.c_str()))
                        continue;
                    std::string texture_error;
                    if (gpu_create_dds_view(candidate.c_str(), &gpu.environment_splash, &texture_error)) {
                        last_texture_error.clear();
                        break;
                    }
                    last_texture_error = std::move(texture_error);
                }
            }
            if (!gpu.environment_splash && last_texture_error.empty())
                last_texture_error = "Wet WTHR is enabled, but SpecialFX\\splash.dds was not found.";
        }
        for (GpuMaterialRange& range : gpu.material_ranges) {
            if (range.original_material_index < 0 ||
                static_cast<uint32_t>(range.original_material_index) >= materials.size()) {
                ++missing;
                continue;
            }
            const PcEnvironmentMaterialBinding& material = materials[range.original_material_index];
            range.source_pc_material = true;
            range.material_flags = material.flags;
            range.texture_flags = material.texture_flags;
            if (!material.texture_name.size) {
                ++missing;
                continue;
            }
            RscfInfo resource{};
            if (!pc_texture_resource(chunks, material.texture_name, &resource)) {
                ++missing;
                continue;
            }
            const std::string label(material.texture_name.data, material.texture_name.size);
            std::string texture_error;
            if (gpu_create_dds_view_from_memory(resource.payload, resource.payload_size, label.c_str(),
                                                &range.texture, &texture_error)) {
                ++loaded;
            } else {
                ++missing;
                last_texture_error = std::move(texture_error);
            }
        }
        const bool needs_detail = std::any_of(
            gpu.material_ranges.begin(), gpu.material_ranges.end(),
            [](const GpuMaterialRange& range) { return range.texture && (range.material_flags & 0x4u) != 0; });
        const bool needs_spheremap = std::any_of(
            gpu.material_ranges.begin(), gpu.material_ranges.end(),
            [](const GpuMaterialRange& range) { return range.texture && (range.material_flags & 0x80u) != 0; });
        std::string global_error;
        if (needs_detail &&
            !gpu_load_global_texture("GraphicNovel\\detail.dds", pc_path,
                                     &gpu.environment_detail, &global_error))
            last_texture_error = global_error;
        global_error.clear();
        if (needs_spheremap &&
            !gpu_load_global_texture("SpecialFX\\spheremap1.dds", pc_path,
                                     &gpu.environment_spheremap, &global_error))
            last_texture_error = global_error;
    }
    if (loaded_count)
        *loaded_count = loaded;
    if (missing_count)
        *missing_count = missing;
    if (!ok && why)
        *why = err.set ? err.message : "Could not resolve embedded PC environment materials.";
    else if (!last_texture_error.empty() && why)
        *why = last_texture_error;
    unmap_file(&chunks.file);
    arena_release(&arena);
    return ok;
}

void gpu_apply_material_map_colors(const MaterialMap& materials) {
    for (GpuMaterialRange& range : gpu.material_ranges) {
        if (range.original_material_index < 0)
            continue;
        const uint32_t original = static_cast<uint32_t>(range.original_material_index);
        const uint32_t ordinal = original >= 1000 ? original - 1000 : original;
        constexpr uint32_t absent = 0xffffffffu;
        const uint32_t surface_type =
            material_override(materials, "surface_type_by_material_index", ordinal, absent);
        range.material_flags =
            material_override(materials, "transparency_flag_by_material_index", ordinal, 0);
        // External maps have no target TXFL table. Their explicit flag-2
        // entries still enter the common target Env alpha composition.
        range.texture_flags = 0;
        // Rich maps colour equal surface types alike. A simple name/index map
        // still receives one stable colour per mapped material.
        const uint32_t color_key = surface_type == absent ? ordinal ^ 0xa511e9b3u : surface_type;
        range.fallback_color = material_map_color(color_key);
        range.has_material_color = true;
    }
}

bool gpu_reload_environment_textures(std::string* why = nullptr, uint32_t* loaded_count = nullptr,
                                     uint32_t* missing_count = nullptr) {
    gpu_release_environment_textures();
    if (loaded_count)
        *loaded_count = 0;
    if (missing_count)
        *missing_count = 0;
    if (!gpu.ready || gpu.material_ranges.empty()) {
        refresh_scene_animation_timer();
        return true;
    }
    gpu.environment_wet_weather = g.document.rain_enabled;
    for (GpuMaterialRange& range : gpu.material_ranges) {
        range.material_flags = 0;
        range.texture_flags = 0;
        range.fallback_color = {.32f, .39f, .43f, 1.0f};
        range.has_material_color = false;
        range.source_pc_material = false;
    }

    const bool use_external_textures =
        !g.document.material_map.empty() && !g.document.texture_dir.empty();
    
    bool pc_loaded = false;
    if (!use_external_textures && !g.document.source_pc_path.empty()) {
        pc_loaded =
            gpu_load_pc_environment_textures(g.document.source_pc_path, loaded_count, missing_count, why);
        if (g.document.material_map.empty()) {
            refresh_scene_animation_timer();
            return pc_loaded;
        }
    } else if (g.document.rain_enabled) {
        const std::string& level_path = g.document.source_pc_path.empty()
                                            ? g.document.obj_path
                                            : g.document.source_pc_path;
        gpu_load_rain_sprite(nullptr, level_path, why);
    }
    if (g.document.material_map.empty()) {
        if (missing_count)
            *missing_count = static_cast<uint32_t>(gpu.material_ranges.size());
        refresh_scene_animation_timer();
        return true;
    }

    Error err{};
    Arena arena{};
    MaterialMap materials{};
    Config config{};
    config.material_map = g.document.material_map.c_str();
    Vec<DiskFile> files{};
    bool ok = arena_init(&arena, 64 * MiB, &err) && load_material_map(config, &materials, &arena, &err);
    bool texture_files_ready = false;
    std::string last_texture_error;
    if (ok)
        gpu_apply_material_map_colors(materials);
    if (ok && g.document.rain_enabled) {
        const bool needs_splash = std::any_of(
            gpu.material_ranges.begin(), gpu.material_ranges.end(),
            [](const GpuMaterialRange& range) {
                return (range.material_flags & 0x4000u) != 0;
            });
        if (needs_splash) {
            const std::string& level_path = g.document.obj_path.empty()
                                                ? g.document.source_pc_path
                                                : g.document.obj_path;
            std::string splash_error;
            if (!gpu_load_global_texture("SpecialFX\\splash.dds", level_path,
                                         &gpu.environment_splash, &splash_error))
                last_texture_error = std::move(splash_error);
        }
    }
    if (ok && use_external_textures) {
        texture_files_ready = list_files(g.document.texture_dir.c_str(), &arena, &files, &err);
        if (!texture_files_ready)
            last_texture_error = err.set ? err.message : "Could not enumerate the environment texture folder.";
    }
    if (ok && use_external_textures) {
        uint32_t loaded = 0, missing = 0;
        for (GpuMaterialRange& range : gpu.material_ranges) {
            if (!texture_files_ready || range.original_material_index < 0) {
                ++missing;
                continue;
            }
            const uint32_t original = static_cast<uint32_t>(range.original_material_index);
            // Keep parity with append_textures: older editor output sometimes
            // serialized the 1000-based runtime handle in this original-index
            // slot, while the final target uses the zero-based ordinal.
            const uint32_t ordinal = original >= 1000 ? original - 1000 : original;
            const Str wanted = path_basename(material_texture_name(materials, ordinal));
            const DiskFile* selected = nullptr;
            for (uint32_t file_index = 0; file_index < files.count; ++file_index) {
                const Str candidate = str_from_c(files.data[file_index].name);
                if (str_ieq(wanted, candidate) || str_ieq(path_stem(wanted), path_stem(candidate))) {
                    selected = &files.data[file_index];
                    break;
                }
            }
            if (!selected) {
                ++missing;
                continue;
            }
            std::string texture_error;
            if (gpu_create_dds_view(selected->path, &range.texture, &texture_error))
                ++loaded;
            else {
                ++missing;
                last_texture_error = std::move(texture_error);
            }
        }
        if (loaded_count)
            *loaded_count = loaded;
        if (missing_count)
            *missing_count = missing;
    }
    if (!ok && why)
        *why = err.set ? err.message : "Could not resolve environment materials.";
    else if (!last_texture_error.empty() && why)
        *why = last_texture_error;
    unmap_file(&materials.file);
    arena_release(&arena);
    refresh_scene_animation_timer();
    return (!use_external_textures && !g.document.source_pc_path.empty()) ? (ok && pc_loaded) : ok;
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

std::string skybox_texture_stem(const std::string& path) {
    if (path.empty())
        return {};
    const Str source{path.data(), static_cast<uint32_t>(path.size())};
    const Str stem = path_stem(path_basename(source));
    return {stem.data, stem.size};
}

bool gpu_rebuild_skybox_vertices(float orientation, bool back_texture_is_front_upside_down,
                                 bool right_texture_is_left_upside_down, std::string* why);

bool gpu_load_skybox(const std::string& directory, std::string* why) {
    gpu_release_skybox_textures();
    refresh_scene_animation_timer();
    if (directory.empty())
        return true;
    if (!gpu.ready) {
        if (why)
            *why = "The Direct3D viewport is unavailable.";
        return false;
    }
    if (!gpu_rebuild_skybox_vertices(g.document.skybox.orientation_radians,
                                     g.document.skybox.back_texture_is_front_upside_down,
                                     g.document.skybox.right_texture_is_left_upside_down, why))
        return false;
    gpu.skybox_tint = {1, 1, 1, 1};
    // Resolve the document's own path stems first. Newer-permutation SKYBs
    // commonly repeat three files with level-specific names instead of using
    // the constructor's fr/lf/bk/rt/up convention.
    constexpr const char* default_stems[] = {"", "fr", "lf", "bk", "rt", "up"};
    ID3D11ShaderResourceView* next[6]{};
    ID3D11ShaderResourceView* next_cloud = nullptr;
    uint32_t found = 0;
    for (uint32_t slot = 0; slot < _countof(next); ++slot) {
        std::string stem = skybox_texture_stem(g.document.skybox.texture_paths[slot]);
        if (stem.empty())
            stem = default_stems[slot];
        if (stem.empty())
            continue;
        char path[MAX_PATH * 4]{};
        if (!find_skybox_texture(directory, stem.c_str(), path, sizeof(path)))
            continue;
        ++found;
        if (!gpu_create_dds_view(path, &next[slot], why)) {
            for (ID3D11ShaderResourceView*& face : next)
                gpu_release(face);
            return false;
        }
    }
    for (uint32_t slot = 6; slot < ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT && !next_cloud; ++slot) {
        std::string stem = skybox_texture_stem(g.document.skybox.texture_paths[slot]);
        if (stem.empty())
            stem = "ch_04_sky";
        char cloud_path[MAX_PATH * 4]{};
        if (find_skybox_texture(directory, stem.c_str(), cloud_path, sizeof(cloud_path))) {
            ++found;
            if (!gpu_create_dds_view(cloud_path, &next_cloud, why)) {
                for (ID3D11ShaderResourceView*& face : next)
                    gpu_release(face);
                return false;
            }
        }
    }
    if (!found) {
        if (why)
            *why = "The selected folder contains no DDS files matching the SKYB path basenames or "
                   "the default fr/lf/bk/rt/up/ch_04_sky names.";
        return false;
    }
    for (uint32_t i = 0; i < _countof(next); ++i)
        gpu.skybox_faces[i] = next[i];
    gpu.skybox_cloud = next_cloud;
    gpu.skybox_active = true;
    refresh_scene_animation_timer();
    return true;
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
    gpu_release_skybox_textures();
    refresh_scene_animation_timer();
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
        ok = gpu_rebuild_skybox_vertices(info.orientation,
                                         info.back_texture_is_front_upside_down,
                                         info.right_texture_is_left_upside_down, why);

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
        refresh_scene_animation_timer();
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
    gpu_release_environment_textures();
    gpu.material_ranges.clear();
    gpu_release(gpu.white_texture);
    gpu_release(gpu.environment_sampler);
    gpu_release(gpu.skybox_cloud_sampler);
    gpu_release(gpu.skybox_sampler);
    gpu_release(gpu.modulate2x_blend);
    gpu_release(gpu.reflection_blend);
    gpu_release(gpu.additive_blend);
    gpu_release(gpu.alpha_blend);
    gpu_release(gpu.skybox_depth);
    gpu_release(gpu.depth_equal);
    gpu_release(gpu.skybox_cloud_vertices);
    gpu_release(gpu.skybox_vertices);
    gpu_release(gpu.skybox_animation_buffer);
    gpu_release(gpu.environment_view_buffer);
    gpu_release(gpu.environment_material_buffer);
    gpu_release(gpu.skybox_input_layout);
    gpu_release(gpu.skybox_cloud_pixel_shader);
    gpu_release(gpu.skybox_pixel_shader);
    gpu_release(gpu.skybox_vertex_shader);
    gpu_release(gpu.rain_vertices);
    gpu_release(gpu.overlay_vertices);
    gpu_release(gpu.puppet_vertices);
    gpu_release(gpu.mesh_indices);
    gpu_release(gpu.mesh_vertices);
    gpu_release(gpu.depth_disabled);
    gpu_release(gpu.depth_enabled);
    gpu_release(gpu.rasterizer);
    gpu_release(gpu.camera_buffer);
    gpu_release(gpu.input_layout);
    gpu_release(gpu.rain_pixel_shader);
    gpu_release(gpu.environment_pixel_shader);
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

bool gpu_rebuild_skybox_vertices(float orientation, bool back_texture_is_front_upside_down,
                                 bool right_texture_is_left_upside_down, std::string* why) {
    if (!gpu.device || !isfinite(orientation)) {
        if (why)
            *why = "The .PC SKYB orientation is invalid.";
        return false;
    }

    // SniperElite.exe sub_49D000 uses these eight corners and six face
    // quads. Its version-gated compatibility flags permute face vertices
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
        if (right_texture_is_left_upside_down) {
            if (face == 2)
                for (DirectX::XMFLOAT2& item : uv)
                    item.x = 1.0f - item.x;
        } else if (back_texture_is_front_upside_down) {
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
struct VSInput { float3 position : POSITION; float3 normal : NORMAL; float4 color : COLOR; float2 uv : TEXCOORD; };
struct VSOutput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
};
VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.normal = input.normal;
    output.color = input.color;
    output.uv = input.uv;
    output.worldPosition = input.position;
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
Texture2D environmentTexture : register(t2);
Texture2D environmentAuxiliary : register(t3);
Texture2D rainTexture : register(t4);
SamplerState environmentSampler : register(s2);
cbuffer EnvironmentMaterialBuffer : register(b2) {
    float4 materialFallbackColor;
    float materialHasTexture;
    float materialHasColor;
    float materialRenderMode;
    float materialAuxiliaryMode;
};
cbuffer EnvironmentViewBuffer : register(b3) {
    float4 environmentCameraPosition;
    float4 environmentCameraRight;
    float4 environmentCameraUp;
    float4 environmentCameraForward;
};
float4 EnvPSMain(VSOutput input) : SV_TARGET {
    // The target's default gamma option (50) produces an identity hardware
    // ramp. Baked prelight therefore reaches the texture combiner unchanged.
    float3 diffuse = saturate(input.color.rgb);
    float4 albedo = materialHasTexture > 0.5
                        ? environmentTexture.Sample(environmentSampler, input.uv)
                        : float4(1.0, 1.0, 1.0, 1.0);
    if (materialHasTexture < 0.5)
        return materialHasColor > 0.5
                   ? float4(materialFallbackColor.rgb * diffuse, 1.0)
                   : float4(diffuse, 1.0);
    if (materialRenderMode < 0.5)
        return float4(diffuse, 1.0);
    if (materialRenderMode < 1.5) {
        float3 source = albedo.rgb;
        if (materialAuxiliaryMode > 1.5) {
            // Material flag 0x4: the target samples GraphicNovel/detail.dds
            // through a 32x texture matrix and applies stage-1 MODULATE2X.
            float3 detail = environmentAuxiliary.Sample(environmentSampler, input.uv * 32.0).rgb;
            source = saturate(2.0 * source * detail);
        } else if (materialAuxiliaryMode > 0.5) {
            // MCP2 mode 10: stage 1 MODULATE2X with the WTHR-gated splash
            // texture, transformed as uv*2+offset. The existing framebuffer
            // DESTCOLOR/SRCCOLOR blend supplies the second factor of two.
            float3 splash = environmentAuxiliary.Sample(
                environmentSampler, input.uv * 2.0 + materialFallbackColor.xy).rgb;
            source = saturate(2.0 * source * splash);
        }
        return float4(source, 1.0);
    }
    // Mode 2 is target combiner 13: baked diffuse RGB and texture-only alpha.
    // The alpha test rejects holes. Target-composited materials retain texture
    // alpha for SRC_ALPHA/INV_SRC_ALPHA; plain solid cutouts use the same shader
    // with blending disabled, so texture alpha acts only as the coverage mask.
    // Mode 1 supplies texture RGB to the later equal-depth modulation pass.
    if (materialRenderMode < 2.5) {
        if (albedo.a <= 10.0 / 255.0)
            discard;
        return float4(diffuse, albedo.a);
    }
    // Material flag 0x1 replays the base texture with SRC_ALPHA/ONE.
    if (materialRenderMode < 3.5)
        return albedo;

    if (materialRenderMode < 4.5) {
        // Material flag 0x80 uses D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR and
        // the target's (0.5,-0.5)+(0.5,0.5) sphere-map matrix. Combiner 12
        // blends the sphere texture over white by the stage-0 base-texture
        // alpha carried in CURRENT before DESTCOLOR/ZERO framebuffer blending.
        float3 toEye = normalize(environmentCameraPosition.xyz - input.worldPosition);
        float3 reflected = reflect(-toEye, normalize(input.normal));
        float2 sphereUv = float2(dot(reflected, environmentCameraRight.xyz) * 0.5 + 0.5,
                                 dot(reflected, environmentCameraUp.xyz) * -0.5 + 0.5);
        float4 sphere = environmentAuxiliary.Sample(environmentSampler, sphereUv);
        return float4(lerp(float3(1.0, 1.0, 1.0), sphere.rgb, albedo.a), 1.0);
    }

    // Mode 5 is the solid-cutout preview. MCP2's ObjectHierarchy path selects
    // combiner 1 (MODULATE2X texture/diffuse), alpha-tests at 10/255, and keeps
    // blending disabled. The equivalent Environment replay is combiner 8 with
    // DESTCOLOR/SRCCOLOR. Both produce this exact 2*texture*prelight result.
    if (albedo.a <= 10.0 / 255.0)
        discard;
    return float4(saturate(2.0 * albedo.rgb * diffuse), 1.0);
}
float4 RainPSMain(VSOutput input) : SV_TARGET {
    // The original rn_p sprites contain horizontal streaks; the particle
    // emitter rotates them into falling droplets when building its billboards.
    float4 streak = rainTexture.Sample(environmentSampler,
                                       float2(input.uv.y, input.uv.x));
    float alpha = streak.a * input.color.a;
    clip(alpha - 0.012);
    return float4(streak.rgb * input.color.rgb, alpha);
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
    ID3DBlob *vs_blob = nullptr, *ps_blob = nullptr, *environment_ps_blob = nullptr,
             *rain_ps_blob = nullptr, *errors = nullptr;
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
    if (SUCCEEDED(result))
        result = D3DCompile(shader_source, sizeof(shader_source) - 1, nullptr, nullptr, nullptr, "EnvPSMain", "ps_4_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &environment_ps_blob, &errors);
    gpu_release(errors);
    if (SUCCEEDED(result))
        result = D3DCompile(shader_source, sizeof(shader_source) - 1, nullptr, nullptr, nullptr,
                            "RainPSMain", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &rain_ps_blob, &errors);
    gpu_release(errors);
    if (FAILED(result) || FAILED(gpu.device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                                                nullptr, &gpu.vertex_shader)) ||
        FAILED(gpu.device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr,
                                             &gpu.pixel_shader)) ||
        FAILED(gpu.device->CreatePixelShader(environment_ps_blob->GetBufferPointer(),
                                             environment_ps_blob->GetBufferSize(), nullptr,
                                             &gpu.environment_pixel_shader)) ||
        FAILED(gpu.device->CreatePixelShader(rain_ps_blob->GetBufferPointer(),
                                             rain_ps_blob->GetBufferSize(), nullptr,
                                             &gpu.rain_pixel_shader))) {
        gpu_release(vs_blob);
        gpu_release(ps_blob);
        gpu_release(environment_ps_blob);
        gpu_release(rain_ps_blob);
        gpu_shutdown();
        return false;
    }
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GpuVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GpuVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(GpuVertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(GpuVertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    result = gpu.device->CreateInputLayout(elements, _countof(elements), vs_blob->GetBufferPointer(),
                                           vs_blob->GetBufferSize(), &gpu.input_layout);
    gpu_release(vs_blob);
    gpu_release(ps_blob);
    gpu_release(environment_ps_blob);
    gpu_release(rain_ps_blob);
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

    if (!gpu_rebuild_skybox_vertices(g.document.skybox.orientation_radians,
                                     g.document.skybox.back_texture_is_front_upside_down,
                                     g.document.skybox.right_texture_is_left_upside_down, nullptr)) {
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
    if (FAILED(gpu.device->CreateSamplerState(&sampler_desc, &gpu.environment_sampler))) {
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
    constant_desc.ByteWidth = sizeof(EnvironmentMaterialConstants);
    if (FAILED(gpu.device->CreateBuffer(&constant_desc, nullptr, &gpu.environment_material_buffer))) {
        gpu_shutdown();
        return false;
    }
    constant_desc.ByteWidth = sizeof(EnvironmentViewConstants);
    if (FAILED(gpu.device->CreateBuffer(&constant_desc, nullptr, &gpu.environment_view_buffer))) {
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
    depth_desc.DepthFunc = D3D11_COMPARISON_EQUAL;
    if (FAILED(gpu.device->CreateDepthStencilState(&depth_desc, &gpu.depth_equal))) {
        gpu_shutdown();
        return false;
    }
    depth_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
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
    if (FAILED(gpu.device->CreateBlendState(&cloud_blend_desc, &gpu.alpha_blend))) {
        gpu_shutdown();
        return false;
    }
    D3D11_BLEND_DESC modulate_blend_desc{};
    modulate_blend_desc.RenderTarget[0].BlendEnable = TRUE;
    modulate_blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;
    modulate_blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    modulate_blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    modulate_blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    modulate_blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    modulate_blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    modulate_blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(gpu.device->CreateBlendState(&modulate_blend_desc, &gpu.modulate2x_blend))) {
        gpu_shutdown();
        return false;
    }
    D3D11_BLEND_DESC additive_blend_desc = cloud_blend_desc;
    additive_blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    additive_blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    if (FAILED(gpu.device->CreateBlendState(&additive_blend_desc, &gpu.additive_blend))) {
        gpu_shutdown();
        return false;
    }
    D3D11_BLEND_DESC reflection_blend_desc = modulate_blend_desc;
    reflection_blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;
    // Target blend mode 3 (sub_48F7A0): SRC=DESTCOLOR, DEST=ZERO.
    // The sphere-map pass modulates the existing framebuffer; it does not add
    // another copy of it.
    reflection_blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    if (FAILED(gpu.device->CreateBlendState(&reflection_blend_desc, &gpu.reflection_blend))) {
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
    gpu_release_environment_textures();
    gpu.material_ranges.clear();
    gpu_release(gpu.mesh_indices);
    gpu_release(gpu.mesh_vertices);
    gpu.mesh_index_count = 0;
    if (!gpu.ready || g.mesh.positions.empty() || g.mesh.faces.empty())
        return gpu.ready;
    std::vector<Asura_Vector_3> fallback_normals(g.mesh.positions.size());
    std::vector<uint32_t> face_order(g.mesh.faces.size());
    for (uint32_t face_index = 0; face_index < face_order.size(); ++face_index)
        face_order[face_index] = face_index;
    std::stable_sort(face_order.begin(), face_order.end(), [](uint32_t a, uint32_t b) {
        const int32_t material_a = a < g.mesh.face_materials.size() ? g.mesh.face_materials[a] : -1;
        const int32_t material_b = b < g.mesh.face_materials.size() ? g.mesh.face_materials[b] : -1;
        return material_a < material_b;
    });
    std::vector<uint32_t> indices;
    indices.reserve(g.mesh.faces.size() * 3);
    int32_t previous_material = INT32_MIN;
    for (uint32_t face_index : face_order) {
        const auto& face = g.mesh.faces[face_index];
        const Asura_Vector_3 ab = sub(g.mesh.positions[face[1]], g.mesh.positions[face[0]]);
        const Asura_Vector_3 ac = sub(g.mesh.positions[face[2]], g.mesh.positions[face[0]]);
        const Asura_Vector_3 n = cross(ab, ac);
        fallback_normals[face[0]] = add(fallback_normals[face[0]], n);
        fallback_normals[face[1]] = add(fallback_normals[face[1]], n);
        fallback_normals[face[2]] = add(fallback_normals[face[2]], n);
        const int32_t material = face_index < g.mesh.face_materials.size() ? g.mesh.face_materials[face_index] : -1;
        if (gpu.material_ranges.empty() || material != previous_material) {
            gpu.material_ranges.push_back(
                {material, static_cast<uint32_t>(indices.size()), 0, nullptr});
            previous_material = material;
        }
        indices.push_back(face[0]);
        indices.push_back(face[1]);
        indices.push_back(face[2]);
        gpu.material_ranges.back().index_count += 3;
    }
    std::vector<GpuVertex> vertices(g.mesh.positions.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        Asura_Vector_3 n = i < g.mesh.normals.size() ? normalized(g.mesh.normals[i]) : Asura_Vector_3{};
        if (dot(n, n) < .5f)
            n = normalized(fallback_normals[i]);
        const Asura_Vector_2 uv = i < g.mesh.texcoords.size() ? g.mesh.texcoords[i] : Asura_Vector_2{};
        const uint32_t diffuse = i < g.mesh.diffuse_abgr.size() ? g.mesh.diffuse_abgr[i] : 0xff52636du;
        constexpr float inverse_byte = 1.0f / 255.0f;
        const DirectX::XMFLOAT4 color{((diffuse >> 16) & 0xff) * inverse_byte,
                                      ((diffuse >> 8) & 0xff) * inverse_byte,
                                      (diffuse & 0xff) * inverse_byte,
                                      ((diffuse >> 24) & 0xff) * inverse_byte};
        vertices[i] = {{g.mesh.positions[i].x, g.mesh.positions[i].y, g.mesh.positions[i].z}, {n.x, n.y, n.z},
                       color, {uv.x, uv.y}};
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
    // Missing maps/folders/files deliberately leave null SRVs. The render path
    // binds white in that case, exposing the exact prelit vertex diffuse.
    gpu_reload_environment_textures();
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

std::vector<GpuVertex> build_gpu_rain_vertices(const Asura_Vector_3& camera_position,
                                               const Asura_Vector_3& right,
                                               const Asura_Vector_3& up,
                                               const Asura_Vector_3& forward,
                                               float vertical_fov, float aspect,
                                               float animation_seconds) {
    constexpr uint32_t layer_count = 6;
    const float nearest = fmaxf(g.camera.distance * .002f,
                                std::clamp(g.camera.distance * .055f, 1.5f, 12.0f));
    std::vector<GpuVertex> vertices;
    vertices.reserve(layer_count * 6);
    for (int layer = layer_count - 1; layer >= 0; --layer) {
        // sub_41D4D0 doubles each camera-centered emitter layer's distance.
        const float distance = nearest * static_cast<float>(1u << layer);
        const Asura_Vector_3 center = add(camera_position, mul(forward, distance));
        const float half_height = distance * tanf(vertical_fov * .5f) * 1.08f;
        const float half_width = half_height * aspect;
        const Asura_Vector_3 horizontal = mul(right, half_width);
        const Asura_Vector_3 vertical = mul(up, half_height);
        const Asura_Vector_3 top_left = add(sub(center, horizontal), vertical);
        const Asura_Vector_3 top_right = add(add(center, horizontal), vertical);
        const Asura_Vector_3 bottom_right = sub(add(center, horizontal), vertical);
        const Asura_Vector_3 bottom_left = sub(sub(center, horizontal), vertical);
        const float tile = 1.0f + static_cast<float>(layer) * .13f;
        const float u = static_cast<float>(layer) * .173f;
        const float v = static_cast<float>(layer) * .311f -
                        animation_seconds * (.72f + static_cast<float>(layer) * .14f);
        const DirectX::XMFLOAT4 color{.91f, .95f, 1.0f,
                                     .68f - static_cast<float>(layer) * .055f};
        const auto vertex = [color](Asura_Vector_3 position, float x, float y) {
            return GpuVertex{{position.x, position.y, position.z},
                             {0.0f, -1.0f, 0.0f}, color, {x, y}};
        };
        const GpuVertex a = vertex(top_left, u, v);
        const GpuVertex b = vertex(top_right, u + tile, v);
        const GpuVertex c = vertex(bottom_right, u + tile, v + tile);
        const GpuVertex d = vertex(bottom_left, u, v + tile);
        vertices.insert(vertices.end(), {a, b, c, a, c, d});
    }
    return vertices;
}

bool gpu_render_rain(const Asura_Vector_3& camera_position, const Asura_Vector_3& right,
                     const Asura_Vector_3& up, const Asura_Vector_3& forward,
                     float vertical_fov, float aspect, float animation_seconds) {
    gpu.rain_vertex_count = 0;
    gpu.rain_frame_drawn = false;
    if (!g.document.rain_enabled || !gpu.rain_texture || !gpu.rain_pixel_shader)
        return false;
    const std::vector<GpuVertex> vertices =
        build_gpu_rain_vertices(camera_position, right, up, forward,
                                vertical_fov, aspect, animation_seconds);
    if (!gpu_update_dynamic_vertices(&gpu.rain_vertices, &gpu.rain_capacity, vertices) ||
        !gpu.rain_vertices)
        return false;
    const UINT stride = sizeof(GpuVertex), offset = 0;
    gpu.context->IASetVertexBuffers(0, 1, &gpu.rain_vertices, &stride, &offset);
    gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.context->PSSetShader(gpu.rain_pixel_shader, nullptr, 0);
    gpu.context->PSSetShaderResources(4, 1, &gpu.rain_texture);
    gpu.context->PSSetSamplers(2, 1, &gpu.environment_sampler);
    gpu.context->OMSetDepthStencilState(gpu.skybox_depth, 0);
    gpu.context->OMSetBlendState(gpu.alpha_blend, nullptr, 0xffffffffu);
    gpu.rain_vertex_count = static_cast<uint32_t>(vertices.size());
    gpu.context->Draw(gpu.rain_vertex_count, 0);
    ID3D11ShaderResourceView* none = nullptr;
    gpu.context->PSSetShaderResources(4, 1, &none);
    gpu.context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
    gpu.rain_frame_drawn = true;
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
    if (g.document.skybox.draw_clouds && gpu.skybox_cloud && gpu.skybox_cloud_pixel_shader && gpu.skybox_cloud_sampler &&
        gpu.skybox_cloud_vertices && gpu.skybox_cloud_vertex_count) {
        gpu.context->PSSetShader(gpu.skybox_cloud_pixel_shader, nullptr, 0);
        gpu.context->PSSetShaderResources(1, 1, &gpu.skybox_cloud);
        gpu.context->PSSetSamplers(1, 1, &gpu.skybox_cloud_sampler);
        gpu.context->OMSetBlendState(gpu.alpha_blend, nullptr, 0xffffffffu);
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
        color = DirectX::XMFLOAT4{ .816f, .620f, .420f, 0 };
        if (selected)
            color = DirectX::XMFLOAT4{ 1.f, .761f, .518f, 0 };
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
    gpu.alpha_tested_prelight_drawn = false;
    gpu.solid_cutout_prelight_drawn = false;
    gpu.composited_alpha_prelight_drawn = false;
    gpu.wet_splash_drawn = false;
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
    for (int selected_index : g.selected_entities) {
        if (!valid_entity_index(selected_index))
            continue;
        const Entity& entity = g.document.entities[selected_index];
        if (entity.kind != EntityKind::Light && entity.kind != EntityKind::Sound)
            continue;
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
    gpu.context->UpdateSubresource(gpu.camera_buffer, 0, nullptr, &view_projection, 0, 0);
    gpu.context->VSSetConstantBuffers(0, 1, &gpu.camera_buffer);
    const UINT stride = sizeof(GpuVertex), offset = 0;
    if (gpu.mesh_vertices && gpu.mesh_indices && gpu.mesh_index_count) {
        gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
        gpu.context->IASetVertexBuffers(0, 1, &gpu.mesh_vertices, &stride, &offset);
        gpu.context->IASetIndexBuffer(gpu.mesh_indices, DXGI_FORMAT_R32_UINT, 0);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu.context->PSSetShader(gpu.environment_pixel_shader, nullptr, 0);
        gpu.context->PSSetSamplers(2, 1, &gpu.environment_sampler);
        gpu.context->PSSetConstantBuffers(2, 1, &gpu.environment_material_buffer);
        const EnvironmentViewConstants environment_view{
            {camera_position.x, camera_position.y, camera_position.z, 1.0f},
            {right.x, right.y, right.z, 0.0f},
            {up.x, up.y, up.z, 0.0f},
            {forward.x, forward.y, forward.z, 0.0f}};
        gpu.context->UpdateSubresource(gpu.environment_view_buffer, 0, nullptr,
                                       &environment_view, 0, 0);
        gpu.context->PSSetConstantBuffers(3, 1, &gpu.environment_view_buffer);
        // Draw all opaque prelighting first. This small staging adjustment to
        // the target's strip walk ensures authored underground mirror geometry
        // is present before a puddle composites over it.
        for (const GpuMaterialRange& range : gpu.material_ranges) {
            if (gpu_material_uses_alpha(range))
                continue;
            ID3D11ShaderResourceView* texture = range.texture ? range.texture : gpu.white_texture;
            const EnvironmentMaterialConstants material_constants =
                environment_material_constants(range.fallback_color, range.texture != nullptr,
                                               range.has_material_color, 0.0f, false, false);
            gpu.context->UpdateSubresource(gpu.environment_material_buffer, 0, nullptr, &material_constants, 0, 0);
            gpu.context->PSSetShaderResources(2, 1, &texture);
            gpu.context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
            gpu.context->DrawIndexed(range.index_count, range.start_index, 0);
        }
        // Plain flag-2/TXFL-bit-clear materials are solid cutouts. MCP2's
        // ObjectHierarchy renderer selects opaque blending for this class, and
        // MCP1 likewise keeps opaque and background-blended strips separate.
        // Use opaque alpha-test coverage plus the exact one-pass MODULATE2X
        // colour equation, avoiding background blending and nonlinear lifts.
        gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
        gpu.context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        for (const GpuMaterialRange& range : gpu.material_ranges) {
            if (!gpu_material_is_solid_cutout(range))
                continue;
            const EnvironmentMaterialConstants material_constants =
                environment_material_constants(range.fallback_color, true,
                                               range.has_material_color,
                                               kEnvironmentRenderModeSolidCutout, false, true);
            gpu.context->UpdateSubresource(gpu.environment_material_buffer, 0, nullptr, &material_constants, 0, 0);
            gpu.context->PSSetShaderResources(2, 1, &range.texture);
            gpu.context->DrawIndexed(range.index_count, range.start_index, 0);
            gpu.alpha_tested_prelight_drawn = true;
            gpu.solid_cutout_prelight_drawn |=
                material_constants.render_mode == kEnvironmentRenderModeSolidCutout;
        }
        // sub_48D14A/sub_48D15F: target Env's two flag-2 TXFL branches converge
        // on blend mode 1 (SRC_ALPHA/INV_SRC_ALPHA). Retain that path for true
        // blended and multipass materials such as wet roads and detail fences.
        gpu.context->OMSetBlendState(gpu.alpha_blend, nullptr, 0xffffffffu);
        for (const GpuMaterialRange& range : gpu.material_ranges) {
            if (!gpu_material_uses_alpha(range) || gpu_material_is_solid_cutout(range))
                continue;
            const EnvironmentMaterialConstants material_constants =
                environment_material_constants(range.fallback_color, true,
                                               range.has_material_color,
                                               kEnvironmentRenderModeAlphaPrelight, false, true);
            gpu.context->UpdateSubresource(gpu.environment_material_buffer, 0, nullptr, &material_constants, 0, 0);
            gpu.context->PSSetShaderResources(2, 1, &range.texture);
            gpu.context->DrawIndexed(range.index_count, range.start_index, 0);
            gpu.alpha_tested_prelight_drawn = true;
            gpu.composited_alpha_prelight_drawn |=
                material_constants.render_mode == kEnvironmentRenderModeAlphaPrelight;
        }
        // sub_48D2FC/sub_48D311/sub_48D326 disable alpha test and depth writes,
        // select EQUAL depth, then the pass at 0x48DB52 applies texture RGB with
        // D3DBLEND_DESTCOLOR/SRCCOLOR (modulate 2x). It includes flag-2 ranges,
        // tinting both their diffuse contribution and the reflection already
        // present behind it instead of leaving a sharp, raw mirror image.
        gpu.context->OMSetDepthStencilState(gpu.depth_equal, 0);
        gpu.context->OMSetBlendState(gpu.modulate2x_blend, nullptr, 0xffffffffu);
        // The target refreshes the wet texture's offset from its renderer RNG.
        // Use one deterministic per-frame offset for every wet range, matching
        // the single shared texture matrix installed by sub_48C860.
        const uint32_t wet_frame = static_cast<uint32_t>(GetTickCount64() / 33);
        const auto wet_random = [](uint32_t value) {
            value ^= value >> 16;
            value *= 0x7feb352du;
            value ^= value >> 15;
            value *= 0x846ca68bu;
            value ^= value >> 16;
            return (value & 0xffffu) * (5.0f / 65535.0f);
        };
        const DirectX::XMFLOAT4 wet_transform{
            wet_random(wet_frame ^ 0x51ed270bu), wet_random(wet_frame ^ 0xa3c59ac3u), 0.0f, 0.0f};
        for (const GpuMaterialRange& range : gpu.material_ranges) {
            if (!range.texture || gpu_material_is_solid_cutout(range))
                continue;
            const bool wet_splash = gpu.environment_wet_weather && gpu.environment_splash &&
                                    (range.material_flags & 0x4000u) != 0;
            const bool detail = !wet_splash && gpu.environment_detail &&
                                (range.material_flags & 0x4u) != 0;
            ID3D11ShaderResourceView* auxiliary =
                wet_splash ? gpu.environment_splash : (detail ? gpu.environment_detail : nullptr);
            gpu.context->PSSetShaderResources(3, 1, &auxiliary);
            const EnvironmentMaterialConstants material_constants =
                environment_material_constants(wet_splash ? wet_transform : range.fallback_color,
                                               true, range.has_material_color, 1.0f,
                                               wet_splash ? 1.0f : (detail ? 2.0f : 0.0f),
                                               gpu_material_uses_alpha(range));
            gpu.context->UpdateSubresource(gpu.environment_material_buffer, 0, nullptr, &material_constants, 0, 0);
            gpu.context->PSSetShaderResources(2, 1, &range.texture);
            gpu.context->DrawIndexed(range.index_count, range.start_index, 0);
            gpu.wet_splash_drawn |= wet_splash;

            if ((range.material_flags & 0x1u) != 0) {
                // sub_48DD7D: flag 0x1 replays combiner 8 with blend mode 5
                // (SRC_ALPHA/ONE) after the common material draw.
                const EnvironmentMaterialConstants additive_constants =
                    environment_material_constants(range.fallback_color, true,
                                                   range.has_material_color, 3.0f, 0.0f, false);
                gpu.context->UpdateSubresource(gpu.environment_material_buffer, 0, nullptr,
                                               &additive_constants, 0, 0);
                gpu.context->OMSetBlendState(gpu.additive_blend, nullptr, 0xffffffffu);
                gpu.context->DrawIndexed(range.index_count, range.start_index, 0);
                gpu.context->OMSetBlendState(gpu.modulate2x_blend, nullptr, 0xffffffffu);
            }
            if ((range.material_flags & 0x80u) != 0 && gpu.environment_spheremap) {
                // sub_48DDE6..0x48DE86: generated reflection coordinates,
                // combiner 12, then blend mode 3 (DESTCOLOR/ZERO).
                ID3D11ShaderResourceView* sphere = gpu.environment_spheremap;
                gpu.context->PSSetShaderResources(3, 1, &sphere);
                const EnvironmentMaterialConstants reflection_constants =
                    environment_material_constants(range.fallback_color, true,
                                                   range.has_material_color, 4.0f, 0.0f, false);
                gpu.context->UpdateSubresource(gpu.environment_material_buffer, 0, nullptr,
                                               &reflection_constants, 0, 0);
                gpu.context->OMSetBlendState(gpu.reflection_blend, nullptr, 0xffffffffu);
                gpu.context->DrawIndexed(range.index_count, range.start_index, 0);
                gpu.context->OMSetBlendState(gpu.modulate2x_blend, nullptr, 0xffffffffu);
            }
        }
        gpu.context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
        ID3D11ShaderResourceView* no_environment_textures[] = {nullptr, nullptr};
        gpu.context->PSSetShaderResources(2, _countof(no_environment_textures), no_environment_textures);
    }
    const float rain_animation_time = static_cast<float>(fmod(GetTickCount64() * .001, 8192.0));
    gpu_render_rain(camera_position, right, up, forward, fov_y,
                    static_cast<float>(width) / height, rain_animation_time);
    gpu.context->PSSetShader(gpu.pixel_shader, nullptr, 0);
    std::vector<GpuVertex> puppet_vertices;
    size_t puppet_vertex_count = 0;
    for (const Entity& entity : g.document.entities) {
        const SpawnPuppet* model = entity_render_model(entity);
        if (model && model->faces.size() <= (SIZE_MAX - puppet_vertex_count) / 3)
            puppet_vertex_count += model->faces.size() * 3;
    }
    puppet_vertices.reserve(puppet_vertex_count);
    for (int i = 0; i < static_cast<int>(g.document.entities.size()); ++i) {
        if (entity_is_selected(i))
            continue;
        const Entity& entity = g.document.entities[i];
        if (entity.kind == EntityKind::SpawnPoint)
            append_gpu_spawn_puppet(entity, false, &puppet_vertices);
        else if (entity.kind == EntityKind::PhysicalObject)
            append_gpu_pickup_model(entity, false, &puppet_vertices);
    }
    const uint32_t unselected_puppet_count = static_cast<uint32_t>(puppet_vertices.size());
    for (int selected_index : g.selected_entities) {
        if (!valid_entity_index(selected_index))
            continue;
        const Entity& selected_entity = g.document.entities[selected_index];
        if (selected_entity.kind == EntityKind::SpawnPoint)
            append_gpu_spawn_puppet(selected_entity, true, &puppet_vertices);
        else if (selected_entity.kind == EntityKind::PhysicalObject)
            append_gpu_pickup_model(selected_entity, true, &puppet_vertices);
    }
    const uint32_t selected_puppet_count =
        static_cast<uint32_t>(puppet_vertices.size()) - unselected_puppet_count;
    const bool puppets_ready =
        gpu_update_dynamic_vertices(&gpu.puppet_vertices, &gpu.puppet_capacity, puppet_vertices) &&
        gpu.puppet_vertices && !puppet_vertices.empty();
    std::vector<GpuVertex> overlay;
    const float extent = fmaxf(50.0f, g.mesh.radius * 1.5f);
    float step = fmaxf(1.0f, powf(10.0f, floorf(log10f(extent / 10.0f))));
    const int lines = static_cast<int>(extent / step);
    const DirectX::XMFLOAT4 minor{.18f, .22f, .25f, 1}, major{.32f, .38f, .42f, 1};
    size_t gizmo_line_capacity = 0;
    if (valid_entity_index(g.selected)) {
        const EntityKind kind = g.document.entities[g.selected].kind;
        if (kind == EntityKind::Light)
            gizmo_line_capacity = kMaximumLightGizmoLines * g.selected_entities.size();
        else if (kind == EntityKind::Sound)
            gizmo_line_capacity = kLightRangeSegments * 3 * g.selected_entities.size();
    }
    overlay.reserve((lines * 2 + 1) * 4 +
                    g.document.entities.size() * (6 + kCameraSpawnArrowLines * 2) +
                    gizmo_line_capacity * 2);
    for (int i = -lines; i <= lines; ++i) {
        const auto color = i == 0 ? major : minor;
        overlay.push_back(gpu_line_vertex({i * step, 0, -extent}, color));
        overlay.push_back(gpu_line_vertex({i * step, 0, extent}, color));
        overlay.push_back(gpu_line_vertex({-extent, 0, i * step}, color));
        overlay.push_back(gpu_line_vertex({extent, 0, i * step}, color));
    }
    const uint32_t entity_start = static_cast<uint32_t>(overlay.size());
    uint32_t selected_entity_start = entity_start;
    const float marker = fmaxf(.35f, g.mesh.radius * .008f);
    std::vector<LightGizmoLine> entity_gizmo;
    entity_gizmo.reserve(kMaximumLightGizmoLines);
    for (int pass = 0; pass < 2; ++pass) {
      for (int i = 0; i < static_cast<int>(g.document.entities.size()); ++i) {
        const bool selected = entity_is_selected(i);
        if (selected != (pass == 1))
            continue;
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
        if (selected)
            color = {1, 1, 1, 1};
        if (entity.kind == EntityKind::SpawnPoint &&
            (entity.value_u32_a & SnipeSpawnTeam_Camera)) {
            entity_gizmo.clear();
            append_camera_spawn_arrow(entity, selected ? marker * 1.6f : marker, &entity_gizmo);
            for (const LightGizmoLine& line : entity_gizmo) {
                overlay.push_back(gpu_line_vertex(line.a, color));
                overlay.push_back(gpu_line_vertex(line.b, color));
            }
        }
        if (selected && (entity.kind == EntityKind::Light || entity.kind == EntityKind::Sound)) {
            entity_gizmo.clear();
            if (entity.kind == EntityKind::Light)
                append_light_gizmo(entity, &entity_gizmo);
            else
                append_sound_gizmo(entity, &entity_gizmo);
            const DirectX::XMFLOAT4 range_color = entity.kind == EntityKind::Light
                                                       ? DirectX::XMFLOAT4{1, .92f, .35f, 1}
                                                       : DirectX::XMFLOAT4{.25f, .8f, 1, 1};
            for (const LightGizmoLine& line : entity_gizmo) {
                overlay.push_back(gpu_line_vertex(line.a, range_color));
                overlay.push_back(gpu_line_vertex(line.b, range_color));
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
      if (pass == 0)
          selected_entity_start = static_cast<uint32_t>(overlay.size());
    }
    const bool overlay_ready = gpu_update_overlay(overlay) && gpu.overlay_vertices;
    if (overlay_ready) {
        gpu.context->IASetVertexBuffers(0, 1, &gpu.overlay_vertices, &stride, &offset);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
        if (entity_start)
            gpu.context->Draw(entity_start, 0);
    }
    // Unselected models and markers retain the environment depth buffer and
    // therefore disappear naturally behind walls and terrain.
    if (puppets_ready && unselected_puppet_count) {
        gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
        gpu.context->IASetVertexBuffers(0, 1, &gpu.puppet_vertices, &stride, &offset);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu.context->Draw(unselected_puppet_count, 0);
    }
    if (overlay_ready && selected_entity_start > entity_start) {
        gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
        gpu.context->IASetVertexBuffers(0, 1, &gpu.overlay_vertices, &stride, &offset);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        gpu.context->Draw(selected_entity_start - entity_start, entity_start);
    }
    // Only the selected model discards scene depth. Drawing with depth enabled
    // after the clear preserves the model's own self-occlusion.
    if (puppets_ready && selected_puppet_count) {
        gpu.context->ClearDepthStencilView(gpu.depth_view, D3D11_CLEAR_DEPTH, 1, 0);
        gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
        gpu.context->IASetVertexBuffers(0, 1, &gpu.puppet_vertices, &stride, &offset);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu.context->Draw(selected_puppet_count, unselected_puppet_count);
    }
    if (overlay_ready && overlay.size() > selected_entity_start) {
        gpu.context->OMSetDepthStencilState(gpu.depth_disabled, 0);
        gpu.context->IASetVertexBuffers(0, 1, &gpu.overlay_vertices, &stride, &offset);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        gpu.context->Draw(static_cast<UINT>(overlay.size() - selected_entity_start), selected_entity_start);
    }
    gpu.swap_chain->Present(0, 0);
}

bool gpu_verify_environment_color_pipeline() {
    if (!gpu.ready || !gpu.device || !gpu.context)
        return false;
    ID3D11Texture2D* target = nullptr;
    ID3D11Texture2D* readback = nullptr;
    ID3D11RenderTargetView* target_view = nullptr;
    ID3D11Buffer* vertices = nullptr;
    ID3D11Texture2D* alpha_texture = nullptr;
    ID3D11ShaderResourceView* alpha_view = nullptr;
    ID3D11Texture2D* cutout_hole_texture = nullptr;
    ID3D11ShaderResourceView* cutout_hole_view = nullptr;
    ID3D11Texture2D* reflection_texture = nullptr;
    ID3D11ShaderResourceView* reflection_view = nullptr;
    D3D11_TEXTURE2D_DESC target_desc{};
    target_desc.Width = target_desc.Height = target_desc.MipLevels = target_desc.ArraySize = 1;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1;
    target_desc.Usage = D3D11_USAGE_DEFAULT;
    target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    bool ok = SUCCEEDED(gpu.device->CreateTexture2D(&target_desc, nullptr, &target)) &&
              SUCCEEDED(gpu.device->CreateRenderTargetView(target, nullptr, &target_view));
    if (ok) {
        D3D11_TEXTURE2D_DESC staging_desc = target_desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ok = SUCCEEDED(gpu.device->CreateTexture2D(&staging_desc, nullptr, &readback));
    }
    constexpr float authored_red = 75.0f / 255.0f;
    constexpr float authored_green = 70.0f / 255.0f;
    constexpr float authored_blue = 62.0f / 255.0f;
    const DirectX::XMFLOAT4 authored_color{authored_red, authored_green, authored_blue, 1.0f};
    const GpuVertex triangle[] = {
        {{-1.0f, -1.0f, .5f}, {0.0f, 0.0f, 1.0f}, authored_color, {0.0f, 0.0f}},
        {{-1.0f, 3.0f, .5f}, {0.0f, 0.0f, 1.0f}, authored_color, {0.0f, 0.0f}},
        {{3.0f, -1.0f, .5f}, {0.0f, 0.0f, 1.0f}, authored_color, {0.0f, 0.0f}},
    };
    if (ok) {
        D3D11_BUFFER_DESC vertex_desc{};
        vertex_desc.ByteWidth = sizeof(triangle);
        vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
        vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA data{triangle};
        ok = SUCCEEDED(gpu.device->CreateBuffer(&vertex_desc, &data, &vertices));
    }
    if (ok) {
        DirectX::XMFLOAT4X4 identity{};
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        const EnvironmentMaterialConstants material =
            environment_material_constants({0, 0, 0, 0}, false, false, 0.0f, false, false);
        const float clear[] = {0, 0, 0, 1};
        const D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        const UINT stride = sizeof(GpuVertex), offset = 0;
        gpu.context->ClearRenderTargetView(target_view, clear);
        gpu.context->OMSetRenderTargets(1, &target_view, nullptr);
        gpu.context->OMSetDepthStencilState(gpu.depth_disabled, 0);
        gpu.context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        gpu.context->RSSetViewports(1, &viewport);
        gpu.context->RSSetState(gpu.rasterizer);
        gpu.context->IASetInputLayout(gpu.input_layout);
        gpu.context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu.context->VSSetShader(gpu.vertex_shader, nullptr, 0);
        gpu.context->UpdateSubresource(gpu.camera_buffer, 0, nullptr, &identity, 0, 0);
        gpu.context->VSSetConstantBuffers(0, 1, &gpu.camera_buffer);
        gpu.context->PSSetShader(gpu.environment_pixel_shader, nullptr, 0);
        gpu.context->PSSetSamplers(2, 1, &gpu.environment_sampler);
        gpu.context->UpdateSubresource(gpu.environment_material_buffer, 0, nullptr, &material, 0, 0);
        gpu.context->PSSetConstantBuffers(2, 1, &gpu.environment_material_buffer);
        const EnvironmentViewConstants environment_view{
            {0.0f, 0.0f, 2.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f, 0.0f}};
        gpu.context->UpdateSubresource(gpu.environment_view_buffer, 0, nullptr,
                                       &environment_view, 0, 0);
        gpu.context->PSSetConstantBuffers(3, 1, &gpu.environment_view_buffer);
        gpu.context->Draw(_countof(triangle), 0);
        gpu.context->CopyResource(readback, target);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ok = SUCCEEDED(gpu.context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped));
        if (ok) {
            const auto* actual = static_cast<const uint8_t*>(mapped.pData);
            ok = abs(static_cast<int>(actual[0]) - 75) <= 1 &&
                 abs(static_cast<int>(actual[1]) - 70) <= 1 &&
                 abs(static_cast<int>(actual[2]) - 62) <= 1;
            gpu.context->Unmap(readback, 0);
        }
    }
    if (ok) {
        // A one-pixel texture with fractional alpha verifies the target's
        // genuinely composited flag-2 prelight blend, not merely shader output
        // in isolation.
        const uint32_t alpha_pixel = 0x80c08040u;
        D3D11_TEXTURE2D_DESC alpha_desc{};
        alpha_desc.Width = alpha_desc.Height = alpha_desc.MipLevels = alpha_desc.ArraySize = 1;
        alpha_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        alpha_desc.SampleDesc.Count = 1;
        alpha_desc.Usage = D3D11_USAGE_IMMUTABLE;
        alpha_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA alpha_data{&alpha_pixel, sizeof(alpha_pixel), sizeof(alpha_pixel)};
        ok = SUCCEEDED(gpu.device->CreateTexture2D(&alpha_desc, &alpha_data, &alpha_texture)) &&
             SUCCEEDED(gpu.device->CreateShaderResourceView(alpha_texture, nullptr, &alpha_view));
    }
    if (ok) {
        const uint32_t hole_pixel = 0x00000000u;
        D3D11_TEXTURE2D_DESC hole_desc{};
        hole_desc.Width = hole_desc.Height = hole_desc.MipLevels = hole_desc.ArraySize = 1;
        hole_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hole_desc.SampleDesc.Count = 1;
        hole_desc.Usage = D3D11_USAGE_IMMUTABLE;
        hole_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA hole_data{&hole_pixel, sizeof(hole_pixel), sizeof(hole_pixel)};
        ok = SUCCEEDED(gpu.device->CreateTexture2D(&hole_desc, &hole_data, &cutout_hole_texture)) &&
             SUCCEEDED(gpu.device->CreateShaderResourceView(cutout_hole_texture, nullptr,
                                                            &cutout_hole_view));
    }
    if (ok) {
        const EnvironmentMaterialConstants alpha_material =
            environment_material_constants({0, 0, 0, 0}, true, false,
                                           kEnvironmentRenderModeAlphaPrelight, 0.0f, true);
        const float clear[] = {.2f, .4f, .6f, 1.0f};
        gpu.context->ClearRenderTargetView(target_view, clear);
        gpu.context->OMSetRenderTargets(1, &target_view, nullptr);
        gpu.context->OMSetBlendState(gpu.alpha_blend, nullptr, 0xffffffffu);
        gpu.context->UpdateSubresource(gpu.environment_material_buffer, 0, nullptr,
                                       &alpha_material, 0, 0);
        gpu.context->PSSetShaderResources(2, 1, &alpha_view);
        gpu.context->Draw(_countof(triangle), 0);
        gpu.context->CopyResource(readback, target);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ok = SUCCEEDED(gpu.context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped));
        if (ok) {
            const auto* actual = static_cast<const uint8_t*>(mapped.pData);
            constexpr float texture_alpha = 128.0f / 255.0f;
            const int expected[] = {
                static_cast<int>(255.0f * (authored_red * texture_alpha + .2f * (1.0f - texture_alpha)) + .5f),
                static_cast<int>(255.0f * (authored_green * texture_alpha + .4f * (1.0f - texture_alpha)) + .5f),
                static_cast<int>(255.0f * (authored_blue * texture_alpha + .6f * (1.0f - texture_alpha)) + .5f)};
            ok = abs(static_cast<int>(actual[0]) - expected[0]) <= 2 &&
                 abs(static_cast<int>(actual[1]) - expected[1]) <= 2 &&
                 abs(static_cast<int>(actual[2]) - expected[2]) <= 2;
            gpu.context->Unmap(readback, 0);
        }
    }
    if (ok) {
        // Plain solid cutouts use the target's exact opaque MODULATE2X equation
        // after passing the alpha test. This guards against nonlinear texture
        // lifts that flatten contrast or change apparent saturation.
        const EnvironmentMaterialConstants cutout_material =
            environment_material_constants({0, 0, 0, 0}, true, false,
                                           kEnvironmentRenderModeSolidCutout, 0.0f, true);
        const float clear[] = {.2f, .4f, .6f, 1.0f};
        gpu.context->ClearRenderTargetView(target_view, clear);
        gpu.context->OMSetRenderTargets(1, &target_view, nullptr);
        gpu.context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        gpu.context->UpdateSubresource(gpu.environment_material_buffer, 0, nullptr,
                                       &cutout_material, 0, 0);
        gpu.context->PSSetShaderResources(2, 1, &alpha_view);
        gpu.context->Draw(_countof(triangle), 0);
        gpu.context->CopyResource(readback, target);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ok = SUCCEEDED(gpu.context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped));
        if (ok) {
            const auto* actual = static_cast<const uint8_t*>(mapped.pData);
            constexpr float albedo_red = 64.0f / 255.0f;
            constexpr float albedo_green = 128.0f / 255.0f;
            constexpr float albedo_blue = 192.0f / 255.0f;
            const int expected[] = {
                static_cast<int>(255.0f * std::min(1.0f, 2.0f * albedo_red * authored_red) + .5f),
                static_cast<int>(255.0f * std::min(1.0f, 2.0f * albedo_green * authored_green) + .5f),
                static_cast<int>(255.0f * std::min(1.0f, 2.0f * albedo_blue * authored_blue) + .5f)};
            ok = abs(static_cast<int>(actual[0]) - expected[0]) <= 2 &&
                 abs(static_cast<int>(actual[1]) - expected[1]) <= 2 &&
                 abs(static_cast<int>(actual[2]) - expected[2]) <= 2;
            gpu.context->Unmap(readback, 0);
        }
    }
    if (ok) {
        // Zero-alpha texels must still expose the already rendered sky/background.
        const EnvironmentMaterialConstants cutout_material =
            environment_material_constants({0, 0, 0, 0}, true, false,
                                           kEnvironmentRenderModeSolidCutout, 0.0f, true);
        const float clear[] = {.2f, .4f, .6f, 1.0f};
        gpu.context->ClearRenderTargetView(target_view, clear);
        gpu.context->UpdateSubresource(gpu.environment_material_buffer, 0, nullptr,
                                       &cutout_material, 0, 0);
        gpu.context->PSSetShaderResources(2, 1, &cutout_hole_view);
        gpu.context->Draw(_countof(triangle), 0);
        gpu.context->CopyResource(readback, target);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ok = SUCCEEDED(gpu.context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped));
        if (ok) {
            const auto* actual = static_cast<const uint8_t*>(mapped.pData);
            constexpr int expected[] = {51, 102, 153};
            ok = abs(static_cast<int>(actual[0]) - expected[0]) <= 1 &&
                 abs(static_cast<int>(actual[1]) - expected[1]) <= 1 &&
                 abs(static_cast<int>(actual[2]) - expected[2]) <= 1;
            gpu.context->Unmap(readback, 0);
        }
    }
    if (ok) {
        // Give the sphere map an alpha that deliberately differs from the base
        // texture. Target combiner 12 must use CURRENT alpha carried from the
        // base texture, not the sphere map's own alpha.
        const uint32_t reflection_pixel = 0x11c08040u;
        D3D11_TEXTURE2D_DESC reflection_desc{};
        reflection_desc.Width = reflection_desc.Height = reflection_desc.MipLevels = reflection_desc.ArraySize = 1;
        reflection_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        reflection_desc.SampleDesc.Count = 1;
        reflection_desc.Usage = D3D11_USAGE_IMMUTABLE;
        reflection_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA reflection_data{
            &reflection_pixel, sizeof(reflection_pixel), sizeof(reflection_pixel)};
        ok = SUCCEEDED(gpu.device->CreateTexture2D(&reflection_desc, &reflection_data,
                                                   &reflection_texture)) &&
             SUCCEEDED(gpu.device->CreateShaderResourceView(reflection_texture, nullptr,
                                                            &reflection_view));
    }
    if (ok) {
        // Target blend mode 3 is SRC=DESTCOLOR, DEST=ZERO. This verifies the
        // stage-0-alpha combiner and final framebuffer multiplication together.
        const EnvironmentMaterialConstants reflection_material =
            environment_material_constants({0, 0, 0, 0}, true, false, 4.0f, 0.0f, false);
        const float clear[] = {.25f, .5f, .75f, 1.0f};
        gpu.context->ClearRenderTargetView(target_view, clear);
        gpu.context->OMSetRenderTargets(1, &target_view, nullptr);
        gpu.context->OMSetBlendState(gpu.reflection_blend, nullptr, 0xffffffffu);
        gpu.context->UpdateSubresource(gpu.environment_material_buffer, 0, nullptr,
                                       &reflection_material, 0, 0);
        ID3D11ShaderResourceView* reflection_resources[] = {alpha_view, reflection_view};
        gpu.context->PSSetShaderResources(2, _countof(reflection_resources), reflection_resources);
        gpu.context->Draw(_countof(triangle), 0);
        gpu.context->CopyResource(readback, target);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ok = SUCCEEDED(gpu.context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped));
        if (ok) {
            const auto* actual = static_cast<const uint8_t*>(mapped.pData);
            constexpr float base_alpha = 128.0f / 255.0f;
            constexpr float reflection_red = 64.0f / 255.0f;
            constexpr float reflection_green = 128.0f / 255.0f;
            constexpr float reflection_blue = 192.0f / 255.0f;
            const int expected[] = {
                static_cast<int>(255.0f * .25f * (1.0f + (reflection_red - 1.0f) * base_alpha) + .5f),
                static_cast<int>(255.0f * .5f * (1.0f + (reflection_green - 1.0f) * base_alpha) + .5f),
                static_cast<int>(255.0f * .75f * (1.0f + (reflection_blue - 1.0f) * base_alpha) + .5f)};
            ok = abs(static_cast<int>(actual[0]) - expected[0]) <= 2 &&
                 abs(static_cast<int>(actual[1]) - expected[1]) <= 2 &&
                 abs(static_cast<int>(actual[2]) - expected[2]) <= 2;
            gpu.context->Unmap(readback, 0);
        }
    }
    ID3D11ShaderResourceView* no_textures[] = {nullptr, nullptr};
    gpu.context->PSSetShaderResources(2, _countof(no_textures), no_textures);
    gpu.context->OMSetRenderTargets(0, nullptr, nullptr);
    gpu.context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    gpu_release(reflection_view);
    gpu_release(reflection_texture);
    gpu_release(cutout_hole_view);
    gpu_release(cutout_hole_texture);
    gpu_release(alpha_view);
    gpu_release(alpha_texture);
    gpu_release(vertices);
    gpu_release(target_view);
    gpu_release(readback);
    gpu_release(target);
    return ok;
}

bool gpu_verify_rain_streak_display() {
    if (!gpu.ready || !gpu.rain_texture || !gpu.rain_pixel_shader)
        return false;
    constexpr UINT size = 256;
    ID3D11Texture2D* target = nullptr;
    ID3D11Texture2D* readback = nullptr;
    ID3D11RenderTargetView* target_view = nullptr;
    ID3D11Buffer* vertices = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = size;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    bool ok = SUCCEEDED(gpu.device->CreateTexture2D(&desc, nullptr, &target)) &&
              SUCCEEDED(gpu.device->CreateRenderTargetView(target, nullptr, &target_view));
    if (ok) {
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ok = SUCCEEDED(gpu.device->CreateTexture2D(&staging, nullptr, &readback));
    }
    const DirectX::XMFLOAT4 color{1, 1, 1, 1};
    const GpuVertex triangle[] = {
        {{-1.0f, -1.0f, .5f}, {0, -1, 0}, color, {0, 1}},
        {{-1.0f, 3.0f, .5f}, {0, -1, 0}, color, {0, -1}},
        {{3.0f, -1.0f, .5f}, {0, -1, 0}, color, {2, 1}},
    };
    if (ok) {
        D3D11_BUFFER_DESC vertex_desc{};
        vertex_desc.ByteWidth = sizeof(triangle);
        vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
        vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA data{triangle};
        ok = SUCCEEDED(gpu.device->CreateBuffer(&vertex_desc, &data, &vertices));
    }
    if (ok) {
        DirectX::XMFLOAT4X4 identity{};
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        const float clear[] = {0, 0, 0, 1};
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(size),
                                      static_cast<float>(size), 0, 1};
        const UINT stride = sizeof(GpuVertex), offset = 0;
        gpu.context->ClearRenderTargetView(target_view, clear);
        gpu.context->OMSetRenderTargets(1, &target_view, nullptr);
        gpu.context->OMSetDepthStencilState(gpu.depth_disabled, 0);
        gpu.context->OMSetBlendState(gpu.alpha_blend, nullptr, 0xffffffffu);
        gpu.context->RSSetViewports(1, &viewport);
        gpu.context->RSSetState(gpu.rasterizer);
        gpu.context->IASetInputLayout(gpu.input_layout);
        gpu.context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu.context->VSSetShader(gpu.vertex_shader, nullptr, 0);
        gpu.context->UpdateSubresource(gpu.camera_buffer, 0, nullptr, &identity, 0, 0);
        gpu.context->VSSetConstantBuffers(0, 1, &gpu.camera_buffer);
        gpu.context->PSSetShader(gpu.rain_pixel_shader, nullptr, 0);
        gpu.context->PSSetShaderResources(4, 1, &gpu.rain_texture);
        gpu.context->PSSetSamplers(2, 1, &gpu.environment_sampler);
        gpu.context->Draw(_countof(triangle), 0);
        gpu.context->CopyResource(readback, target);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ok = SUCCEEDED(gpu.context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped));
        if (ok) {
            const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
            uint32_t visible = 0, horizontal = 0, vertical = 0;
            for (UINT y = 0; y < size; ++y) {
                for (UINT x = 0; x < size; ++x) {
                    const uint8_t* pixel = bytes + y * mapped.RowPitch + x * 4;
                    if (pixel[0] <= 12)
                        continue;
                    ++visible;
                    horizontal += x > 0 && pixel[-4] > 12;
                    vertical += y > 0 && pixel[-static_cast<int>(mapped.RowPitch)] > 12;
                }
            }
            ok = visible >= 40 && vertical > horizontal * 2;
            gpu.context->Unmap(readback, 0);
        }
    }
    ID3D11ShaderResourceView* none = nullptr;
    gpu.context->PSSetShaderResources(4, 1, &none);
    gpu.context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    gpu.context->OMSetRenderTargets(0, nullptr, nullptr);
    gpu_release(vertices);
    gpu_release(target_view);
    gpu_release(readback);
    gpu_release(target);
    return ok;
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

bool commit_history_transaction() {
    const bool changed = g.history.commit(&g.document, g.selected);
    update_title();
    return changed;
}

void reset_history(bool mark_as_saved) {
    g.history.reset(&g.document, g.selected, mark_as_saved);
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
    char text[384]{};
    const std::to_chars_result result = std::to_chars(text, text + sizeof(text) - 1, value,
                                                       std::chars_format::fixed, 15);
    if (result.ec == std::errc{}) {
        char* end = result.ptr;
        while (end > text && end[-1] == '0')
            --end;
        if (end > text && end[-1] == '.')
            *end++ = '0';
        *end = 0;
        SetWindowTextA(control, text);
    }
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

struct SpawnMaskControl {
    const char* label;
    uint32_t mask;
};

constexpr SpawnMaskControl kSpawnTeamControls[] = {
    {"Teamless", SnipeSpawnTeam_Deathmatch},
    {"Germany", SnipeSpawnTeam_German},
    {"Russia", SnipeSpawnTeam_Russian},
    {"Camera-only", SnipeSpawnTeam_Camera}
};

constexpr SpawnMaskControl kSpawnGameModeControls[] = {
    {"Single-Player", SnipeSpawnGameMode_SinglePlayer},
    {"Cooperative", SnipeSpawnGameMode_LocalCooperative},
    {"Deathmatch", SnipeSpawnGameMode_Deathmatch},
    {"TDM", SnipeSpawnGameMode_TeamDeathmatch},
    {"Manhunt", SnipeSpawnGameMode_Manhunt},
    {"Assassination", SnipeSpawnGameMode_Assassination}
};

enum LightPropertiesId : int {
    ID_LIGHT_POSITION_X = 3000,
    ID_LIGHT_POSITION_Y,
    ID_LIGHT_POSITION_Z,
    ID_LIGHT_R,
    ID_LIGHT_G,
    ID_LIGHT_B,
    ID_LIGHT_BRIGHTNESS,
    ID_LIGHT_RANGE,
    ID_LIGHT_INNER_RANGE,
    ID_LIGHT_SHADOW_STRENGTH,
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
    ID_LIGHT_HAS_CHANGED
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
    {"Shadow volume", ASURA_LIGHT_FLAG_IS_SHADOW_VOLUME}
};

struct LightPropertiesState {
    HWND window = nullptr;
    HWND position[3]{};
    HWND colour[3]{};
    HWND brightness = nullptr;
    HWND range = nullptr;
    HWND inner_range = nullptr;
    HWND shadow_strength = nullptr;
    HWND bounds[6]{};
    HWND flags = nullptr;
    HWND flag_checks[_countof(kLightFlagControls)]{};
    HWND old_position[3]{};
    HWND old_range = nullptr;
    HWND has_changed = nullptr;
    Asura_Light value{};
    bool accepted = false;
};

void update_light_flag_dependent_controls(LightPropertiesState* state, uint32_t flags) {
    const BOOL shadow_volume = (flags & ASURA_LIGHT_FLAG_IS_SHADOW_VOLUME) != 0;
    const BOOL use_bounding_box = (flags & ASURA_LIGHT_FLAG_USE_BOUNDING_BOX) != 0;
    for (HWND field : state->colour)
        EnableWindow(field, !shadow_volume);
    EnableWindow(state->brightness, !shadow_volume);
    EnableWindow(state->shadow_strength, shadow_volume);
    for (HWND field : state->bounds)
        EnableWindow(field, use_bounding_box);
}

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
    make_light_vector_row(state, "Colour (RGB255)", 50, ID_LIGHT_R, state->colour);

    make_light_scalar(state, "Brightness", ID_LIGHT_BRIGHTNESS, 14, 88, &state->brightness);
    make_light_scalar(state, "Range", ID_LIGHT_RANGE, 262, 88, &state->range);
    make_light_scalar(state, "Inner range", ID_LIGHT_INNER_RANGE, 510, 88, &state->inner_range);
    make_light_scalar(state, "Shadow strength", ID_LIGHT_SHADOW_STRENGTH, 14, 122,
                      &state->shadow_strength);

    make_light_control(state, "STATIC", "World-axis bounding box", SS_LEFT, 0, 14, 164, 150, 22);
    constexpr const char* bound_labels[] = {"Min X", "Max X", "Min Y", "Max Y", "Min Z", "Max Z"};
    constexpr int bound_ids[] = {ID_LIGHT_BOUND_MIN_X, ID_LIGHT_BOUND_MAX_X, ID_LIGHT_BOUND_MIN_Y,
                                 ID_LIGHT_BOUND_MAX_Y, ID_LIGHT_BOUND_MIN_Z, ID_LIGHT_BOUND_MAX_Z};
    for (int i = 0; i < 6; ++i) {
        const int column = i % 3, row = i / 3;
        const int x = 130 + column * 204, y = 160 + row * 34;
        make_light_control(state, "STATIC", bound_labels[i], SS_LEFT, 0, x, y + 3, 48, 22);
        state->bounds[i] = make_light_control(state, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                              bound_ids[i], x + 52, y, 90, 24);
    }

    make_light_control(state, "STATIC", "Raw flags", SS_LEFT, 0, 14, 236, 94, 22);
    state->flags = make_light_control(state, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                      ID_LIGHT_FLAGS, 130, 233, 142, 24);
    for (int i = 0; i < static_cast<int>(_countof(kLightFlagControls)); ++i) {
        const int column = i % 4, row = i / 4;
        state->flag_checks[i] = make_light_control(
            state, "BUTTON", kLightFlagControls[i].label, BS_AUTOCHECKBOX | WS_TABSTOP,
            ID_LIGHT_FLAG_FIRST + i, 14 + column * 185, 270 + row * 30, 178, 24);
    }

    make_light_vector_row(state, "Old position", 338, ID_LIGHT_OLD_POSITION_X, state->old_position);
    make_light_scalar(state, "Old range", ID_LIGHT_OLD_RANGE, 14, 376, &state->old_range);
    state->has_changed = make_light_control(state, "BUTTON", "Has changed", BS_AUTOCHECKBOX | WS_TABSTOP,
                                             ID_LIGHT_HAS_CHANGED, 280, 376, 150, 24);

    make_light_control(state, "BUTTON", "Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 526, 430, 104, 30);
    make_light_control(state, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 638, 430, 104, 30);

    const Asura_Light& light = state->value;
    const float position[] = {light.Position.x, light.Position.y, light.Position.z};
    const float colour[] = {light.R, light.G, light.B};
    const float bounds[] = {light.m_xBoundingBox.MinX, light.m_xBoundingBox.MaxX,
                            light.m_xBoundingBox.MinY, light.m_xBoundingBox.MaxY,
                            light.m_xBoundingBox.MinZ, light.m_xBoundingBox.MaxZ};
    const float old_position[] = {light.OldPosition.x, light.OldPosition.y, light.OldPosition.z};
    for (int i = 0; i < 3; ++i) {
        set_float(state->position[i], position[i]);
        set_float(state->colour[i], colour[i]);
        set_float(state->old_position[i], old_position[i]);
    }
    for (int i = 0; i < 6; ++i)
        set_float(state->bounds[i], bounds[i]);
    set_float(state->brightness, light.Brightness);
    set_float(state->range, light.Range);
    set_float(state->inner_range, light.m_fInnerRange);
    set_float(state->shadow_strength, light.ShadowStrength);
    set_u32_hex(state->flags, light.m_uFlags);
    for (int i = 0; i < static_cast<int>(_countof(kLightFlagControls)); ++i)
        SendMessageA(state->flag_checks[i], BM_SETCHECK,
                     light.m_uFlags & kLightFlagControls[i].mask ? BST_CHECKED : BST_UNCHECKED, 0);
    set_float(state->old_range, light.OldRange);
    SendMessageA(state->has_changed, BM_SETCHECK, light.HasChanged ? BST_CHECKED : BST_UNCHECKED, 0);
    update_light_flag_dependent_controls(state, light.m_uFlags);
}

void apply_light_properties(LightPropertiesState* state) {
    Asura_Light& light = state->value;
    light.Position = {get_float(state->position[0], light.Position.x),
                      get_float(state->position[1], light.Position.y),
                      get_float(state->position[2], light.Position.z)};
    light.R = get_float(state->colour[0], light.R);
    light.G = get_float(state->colour[1], light.G);
    light.B = get_float(state->colour[2], light.B);
    light.Brightness = get_float(state->brightness, light.Brightness);
    light.Range = get_float(state->range, light.Range);
    light.m_fInnerRange = get_float(state->inner_range, light.m_fInnerRange);
    light.ShadowStrength = get_float(state->shadow_strength, light.ShadowStrength);
    float* bounds[] = {&light.m_xBoundingBox.MinX, &light.m_xBoundingBox.MaxX,
                       &light.m_xBoundingBox.MinY, &light.m_xBoundingBox.MaxY,
                       &light.m_xBoundingBox.MinZ, &light.m_xBoundingBox.MaxZ};
    for (int i = 0; i < 6; ++i)
        *bounds[i] = get_float(state->bounds[i], *bounds[i]);
    light.m_uFlags = get_u32(state->flags, light.m_uFlags);
    for (int i = 0; i < static_cast<int>(_countof(kLightFlagControls)); ++i) {
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
            for (int i = 0; i < static_cast<int>(_countof(kLightFlagControls)); ++i)
                SendMessageA(state->flag_checks[i], BM_SETCHECK,
                             flags & kLightFlagControls[i].mask ? BST_CHECKED : BST_UNCHECKED, 0);
            update_light_flag_dependent_controls(state, flags);
        } else if (LOWORD(wparam) >= ID_LIGHT_FLAG_FIRST && LOWORD(wparam) <= ID_LIGHT_FLAG_LAST &&
                   HIWORD(wparam) == BN_CLICKED) {
            uint32_t flags = get_u32(state->flags, state->value.m_uFlags);
            const int index = LOWORD(wparam) - ID_LIGHT_FLAG_FIRST;
            if (SendMessageA(state->flag_checks[index], BM_GETCHECK, 0, 0) == BST_CHECKED)
                flags |= kLightFlagControls[index].mask;
            else
                flags &= ~kLightFlagControls[index].mask;
            set_u32_hex(state->flags, flags);
            update_light_flag_dependent_controls(state, flags);
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

    if (!g.history.begin(g.document, g.selected))
        return;
    for (int index : g.selected_entities) {
        if (!valid_entity_index(index) || g.document.entities[index].kind != EntityKind::Light)
            continue;
        Entity& target = g.document.entities[index];
        target.light = state.value;
        target.position = state.value.Position;
        target.value_a = state.value.Brightness;
        target.value_b = state.value.Range;
    }
    commit_history_transaction();
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

const char* entity_type_label(EntityKind kind) {
    switch (kind) {
    case EntityKind::SpawnPoint: return "Spawn";
    case EntityKind::Light: return "Light";
    case EntityKind::Sound: return "Sound";
    case EntityKind::PhysicalObject: return "Pickup";
    case EntityKind::AssassinationTarget: return "Target";
    case EntityKind::PositionMarker: return "Marker";
    }
    return "Entity";
}

int entity_list_row(int document_index) {
    const int count = static_cast<int>(SendMessageA(g.list, LB_GETCOUNT, 0, 0));
    for (int row = 0; row < count; ++row)
        if (static_cast<int>(SendMessageA(g.list, LB_GETITEMDATA, row, 0)) == document_index)
            return row;
    return LB_ERR;
}

void sync_entity_list_selection() {
    if (!g.list)
        return;
    const int count = static_cast<int>(SendMessageA(g.list, LB_GETCOUNT, 0, 0));
    for (int row = 0; row < count; ++row) {
        const int index = static_cast<int>(SendMessageA(g.list, LB_GETITEMDATA, row, 0));
        SendMessageA(g.list, LB_SETSEL, entity_is_selected(index), row);
    }
    if (g.selected >= 0) {
        const int row = entity_list_row(g.selected);
        if (row != LB_ERR)
            SendMessageA(g.list, LB_SETCARETINDEX, row, FALSE);
    }
}

void select_entities_from_list() {
    const int old_primary = g.selected;
    const std::vector<int> old_selection = g.selected_entities;
    std::vector<int> selection;
    const int count = static_cast<int>(SendMessageA(g.list, LB_GETCOUNT, 0, 0));
    for (int row = 0; row < count; ++row) {
        if (SendMessageA(g.list, LB_GETSEL, row, 0) <= 0)
            continue;
        const int index = static_cast<int>(SendMessageA(g.list, LB_GETITEMDATA, row, 0));
        if (valid_entity_index(index))
            selection.push_back(index);
    }

    const LRESULT caret_row = SendMessageA(g.list, LB_GETCARETINDEX, 0, 0);
    const int caret_index = caret_row == LB_ERR
                                ? -1
                                : static_cast<int>(SendMessageA(g.list, LB_GETITEMDATA,
                                                                 static_cast<WPARAM>(caret_row), 0));
    bool restricted = false;
    if (selection.size() > 1) {
        EntityKind allowed_kind = g.document.entities[selection.front()].kind;
        bool mixed = false;
        for (int index : selection)
            mixed |= g.document.entities[index].kind != allowed_kind;
        if (mixed) {
            // When Ctrl/Shift adds another type, preserve the type that was
            // already selected. A plain click has only one item and switches
            // types normally.
            if (!old_selection.empty() && valid_entity_index(old_selection.front()))
                allowed_kind = g.document.entities[old_selection.front()].kind;
            else if (valid_entity_index(caret_index))
                allowed_kind = g.document.entities[caret_index].kind;
            selection.erase(std::remove_if(selection.begin(), selection.end(), [&](int index) {
                                return g.document.entities[index].kind != allowed_kind;
                            }),
                            selection.end());
            restricted = true;
        }
    }

    g.selected_entities = std::move(selection);
    if (entity_is_selected(caret_index))
        g.selected = caret_index;
    else if (entity_is_selected(old_primary))
        g.selected = old_primary;
    else
        g.selected = g.selected_entities.empty() ? -1 : g.selected_entities.back();
    if (restricted)
        sync_entity_list_selection();
    if (g.selected != old_primary || g.selected_entities != old_selection)
        stop_sound_preview();
    refresh_inspector();
    request_redraw();
    if (restricted)
        set_status("Multiple selection is limited to one entity type.");
}

void refresh_list() {
    SendMessageA(g.list, LB_RESETCONTENT, 0, 0);
    std::vector<uint32_t> order(g.document.entities.size());
    for (uint32_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::stable_sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) {
        const Entity& first = g.document.entities[a];
        const Entity& second = g.document.entities[b];
        const int names = _stricmp(first.name.c_str(), second.name.c_str());
        if (names != 0)
            return names < 0;
        const int types = _stricmp(entity_type_label(first.kind), entity_type_label(second.kind));
        if (types != 0)
            return types < 0;
        return first.guid < second.guid;
    });
    for (uint32_t index : order) {
        const Entity& entity = g.document.entities[index];
        const std::string line = std::string(entity_type_label(entity.kind)) + "  " + entity.name;
        const LRESULT row = SendMessageA(g.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        if (row != LB_ERR && row != LB_ERRSPACE)
            SendMessageA(g.list, LB_SETITEMDATA, static_cast<WPARAM>(row), index);
    }
    normalize_selection_state();
    sync_entity_list_selection();
}

void refresh_pickup_choices(uint32_t selected_item) {
    SendMessageA(g.pickup_item, CB_RESETCONTENT, 0, 0);
    std::vector<const PickupTemplate*> choices;
    choices.reserve(g.document.pickup_templates.size());
    for (const PickupTemplate& item : g.document.pickup_templates)
        choices.push_back(&item);
    std::stable_sort(choices.begin(), choices.end(), [](const PickupTemplate* a, const PickupTemplate* b) {
        const std::string first = snipe_item_label(a->item_id);
        const std::string second = snipe_item_label(b->item_id);
        const int names = _stricmp(first.c_str(), second.c_str());
        return names != 0 ? names < 0 : a->item_id < b->item_id;
    });
    for (const PickupTemplate* item : choices) {
        const std::string label = snipe_item_label(item->item_id);
        const LRESULT row = SendMessageA(g.pickup_item, CB_ADDSTRING, 0,
                                         reinterpret_cast<LPARAM>(label.c_str()));
        if (row == CB_ERR || row == CB_ERRSPACE)
            continue;
        SendMessageA(g.pickup_item, CB_SETITEMDATA, static_cast<WPARAM>(row), item->item_id);
        if (item->item_id == selected_item)
            SendMessageA(g.pickup_item, CB_SETCURSEL, static_cast<WPARAM>(row), 0);
    }
}

void refresh_inspector() {
    g.refreshing_inspector = true;
    g.inspector_dirty = 0;
    const bool enabled = g.selected >= 0 && g.selected < static_cast<int>(g.document.entities.size());
    const bool source_entity = enabled && g.document.entities[g.selected].source_entity_record;
    const bool pickup = enabled && g.document.entities[g.selected].kind == EntityKind::PhysicalObject;
    bool selection_deletable = enabled;
    for (int index : g.selected_entities) {
        const Entity& entity = g.document.entities[index];
        selection_deletable &= !entity.source_entity_record || entity.kind == EntityKind::PhysicalObject;
    }
    if (g.rain_toggle) {
        SendMessageA(g.rain_toggle, BM_SETCHECK,
                     g.document.rain_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(g.rain_toggle,
                     !g.document.source_pc_path.empty() || !g.document.obj_path.empty());
    }
    if (g.ambience_properties)
        EnableWindow(g.ambience_properties,
                     !g.document.source_pc_path.empty() || !g.document.obj_path.empty());
    HWND fields[] = {g.name, g.pos[0], g.pos[1], g.pos[2], g.rot[0], g.rot[1], g.rot[2], g.value[0], g.value[1]};
    for (HWND h : fields)
        EnableWindow(h, enabled);
    EnableWindow(g.value[0], enabled && (!source_entity || pickup));
    EnableWindow(g.value[1], enabled && !source_entity && !pickup);
    EnableWindow(GetDlgItem(g.window, ID_APPLY_INSPECTOR), enabled);
    EnableWindow(GetDlgItem(g.window, ID_DELETE_ENTITY), selection_deletable);
    if (g.selected_entities.size() > 1) {
        char label[96]{};
        snprintf(label, sizeof(label), "Apply properties to %zu selected", g.selected_entities.size());
        SetWindowTextA(GetDlgItem(g.window, ID_APPLY_INSPECTOR), label);
    } else {
        SetWindowTextA(GetDlgItem(g.window, ID_APPLY_INSPECTOR), "Apply properties");
    }
    ShowWindow(g.sound_browse, SW_HIDE);
    ShowWindow(g.sound_loop, SW_HIDE);
    ShowWindow(g.sound_preview, SW_HIDE);
    ShowWindow(g.light_properties, SW_HIDE);
    ShowWindow(g.pickup_item, SW_HIDE);
    ShowWindow(g.spawn_team_label, SW_HIDE);
    ShowWindow(g.spawn_game_mode_label, SW_HIDE);
    for (HWND check : g.spawn_team_checks)
        ShowWindow(check, SW_HIDE);
    for (HWND check : g.spawn_game_mode_checks)
        ShowWindow(check, SW_HIDE);
    for (int i = 0; i < 3; ++i) {
        ShowWindow(GetDlgItem(g.window, 914 + i), SW_SHOW);
        ShowWindow(g.rot[i], SW_SHOW);
    }
    for (int i = 0; i < 2; ++i) {
        ShowWindow(g.value_label[i], SW_SHOW);
        ShowWindow(g.value[i], SW_SHOW);
    }
    if (!enabled) {
        for (HWND h : fields)
            SetWindowTextA(h, "");
        set_control_text(g.value_label[0], "Property A");
        set_control_text(g.value_label[1], "Property B");
        set_control_text(GetDlgItem(g.window, 914), "Pitch");
        set_control_text(GetDlgItem(g.window, 915), "Yaw");
        set_control_text(GetDlgItem(g.window, 916), "Roll");
        g.refreshing_inspector = false;
        return;
    }
    const Entity& e = g.document.entities[g.selected];
    SetWindowTextA(g.name, e.name.c_str());
    set_float(g.pos[0], e.position.x);
    set_float(g.pos[1], e.position.y);
    set_float(g.pos[2], e.position.z);
    if (e.kind == EntityKind::Light) {
        for (int i = 0; i < 3; ++i) {
            ShowWindow(GetDlgItem(g.window, 914 + i), SW_HIDE);
            ShowWindow(g.rot[i], SW_HIDE);
        }
    } else {
        set_control_text(GetDlgItem(g.window, 914), "Pitch");
        set_control_text(GetDlgItem(g.window, 915), "Yaw");
        set_control_text(GetDlgItem(g.window, 916), "Roll");
        set_float(g.rot[0], e.rotation.x);
        set_float(g.rot[1], e.rotation.y);
        set_float(g.rot[2], e.rotation.z);
        if (e.kind == EntityKind::SpawnPoint) {
            set_control_text(GetDlgItem(g.window, 914), "Camera pitch");
            set_control_text(GetDlgItem(g.window, 915), "Camera yaw");
            ShowWindow(GetDlgItem(g.window, 916), SW_HIDE);
            ShowWindow(g.rot[2], SW_HIDE);
        }
    }
    if (e.kind == EntityKind::SpawnPoint) {
        for (int i = 0; i < 2; ++i) {
            ShowWindow(g.value_label[i], SW_HIDE);
            ShowWindow(g.value[i], SW_HIDE);
        }
        ShowWindow(g.spawn_team_label, SW_SHOW);
        ShowWindow(g.spawn_game_mode_label, SW_SHOW);
        for (int i = 0; i < static_cast<int>(_countof(kSpawnTeamControls)); ++i) {
            ShowWindow(g.spawn_team_checks[i], SW_SHOW);
            SendMessageA(g.spawn_team_checks[i], BM_SETCHECK,
                         e.value_u32_a & kSpawnTeamControls[i].mask ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        for (int i = 0; i < static_cast<int>(_countof(kSpawnGameModeControls)); ++i) {
            ShowWindow(g.spawn_game_mode_checks[i], SW_SHOW);
            SendMessageA(g.spawn_game_mode_checks[i], BM_SETCHECK,
                         e.value_u32_b & kSpawnGameModeControls[i].mask ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    } else if (e.kind == EntityKind::Light) {
        const bool shadow_volume = (e.light.m_uFlags & ASURA_LIGHT_FLAG_IS_SHADOW_VOLUME) != 0;
        set_control_text(g.value_label[0], shadow_volume ? "Shadow strength" : "Brightness");
        set_control_text(g.value_label[1], "Range");
        set_float(g.value[0], shadow_volume ? e.light.ShadowStrength : e.light.Brightness);
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
        ShowWindow(g.sound_preview, SW_SHOW);
    } else if (e.kind == EntityKind::PhysicalObject) {
        set_control_text(g.value_label[0], "Item type");
        set_control_text(g.value_label[1], "Object file ID");
        set_u32_hex(g.value[0], e.value_u32_a);
        set_u32_hex(g.value[1], e.value_u32_b);
        ShowWindow(g.value[0], SW_HIDE);
        refresh_pickup_choices(e.value_u32_a);
        EnableWindow(g.pickup_item,
                     SendMessageA(g.pickup_item, CB_GETCOUNT, 0, 0) > 0);
        ShowWindow(g.pickup_item, SW_SHOW);
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
    g.refreshing_inspector = false;
}

void select_entity(int index) {
    if (index != g.selected)
        stop_sound_preview();
    set_single_selection_state(index);
    sync_entity_list_selection();
    refresh_inspector();
    request_redraw();
}

void toggle_entity_selection(int index) {
    if (!valid_entity_index(index))
        return;
    if (!g.selected_entities.empty() && valid_entity_index(g.selected_entities.front()) &&
        g.document.entities[g.selected_entities.front()].kind != g.document.entities[index].kind) {
        set_status("Multiple selection is limited to one entity type.");
        return;
    }
    stop_sound_preview();
    const auto found = std::find(g.selected_entities.begin(), g.selected_entities.end(), index);
    if (found == g.selected_entities.end()) {
        g.selected_entities.push_back(index);
        g.selected = index;
    } else {
        g.selected_entities.erase(found);
        g.selected = g.selected_entities.empty() ? -1 : g.selected_entities.back();
    }
    sync_entity_list_selection();
    refresh_inspector();
    request_redraw();
}

void focus_camera_on_entity(int index) {
    if (index < 0 || index >= static_cast<int>(g.document.entities.size()))
        return;
    const Entity& entity = g.document.entities[index];
    g.camera.target = entity_view_position(entity.position);

    // Fit an actual spawn/pickup mesh when one is available. Rotation and the
    // target-to-editor Y conversion preserve the local bounding-sphere radius.
    float radius = fmaxf(.35f, g.mesh.radius * .008f) * 1.6f;
    if (const SpawnPuppet* model = entity_render_model(entity)) {
        const Asura_Vector_3 local_center{(model->min.x + model->max.x) * .5f,
                                          (model->min.y + model->max.y) * .5f,
                                          (model->min.z + model->max.z) * .5f};
        g.camera.target = spawn_puppet_view_position(local_center, entity);
        const float width = model->max.x - model->min.x;
        const float height = model->max.y - model->min.y;
        const float depth = model->max.z - model->min.z;
        const float model_radius = .5f * sqrtf(width * width + height * height + depth * depth);
        if (isfinite(model_radius) && model_radius > .01f)
            radius = model_radius;
    } else if (entity.kind == EntityKind::PositionMarker) {
        const float width = entity.source_bounds.MaxX - entity.source_bounds.MinX;
        const float height = entity.source_bounds.MaxY - entity.source_bounds.MinY;
        const float depth = entity.source_bounds.MaxZ - entity.source_bounds.MinZ;
        const float marker_radius = .5f * sqrtf(width * width + height * height + depth * depth);
        if (isfinite(marker_radius) && marker_radius > .01f)
            radius = marker_radius;
    }

    g.camera.distance = std::clamp(radius * 2.8f, .5f, 1000000.0f);
    invalidate_environment_cache();
    request_redraw();
    set_status("Camera focused on the selected entity.");
}

void apply_inspector() {
    normalize_selection_state();
    if (!valid_entity_index(g.selected))
        return;
    if (!g.history.begin(g.document, g.selected))
        return;
    char name[512]{};
    GetWindowTextA(g.name, name, sizeof(name));
    const std::vector<int> targets = g.selected_entities;
    const bool apply_all = targets.size() == 1;
    const auto changed = [&](uint32_t flag) { return apply_all || (g.inspector_dirty & flag) != 0; };
    const uint32_t position_dirty = kInspectorDirtyPosX | kInspectorDirtyPosY | kInspectorDirtyPosZ;
    const uint32_t rotation_dirty = kInspectorDirtyRotX | kInspectorDirtyRotY | kInspectorDirtyRotZ;
    for (int index : targets) {
        Entity& e = g.document.entities[index];
        if (changed(kInspectorDirtyName))
            e.name = name;
        if (changed(kInspectorDirtyPosX))
            e.position.x = get_float(g.pos[0], e.position.x);
        if (changed(kInspectorDirtyPosY))
            e.position.y = get_float(g.pos[1], e.position.y);
        if (changed(kInspectorDirtyPosZ))
            e.position.z = get_float(g.pos[2], e.position.z);
        if (e.kind == EntityKind::Light) {
            if (apply_all || (g.inspector_dirty & position_dirty))
                e.light.Position = e.position;
        } else if (e.kind == EntityKind::SpawnPoint) {
            if (changed(kInspectorDirtyRotX))
                e.rotation.x = get_float(g.rot[0], e.rotation.x);
            if (changed(kInspectorDirtyRotY))
                e.rotation.y = get_float(g.rot[1], e.rotation.y);
            if (apply_all)
                e.rotation.z = 0.0f;
        } else {
            if (changed(kInspectorDirtyRotX))
                e.rotation.x = get_float(g.rot[0], e.rotation.x);
            if (changed(kInspectorDirtyRotY))
                e.rotation.y = get_float(g.rot[1], e.rotation.y);
            if (changed(kInspectorDirtyRotZ))
                e.rotation.z = get_float(g.rot[2], e.rotation.z);
        }
        if (e.kind == EntityKind::SpawnPoint) {
            for (int i = 0; i < static_cast<int>(_countof(kSpawnTeamControls)); ++i) {
                if (!changed(1u << (kInspectorDirtySpawnTeamShift + i)))
                    continue;
                if (SendMessageA(g.spawn_team_checks[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
                    e.value_u32_a |= kSpawnTeamControls[i].mask;
                else
                    e.value_u32_a &= ~kSpawnTeamControls[i].mask;
            }
            for (int i = 0; i < static_cast<int>(_countof(kSpawnGameModeControls)); ++i) {
                if (!changed(1u << (kInspectorDirtySpawnGameModeShift + i)))
                    continue;
                if (SendMessageA(g.spawn_game_mode_checks[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
                    e.value_u32_b |= kSpawnGameModeControls[i].mask;
                else
                    e.value_u32_b &= ~kSpawnGameModeControls[i].mask;
            }
            if (apply_all || (g.inspector_dirty & (kInspectorDirtyRotX | kInspectorDirtyRotY))) {
                const float yaw = e.rotation.y * 3.14159265358979323846f / 180.0f;
                const float pitch = e.rotation.x * 3.14159265358979323846f / 180.0f;
                e.spawn_direction = {cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw)};
            }
        } else if (e.kind == EntityKind::Light) {
            if (changed(kInspectorDirtyValueA)) {
                if (e.light.m_uFlags & ASURA_LIGHT_FLAG_IS_SHADOW_VOLUME)
                    e.light.ShadowStrength = get_float(g.value[0], e.light.ShadowStrength);
                else
                    e.light.Brightness = get_float(g.value[0], e.light.Brightness);
            }
            if (changed(kInspectorDirtyValueB))
                e.value_b = get_float(g.value[1], e.light.Range);
            e.value_a = e.light.Brightness;
            e.light.Range = e.value_b;
        } else if (e.kind == EntityKind::Sound) {
            if (changed(kInspectorDirtyValueA))
                e.value_a = get_float(g.value[0], e.value_a);
            if (changed(kInspectorDirtyValueB))
                e.value_b = get_float(g.value[1], e.value_b);
            if (changed(kInspectorDirtySoundLoop))
                e.sound_loop = SendMessageA(g.sound_loop, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (e.sound_source_record &&
                (apply_all || (g.inspector_dirty & (position_dirty | rotation_dirty |
                                                    kInspectorDirtyValueA | kInspectorDirtyValueB |
                                                    kInspectorDirtySoundLoop)))) {
                e.sound_phonon.m_xPosition = e.position;
                e.sound_phonon.m_fInnerRadius = e.value_a;
                e.sound_phonon.m_fOuterRadius = e.value_b;
                e.sound_phonon.m_xOrient = euler_quaternion(e.rotation);
                e.sound_phonon.m_uFlags = e.sound_loop ? (e.sound_phonon.m_uFlags | 1u)
                                                       : (e.sound_phonon.m_uFlags & ~1u);
            }
        } else if (e.kind == EntityKind::PhysicalObject) {
            const LRESULT selected_item = SendMessageA(g.pickup_item, CB_GETCURSEL, 0, 0);
            const uint32_t requested_item = !changed(kInspectorDirtyPickup) || selected_item == CB_ERR
                                                ? e.value_u32_a
                                                : static_cast<uint32_t>(SendMessageA(
                                                      g.pickup_item, CB_GETITEMDATA,
                                                      static_cast<WPARAM>(selected_item), 0));
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
                } else {
                    char message[192]{};
                    snprintf(message, sizeof(message),
                             "Item 0x%02X is not present in the loaded 0x0008 pickup catalog.",
                             requested_item);
                    set_status(message);
                }
            }
        }
    }
    commit_history_transaction();
    refresh_list();
    refresh_inspector();
    request_redraw();
    if (targets.size() > 1) {
        char status[128]{};
        snprintf(status, sizeof(status), "Properties applied to %zu selected %s entities.",
                 targets.size(), entity_type_label(g.document.entities[targets.front()].kind));
        set_status(status);
    }
}

void frame_mesh() {
    g.camera.target = g.mesh.center;
    g.camera.distance = fmaxf(10.0f, g.mesh.radius * 2.3f);
    g.environment_raycast.build(g.mesh);
    invalidate_environment_cache();
    if (gpu.ready)
        gpu_upload_mesh();
}

bool open_obj_path(const std::string& path) {
    stop_sound_preview();
    Mesh mesh;
    std::string why;
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const std::string preview_material_map = g.document.source_pc_path.empty() ? g.document.material_map : std::string{};
    const bool ok = load_preview_mesh(path, preview_material_map, &mesh, &why);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!ok) {
        MessageBoxA(g.window, why.c_str(), "Could not open OBJ", MB_ICONERROR);
        return false;
    }
    if (!g.document.source_pc_path.empty()) {
        g.document = Document{};
        g.pickup_models.clear();
        set_single_selection_state(-1);
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
    reset_history(false);
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
    if (!g.history.begin(g.document, g.selected))
        return;
    e.kind = kind;
    e.position = p;
    e.guid = allocate_editor_guid(&g.document);
    if (!e.guid) {
        g.history.cancel_transaction();
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
    stop_sound_preview();
    g.document.entities.push_back(std::move(e));
    g.pending_kind = -1;
    set_single_selection_state(static_cast<int>(g.document.entities.size()) - 1);
    commit_history_transaction();
    refresh_list();
    refresh_inspector();
    set_status("Entity placed on the environment. Drag it across environment geometry or edit its transform.");
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
    set_status("Click visible environment geometry to place the entity. Right-drag orbits; wheel zooms.");
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
        if (!entity_is_selected(i) &&
            environment_occludes_view_position(entity_view_position(entity.position)))
            continue;
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

void draw_gizmo_lines(HDC dc, const std::vector<LightGizmoLine>& lines, int width, COLORREF color) {
    if (lines.empty())
        return;
    HPEN pen = CreatePen(PS_SOLID, width, color);
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

void draw_light_gizmo(HDC dc, const Entity& entity, bool selected) {
    std::vector<LightGizmoLine> lines;
    lines.reserve(kMaximumLightGizmoLines);
    append_light_gizmo(entity, &lines);
    draw_gizmo_lines(dc, lines, selected ? 2 : 1,
                     selected ? RGB(255, 235, 90) : RGB(148, 120, 24));
}

void draw_sound_gizmo(HDC dc, const Entity& entity) {
    std::vector<LightGizmoLine> lines;
    lines.reserve(kLightRangeSegments * 3);
    append_sound_gizmo(entity, &lines);
    draw_gizmo_lines(dc, lines, 2, RGB(65, 205, 255));
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
        const bool selected = entity_is_selected(i);
        if (!selected && environment_occludes_view_position(entity_view_position(e.position)))
            continue;
        POINT p{};
        if (!project_point(entity_view_position(e.position), &p))
            continue;
        const COLORREF color = entity_color(e.kind);
        if (e.kind == EntityKind::Light && selected)
            draw_light_gizmo(dc, e, true);
        else if (e.kind == EntityKind::Sound && selected)
            draw_sound_gizmo(dc, e);
        if (e.kind == EntityKind::SpawnPoint && (e.value_u32_a & SnipeSpawnTeam_Camera)) {
            std::vector<LightGizmoLine> lines;
            lines.reserve(kCameraSpawnArrowLines);
            const float marker = fmaxf(.35f, g.mesh.radius * .008f) * (selected ? 1.6f : 1.0f);
            append_camera_spawn_arrow(e, marker, &lines);
            draw_gizmo_lines(dc, lines, selected ? 2 : 1,
                             selected ? RGB(255, 255, 255) : color);
        }
        if ((e.kind == EntityKind::SpawnPoint || e.kind == EntityKind::PhysicalObject) &&
            draw_spawn_puppet(dc, e, selected)) {
            if (selected)
                TextOutA(dc, p.x + 10, p.y - 8, e.name.c_str(), static_cast<int>(e.name.size()));
            continue;
        }
        HBRUSH brush = CreateSolidBrush(color);
        HPEN pen = CreatePen(PS_SOLID, selected ? 3 : 1, selected ? RGB(255, 255, 255) : color);
        HGDIOBJ ob = SelectObject(dc, brush), op = SelectObject(dc, pen);
        const int r = selected ? 7 : 5;
        Ellipse(dc, p.x - r, p.y - r, p.x + r + 1, p.y + r + 1);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(brush);
        DeleteObject(pen);
        if (selected)
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
                                            {ID_EXPORT_PC, 360, 90},       {ID_MATERIAL_MAP, 454, 140},
                                            {ID_EXPORT_MATERIAL_MAP, 598, 126}, {ID_TEXTURE_DIR, 728, 100},
                                            {ID_WEAPONS_DONOR, 832, 112},  {ID_SKYBOX_TEXTURES, 948, 108},
                                            {ID_TOGGLE_RAIN, 1060, 54}
    };
    for (auto c : top)
        MoveWindow(GetDlgItem(g.window, c.id), c.x, top_y, c.w, 28, TRUE);
    MoveWindow(g.ambience_properties, right, top_y, 252, 28, TRUE);
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
    MoveWindow(hint, 8, y, 220, 75, TRUE);

    const int label_x = right, edit_x = right + 88, ew = 164;
    const int property_edit_x = right + 116, property_ew = 136;
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
    MoveWindow(g.value_label[0], label_x, iy + 3, 112, 22, TRUE);
    MoveWindow(g.value[0], property_edit_x, iy, property_ew, 24, TRUE);
    MoveWindow(g.pickup_item, property_edit_x, iy, property_ew, 240, TRUE);
    iy += 30;
    MoveWindow(g.value_label[1], label_x, iy + 3, 112, 22, TRUE);
    MoveWindow(g.value[1], property_edit_x, iy, property_ew, 24, TRUE);
    iy += 34;
    MoveWindow(g.sound_browse, edit_x, iy, 80, 26, TRUE);
    MoveWindow(g.sound_preview, edit_x + 84, iy, 80, 26, TRUE);
    MoveWindow(g.sound_loop, label_x, iy, 82, 26, TRUE);
    MoveWindow(g.light_properties, edit_x, iy, ew, 26, TRUE);
    MoveWindow(g.spawn_team_label, label_x, iy, 252, 22, TRUE);
    for (int i = 0; i < static_cast<int>(_countof(kSpawnTeamControls)); ++i) {
        const int column = i % 2, row = i / 2;
        MoveWindow(g.spawn_team_checks[i], label_x + column * 126, iy + 22 + row * 24, 126, 22, TRUE);
    }
    MoveWindow(g.spawn_game_mode_label, label_x, iy + 72, 252, 22, TRUE);
    for (int i = 0; i < static_cast<int>(_countof(kSpawnGameModeControls)); ++i) {
        const int column = i % 2, row = i / 2;
        MoveWindow(g.spawn_game_mode_checks[i], label_x + column * 126, iy + 94 + row * 24, 126, 22, TRUE);
    }
    iy += 172;
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
    make_control("BUTTON", "Import material map", BS_PUSHBUTTON, ID_MATERIAL_MAP);
    make_control("BUTTON", "Export material map", BS_PUSHBUTTON, ID_EXPORT_MATERIAL_MAP);
    make_control("BUTTON", "Texture folder", BS_PUSHBUTTON, ID_TEXTURE_DIR);
    make_control("BUTTON", "Weapons donor", BS_PUSHBUTTON, ID_WEAPONS_DONOR);
    make_control("BUTTON", "Skybox properties", BS_PUSHBUTTON, ID_SKYBOX_TEXTURES);
    g.rain_toggle = make_control("BUTTON", "Rain", BS_AUTOCHECKBOX, ID_TOGGLE_RAIN);
    g.ambience_properties = make_control("BUTTON", "Ambience sound", BS_PUSHBUTTON, ID_AMBIENCE_PROPERTIES);
    g.list = make_control("LISTBOX", "", LBS_NOTIFY | LBS_EXTENDEDSEL | WS_VSCROLL | WS_BORDER,
                          ID_ENTITY_LIST);
    make_control("BUTTON", "+ Spawn", BS_PUSHBUTTON, ID_ADD_SPAWN);
    make_control("BUTTON", "+ Light", BS_PUSHBUTTON, ID_ADD_LIGHT);
    make_control("BUTTON", "+ Pickup", BS_PUSHBUTTON, ID_ADD_PICKUP);
    make_control("BUTTON", "+ Sound", BS_PUSHBUTTON, ID_ADD_SOUND);
    make_control("BUTTON", "Delete selected", BS_PUSHBUTTON, ID_DELETE_ENTITY);
    make_control("STATIC",
                 "Right-drag: orbit; middle-drag: pan; wheel: zoom\r\n"
                 "Ctrl/Shift list: select same-type entities\r\n"
                 "Ctrl+Z/Y: undo/redo; Ctrl+C/V: copy/paste; Delete: remove",
                 SS_LEFT, 900);
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
    g.pickup_item = make_control("COMBOBOX", "", CBS_DROPDOWNLIST | CBS_AUTOHSCROLL | WS_VSCROLL,
                                 ID_PICKUP_ITEM);
    g.sound_browse = make_control("BUTTON", "Choose WAV...", BS_PUSHBUTTON, ID_BROWSE_SOUND);
    g.sound_loop = make_control("BUTTON", "Loop", BS_AUTOCHECKBOX, ID_SOUND_LOOP);
    g.sound_preview = make_control("BUTTON", "Play preview", BS_PUSHBUTTON, ID_SOUND_PREVIEW);
    g.spawn_team_label = make_control("STATIC", "Teams", SS_LEFT, 919);
    g.spawn_game_mode_label = make_control("STATIC", "Game modes", SS_LEFT, 920);
    for (int i = 0; i < static_cast<int>(_countof(kSpawnTeamControls)); ++i)
        g.spawn_team_checks[i] = make_control("BUTTON", kSpawnTeamControls[i].label, BS_AUTOCHECKBOX,
                                             ID_SPAWN_TEAM_FIRST + i);
    for (int i = 0; i < static_cast<int>(_countof(kSpawnGameModeControls)); ++i)
        g.spawn_game_mode_checks[i] = make_control("BUTTON", kSpawnGameModeControls[i].label, BS_AUTOCHECKBOX,
                                                  ID_SPAWN_GAME_MODE_FIRST + i);
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
        if (!load_preview_mesh(document->obj_path, document->material_map, mesh, why))
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
        if (!document->skybox.source_record && imported.skybox.source_record)
            document->skybox = imported.skybox;
        if (!document->weather_source_record && imported.weather_source_record) {
            document->weather_source_record = true;
            document->rain_enabled = imported.rain_enabled;
        }
        if (!document->ambient_source_record && imported.ambient_source_record) {
            document->ambient_source_record = true;
            document->ambient_stream_path = imported.ambient_stream_path;
            document->ambient_volume = imported.ambient_volume;
        }
        enrich_project_pickup_templates(document, imported);
        return true;
    }
    if (pickup_models)
        pickup_models->clear();
    *mesh = {};
    return true;
}

bool open_pc_path(const std::string& path) {
    stop_sound_preview();
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
    set_single_selection_state(g.document.entities.empty() ? -1 : 0);
    g.pending_kind = -1;
    std::string skybox_why;
    const bool skybox_loaded = gpu_load_pc_skybox(path, &skybox_why);
    frame_mesh();
    reset_history(true);
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
    g.history.mark_saved(&g.document);
    update_title();
    set_status("Project saved.");
}

bool reload_skybox_preview(bool show_warning) {
    std::string why;
    const bool use_embedded = g.document.sky_texture_dir.empty() && !g.document.source_pc_path.empty();
    const bool loaded = use_embedded ? gpu_load_pc_skybox(g.document.source_pc_path, &why)
                                     : gpu_load_skybox(g.document.sky_texture_dir, &why);
    if (loaded) {
        gpu_rebuild_skybox_vertices(g.document.skybox.orientation_radians,
                                    g.document.skybox.back_texture_is_front_upside_down,
                                    g.document.skybox.right_texture_is_left_upside_down, nullptr);
        gpu.skybox_tint = {std::clamp(g.document.skybox.red / 255.0f, 0.0f, 1.0f),
                           std::clamp(g.document.skybox.green / 255.0f, 0.0f, 1.0f),
                           std::clamp(g.document.skybox.blue / 255.0f, 0.0f, 1.0f), 1.0f};
    }
    if (!loaded && show_warning && (use_embedded || !g.document.sky_texture_dir.empty()))
        MessageBoxA(g.window, why.c_str(), "Skybox preview unavailable", MB_ICONWARNING);
    request_redraw();
    return loaded;
}

bool same_skybox_settings(const SkyboxSettings& a, const SkyboxSettings& b) {
    return a.chunk_version == b.chunk_version && a.red == b.red &&
           a.green == b.green && a.blue == b.blue &&
           a.orientation_radians == b.orientation_radians && a.texture_paths == b.texture_paths &&
           a.draw_clouds == b.draw_clouds &&
           a.back_texture_is_front_upside_down == b.back_texture_is_front_upside_down &&
           a.right_texture_is_left_upside_down == b.right_texture_is_left_upside_down &&
           a.source_record == b.source_record;
}

bool same_ambience_settings(const Document& a, const Document& b) {
    return a.ambient_stream_path == b.ambient_stream_path &&
           memcmp(&a.ambient_volume, &b.ambient_volume, sizeof(a.ambient_volume)) == 0 &&
           a.ambient_source_record == b.ambient_source_record;
}

bool all_pc_rain_records_match(const char* path, bool rain_enabled) {
    Error err{};
    Arena arena{};
    ChunkList chunks{};
    bool found_weather = false, matches = true;
    if (!arena_init(&arena, 8 * MiB, &err) || !parse_chunks(path, &chunks, &arena, &err)) {
        arena_release(&arena);
        return false;
    }
    for (uint32_t index = 0; index < chunks.count; ++index) {
        const ChunkRef& chunk = chunks.chunks[index];
        if (chunk.cid == ASURA_CHUNK_WEATHERSYSTEM && chunk.version >= 5 &&
            chunk.version <= 6 && chunk.size > 22) {
            found_weather = true;
            matches &= (chunk.data[21] != 0) == rain_enabled &&
                       (chunk.data[22] != 0) == rain_enabled;
        } else if (chunk.cid == ASURA_CHUNK_EXTENDED_PARTICLE_RAIN_SYSTEM &&
                   chunk.version == 0 &&
                   chunk.size >= kExtendedRainFlagsOffset + sizeof(uint32_t)) {
            const uint32_t flags = read_u32(chunk.data + kExtendedRainFlagsOffset);
            matches &= (flags & kExtendedRainEnabledMask) ==
                       (rain_enabled ? kExtendedRainEnabledMask : 0u);
        }
    }
    unmap_file(&chunks.file);
    arena_release(&arena);
    return found_weather && matches;
}

bool refresh_history_derived_resources(const Document& previous, std::string* why) {
    const bool preview_changed = previous.obj_path != g.document.obj_path ||
                                 previous.source_pc_path != g.document.source_pc_path ||
                                 previous.material_map != g.document.material_map ||
                                 previous.weapons_donor != g.document.weapons_donor;
    const bool textures_changed = previous.texture_dir != g.document.texture_dir ||
                                  previous.material_map != g.document.material_map ||
                                  previous.rain_enabled != g.document.rain_enabled;
    const bool skybox_changed = previous.source_pc_path != g.document.source_pc_path ||
                                previous.sky_texture_dir != g.document.sky_texture_dir ||
                                !same_skybox_settings(previous.skybox, g.document.skybox);

    bool ok = true;
    if (preview_changed) {
        // Preview loading enriches recovered document metadata.  Use a copy so
        // derived-resource refresh never mutates the restored history state.
        Document preview_document = g.document;
        Mesh mesh;
        std::vector<PickupModel> pickup_models;
        std::string preview_why;
        bool preview_loaded = load_document_preview(&preview_document, &mesh, &preview_why, &pickup_models);
        if (preview_loaded && !g.document.source_pc_path.empty() &&
            !g.document.weapons_donor.empty()) {
            std::vector<PickupTemplate> donor_templates;
            std::vector<PickupModel> donor_models;
            preview_loaded = load_pickup_donor(g.document.weapons_donor, &donor_templates,
                                               &donor_models, &preview_why);
            if (preview_loaded) {
                for (PickupModel& model : donor_models) {
                    const bool present = std::any_of(
                        pickup_models.begin(), pickup_models.end(), [&model](const PickupModel& existing) {
                            return existing.skin_id == model.skin_id;
                        });
                    if (!present)
                        pickup_models.push_back(std::move(model));
                }
            }
        }
        if (preview_loaded) {
            g.mesh = std::move(mesh);
            g.pickup_models = std::move(pickup_models);
        } else {
            g.mesh = {};
            g.pickup_models.clear();
            ok = false;
            if (why)
                *why = preview_why;
        }
        frame_mesh();
    } else if (textures_changed) {
        std::string texture_why;
        if (!gpu_reload_environment_textures(&texture_why)) {
            ok = false;
            if (why && why->empty())
                *why = texture_why;
        }
    }

    if (skybox_changed && !reload_skybox_preview(false)) {
        ok = false;
        if (why && why->empty())
            *why = "Skybox preview resources are unavailable for the restored state.";
    }
    return ok;
}

void finish_entity_drag_transaction() {
    if (!g.moving_entity)
        return;
    g.moving_entity = false;
    commit_history_transaction();
    if (GetCapture() == g.window)
        ReleaseCapture();
}

void refresh_after_history_restore(const Document& previous, const char* action) {
    stop_sound_preview();
    g.pending_kind = -1;
    set_single_selection_state(g.selected);
    std::string why;
    const bool resources_ready = refresh_history_derived_resources(previous, &why);
    refresh_list();
    refresh_inspector();
    update_title();
    request_redraw();
    if (resources_ready)
        set_status(action);
    else
        set_status((std::string(action) + " Preview refresh warning: " + why).c_str());
}

void command_undo() {
    finish_entity_drag_transaction();
    Document previous = g.document;
    if (!g.history.undo(&g.document, &g.selected)) {
        set_status("Nothing to undo.");
        return;
    }
    refresh_after_history_restore(previous, "Undo complete.");
}

void command_redo() {
    finish_entity_drag_transaction();
    Document previous = g.document;
    if (!g.history.redo(&g.document, &g.selected)) {
        set_status("Nothing to redo.");
        return;
    }
    refresh_after_history_restore(previous, "Redo complete.");
}

void command_copy_entity() {
    std::string why;
    if (!g.history.copy(g.document, g.selected, &why)) {
        set_status(why.c_str());
        return;
    }
    set_status("Entity copied. Paste creates an authored clone with a fresh target-valid GUID.");
}

void command_paste_entity() {
    stop_sound_preview();
    std::string why;
    if (!g.history.paste(&g.document, &g.selected, &why)) {
        set_status(why.c_str());
        return;
    }
    set_single_selection_state(g.selected);
    g.pending_kind = -1;
    refresh_list();
    refresh_inspector();
    update_title();
    request_redraw();
    set_status("Entity pasted as a new authored record.");
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
    stop_sound_preview();
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
    set_single_selection_state(g.document.entities.empty() ? -1 : 0);
    frame_mesh();
    reset_history(!g.document.dirty);
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
    if (!g.history.begin(g.document, g.selected))
        return;
    set_status("Packing environment, resources, and entities...");
    UpdateWindow(g.window);
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::string why;
    const bool ok = pack_document(g.document, path.c_str(), &why);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (!ok) {
        commit_history_transaction();
        set_status("Export failed.");
        MessageBoxA(g.window, why.c_str(), "Could not export .PC", MB_ICONERROR);
        return;
    }
    g.document.output_path = path;
    commit_history_transaction();
    set_status(g.document.source_pc_path.empty()
                   ? "Export complete: the .PC contains the environment and editor-authored entities."
                   : "Export complete: source chunks were preserved and editable PC records were updated.");
    MessageBoxA(g.window, path.c_str(), "Exported .PC", MB_ICONINFORMATION);
}

bool valid_wave_bytes(const std::vector<uint8_t>& bytes) {
    return bytes.size() >= 12 && memcmp(bytes.data(), "RIFF", 4) == 0 &&
           memcmp(bytes.data() + 8, "WAVE", 4) == 0;
}

bool load_preview_wave_file(const std::string& path, std::vector<uint8_t>* bytes, std::string* why) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        if (why)
            *why = "Could not open the selected WAV file.";
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 12 || size > static_cast<std::streamoff>(512 * MiB)) {
        if (why)
            *why = "The selected WAV file has an invalid size.";
        return false;
    }
    std::vector<uint8_t> next(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(next.data()), static_cast<std::streamsize>(size)) ||
        !valid_wave_bytes(next)) {
        if (why)
            *why = "The selected sound is not a raw RIFF/WAVE resource.";
        return false;
    }
    *bytes = std::move(next);
    return true;
}

bool load_embedded_preview_wave(const Entity& entity, std::vector<uint8_t>* bytes, std::string* why) {
    if (g.document.source_pc_path.empty() || !entity.sound_source_record) {
        if (why)
            *why = "This sound has no local WAV or imported embedded resource.";
        return false;
    }
    Arena arena{};
    Error err{};
    ChunkList chunks{};
    bool ok = arena_init(&arena, 8 * MiB, &err) &&
              parse_chunks(g.document.source_pc_path.c_str(), &chunks, &arena, &err);
    if (ok) {
        ok = false;
        for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
            RscfInfo resource{};
            if (!rscf_info(chunks.chunks[chunk_index], &resource) ||
                resource.type != ASURA_RESOURCEFILE_TYPE_SOUND ||
                resource.subtype != entity.sound_phonon.m_uSoundResourceID)
                continue;
            bytes->assign(resource.payload, resource.payload + resource.payload_size);
            ok = valid_wave_bytes(*bytes);
            if (!ok)
                fail(&err, "embedded sound resource %u is not raw RIFF/WAVE data",
                     entity.sound_phonon.m_uSoundResourceID);
            break;
        }
        if (!ok && !err.set)
            fail(&err, "embedded sound resource %u was not found", entity.sound_phonon.m_uSoundResourceID);
    }
    unmap_file(&chunks.file);
    arena_release(&arena);
    if (!ok && why)
        *why = err.set ? err.message : "Could not read the embedded sound resource.";
    return ok;
}

void command_sound_preview() {
    if (g.selected < 0 || g.selected >= static_cast<int>(g.document.entities.size()) ||
        g.document.entities[g.selected].kind != EntityKind::Sound)
        return;
    if (g.sound_preview_entity == g.selected) {
        stop_sound_preview();
        set_status("Sound preview stopped.");
        return;
    }

    const Entity& entity = g.document.entities[g.selected];
    std::vector<uint8_t> bytes;
    std::string why;
    bool loaded = !entity.sound_file.empty() && load_preview_wave_file(entity.sound_file, &bytes, &why);
    bool using_embedded = false;
    if (!loaded && entity.sound_source_record) {
        loaded = load_embedded_preview_wave(entity, &bytes, &why);
        using_embedded = loaded;
    }
    if (!loaded) {
        MessageBoxA(g.window, why.c_str(), "Sound preview unavailable", MB_ICONWARNING);
        return;
    }

    stop_sound_preview();
    g.sound_preview_bytes = std::move(bytes);
    // Preview once even when the in-game repeat flag is set; the same button
    // remains available as an explicit stop control while playback is active.
    const DWORD flags = SND_MEMORY | SND_ASYNC | SND_NODEFAULT;
    if (!PlaySoundA(reinterpret_cast<LPCSTR>(g.sound_preview_bytes.data()), nullptr, flags)) {
        g.sound_preview_bytes.clear();
        MessageBoxA(g.window, "Windows could not play this WAV resource.", "Sound preview unavailable",
                    MB_ICONWARNING);
        return;
    }
    g.sound_preview_entity = g.selected;
    SetWindowTextA(g.sound_preview, "Stop preview");
    set_status(using_embedded ? "Playing embedded sound resource." : "Playing local WAV resource.");
}

void command_browse_sound() {
    if (g.selected < 0 || g.selected >= static_cast<int>(g.document.entities.size()))
        return;
    Entity& e = g.document.entities[g.selected];
    if (e.kind != EntityKind::Sound)
        return;
    stop_sound_preview();
    std::string path = e.sound_file;
    if (!choose_path(g.window, false, "Choose sound resource", "Wave audio\0*.wav\0All files\0*.*\0", "wav", &path))
        return;
    if (!g.history.begin(g.document, g.selected))
        return;
    e.sound_file = path;
    e.sound_name = "sounds\\" + basename_without_extension(path);
    e.name = basename_without_extension(path);
    commit_history_transaction();
    refresh_list();
    refresh_inspector();
    set_status(e.sound_name.c_str());
}

void command_export_material_map() {
    if (g.document.source_pc_path.empty()) {
        MessageBoxA(g.window, "You can't export material map out of a custom level", "Cannot export material map", MB_ICONERROR);
        return;
    }
    std::string path;
    if (!choose_path(g.window, true, "Save material map as", "Material map JSON\0*.json\0All files\0*.*\0", "json", &path))
        return;

    Error err{};
    Arena arena{};
    ChunkList chunks{};
    std::vector<PcEnvironmentMaterialBinding> materials;
    bool ok = arena_init(&arena, 8 * MiB, &err) &&
              parse_chunks(g.document.source_pc_path.c_str(), &chunks, &arena, &err) &&
              pc_environment_material_bindings(chunks, &materials, &err);

    std::string out = "{\n";
    if (ok && !materials.empty()) {
        out += "  \"texture_by_material_index\": {\n";
        for (size_t i = 0; i < materials.size(); ++i) {
            std::string tname(materials[i].texture_name.data, materials[i].texture_name.size);
            std::string escaped;
            for (char c : tname) {
                if (c == '\\' || c == '"') escaped += '\\';
                escaped += c;
            }
            char line[1024];
            snprintf(line, sizeof(line), "    \"%zu\": \"%s\"%s\n", i, escaped.c_str(), i + 1 == materials.size() ? "" : ",");
            out += line;
        }
        out += "  },\n";

        out += "  \"transparency_flag_by_material_index\": {\n";
        for (size_t i = 0; i < materials.size(); ++i) {
            char line[256];
            snprintf(line, sizeof(line), "    \"%zu\": %u%s\n", i, materials[i].flags, i + 1 == materials.size() ? "" : ",");
            out += line;
        }
        out += "  },\n";

        out += "  \"surface_type_by_material_index\": {\n";
        for (size_t i = 0; i < materials.size(); ++i) {
            char line[256];
            snprintf(line, sizeof(line), "    \"%zu\": %u%s\n", i, materials[i].surface_type, i + 1 == materials.size() ? "" : ",");
            out += line;
        }
        out += "  }\n";
    }
    unmap_file(&chunks.file);
    arena_release(&arena);

    out += "}\n";

    if (!write_entire_file(path.c_str(), out.c_str(), out.size(), nullptr)) {
        MessageBoxA(g.window, "Failed to write file.", "Error", MB_ICONERROR);
        return;
    }

    if (!g.history.begin(g.document, g.selected))
        return;
    g.document.material_map = path;
    std::string why;
    if (!g.document.obj_path.empty()) {
        Mesh rebuilt;
        if (load_preview_mesh(g.document.obj_path, path, &rebuilt, &why)) {
            g.mesh = std::move(rebuilt);
            g.environment_raycast.build(g.mesh);
            invalidate_environment_cache();
            gpu_upload_mesh();
        }
    } else {
        gpu_reload_environment_textures(&why);
    }
    commit_history_transaction();
    request_redraw();
    set_status(why.empty() ? "Material map exported and applied." : why.c_str());
}

void command_material_map() {
    std::string path = g.document.material_map;
    if (!choose_path(g.window, false, "Choose material map", "Material map JSON\0*.json\0All files\0*.*\0", "json", &path))
        return;
    Mesh rebuilt;
    std::string why;
    if (!g.document.obj_path.empty() && !load_preview_mesh(g.document.obj_path, path, &rebuilt, &why)) {
        MessageBoxA(g.window, why.c_str(), "Could not apply material map", MB_ICONERROR);
        return;
    }
    if (!g.history.begin(g.document, g.selected))
        return;
    g.document.material_map = path;
    if (!g.document.obj_path.empty()) {
        g.mesh = std::move(rebuilt);
        g.environment_raycast.build(g.mesh);
        invalidate_environment_cache();
        gpu_upload_mesh();
    } else {
        gpu_reload_environment_textures(&why);
    }
    commit_history_transaction();
    request_redraw();
    set_status(why.empty() ? "Material map applied to the environment preview." : why.c_str());
}

void command_texture_dir() {
    std::string path = g.document.texture_dir.empty() ? folder_from_path(g.document.obj_path) : g.document.texture_dir;
    if (!choose_directory(g.window, "Choose texture folder", &path))
        return;
    if (!g.history.begin(g.document, g.selected))
        return;
    g.document.texture_dir = path;
    uint32_t loaded = 0, missing = 0;
    std::string why;
    gpu_reload_environment_textures(&why, &loaded, &missing);
    commit_history_transaction();
    request_redraw();
    char status[256]{};
    snprintf(status, sizeof(status), "Environment textures: %u loaded, %u using diffuse fallback.%s%s",
             loaded, missing, why.empty() ? "" : " ", why.c_str());
    set_status(status);
}

void command_toggle_rain() {
    if (g.document.source_pc_path.empty() && g.document.obj_path.empty())
        return;
    const bool enabled = SendMessageA(g.rain_toggle, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (enabled == g.document.rain_enabled)
        return;
    if (!g.history.begin(g.document, g.selected)) {
        SendMessageA(g.rain_toggle, BM_SETCHECK,
                     g.document.rain_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        return;
    }
    g.document.rain_enabled = enabled;
    std::string why;
    const bool preview_ready = gpu_reload_environment_textures(&why);
    if (g.document.source_pc_path.empty())
        gpu.environment_wet_weather = enabled;
    commit_history_transaction();
    invalidate_environment_cache();
    request_redraw();
    if (!preview_ready || !why.empty())
        set_status((std::string(enabled ? "Rain enabled. " : "Rain disabled. ") + why).c_str());
    else
        set_status(enabled ? "WTHR rain enabled." : "WTHR rain disabled.");
}

enum AmbiencePropertiesId : int {
    ID_AMBIENCE_STREAM = 3400,
    ID_AMBIENCE_VOLUME,
};

struct AmbiencePropertiesState {
    HWND window = nullptr;
    HWND stream = nullptr;
    HWND volume = nullptr;
    std::vector<std::string> paths;
    std::string selected_path;
    float selected_volume = 1.0f;
    bool accepted = false;
};

constexpr const char* kAmbienceSoundNames[] = {
    "01_temp.wav",
    "02_temp.wav",
    "indoor_s\\m1_kar1i.wav",
    "indoor_s\\m1_roof.wav",
    "indoor_s\\m2_bra1i.wav",
    "indoor_s\\m2_bra1r.wav",
    "indoor_s\\m3_pla5i.wav",
    "indoor_s\\m4_anh3i.wav",
    "indoor_s\\m4_entr.wav",
    "indoor_s\\m4_roof.wav",
    "indoor_s\\m4_tube.wav",
    "indoor_s\\m5_bor3i.wav",
    "indoor_s\\m5_bor4i.wav",
    "indoor_s\\m5_roof.wav",
    "indoor_s\\m5_room.wav",
    "indoor_s\\m7_kei1i.wav",
    "indoor_s\\m8_air1i.wav",
    "indoor_s\\m8_contr.wav",
    "m1_karl1.wav",
    "m1_karl2.wav",
    "m10_Hofe.wav",
    "m11_bunk.wav",
    "m2_bran1.wav",
    "m2_bran2.wav",
    "m2_bran3.wav",
    "m2_bran4.wav",
    "m2_bran5.wav",
    "m3_play1.wav",
    "m3_play2.wav",
    "m3_play3.wav",
    "m3_play4.wav",
    "m3_play5.wav",
    "m4_anh1.wav",
    "m4_anh2.wav",
    "m4_anh3.wav",
    "m4_anh3o.wav",
    "m5_bor1.wav",
    "m5_bor2.wav",
    "m5_bor3o.wav",
    "m5_bor4o.wav",
    "m5_bor5.wav",
    "m5_bor6.wav",
    "m6_schl1.wav",
    "m6_schl2.wav",
    "m6_schl3.wav",
    "m6_schl4.wav",
    "m6_schl5.wav",
    "m7_kei1o.wav",
    "m7_kei2o.wav",
    "m7_kei3o.wav",
    "m8_air1o.wav",
    "m8_air2o.wav",
    "m8_air3o.wav",
    "m8_air4o.wav"
};

std::vector<std::string> available_ambience_streams() {
    std::vector<std::string> paths;
    paths.reserve(sizeof(kAmbienceSoundNames) / sizeof(kAmbienceSoundNames[0]));
    for (const char* name : kAmbienceSoundNames)
        paths.emplace_back(std::string("Sounds\\Streams\\") + name);
    return paths;
}

std::string ambience_stream_label(const std::string& path) {
    constexpr const char prefix[] = "Sounds\\Streams\\";
    size_t start = 0;
    while (start < path.size() && (path[start] == '\\' || path[start] == '/'))
        ++start;
    if (path.size() - start >= sizeof(prefix) - 1 &&
        _strnicmp(path.c_str() + start, prefix, sizeof(prefix) - 1) == 0)
        start += sizeof(prefix) - 1;
    size_t end = path.size();
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && dot >= start)
        end = dot;
    std::string label = path.substr(start, end - start);
    for (char& c : label)
        if (c == '/')
            c = '\\';
    return label;
}

HWND make_ambience_control(AmbiencePropertiesState* state, const char* cls, const char* text,
                           DWORD style, int id, int x, int y, int width, int height) {
    HWND control = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                                   state->window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandle(nullptr), nullptr);
    SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return control;
}

void create_ambience_properties_controls(AmbiencePropertiesState* state) {
    make_ambience_control(state, "STATIC", "Default stream", SS_LEFT, -1, 18, 22, 104, 22);
    state->stream = make_ambience_control(state, "COMBOBOX", "",
                                          CBS_DROPDOWNLIST | CBS_AUTOHSCROLL | WS_VSCROLL,
                                          ID_AMBIENCE_STREAM, 126, 18, 380, 260);
    make_ambience_control(state, "STATIC", "Volume (0-1)", SS_LEFT, -1, 18, 62, 104, 22);
    state->volume = make_ambience_control(state, "EDIT", "",
                                          ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                          ID_AMBIENCE_VOLUME, 126, 58, 100, 24);
    make_ambience_control(
        state, "STATIC",
        "Ambience sound names come from game's root Sounds\\Streams",
        SS_LEFT, -1, 18, 98, 355, 17);
    make_ambience_control(state, "BUTTON", "OK", BS_DEFPUSHBUTTON | WS_TABSTOP,
                          IDOK, 290, 158, 104, 30);
    make_ambience_control(state, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP,
                          IDCANCEL, 402, 158, 104, 30);

    SendMessageA(state->stream, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>("(None - No ambience sound)"));
    int selected = state->selected_path.empty() ? 0 : -1;
    for (size_t index = 0; index < state->paths.size(); ++index) {
        const std::string label = ambience_stream_label(state->paths[index]);
        const LRESULT row = SendMessageA(state->stream, CB_ADDSTRING, 0,
                                         reinterpret_cast<LPARAM>(label.c_str()));
        if (row != CB_ERR && row != CB_ERRSPACE &&
            _stricmp(state->paths[index].c_str(), state->selected_path.c_str()) == 0)
            selected = static_cast<int>(row);
    }
    SendMessageA(state->stream, CB_SETCURSEL, selected >= 0 ? selected : 0, 0);
    set_float(state->volume, state->selected_volume);
}

LRESULT CALLBACK ambience_properties_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTA* create = reinterpret_cast<const CREATESTRUCTA*>(lparam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    AmbiencePropertiesState* state =
        reinterpret_cast<AmbiencePropertiesState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE:
        state->window = hwnd;
        create_ambience_properties_controls(state);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            const int selection = static_cast<int>(SendMessageA(state->stream, CB_GETCURSEL, 0, 0));
            const float volume = get_float(state->volume, state->selected_volume);
            if (selection < 0 || selection > static_cast<int>(state->paths.size()) ||
                !isfinite(volume) || volume < 0.0f || volume > 1.0f) {
                MessageBoxA(hwnd, "Choose a stream and enter a volume from 0 to 1.",
                            "Invalid ambience settings", MB_ICONWARNING);
                return 0;
            }
            state->selected_path = selection == 0 ? std::string{} : state->paths[selection - 1];
            state->selected_volume = volume;
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

void command_ambience_properties() {
    if (g.document.source_pc_path.empty() && g.document.obj_path.empty())
        return;
    AmbiencePropertiesState state{};
    state.selected_path = g.document.ambient_stream_path;
    state.selected_volume = g.document.ambient_volume;
    state.paths = available_ambience_streams();
    bool current_present = state.selected_path.empty();
    for (const std::string& path : state.paths)
        current_present |= _stricmp(path.c_str(), state.selected_path.c_str()) == 0;
    if (!current_present)
        state.paths.push_back(state.selected_path);

    RECT owner{};
    GetWindowRect(g.window, &owner);
    constexpr int width = 540, height = 235;
    const int x = owner.left + std::max(0L, (owner.right - owner.left - width) / 2);
    const int y = owner.top + std::max(0L, (owner.bottom - owner.top - height) / 2);
    HWND window = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                  "Asura2005AmbienceProperties", "Streaming ambience",
                                  WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
                                  x, y, width, height, g.window, nullptr,
                                  GetModuleHandle(nullptr), &state);
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
    if (!g.history.begin(g.document, g.selected))
        return;
    g.document.ambient_stream_path = std::move(state.selected_path);
    g.document.ambient_volume = state.selected_volume;
    commit_history_transaction();
    if (g.document.ambient_stream_path.empty()) {
        set_status("Default streaming ambience disabled");
    } else {
        const std::string label = ambience_stream_label(g.document.ambient_stream_path);
        char status[320]{};
        snprintf(status, sizeof(status), "Streaming ambience: %s at %.3g",
                 label.c_str(), g.document.ambient_volume);
        set_status(status);
    }
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
    if (!g.history.begin(g.document, g.selected))
        return;
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
    commit_history_transaction();
    char status[220]{};
    snprintf(status, sizeof(status), "Weapons donor loaded: %zu pickup item definitions, %zu rendered models.",
             templates.size(), g.pickup_models.size());
    set_status(status);
    refresh_list();
    refresh_inspector();
    request_redraw();
}

enum SkyboxPropertiesId : int {
    ID_SKYBOX_RED = 3300,
    ID_SKYBOX_GREEN,
    ID_SKYBOX_BLUE,
    ID_SKYBOX_ORIENTATION,
    ID_SKYBOX_PATH_FIRST,
    ID_SKYBOX_PATH_LAST = ID_SKYBOX_PATH_FIRST + ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT - 1,
    ID_SKYBOX_DRAW_CLOUDS,
    ID_SKYBOX_VERSION_6,
    ID_SKYBOX_VERSION_7,
    ID_SKYBOX_BACK_FLIPPED,
    ID_SKYBOX_RIGHT_FLIPPED,
    ID_SKYBOX_PREVIEW_FOLDER,
    ID_SKYBOX_USE_EMBEDDED,
};

struct SkyboxPropertiesState {
    HWND window = nullptr;
    HWND red = nullptr;
    HWND green = nullptr;
    HWND blue = nullptr;
    HWND orientation = nullptr;
    HWND paths[ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT]{};
    HWND draw_clouds = nullptr;
    HWND version_6 = nullptr;
    HWND version_7 = nullptr;
    HWND back_flipped = nullptr;
    HWND right_flipped = nullptr;
    HWND preview_folder = nullptr;
    SkyboxSettings value;
    std::string texture_directory;
    bool accepted = false;
};

HWND make_skybox_control(SkyboxPropertiesState* state, const char* cls, const char* text, DWORD style,
                         int id, int x, int y, int width, int height) {
    HWND control = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                                   state->window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandle(nullptr), nullptr);
    SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    return control;
}

void refresh_skybox_folder_text(SkyboxPropertiesState* state) {
    SetWindowTextA(state->preview_folder,
                   state->texture_directory.empty() ? "Embedded/source resources" : state->texture_directory.c_str());
}

void refresh_skybox_version_controls(SkyboxPropertiesState* state) {
    const bool version_7 =
        SendMessageA(state->version_7, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(state->right_flipped, version_7);
    if (!version_7)
        SendMessageA(state->right_flipped, BM_SETCHECK, BST_UNCHECKED, 0);
}

void create_skybox_properties_controls(SkyboxPropertiesState* state) {
    make_skybox_control(state, "STATIC", "RGB tint", SS_LEFT, 0, 14, 19, 82, 22);
    constexpr const char* colour_labels[] = {"R", "G", "B"};
    HWND* colours[] = {&state->red, &state->green, &state->blue};
    constexpr int colour_ids[] = {ID_SKYBOX_RED, ID_SKYBOX_GREEN, ID_SKYBOX_BLUE};
    for (int i = 0; i < 3; ++i) {
        const int x = 100 + i * 150;
        make_skybox_control(state, "STATIC", colour_labels[i], SS_LEFT, 0, x, 19, 18, 22);
        *colours[i] = make_skybox_control(state, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                          colour_ids[i], x + 20, 16, 118, 24);
    }
    make_skybox_control(state, "STATIC", "Orientation (radians)", SS_LEFT, 0, 556, 19, 126, 22);
    state->orientation = make_skybox_control(state, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                             ID_SKYBOX_ORIENTATION, 682, 16, 112, 24);

    constexpr const char* path_labels[] = {"Path 0 (lower)", "Path 1 (front)", "Path 2 (left)",
                                            "Path 3 (back)", "Path 4 (right)", "Path 5 (upper)",
                                            "Path 6 (cloud A)", "Path 7 (cloud B)"};
    for (int i = 0; i < static_cast<int>(_countof(path_labels)); ++i) {
        const int y = 58 + i * 36;
        make_skybox_control(state, "STATIC", path_labels[i], SS_LEFT, 0, 14, y + 3, 112, 22);
        state->paths[i] = make_skybox_control(state, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                              ID_SKYBOX_PATH_FIRST + i, 130, y, 664, 24);
        SendMessageA(state->paths[i], EM_SETLIMITTEXT, 4096, 0);
    }

    state->draw_clouds = make_skybox_control(state, "BUTTON", "Draw clouds", BS_AUTOCHECKBOX | WS_TABSTOP,
                                              ID_SKYBOX_DRAW_CLOUDS, 14, 356, 128, 24);
    state->version_6 = make_skybox_control(state, "BUTTON", "SKYB format version 6",
                                            BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
                                            ID_SKYBOX_VERSION_6, 158, 356, 166, 24);
    state->version_7 = make_skybox_control(state, "BUTTON", "SKYB format version 7",
                                            BS_AUTORADIOBUTTON | WS_TABSTOP,
                                            ID_SKYBOX_VERSION_7, 330, 356, 204, 24);
    state->back_flipped = make_skybox_control(
        state, "BUTTON", "Back texture is front upside down", BS_AUTOCHECKBOX | WS_TABSTOP,
        ID_SKYBOX_BACK_FLIPPED, 14, 386, 332, 24);
    state->right_flipped = make_skybox_control(
        state, "BUTTON", "Right texture is left upside down", BS_AUTOCHECKBOX | WS_TABSTOP,
        ID_SKYBOX_RIGHT_FLIPPED, 366, 386, 346, 24);
    make_skybox_control(state, "STATIC",
                        "Both target formats render the same cube; v7 only adds the right-face mapping flag.",
                        SS_LEFT, 0, 14, 414, 760, 20);
    make_skybox_control(state, "STATIC", "Preview resources", SS_LEFT, 0, 14, 438, 112, 22);
    state->preview_folder = make_skybox_control(state, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | ES_READONLY,
                                                0, 130, 435, 414, 24);
    make_skybox_control(state, "BUTTON", "Choose folder...", BS_PUSHBUTTON | WS_TABSTOP,
                        ID_SKYBOX_PREVIEW_FOLDER, 552, 434, 116, 27);
    make_skybox_control(state, "BUTTON", "Use embedded", BS_PUSHBUTTON | WS_TABSTOP,
                        ID_SKYBOX_USE_EMBEDDED, 676, 434, 118, 27);
    make_skybox_control(state, "BUTTON", "Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 566, 473, 104, 30);
    make_skybox_control(state, "BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 682, 473, 112, 30);

    set_float(state->red, state->value.red);
    set_float(state->green, state->value.green);
    set_float(state->blue, state->value.blue);
    set_float(state->orientation, state->value.orientation_radians);
    for (int i = 0; i < static_cast<int>(_countof(state->paths)); ++i)
        SetWindowTextA(state->paths[i], state->value.texture_paths[i].c_str());
    SendMessageA(state->draw_clouds, BM_SETCHECK, state->value.draw_clouds ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(state->version_6, BM_SETCHECK,
                 state->value.chunk_version == 6 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(state->version_7, BM_SETCHECK,
                 state->value.chunk_version == 7 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(state->back_flipped, BM_SETCHECK,
                 state->value.back_texture_is_front_upside_down ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(state->right_flipped, BM_SETCHECK,
                 state->value.right_texture_is_left_upside_down ? BST_CHECKED : BST_UNCHECKED, 0);
    refresh_skybox_version_controls(state);
    refresh_skybox_folder_text(state);
}

void apply_skybox_properties(SkyboxPropertiesState* state) {
    state->value.red = get_float(state->red, state->value.red);
    state->value.green = get_float(state->green, state->value.green);
    state->value.blue = get_float(state->blue, state->value.blue);
    state->value.orientation_radians = get_float(state->orientation, state->value.orientation_radians);
    for (int i = 0; i < static_cast<int>(_countof(state->paths)); ++i) {
        char path[4097]{};
        GetWindowTextA(state->paths[i], path, sizeof(path));
        state->value.texture_paths[i] = path;
    }
    state->value.draw_clouds = SendMessageA(state->draw_clouds, BM_GETCHECK, 0, 0) == BST_CHECKED;
    state->value.chunk_version =
        SendMessageA(state->version_6, BM_GETCHECK, 0, 0) == BST_CHECKED ? 6u : 7u;
    state->value.back_texture_is_front_upside_down =
        SendMessageA(state->back_flipped, BM_GETCHECK, 0, 0) == BST_CHECKED;
    state->value.right_texture_is_left_upside_down =
        state->value.chunk_version == 7 &&
        SendMessageA(state->right_flipped, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

LRESULT CALLBACK skybox_properties_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTA* create = reinterpret_cast<const CREATESTRUCTA*>(lparam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    SkyboxPropertiesState* state =
        reinterpret_cast<SkyboxPropertiesState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE:
        state->window = hwnd;
        create_skybox_properties_controls(state);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_SKYBOX_VERSION_6 ||
            LOWORD(wparam) == ID_SKYBOX_VERSION_7) {
            refresh_skybox_version_controls(state);
        } else if (LOWORD(wparam) == ID_SKYBOX_PREVIEW_FOLDER) {
            std::string path = state->texture_directory;
            if (choose_directory(hwnd, "Choose skybox texture folder", &path)) {
                state->texture_directory = std::move(path);
                refresh_skybox_folder_text(state);
            }
        } else if (LOWORD(wparam) == ID_SKYBOX_USE_EMBEDDED) {
            state->texture_directory.clear();
            refresh_skybox_folder_text(state);
        } else if (LOWORD(wparam) == IDOK) {
            apply_skybox_properties(state);
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

void command_skybox_textures() {
    SkyboxPropertiesState state{};
    state.value = g.document.skybox;
    state.texture_directory = g.document.sky_texture_dir;
    RECT owner{};
    GetWindowRect(g.window, &owner);
    constexpr int width = 830, height = 565;
    const int x = owner.left + std::max(0L, (owner.right - owner.left - width) / 2);
    const int y = owner.top + std::max(0L, (owner.bottom - owner.top - height) / 2);
    HWND window = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                  "Asura2005SkyboxProperties", "SKYB version and properties",
                                  WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE, x, y, width, height,
                                  g.window, nullptr, GetModuleHandle(nullptr), &state);
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

    if (!g.history.begin(g.document, g.selected))
        return;
    g.document.skybox = std::move(state.value);
    g.document.sky_texture_dir = std::move(state.texture_directory);
    commit_history_transaction();
    const bool loaded = reload_skybox_preview(true);
    if (loaded) {
        gpu_rebuild_skybox_vertices(g.document.skybox.orientation_radians,
                                    g.document.skybox.back_texture_is_front_upside_down,
                                    g.document.skybox.right_texture_is_left_upside_down, nullptr);
        gpu.skybox_tint = {std::clamp(g.document.skybox.red / 255.0f, 0.0f, 1.0f),
                           std::clamp(g.document.skybox.green / 255.0f, 0.0f, 1.0f),
                           std::clamp(g.document.skybox.blue / 255.0f, 0.0f, 1.0f), 1.0f};
        request_redraw();
    }
    set_status(loaded ? "SKYB version and properties applied."
                      : "SKYB version and properties saved; preview unavailable.");
}

void delete_selected() {
    normalize_selection_state();
    if (g.selected_entities.empty())
        return;
    for (int index : g.selected_entities) {
        if (g.document.entities[index].source_entity_record &&
            g.document.entities[index].kind != EntityKind::PhysicalObject) {
            set_status("Imported target/marker records remain source-preserved and cannot be deleted yet.");
            return;
        }
    }
    stop_sound_preview();
    if (!g.history.begin(g.document, g.selected))
        return;
    std::vector<int> targets = g.selected_entities;
    std::sort(targets.begin(), targets.end());
    const int next_index = targets.front();
    for (auto it = targets.rbegin(); it != targets.rend(); ++it)
        g.document.entities.erase(g.document.entities.begin() + *it);
    set_single_selection_state(std::min(next_index, static_cast<int>(g.document.entities.size()) - 1));
    commit_history_transaction();
    refresh_list();
    refresh_inspector();
    request_redraw();
    if (targets.size() > 1) {
        char status[96]{};
        snprintf(status, sizeof(status), "Deleted %zu selected entities.", targets.size());
        set_status(status);
    }
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
        reset_history(true);
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
        } else if (wparam == 2 &&
                   ((g.document.skybox.draw_clouds && gpu.skybox_cloud) ||
                    (g.document.rain_enabled && gpu.rain_texture))) {
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
        record_inspector_edit(id, HIWORD(wparam));
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
        else if (id == ID_EXPORT_MATERIAL_MAP)
            command_export_material_map();
        else if (id == ID_TEXTURE_DIR)
            command_texture_dir();
        else if (id == ID_WEAPONS_DONOR)
            command_weapons_donor();
        else if (id == ID_SKYBOX_TEXTURES)
            command_skybox_textures();
        else if (id == ID_TOGGLE_RAIN)
            command_toggle_rain();
        else if (id == ID_AMBIENCE_PROPERTIES)
            command_ambience_properties();
        else if (id == ID_ADD_SPAWN)
            begin_place(EntityKind::SpawnPoint);
        else if (id == ID_ADD_LIGHT)
            begin_place(EntityKind::Light);
        else if (id == ID_ADD_SOUND)
            begin_place(EntityKind::Sound);
        else if (id == ID_ADD_PICKUP)
            begin_place(EntityKind::PhysicalObject);
        else if (id == ID_UNDO)
            command_undo();
        else if (id == ID_REDO)
            command_redo();
        else if (id == ID_COPY_ENTITY)
            command_copy_entity();
        else if (id == ID_PASTE_ENTITY)
            command_paste_entity();
        else if (id == ID_DELETE_ENTITY)
            delete_selected();
        else if (id == ID_APPLY_INSPECTOR)
            apply_inspector();
        else if (id == ID_BROWSE_SOUND)
            command_browse_sound();
        else if (id == ID_SOUND_PREVIEW)
            command_sound_preview();
        else if (id == ID_LIGHT_PROPERTIES)
            command_light_properties();
        else if (id == ID_ENTITY_LIST) {
            const int notification = HIWORD(wparam);
            if (notification == LBN_SELCHANGE)
                select_entities_from_list();
            else if (notification == LBN_DBLCLK) {
                select_entities_from_list();
                focus_camera_on_entity(g.selected);
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const RECT vr = viewport_rect();
        if (!PtInRect(&vr, p))
            break;
        SetFocus(hwnd);
        Asura_Vector_3 world{};
        if (g.pending_kind >= 0) {
            if (environment_point_from_screen(p.x, p.y, &world))
                add_entity_at(static_cast<EntityKind>(g.pending_kind), world);
            else
                set_status("No environment geometry is under the cursor; placement remains active.");
            return 0;
        }
        const int hit = hit_entity(p.x, p.y);
        if ((GetKeyState(VK_CONTROL) & 0x8000) && hit >= 0) {
            toggle_entity_selection(hit);
            return 0;
        }
        select_entity(hit);
        if (hit >= 0 && g.history.begin(g.document, g.selected)) {
            g.moving_entity = true;
            g.entity_drag_last_mouse = p;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        finish_entity_drag_transaction();
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
    case WM_CAPTURECHANGED:
        // A modal dialog or another window can steal capture before a button-up
        // arrives. Never let that leave a latent drag that moves on the next
        // unrelated mouse event.
        if (reinterpret_cast<HWND>(lparam) != hwnd) {
            if (g.moving_entity)
                finish_entity_drag_transaction();
            g.moving_entity = false;
            g.orbiting = false;
            g.panning = false;
            g.fast_preview = false;
            invalidate_environment_cache();
            request_redraw();
        }
        return 0;
    case WM_MOUSEMOVE: {
        const POINT now{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (g.moving_entity && g.selected >= 0) {
            if (now.x == g.entity_drag_last_mouse.x && now.y == g.entity_drag_last_mouse.y)
                return 0;
            g.entity_drag_last_mouse = now;
            Asura_Vector_3 p{};
            if (environment_point_from_screen(now.x, now.y, &p)) {
                Entity& e = g.document.entities[g.selected];
                if (e.position.x == p.x && e.position.y == p.y && e.position.z == p.z)
                    return 0;
                e.position = p;
                if (e.kind == EntityKind::Light)
                    e.light.Position = e.position;
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
            if (!confirm_discard())
                return 0;
            std::string why;
            Document doc;
            if (load_project(&doc, p.c_str(), &why)) {
                stop_sound_preview();
                g.document = std::move(doc);
                set_single_selection_state(g.document.entities.empty() ? -1 : 0);
                g.pending_kind = -1;
                const bool skybox_loaded = reload_skybox_preview(true);
                load_document_preview(&g.document, &g.mesh, &why, &g.pickup_models);
                frame_mesh();
                reset_history(!g.document.dirty);
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
        stop_sound_preview();
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

bool entity_gizmo_smoke() {
    Entity light;
    light.kind = EntityKind::Light;
    light.light.Range = 0.0f;
    light.light.m_uFlags = ASURA_LIGHT_FLAG_USE_BOUNDING_BOX;
    light.light.m_xBoundingBox = {-1, 2, -3, 4, -5, 6};
    std::vector<LightGizmoLine> lines;
    append_light_gizmo(light, &lines);
    if (lines.size() != kLightBoundingBoxLines)
        return false;
    light.light.m_uFlags = 0;
    lines.clear();
    append_light_gizmo(light, &lines);
    if (!lines.empty())
        return false;

    Entity spawn;
    spawn.kind = EntityKind::SpawnPoint;
    spawn.position = {1, 2, 3};
    spawn.spawn_direction = {0, 1, 0};
    spawn.value_u32_a = SnipeSpawnTeam_Camera;
    append_camera_spawn_arrow(spawn, .5f, &lines);
    if (lines.size() != kCameraSpawnArrowLines ||
        !nearly_equal(lines[0].a, Asura_Vector_3{1, -2, 3}) ||
        !nearly_equal(lines[0].b, Asura_Vector_3{1, -4, 3}))
        return false;
    lines.clear();
    spawn.value_u32_a = SnipeSpawnTeam_Deathmatch;
    append_camera_spawn_arrow(spawn, .5f, &lines);
    return lines.empty();
}

bool pc_material_map_roundtrip_smoke(const char* source_path, const char* material_map_path,
                                     const char* output_path) {
    struct ExpectedMaterial {
        std::string texture_name;
        uint32_t flags = 0;
        uint32_t texture_flags = 0;
        uint32_t surface_type = 0;
    };

    Error err{};
    Arena arena{};
    ChunkList source{};
    MaterialMap material_map{};
    Config config{};
    config.material_map = material_map_path;
    std::vector<PcEnvironmentMaterialBinding> source_materials;
    std::vector<ExpectedMaterial> expected;
    uint32_t source_text_chunk = 0xffffffffu;
    uint32_t source_material_chunk = 0xffffffffu;
    bool ok = arena_init(&arena, 64 * MiB, &err) &&
              parse_chunks(source_path, &source, &arena, &err) &&
              load_material_map(config, &material_map, &arena, &err) &&
              pc_environment_material_bindings(source, &source_materials, &err) &&
              pc_environment_material_chunk_indices(source, &source_text_chunk,
                                                    &source_material_chunk, &err) &&
              !source_materials.empty();
    if (ok) {
        expected.resize(source_materials.size());
        for (uint32_t i = 0; i < static_cast<uint32_t>(source_materials.size()); ++i) {
            const Str mapped_name = material_texture_name(material_map, i);
            const Str expected_name = mapped_name.size ? mapped_name : source_materials[i].texture_name;
            expected[i].texture_name.assign(expected_name.data ? expected_name.data : "", expected_name.size);
            expected[i].flags = material_override(
                material_map, "transparency_flag_by_material_index", i, source_materials[i].flags);
            expected[i].texture_flags = source_materials[i].texture_flags;
            expected[i].surface_type = material_override(
                material_map, "surface_type_by_material_index", i, source_materials[i].surface_type);
        }
    }
    unmap_file(&material_map.file);
    unmap_file(&source.file);
    arena_release(&arena);

    Document document;
    Mesh mesh;
    std::string why;
    if (!ok || !load_pc_level(source_path, &document, &mesh, &why))
        return false;
    document.material_map = material_map_path;
    if (!pack_document(document, output_path, &why))
        return false;

    Arena output_arena{};
    ChunkList output{};
    std::vector<PcEnvironmentMaterialBinding> actual;
    uint32_t actual_text_chunk = 0xffffffffu;
    uint32_t actual_material_chunk = 0xffffffffu;
    ok = arena_init(&output_arena, 64 * MiB, &err) &&
         parse_chunks(output_path, &output, &output_arena, &err) &&
         pc_environment_material_bindings(output, &actual, &err) &&
         pc_environment_material_chunk_indices(output, &actual_text_chunk,
                                               &actual_material_chunk, &err) &&
         actual.size() == expected.size() && actual_text_chunk == source_text_chunk &&
         (source_material_chunk == 0xffffffffu || actual_material_chunk == source_material_chunk);
    for (uint32_t i = 0; ok && i < static_cast<uint32_t>(actual.size()); ++i) {
        const Str actual_name = actual[i].texture_name;
        ok = actual_name.size == expected[i].texture_name.size() &&
             (!actual_name.size ||
              memcmp(actual_name.data, expected[i].texture_name.data(), actual_name.size) == 0) &&
             actual[i].flags == expected[i].flags &&
             actual[i].texture_flags == expected[i].texture_flags &&
             actual[i].surface_type == expected[i].surface_type;
    }
    unmap_file(&output.file);
    arena_release(&output_arena);
    return ok;
}

bool focused_edit_owns_clipboard_shortcut(const MSG& message) {
    if (message.message != WM_KEYDOWN || !(GetKeyState(VK_CONTROL) & 0x8000) ||
        (message.wParam != 'C' && message.wParam != 'V'))
        return false;
    const HWND focused = GetFocus();
    if (!focused || (focused != g.window && !IsChild(g.window, focused)))
        return false;
    char class_name[32]{};
    return GetClassNameA(focused, class_name, static_cast<int>(sizeof(class_name))) > 0 &&
           _stricmp(class_name, "Edit") == 0;
}

} // namespace editor

int APIENTRY WinMain(HINSTANCE instance, HINSTANCE, LPSTR command_line, int show) {
    using namespace editor;
    if (__argc == 2 && strcmp(__argv[1], "--entity-gizmo-smoke") == 0)
        return entity_gizmo_smoke() ? 0 : 40;
    if (__argc == 5 && strcmp(__argv[1], "--pc-material-map-roundtrip") == 0)
        return pc_material_map_roundtrip_smoke(__argv[2], __argv[3], __argv[4]) ? 0 : 44;
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
            SnipeItem_FragGrenade, SnipeItem_SmokeGrenade, SnipeItem_MedKit,
            SnipeItem_Bandage, SnipeItem_TnT, SnipeItem_Gewehr43,
            SnipeItem_Mosin91, SnipeItem_SVT40, SnipeItem_PPSH,
            SnipeItem_MP40, SnipeItem_MG42, SnipeItem_DP28, SnipeItem_TimeBomb,
            SnipeItem_Panzerschreck, SnipeItem_PanzerschreckAmmo
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
    const bool pc_editor_ui_smoke = __argc == 3 && strcmp(__argv[1], "--pc-editor-ui-smoke") == 0;
    const bool selection_smoke = __argc == 2 && strcmp(__argv[1], "--selection-smoke") == 0;
    const bool pc_weather_render_smoke =
        __argc == 3 && strcmp(__argv[1], "--pc-weather-render-smoke") == 0;
    const bool project_weather_render_smoke =
        __argc == 3 && strcmp(__argv[1], "--project-weather-render-smoke") == 0;
    const bool pc_material_render_smoke =
        __argc == 4 && strcmp(__argv[1], "--pc-material-render-smoke") == 0;
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
    if (__argc == 4 && strcmp(__argv[1], "--project-ambience-smoke") == 0) {
        Document document, exported;
        Mesh mesh;
        std::string why;
        if (!load_project(&document, __argv[2], &why))
            return 39;
        document.ambient_stream_path = "Sounds\\Streams\\indoor_s\\m1_roof.wav";
        document.ambient_volume = 0.375f;
        return pack_document(document, __argv[3], &why) &&
                       load_pc_level(__argv[3], &exported, &mesh, &why) &&
                       exported.ambient_stream_path == document.ambient_stream_path &&
                       memcmp(&exported.ambient_volume, &document.ambient_volume,
                              sizeof(document.ambient_volume)) == 0
                   ? 0
                   : 39;
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
        if (!load_pc_level(__argv[2], &imported, &mesh, &why) || !imported.skybox.source_record)
            return 8;
        // Exercise an actual version change and independent face flags rather
        // than conflating the flags with the serialized SKYB chunk version.
        imported.skybox.chunk_version = imported.skybox.chunk_version == 7 ? 6u : 7u;
        imported.skybox.back_texture_is_front_upside_down =
            !imported.skybox.back_texture_is_front_upside_down;
        imported.skybox.right_texture_is_left_upside_down =
            imported.skybox.chunk_version == 7 &&
            !imported.skybox.right_texture_is_left_upside_down;
        imported.rain_enabled = !imported.rain_enabled;
        imported.ambient_stream_path = "Sounds\\Streams\\indoor_s\\m1_roof.wav";
        imported.ambient_volume = 0.375f;
        return save_project(imported, __argv[3], &why) && load_project(&restored, __argv[3], &why) &&
                       same_skybox_settings(restored.skybox, imported.skybox) &&
                       restored.rain_enabled == imported.rain_enabled &&
                       restored.weather_source_record == imported.weather_source_record &&
                       same_ambience_settings(restored, imported) &&
                       restored.entities.size() == imported.entities.size() &&
                       restored.pickup_templates.size() == imported.pickup_templates.size() &&
                       restored.source_pickup_inventory_complete == imported.source_pickup_inventory_complete &&
                       pack_document(restored, __argv[4], &why) &&
                       load_pc_level(__argv[4], &exported, &exported_mesh, &why) &&
                       same_skybox_settings(exported.skybox, imported.skybox) &&
                       exported.rain_enabled == imported.rain_enabled &&
                       same_ambience_settings(exported, imported) &&
                       exported.entities.size() == imported.entities.size()
                   ? 0
                   : 8;
    }
    if (__argc == 5 && strcmp(__argv[1], "--pc-sky-weather-smoke") == 0) {
        Document imported, restored, exported;
        Mesh mesh, exported_mesh;
        std::string why;
        if (!load_pc_level(__argv[2], &imported, &mesh, &why) ||
            !imported.skybox.source_record || !imported.weather_source_record)
            return 32;
        const SkyboxSettings original_skybox = imported.skybox;
        const std::string original_ambience_path = imported.ambient_stream_path;
        const float original_ambience_volume = imported.ambient_volume;
        imported.rain_enabled = !imported.rain_enabled;
        if (!save_project(imported, __argv[3], &why) ||
            !load_project(&restored, __argv[3], &why) ||
            !same_skybox_settings(restored.skybox, original_skybox) ||
            restored.rain_enabled != imported.rain_enabled ||
            restored.ambient_stream_path != original_ambience_path ||
            memcmp(&restored.ambient_volume, &original_ambience_volume,
                   sizeof(original_ambience_volume)) != 0 ||
            !pack_document(restored, __argv[4], &why) ||
            !all_pc_rain_records_match(__argv[4], imported.rain_enabled) ||
            !load_pc_level(__argv[4], &exported, &exported_mesh, &why))
            return 33;
        return same_skybox_settings(exported.skybox, original_skybox) &&
                       exported.rain_enabled == imported.rain_enabled &&
                       exported.ambient_stream_path == original_ambience_path &&
                       memcmp(&exported.ambient_volume, &original_ambience_volume,
                              sizeof(original_ambience_volume)) == 0
                   ? 0
                   : 34;
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
    WNDCLASSEXA skybox_properties_class{};
    skybox_properties_class.cbSize = sizeof(skybox_properties_class);
    skybox_properties_class.lpfnWndProc = skybox_properties_proc;
    skybox_properties_class.hInstance = instance;
    skybox_properties_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    skybox_properties_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH));
    skybox_properties_class.lpszClassName = "Asura2005SkyboxProperties";
    if (!RegisterClassExA(&skybox_properties_class))
        return 1;
    WNDCLASSEXA ambience_properties_class{};
    ambience_properties_class.cbSize = sizeof(ambience_properties_class);
    ambience_properties_class.lpfnWndProc = ambience_properties_proc;
    ambience_properties_class.hInstance = instance;
    ambience_properties_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    ambience_properties_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH));
    ambience_properties_class.lpszClassName = "Asura2005AmbienceProperties";
    if (!RegisterClassExA(&ambience_properties_class))
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
    if (selection_smoke) {
        g.document = {};
        Entity first;
        first.kind = EntityKind::SpawnPoint;
        first.name = "First spawn";
        first.position = {1, 2, 3};
        Entity second = first;
        second.name = "Second spawn";
        second.position = {4, 5, 6};
        Entity light;
        light.kind = EntityKind::Light;
        light.name = "Other type";
        g.document.entities = {first, second, light};
        set_single_selection_state(0);
        reset_history(true);
        refresh_list();
        refresh_inspector();

        SendMessageA(g.list, LB_SETSEL, FALSE, -1);
        SendMessageA(g.list, LB_SETSEL, TRUE, entity_list_row(0));
        SendMessageA(g.list, LB_SETSEL, TRUE, entity_list_row(1));
        SendMessageA(g.list, LB_SETCARETINDEX, entity_list_row(1), FALSE);
        SendMessageA(window, WM_COMMAND, MAKEWPARAM(ID_ENTITY_LIST, LBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(g.list));
        const bool same_type_selected = g.selected_entities.size() == 2 && entity_is_selected(0) &&
                                        entity_is_selected(1) && g.selected == 1;

        SendMessageA(g.list, LB_SETSEL, TRUE, entity_list_row(2));
        SendMessageA(g.list, LB_SETCARETINDEX, entity_list_row(2), FALSE);
        SendMessageA(window, WM_COMMAND, MAKEWPARAM(ID_ENTITY_LIST, LBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(g.list));
        const bool mixed_type_rejected = g.selected_entities.size() == 2 && entity_is_selected(0) &&
                                         entity_is_selected(1) && !entity_is_selected(2);

        SetWindowTextA(g.pos[0], "42");
        SendMessageA(window, WM_COMMAND, MAKEWPARAM(ID_POS_X, EN_CHANGE),
                     reinterpret_cast<LPARAM>(g.pos[0]));
        apply_inspector();
        const bool batch_applied = g.document.entities[0].position.x == 42.0f &&
                                   g.document.entities[1].position.x == 42.0f &&
                                   g.document.entities[0].position.y == 2.0f &&
                                   g.document.entities[1].position.y == 5.0f &&
                                   g.document.entities[0].name == "First spawn" &&
                                   g.document.entities[1].name == "Second spawn";
        int restored_selection = g.selected;
        const bool undo_restored = g.history.undo(&g.document, &restored_selection) &&
                                   g.document.entities[0].position.x == 1.0f &&
                                   g.document.entities[1].position.x == 4.0f;
        DestroyWindow(window);
        return same_type_selected && mixed_type_rejected && batch_applied && undo_restored ? 0 : 41;
    }
    if (project_weather_render_smoke) {
        Document document;
        Mesh mesh;
        std::vector<PickupModel> pickup_models;
        std::string why;
        if (!load_project(&document, __argv[2], &why)) {
            DestroyWindow(window);
            return 42;
        }
        g.document = std::move(document);
        if (!load_document_preview(&g.document, &mesh, &why, &pickup_models)) {
            DestroyWindow(window);
            return 42;
        }
        g.mesh = std::move(mesh);
        g.pickup_models = std::move(pickup_models);
        frame_mesh();
        g.document.rain_enabled = true;
        uint32_t loaded_count = 0, missing_count = 0;
        const bool textures_loaded =
            gpu_reload_environment_textures(&why, &loaded_count, &missing_count);
        bool found_wet_material = false;
        for (const GpuMaterialRange& range : gpu.material_ranges)
            found_wet_material |= range.texture && (range.material_flags & 0x4000u) != 0;
        gpu_render();
        const bool valid = textures_loaded && loaded_count > 0 && found_wet_material &&
                           gpu.environment_wet_weather && gpu.environment_splash &&
                           gpu.rain_texture && gpu.wet_splash_drawn;
        DestroyWindow(window);
        return valid ? 0 : 43;
    }
    if (pc_material_render_smoke) {
        char* mask_end = nullptr;
        const unsigned long requested_mask = strtoul(__argv[3], &mask_end, 0);
        if (!mask_end || *mask_end || !requested_mask || requested_mask > 0xfffffffful ||
            !open_pc_path(__argv[2])) {
            DestroyWindow(window);
            return 39;
        }
        uint32_t loaded_count = 0, missing_count = 0;
        std::string why;
        const bool textures_loaded =
            gpu_reload_environment_textures(&why, &loaded_count, &missing_count);
        bool found_requested_material = false;
        for (const GpuMaterialRange& range : gpu.material_ranges)
            found_requested_material |= range.texture &&
                                        (range.material_flags & static_cast<uint32_t>(requested_mask)) != 0;
        gpu_render();
        const bool resources_ready =
            (!(requested_mask & 0x4u) || gpu.environment_detail) &&
            (!(requested_mask & 0x80u) || gpu.environment_spheremap);
        const bool valid = textures_loaded && loaded_count > 0 && found_requested_material &&
                           resources_ready && gpu.environment_view_buffer && gpu.additive_blend &&
                           gpu.reflection_blend && gpu_verify_environment_color_pipeline();
        DestroyWindow(window);
        return valid ? 0 : 40;
    }
    if (pc_weather_render_smoke) {
        if (!open_pc_path(__argv[2])) {
            DestroyWindow(window);
            return 37;
        }
        const bool original_rain = g.document.rain_enabled;
        bool state_matches_document =
            gpu.environment_wet_weather == original_rain &&
            (gpu.rain_texture != nullptr) == original_rain;
        if (!original_rain) {
            SendMessageA(g.rain_toggle, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageA(window, WM_COMMAND, MAKEWPARAM(ID_TOGGLE_RAIN, BN_CLICKED),
                         reinterpret_cast<LPARAM>(g.rain_toggle));
        }
        Asura_Vector_3 camera_position, right, up, forward;
        camera_axes(&camera_position, &right, &up, &forward);
        const auto first = build_gpu_rain_vertices(camera_position, right, up, forward,
                                                   1.0f, 1.5f, 0.0f);
        const auto second = build_gpu_rain_vertices(camera_position, right, up, forward,
                                                    1.0f, 1.5f, .25f);
        bool animated_layers = first.size() == 36 && second.size() == first.size();
        for (size_t index = 0; animated_layers && index < first.size(); ++index) {
            animated_layers = fabsf(first[index].position.x - second[index].position.x) < .0001f &&
                              fabsf(first[index].position.y - second[index].position.y) < .0001f &&
                              fabsf(first[index].position.z - second[index].position.z) < .0001f &&
                              fabsf(first[index].uv.y - second[index].uv.y) > .01f;
        }
        for (size_t index = 6; animated_layers && index < first.size(); index += 6) {
            const Asura_Vector_3 previous{first[index - 6].position.x,
                                          first[index - 6].position.y,
                                          first[index - 6].position.z};
            const Asura_Vector_3 current{first[index].position.x,
                                         first[index].position.y,
                                         first[index].position.z};
            animated_layers = dot(sub(previous, camera_position), forward) >
                              dot(sub(current, camera_position), forward);
        }
        const bool clouds_enabled = g.document.skybox.draw_clouds;
        g.document.skybox.draw_clouds = false;
        refresh_scene_animation_timer();
        gpu_render();
        const bool rainfall_drawn = g.document.rain_enabled && gpu.environment_wet_weather &&
                                    gpu.rain_texture && gpu.rain_pixel_shader &&
                                    gpu.rain_vertices && gpu.rain_capacity &&
                                    gpu.rain_vertex_count == 36 && gpu.rain_frame_drawn;
        const bool visible_vertical_streaks = gpu_verify_rain_streak_display();
        const bool prelight_identity = gpu_verify_environment_color_pipeline();
        g.document.skybox.draw_clouds = clouds_enabled;
        refresh_scene_animation_timer();
        SendMessageA(g.rain_toggle, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageA(window, WM_COMMAND, MAKEWPARAM(ID_TOGGLE_RAIN, BN_CLICKED),
                     reinterpret_cast<LPARAM>(g.rain_toggle));
        gpu_render();
        const bool rainfall_disabled = !g.document.rain_enabled && !gpu.environment_wet_weather &&
                                       !gpu.rain_texture && !gpu.rain_frame_drawn &&
                                       gpu.rain_vertex_count == 0;
        const bool valid = state_matches_document && animated_layers && rainfall_drawn &&
                           visible_vertical_streaks && prelight_identity && rainfall_disabled;
        DestroyWindow(window);
        return valid ? 0 : 38;
    }
    if (pc_editor_ui_smoke) {
        if (!open_pc_path(__argv[2])) {
            DestroyWindow(window);
            return 35;
        }
        const int count = static_cast<int>(SendMessageA(g.list, LB_GETCOUNT, 0, 0));
        bool sorted = count == static_cast<int>(g.document.entities.size());
        for (int row = 1; sorted && row < count; ++row) {
            const LRESULT previous = SendMessageA(g.list, LB_GETITEMDATA, row - 1, 0);
            const LRESULT current = SendMessageA(g.list, LB_GETITEMDATA, row, 0);
            if (previous == LB_ERR || current == LB_ERR) {
                sorted = false;
                break;
            }
            const Entity& first = g.document.entities[static_cast<size_t>(previous)];
            const Entity& second = g.document.entities[static_cast<size_t>(current)];
            const int names = _stricmp(first.name.c_str(), second.name.c_str());
            sorted = names < 0 ||
                     (names == 0 &&
                      _stricmp(entity_type_label(first.kind), entity_type_label(second.kind)) <= 0);
        }

        int pickup_index = -1;
        for (int index = 0; index < static_cast<int>(g.document.entities.size()); ++index)
            if (g.document.entities[index].kind == EntityKind::PhysicalObject) {
                pickup_index = index;
                break;
            }
        bool pickup_sorted = pickup_index >= 0;
        bool pickup_switch = false;
        if (pickup_sorted) {
            select_entity(pickup_index);
            const int item_count = static_cast<int>(SendMessageA(g.pickup_item, CB_GETCOUNT, 0, 0));
            pickup_sorted = item_count > 1 &&
                            (GetWindowLongPtrA(g.pickup_item, GWL_STYLE) & WS_VISIBLE) != 0;
            std::string previous_label;
            for (int row = 0; pickup_sorted && row < item_count; ++row) {
                char label[128]{};
                SendMessageA(g.pickup_item, CB_GETLBTEXT, row,
                             reinterpret_cast<LPARAM>(label));
                pickup_sorted = previous_label.empty() ||
                                _stricmp(previous_label.c_str(), label) <= 0;
                previous_label = label;
            }
            const LRESULT selected = SendMessageA(g.pickup_item, CB_GETCURSEL, 0, 0);
            if (pickup_sorted && selected != CB_ERR) {
                const int next = selected == 0 ? 1 : 0;
                const uint32_t wanted = static_cast<uint32_t>(
                    SendMessageA(g.pickup_item, CB_GETITEMDATA, next, 0));
                SendMessageA(g.pickup_item, CB_SETCURSEL, next, 0);
                apply_inspector();
                pickup_switch = g.document.entities[g.selected].value_u32_a == wanted;
            }
        }

        bool skybox_uv = false;
        if (gpu.skybox_vertices) {
            D3D11_BUFFER_DESC source{};
            gpu.skybox_vertices->GetDesc(&source);
            D3D11_BUFFER_DESC staging = source;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.MiscFlags = 0;
            ID3D11Buffer* copy = nullptr;
            if (SUCCEEDED(gpu.device->CreateBuffer(&staging, nullptr, &copy))) {
                gpu.context->CopyResource(copy, gpu.skybox_vertices);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(gpu.context->Map(copy, 0, D3D11_MAP_READ, 0, &mapped))) {
                    const auto* vertices = static_cast<const SkyboxVertex*>(mapped.pData);
                    const float expected_u =
                        g.document.skybox.right_texture_is_left_upside_down ? 0.0f : 1.0f;
                    const float expected_v =
                        !g.document.skybox.right_texture_is_left_upside_down &&
                                g.document.skybox.back_texture_is_front_upside_down
                            ? 0.0f
                            : 1.0f;
                    skybox_uv = fabsf(vertices[12].uv.x - expected_u) < .001f &&
                                fabsf(vertices[12].uv.y - expected_v) < .001f;
                    gpu.context->Unmap(copy, 0);
                }
                copy->Release();
            }
        }

        const bool original_rain = g.document.rain_enabled;
        SendMessageA(g.rain_toggle, BM_SETCHECK,
                     original_rain ? BST_UNCHECKED : BST_CHECKED, 0);
        SendMessageA(window, WM_COMMAND, MAKEWPARAM(ID_TOGGLE_RAIN, BN_CLICKED),
                     reinterpret_cast<LPARAM>(g.rain_toggle));
        const bool rain_toggled = g.document.rain_enabled != original_rain &&
                                  gpu.environment_wet_weather == g.document.rain_enabled &&
                                  (gpu.rain_texture != nullptr) == g.document.rain_enabled;
        command_undo();
        const bool rain_undo = g.document.rain_enabled == original_rain &&
                               gpu.environment_wet_weather == original_rain &&
                               (gpu.rain_texture != nullptr) == original_rain &&
                               (SendMessageA(g.rain_toggle, BM_GETCHECK, 0, 0) == BST_CHECKED) ==
                                   original_rain;
        const std::vector<std::string> ambience_streams = available_ambience_streams();
        AmbiencePropertiesState ambience_test_state{};
        ambience_test_state.paths = ambience_streams;
        HWND ambience_test_window = CreateWindowExA(
            WS_EX_TOOLWINDOW, "Asura2005AmbienceProperties", "Ambience combo test", WS_POPUP,
            0, 0, 540, 235, window, nullptr, GetModuleHandle(nullptr), &ambience_test_state);
        const bool ambience_first_choice =
            ambience_test_window && ambience_test_state.stream &&
            SendMessageA(ambience_test_state.stream, CB_GETCOUNT, 0, 0) ==
                static_cast<LRESULT>(ambience_streams.size() + 1) &&
            SendMessageA(ambience_test_state.stream, CB_FINDSTRINGEXACT,
                         static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>("01_temp")) == 1;
        if (ambience_test_window)
            DestroyWindow(ambience_test_window);
        const bool ambience_ui = IsWindowEnabled(g.ambience_properties) &&
                                 ambience_streams.size() >= 54 &&
                                 ambience_stream_label(ambience_streams.front()) == "01_temp" &&
                                 ambience_first_choice &&
                                 std::any_of(ambience_streams.begin(), ambience_streams.end(),
                                             [&](const std::string& path) {
                                                 return _stricmp(path.c_str(),
                                                                 g.document.ambient_stream_path.c_str()) == 0;
                                             });
        const bool valid = sorted && pickup_sorted && pickup_switch && skybox_uv &&
                           rain_toggled && rain_undo && ambience_ui &&
                           g.document.skybox.chunk_version == 7;
        DestroyWindow(window);
        return valid ? 0 : 36;
    }
    if (pc_gpu_smoke) {
        const bool loaded = open_pc_path(__argv[2]);
        bool list_double_click_focus = false;
        if (loaded && g.list && !g.document.entities.empty()) {
            const Camera before = g.camera;
            SendMessageA(g.list, LB_SETSEL, TRUE, 0);
            SendMessageA(g.list, LB_SETCARETINDEX, 0, FALSE);
            const int expected_index =
                static_cast<int>(SendMessageA(g.list, LB_GETITEMDATA, 0, 0));
            SendMessageA(window, WM_COMMAND, MAKEWPARAM(ID_ENTITY_LIST, LBN_DBLCLK),
                         reinterpret_cast<LPARAM>(g.list));
            list_double_click_focus =
                g.selected == expected_index && (!nearly_equal(g.camera.target, before.target) ||
                                    !nearly_equal(g.camera.distance, before.distance));
        }
        uint32_t environment_textures = 0, environment_fallbacks = 0;
        std::string environment_why;
        const bool environment_materials =
            gpu_reload_environment_textures(&environment_why, &environment_textures, &environment_fallbacks);
        gpu_render();
        const bool environment_multipass_ready =
            gpu.alpha_blend && gpu.modulate2x_blend && gpu.additive_blend &&
            gpu.reflection_blend && gpu.depth_equal && gpu.environment_view_buffer;
        bool found_txfl_bit_clear = false, found_txfl_bit_set = false, found_wet_material = false;
        bool found_detail_material = false, found_spheremap_material = false;
        bool found_solid_cutout = false;
        for (const GpuMaterialRange& range : gpu.material_ranges) {
            if (!range.texture)
                continue;
            found_detail_material |= (range.material_flags & 0x4u) != 0;
            found_spheremap_material |= (range.material_flags & 0x80u) != 0;
            found_solid_cutout |= gpu_material_is_solid_cutout(range);
            if ((range.material_flags & 2u) == 0)
                continue;
            found_txfl_bit_clear |= (range.texture_flags & 8u) == 0;
            found_txfl_bit_set |= (range.texture_flags & 8u) != 0;
            found_wet_material |= (range.material_flags & 0x4000u) != 0;
        }
        bool pickups_resolved = !g.pickup_models.empty();
        for (const Entity& entity : g.document.entities)
            if (entity.kind == EntityKind::PhysicalObject)
                pickups_resolved &= pickup_model_for_skin(entity.pickup_skin_id) != nullptr;
        const bool rendered = loaded && gpu.ready && gpu.mesh_vertices && gpu.mesh_indices && gpu.mesh_index_count &&
                              list_double_click_focus &&
                              environment_materials && environment_textures > 0 &&
                              environment_textures + environment_fallbacks == gpu.material_ranges.size() &&
                              environment_multipass_ready && found_txfl_bit_clear && found_txfl_bit_set &&
                              found_solid_cutout && gpu.alpha_tested_prelight_drawn &&
                              gpu.solid_cutout_prelight_drawn &&
                              gpu.composited_alpha_prelight_drawn &&
                              (!found_detail_material || gpu.environment_detail) &&
                              (!found_spheremap_material || gpu.environment_spheremap) &&
                              gpu_verify_environment_color_pipeline() &&
                              found_wet_material && gpu.environment_wet_weather && gpu.environment_splash &&
                              gpu.rain_texture && gpu.rain_pixel_shader && gpu.rain_vertices &&
                              gpu.rain_vertex_count == 36 && gpu.rain_frame_drawn &&
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
            set_single_selection_state(0);
        }
        reset_history(false);
        gpu_render();
        if (loaded) {
            set_single_selection_state(1);
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
                set_single_selection_state(g.document.entities.empty() ? -1 : 0);
                g.pending_kind = -1;
                const bool skybox_loaded = reload_skybox_preview(true);
                load_document_preview(&g.document, &g.mesh, &why, &g.pickup_models);
                frame_mesh();
                reset_history(!g.document.dirty);
                refresh_list();
                refresh_inspector();
                update_title();
                set_status(skybox_loaded ? "Project loaded." : "Project loaded; skybox preview is unavailable.");
            }
        }
    }
    const ACCEL accelerator_entries[] = {
        {FVIRTKEY | FCONTROL, 'Z', ID_UNDO},
        {FVIRTKEY | FCONTROL, 'Y', ID_REDO},
        {FVIRTKEY | FCONTROL, 'C', ID_COPY_ENTITY},
        {FVIRTKEY | FCONTROL, 'V', ID_PASTE_ENTITY},
        {FVIRTKEY, VK_DELETE, ID_DELETE_ENTITY},
    };
    HACCEL accelerators = CreateAcceleratorTableA(
        const_cast<LPACCEL>(accelerator_entries), static_cast<int>(_countof(accelerator_entries)));
    MSG message{};
    while (GetMessageA(&message, nullptr, 0, 0) > 0) {
        if (!accelerators || focused_edit_owns_clipboard_shortcut(message) ||
            !TranslateAcceleratorA(window, accelerators, &message)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
    if (accelerators)
        DestroyAcceleratorTable(accelerators);
    return static_cast<int>(message.wParam);
}
