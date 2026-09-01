#include "LevelEditorInternal.h"

using namespace asura;
using namespace asura::level;

namespace editor {

bool load_preview_mesh(const std::string& path, const std::string& material_map_path, Mesh* mesh, std::string* why) {
    Error err{};
    Arena arena{};
    MappedFile file{};
    ObjData obj{};
    MaterialMap materials{};
    Config cfg{};
    bool ok = arena_init(&arena, sizeof(void*) == 4 ? 256 * MiB : 2 * GiB, &err) &&
              initialize_editor_config(path.c_str(), &cfg, &err) &&
              map_file(path.c_str(), &file, &err) &&
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

namespace {

struct Ps2EnvironmentPair {
    uint16_t module_index;
    uint16_t triangle_count;
};

struct Ps2EnvironmentGroup {
    uint16_t material_index;
    std::vector<Ps2EnvironmentPair> pairs;
};

struct Ps2VifBatch {
    std::vector<Asura_Vector_3> positions;
    std::vector<Asura_Vector_2> texcoords;
    std::vector<uint32_t> diffuse_abgr;
    std::vector<Asura_Vector_3> normals;
    std::vector<uint8_t> adc;
};

struct Ps2ObjectView {
    Str object_path{};
    Str texture_name{};
    float lod_distance = 0;
    uint32_t material_flags = 0;
    uint32_t object_flags = 0;
    uint32_t triangle_count = 0;
    uint32_t packet_count = 0;
    const uint8_t* packet_table = nullptr;
    const uint8_t* packets = nullptr;
    uint32_t packet_bytes = 0;
};

constexpr uint32_t kPs2ObjectResourceSubtype = 6;

bool ps2_object_view(const RscfInfo& resource, Ps2ObjectView* output, Error* err) {
    if (resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
        resource.subtype != kPs2ObjectResourceSubtype)
        return false;
    const uint8_t* data = resource.payload;
    const uint32_t size = resource.payload_size;
    Str object_path = padded_string_at(data, size, 0);
    if (!object_path.data)
        return fail(err, "PS2 object resource '%.*s' has no embedded object path",
                    resource.name.size, resource.name.data);
    uint64_t at = align_up(static_cast<uint64_t>(object_path.size) + 1, 4);
    if (at + 8 > size)
        return fail(err, "PS2 object resource '%.*s' has a truncated LOD header",
                    resource.name.size, resource.name.data);
    const float lod_distance = read_f32(data + at);
    const uint32_t texture_bytes = read_u32(data + at + 4);
    at += 8;
    if (!isfinite(lod_distance) || !texture_bytes || texture_bytes > size - at)
        return fail(err, "PS2 object resource '%.*s' has an invalid texture binding",
                    resource.name.size, resource.name.data);
    Str texture_name = padded_string_at(data + at, texture_bytes, 0);
    if (!texture_name.data || texture_name.size + 1 > texture_bytes)
        return fail(err, "PS2 object resource '%.*s' has a malformed texture name",
                    resource.name.size, resource.name.data);
    at += texture_bytes;
    if (at + 16 > size)
        return fail(err, "PS2 object resource '%.*s' has a truncated geometry header",
                    resource.name.size, resource.name.data);
    Ps2ObjectView next;
    next.object_path = object_path;
    next.texture_name = texture_name;
    next.lod_distance = lod_distance;
    next.material_flags = read_u32(data + at);
    next.object_flags = read_u32(data + at + 4);
    next.triangle_count = read_u32(data + at + 8);
    next.packet_count = read_u32(data + at + 12);
    at += 16;
    if (next.packet_count > (size - at) / 4)
        return fail(err, "PS2 object resource '%.*s' has a truncated packet table",
                    resource.name.size, resource.name.data);
    next.packet_table = data + at;
    uint64_t packet_bytes = 0;
    for (uint32_t packet = 0; packet < next.packet_count; ++packet)
        packet_bytes += static_cast<uint64_t>(read_u16(next.packet_table + packet * 4)) * 16;
    at += static_cast<uint64_t>(next.packet_count) * 4;
    if (packet_bytes > size - at)
        return fail(err, "PS2 object resource '%.*s' has truncated VIF packets",
                    resource.name.size, resource.name.data);
    if (at + packet_bytes != size)
        return fail(err, "PS2 object resource '%.*s' has unexpected trailing data",
                    resource.name.size, resource.name.data);
    if ((next.triangle_count == 0) != (next.packet_count == 0))
        return fail(err, "PS2 object resource '%.*s' has inconsistent geometry counts",
                    resource.name.size, resource.name.data);
    next.packets = data + at;
    next.packet_bytes = static_cast<uint32_t>(packet_bytes);
    *output = next;
    return true;
}

int32_t ps2_signed_5(uint16_t value) {
    const int32_t component = value & 31;
    return component < 16 ? component : component - 32;
}

bool append_ps2_batch(Ps2VifBatch* batch, int32_t material_index, Mesh* mesh,
                      uint32_t* triangle_count, Error* err) {
    const size_t count = batch->positions.size();
    if (!count)
        return true;
    if (count < 3)
        return fail(err, "PS2 environment VIF batch has only %zu vertices", count);
    if (!batch->texcoords.empty() && batch->texcoords.size() != count)
        return fail(err, "PS2 environment VIF UV count does not match its positions");
    if (!batch->diffuse_abgr.empty() && batch->diffuse_abgr.size() != count)
        return fail(err, "PS2 environment VIF colour count does not match its positions");
    if (!batch->normals.empty() && batch->normals.size() != count)
        return fail(err, "PS2 environment VIF normal count does not match its positions");
    if (!batch->adc.empty() && batch->adc.size() != count)
        return fail(err, "PS2 environment VIF ADC count does not match its positions");
    if (mesh->positions.size() > 0xffffffffull - count)
        return fail(err, "PS2 environment has too many vertices for the editor");

    const uint32_t base = static_cast<uint32_t>(mesh->positions.size());
    mesh->positions.reserve(mesh->positions.size() + count);
    mesh->normals.reserve(mesh->normals.size() + count);
    mesh->texcoords.reserve(mesh->texcoords.size() + count);
    mesh->diffuse_abgr.reserve(mesh->diffuse_abgr.size() + count);
    for (size_t i = 0; i < count; ++i) {
        Asura_Vector_3 position = batch->positions[i];
        // The 2005 target uses negative Y as up, matching the PC renderer.
        position.y = -position.y;
        mesh->positions.push_back(position);

        Asura_Vector_3 normal = batch->normals.empty()
                                      ? Asura_Vector_3{0, 1, 0}
                                      : batch->normals[i];
        normal.y = -normal.y;
        const float length = sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        if (length > 1e-8f) {
            normal.x /= length;
            normal.y /= length;
            normal.z /= length;
        } else {
            normal = {0, 1, 0};
        }
        mesh->normals.push_back(normal);
        mesh->texcoords.push_back(batch->texcoords.empty() ? Asura_Vector_2{} : batch->texcoords[i]);
        mesh->diffuse_abgr.push_back(batch->diffuse_abgr.empty()
                                         ? 0xff808080u
                                         : batch->diffuse_abgr[i]);
    }

    for (uint32_t vertex = 2; vertex < count; ++vertex) {
        // ADC is set on the first two vertices after a strip restart. A vertex
        // with ADC set is transferred to VU1 but does not emit a primitive.
        if (!batch->adc.empty() && batch->adc[vertex])
            continue;
        uint32_t a = base + vertex - 2;
        uint32_t b = base + vertex - 1;
        const uint32_t c = base + vertex;
        if ((vertex - 2) & 1)
            std::swap(a, b);
        // VU1's triangle kick order is the reverse of the PC Env strip order.
        // The Y-axis conversion below and the PS2 kick convention therefore
        // cancel; applying the PC decoder's additional swap culls the visible
        // side of most PS2 geometry.
        mesh->faces.push_back({a, b, c});
        mesh->face_materials.push_back(material_index);
        ++*triangle_count;
    }
    *batch = {};
    return true;
}

bool append_ps2_dma_page_stream(const uint8_t* pages, uint32_t page_count,
                                uint16_t last_page_qwords, std::vector<uint8_t>* stream,
                                Error* err) {
    if (!page_count || !last_page_qwords || last_page_qwords > 64)
        return fail(err, "PS2 environment render record has invalid DMA page counts");
    stream->clear();
    stream->reserve(static_cast<size_t>(page_count) * 1024);
    for (uint32_t page_index = 0; page_index < page_count; ++page_index) {
        const uint8_t* page = pages + static_cast<uint64_t>(page_index) * 1024;
        const uint32_t valid_qwords = page_index + 1 == page_count ? last_page_qwords : 64;
        uint32_t qword = 0;
        while (qword < valid_qwords) {
            const uint8_t* tag = page + static_cast<uint64_t>(qword) * 16;
            const uint32_t transfer_qwords = read_u32(tag) & 0xffffu;
            if (transfer_qwords > valid_qwords - qword - 1)
                return fail(err, "PS2 environment DMA tag exceeds its 1 KiB page");
            // With tag transfer enabled the tag's upper 64 bits enter VIF1
            // before the qword payload. Page links split UNPACK payloads, so
            // preserving this order is essential.
            stream->insert(stream->end(), tag + 8, tag + 16);
            const uint8_t* payload = tag + 16;
            stream->insert(stream->end(), payload,
                           payload + static_cast<uint64_t>(transfer_qwords) * 16);
            qword += transfer_qwords + 1;
        }
    }
    return true;
}

uint32_t ps2_vif_unpack_bytes(uint32_t format, uint32_t count) {
    if (format == 0x0f)
        return count * 2;
    const uint32_t components = (format >> 2) + 1;
    const uint32_t width_code = format & 3;
    if (width_code == 3)
        return 0;
    return count * components * (4u >> width_code);
}

bool decode_ps2_vif_stream(const uint8_t* stream_data, size_t stream_size,
                           int32_t material_index, Mesh* mesh,
                           uint32_t* decoded_triangles, Error* err) {
    Ps2VifBatch batch;
    uint64_t at = 0;
    while (at + 4 <= stream_size) {
        const uint32_t code = read_u32(stream_data + at);
        at += 4;
        const uint32_t command = (code >> 24) & 0x7fu;
        if (command >= 0x60) {
            const uint32_t format = command & 0x0fu;
            uint32_t count = (code >> 16) & 0xffu;
            if (!count)
                count = 256;
            const uint32_t byte_count = ps2_vif_unpack_bytes(format, count);
            if (!byte_count)
                return fail(err, "PS2 environment uses unsupported VIF UNPACK format 0x%X", format);
            if (byte_count > stream_size - at)
                return fail(err, "PS2 environment VIF UNPACK payload is truncated");
            const uint8_t* payload = stream_data + at;

            if (format == 0x08) { // V3-32 positions
                if (!append_ps2_batch(&batch, material_index, mesh, decoded_triangles, err))
                    return false;
                batch.positions.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    const uint8_t* vertex = payload + static_cast<uint64_t>(i) * 12;
                    const Asura_Vector_3 position{read_f32(vertex), read_f32(vertex + 4),
                                                  read_f32(vertex + 8)};
                    if (!isfinite(position.x) || !isfinite(position.y) || !isfinite(position.z))
                        return fail(err, "PS2 environment contains a non-finite position");
                    batch.positions.push_back(position);
                }
            } else if (format == 0x05) { // V2-16 fixed-point texture coordinates
                batch.texcoords.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    const uint8_t* uv = payload + static_cast<uint64_t>(i) * 4;
                    batch.texcoords.push_back({static_cast<int16_t>(read_u16(uv)) / 4096.0f,
                                               static_cast<int16_t>(read_u16(uv + 2)) / 4096.0f});
                }
            } else if (format == 0x0a) { // V3-8 unsigned baked RGB prelight
                batch.diffuse_abgr.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    const uint8_t* colour = payload + static_cast<uint64_t>(i) * 3;
                    // MCP2 uploads this array at VU address 2, between UV at
                    // address 1 and normal/ADC at address 3. Preserve its GS
                    // 0x80-neutral scale: the PC Env texture pass is likewise
                    // MODULATE2X, so expanding it to 0xff would overbrighten it.
                    batch.diffuse_abgr.push_back(0xff000000u |
                                                 (static_cast<uint32_t>(colour[0]) << 16u) |
                                                 (static_cast<uint32_t>(colour[1]) << 8u) |
                                                 colour[2]);
                }
            } else if (format == 0x0f) { // V4-5 packed normal plus ADC
                batch.normals.reserve(count);
                batch.adc.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    const uint16_t packed = read_u16(payload + static_cast<uint64_t>(i) * 2);
                    batch.normals.push_back(
                        {static_cast<float>(ps2_signed_5(packed)),
                         static_cast<float>(ps2_signed_5(packed >> 5)),
                         static_cast<float>(ps2_signed_5(packed >> 10))});
                    batch.adc.push_back((packed & 0x8000u) != 0);
                }
            }
            at += align_up(byte_count, 4);
            continue;
        }

