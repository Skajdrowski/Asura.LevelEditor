#include "AsuraLevelCore.h"

using namespace asura;

namespace asura::level {

// Shared binary/geometry implementation used directly by the level editor.

// sub_4836C0 shares one 0x4000-byte buffer between AABB
// traversal frames (28 bytes each) and returned object IDs (4 bytes each).
// Keep a complete module query comfortably below that workspace limit.
constexpr uint32_t kDefaultMaxCollisionPolys = 3000;

bool parse_chunks(const char* path, ChunkList* out, Arena* arena, Error* err) {
    memset(out, 0, sizeof(*out));
    if (!map_file(path, &out->file, err))
        return false;
    if (out->file.size < 8 || memcmp(out->file.data, kAsuraMagic, 8) != 0)
        return fail(err, "'%s' is not an Asura file", path);

    uint64_t off = 8;
    uint32_t count = 0;
    while (off + sizeof(Asura_Chunk_Header) <= out->file.size) {
        const uint8_t* p = out->file.data + off;
        const uint32_t cid = read_u32(p);
        const uint32_t size = read_u32(p + 4);
        if (!cid || !size)
            break;
        if (size < sizeof(Asura_Chunk_Header) || size > out->file.size - off)
            return fail(err, "'%s' has an invalid chunk at 0x%llx", path, static_cast<unsigned long long>(off));
        ++count;
        off += size;
    }
    out->chunks = arena_array<ChunkRef>(arena, count, err);
    if (count && !out->chunks)
        return false;
    out->count = count;
    off = 8;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = out->file.data + off;
        const uint32_t size = read_u32(p + 4);
        out->chunks[i] = {p, size, read_u32(p), read_u32(p + 8), read_u32(p + 12), i};
        off += size;
    }
    return true;
}

Str padded_string_at(const uint8_t* data, uint32_t size, uint32_t at) {
    if (at >= size)
        return {};
    uint32_t end = at;
    while (end < size && data[end])
        ++end;
    if (end == size)
        return {};
    return {reinterpret_cast<const char*>(data + at), end - at};
}

bool rscf_info(const ChunkRef& ch, RscfInfo* out) {
    if (ch.cid != ASURA_CHUNK_RESOURCEFILE || ch.size < 32)
        return false;
    const uint8_t* p = ch.data + sizeof(Asura_Chunk_Header);
    const uint32_t meta_size = ch.size - sizeof(Asura_Chunk_Header);
    Str name = padded_string_at(p, meta_size, 12);
    if (!name.data)
        return false;
    const uint32_t payload_at = static_cast<uint32_t>(align_up(12ull + name.size + 1, 4));
    const uint32_t payload_size = read_u32(p + 8);
    if (payload_at > meta_size || payload_size > meta_size - payload_at)
        return false;
    *out = {read_u32(p), read_u32(p + 4), payload_size, name, p + payload_at};
    return true;
}

ChunkMark begin_chunk(Buffer* out, ASURA_CHUNKID cid, uint32_t version, uint32_t flags, Error* err) {
    ChunkMark mark{out->size};
    const Asura_Chunk_Header header{cid, 0, static_cast<int32_t>(version), static_cast<int32_t>(flags)};
    buffer_append(out, &header, sizeof(header), err);
    return mark;
}

bool end_chunk(Buffer* out, ChunkMark mark, Error* err) {
    const uint64_t size = out->size - mark.start;
    if (size > 0xffffffffull)
        return fail(err, "chunk exceeds the 32-bit Asura size field");
    return patch_u32(out, mark.start + offsetof(Asura_Chunk_Header, Size), static_cast<uint32_t>(size), err);
}

bool append_chunk_copy(Buffer* out, const ChunkRef& ch, Error* err) {
    return buffer_append(out, ch.data, ch.size, err) != ~0ull;
}

bool append_rscf(Buffer* out, Str name, uint32_t type, uint32_t subtype, const void* payload, uint32_t payload_size,
                 Error* err) {
    ChunkMark mark = begin_chunk(out, ASURA_CHUNK_RESOURCEFILE, 0, 0, err);
    append_u32(out, type, err);
    append_u32(out, subtype, err);
    append_u32(out, payload_size, err);
    if (!append_padded_cstr(out, name, err))
        return false;
    if (buffer_append(out, payload, payload_size, err) == ~0ull)
        return false;
    return end_chunk(out, mark, err);
}

bool patch_fnfo_file_size(Buffer* out, Error* err) {
    if (!out || !out->base || out->size < sizeof(kAsuraMagic) ||
        memcmp(out->base, kAsuraMagic, sizeof(kAsuraMagic)) != 0)
        return fail(err, "generated output is not an Asura file");
    if (out->size > 0xffffffffull)
        return fail(err, "generated Asura file exceeds the 32-bit FNFO size field");

    // MCP2 0x43F1D0 stores this value as the denominator used by the loading
    // progress callback at 0x43F0B0. Original target-game levels store their
    // complete file length here. Patch every copied FNFO as well as the one
    // emitted by append_fnfo so edited and newly authored levels stay exact.
    const uint32_t file_size = static_cast<uint32_t>(out->size);
    uint64_t off = sizeof(kAsuraMagic);
    bool patched = false;
    while (off + sizeof(Asura_Chunk_Header) <= out->size) {
        const uint8_t* chunk = out->base + off;
        const uint32_t cid = read_u32(chunk);
        const uint32_t size = read_u32(chunk + offsetof(Asura_Chunk_Header, Size));
        if (!cid || !size)
            break;
        if (size < sizeof(Asura_Chunk_Header) || size > out->size - off)
            return fail(err, "generated output has an invalid chunk at 0x%llx",
                        static_cast<unsigned long long>(off));
        if (cid == ASURA_CHUNK_FILEINFO) {
            if (size < sizeof(Asura_Chunk_Header) + sizeof(uint32_t))
                return fail(err, "generated output has a truncated FNFO chunk");
            if (!patch_u32(out, off + sizeof(Asura_Chunk_Header), file_size, err))
                return false;
            patched = true;
        }
        off += size;
    }
    if (!patched)
        return fail(err, "generated output contains no FNFO chunk");
    return true;
}

bool initialize_editor_config(const char* obj_path, Config* cfg, Error* err) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->obj = obj_path;
    cfg->env_name = "Env";
    // Target-game environment resources use the original Y/Z-flipped Blender
    // conversion. The level editor compensates Y only in its authoring view.
    cfg->flip_y = true;
    cfg->flip_z = true;
    cfg->diffuse_abgr = 0xff808080u;
    cfg->max_prim_count = 1000;
    cfg->max_collision_polys = kDefaultMaxCollisionPolys;
    cfg->auto_block_xz_cell = 40.0f;
    cfg->texture_prefix = "\\environments\\";
    cfg->arena_reserve = sizeof(void*) == 4 ? 256ull * MiB : 8ull * GiB;
    cfg->output_reserve = sizeof(void*) == 4 ? 128ull * MiB : 4ull * GiB;

    if (!obj_path || !file_exists(obj_path))
        return fail(err, "OBJ not found: %s", obj_path ? obj_path : "");
    return true;
}
Asura_Vector_3 transform_vec(Asura_Vector_3 v, const Config& cfg) {
    if (cfg.flip_y)
        v.y = -v.y;
    if (cfg.flip_z)
        v.z = -v.z;
    return v;
}

bool transform_reverses_winding(const Config& cfg) {
    return cfg.flip_y ^ cfg.flip_z;
}

uint8_t clamp_u8(double v) {
    if (v <= 0.0)
        return 0;
    if (v >= 255.0)
        return 255;
    return static_cast<uint8_t>(floor(v + 0.5));
}

bool parse_decimal_token(Str token, float* out) {
    double v = 0;
    if (!parse_f64(token, &v) || !isfinite(v))
        return false;
    *out = static_cast<float>(v);
    return true;
}

uint32_t split_ws(Str line, Str* parts, uint32_t cap) {
    uint32_t count = 0, i = 0;
    while (i < line.size) {
        while (i < line.size && static_cast<unsigned char>(line.data[i]) <= ' ')
            ++i;
        if (i == line.size)
            break;
        const uint32_t start = i;
        while (i < line.size && static_cast<unsigned char>(line.data[i]) > ' ')
            ++i;
        if (count < cap)
            parts[count] = {line.data + start, i - start};
        ++count;
    }
    return count;
}

bool parse_obj_trip(Str s, ObjIndex* out) {
    Str p[3]{};
    uint32_t count = 0, start = 0;
    for (uint32_t i = 0; i <= s.size && count < 3; ++i) {
        if (i == s.size || s.data[i] == '/') {
            p[count++] = {s.data + start, i - start};
            start = i + 1;
        }
    }
    int64_t v[3]{};
    for (uint32_t i = 0; i < count; ++i)
        if (p[i].size && !parse_i64(p[i], &v[i]))
            return false;
    out->v = static_cast<int32_t>(v[0]);
    out->vt = static_cast<int32_t>(v[1]);
    out->vn = static_cast<int32_t>(v[2]);
    return out->v != 0;
}