        uint32_t payload_bytes = 0;
        if (command == 0x20)
            payload_bytes = 4; // STMASK
        else if (command == 0x30 || command == 0x31)
            payload_bytes = 16; // STROW / STCOL
        else if (command == 0x4a) {
            uint32_t count = (code >> 16) & 0xffu;
            payload_bytes = (count ? count : 256) * 8; // MPG
        } else if (command == 0x50 || command == 0x51) {
            payload_bytes = (code & 0xffffu) * 16; // DIRECT / DIRECTHL
        }
        if (payload_bytes > stream_size - at)
            return fail(err, "PS2 environment VIF command payload is truncated");
        at += payload_bytes;
    }
    if (at != stream_size)
        return fail(err, "PS2 environment VIF stream is not dword-aligned");
    return append_ps2_batch(&batch, material_index, mesh, decoded_triangles, err);
}

bool decode_ps2_vif_record(const uint8_t* pages, uint32_t page_count,
                           uint16_t last_page_qwords, int32_t material_index,
                           uint16_t declared_triangles, Mesh* mesh, Error* err) {
    std::vector<uint8_t> stream;
    if (!append_ps2_dma_page_stream(pages, page_count, last_page_qwords, &stream, err))
        return false;
    uint32_t decoded_triangles = 0;
    if (!decode_ps2_vif_stream(stream.data(), stream.size(), material_index, mesh,
                               &decoded_triangles, err))
        return false;
    if (decoded_triangles != declared_triangles)
        return fail(err, "PS2 environment render record decoded %u triangles; target metadata declares %u",
                    decoded_triangles, declared_triangles);
    return true;
}

} // namespace

const RscfInfo* find_ps2_environment(const ChunkList& chunks, RscfInfo* storage) {
    for (uint32_t i = 0; i < chunks.count; ++i) {
        RscfInfo resource{};
        if (!rscf_info(chunks.chunks[i], &resource) ||
            resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
            resource.subtype != 0)
            continue;
        const std::string name(resource.name.data, resource.name.size);
        if (name.find("PS2StrippedEnv") != std::string::npos) {
            *storage = resource;
            return storage;
        }
    }
    return nullptr;
}

bool decode_ps2_tim2(const uint8_t* bytes, uint32_t byte_count, uint32_t* width,
                     uint32_t* height, std::vector<uint8_t>* rgba, Error* err) {
    if (!bytes || byte_count < 16 || memcmp(bytes, "TIM2", 4) != 0)
        return fail(err, "PS2 texture is not a TIM2 file");
    if ((bytes[4] != 3 && bytes[4] != 4) || !read_u16(bytes + 6))
        return fail(err, "PS2 texture uses an unsupported TIM2 header");

    // MCP2 sub_1AA298 selects the 128-byte picture alignment when the TIM2
    // format byte is non-zero; otherwise the first picture follows at +16.
    const uint32_t picture_at = bytes[5] ? 128u : 16u;
    if (picture_at > byte_count || byte_count - picture_at < 0x58)
        return fail(err, "PS2 TIM2 picture header is truncated");
    const uint8_t* picture = bytes + picture_at;
    const uint32_t total_size = read_u32(picture);
    const uint32_t clut_size = read_u32(picture + 4);
    const uint32_t image_size = read_u32(picture + 8);
    const uint32_t header_size = read_u16(picture + 12);
    const uint32_t clut_colors = read_u16(picture + 14);
    const uint32_t stored_width = read_u16(picture + 20);
    const uint32_t stored_height = read_u16(picture + 22);
    if (header_size < 0x58 || total_size < header_size || total_size > byte_count - picture_at ||
        image_size > total_size - header_size || clut_size > total_size - header_size - image_size)
        return fail(err, "PS2 TIM2 picture sizes are invalid");

    // MCP2 sub_19D8F8 asks the TIM2 library for the user-data extension, then
    // enables sub_1AA3C0's expanded upload layout for an ASUR record with bit
    // zero set. Its offset depends on the picture-header variant: SKYB TIM2s
    // place it at +0x30, while environment/object TIM2s place it at +0x50.
    const bool asur_30 = header_size >= 0x38 && read_u32(picture + 0x30) == 0x52555341u &&
                         (read_u32(picture + 0x34) & 1u);
    const bool asur_50 = header_size >= 0x58 && read_u32(picture + 0x50) == 0x52555341u &&
                         (read_u32(picture + 0x54) & 1u);
    if (!asur_30 && !asur_50)
        return fail(err, "PS2 TIM2 texture does not use the target ASUR layout");
    const uint32_t clut_type = picture[18] & 0x3fu;
    const uint32_t palette_stride =
        clut_type == 1 ? 2u : clut_type == 2 ? 3u : clut_type == 3 ? 4u : 0u;
    if (clut_colors != 256 || !palette_stride ||
        clut_size < clut_colors * palette_stride)
        return fail(err,
                    "PS2 TIM2 texture uses unsupported palette format "
                    "(colours=%u, bytes=%u, clut-type=%u, image-type=%u, stored=%ux%u)",
                    clut_colors, clut_size, picture[18], picture[19], stored_width,
                    stored_height);

    const uint32_t decoded_width = stored_width * 2u;
    const uint32_t decoded_height = stored_height * 2u;
    if (!decoded_width || !decoded_height || decoded_width > 4096 || decoded_height > 4096)
        return fail(err, "PS2 TIM2 texture dimensions are invalid");
    const uint64_t pixel_count = static_cast<uint64_t>(decoded_width) * decoded_height;
    if (pixel_count > image_size || pixel_count > SIZE_MAX / 4)
        return fail(err, "PS2 TIM2 base image is truncated");

    const uint8_t* indexed = picture + header_size;
    const uint8_t* palette = indexed + image_size;
    rgba->assign(static_cast<size_t>(pixel_count) * 4, 0);
    for (uint32_t y = 0; y < decoded_height; ++y) {
        for (uint32_t x = 0; x < decoded_width; ++x) {
            // GS PSMT8 block/column addressing, matching the swizzled bytes
            // consumed by the retail PS2 renderer.
            const uint64_t block = static_cast<uint64_t>(y & ~15u) * decoded_width +
                                   static_cast<uint64_t>(x & ~15u) * 2u;
            const uint32_t swap = (((y + 2u) >> 2u) & 1u) * 4u;
            const uint32_t row = ((((y & ~3u) >> 1u) + (y & 1u)) & 7u);
            const uint64_t column = static_cast<uint64_t>(row) * decoded_width * 2u +
                                    static_cast<uint64_t>((x + swap) & 7u) * 4u;
            const uint32_t byte = ((y >> 1u) & 1u) + ((x >> 2u) & 2u);
            const uint64_t source_at = block + column + byte;
            if (source_at >= pixel_count)
                return fail(err, "PS2 TIM2 swizzled image address exceeds its base mip");
            const uint32_t source_index = indexed[source_at];
            const uint32_t palette_index = (source_index & 0xe7u) |
                                           ((source_index & 0x08u) << 1u) |
                                           ((source_index & 0x10u) >> 1u);
            const size_t target = (static_cast<size_t>(y) * decoded_width + x) * 4;
            if (palette_stride == 2) {
                const uint16_t color = read_u16(palette + palette_index * 2u);
                (*rgba)[target] = static_cast<uint8_t>(((color >> 0u) & 31u) * 255u / 31u);
                (*rgba)[target + 1] = static_cast<uint8_t>(((color >> 5u) & 31u) * 255u / 31u);
                (*rgba)[target + 2] = static_cast<uint8_t>(((color >> 10u) & 31u) * 255u / 31u);
                (*rgba)[target + 3] = (color & 0x8000u) ? 255u : 0u;
            } else if (palette_stride == 3) {
                const uint8_t* color = palette + palette_index * 3u;
                (*rgba)[target] = color[0];
                (*rgba)[target + 1] = color[1];
                (*rgba)[target + 2] = color[2];
                (*rgba)[target + 3] = 255u;
            } else {
                const uint8_t* color = palette + palette_index * 4u;
                (*rgba)[target] = color[0];
                (*rgba)[target + 1] = color[1];
                (*rgba)[target + 2] = color[2];
                // GS 32-bit colour stores alpha on a 0..0x80 scale.
                (*rgba)[target + 3] = static_cast<uint8_t>(std::min(255u, color[3] * 2u));
            }
        }
    }
    *width = decoded_width;
    *height = decoded_height;
    return true;
}

bool decode_ps2_environment(const RscfInfo& resource, Mesh* mesh, Error* err) {
    if (resource.payload_size < 32)
        return fail(err, "PS2 environment header is truncated");
    const uint8_t* data = resource.payload;
    const uint64_t size = resource.payload_size;
    const uint32_t group_count = read_u32(data);
    const uint32_t record_count = read_u32(data + 16);
    const uint32_t page_count = read_u32(data + 24);
    if (!group_count || !record_count || !page_count || group_count > record_count)
        return fail(err, "PS2 environment header has invalid material or packet counts");

    std::vector<Ps2EnvironmentGroup> groups;
    groups.reserve(group_count);
    uint64_t at = 32;
    uint64_t parsed_records = 0;
    for (uint32_t group_index = 0; group_index < group_count; ++group_index) {
        if (at + 4 > size)
            return fail(err, "PS2 environment material table is truncated");
        Ps2EnvironmentGroup group;
        group.material_index = read_u16(data + at);
        const uint16_t count = read_u16(data + at + 2);
        const uint64_t used = 4ull + static_cast<uint64_t>(count) * 4;
        const uint64_t stored = align_up(used, 16);
        if (stored > size - at)
            return fail(err, "PS2 environment material record is truncated");
        group.pairs.reserve(count);
        for (uint32_t pair_index = 0; pair_index < count; ++pair_index) {
            const uint8_t* pair = data + at + 4 + static_cast<uint64_t>(pair_index) * 4;
            group.pairs.push_back({read_u16(pair), read_u16(pair + 2)});
        }
        parsed_records += count;
        groups.push_back(std::move(group));
        at += stored;
    }
    if (parsed_records != record_count)
        return fail(err, "PS2 environment material table declares %u records but contains %llu",
                    record_count, static_cast<unsigned long long>(parsed_records));
    // MCP2 sub_1BD6C0 reads a fixed 32-dword page-offset table after the
    // variable material records. The retail sample uses one active page set.
    if (at + 128 > size)
        return fail(err, "PS2 environment page-offset table is truncated");
    at += 128;
    if (at + 16 > size)
        return fail(err, "PS2 environment geometry header is truncated");
    if (read_u32(data + at) != group_count || read_u32(data + at + 4) != page_count)
        return fail(err, "PS2 environment geometry counts disagree with its header");
    at += 16;

    Mesh next;
    uint64_t parsed_pages = 0;
    for (uint32_t group_index = 0; group_index < group_count; ++group_index) {
        const Ps2EnvironmentGroup& group = groups[group_index];
        if (at + 16 > size)
            return fail(err, "PS2 environment geometry material header is truncated");
        const uint32_t serialized_group = read_u32(data + at);
        const uint32_t serialized_count = read_u32(data + at + 4);
        if (serialized_group != group_index || serialized_count != group.pairs.size())
            return fail(err, "PS2 environment geometry material table is out of sequence");
        at += 16;
        for (uint32_t pair_index = 0; pair_index < group.pairs.size(); ++pair_index) {
            if (at + 16 > size)
                return fail(err, "PS2 environment render record header is truncated");
            const uint32_t serialized_record = read_u32(data + at);
            const uint32_t record_pages = read_u32(data + at + 4);
            const uint16_t first_page_qwords = read_u16(data + at + 8);
            const uint16_t last_page_qwords = read_u16(data + at + 10);
            if (serialized_record != pair_index || !record_pages || !first_page_qwords ||
                first_page_qwords > 64 || !last_page_qwords || last_page_qwords > 64)
                return fail(err, "PS2 environment render record has invalid packet metadata");
            at += 16;
            const uint64_t packet_bytes = static_cast<uint64_t>(record_pages) * 1024;
            if (packet_bytes > size - at)
                return fail(err, "PS2 environment render record packets are truncated");
            if (!decode_ps2_vif_record(data + at, record_pages, last_page_qwords,
                                       group.material_index, group.pairs[pair_index].triangle_count,
                                       &next, err))
                return false;
            at += packet_bytes;
            parsed_pages += record_pages;
        }
    }
    if (parsed_pages != page_count)
        return fail(err, "PS2 environment declares %u DMA pages but contains %llu", page_count,
                    static_cast<unsigned long long>(parsed_pages));
    if (at != size)
        return fail(err, "PS2 environment has %llu trailing bytes",
                    static_cast<unsigned long long>(size - at));
    if (next.faces.empty())
        return fail(err, "PS2 environment contains no renderable triangles");
    finish_mesh_bounds(&next);
    *mesh = std::move(next);
    return true;
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
    case SnipeItem_TripWire: return "Trip-wire grenade";
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

    Snipe_ServerEntity_Pickup_ChunkDataV0 body{};
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
                                               offsetof(Snipe_ServerEntity_Pickup_ChunkDataV0,
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

bool valid_static_object_body(const Snipe_ServerEntity_StaticObject_ChunkDataV0& body) {
    return body.m_iStaticObjectVersion == 3 && body.m_iAsuraStaticObjectVersion == 0 &&
           body.m_iPhysicalObjectVersion == 7 && body.m_iAsuraPhysicalObjectVersion == 7;
}

StaticObjectTemplate make_canonical_static_object_template(uint32_t file_id, Str resource_name,
                                                            const std::string& donor_path) {
    StaticObjectTemplate object;
    object.file_id = file_id;
    object.resource_name.assign(resource_name.data, resource_name.size);
    object.donor_path = donor_path;
    Snipe_ServerEntity_StaticObject_ChunkDataV0 body{};
    body.m_iStaticObjectVersion = 3;
    body.m_iAsuraStaticObjectVersion = 0;
    body.m_iPhysicalObjectVersion = 7;
    body.m_uSnipePhysicalPropertyA = 999;
    body.m_uSnipePhysicalPropertyC = 999;
    body.m_uSnipePhysicalPropertyD = 999;
    body.m_iAsuraPhysicalObjectVersion = 7;
    body.m_xPhysicalObject.m_xOrientation.w = 1.0f;
    body.m_xPhysicalObject.m_fHealth = 100.0f;
    body.m_xPhysicalObject.m_uFileID = file_id;
    body.m_xPhysicalObject.m_iAnimFlags = 1;
    body.m_xPhysicalObject.m_iBBIndex = -1;
    body.m_xPhysicalObject.m_uStateBits = 0x40;
    body.m_xPhysicalObject.m_uPhysicalObjectFlags = 2;
    memcpy(object.body.data(), &body, sizeof(body));
    return object;
}

std::string static_object_resource_name(const ChunkList& chunks, uint32_t file_id, uint32_t skin_id) {
    for (uint32_t i = 0; i < chunks.count; ++i) {
        RscfInfo resource{};
        if (!rscf_info(chunks.chunks[i], &resource) ||
            resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC)
            continue;
        if (resource.subtype == ASURA_RESOURCEFILE_TYPE_PC_OBJECT && resource.payload_size >= 4 &&
            read_u32(resource.payload) == file_id)
            return std::string(resource.name.data, resource.name.size);
        if (resource.subtype == ASURA_RESOURCEFILE_TYPE_PC_OBJECTHIERARCHY && skin_id &&
            asura_lower_name_hash(resource.name) == skin_id)
            return std::string(resource.name.data, resource.name.size);
        Ps2ObjectView object{};
        Error ignored{};
        if (resource.subtype == kPs2ObjectResourceSubtype &&
            ps2_object_view(resource, &object, &ignored) &&
            asura_lower_name_hash(object.object_path) == file_id)
            return std::string(object.object_path.data, object.object_path.size);
    }
    return {};
}

StaticObjectTemplate static_object_template_from_entity(const Entity& entity, const std::string& donor_path) {
    Snipe_ServerEntity_StaticObject_ChunkDataV0 body{};
    memcpy(&body, entity.static_object_body.data(), sizeof(body));
    StaticObjectTemplate object = make_canonical_static_object_template(
        entity.value_u32_b, Str{entity.name.data(), static_cast<uint32_t>(entity.name.size())}, donor_path);
    object.entity_padding = entity.entity_padding;
    if (entity.static_object_has_template && valid_static_object_body(body) && !body.m_uNumLinksToBlock)
        object.body = entity.static_object_body;
    return object;
}

void note_static_object_template(Document* document, const Entity& entity, const std::string& donor_path = {}) {
    for (const StaticObjectTemplate& object : document->static_object_templates)
        if (object.file_id == entity.value_u32_b)
            return;
    document->static_object_templates.push_back(static_object_template_from_entity(entity, donor_path));
}

bool decode_pc_model_materials(const ChunkList& chunks, uint32_t before_chunk,
                               std::vector<SpawnPuppetMaterial>* output, Error* err) {
    struct MaterialRecord {
        int32_t texture_index = -1;
        uint32_t flags = 0;
        uint32_t texture_flags = 0;
    };
    std::vector<std::string> texture_names;
    std::vector<uint32_t> texture_flags;
    std::vector<MaterialRecord> materials;
    for (uint32_t chunk_index = 0; chunk_index < before_chunk; ++chunk_index) {
        const ChunkRef& chunk = chunks.chunks[chunk_index];
        if (chunk.cid == ASURA_CHUNK_TEXTURENAMES) {
            if (chunk.version > 3 || chunk.size < sizeof(Asura_Chunk_TextureNames))
                return fail(err, "Object preview encountered an unsupported or truncated TEXT chunk");
            const uint32_t count = read_u32(chunk.data + sizeof(Asura_Chunk_Header));
            texture_names.clear();
            texture_names.reserve(count);
            texture_flags.assign(count, 0);
            uint64_t at = sizeof(Asura_Chunk_TextureNames);
            for (uint32_t texture_index = 0; texture_index < count; ++texture_index) {
                if (at > chunk.size)
                    return fail(err, "Object preview TEXT table is truncated");
                const Str name = padded_string_at(chunk.data, chunk.size, static_cast<uint32_t>(at));
                if (!name.data)
                    return fail(err, "Object preview TEXT string is unterminated");
                texture_names.emplace_back(name.data, name.size);
                at = align_up(at + name.size + 1, 4);
            }
            if (chunk.version < 3) {
                materials.assign(count, {});
                for (uint32_t texture_index = 0; texture_index < count; ++texture_index)
                    materials[texture_index].texture_index = static_cast<int32_t>(texture_index);
            }
        } else if (chunk.cid == ASURA_CHUNK_TEXTUREFLAGS) {
            if (chunk.version > 1 || chunk.size < sizeof(Asura_Chunk_Header) + sizeof(uint32_t))
                return fail(err, "Object preview encountered an unsupported or truncated TXFL chunk");
            const uint32_t count = read_u32(chunk.data + sizeof(Asura_Chunk_Header));
            const uint64_t values_at = sizeof(Asura_Chunk_Header) + sizeof(uint32_t);
            if (count > (chunk.size - values_at) / sizeof(uint32_t) || count > texture_flags.size())
                return fail(err, "Object preview TXFL table exceeds its TEXT table");
            for (uint32_t texture_index = 0; texture_index < count; ++texture_index) {
                uint32_t value = read_u32(chunk.data + values_at + texture_index * sizeof(uint32_t));
                if (!chunk.version) {
                    if (texture_index < materials.size())
                        materials[texture_index].flags |= value & 0xDE87u;
                    value &= 0xFFFF2178u;
                }
                texture_flags[texture_index] |= value;
            }
        } else if (chunk.cid == ASURA_CHUNK_MATERIAL) {
            if (chunk.version > 1 || chunk.size < sizeof(Asura_Chunk_Header) + sizeof(uint32_t))
                return fail(err, "Object preview encountered an unsupported or truncated MTRL chunk");
            const uint32_t count = read_u32(chunk.data + sizeof(Asura_Chunk_Header));
            const uint32_t stride = chunk.version ? sizeof(Asura_PC_Material_V1) : 8u;
            const uint64_t records_at = sizeof(Asura_Chunk_Header) + sizeof(uint32_t);
            if (count > (chunk.size - records_at) / stride)
                return fail(err, "Object preview MTRL table is truncated");
            materials.assign(count, {});
            for (uint32_t material_index = 0; material_index < count; ++material_index) {
                const uint8_t* record = chunk.data + records_at + static_cast<uint64_t>(material_index) * stride;
                materials[material_index].texture_index = static_cast<int32_t>(read_u32(record));
                materials[material_index].flags = read_u32(record + 4);
            }
        }
    }

    output->assign(materials.size(), {});
    for (uint32_t material_index = 0; material_index < materials.size(); ++material_index) {
        const MaterialRecord& source = materials[material_index];
        SpawnPuppetMaterial& material = (*output)[material_index];
        material.flags = source.flags;
        if (source.texture_index < 0 || static_cast<uint32_t>(source.texture_index) >= texture_names.size())
            continue;
        material.texture_name = texture_names[source.texture_index];
        if (static_cast<uint32_t>(source.texture_index) < texture_flags.size())
            material.texture_flags = texture_flags[source.texture_index];
        const Str wanted{material.texture_name.data(), static_cast<uint32_t>(material.texture_name.size())};
        for (uint32_t resource_index = 0; resource_index < chunks.count; ++resource_index) {
            RscfInfo texture{};
            if (!rscf_info(chunks.chunks[resource_index], &texture) ||
                texture.type != ASURA_RESOURCEFILE_TYPE_TEXTURE ||
                !text_name_matches_resource(wanted, texture.name))
                continue;
            material.texture_bytes.assign(texture.payload, texture.payload + texture.payload_size);
            material.texture_fingerprint = 1469598103934665603ull;
            for (uint8_t byte : material.texture_bytes) {
                material.texture_fingerprint ^= byte;
                material.texture_fingerprint *= 1099511628211ull;
            }
            break;
        }
    }
    return true;
}

bool decode_pc_static_object_model(const ChunkList& chunks, uint32_t chunk_index,
                                   const RscfInfo& resource, StaticObjectModel* output, Error* err) {
    constexpr uint32_t header_size = 20;
    constexpr uint32_t vertex_stride = 32;
    if (resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
        resource.subtype != ASURA_RESOURCEFILE_TYPE_PC_OBJECT || resource.payload_size < header_size)
        return false;
    const uint32_t file_id = read_u32(resource.payload);
    const uint32_t triangle_count = read_u32(resource.payload + 4);
    const uint32_t vertex_count = read_u32(resource.payload + 8);
    const uint32_t index_count = read_u32(resource.payload + 12);
    const uint64_t indices_at = header_size + static_cast<uint64_t>(vertex_count) * vertex_stride;
    const uint64_t required = indices_at + static_cast<uint64_t>(index_count) * sizeof(uint16_t) + 8;
    if (!triangle_count || !vertex_count)
        return false;
    if (vertex_count > 65535 || index_count < 3 || triangle_count > index_count - 2 ||
        required > resource.payload_size)
        return fail(err, "Object resource '%.*s' has invalid geometry counts", resource.name.size,
                    resource.name.data);

    StaticObjectModel next;
    next.file_id = file_id;
    next.mesh.resource_name.assign(resource.name.data, resource.name.size);
    if (!decode_pc_model_materials(chunks, chunk_index, &next.mesh.materials, err))
        return false;
    next.mesh.vertices.resize(vertex_count);
    for (uint32_t i = 0; i < vertex_count; ++i) {
        const uint8_t* source = resource.payload + header_size + static_cast<uint64_t>(i) * vertex_stride;
        SpawnPuppetVertex& vertex = next.mesh.vertices[i];
        vertex.position = {read_f32(source), read_f32(source + 4), read_f32(source + 8)};
        vertex.normal = {read_f32(source + 12), read_f32(source + 16), read_f32(source + 20)};
        vertex.texcoord = {read_f32(source + 24), read_f32(source + 28)};
        if (!isfinite(vertex.position.x) || !isfinite(vertex.position.y) || !isfinite(vertex.position.z) ||
            !isfinite(vertex.normal.x) || !isfinite(vertex.normal.y) || !isfinite(vertex.normal.z) ||
            !isfinite(vertex.texcoord.x) || !isfinite(vertex.texcoord.y))
            return fail(err, "Object resource '%.*s' contains non-finite vertices", resource.name.size,
                        resource.name.data);
        const float length = sqrtf(vertex.normal.x * vertex.normal.x + vertex.normal.y * vertex.normal.y +
                                   vertex.normal.z * vertex.normal.z);
        if (length > 1.0e-5f) {
            vertex.normal.x /= length;
            vertex.normal.y /= length;
            vertex.normal.z /= length;
        } else {
            vertex.normal = {0, -1, 0};
        }
        if (!i) {
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
    next.mesh.faces.reserve(triangle_count);
    for (uint32_t triangle = 0; triangle < triangle_count; ++triangle) {
        uint16_t a = read_u16(indices + static_cast<uint64_t>(triangle) * 2);
        uint16_t b = read_u16(indices + static_cast<uint64_t>(triangle + 1) * 2);
        uint16_t c = read_u16(indices + static_cast<uint64_t>(triangle + 2) * 2);
        if (triangle & 1)
            std::swap(a, b);
        if (a == 0xffff || b == 0xffff || c == 0xffff)
            continue;
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count)
            return fail(err, "Object resource '%.*s' has an out-of-range index", resource.name.size,
                        resource.name.data);
        if (a != b && b != c && a != c) {
            next.mesh.faces.push_back({a, b, c});
            next.mesh.face_materials.push_back(static_cast<int32_t>(read_u32(resource.payload + 16)));
        }
    }
    if (next.mesh.faces.empty())
        return false;
    *output = std::move(next);
    return true;
}

bool decode_ps2_static_object_models(const ChunkList& chunks, const Document& document,
                                     std::vector<StaticObjectModel>* output, Error* err) {
    std::vector<StaticObjectModel> next_models;
    std::vector<float> selected_lods;
    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        RscfInfo resource{};
        if (!rscf_info(chunks.chunks[chunk_index], &resource) ||
            resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
            resource.subtype != kPs2ObjectResourceSubtype)
            continue;
        Ps2ObjectView object{};
        if (!ps2_object_view(resource, &object, err))
            return false;
        const uint32_t file_id = asura_lower_name_hash(object.object_path);
        bool referenced = false;
        for (const Entity& entity : document.entities)
            referenced |= entity.kind == EntityKind::StaticObject && entity.value_u32_b == file_id;
        if (!referenced || !object.triangle_count || !object.packet_count)
            continue;

        size_t model_index = next_models.size();
        for (size_t existing = 0; existing < next_models.size(); ++existing) {
            if (next_models[existing].file_id == file_id) {
                model_index = existing;
                break;
            }
        }
        if (model_index < next_models.size() && selected_lods[model_index] <= object.lod_distance)
            continue;

        Mesh decoded;
        uint32_t decoded_triangles = 0;
        uint32_t packet_at = 0;
        for (uint32_t packet = 0; packet < object.packet_count; ++packet) {
            const uint32_t qwords = read_u16(object.packet_table + packet * 4);
            const uint32_t bytes = qwords * 16;
            if (!qwords || bytes > object.packet_bytes - packet_at)
                return fail(err, "PS2 object '%.*s' has an invalid VIF packet",
                            object.object_path.size, object.object_path.data);
            if (!decode_ps2_vif_stream(object.packets + packet_at, bytes, 0, &decoded,
                                       &decoded_triangles, err))
                return false;
            packet_at += bytes;
        }
        if (packet_at != object.packet_bytes || decoded_triangles != object.triangle_count)
            return fail(err, "PS2 object '%.*s' decoded %u triangles; metadata declares %u",
                        object.object_path.size, object.object_path.data, decoded_triangles,
                        object.triangle_count);
        if (decoded.positions.size() > 65535)
            return fail(err, "PS2 object '%.*s' exceeds the PC object's 16-bit vertex limit",
                        object.object_path.size, object.object_path.data);

        StaticObjectModel model;
        model.file_id = file_id;
        model.mesh.resource_name.assign(object.object_path.data, object.object_path.size);
        model.mesh.vertices.resize(decoded.positions.size());
        for (size_t vertex_index = 0; vertex_index < decoded.positions.size(); ++vertex_index) {
            SpawnPuppetVertex& vertex = model.mesh.vertices[vertex_index];
            vertex.position = decoded.positions[vertex_index];
            vertex.position.y = -vertex.position.y;
            vertex.normal = vertex_index < decoded.normals.size()
                                ? decoded.normals[vertex_index]
                                : Asura_Vector_3{0, -1, 0};
            vertex.normal.y = -vertex.normal.y;
            if (vertex_index < decoded.texcoords.size())
                vertex.texcoord = decoded.texcoords[vertex_index];
            if (!vertex_index) {
                model.mesh.min = model.mesh.max = vertex.position;
            } else {
                model.mesh.min.x = fminf(model.mesh.min.x, vertex.position.x);
                model.mesh.min.y = fminf(model.mesh.min.y, vertex.position.y);
                model.mesh.min.z = fminf(model.mesh.min.z, vertex.position.z);
                model.mesh.max.x = fmaxf(model.mesh.max.x, vertex.position.x);
                model.mesh.max.y = fmaxf(model.mesh.max.y, vertex.position.y);
                model.mesh.max.z = fmaxf(model.mesh.max.z, vertex.position.z);
            }
        }
        model.mesh.faces.reserve(decoded.faces.size());
        model.mesh.face_materials.reserve(decoded.faces.size());
        for (const std::array<uint32_t, 3>& face : decoded.faces) {
            if (face[0] >= model.mesh.vertices.size() || face[1] >= model.mesh.vertices.size() ||
                face[2] >= model.mesh.vertices.size())
                return fail(err, "PS2 object '%.*s' contains an out-of-range face",
                            object.object_path.size, object.object_path.data);
            model.mesh.faces.push_back({static_cast<uint16_t>(face[0]),
                                        static_cast<uint16_t>(face[1]),
                                        static_cast<uint16_t>(face[2])});
            model.mesh.face_materials.push_back(0);
        }

        SpawnPuppetMaterial material;
        material.texture_name.assign(object.texture_name.data, object.texture_name.size);
        material.flags = object.material_flags;
        material.texture_flags = 0x2000;
        for (uint32_t texture_index = 0; texture_index < chunks.count; ++texture_index) {
            RscfInfo texture{};
            if (!rscf_info(chunks.chunks[texture_index], &texture) ||
                texture.type != ASURA_RESOURCEFILE_TYPE_TEXTURE ||
                !text_name_matches_resource(object.texture_name, texture.name))
                continue;
            material.texture_bytes.assign(texture.payload, texture.payload + texture.payload_size);
            material.texture_fingerprint = 1469598103934665603ull;
            for (uint8_t byte : material.texture_bytes) {
                material.texture_fingerprint ^= byte;
                material.texture_fingerprint *= 1099511628211ull;
            }
            break;
        }
        model.mesh.materials.push_back(std::move(material));
        if (model_index == next_models.size()) {
            next_models.push_back(std::move(model));
            selected_lods.push_back(object.lod_distance);
        } else {
            next_models[model_index] = std::move(model);
            selected_lods[model_index] = object.lod_distance;
        }
    }
    *output = std::move(next_models);
    return true;
}

bool donor_has_static_shape(const ChunkList& chunks, uint32_t file_id) {
    for (uint32_t i = 0; i < chunks.count; ++i) {
        const ChunkRef& chunk = chunks.chunks[i];
        if (chunk.cid == ASURA_CHUNK_SHAPE && chunk.size >= sizeof(Asura_Chunk_Header) + 4 &&
            read_u32(chunk.data + sizeof(Asura_Chunk_Header)) == file_id)
            return true;
    }
    return false;
}

bool load_static_object_donors(const std::vector<std::string>& paths,
                               std::vector<StaticObjectTemplate>* templates,
                               std::vector<StaticObjectModel>* models, std::string* why) {
    Error err{};
    Arena arena{};
    std::vector<StaticObjectTemplate> next_templates;
    std::vector<StaticObjectModel> next_models;
    bool ok = arena_init(&arena, 128 * MiB, &err);
    for (const std::string& path : paths) {
        ArenaMark mark = arena_mark(&arena);
        ChunkList chunks{};
        ok = ok && parse_chunks(path.c_str(), &chunks, &arena, &err);
        for (uint32_t resource_index = 0; ok && resource_index < chunks.count; ++resource_index) {
            RscfInfo resource{};
            if (!rscf_info(chunks.chunks[resource_index], &resource) ||
                resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
                resource.subtype != ASURA_RESOURCEFILE_TYPE_PC_OBJECT || resource.payload_size < 20)
                continue;
            const uint32_t file_id = read_u32(resource.payload);
            bool duplicate = false;
            for (const StaticObjectTemplate& existing : next_templates)
                duplicate |= existing.file_id == file_id;
            if (duplicate || !donor_has_static_shape(chunks, file_id))
                continue;

            StaticObjectTemplate object = make_canonical_static_object_template(file_id, resource.name, path);
            for (uint32_t entity_index = 0; entity_index < chunks.count; ++entity_index) {
                const ChunkRef& chunk = chunks.chunks[entity_index];
                if (chunk.cid != ASURA_CHUNK_ENTITY || chunk.version != 0 ||
                    chunk.size < sizeof(Asura_Chunk_Entity) + kStaticObjectBodySize)
                    continue;
                const uint8_t* payload = chunk.data + sizeof(Asura_Chunk_Header);
                if (read_u16(payload + offsetof(Asura_Chunk_Entity_PayloadHeader, Classification)) !=
                    SnipeEntityClass_StaticObject)
                    continue;
                Snipe_ServerEntity_StaticObject_ChunkDataV0 body{};
                memcpy(&body, payload + sizeof(Asura_Chunk_Entity_PayloadHeader), sizeof(body));
                if (valid_static_object_body(body) && !body.m_uNumLinksToBlock &&
                    body.m_xPhysicalObject.m_uFileID == file_id) {
                    object.entity_padding =
                        read_u16(payload + offsetof(Asura_Chunk_Entity_PayloadHeader, m_usPadding));
                    memcpy(object.body.data(), &body, sizeof(body));
                    break;
                }
            }
            next_templates.push_back(std::move(object));
            StaticObjectModel model;
            if (decode_pc_static_object_model(chunks, resource_index, resource, &model, &err))
                next_models.push_back(std::move(model));
            else if (err.set)
                ok = false;
        }
        unmap_file(&chunks.file);
        arena_reset(&arena, mark);
        if (!ok)
            break;
    }
    if (ok && next_templates.empty())
        ok = fail(&err, "the selected Objects donors contain no class-0x7 Object resources with shapes");
    if (ok) {
        *templates = std::move(next_templates);
        if (models)
            *models = std::move(next_models);
    } else if (why) {
        *why = err.set ? err.message : "Could not load Objects definitions from the selected donors.";
    }
    arena_release(&arena);
    return ok;
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
        if ((entity.kind == EntityKind::Pickup || entity.kind == EntityKind::StaticObject) &&
            entity.pickup_skin_id == skin_id)
            return true;
    for (const PickupTemplate& pickup : document.pickup_templates)
        if (pickup.skin_id == skin_id)
            return true;
    return false;
}

struct HierarchyBindTransform {
    Asura_Vector_3 position{};
    Asura_Quat orientation{0, 0, 0, 1};
};

Asura_Vector_3 hierarchy_rotate(Asura_Vector_3 value, const Asura_Quat& rotation) {
    const Asura_Vector_3 q{rotation.x, rotation.y, rotation.z};
    const Asura_Vector_3 twice_cross{2.0f * (q.y * value.z - q.z * value.y),
                                     2.0f * (q.z * value.x - q.x * value.z),
                                     2.0f * (q.x * value.y - q.y * value.x)};
    const Asura_Vector_3 second_cross{q.y * twice_cross.z - q.z * twice_cross.y,
                                      q.z * twice_cross.x - q.x * twice_cross.z,
                                      q.x * twice_cross.y - q.y * twice_cross.x};
    return {value.x + rotation.w * twice_cross.x + second_cross.x,
            value.y + rotation.w * twice_cross.y + second_cross.y,
            value.z + rotation.w * twice_cross.z + second_cross.z};
}

Asura_Quat hierarchy_multiply(const Asura_Quat& a, const Asura_Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

HierarchyBindTransform hierarchy_compose(const HierarchyBindTransform& parent,
                                         const HierarchyBindTransform& local) {
    const Asura_Vector_3 offset = hierarchy_rotate(local.position, parent.orientation);
    return {{parent.position.x + offset.x, parent.position.y + offset.y, parent.position.z + offset.z},
            hierarchy_multiply(parent.orientation, local.orientation)};
}

bool decode_pc_hierarchy_bind_pose(const ChunkList& chunks, uint32_t before_chunk, Str hierarchy_name,
                                   uint32_t strip_count, std::vector<HierarchyBindTransform>* output,
                                   Error* err) {
    output->clear();
    for (uint32_t scan = before_chunk; scan > 0; --scan) {
        const ChunkRef& chunk = chunks.chunks[scan - 1];
        if (chunk.cid != ASURA_CHUNK_HIERARCHY_SKIN)
            continue;
        if (chunk.version < 4 || chunk.version > 6 || chunk.size < sizeof(Asura_Chunk_Header) + 12)
            return fail(err, "ObjectHierarchy '%.*s' has an unsupported HSKN", hierarchy_name.size,
                        hierarchy_name.data);
        const uint8_t* payload = chunk.data + sizeof(Asura_Chunk_Header);
        const uint32_t payload_size = chunk.size - sizeof(Asura_Chunk_Header);
        const uint32_t weighted_vertex_count = read_u32(payload);
        const uint32_t bone_count = read_u32(payload + 4);
        const Str skin_name = padded_string_at(payload, payload_size, 8);
        if (!skin_name.data || !str_ieq(skin_name, hierarchy_name))
            continue;
        if (!bone_count || bone_count > 65535 || bone_count < strip_count)
            return fail(err, "ObjectHierarchy '%.*s' HSKN has fewer bones than strips", hierarchy_name.size,
                        hierarchy_name.data);
        uint64_t at = 8 + align_up(static_cast<uint64_t>(skin_name.size) + 1, 4);
        at += static_cast<uint64_t>(weighted_vertex_count) * 72;
        const uint64_t transforms_at = at + static_cast<uint64_t>(bone_count) * sizeof(uint32_t);
        if (transforms_at + static_cast<uint64_t>(bone_count) * 28 > payload_size)
            return fail(err, "ObjectHierarchy '%.*s' HSKN bind pose is truncated", hierarchy_name.size,
                        hierarchy_name.data);
        std::vector<uint32_t> parents(bone_count);
        std::vector<HierarchyBindTransform> local(bone_count), global(bone_count);
        for (uint32_t bone = 0; bone < bone_count; ++bone) {
            parents[bone] = read_u32(payload + at + static_cast<uint64_t>(bone) * 4);
            const uint8_t* transform = payload + transforms_at + static_cast<uint64_t>(bone) * 28;
            local[bone].position = {read_f32(transform), read_f32(transform + 4), read_f32(transform + 8)};
            local[bone].orientation = {read_f32(transform + 12), read_f32(transform + 16),
                                       read_f32(transform + 20), read_f32(transform + 24)};
        }
        for (uint32_t bone = 0; bone < bone_count; ++bone) {
            const uint32_t parent = parents[bone];
            global[bone] = bone && parent < bone ? hierarchy_compose(global[parent], local[bone]) : local[bone];
        }
        output->assign(global.begin(), global.begin() + strip_count);
        return true;
    }
    // One-bone pickup assets from some toolchains omit HSKN. Their vertices
    // are already root-local, so absence remains a supported identity pose.
    output->assign(strip_count, {});
    return true;
}

bool decode_pc_pickup_model(const ChunkList& chunks, uint32_t chunk_index, const RscfInfo& resource,
                            uint32_t skin_id, PickupModel* output, Error* err) {
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
    if (!decode_pc_model_materials(chunks, chunk_index, &next.mesh.materials, err))
        return false;
    std::vector<HierarchyBindTransform> bind_pose;
    if (!decode_pc_hierarchy_bind_pose(chunks, chunk_index, resource.name, strip_count, &bind_pose, err))
        return false;
    std::vector<SpawnPuppetVertex> source_vertices(vertex_count);
    for (uint32_t i = 0; i < vertex_count; ++i) {
        const uint8_t* source = resource.payload + vertices_at + static_cast<uint64_t>(i) * vertex_stride;
        SpawnPuppetVertex& vertex = source_vertices[i];
        vertex.position = {read_f32(source), read_f32(source + 4), read_f32(source + 8)};
        vertex.normal = {read_f32(source + 12), read_f32(source + 16), read_f32(source + 20)};
        vertex.texcoord = {read_f32(source + 24), read_f32(source + 28)};
        if (!isfinite(vertex.position.x) || !isfinite(vertex.position.y) || !isfinite(vertex.position.z) ||
            !isfinite(vertex.normal.x) || !isfinite(vertex.normal.y) || !isfinite(vertex.normal.z) ||
            !isfinite(vertex.texcoord.x) || !isfinite(vertex.texcoord.y))
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
    }

    const uint8_t* indices = resource.payload + indices_at;
    uint64_t triangle_capacity = 0;
    for (uint32_t strip_index = 0; strip_index < strip_count; ++strip_index)
        triangle_capacity += read_u32(resource.payload + strips_at + static_cast<uint64_t>(strip_index) * strip_stride);
    if (triangle_capacity > SIZE_MAX)
        return fail(err, "pickup ObjectHierarchy '%.*s' has too many triangles", resource.name.size,
                    resource.name.data);
    next.mesh.faces.reserve(static_cast<size_t>(triangle_capacity));
    next.mesh.vertices.reserve(vertex_count);
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
        const HierarchyBindTransform& bind = bind_pose[strip_index];
        std::vector<uint16_t> remapped(number_vertices, 0xffffu);
        const int32_t material_index = static_cast<int32_t>(read_u32(strip + 8));
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
            if (a != b && b != c && a != c) {
                const uint16_t source_indices[3] = {a, b, c};
                uint16_t destination_indices[3]{};
                for (uint32_t corner = 0; corner < 3; ++corner) {
                    const uint32_t source_index = source_indices[corner];
                    if (source_index < lowest_vertex || source_index >= lowest_vertex + number_vertices)
                        return fail(err, "pickup ObjectHierarchy '%.*s' strip index exceeds its vertex range",
                                    resource.name.size, resource.name.data);
                    uint16_t& destination = remapped[source_index - lowest_vertex];
                    if (destination == 0xffffu) {
                        if (next.mesh.vertices.size() >= 65535)
                            return fail(err, "pickup ObjectHierarchy '%.*s' expands beyond preview limits",
                                        resource.name.size, resource.name.data);
                        SpawnPuppetVertex vertex = source_vertices[source_index];
                        const Asura_Vector_3 rotated_position = hierarchy_rotate(vertex.position, bind.orientation);
                        vertex.position = {rotated_position.x + bind.position.x,
                                           rotated_position.y + bind.position.y,
                                           rotated_position.z + bind.position.z};
                        vertex.normal = hierarchy_rotate(vertex.normal, bind.orientation);
                        destination = static_cast<uint16_t>(next.mesh.vertices.size());
                        next.mesh.vertices.push_back(vertex);
                    }
                    destination_indices[corner] = destination;
                }
                next.mesh.faces.push_back(
                    {destination_indices[0], destination_indices[1], destination_indices[2]});
                next.mesh.face_materials.push_back(material_index);
            }
        }
    }
    if (next.mesh.faces.empty())
        return fail(err, "pickup ObjectHierarchy '%.*s' has no renderable triangles",
                    resource.name.size, resource.name.data);
    for (uint32_t i = 0; i < next.mesh.vertices.size(); ++i) {
        const Asura_Vector_3 position = next.mesh.vertices[i].position;
        if (!i) {
            next.mesh.min = next.mesh.max = position;
        } else {
            next.mesh.min.x = fminf(next.mesh.min.x, position.x);
            next.mesh.min.y = fminf(next.mesh.min.y, position.y);
            next.mesh.min.z = fminf(next.mesh.min.z, position.z);
            next.mesh.max.x = fmaxf(next.mesh.max.x, position.x);
            next.mesh.max.y = fmaxf(next.mesh.max.y, position.y);
            next.mesh.max.z = fmaxf(next.mesh.max.z, position.z);
        }
    }
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
        if (!decode_pc_pickup_model(chunks, chunk_index, resource, skin_id, &model, err))
            return false;
        models->push_back(std::move(model));
    }
    return true;
}