bool parse_obj(MappedFile* file, ObjData* obj, Arena* arena, Error* err) {
    memset(obj, 0, sizeof(*obj));
    uint64_t nv = 0, nvt = 0, nvn = 0, nf = 0;
    const char* at = reinterpret_cast<const char*>(file->data);
    const char* end = at + file->size;
    while (at < end) {
        const char* line = at;
        while (at < end && *at != '\n' && *at != '\r')
            ++at;
        Str s = str_trim({line, static_cast<uint32_t>(at - line)});
        while (at < end && (*at == '\n' || *at == '\r'))
            ++at;
        if (s.size >= 2 && s.data[0] == 'v' && s.data[1] == ' ')
            ++nv;
        else if (s.size >= 3 && s.data[0] == 'v' && s.data[1] == 't' && s.data[2] == ' ')
            ++nvt;
        else if (s.size >= 3 && s.data[0] == 'v' && s.data[1] == 'n' && s.data[2] == ' ')
            ++nvn;
        else if (s.size >= 2 && s.data[0] == 'f' && s.data[1] == ' ') {
            Str parts[4]{};
            const uint32_t n = split_ws(s, parts, 4);
            if (n >= 4)
                nf += n - 3;
            // Count polygons with more than three vertices without storing every token.
            if (n > 4) {
                uint32_t words = 0;
                bool in = false;
                for (uint32_t i = 0; i < s.size; ++i) {
                    bool now = static_cast<unsigned char>(s.data[i]) > ' ';
                    if (now && !in)
                        ++words;
                    in = now;
                }
                if (words > n)
                    nf += words - n;
            }
        }
    }
    if (!nv || !nf || nv > 0xffffffffull || nvt > 0xffffffffull || nvn > 0xffffffffull || nf > 0xffffffffull)
        return fail(err, "%s: OBJ has no usable geometry or is too large", file->path);
    obj->positions = arena_array<Asura_Vector_3>(arena, nv, err, false);
    obj->colors = arena_array<uint32_t>(arena, nv, err);
    obj->has_color = arena_array<uint8_t>(arena, nv, err);
    obj->texcoords = arena_array<Asura_Vector_2>(arena, nvt, err, false);
    obj->normals = arena_array<Asura_Vector_3>(arena, nvn, err, false);
    obj->faces = arena_array<ObjFace>(arena, nf, err, false);
    if (err->set)
        return false;

    uint32_t vi = 0, vti = 0, vni = 0, fi = 0;
    Str material{};
    at = reinterpret_cast<const char*>(file->data);
    while (at < end) {
        const char* line = at;
        while (at < end && *at != '\n' && *at != '\r')
            ++at;
        Str s = str_trim({line, static_cast<uint32_t>(at - line)});
        while (at < end && (*at == '\n' || *at == '\r'))
            ++at;
        if (!s.size || s.data[0] == '#')
            continue;
        Str parts[16]{};
        const uint32_t n = split_ws(s, parts, 16);
        if (!n)
            continue;
        if (str_eq(parts[0], str_lit("usemtl")) && n >= 2) {
            material = {parts[1].data, static_cast<uint32_t>((s.data + s.size) - parts[1].data)};
            material = str_trim(material);
        } else if (str_eq(parts[0], str_lit("v")) && n >= 4) {
            if (!parse_decimal_token(parts[1], &obj->positions[vi].x) ||
                !parse_decimal_token(parts[2], &obj->positions[vi].y) ||
                !parse_decimal_token(parts[3], &obj->positions[vi].z))
                return fail(err, "%s: invalid vertex", file->path);
            if (n >= 7) {
                float c[4]{}, max_abs = 0;
                for (uint32_t k = 0; k < 3; ++k) {
                    if (!parse_decimal_token(parts[4 + k], &c[k]))
                        return fail(err, "%s: invalid vertex color", file->path);
                    max_abs = fmax(max_abs, fabs(c[k]));
                }
                c[3] = 1.0f;
                if (n >= 8) {
                    if (!parse_decimal_token(parts[7], &c[3]))
                        return fail(err, "%s: invalid alpha", file->path);
                    max_abs = fmax(max_abs, fabs(c[3]));
                }
                const bool normalized = max_abs <= 4.0f;
                const uint8_t r = clamp_u8(normalized ? fmin(1.0f, fmax(0.0f, c[0])) * 255.0 : c[0]);
                const uint8_t g = clamp_u8(normalized ? fmin(1.0f, fmax(0.0f, c[1])) * 255.0 : c[1]);
                const uint8_t b = clamp_u8(normalized ? fmin(1.0f, fmax(0.0f, c[2])) * 255.0 : c[2]);
                const uint8_t a = clamp_u8(normalized ? fmin(1.0f, fmax(0.0f, c[3])) * 255.0 : c[3]);
                obj->colors[vi] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
                                  (static_cast<uint32_t>(g) << 8) | b;
                obj->has_color[vi] = 1;
            }
            ++vi;
        } else if (str_eq(parts[0], str_lit("vt")) && n >= 3) {
            if (!parse_decimal_token(parts[1], &obj->texcoords[vti].x) ||
                !parse_decimal_token(parts[2], &obj->texcoords[vti].y))
                return fail(err, "%s: invalid texcoord", file->path);
            ++vti;
        } else if (str_eq(parts[0], str_lit("vn")) && n >= 4) {
            if (!parse_decimal_token(parts[1], &obj->normals[vni].x) ||
                !parse_decimal_token(parts[2], &obj->normals[vni].y) ||
                !parse_decimal_token(parts[3], &obj->normals[vni].z))
                return fail(err, "%s: invalid normal", file->path);
            ++vni;
        } else if (str_eq(parts[0], str_lit("f"))) {
            // Re-tokenize to support arbitrary polygons without a fixed token array.
            const char *p = s.data + 1, *se = s.data + s.size;
            Str first{};
            ObjIndex v0{};
            uint32_t poly_i = 0;
            ObjIndex prev{};
            while (p < se) {
                while (p < se && static_cast<unsigned char>(*p) <= ' ')
                    ++p;
                const char* ps = p;
                while (p < se && static_cast<unsigned char>(*p) > ' ')
                    ++p;
                if (ps == p)
                    break;
                ObjIndex cur{};
                if (!parse_obj_trip({ps, static_cast<uint32_t>(p - ps)}, &cur))
                    return fail(err, "%s: invalid face", file->path);
                if (poly_i == 0) {
                    v0 = cur;
                    first = {ps, static_cast<uint32_t>(p - ps)};
                } else if (poly_i >= 2) {
                    if (fi >= nf)
                        return fail(err, "internal OBJ face count mismatch");
                    obj->faces[fi] = {v0, prev, cur, material, fi};
                    ++fi;
                }
                prev = cur;
                ++poly_i;
            }
        }
    }
    obj->position_count = vi;
    obj->texcoord_count = vti;
    obj->normal_count = vni;
    obj->face_count = fi;
    if (!fi)
        return fail(err, "%s: OBJ generated zero triangles", file->path);
    return true;
}

} // namespace