bool valid_physical_pickup_body(const Snipe_ServerEntity_Pickup_ChunkDataV0& body) {
    return body.m_iPickupVersion == 2 && body.m_iAsuraPickupVersion == 2 &&
           body.m_iStaticObjectVersion == 3 && body.m_iAsuraStaticObjectVersion == 0 &&
           body.m_iPhysicalObjectVersion == 7 && body.m_iAsuraPhysicalObjectVersion == 7;
}

bool valid_ps2_static_object_body(const Snipe_PS2_ServerEntity_StaticObject_ChunkDataV0& body) {
    return body.m_iStaticObjectVersion == 2 && body.m_iAsuraStaticObjectVersion == 0 &&
           body.m_iPhysicalObjectVersion == 6 && body.m_iAsuraPhysicalObjectVersion == 7;
}

bool valid_ps2_pickup_body(const Snipe_PS2_ServerEntity_Pickup_ChunkDataV0& body) {
    return body.m_iPickupVersion == 2 && body.m_iAsuraPickupVersion == 2 &&
           valid_ps2_static_object_body(body.m_xStaticObject);
}

Snipe_ServerEntity_StaticObject_ChunkDataV0
pc_static_object_body(const Snipe_PS2_ServerEntity_StaticObject_ChunkDataV0& source) {
    Snipe_ServerEntity_StaticObject_ChunkDataV0 output{};
    output.m_iStaticObjectVersion = 3;
    output.m_uStaticObjectFlags = source.m_uStaticObjectFlags;
    output.m_iAsuraStaticObjectVersion = source.m_iAsuraStaticObjectVersion;
    output.m_iPhysicalObjectVersion = 7;
    output.m_uTeam = source.m_uTeam;
    output.m_uSnipePhysicalFlags = source.m_uSnipePhysicalFlags;
    output.m_uSnipePhysicalPropertyA = source.m_uSnipePhysicalPropertyA;
    output.m_uSnipePhysicalPropertyB = source.m_uSnipePhysicalPropertyB;
    output.m_uSnipePhysicalPropertyC = source.m_uSnipePhysicalPropertyC;
    output.m_uSnipePhysicalPropertyD = 999;
    output.m_uSnipePhysicalPropertyE = 0;
    output.m_iAsuraPhysicalObjectVersion = source.m_iAsuraPhysicalObjectVersion;
    output.m_xPhysicalObject = source.m_xPhysicalObject;
    output.m_uNumLinksToBlock = source.m_uNumLinksToBlock;
    return output;
}