namespace asura::level {

Json* json_get_i(Json* object, Str key) {
    if (!object || object->kind != JsonKind::Object)
        return nullptr;
    for (Json* it = object->child; it; it = it->next)
        if (str_ieq(it->key, key))
            return it;
    return nullptr;
}

bool load_material_map(const Config& cfg, MaterialMap* map, Arena* arena, Error* err) {
    memset(map, 0, sizeof(*map));
    if (!cfg.material_map)
        return true;
    if (!map_file(cfg.material_map, &map->file, err))
        return false;
    map->root = json_parse(&map->file, arena, err);
    return map->root != nullptr;
}

bool material_from_mat_name(Str name, uint32_t* out) {
    name = str_trim(name);
    if (name.size < 5 || !str_starts_i(name, str_lit("mat_")))
        return false;
    uint64_t v = 0;
    if (!parse_u64({name.data + 4, name.size - 4}, &v) || v > 0xffffffffull)
        return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

bool json_node_u32(Json* n, uint32_t* out) {
    if (!n)
        return false;
    if (n->kind == JsonKind::Number) {
        if (n->number < 0 || n->number > 4294967295.0)
            return false;
        *out = static_cast<uint32_t>(n->number);
        return true;
    }
    if (n->kind == JsonKind::String) {
        uint64_t v = 0;
        if (parse_u64(n->string, &v) && v <= 0xffffffffull) {
            *out = static_cast<uint32_t>(v);
            return true;
        }
    }
    return false;
}

bool resolve_material(const MaterialMap& map, Str name, bool allow_unknown, uint32_t* out, Error* err) {
    if (material_from_mat_name(name, out)) {
        if (*out == 0xffffffffu)
            *out = 0;
        return true;
    }
    if (map.root && map.root->kind == JsonKind::Object) {
        // Direct {"material name": index} map.
        Json* direct = json_get_i(map.root, str_trim(name));
        if (json_node_u32(direct, out)) {
            if (*out == 0xffffffffu)
                *out = 0;
            return true;
        }

        Json* orig = json_get(map.root, "orig_to_handle");
        Json* names = json_get(map.root, "handle_to_texname");
        if (orig && orig->kind == JsonKind::Object && names && names->kind == JsonKind::Object) {
            const Str wanted = str_trim(name);
            const Str wanted_base = path_basename(wanted);
            for (Json* oi = orig->child; oi; oi = oi->next) {
                uint32_t handle = 0;
                if (!json_node_u32(oi, &handle))
                    continue;
                char key_buf[32];
                const int kn = snprintf(key_buf, sizeof(key_buf), "%u", handle);
                Json* tn = json_get_i(names, {key_buf, static_cast<uint32_t>(kn)});
                if (!tn || tn->kind != JsonKind::String)
                    continue;
                const Str tex = tn->string;
                if (str_ieq(wanted, tex) || str_ieq(wanted_base, path_basename(tex))) {
                    uint64_t original = 0;
                    if (parse_u64(oi->key, &original) && original <= 0xffffffffull) {
                        *out = static_cast<uint32_t>(original);
                        return true;
                    }
                }
            }
        }
    }
    if (allow_unknown) {
        *out = 0;
        return true;
    }
    return fail(err, "unknown OBJ material '%.*s'; choose a material map or export without one",
                name.size, name.data);
}

Str material_texture_name_exact(const MaterialMap& map, uint32_t material_index) {
    if (!map.root || map.root->kind != JsonKind::Object)
        return {};
    Json* indexed = json_get(map.root, "texture_by_material_index");
    if (indexed && indexed->kind == JsonKind::Object) {
        char key[32];
        const int n = snprintf(key, sizeof(key), "%u", material_index);
        Json* texture = json_get_i(indexed, {key, static_cast<uint32_t>(n)});
        if (texture && texture->kind == JsonKind::String)
            return texture->string;
    }
    Json* orig = json_get(map.root, "orig_to_handle");
    Json* names = json_get(map.root, "handle_to_texname");
    if (orig && orig->kind == JsonKind::Object && names && names->kind == JsonKind::Object) {
        char key[32];
        int n = snprintf(key, sizeof(key), "%u", material_index);
        Json* h = json_get_i(orig, {key, static_cast<uint32_t>(n)});
        uint32_t handle = 0;
        if (json_node_u32(h, &handle)) {
            n = snprintf(key, sizeof(key), "%u", handle);
            Json* t = json_get_i(names, {key, static_cast<uint32_t>(n)});
            if (t && t->kind == JsonKind::String)
                return t->string;
        }
    }
    // Direct map is name -> material index.
    for (Json* it = map.root->child; it; it = it->next) {
        uint32_t v = 0;
        if (json_node_u32(it, &v) && v == material_index)
            return it->key;
    }
    return {};
}

Str material_texture_name(const MaterialMap& map, uint32_t material_ordinal) {
    Str name = material_texture_name_exact(map, material_ordinal);
    if (!name.size && material_ordinal <= 0xffffffffu - 1000u)
        name = material_texture_name_exact(map, material_ordinal + 1000u);
    return name;
}

uint32_t material_override(const MaterialMap& map, const char* section, uint32_t material_ordinal, uint32_t fallback) {
    if (!map.root)
        return fallback;
    Json* obj = json_get(map.root, section);
    if (!obj || obj->kind != JsonKind::Object)
        return fallback;
    char key[32];
    int n = snprintf(key, sizeof(key), "%u", material_ordinal);
    Json* v = json_get_i(obj, {key, static_cast<uint32_t>(n)});
    uint32_t out = 0;
    if (json_node_u32(v, &out))
        return out;
    if (material_ordinal <= 0xffffffffu - 1000u) {
        n = snprintf(key, sizeof(key), "%u", material_ordinal + 1000u);
        v = json_get_i(obj, {key, static_cast<uint32_t>(n)});
        if (json_node_u32(v, &out))
            return out;
    }
    return fallback;
}

struct FaceWork {
    uint32_t face_index;
    int32_t gx, gz;
    float cx, cz;
    uint32_t material;
    uint32_t order;
};

bool work_less_group(const FaceWork& a, const FaceWork& b) {
    if (a.gx != b.gx)
        return a.gx < b.gx;
    if (a.gz != b.gz)
        return a.gz < b.gz;
    return a.order < b.order;
}

inline bool vertex_key_eq(const VertexKey& a, const VertexKey& b) {
    return a.v == b.v && a.vt == b.vt && a.vn == b.vn;
}
uint64_t vertex_hash(VertexKey k) {
    uint64_t x = (static_cast<uint64_t>(k.v) << 32) ^
                 (static_cast<uint64_t>(k.vt) * 0x9e3779b185ebca87ull) ^ k.vn;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x ? x : 1;
}
struct VertexSlot {
    VertexKey key;
    uint32_t value;
    uint8_t used;
};
struct LocalTri {
    uint32_t a, b, c, material, order;
};
bool tri_less(const LocalTri& a, const LocalTri& b) {
    return a.material != b.material ? a.material < b.material : a.order < b.order;
}

int32_t resolve_obj_index(int32_t raw, uint32_t count) {
    if (raw > 0) {
        const int64_t x = static_cast<int64_t>(raw) - 1;
        return x < count ? static_cast<int32_t>(x) : -1;
    }
    if (raw < 0) {
        const int64_t x = static_cast<int64_t>(count) + raw;
        return x >= 0 && x < count ? static_cast<int32_t>(x) : -1;
    }
    return -1;
}

bool face_keys(const ObjData& obj, const ObjFace& f, VertexKey keys[3], Error* err) {
    const ObjIndex raw[3] = {f.a, f.b, f.c};
    for (uint32_t i = 0; i < 3; ++i) {
        const int32_t v = resolve_obj_index(raw[i].v, obj.position_count);
        const int32_t vt = raw[i].vt ? resolve_obj_index(raw[i].vt, obj.texcoord_count) : -1;
        const int32_t vn = raw[i].vn ? resolve_obj_index(raw[i].vn, obj.normal_count) : -1;
        if (v < 0 || (raw[i].vt && vt < 0) || (raw[i].vn && vn < 0))
            return fail(err, "OBJ face index out of range");
        keys[i] = {static_cast<uint32_t>(v), vt < 0 ? 0xffffffffu : static_cast<uint32_t>(vt),
                   vn < 0 ? 0xffffffffu : static_cast<uint32_t>(vn)};
    }
    return true;
}

uint32_t next_pow2(uint32_t v) {
    if (v <= 1)
        return 1;
    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

bool build_env(const Config& cfg, const ObjData& obj, const MaterialMap& materials,
               Arena* arena, Arena* scratch, EnvBuild* out, Error* err) {
    memset(out, 0, sizeof(*out));
    FaceWork* work = arena_array<FaceWork>(arena, obj.face_count, err, false);
    if (!work)
        return false;
    for (uint32_t i = 0; i < obj.face_count; ++i) {
        const ObjFace& f = obj.faces[i];
        VertexKey k[3];
        if (!face_keys(obj, f, k, err))
            return false;
        const Asura_Vector_3 a = transform_vec(obj.positions[k[0].v], cfg), b = transform_vec(obj.positions[k[1].v], cfg),
                   c = transform_vec(obj.positions[k[2].v], cfg);
        uint32_t m = 0;
        if (!resolve_material(materials, f.material, cfg.allow_unknown_materials, &m, err))
            return false;
        if (m == 0xffffffffu)
            m = 0;
        work[i] = {i, 0, 0, (a.x + b.x + c.x) / 3.0f, (a.z + b.z + c.z) / 3.0f, m, f.order};
    }
    if (cfg.auto_block_xz_cell > 0.0f) {
        float minx = work[0].cx, minz = work[0].cz;
        for (uint32_t i = 1; i < obj.face_count; ++i) {
            minx = fmin(minx, work[i].cx);
            minz = fmin(minz, work[i].cz);
        }
        const float inv = 1.0f / cfg.auto_block_xz_cell;
        for (uint32_t i = 0; i < obj.face_count; ++i) {
            work[i].gx = static_cast<int32_t>((work[i].cx - minx) * inv);
            work[i].gz = static_cast<int32_t>((work[i].cz - minz) * inv);
        }
    }
    heap_sort(work, obj.face_count, work_less_group);

    Vec<Asura_PC_EnvironmentRenderer_Module> ta =
        make_vec<Asura_PC_EnvironmentRenderer_Module>(arena, err, obj.face_count < 1024 ? obj.face_count : 1024);
    Vec<Asura_PC_EnvironmentRenderer_Strip> tb =
        make_vec<Asura_PC_EnvironmentRenderer_Strip>(arena, err, obj.face_count < 4096 ? obj.face_count : 4096);
    Buffer blocks{};
    if (!buffer_init(&blocks, cfg.output_reserve, err))
        return false;
    uint32_t group_begin = 0, block_index = 0;
    while (group_begin < obj.face_count) {
        uint32_t group_end = group_begin + 1;
        while (group_end < obj.face_count && work[group_end].gx == work[group_begin].gx &&
               work[group_end].gz == work[group_begin].gz)
            ++group_end;
        uint32_t cursor = group_begin;
        while (cursor < group_end) {
            ArenaMark mark = arena_mark(scratch);
            const uint32_t remaining = group_end - cursor;
            uint32_t slot_cap = next_pow2((remaining > 21845 ? 65535 : remaining * 3) * 2 + 1);
            if (slot_cap < 16)
                slot_cap = 16;
            VertexSlot* slots = arena_array<VertexSlot>(scratch, slot_cap, err);
            Asura_PC_EnvironmentRenderer_Vertex* verts =
                arena_array<Asura_PC_EnvironmentRenderer_Vertex>(scratch, 65535, err, false);
            LocalTri* tris = arena_array<LocalTri>(scratch, remaining, err, false);
            if (err->set) {
                buffer_release(&blocks);
                return false;
            }
            uint32_t vert_count = 0, tri_count = 0;
            auto lookup = [&](VertexKey key, uint32_t material, uint32_t* out_index, bool add) -> bool {
                const uint32_t mask = slot_cap - 1;
                uint32_t at = static_cast<uint32_t>(vertex_hash(key)) & mask;
                while (slots[at].used) {
                    if (vertex_key_eq(slots[at].key, key)) {
                        *out_index = slots[at].value;
                        return true;
                    }
                    at = (at + 1) & mask;
                }
                if (!add)
                    return false;
                if (vert_count >= 65535)
                    return false;
                const Asura_Vector_3 p = transform_vec(obj.positions[key.v], cfg);
                Asura_Vector_3 n{0, 0, 1};
                if (key.vn != 0xffffffffu)
                    n = transform_vec(obj.normals[key.vn], cfg);
                Asura_Vector_2 uv{};
                if (key.vt != 0xffffffffu) {
                    uv = obj.texcoords[key.vt];
                    uv.y = 1.0f - uv.y;
                }
                const uint32_t color = obj.has_color[key.v] ? obj.colors[key.v] : cfg.diffuse_abgr;
                verts[vert_count] = {{p.x, p.y, p.z}, {n.x, n.y, n.z}, color, {uv.x, uv.y}};
                slots[at].used = 1;
                slots[at].key = key;
                slots[at].value = vert_count;
                *out_index = vert_count++;
                return true;
            };
            while (cursor < group_end) {
                if (tri_count >= cfg.max_collision_polys)
                    break;
                VertexKey keys[3];
                if (!face_keys(obj, obj.faces[work[cursor].face_index], keys, err)) {
                    buffer_release(&blocks);
                    return false;
                }
                if (transform_reverses_winding(cfg)) {
                    const VertexKey swap = keys[1];
                    keys[1] = keys[2];
                    keys[2] = swap;
                }
                uint32_t missing = 0, tmp = 0;
                for (uint32_t k = 0; k < 3; ++k)
                    if (!lookup(keys[k], work[cursor].material, &tmp, false))
                        ++missing;
                if (vert_count && vert_count + missing > 65535)
                    break;
                uint32_t idx[3];
                for (uint32_t k = 0; k < 3; ++k)
                    if (!lookup(keys[k], work[cursor].material, &idx[k], true)) {
                        buffer_release(&blocks);
                        return fail(err, "failed to index Env vertex");
                    }
                tris[tri_count++] = {idx[0], idx[1], idx[2], work[cursor].material, work[cursor].order};
                ++cursor;
            }
            if (!tri_count) {
                buffer_release(&blocks);
                return fail(err, "could not fit an Env triangle in a uint16_t block");
            }
            heap_sort(tris, tri_count, tri_less);
            const uint64_t block_start = blocks.size;
            append_u32(&blocks, vert_count, err);
            const uint64_t idx_count_at = blocks.size;
            append_u32(&blocks, 0, err);
            buffer_append(&blocks, verts, static_cast<uint64_t>(vert_count) * sizeof(Asura_PC_EnvironmentRenderer_Vertex),
                          err);
            const uint32_t b_start = tb.count;
            uint32_t index_count = 0;
            uint32_t run = 0;
            // An independent triangle strip uses 6*N-2 indices after parity
            // degenerates, therefore it exposes 6*N-4 strip primitives.
            const uint32_t max_tris =
                static_cast<uint32_t>((static_cast<uint64_t>(cfg.max_prim_count) + 4) / 6);
            while (run < tri_count) {
                uint32_t run_end = run + 1;
                while (run_end < tri_count && tris[run_end].material == tris[run].material)
                    ++run_end;
                for (uint32_t part = run; part < run_end; part += max_tris) {
                    const uint32_t part_end = (part + max_tris < run_end) ? part + max_tris : run_end;
                    const uint32_t seg_start = index_count;
                    uint32_t seg_len = 0, min_i = 0xffffffffu, max_i = 0, last = 0;
                    auto put = [&](uint32_t x) {
                        append_u16(&blocks, static_cast<uint16_t>(x), err);
                        ++index_count;
                        ++seg_len;
                        min_i = x < min_i ? x : min_i;
                        max_i = x > max_i ? x : max_i;
                        last = x;
                    };
                    put(tris[part].a);
                    put(tris[part].b);
                    put(tris[part].c);
                    for (uint32_t t = part + 1; t < part_end; ++t) {
                        if (seg_len & 1)
                            put(last);
                        put(last);
                        put(tris[t].a);
                        put(tris[t].a);
                        put(tris[t].b);
                        put(tris[t].c);
                    }
                    if (seg_len & 1)
                        put(last);
                    if (!tb.push({seg_len >= 2 ? seg_len - 2 : 0, seg_start,
                                  static_cast<int32_t>(tris[run].material), min_i,
                                  max_i - min_i + 1})) {
                        buffer_release(&blocks);
                        return false;
                    }
                }
                run = run_end;
            }
            patch_u32(&blocks, idx_count_at, index_count, err);
            if (!ta.push({tb.count - b_start, b_start, block_index++})) {
                buffer_release(&blocks);
                return false;
            }
            arena_reset(scratch, mark);
            (void)block_start;
        }
        group_begin = group_end;
    }
    if (err->set) {
        buffer_release(&blocks);
        return false;
    }
    if (!buffer_init(&out->payload, cfg.output_reserve, err)) {
        buffer_release(&blocks);
        return false;
    }
    append_u32(&out->payload, ta.count, err);
    append_u32(&out->payload, tb.count, err);
    append_u32(&out->payload, ta.count, err);
    buffer_append(&out->payload, ta.data, static_cast<uint64_t>(ta.count) * sizeof(Asura_PC_EnvironmentRenderer_Module), err);
    buffer_append(&out->payload, tb.data, static_cast<uint64_t>(tb.count) * sizeof(Asura_PC_EnvironmentRenderer_Strip), err);
    buffer_append(&out->payload, blocks.base, blocks.size, err);
    out->modules = ta.count;
    out->strip_count = tb.count;
    buffer_release(&blocks);
    return !err->set;
}

} // namespace

namespace asura::level {

bool env_view(const Buffer& payload, EnvView* v, Arena* arena, Error* err) {
    memset(v, 0, sizeof(*v));
    if (payload.size < 12 || payload.size > 0xffffffffull)
        return fail(err, "generated Env payload is invalid");
    const uint8_t* p = payload.base;
    const uint32_t ac = read_u32(p), bc = read_u32(p + 4), cc = read_u32(p + 8);
    const uint64_t table_a_at = 12;
    const uint64_t table_b_at = table_a_at + static_cast<uint64_t>(ac) * 12;
    uint64_t off = table_b_at + static_cast<uint64_t>(bc) * 20;
    if (off > payload.size)
        return fail(err, "generated Env tables exceed payload");
    const uint8_t** blocks = arena_array<const uint8_t*>(arena, cc, err, false);
    uint32_t* sizes = arena_array<uint32_t>(arena, cc, err, false);
    if (err->set)
        return false;
    for (uint32_t i = 0; i < cc; ++i) {
        if (off + 8 > payload.size)
            return fail(err, "generated Env block header is truncated");
        const uint32_t nv = read_u32(p + off), ni = read_u32(p + off + 4);
        const uint64_t sz = 8ull + static_cast<uint64_t>(nv) * 36 + static_cast<uint64_t>(ni) * 2;
        if (sz > payload.size - off || sz > 0xffffffffull)
            return fail(err, "generated Env block is truncated");
        blocks[i] = p + off;
        sizes[i] = static_cast<uint32_t>(sz);
        off += sz;
    }
    if (off != payload.size)
        return fail(err, "generated Env has %llu trailing bytes", static_cast<unsigned long long>(payload.size - off));
    v->data = p;
    v->size = static_cast<uint32_t>(payload.size);
    v->modules = reinterpret_cast<const Asura_PC_EnvironmentRenderer_Module*>(p + table_a_at);
    v->strips = reinterpret_cast<const Asura_PC_EnvironmentRenderer_Strip*>(p + table_b_at);
    v->module_count = ac;
    v->strip_count = bc;
    v->block_count = cc;
    v->blocks = blocks;
    v->block_sizes = sizes;
    return true;
}

struct CollTri {
    uint16_t a, b, c, flags, material;
};
bool append_minimal_collision_v0(Buffer* out, Error* err) {
    const float bounds[6] = {-1, 1, -1, 1, -1, 1};
    const float radius = sqrt(12.0f) * 0.5f;
    const float verts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint16_t poly[4] = {0, 1, 2, 0};
    append_u32(out, 0, err);
    append_u32(out, 3, err);
    append_u32(out, 1, err);
    append_u32(out, 0, err);
    append_u32(out, 0, err);
    buffer_append(out, bounds, sizeof(bounds), err);
    append_f32(out, radius, err);
    buffer_append(out, verts, sizeof(verts), err);
    buffer_append(out, poly, sizeof(poly), err);
    return !err->set;
}

bool append_module_collision_v3(Buffer* out, const EnvView& env, uint32_t module, const Config& cfg,
                                const MaterialMap& materials, Arena* scratch, ModuleMetric* metric, Error* err) {
    if (module >= env.module_count)
        return fail(err, "collision module %u exceeds Env module count %u", module, env.module_count);
    const Asura_PC_EnvironmentRenderer_Module a = env.modules[module];
    if (a.m_uBufferIndex >= env.block_count)
        return fail(err, "Env module %u references missing buffer %u", module, a.m_uBufferIndex);
    const uint8_t* block = env.blocks[a.m_uBufferIndex];
    const uint32_t block_size = env.block_sizes[a.m_uBufferIndex];
    const uint32_t nv = read_u32(block), ni = read_u32(block + 4);
    const uint8_t* vertices = block + 8;
    const uint8_t* indices = vertices + static_cast<uint64_t>(nv) * 36;
    if (!nv || nv > 65535) {
        metric->translation = {0, 0, 0};
        metric->vertex_count = 3;
        metric->triangle_count = 1;
        return append_minimal_collision_v0(out, err);
    }
    uint64_t max_tris = 0;
    for (uint32_t k = 0; k < a.m_uNumberOfStrips && a.m_uFirstStrip + k < env.strip_count; ++k)
        max_tris += env.strips[a.m_uFirstStrip + k].m_uNumberOfTriangles;
    if (max_tris > 0xffffffffull)
        return fail(err, "Env module %u has too many collision triangles", module);
    CollTri* tris = arena_array<CollTri>(scratch, max_tris ? max_tris : 1, err, false);
    if (!tris)
        return false;
    uint32_t nt = 0;
    for (uint32_t k = 0; k < a.m_uNumberOfStrips; ++k) {
        const uint32_t bj = a.m_uFirstStrip + k;
        if (bj >= env.strip_count)
            break;
        const Asura_PC_EnvironmentRenderer_Strip seg = env.strips[bj];
        const uint64_t seg_count = static_cast<uint64_t>(seg.m_uNumberOfTriangles) + 2;
        if (static_cast<uint64_t>(seg.m_uStartIndex) + seg_count > ni)
            return fail(err, "Env strip exceeds block index buffer");
        for (uint32_t j = 0; j < seg.m_uNumberOfTriangles; ++j) {
            uint16_t x = read_u16(indices + (static_cast<uint64_t>(seg.m_uStartIndex) + j) * 2);
            uint16_t y = read_u16(indices + (static_cast<uint64_t>(seg.m_uStartIndex) + j + 1) * 2);
            uint16_t z = read_u16(indices + (static_cast<uint64_t>(seg.m_uStartIndex) + j + 2) * 2);
            if (j & 1) {
                uint16_t t = x;
                x = y;
                y = t;
            }
            if (x == 0xffff || y == 0xffff || z == 0xffff || x >= nv || y >= nv || z >= nv || x == y || y == z ||
                x == z)
                continue;
            uint32_t m = seg.m_iOriginalMaterialIndex < 0
                             ? 0xffffu
                             : static_cast<uint32_t>(seg.m_iOriginalMaterialIndex);
            if (m > 0xffff)
                m = 0xffff;
            const uint32_t material_ordinal = m >= 1000 ? m - 1000 : m;
            const uint32_t flags = material_override(materials, "collision_flags", material_ordinal, 0);
            if (flags > 0xffff)
                return fail(err, "collision_flags[%u] exceeds a u16 mask", material_ordinal);
            tris[nt++] = {x, y, z, static_cast<uint16_t>(flags), static_cast<uint16_t>(m)};
        }
    }
    if (!nt) {
        metric->translation = {0, 0, 0};
        metric->vertex_count = 3;
        metric->triangle_count = 1;
        return append_minimal_collision_v0(out, err);
    }
    if (nt > kMaxAabbTreeObjects)
        return fail(err, "Env module %u has %u collision triangles; the 2005 AABB tree supports at most %u", module,
                    nt, kMaxAabbTreeObjects);
    Asura_Vector_3 mn{read_f32(vertices), read_f32(vertices + 4), read_f32(vertices + 8)}, mx = mn;
    for (uint32_t i = 1; i < nv; ++i) {
        const uint8_t* q = vertices + static_cast<uint64_t>(i) * 36;
        Asura_Vector_3 p{read_f32(q), read_f32(q + 4), read_f32(q + 8)};
        mn.x = fmin(mn.x, p.x);
        mn.y = fmin(mn.y, p.y);
        mn.z = fmin(mn.z, p.z);
        mx.x = fmax(mx.x, p.x);
        mx.y = fmax(mx.y, p.y);
        mx.z = fmax(mx.z, p.z);
    }
    Asura_Vector_3 c{(mn.x + mx.x) * .5f, (mn.y + mx.y) * .5f, (mn.z + mx.z) * .5f};
    const float bounds[6] = {mn.x - c.x, mx.x - c.x, mn.y - c.y,
                             mx.y - c.y, mn.z - c.z, mx.z - c.z};
    const float rx = bounds[1] - bounds[0], ry = bounds[3] - bounds[2], rz = bounds[5] - bounds[4];
    const float radius = .5f * sqrt(rx * rx + ry * ry + rz * rz);
    metric->translation = c;
    metric->vertex_count = nv;
    metric->triangle_count = nt;
    const uint16_t overall_flags = 0;
    bool has_polygon_flags = false;
    for (uint32_t i = 0; i < nt; ++i)
        if (tris[i].flags != overall_flags) {
            has_polygon_flags = true;
            break;
        }
    append_u32(out, 3, err);
    append_u32(out, nv, err);
    append_u32(out, nt, err);
    append_u32(out, has_polygon_flags ? 1 : 0, err);
    append_u32(out, 1, err);
    append_u16(out, overall_flags, err);
    append_u16(out, 0xffff, err);
    buffer_append(out, bounds, sizeof(bounds), err);
    append_f32(out, radius, err);
    for (uint32_t i = 0; i < nv; ++i) {
        const uint8_t* q = vertices + static_cast<uint64_t>(i) * 36;
        append_f32(out, read_f32(q) - c.x, err);
        append_f32(out, read_f32(q + 4) - c.y, err);
        append_f32(out, read_f32(q + 8) - c.z, err);
    }
    for (uint32_t i = 0; i < nt; ++i) {
        append_u16(out, tris[i].a, err);
        append_u16(out, tris[i].b, err);
        append_u16(out, tris[i].c, err);
        append_u16(out, 0xffff, err);
    }
    if (has_polygon_flags)
        for (uint32_t i = 0; i < nt; ++i)
            append_u16(out, tris[i].flags, err);
    for (uint32_t i = 0; i < nt; ++i)
        append_u16(out, tris[i].material, err);
    (void)block_size;
    return !err->set;
}

bool append_emod(Buffer* out, const EnvView& env, uint32_t modules, const Config& cfg,
                 const MaterialMap& materials, Arena* scratch, ModuleMetric* metrics, Error* err) {
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_ENVIRONMENT_MODULELIST, 6, 0, err);
    append_u32(out, modules, err);
    append_u32(out, 0, err); // empty padded environment name
    for (uint32_t i = 0; i < modules; ++i) {
        ArenaMark mark = arena_mark(scratch);
        append_u32(out, 0, err); // empty padded module name
        Asura_Chunk_Environment_ModuleList_EntryV6 module_data{};
        module_data.m_xModule.m_uReqdRegions = 1;
        const uint64_t module_data_at = out->size;
        buffer_append(out, &module_data, sizeof(module_data), err);
        const uint64_t blob_at = out->size;
        if (!append_module_collision_v3(out, env, i, cfg, materials, scratch, &metrics[i], err))
            return false;
        patch_u32(out, module_data_at + offsetof(Asura_Chunk_Environment_ModuleList_EntryV6, m_uCollisionDataSize),
                  static_cast<uint32_t>(out->size - blob_at), err);
        buffer_patch(out, module_data_at + offsetof(Asura_Chunk_Environment_ModuleList_EntryV6, m_xTranslation),
                     &metrics[i].translation, sizeof(metrics[i].translation), err);
        arena_reset(scratch, mark);
    }
    return end_chunk(out, ch, err);
}

bool append_mlin(Buffer* out, ModuleMetric* metrics, uint32_t module_count, Error* err) {
    const uint64_t total_links = static_cast<uint64_t>(module_count) * (module_count ? module_count - 1 : 0);
    const uint64_t chunk_size = sizeof(Asura_Chunk_Header) + sizeof(uint32_t) +
                                static_cast<uint64_t>(module_count) * sizeof(uint32_t) +
                                total_links * sizeof(uint32_t);
    if (chunk_size > 0xffffffffull)
        return fail(err, "all-to-all MLIN exceeds the 32-bit Asura chunk size");
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_MODULELINKING, 1, 0, err);
    append_u32(out, module_count, err);
    for (uint32_t i = 0; i < module_count; ++i) {
        metrics[i].link_count = module_count - 1;
        append_u32(out, metrics[i].link_count, err);
        for (uint32_t j = 0; j < module_count; ++j)
            if (j != i)
                append_u32(out, j, err);
    }
    return end_chunk(out, ch, err);
}

bool append_mrvb(Buffer* out, uint32_t n, Error* err) {
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_MODULEREVERB, 0, 0, err);
    append_u32(out, n, err);
    append_u32(out, 0, err);
    for (uint32_t i = 0; i < n; ++i)
        append_u32(out, 0, err);
    return end_chunk(out, ch, err);
}
bool append_nav1(Buffer* out, uint32_t n, Error* err) {
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_NAVIGATION, 9, 0, err);
    append_u32(out, n, err);
    append_u32(out, 0, err);
    buffer_append(out, nullptr, static_cast<uint64_t>(n) * 32, err);
    append_u32(out, 0, err);
    append_u32(out, 0, err);
    buffer_append(out, nullptr, 24, err);
    if (n > 1)
        buffer_append(out, nullptr, static_cast<uint64_t>(n - 1) * 12, err);
    return end_chunk(out, ch, err);
}

bool append_fog(Buffer* out, Error* err) {
    const Asura_Chunk_Fog_ChunkDataV0 fog{{0.4705885946750641f, 0.41176480054855347f, 0.29411765933036804f, 0.0f},
                                    0.0f,
                                    339.0f,
                                    1.0f,
                                    0.29999998211860657f};
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_FOG, 0, 1, err);
    buffer_append(out, &fog, sizeof(fog), err);
    return end_chunk(out, ch, err);
}

bool append_wthr(Buffer* out, Error* err) {
    const OldVersionExtraHeaderData extra_header{0, {0x5F000001, 0x49F71FF8, 0x3F400000, 0, 0}};
    const Asura_Chunk_WeatherSystem_ChunkDataV6 weather{
        1,     {-241.931518555f, -32.9773826599f, -205.708862305f},
        0.09f, 0.08f,
        0.07f, {0.0f, 0.77f, 1.5f, 0.0f, 0.67f, 1.5f, 0.0f, 0.62f, 1.5f, 0.0f, 0.84f, 2.0f}};
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_WEATHERSYSTEM, 6, 0, err);
    buffer_append(out, &extra_header, sizeof(extra_header), err);
    buffer_append(out, &weather, sizeof(weather), err);
    return end_chunk(out, ch, err);
}