Snipe_ServerEntity_Pickup_ChunkDataV0
pc_pickup_body(const Snipe_PS2_ServerEntity_Pickup_ChunkDataV0& source) {
    Snipe_ServerEntity_Pickup_ChunkDataV0 output{};
    output.m_iPickupVersion = source.m_iPickupVersion;
    output.m_uPickupClassID = source.m_uPickupClassID;
    output.m_uPickupFlags = source.m_uPickupFlags;
    output.m_iAsuraPickupVersion = source.m_iAsuraPickupVersion;
    output.m_uItemID = source.m_uItemID;
    output.m_uPickupPropertyA = source.m_uPickupPropertyA;
    output.m_uPickupPropertyB = source.m_uPickupPropertyB;
    const Snipe_ServerEntity_StaticObject_ChunkDataV0 object =
        pc_static_object_body(source.m_xStaticObject);
    memcpy(reinterpret_cast<uint8_t*>(&output) +
               offsetof(Snipe_ServerEntity_Pickup_ChunkDataV0, m_iStaticObjectVersion),
           &object, sizeof(object));
    return output;
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
        if (classification != SnipeEntityClass_Pickup)
            continue;
        if (chunk.version != 0 ||
            chunk.size < sizeof(Asura_Chunk_Entity) + sizeof(Snipe_ServerEntity_Pickup_ChunkDataV0)) {
            ok = fail(&err, "physical-pickup ENTI chunk %u is truncated or unsupported", chunk_index);
            break;
        }
        Snipe_ServerEntity_Pickup_ChunkDataV0 body{};
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
                                   entity.kind == EntityKind::Pickup || entity.kind == EntityKind::StaticObject;
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
    uint32_t light_number = 0, sound_number = 0, spawn_number = 0, object_number = 0, static_number = 0;
    uint32_t target_number = 0, marker_number = 0, volume_number = 0;
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
        // The PS2 target serializes PHON v8. Its record predates the v9
        // layout used by the PC editor, so leave those sounds preserved in
        // the source chunk instead of interpreting them with the wrong ABI.
        if (chunk.version == 8)
            break;
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
        } else if (classification == SnipeEntityClass_StaticObject) {
            if (chunk.version != 0 || chunk.size < sizeof(Asura_Chunk_Entity) + kStaticObjectBodySize)
                return fail(err, "static-object ENTI chunk %u is truncated or unsupported", chunk_index);
            const uint8_t* body_bytes = payload + sizeof(Asura_Chunk_Entity_PayloadHeader);
            Snipe_ServerEntity_StaticObject_ChunkDataV0 body{};
            memcpy(&body, body_bytes, sizeof(body));
            if (!valid_static_object_body(body))
                return fail(err, "static-object ENTI chunk %u has unsupported payload versions", chunk_index);
            Entity entity;
            entity.kind = EntityKind::StaticObject;
            entity.guid = read_u32(payload);
            entity.source_entity_record = true;
            entity.source_entity_classification = classification;
            entity.entity_padding = read_u16(payload + offsetof(Asura_Chunk_Entity_PayloadHeader, m_usPadding));
            entity.value_a = body.m_xPhysicalObject.m_fHealth;
            entity.value_u32_b = body.m_xPhysicalObject.m_uFileID;
            entity.pickup_skin_id = body.m_xPhysicalObject.m_uSkinID;
            entity.pickup_anim_id = body.m_xPhysicalObject.m_uAnimID;
            entity.pickup_anim_file_id = body.m_xPhysicalObject.m_uAnimFileID;
            entity.static_object_has_template = true;
            memcpy(entity.static_object_body.data(), body_bytes, entity.static_object_body.size());
            entity.position = body.m_xPhysicalObject.m_xPosition;
            entity.rotation = quaternion_euler(body.m_xPhysicalObject.m_xOrientation);
            entity.name = static_object_resource_name(chunks, entity.value_u32_b, entity.pickup_skin_id);
            if (entity.name.empty()) {
                char name[96]{};
                snprintf(name, sizeof(name), "Object %u (%08X)", ++static_number, entity.value_u32_b);
                entity.name = name;
            } else {
                ++static_number;
            }
            note_static_object_template(document, entity);
            document->entities.push_back(std::move(entity));
        } else if (classification == SnipeEntityClass_Pickup) {
            if (chunk.version != 0 || chunk.size < sizeof(Asura_Chunk_Entity) + kPickupBodySize)
                return fail(err, "physical-object ENTI chunk %u is truncated or unsupported", chunk_index);
            const uint8_t* body = payload + sizeof(Asura_Chunk_Entity_PayloadHeader);
            Snipe_ServerEntity_Pickup_ChunkDataV0 pickup{};
            memcpy(&pickup, body, sizeof(pickup));
            if (!valid_physical_pickup_body(pickup))
                return fail(err, "physical-object ENTI chunk %u has unsupported payload versions", chunk_index);
            Entity entity;
            entity.kind = EntityKind::Pickup;
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
        } else if (classification == SnipeEntityClass_PositionMarker ||
                   classification == SnipeEntityClass_BuildingVolume) {
            const bool building_volume = classification == SnipeEntityClass_BuildingVolume;
            constexpr uint32_t body_size = 0x74;
            if (chunk.version != 0 || chunk.size < sizeof(Asura_Chunk_Entity) + body_size)
                return fail(err, "%s ENTI chunk %u is truncated or unsupported",
                            building_volume ? "building-volume" : "position-marker", chunk_index);
            const uint8_t* body = payload + sizeof(Asura_Chunk_Entity_PayloadHeader);
            if (read_u32(body) != 0)
                return fail(err, "%s ENTI chunk %u has unsupported payload version",
                            building_volume ? "building-volume" : "position-marker", chunk_index);
            Entity entity;
            entity.kind = building_volume ? EntityKind::BuildingVolume : EntityKind::PositionMarker;
            entity.guid = read_u32(payload);
            entity.source_entity_record = true;
            entity.source_entity_classification = classification;
            entity.entity_padding = read_u16(
                payload + offsetof(Asura_Chunk_Entity_PayloadHeader, m_usPadding));
            memcpy(&entity.source_bounds, body + 0x4c, sizeof(entity.source_bounds));
            memcpy(&entity.position, body + 0x64, sizeof(entity.position));
            float orientation[9]{};
            memcpy(orientation, body + 4, sizeof(orientation));
            entity.rotation = matrix_euler(orientation);
            entity.value_a = entity.source_bounds.MaxX - entity.source_bounds.MinX;
            entity.value_b = entity.source_bounds.MaxZ - entity.source_bounds.MinZ;
            entity.value_u32_a = read_u32(body + 0x70);
            entity.name = building_volume
                              ? "Building volume " + std::to_string(++volume_number)
                              : "Position marker " + std::to_string(++marker_number);
            document->entities.push_back(std::move(entity));
        }
    }
    document->source_pickup_inventory_complete = true;
    document->source_static_object_inventory_complete = true;
    return true;
}