bool append_fnfo(Buffer* out, Error* err) {
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_FILEINFO, 0, 0, err);
    append_u32(out, 0, err);
    return end_chunk(out, ch, err);
}

} // namespace

namespace asura::level {

bool disk_less(const DiskFile& a, const DiskFile& b) {
    return _stricmp(a.name, b.name) < 0;
}

bool list_files(const char* dir, Arena* arena, Vec<DiskFile>* files, Error* err) {
    *files = make_vec<DiskFile>(arena, err, 32);
    char mask[MAX_PATH];
    if (!join_path(mask, MAX_PATH, dir, str_lit("*")))
        return fail(err, "directory path too long: %s", dir);
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(mask, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return fail(err, "cannot list '%s' (win32=%lu)", dir, GetLastError());
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !strcmp(fd.cFileName, ".") ||
            !strcmp(fd.cFileName, ".."))
            continue;
        DiskFile f{};
        if (!join_path(f.path, MAX_PATH, dir, str_from_c(fd.cFileName))) {
            FindClose(h);
            return fail(err, "file path too long under %s", dir);
        }
        snprintf(f.name, sizeof(f.name), "%s", fd.cFileName);
        f.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        if (!files->push(f)) {
            FindClose(h);
            return false;
        }
    } while (FindNextFileA(h, &fd));
    const DWORD e = GetLastError();
    FindClose(h);
    if (e != ERROR_NO_MORE_FILES)
        return fail(err, "cannot enumerate '%s' (win32=%lu)", dir, e);
    heap_sort(files->data, files->count, disk_less);
    return true;
}

bool append_text(Buffer* out, Str* names, uint32_t count, Error* err) {
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_TEXTURENAMES, 3, 0, err);
    append_u32(out, count, err);
    for (uint32_t i = 0; i < count; ++i)
        append_padded_cstr(out, names[i], err);
    return end_chunk(out, ch, err);
}
bool append_txfl(Buffer* out, uint32_t count, Error* err) {
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_TEXTUREFLAGS, 1, 0, err);
    append_u32(out, count, err);
    for (uint32_t i = 0; i < count; ++i)
        append_u32(out, 8, err);
    return end_chunk(out, ch, err);
}
bool append_mtrl(Buffer* out, const int32_t* tex, const uint32_t* flags, const uint32_t* surface, uint32_t count,
                 Error* err) {
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_MATERIAL, 1, 0, err);
    append_u32(out, count, err);
    for (uint32_t i = 0; i < count; ++i) {
        append_u32(out, tex && tex[i] >= 0 ? static_cast<uint32_t>(tex[i]) : 0xffffffffu, err);
        append_u32(out, flags ? flags[i] : 0, err);
        append_u32(out, surface ? surface[i] : 1, err);
    }
    return end_chunk(out, ch, err);
}

bool append_file_rscf(Buffer* out, Str name, uint32_t type, uint32_t subtype,
                      const char* path, Arena* scratch, Error* err) {
    ArenaMark mark = arena_mark(scratch);
    MappedFile file{};
    if (!map_file(path, &file, err))
        return false;
    if (file.size > 0xffffffffull) {
        unmap_file(&file);
        return fail(err, "resource exceeds 4 GiB: %s", path);
    }
    const bool ok = append_rscf(out, name, type, subtype, file.data,
                                static_cast<uint32_t>(file.size), err);
    unmap_file(&file);
    arena_reset(scratch, mark);
    return ok;
}
bool append_textures(Buffer* out, const Config& cfg, const EnvView& env, const MaterialMap& map, Arena* arena,
                     Arena* scratch, TextureSet* set, Error* err) {
    memset(set, 0, sizeof(*set));
    if (!cfg.texture_dir)
        return true;
    if (!dir_exists(cfg.texture_dir))
        return fail(err, "texture directory not found: %s", cfg.texture_dir);
    if (!list_files(cfg.texture_dir, arena, &set->files, err))
        return false;
    uint32_t write = 0;
    for (uint32_t i = 0; i < set->files.count; ++i) {
        MappedFile f{};
        if (!map_file(set->files.data[i].path, &f, err))
            return false;
        const bool dds = f.size >= 4 && memcmp(f.data, "DDS ", 4) == 0;
        unmap_file(&f);
        if (!dds)
            return fail(err, "texture payload must start with DDS: %s", set->files.data[i].path);
        set->files.data[write++] = set->files.data[i];
    }
    set->files.count = write;
    char prefix[512];
    const char* raw = cfg.texture_prefix ? cfg.texture_prefix : "\\environments\\";
    uint32_t at = 0;
    if (raw[0] != '\\')
        prefix[at++] = '\\';
    const uint32_t rn = static_cast<uint32_t>(strlen(raw));
    if (at + rn + 2 > sizeof(prefix))
        return fail(err, "texture prefix too long");
    memcpy(prefix + at, raw, rn);
    at += rn;
    if (!at || prefix[at - 1] != '\\')
        prefix[at++] = '\\';
    prefix[at] = 0;
    for (uint32_t i = 0; i < set->files.count; ++i) {
        char text_name[1024], rscf_name[1100];
        snprintf(text_name, sizeof(text_name), "%s%s", prefix, set->files.data[i].name);
        snprintf(rscf_name, sizeof(rscf_name), "\\graphics%s", text_name);
        Str one = str_from_c(text_name);
        if (!append_text(out, &one, 1, err) || !append_txfl(out, 1, err)) {
            return false;
        }
        const int32_t ti = 0;
        const uint32_t fl = 0, su = 1;
        if (!append_mtrl(out, &ti, &fl, &su, 1, err) ||
            !append_file_rscf(out, str_from_c(rscf_name), ASURA_RESOURCEFILE_TYPE_TEXTURE, 0, set->files.data[i].path,
                              scratch, err))
            return false;
    }
    // Global material table indexed by each Env strip's serialized original
    // material ordinal. Keep accepting legacy tool output that wrote a
    // 1000-based runtime handle into this slot, and fold it back to an ordinal.
    uint32_t max_ord = 0;
    bool any = false;
    for (uint32_t i = 0; i < env.strip_count; ++i) {
        const int32_t original = env.strips[i].m_iOriginalMaterialIndex;
        if (original < 0)
            continue;
        const uint32_t m = static_cast<uint32_t>(original), ord = m >= 1000 ? m - 1000 : m;
        max_ord = ord > max_ord ? ord : max_ord;
        any = true;
    }
    if (!any)
        return true;
    const uint32_t count = max_ord + 1;
    int32_t* tex = arena_array<int32_t>(arena, count, err, false);
    uint32_t* flags = arena_array<uint32_t>(arena, count, err);
    uint32_t* surface = arena_array<uint32_t>(arena, count, err, false);
    Str* names = arena_array<Str>(arena, set->files.count, err, false);
    if (err->set)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        tex[i] = -1;
        surface[i] = 1;
    }
    for (uint32_t i = 0; i < set->files.count; ++i) {
        char* name = set->files.data[i].name;
        char* joined = arena_array<char>(arena, strlen(prefix) + strlen(name) + 1, err, false);
        if (!joined)
            return false;
        snprintf(joined, strlen(prefix) + strlen(name) + 1, "%s%s", prefix, name);
        names[i] = str_from_c(joined);
    }
    for (uint32_t i = 0; i < env.strip_count; ++i) {
        const int32_t original = env.strips[i].m_iOriginalMaterialIndex;
        if (original < 0)
            continue;
        const uint32_t m = static_cast<uint32_t>(original), ord = m >= 1000 ? m - 1000 : m;
        Str wanted = path_basename(material_texture_name(map, ord));
        for (uint32_t j = 0; j < set->files.count; ++j) {
            Str file = str_from_c(set->files.data[j].name);
            if (str_ieq(wanted, file) || str_ieq(path_stem(wanted), path_stem(file))) {
                tex[ord] = static_cast<int32_t>(j);
                break;
            }
        }
        flags[ord] = material_override(map, "transparency_flag_by_material_index", ord, 0);
        surface[ord] = material_override(map, "surface_type_by_material_index", ord, 1);
    }
    if (set->files.count &&
        (!append_text(out, names, set->files.count, err) || !append_txfl(out, set->files.count, err)))
        return false;
    return append_mtrl(out, tex, flags, surface, count, err);
}