bool ps2_passthrough_entity_class(uint16_t classification) {
    return classification == AsuraEntityClass_CutsceneController;
}

bool import_ps2_entities(const ChunkList& chunks, Document* document, Error* err) {
    uint32_t spawn_number = 0, pickup_number = 0, static_number = 0;
    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        const ChunkRef& chunk = chunks.chunks[chunk_index];
        if (chunk.cid != ASURA_CHUNK_ENTITY)
            continue;
        if (chunk.version != 0 || chunk.size < sizeof(Asura_Chunk_Entity))
            return fail(err, "PS2 ENTI chunk %u has an unsupported version or size", chunk_index);

        const uint8_t* payload = chunk.data + sizeof(Asura_Chunk_Header);
        const uint32_t guid = read_u32(payload);
        const uint16_t classification = read_u16(
            payload + offsetof(Asura_Chunk_Entity_PayloadHeader, Classification));
        note_document_guid(document, guid);

        if (classification == SnipeEntityClass_SpawnPoint) {
            if (chunk.size != sizeof(Asura_Chunk_Header) +
                                  sizeof(Snipe_ServerEntity_SpawnPoint_ChunkDataV0))
                return fail(err, "PS2 spawnpoint ENTI chunk %u has an unsupported size", chunk_index);
            Snipe_ServerEntity_SpawnPoint_ChunkDataV0 source{};
            memcpy(&source, payload, sizeof(source));
            if (source.m_iVersion != 0)
                return fail(err, "PS2 spawnpoint ENTI chunk %u has unsupported payload version %d",
                            chunk_index, source.m_iVersion);
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
            document->entities.push_back(std::move(entity));
        } else if (classification == SnipeEntityClass_StaticObject) {
            if (chunk.size != sizeof(Asura_Chunk_Entity) +
                                  sizeof(Snipe_PS2_ServerEntity_StaticObject_ChunkDataV0))
                return fail(err, "PS2 static-object ENTI chunk %u has an unsupported size", chunk_index);
            Snipe_PS2_ServerEntity_StaticObject_ChunkDataV0 source{};
            memcpy(&source, payload + sizeof(Asura_Chunk_Entity_PayloadHeader), sizeof(source));
            if (!valid_ps2_static_object_body(source))
                return fail(err, "PS2 static-object ENTI chunk %u has unsupported payload versions",
                            chunk_index);
            const Snipe_ServerEntity_StaticObject_ChunkDataV0 body = pc_static_object_body(source);
            Entity entity;
            entity.kind = EntityKind::StaticObject;
            entity.guid = guid;
            entity.source_entity_record = true;
            entity.source_entity_classification = classification;
            entity.entity_padding = read_u16(
                payload + offsetof(Asura_Chunk_Entity_PayloadHeader, m_usPadding));
            entity.value_a = body.m_xPhysicalObject.m_fHealth;
            entity.value_u32_b = body.m_xPhysicalObject.m_uFileID;
            entity.pickup_skin_id = body.m_xPhysicalObject.m_uSkinID;
            entity.pickup_anim_id = body.m_xPhysicalObject.m_uAnimID;
            entity.pickup_anim_file_id = body.m_xPhysicalObject.m_uAnimFileID;
            entity.static_object_has_template = true;
            memcpy(entity.static_object_body.data(), &body, sizeof(body));
            entity.position = body.m_xPhysicalObject.m_xPosition;
            entity.rotation = quaternion_euler(body.m_xPhysicalObject.m_xOrientation);
            entity.name = static_object_resource_name(chunks, entity.value_u32_b,
                                                      entity.pickup_skin_id);
            if (entity.name.empty()) {
                char name[96]{};
                snprintf(name, sizeof(name), "Object %u (%08X)", ++static_number,
                         entity.value_u32_b);
                entity.name = name;
            }
            note_static_object_template(document, entity);
            document->entities.push_back(std::move(entity));
        } else if (classification == SnipeEntityClass_Pickup) {
            if (chunk.size != sizeof(Asura_Chunk_Entity) +
                                  sizeof(Snipe_PS2_ServerEntity_Pickup_ChunkDataV0))
                return fail(err, "PS2 pickup ENTI chunk %u has an unsupported size", chunk_index);
            Snipe_PS2_ServerEntity_Pickup_ChunkDataV0 source{};
            memcpy(&source, payload + sizeof(Asura_Chunk_Entity_PayloadHeader), sizeof(source));
            if (!valid_ps2_pickup_body(source))
                return fail(err, "PS2 pickup ENTI chunk %u has unsupported payload versions",
                            chunk_index);
            const Snipe_ServerEntity_Pickup_ChunkDataV0 body = pc_pickup_body(source);
            Entity entity;
            entity.kind = EntityKind::Pickup;
            entity.guid = guid;
            entity.source_entity_record = true;
            entity.source_entity_classification = classification;
            entity.entity_padding = read_u16(
                payload + offsetof(Asura_Chunk_Entity_PayloadHeader, m_usPadding));
            entity.value_u32_a = body.m_uItemID;
            entity.value_u32_b = body.m_xPhysicalObject.m_uFileID;
            entity.value_a = body.m_xPhysicalObject.m_fHealth;
            entity.pickup_skin_id = body.m_xPhysicalObject.m_uSkinID;
            entity.pickup_anim_id = body.m_xPhysicalObject.m_uAnimID;
            entity.pickup_anim_file_id = body.m_xPhysicalObject.m_uAnimFileID;
            memcpy(entity.pickup_body.data(), &body, sizeof(body));
            entity.pickup_has_template = true;
            entity.position = body.m_xPhysicalObject.m_xPosition;
            entity.rotation = quaternion_euler(body.m_xPhysicalObject.m_xOrientation);
            const char* item_name = snipe_item_name(entity.value_u32_a);
            ++pickup_number;
            if (item_name) {
                entity.name = std::string(item_name) + " " + std::to_string(pickup_number);
            } else {
                char name[96]{};
                snprintf(name, sizeof(name), "Unknown item %u (0x%02X)", pickup_number,
                         entity.value_u32_a);
                entity.name = name;
            }
            note_pickup_template(document, entity);
            document->entities.push_back(std::move(entity));
        } else if (ps2_passthrough_entity_class(classification)) {
            if (chunk.size < 52)
                return fail(err, "PS2 ENTI 0x%04X chunk %u has an unsupported size",
                            classification, chunk_index);
        } else {
            // Preserve the editor's explicit entity-class contract: unknown
            // project classifications are neither interpreted nor exported.
            continue;
        }
    }
    document->source_pickup_inventory_complete = true;
    document->source_static_object_inventory_complete = true;
    return true;
}