bool append_sky_resources(Buffer* out, const Config& cfg, Arena* scratch, Error* err) {
    if (!cfg.sky_texture_dir)
        return true;
    if (!dir_exists(cfg.sky_texture_dir))
        return fail(err, "sky texture directory not found: %s", cfg.sky_texture_dir);
    ArenaMark files_mark = arena_mark(scratch);
    Vec<DiskFile> disk_files{};
    if (!list_files(cfg.sky_texture_dir, scratch, &disk_files, err))
        return false;

    const auto hash_stem = [](Str path) {
        Str stem = path_basename(path);
        for (uint32_t i = 0; i < stem.size; ++i)
            if (stem.data[i] == '.') {
                stem.size = i;
                break;
            }
        return stem;
    };
    Str emitted[ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT]{};
    uint32_t emitted_count = 0;
    for (uint32_t slot = 0; slot < ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT; ++slot) {
        if (!cfg.sky_texture_paths[slot])
            continue;
        const Str wanted = hash_stem(str_from_c(cfg.sky_texture_paths[slot]));
        if (!wanted.size)
            continue;
        bool already_emitted = false;
        for (uint32_t i = 0; i < emitted_count; ++i)
            already_emitted |= str_ieq(wanted, emitted[i]);
        if (already_emitted)
            continue;

        const DiskFile* source = nullptr;
        for (uint32_t i = 0; i < disk_files.count; ++i) {
            if (!str_ieq(hash_stem(str_from_c(disk_files.data[i].name)), wanted))
                continue;
            MappedFile file{};
            if (!map_file(disk_files.data[i].path, &file, err))
                return false;
            const bool dds = file.size >= 4 && memcmp(file.data, "DDS ", 4) == 0;
            unmap_file(&file);
            if (dds) {
                source = &disk_files.data[i];
                break;
            }
        }
        if (!source)
            return fail(err, "no DDS payload in the sky texture folder matches SKYB resource '%.*s'",
                        static_cast<int>(wanted.size), wanted.data);
        char name[256];
        const int written = snprintf(name, sizeof(name), "\\graphics\\sky\\%.*s",
                                     static_cast<int>(wanted.size), wanted.data);
        if (written < 0 || written >= static_cast<int>(sizeof(name)))
            return fail(err, "SKYB resource name is too long");
        if (!append_file_rscf(out, str_from_c(name), ASURA_RESOURCEFILE_TYPE_TEXTURE, 0,
                              source->path, scratch, err))
            return false;
        emitted[emitted_count++] = wanted;
    }
    arena_reset(scratch, files_mark);
    return true;
}

} // namespace

namespace asura::level {

bool append_rsfl(Buffer* out, Error* err) {
    ChunkMark chunk = begin_chunk(out, ASURA_CHUNK_RESOURCEFILELIST, 1, 0, err);
    append_u32(out, 0, err);
    return end_chunk(out, chunk, err);
}
bool bytes_contains_i(const uint8_t* data, uint32_t size, const char* needle) {
    Str n = str_from_c(needle);
    return str_contains_i({reinterpret_cast<const char*>(data), size}, n);
}
bool path_name_ieq(Str a, Str b) {
    if (a.size != b.size)
        return false;
    for (uint32_t i = 0; i < a.size; ++i) {
        const char ac = a.data[i] == '/' ? '\\' : ascii_lower(a.data[i]),
                   bc = b.data[i] == '/' ? '\\' : ascii_lower(b.data[i]);
        if (ac != bc)
            return false;
    }
    return true;
}
bool text_name_matches_resource(Str text, Str resource) {
    text = str_trim(text);
    resource = str_trim(resource);
    if (path_name_ieq(text, resource))
        return true;
    const Str graphics = str_lit("\\graphics");
    if (!str_starts_i(resource, graphics))
        return false;
    Str relative{resource.data + graphics.size, resource.size - graphics.size};
    if (path_name_ieq(text, relative))
        return true;
    if (text.size && text.data[0] != '\\' && text.data[0] != '/' && relative.size &&
        (relative.data[0] == '\\' || relative.data[0] == '/')) {
        ++relative.data;
        --relative.size;
        return path_name_ieq(text, relative);
    }
    return false;
}
bool text_chunk_references(const ChunkRef& ch, Str resource) {
    if (ch.cid != ASURA_CHUNK_TEXTURENAMES || ch.size < sizeof(Asura_Chunk_TextureNames))
        return false;
    const uint8_t* payload = ch.data + sizeof(Asura_Chunk_Header);
    const uint32_t size = ch.size - sizeof(Asura_Chunk_Header), count = read_u32(payload);
    uint32_t at = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (at >= size)
            return false;
        uint32_t end = at;
        while (end < size && payload[end])
            ++end;
        if (end == size)
            return false;
        Str name{reinterpret_cast<const char*>(payload + at), end - at};
        if (text_name_matches_resource(name, resource))
            return true;
        const uint64_t next = align_up(static_cast<uint64_t>(end) + 1, 4);
        if (next > size)
            return false;
        at = static_cast<uint32_t>(next);
    }
    return false;
}
constexpr const char* kWeaponHsknNames[] = {"Binoculars",   "FaustRocket",   "Knife",
                                            "Luger",        "MG42",          "Panzerfaust",
                                            "Pineapple",    "Rock",          "ammo_belt",
                                            "ammo_dp28",    "ammo_drum",     "ammo_mp40",
                                            "ammo_pistol",  "ammo_rifle",    "bandage",
                                            "dogtag",       "dp28",          "gore_entry_small",
                                            "machgun",      "mauser",        "mauser_scope",
                                            "mp40",         "nagan",         "nagan_scope",
                                            "p38",          "panzerschreck", "schreckrocket",
                                            "smallmedkit",  "smokegrenade",  "springfield",
                                            "stickgrenade", "tbomb",         "tnt",
                                            "tripbomb",     "tripbomb_stake"};