bool decode_pc_static_object_models(const ChunkList& chunks, const Document& document,
                                    std::vector<StaticObjectModel>* models, Error* err) {
    models->clear();
    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        RscfInfo resource{};
        if (!rscf_info(chunks.chunks[chunk_index], &resource) ||
            resource.type != ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC ||
            resource.subtype != ASURA_RESOURCEFILE_TYPE_PC_OBJECT || resource.payload_size < 20)
            continue;
        const uint32_t file_id = read_u32(resource.payload);
        bool referenced = false;
        for (const Entity& entity : document.entities)
            referenced |= entity.kind == EntityKind::StaticObject && entity.value_u32_b == file_id;
        for (const StaticObjectTemplate& object : document.static_object_templates)
            referenced |= object.file_id == file_id;
        if (!referenced)
            continue;
        StaticObjectModel model;
        if (decode_pc_static_object_model(chunks, chunk_index, resource, &model, err))
            models->push_back(std::move(model));
        else if (err->set)
            return false;
    }
    return true;
}

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

bool ps2_skybox_info(const ChunkList& chunks, PcSkyboxInfo* info, Error* err) {
    for (uint32_t chunk_index = 0; chunk_index < chunks.count; ++chunk_index) {
        const ChunkRef& chunk = chunks.chunks[chunk_index];
        if (chunk.cid != ASURA_CHUNK_SKYBOX)
            continue;
        // MCP1 Asura_Chunk_SkyBox::Process identifies v4 as seven padded
        // strings followed by one 32-bit DrawClouds value. Versions before 6
        // serialize left/back in the opposite order from the canonical slots.
        if (chunk.version != 4 ||
            chunk.size < sizeof(Asura_Chunk_Header) +
                             sizeof(Asura_Chunk_SkyBox_PayloadPrefixV7) +
                             ASURA_SKYBOX_V3_V4_TEXTURE_PATH_COUNT * 4 + 4)
            return fail(err, "the .PS2 SKYB chunk has an unsupported version or size");
        const uint8_t* payload = chunk.data + sizeof(Asura_Chunk_Header);
        const uint32_t payload_size = chunk.size - sizeof(Asura_Chunk_Header);
        Asura_Chunk_SkyBox_PayloadPrefixV7 prefix{};
        memcpy(&prefix, payload, sizeof(prefix));
        info->chunk_version = 7; // Re-emit the canonicalized paths in the PC ABI.
        info->red = prefix.m_fRed;
        info->green = prefix.m_fGreen;
        info->blue = prefix.m_fBlue;
        info->orientation = prefix.m_fOrientationAroundYAxis;
        if (!isfinite(info->red) || !isfinite(info->green) || !isfinite(info->blue) ||
            !isfinite(info->orientation))
            return fail(err, "the .PS2 SKYB colour or orientation is invalid");

        uint64_t at = sizeof(prefix);
        for (uint32_t serialized_slot = 0;
             serialized_slot < ASURA_SKYBOX_V3_V4_TEXTURE_PATH_COUNT; ++serialized_slot) {
            if (at > 0xffffffffull)
                return fail(err, "the .PS2 SKYB texture table is invalid");
            const Str name = padded_string_at(payload, payload_size, static_cast<uint32_t>(at));
            if (!name.data)
                return fail(err, "the .PS2 SKYB texture table is truncated");
            const uint32_t canonical_slot =
                serialized_slot == 2 ? 3u : serialized_slot == 3 ? 2u : serialized_slot;
            info->names[canonical_slot] = name;
            at = align_up(at + name.size + 1, 4);
            if (at > payload_size)
                return fail(err, "the .PS2 SKYB texture table is truncated");
        }
        if (at + 4 > payload_size)
            return fail(err, "the .PS2 SKYB cloud parameter is truncated");
        info->draw_clouds = read_u32(payload + at) != 0;
        info->back_texture_is_front_upside_down = false;
        info->right_texture_is_left_upside_down = false;
        return true;
    }
    return fail(err, "the .PS2 contains no SKYB chunk");
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
                   std::vector<PickupModel>* pickup_models,
                   std::vector<StaticObjectModel>* object_models) {
    Error err{};
    Arena arena{};
    ChunkList chunks{};
    Document next_document;
    Mesh next_mesh;
    std::vector<PickupModel> next_pickup_models;
    std::vector<StaticObjectModel> next_object_models;
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
    if (ok) {
        for (StaticObjectTemplate& object : next_document.static_object_templates)
            object.donor_path = path;
    }
    if (ok && pickup_models)
        ok = decode_pc_pickup_models(chunks, next_document, &next_pickup_models, &err);
    if (ok && object_models)
        ok = decode_pc_static_object_models(chunks, next_document, &next_object_models, &err);
    if (ok) {
        next_document.source_pc_path = path;
        next_document.output_path = edited_pc_path(path);
        next_document.dirty = false;
        *document = std::move(next_document);
        *mesh = std::move(next_mesh);
        if (pickup_models)
            *pickup_models = std::move(next_pickup_models);
        if (object_models)
            *object_models = std::move(next_object_models);
    } else if (why) {
        *why = err.set ? err.message : "Could not load the PC level.";
    }
    unmap_file(&chunks.file);
    arena_release(&arena);
    return ok;
}