bool is_weapon_name(Str name) {
    for (const char* w : kWeaponHsknNames)
        if (str_ieq_c(name, w))
            return true;
    return false;
}
bool is_weapon_name_or_variant(Str name) {
    if (is_weapon_name(name))
        return true;
    for (const char* w : kWeaponHsknNames) {
        Str base = str_lit(w);
        if (name.size > base.size && name.data[base.size] == '_' && str_ieq({name.data, base.size}, base))
            return true;
    }
    return false;
}
bool path_separator(char c) {
    return c == '\\' || c == '/';
}
bool path_has_component_i(Str path, Str token) {
    if (!token.size || token.size > path.size)
        return false;
    for (uint32_t i = 0; i + token.size <= path.size; ++i) {
        if (i && !path_separator(path.data[i - 1]))
            continue;
        if (i + token.size < path.size && !path_separator(path.data[i + token.size]))
            continue;
        if (str_ieq({path.data + i, token.size}, token))
            return true;
    }
    return false;
}
bool has_weapon_token(Str text, bool path_component) {
    for (const char* w : kWeaponHsknNames) {
        Str name = str_lit(w);
        if (path_component ? path_has_component_i(text, name) : str_contains_i(text, name))
            return true;
        uint32_t at = 0;
        while (at < name.size) {
            uint32_t end = at;
            while (end < name.size && name.data[end] != '_')
                ++end;
            Str part{name.data + at, end - at};
            if (part.size && (path_component ? path_has_component_i(text, part) : str_contains_i(text, part)))
                return true;
            at = end + 1;
        }
    }
    return false;
}
bool append_weapon_support(Buffer* out, const Config& cfg, Arena* scratch, Error* err) {
    if (!cfg.weapon_from_pc)
        return true;
    ArenaMark mark = arena_mark(scratch);
    ChunkList d{};
    if (!parse_chunks(cfg.weapon_from_pc, &d, scratch, err))
        return false;
    uint8_t* want = arena_array<uint8_t>(scratch, d.count, err);
    if (!want) {
        unmap_file(&d.file);
        return false;
    }
    const uint32_t support[] = {ASURA_CHUNK_TEXTURENAMES, ASURA_CHUNK_TEXTUREFLAGS, ASURA_CHUNK_MATERIAL,
                                ASURA_CHUNK_MATERIALNAMES};
    auto is_support = [&](uint32_t c) {
        for (uint32_t x : support)
            if (c == x)
                return true;
        return false;
    };
    auto is_pickup_name = [](Str name) {
        return str_ieq_c(name, "mp_russiandogtag") || str_ieq_c(name, "mp_germandogtag") ||
               str_ieq_c(name, "mp_dedcross");
    };
    auto is_pickup_support = [&](uint32_t c) {
        return is_support(c) || c == fourcc('S', 'H', 'P', 'D') || c == fourcc('S', 'H', 'A', 'P');
    };
    for (uint32_t i = 0; i < d.count; ++i)
        if (d.chunks[i].cid == fourcc('H', 'S', 'K', 'N')) {
            Str name = padded_string_at(d.chunks[i].data, d.chunks[i].size, 24);
            if (!name.data || !is_weapon_name(name))
                continue;
            want[i] = 1;
            int32_t j = static_cast<int32_t>(i) - 1;
            while (j >= 0 && is_support(d.chunks[j].cid))
                want[j--] = 1;
            while (j >= 0 && d.chunks[j].cid == fourcc('H', 'C', 'A', 'N'))
                want[j--] = 1;
            uint32_t k = i + 1;
            while (k < d.count &&
                   (d.chunks[k].cid == fourcc('H', 'M', 'P', 'T') || d.chunks[k].cid == fourcc('H', 'S', 'B', 'B')))
                want[k++] = 1;
            RscfInfo r{};
            if (k < d.count && rscf_info(d.chunks[k], &r) && r.type == ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC &&
                str_ieq(r.name, name))
                want[k] = 1;
        }
    for (uint32_t i = 0; i < d.count; ++i) {
        RscfInfo r{};
        if (rscf_info(d.chunks[i], &r)) {
            if (r.type == ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC && is_pickup_name(r.name)) {
                want[i] = 1;
                for (int32_t j = static_cast<int32_t>(i) - 1; j >= 0 && is_pickup_support(d.chunks[j].cid); --j)
                    want[j] = 1;
            }
            if (is_weapon_name(r.name) ||
                (r.type == ASURA_RESOURCEFILE_TYPE_TEXTURE &&
                 str_starts_i(r.name, str_lit("\\graphics\\characters\\")) && has_weapon_token(r.name, true)) ||
                (r.type == ASURA_RESOURCEFILE_TYPE_SOUND && str_contains_i(r.name, str_lit("sounds\\weapons\\")) &&
                 has_weapon_token(r.name, false)) ||
                (r.type == ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC && is_weapon_name_or_variant(r.name)) ||
                is_pickup_name(r.name) || str_ieq_c(r.name, "\\graphics\\objects\\bullets\\shell.bmp") ||
                str_ieq_c(r.name, "\\graphics\\objects\\gore\\gore.bmp"))
                want[i] = 1;
        }
        if (d.chunks[i].cid == fourcc('S', 'H', 'A', 'P') &&
            (bytes_contains_i(d.chunks[i].data, d.chunks[i].size, "mp_russiandogtag") ||
             bytes_contains_i(d.chunks[i].data, d.chunks[i].size, "mp_germandogtag") ||
             bytes_contains_i(d.chunks[i].data, d.chunks[i].size, "mp_dedcross"))) {
            want[i] = 1;
            if (i + 1 < d.count && d.chunks[i + 1].cid == fourcc('S', 'H', 'P', 'D'))
                want[i + 1] = 1;
        }
    }
    // Pickup/weapon TEXT chunks name texture resources without the leading
    // "\\graphics" component. Import every matching type-2 RSCF as well.
    for (uint32_t i = 0; i < d.count; ++i) {
        RscfInfo r{};
        if (!rscf_info(d.chunks[i], &r) || r.type != ASURA_RESOURCEFILE_TYPE_TEXTURE)
            continue;
        for (uint32_t j = 0; j < d.count; ++j)
            if (want[j] && text_chunk_references(d.chunks[j], r.name)) {
                want[i] = 1;
                break;
            }
    }
    for (uint32_t i = 0; i < d.count; ++i)
        if (want[i] && !append_chunk_copy(out, d.chunks[i], err))
            break;
    unmap_file(&d.file);
    arena_reset(scratch, mark);
    return !err->set;
}

bool append_sound_resources(Buffer* out, const Sounds& s, Arena* scratch, Error* err) {
    for (uint32_t i = 0; i < s.count; ++i)
        if (s.items[i].file) {
            if (!file_exists(s.items[i].file))
                return fail(err, "sound file not found: %s", s.items[i].file);
            if (!append_file_rscf(out, s.items[i].name, ASURA_RESOURCEFILE_TYPE_SOUND, s.items[i].sound_resource_id,
                                  s.items[i].file, scratch, err))
                return false;
        }
    return true;
}
bool append_phon(Buffer* out, const Sounds& s, Error* err) {
    if (!s.count)
        return true;
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_PHONONS, 9, 0, err);
    append_u32(out, s.count, err);
    for (uint32_t i = 0; i < s.count; ++i) {
        const SoundEntry& e = s.items[i];
        Asura_Chunk_Phonons_PhononDataV9 rec{};
        rec.m_uSoundResourceID = e.sound_resource_id;
        rec.m_xPosition = e.position;
        rec.m_fInnerRadius = e.inner_radius;
        rec.m_fOuterRadius = e.outer_radius;
        memcpy(rec.m_afLegacyVolumeParameters, e.legacy_volume_parameters, sizeof(rec.m_afLegacyVolumeParameters));
        rec.m_uFlags = e.flags;
        rec.m_xInnerCuboidRadius = e.inner_cuboid_radius;
        rec.m_xOuterCuboidRadius = e.outer_cuboid_radius;
        rec.m_uGuid = e.phonon_guid;
        rec.m_xRetriggerBoundingBox = e.retrigger_bounding_box;
        rec.m_xOrient = e.orientation;
        buffer_append(out, &rec, sizeof(rec), err);
    }
    return end_chunk(out, ch, err);
}
bool append_sound_entities(Buffer* out, const Sounds& s, Error* err) {
    for (uint32_t i = 0; i < s.count; ++i) {
        const SoundEntry& e = s.items[i];
        if (!e.emit_enti)
            continue;
        Asura_ServerEntity_SoundController_ChunkDataV0 data{};
        data.m_xEntity.Guid = e.controller_guid;
        data.m_xEntity.Classification = AsuraEntityClass_SoundController;
        data.m_xEntity.m_usPadding = e.controller_padding;
        data.m_iActivatableVersion = 2;
        data.m_bActive = e.active ? 1u : 0u;
        data.m_iVersion = 0;
        data.m_uPhononGuid = e.phonon_guid;
        ChunkMark ch = begin_chunk(out, ASURA_CHUNK_ENTITY, 0, 0, err);
        buffer_append(out, &data, sizeof(data), err);
        end_chunk(out, ch, err);
    }
    return !err->set;
}

} // namespace