bool load_ps2_level(const std::string& path, Document* document, Mesh* mesh, std::string* why,
                    std::vector<StaticObjectModel>* object_models) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || _stricmp(path.c_str() + dot, ".ps2") != 0) {
        if (why)
            *why = "The level reader accepts only .PS2 files.";
        return false;
    }
    Error err{};
    Arena arena{};
    ChunkList chunks{};
    Document next_document;
    Mesh next_mesh;
    std::vector<StaticObjectModel> next_object_models;
    bool ok = arena_init(&arena, 64 * MiB, &err) && parse_chunks(path.c_str(), &chunks, &arena, &err);
    bool source_has_skybox = false;
    for (uint32_t chunk_index = 0; ok && chunk_index < chunks.count; ++chunk_index)
        source_has_skybox |= chunks.chunks[chunk_index].cid == ASURA_CHUNK_SKYBOX;
    PcSkyboxInfo skybox_info{};
    if (ok && source_has_skybox)
        ok = ps2_skybox_info(chunks, &skybox_info, &err);
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
    if (ok && !find_ps2_environment(chunks, &environment))
        ok = fail(&err, "the .PS2 contains no PS2StrippedEnv RSCF");
    if (ok)
        ok = decode_ps2_environment(environment, &next_mesh, &err) &&
             import_ps2_entities(chunks, &next_document, &err);
    // PS2 LITE v4 and PHON v8 still use older target ABIs. ENTI is handled by
    // the dedicated importer above, which expands the two short physical-object
    // records and preserves the remaining byte-compatible classifications.
    if (ok)
        add_resource_backed_pickup_templates(chunks, &next_document);
    if (ok && object_models)
        ok = decode_ps2_static_object_models(chunks, next_document, &next_object_models, &err);
    if (ok) {
        for (StaticObjectTemplate& object : next_document.static_object_templates)
            object.donor_path = path;
        // Keep the serialized project field for compatibility with existing
        // .alev files; this path now denotes the source PS2 level.
        next_document.source_pc_path = path;
        next_document.output_path.clear();
        next_document.dirty = false;
        *document = std::move(next_document);
        *mesh = std::move(next_mesh);
        if (object_models)
            *object_models = std::move(next_object_models);
    } else if (why) {
        *why = err.set ? err.message : "Could not load the PS2 level.";
    }
    unmap_file(&chunks.file);
    arena_release(&arena);
    return ok;
}


} // namespace editor
