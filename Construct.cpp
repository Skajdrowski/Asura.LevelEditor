#include "asura_base.hpp"
#include "AsuraStructs.h"

using namespace asura;

namespace {

constexpr uint8_t kAsuraMagic[8] = {'A', 's', 'u', 'r', 'a', ' ', ' ', ' '};
constexpr uint32_t kMaxAabbTreeObjects = 65535;
constexpr uint32_t kSpawnGuidBase = 0x186a0;
constexpr uint32_t kPhononGuidBase = 0x186a0;
constexpr uint32_t kSoundControllerGuidBase = 0x18c40;
// sub_4836C0 shares one 0x4000-byte buffer between AABB
// traversal frames (28 bytes each) and returned object IDs (4 bytes each).
// Keep a complete module query comfortably below that workspace limit.
constexpr uint32_t kDefaultMaxCollisionPolys = 3000;
constexpr uint16_t kCollisionPolyFlagBulletIgnore = 0x240;

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

inline uint32_t read_u32(const void* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
inline uint16_t read_u16(const void* p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
inline float read_f32(const void* p) {
    float v;
    memcpy(&v, p, sizeof(v));
    return v;
}

struct ChunkRef {
    const uint8_t* data;
    uint32_t size;
    uint32_t cid;
    uint32_t version;
    uint32_t flags;
    uint32_t index;
};

struct ChunkList {
    MappedFile file;
    ChunkRef* chunks;
    uint32_t count;
};

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

struct RscfInfo {
    uint32_t type;
    uint32_t subtype;
    uint32_t payload_size;
    Str name;
    const uint8_t* payload;
};

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

struct ChunkMark {
    uint64_t start;
};

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

bool append_rscf_zero(Buffer* out, Str name, uint32_t type, uint32_t subtype, uint32_t payload_size, Error* err) {
    ChunkMark mark = begin_chunk(out, ASURA_CHUNK_RESOURCEFILE, 0, 0, err);
    append_u32(out, type, err);
    append_u32(out, subtype, err);
    append_u32(out, payload_size, err);
    if (!append_padded_cstr(out, name, err))
        return false;
    if (buffer_append(out, nullptr, payload_size, err) == ~0ull)
        return false;
    return end_chunk(out, mark, err);
}

struct Config {
    const char* obj;
    const char* out;
    const char* env_name;
    const char* axis_map;
    bool flip_x, flip_y, flip_z;
    bool allow_unknown_materials;
    uint32_t diffuse_abgr;
    bool force_material;
    uint32_t force_material_index;
    uint32_t max_prim_count;
    uint32_t max_collision_polys;
    float auto_block_xz_cell;
    float module_pad_min;
    float module_pad_scale;
    const char* material_map;
    const char* texture_dir;
    const char* texture_prefix;
    const char* sky_texture_dir;
    Str sky_flip_faces;
    const char* import_tex_from_pc;
    const char* import_mtrl_from_pc;
    const char* rsfl_from_pc;
    Str rsfl_names;
    const char* smsg_from_pc;
    const char* lite_from_pc;
    const char* lite_json;
    const char* enti_from_pc;
    bool enti_keep_spawnpoints;
    Str enti_types;
    const char* rscf_from_pc;
    Str rscf_types;
    Str rscf_name_filter;
    Str rscf_names;
    uint32_t rscf_skip;
    bool rscf_limit_set;
    uint32_t rscf_limit;
    const char* rscf_bootstrap_name;
    uint32_t rscf_bootstrap_subtype;
    uint32_t rscf_bootstrap_payload_size;
    const char* weapon_from_pc;
    const char* spawnpoints_json;
    const char* sounds_json;
    const char* ambient_stream_path;
    float ambient_volume;
    const char* shade_from;
    const char* shade_align_from_obj;
    Asura_Vector_3 shade_offset;
    uint64_t arena_reserve;
    uint64_t output_reserve;
};

bool parse_option_u32(const char* text, uint32_t* out) {
    uint64_t v = 0;
    if (!parse_u64(str_from_c(text), &v) || v > 0xffffffffull)
        return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

bool parse_option_float(const char* text, float* out) {
    double v = 0;
    if (!parse_f64(str_from_c(text), &v) || !isfinite(v))
        return false;
    *out = static_cast<float>(v);
    return true;
}

bool parse_vec3_arg(const char* text, Asura_Vector_3* out) {
    const char* at = text;
    float v[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        while (*at == ' ' || *at == '\t' || *at == ',' || *at == ';')
            ++at;
        const char* start = at;
        while (*at && *at != ',' && *at != ';' && *at != ' ' && *at != '\t')
            ++at;
        double d = 0;
        if (at == start || !parse_f64({start, static_cast<uint32_t>(at - start)}, &d))
            return false;
        v[i] = static_cast<float>(d);
    }
    while (*at == ' ' || *at == '\t' || *at == ',' || *at == ';')
        ++at;
    if (*at)
        return false;
    *out = {v[0], v[1], v[2]};
    return true;
}

void print_usage() {
    console_write("Asura 2005 .pc level constructor\n"
                  "usage:\n"
                  "  asura_pc_construct <map.obj> <out.pc> [options]\n\n"
                  "Input:\n"
                  "  Export one joined Blender mesh; object/group names are ignored.\n"
                  "  The constructor splits the level into runtime modules automatically.\n\n"
                  "Core options:\n"
                  "  --env-name NAME\n"
                  "  --axis-map xyz|xzy|yxz|yzx|zxy|zyx\n"
                  "  --flip-x | --flip-y | --flip-z\n"
                  "  --no-flip-x | --no-flip-y | --no-flip-z\n"
                  "                                  default: flip Y/Z (target-game environment space)\n"
                  "  --material-map FILE.json        direct or orig_to_handle mapping\n"
                  "                                  optional collision_flags maps material IDs to u16 masks\n"
                  "  --allow-unknown-materials\n"
                  "  --force-material-index N\n"
                  "  --diffuse-abgr 0xAARRGGBB\n"
                  "  --auto-block-xz-cell F          default: 40\n"
                  "  --max-prim-count N              default: 1000\n"
                  "  --max-collision-polys N         default: 3000\n"
                  "  --module-pad-min F --module-pad-scale F (default: exact bounds)\n\n"
                  "Resource options:\n"
                  "  --texture-dir DIR --texture-prefix PREFIX\n"
                  "  --sky-texture-dir DIR --sky-flip-faces FACE,... (default: none)\n"
                  "  --import-tex-from-pc FILE --import-mtrl-from-pc FILE (legacy no-op)\n"
                  "  --rsfl-from-pc FILE --rsfl-names a,b\n"
                  "  --smsg-from-pc FILE --lite-from-pc FILE --lite-json FILE\n"
                  "  --enti-from-pc FILE [--enti-from-pc-keep-spawnpoints]\n"
                  "  --enti-from-pc-types 0x8,0x804f\n"
                  "  --rscf-from-pc FILE [--rscf-types 0,2,3] [--rscf-names a,b]\n"
                  "  --rscf-name-regex TEXT          ASCII case-insensitive substring/glob\n"
                  "  --rscf-skip N --rscf-limit N\n"
                  "  --weapon-rscf-from-pc FILE\n"
                  "  --spawnpoints-json FILE --sounds-json FILE\n"
                  "  --ambient-stream-path NAME --ambient-volume F\n"
                  "  --rscf-bootstrap-name NAME [--rscf-bootstrap-dummy N]\n"
                  "  --rscf-bootstrap-payload-size N\n\n"
                  "Memory controls:\n"
                  "  --arena-mib N                    virtual reserve; default: x64 8192, Win32 256\n"
                  "  --output-mib N                   virtual reserve; default: x64 4096, Win32 128\n");
}

bool parse_cli(int argc, char** argv, Config* cfg, Error* err) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->env_name = "Env";
    cfg->axis_map = "xyz";
    // Target-game environment resources use the original Y/Z-flipped Blender
    // conversion. The level editor compensates Y only in its authoring view.
    cfg->flip_y = true;
    cfg->flip_z = true;
    cfg->diffuse_abgr = 0xff808080u;
    cfg->max_prim_count = 1000;
    cfg->max_collision_polys = kDefaultMaxCollisionPolys;
    cfg->auto_block_xz_cell = 40.0f;
    cfg->module_pad_min = 0.0f;
    cfg->module_pad_scale = 0.0f;
    cfg->texture_prefix = "\\environments\\";
    cfg->ambient_volume = 1.0f;
    cfg->arena_reserve = sizeof(void*) == 4 ? 256ull * MiB : 8ull * GiB;
    cfg->output_reserve = sizeof(void*) == 4 ? 128ull * MiB : 4ull * GiB;

    if (argc < 3) {
        print_usage();
        return fail(err, "expected blender .OBJ path and .PC output path");
    }
    cfg->obj = argv[1];
    cfg->out = argv[2];

    for (int i = 3; i < argc; ++i) {
        const char* a = argv[i];
        auto value = [&](const char* name) -> const char* {
            if (strcmp(a, name) != 0)
                return nullptr;
            if (i + 1 >= argc) {
                fail(err, "%s needs a value", name);
                return nullptr;
            }
            return argv[++i];
        };
        const char* v = nullptr;
        if (strcmp(a, "--flip-x") == 0)
            cfg->flip_x = true;
        else if (strcmp(a, "--flip-y") == 0)
            cfg->flip_y = true;
        else if (strcmp(a, "--flip-z") == 0)
            cfg->flip_z = true;
        else if (strcmp(a, "--no-flip-x") == 0)
            cfg->flip_x = false;
        else if (strcmp(a, "--no-flip-y") == 0)
            cfg->flip_y = false;
        else if (strcmp(a, "--no-flip-z") == 0)
            cfg->flip_z = false;
        else if (strcmp(a, "--allow-unknown-materials") == 0)
            cfg->allow_unknown_materials = true;
        else if (strcmp(a, "--enti-from-pc-keep-spawnpoints") == 0)
            cfg->enti_keep_spawnpoints = true;
        else if ((v = value("--env-name")))
            cfg->env_name = v;
        else if ((v = value("--axis-map")))
            cfg->axis_map = v;
        else if ((v = value("--material-map")))
            cfg->material_map = v;
        else if ((v = value("--texture-dir")))
            cfg->texture_dir = v;
        else if ((v = value("--texture-prefix")))
            cfg->texture_prefix = v;
        else if ((v = value("--sky-texture-dir")))
            cfg->sky_texture_dir = v;
        else if ((v = value("--sky-flip-faces")))
            cfg->sky_flip_faces = str_from_c(v);
        else if ((v = value("--import-tex-from-pc")))
            cfg->import_tex_from_pc = v;
        else if ((v = value("--import-mtrl-from-pc")))
            cfg->import_mtrl_from_pc = v;
        else if ((v = value("--rsfl-from-pc")))
            cfg->rsfl_from_pc = v;
        else if ((v = value("--rsfl-names")))
            cfg->rsfl_names = str_from_c(v);
        else if ((v = value("--smsg-from-pc")))
            cfg->smsg_from_pc = v;
        else if ((v = value("--lite-from-pc")))
            cfg->lite_from_pc = v;
        else if ((v = value("--lite-json")))
            cfg->lite_json = v;
        else if ((v = value("--enti-from-pc")))
            cfg->enti_from_pc = v;
        else if ((v = value("--enti-from-pc-types")))
            cfg->enti_types = str_from_c(v);
        else if ((v = value("--rscf-from-pc")))
            cfg->rscf_from_pc = v;
        else if ((v = value("--rscf-types")))
            cfg->rscf_types = str_from_c(v);
        else if ((v = value("--rscf-name-regex")))
            cfg->rscf_name_filter = str_from_c(v);
        else if ((v = value("--rscf-names")))
            cfg->rscf_names = str_from_c(v);
        else if ((v = value("--rscf-bootstrap-name")))
            cfg->rscf_bootstrap_name = v;
        else if ((v = value("--weapon-rscf-from-pc")))
            cfg->weapon_from_pc = v;
        else if ((v = value("--spawnpoints-json")))
            cfg->spawnpoints_json = v;
        else if ((v = value("--sounds-json")))
            cfg->sounds_json = v;
        else if ((v = value("--ambient-stream-path")))
            cfg->ambient_stream_path = v;
        else if ((v = value("--shade-from")))
            cfg->shade_from = v;
        else if ((v = value("--shade-align-from-obj")))
            cfg->shade_align_from_obj = v;
        else if ((v = value("--diffuse-abgr"))) {
            if (!parse_option_u32(v, &cfg->diffuse_abgr))
                return fail(err, "bad --diffuse-abgr");
        } else if ((v = value("--force-material-index"))) {
            cfg->force_material = parse_option_u32(v, &cfg->force_material_index);
            if (!cfg->force_material)
                return fail(err, "bad --force-material-index");
        } else if ((v = value("--max-prim-count"))) {
            if (!parse_option_u32(v, &cfg->max_prim_count) || cfg->max_prim_count < 2)
                return fail(err, "bad --max-prim-count (expected >= 2)");
        } else if ((v = value("--max-collision-polys"))) {
            if (!parse_option_u32(v, &cfg->max_collision_polys) || !cfg->max_collision_polys ||
                cfg->max_collision_polys > kMaxAabbTreeObjects)
                return fail(err, "bad --max-collision-polys (expected 1..65535)");
        } else if ((v = value("--auto-block-xz-cell"))) {
            if (!parse_option_float(v, &cfg->auto_block_xz_cell))
                return fail(err, "bad --auto-block-xz-cell");
        } else if ((v = value("--module-pad-min"))) {
            if (!parse_option_float(v, &cfg->module_pad_min))
                return fail(err, "bad --module-pad-min");
        } else if ((v = value("--module-pad-scale"))) {
            if (!parse_option_float(v, &cfg->module_pad_scale))
                return fail(err, "bad --module-pad-scale");
        } else if ((v = value("--rscf-skip"))) {
            if (!parse_option_u32(v, &cfg->rscf_skip))
                return fail(err, "bad --rscf-skip");
        } else if ((v = value("--rscf-limit"))) {
            cfg->rscf_limit_set = parse_option_u32(v, &cfg->rscf_limit);
            if (!cfg->rscf_limit_set)
                return fail(err, "bad --rscf-limit");
        } else if ((v = value("--rscf-bootstrap-dummy"))) {
            if (!parse_option_u32(v, &cfg->rscf_bootstrap_subtype))
                return fail(err, "bad --rscf-bootstrap-dummy");
        } else if ((v = value("--rscf-bootstrap-payload-size"))) {
            if (!parse_option_u32(v, &cfg->rscf_bootstrap_payload_size))
                return fail(err, "bad --rscf-bootstrap-payload-size");
        } else if ((v = value("--ambient-volume"))) {
            if (!parse_option_float(v, &cfg->ambient_volume))
                return fail(err, "bad --ambient-volume");
        } else if ((v = value("--shade-offset"))) {
            if (!parse_vec3_arg(v, &cfg->shade_offset))
                return fail(err, "bad --shade-offset");
        } else if ((v = value("--arena-mib"))) {
            uint32_t n = 0;
            if (!parse_option_u32(v, &n) || n < 64)
                return fail(err, "bad --arena-mib");
            cfg->arena_reserve = static_cast<uint64_t>(n) * MiB;
        } else if ((v = value("--output-mib"))) {
            uint32_t n = 0;
            if (!parse_option_u32(v, &n) || n < 64)
                return fail(err, "bad --output-mib");
            cfg->output_reserve = static_cast<uint64_t>(n) * MiB;
        } else if (err->set)
            return false;
        else
            return fail(err, "unknown option: %s", a);
    }

    if (strcmp(cfg->axis_map, "xyz") && strcmp(cfg->axis_map, "xzy") && strcmp(cfg->axis_map, "yxz") &&
        strcmp(cfg->axis_map, "yzx") && strcmp(cfg->axis_map, "zxy") && strcmp(cfg->axis_map, "zyx"))
        return fail(err, "invalid --axis-map");
    if (!file_exists(cfg->obj))
        return fail(err, "OBJ not found: %s", cfg->obj);
    if (cfg->module_pad_min < 0.0f || cfg->module_pad_scale < 0.0f)
        return fail(err, "module padding must be non-negative");
    return true;
}

Asura_Vector_3 transform_vec(Asura_Vector_3 v, const Config& cfg) {
    if (cfg.flip_x)
        v.x = -v.x;
    if (cfg.flip_y)
        v.y = -v.y;
    if (cfg.flip_z)
        v.z = -v.z;
    if (!strcmp(cfg.axis_map, "xyz"))
        return v;
    if (!strcmp(cfg.axis_map, "xzy"))
        return {v.x, v.z, v.y};
    if (!strcmp(cfg.axis_map, "yxz"))
        return {v.y, v.x, v.z};
    if (!strcmp(cfg.axis_map, "yzx"))
        return {v.y, v.z, v.x};
    if (!strcmp(cfg.axis_map, "zxy"))
        return {v.z, v.x, v.y};
    return {v.z, v.y, v.x};
}

bool transform_reverses_winding(const Config& cfg) {
    bool reversed = cfg.flip_x ^ cfg.flip_y ^ cfg.flip_z;
    const bool odd_permutation = !strcmp(cfg.axis_map, "xzy") || !strcmp(cfg.axis_map, "yxz") ||
                                 !strcmp(cfg.axis_map, "zyx");
    return reversed ^ odd_permutation;
}

uint8_t clamp_u8(double v) {
    if (v <= 0.0)
        return 0;
    if (v >= 255.0)
        return 255;
    return static_cast<uint8_t>(floor(v + 0.5));
}

struct ObjIndex {
    int32_t v, vt, vn;
};
struct ObjFace {
    ObjIndex a, b, c;
    Str material;
    uint32_t order;
};
struct ObjData {
    Asura_Vector_3* positions;
    uint32_t* colors;
    uint8_t* has_color;
    Asura_Vector_2* texcoords;
    Asura_Vector_3* normals;
    ObjFace* faces;
    uint32_t position_count, texcoord_count, normal_count, face_count;
};

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

namespace {

struct MaterialMap {
    MappedFile file;
    Json* root;
};

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
    return fail(err, "unknown OBJ material '%.*s' (use --material-map or --allow-unknown-materials)", name.size,
                name.data);
}

Str material_texture_name_exact(const MaterialMap& map, uint32_t material_index) {
    if (!map.root || map.root->kind != JsonKind::Object)
        return {};
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

struct VertexKey {
    uint32_t v, vt, vn, shade_material;
};
inline bool vertex_key_eq(const VertexKey& a, const VertexKey& b) {
    return a.v == b.v && a.vt == b.vt && a.vn == b.vn && a.shade_material == b.shade_material;
}
uint64_t vertex_hash(VertexKey k) {
    uint64_t x = (static_cast<uint64_t>(k.v) << 32) ^ (static_cast<uint64_t>(k.vt) * 0x9e3779b185ebca87ull) ^ k.vn ^
                 (static_cast<uint64_t>(k.shade_material) * 0xd6e8feb86659fd93ull);
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
                   vn < 0 ? 0xffffffffu : static_cast<uint32_t>(vn), 0};
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

struct EnvBuild {
    Buffer payload;
    uint32_t modules;
    uint32_t strip_count;
};

struct ShadeSource;
bool shade_pick(const ShadeSource* shade, Asura_Vector_3 position, Asura_Vector_3 normal, uint32_t material, uint32_t* out_color);

bool build_env(const Config& cfg, const ObjData& obj, const MaterialMap& materials, const ShadeSource* shade,
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
        if (cfg.force_material)
            m = cfg.force_material_index;
        else if (!resolve_material(materials, f.material, cfg.allow_unknown_materials, &m, err))
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
                key.shade_material = shade ? material : 0;
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
                uint32_t color = obj.has_color[key.v] ? obj.colors[key.v] : cfg.diffuse_abgr;
                if (!obj.has_color[key.v])
                    shade_pick(shade, p, n, material, &color);
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
                    if (!tb.push({seg_len >= 2 ? seg_len - 2 : 0, seg_start, tris[run].material, min_i,
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

namespace {

struct EnvView {
    const uint8_t* data;
    uint32_t size;
    const Asura_PC_EnvironmentRenderer_Module* modules;
    const Asura_PC_EnvironmentRenderer_Strip* strips;
    uint32_t module_count, strip_count, block_count;
    const uint8_t** blocks;
    uint32_t* block_sizes;
};

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

struct ShadeSample {
    Asura_Vector_3 position, normal;
    uint32_t color, material;
};
struct ShadeBucket {
    int32_t x, y, z;
    uint32_t head;
    uint8_t used;
};
struct ShadeSource {
    ShadeSample* samples;
    uint32_t* next;
    ShadeBucket* buckets;
    uint32_t count, bucket_mask;
    float cell_size;
};

uint64_t shade_cell_hash(int32_t x, int32_t y, int32_t z) {
    uint64_t h = static_cast<uint32_t>(x) * 0x9e3779b1u;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(y)) * 0x85ebca77u;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(z)) * 0xc2b2ae3du;
    h ^= h >> 29;
    h *= 0x165667b19e3779f9ull;
    return h ^ (h >> 32);
}

bool transformed_obj_bounds(const ObjData& obj, const Config& cfg, Asura_Vector_3* mn, Asura_Vector_3* mx) {
    if (!obj.position_count)
        return false;
    *mn = transform_vec(obj.positions[0], cfg);
    *mx = *mn;
    for (uint32_t i = 1; i < obj.position_count; ++i) {
        Asura_Vector_3 p = transform_vec(obj.positions[i], cfg);
        mn->x = fmin(mn->x, p.x);
        mn->y = fmin(mn->y, p.y);
        mn->z = fmin(mn->z, p.z);
        mx->x = fmax(mx->x, p.x);
        mx->y = fmax(mx->y, p.y);
        mx->z = fmax(mx->z, p.z);
    }
    return true;
}

bool shade_alignment_offset(const Config& cfg, const ObjData& target, Arena* scratch, Asura_Vector_3* out, Error* err) {
    *out = cfg.shade_offset;
    if (!cfg.shade_align_from_obj)
        return true;
    ArenaMark mark = arena_mark(scratch);
    MappedFile f{};
    if (!map_file(cfg.shade_align_from_obj, &f, err))
        return false;
    ObjData ref{};
    if (!parse_obj(&f, &ref, scratch, err)) {
        unmap_file(&f);
        arena_reset(scratch, mark);
        return false;
    }
    Asura_Vector_3 tmn, tmx, rmn, rmx;
    if (!transformed_obj_bounds(target, cfg, &tmn, &tmx) || !transformed_obj_bounds(ref, cfg, &rmn, &rmx)) {
        unmap_file(&f);
        arena_reset(scratch, mark);
        return fail(err, "cannot derive shading alignment from empty OBJ");
    }
    out->x += (tmn.x + tmx.x - rmn.x - rmx.x) * .5f;
    out->y += (tmn.y + tmx.y - rmn.y - rmx.y) * .5f;
    out->z += (tmn.z + tmx.z - rmn.z - rmx.z) * .5f;
    unmap_file(&f);
    arena_reset(scratch, mark);
    return true;
}

bool load_shade_source(const Config& cfg, const ObjData& target, ShadeSource* out, Arena* arena, Arena* scratch,
                       Error* err) {
    memset(out, 0, sizeof(*out));
    if (!cfg.shade_from)
        return true;
    if (dir_exists(cfg.shade_from))
        return fail(err, "C++ --shade-from expects a donor .pc or raw Env payload, not a directory");
    ArenaMark mark = arena_mark(scratch);
    MappedFile file{};
    if (!map_file(cfg.shade_from, &file, err))
        return false;
    const uint8_t* payload = file.data;
    uint64_t payload_size = file.size;
    if (file.size >= 8 && memcmp(file.data, kAsuraMagic, 8) == 0) {
        payload = nullptr;
        uint64_t off = 8;
        while (off + sizeof(Asura_Chunk_Header) <= file.size) {
            ChunkRef ch{file.data + off,
                        read_u32(file.data + off + 4),
                        read_u32(file.data + off),
                        read_u32(file.data + off + 8),
                        read_u32(file.data + off + 12),
                        0};
            if (!ch.cid || !ch.size)
                break;
            if (ch.size < sizeof(Asura_Chunk_Header) || ch.size > file.size - off) {
                unmap_file(&file);
                arena_reset(scratch, mark);
                return fail(err, "shade donor has an invalid chunk at 0x%llx", static_cast<unsigned long long>(off));
            }
            RscfInfo r{};
            if (rscf_info(ch, &r) && r.type == ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC &&
                r.subtype == ASURA_RESOURCEFILE_TYPE_PC_ENVIRONMENT &&
                str_ieq(r.name, str_from_c(cfg.env_name))) {
                payload = r.payload;
                payload_size = r.payload_size;
                break;
            }
            off += ch.size;
        }
        if (!payload) {
            unmap_file(&file);
            arena_reset(scratch, mark);
            return fail(err, "shade donor contains no PC environment RSCF named '%s'", cfg.env_name);
        }
    }
    if (payload_size > 0xffffffffull) {
        unmap_file(&file);
        arena_reset(scratch, mark);
        return fail(err, "shade Env payload exceeds 4 GiB");
    }
    Buffer fake{};
    fake.base = const_cast<uint8_t*>(payload);
    fake.size = fake.committed = fake.reserved = payload_size;
    EnvView env{};
    if (!env_view(fake, &env, scratch, err)) {
        unmap_file(&file);
        arena_reset(scratch, mark);
        return false;
    }
    uint64_t sample_count = 0;
    for (uint32_t ai = 0; ai < env.module_count; ++ai) {
        const Asura_PC_EnvironmentRenderer_Module& a = env.modules[ai];
        if (a.m_uBufferIndex >= env.block_count)
            continue;
        const uint32_t nv = read_u32(env.blocks[a.m_uBufferIndex]);
        for (uint32_t k = 0; k < a.m_uNumberOfStrips && a.m_uFirstStrip + k < env.strip_count; ++k) {
            const Asura_PC_EnvironmentRenderer_Strip& b = env.strips[a.m_uFirstStrip + k];
            if (b.m_uLowestVertexUsed < nv) {
                const uint32_t available = nv - b.m_uLowestVertexUsed;
                sample_count += b.m_uNumberOfVertices < available ? b.m_uNumberOfVertices : available;
            }
        }
    }
    if (!sample_count || sample_count > 0xffffffffull) {
        unmap_file(&file);
        arena_reset(scratch, mark);
        return fail(err, "shade Env has no usable vertex samples or too many samples");
    }
    ShadeSample* samples = arena_array<ShadeSample>(arena, sample_count, err, false);
    if (!samples) {
        unmap_file(&file);
        arena_reset(scratch, mark);
        return false;
    }
    Asura_Vector_3 offset{};
    if (!shade_alignment_offset(cfg, target, scratch, &offset, err)) {
        unmap_file(&file);
        arena_reset(scratch, mark);
        return false;
    }
    uint32_t written = 0;
    for (uint32_t ai = 0; ai < env.module_count; ++ai) {
        const Asura_PC_EnvironmentRenderer_Module& a = env.modules[ai];
        if (a.m_uBufferIndex >= env.block_count)
            continue;
        const uint8_t* block = env.blocks[a.m_uBufferIndex];
        const uint32_t nv = read_u32(block);
        const uint8_t* verts = block + 8;
        for (uint32_t k = 0; k < a.m_uNumberOfStrips && a.m_uFirstStrip + k < env.strip_count; ++k) {
            const Asura_PC_EnvironmentRenderer_Strip& b = env.strips[a.m_uFirstStrip + k];
            const uint32_t available = b.m_uLowestVertexUsed < nv ? nv - b.m_uLowestVertexUsed : 0;
            const uint32_t take = b.m_uNumberOfVertices < available ? b.m_uNumberOfVertices : available;
            const uint32_t end = b.m_uLowestVertexUsed + take;
            for (uint32_t vi = b.m_uLowestVertexUsed; vi < end; ++vi) {
                const uint8_t* v = verts + static_cast<uint64_t>(vi) * sizeof(Asura_PC_EnvironmentRenderer_Vertex);
                samples[written++] = {{read_f32(v) + offset.x, read_f32(v + 4) + offset.y, read_f32(v + 8) + offset.z},
                                      {read_f32(v + 12), read_f32(v + 16), read_f32(v + 20)},
                                      read_u32(v + 24),
                                      b.m_uMaterialResponseHashID};
            }
        }
    }
    if (!written) {
        unmap_file(&file);
        arena_reset(scratch, mark);
        return fail(err, "shade Env produced no usable samples");
    }
    float nearest[128]{};
    uint32_t nearest_count = 0;
    const uint32_t stride = written > 128 ? written / 128 : 1;
    for (uint32_t si = 0; si < written && nearest_count < 128; si += stride) {
        float best = 3.402823466e+38f;
        const Asura_Vector_3 p = samples[si].position;
        for (uint32_t j = 0; j < written; ++j) {
            const float dx = samples[j].position.x - p.x, dy = samples[j].position.y - p.y,
                        dz = samples[j].position.z - p.z, d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > 1e-12f && d2 < best)
                best = d2;
        }
        if (best < 3.402823466e+38f)
            nearest[nearest_count++] = sqrt(best);
    }
    if (nearest_count)
        heap_sort(nearest, nearest_count, [](float a, float b) { return a < b; });
    const float cell = nearest_count ? fmax(.05f, nearest[nearest_count / 2] * 1.25f) : .25f;
    uint32_t cap = next_pow2(written > 0x3fffffffu ? 0x80000000u : written * 2 + 1);
    if (!cap) {
        unmap_file(&file);
        arena_reset(scratch, mark);
        return fail(err, "shade hash table size overflow");
    }
    ShadeBucket* buckets = arena_array<ShadeBucket>(arena, cap, err);
    uint32_t* next = arena_array<uint32_t>(arena, written, err, false);
    if (err->set) {
        unmap_file(&file);
        arena_reset(scratch, mark);
        return false;
    }
    for (uint32_t i = 0; i < written; ++i)
        next[i] = 0xffffffffu;
    const float inv = 1.0f / cell;
    for (uint32_t i = 0; i < written; ++i) {
        const Asura_Vector_3 p = samples[i].position;
        const int32_t x = static_cast<int32_t>(p.x * inv), y = static_cast<int32_t>(p.y * inv),
                      z = static_cast<int32_t>(p.z * inv);
        uint32_t slot = static_cast<uint32_t>(shade_cell_hash(x, y, z)) & (cap - 1);
        while (buckets[slot].used && (buckets[slot].x != x || buckets[slot].y != y || buckets[slot].z != z))
            slot = (slot + 1) & (cap - 1);
        if (!buckets[slot].used) {
            buckets[slot].used = 1;
            buckets[slot].x = x;
            buckets[slot].y = y;
            buckets[slot].z = z;
            buckets[slot].head = 0xffffffffu;
        }
        next[i] = buckets[slot].head;
        buckets[slot].head = i;
    }
    out->samples = samples;
    out->next = next;
    out->buckets = buckets;
    out->count = written;
    out->bucket_mask = cap - 1;
    out->cell_size = cell;
    unmap_file(&file);
    arena_reset(scratch, mark);
    return true;
}

bool shade_pick(const ShadeSource* shade, Asura_Vector_3 p, Asura_Vector_3 normal, uint32_t material, uint32_t* out_color) {
    if (!shade || !shade->count)
        return false;
    const float inv = 1.0f / shade->cell_size, max_d2 = shade->cell_size * shade->cell_size;
    const int32_t cx = static_cast<int32_t>(p.x * inv), cy = static_cast<int32_t>(p.y * inv),
                  cz = static_cast<int32_t>(p.z * inv);
    const float nl = sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (nl > 1e-12f) {
        normal.x /= nl;
        normal.y /= nl;
        normal.z /= nl;
    }
    for (uint32_t pass = 0; pass < 2; ++pass) {
        float best_score = 3.402823466e+38f;
        uint32_t best = 0xffffffffu;
        for (int32_t dz = -1; dz <= 1; ++dz)
            for (int32_t dy = -1; dy <= 1; ++dy)
                for (int32_t dx = -1; dx <= 1; ++dx) {
                    const int32_t x = cx + dx, y = cy + dy, z = cz + dz;
                    uint32_t slot = static_cast<uint32_t>(shade_cell_hash(x, y, z)) & shade->bucket_mask;
                    while (shade->buckets[slot].used &&
                           (shade->buckets[slot].x != x || shade->buckets[slot].y != y || shade->buckets[slot].z != z))
                        slot = (slot + 1) & shade->bucket_mask;
                    if (!shade->buckets[slot].used)
                        continue;
                    for (uint32_t i = shade->buckets[slot].head; i != 0xffffffffu; i = shade->next[i]) {
                        const ShadeSample& s = shade->samples[i];
                        if (!pass && s.material != material)
                            continue;
                        const float qx = s.position.x - p.x, qy = s.position.y - p.y, qz = s.position.z - p.z,
                                    d2 = qx * qx + qy * qy + qz * qz;
                        if (d2 > max_d2)
                            continue;
                        float score = d2;
                        if (nl > 1e-12f) {
                            const float sl =
                                sqrt(s.normal.x * s.normal.x + s.normal.y * s.normal.y + s.normal.z * s.normal.z);
                            if (sl > 1e-12f) {
                                float dot =
                                    (normal.x * s.normal.x + normal.y * s.normal.y + normal.z * s.normal.z) / sl;
                                dot = fmin(1.0f, fmax(-1.0f, dot));
                                const float nd = 1 - dot;
                                score += max_d2 * nd * nd;
                            }
                        }
                        if (score < best_score) {
                            best_score = score;
                            best = i;
                        }
                    }
                }
        if (best != 0xffffffffu) {
            *out_color = shade->samples[best].color;
            return true;
        }
    }
    return false;
}

struct CollTri {
    uint16_t a, b, c, flags, material;
};
struct ModuleMetric {
    Asura_Vector_3 translation;
    uint32_t vertex_count, triangle_count, link_count;
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
            uint32_t m = seg.m_uMaterialResponseHashID;
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
    const float dx = fmax(mx.x - mn.x, 1.0f), dy = fmax(mx.y - mn.y, 1.0f), dz = fmax(mx.z - mn.z, 1.0f);
    const float px = fmax(cfg.module_pad_min, dx * cfg.module_pad_scale),
                py = fmax(cfg.module_pad_min, dy * cfg.module_pad_scale),
                pz = fmax(cfg.module_pad_min, dz * cfg.module_pad_scale);
    const float bounds[6] = {mn.x - c.x - px, mx.x - c.x + px, mn.y - c.y - py,
                             mx.y - c.y + py, mn.z - c.z - pz, mx.z - c.z + pz};
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

bool append_skyb(Buffer* out, Error* err) {
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_SKYBOX, 7, 0, err);
    const float header[4] = {255.0f, 230.0f, 200.0f, 3.107175588607788f};
    buffer_append(out, header, sizeof(header), err);
    append_u32(out, 0, err);
    const char* names[] = {"\\sky\\fr.tga", "\\sky\\lf.tga",        "\\sky\\bk.tga",       "\\sky\\rt.tga",
                           "\\sky\\up.tga", "\\sky\\ch_04_sky.bmp", "\\sky\\ch_04_sky.bmp"};
    for (const char* name : names)
        append_padded_cstr(out, str_from_c(name), err);
    append_u32(out, 1, err);
    append_u32(out, 0, err);
    append_u32(out, 0, err);
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

namespace {

bool csv_has_u32(Str csv, uint32_t wanted) {
    if (!csv.size)
        return true;
    uint32_t at = 0;
    while (at < csv.size) {
        uint32_t end = at;
        while (end < csv.size && csv.data[end] != ',')
            ++end;
        uint64_t v = 0;
        if (parse_u64(str_trim({csv.data + at, end - at}), &v) && v == wanted)
            return true;
        at = end + 1;
    }
    return false;
}
bool csv_has_str(Str csv, Str wanted) {
    if (!csv.size)
        return true;
    uint32_t at = 0;
    while (at < csv.size) {
        uint32_t end = at;
        while (end < csv.size && csv.data[end] != ',')
            ++end;
        if (str_ieq(str_trim({csv.data + at, end - at}), str_trim(wanted)))
            return true;
        at = end + 1;
    }
    return false;
}
bool glob_i(Str text, Str pat) {
    if (!pat.size)
        return true;
    uint32_t ti = 0, pi = 0, star = 0xffffffffu, retry = 0;
    while (ti < text.size) {
        if (pi < pat.size && (pat.data[pi] == '?' || ascii_lower(pat.data[pi]) == ascii_lower(text.data[ti]))) {
            ++pi;
            ++ti;
            continue;
        }
        if (pi < pat.size && pat.data[pi] == '*') {
            star = pi++;
            retry = ti;
            continue;
        }
        if (star != 0xffffffffu) {
            pi = star + 1;
            ti = ++retry;
            continue;
        }
        return false;
    }
    while (pi < pat.size && pat.data[pi] == '*')
        ++pi;
    return pi == pat.size;
}
bool filter_name(Str name, Str filter) {
    if (!filter.size)
        return true;
    bool has_glob = false;
    for (uint32_t i = 0; i < filter.size; ++i)
        has_glob |= filter.data[i] == '*' || filter.data[i] == '?';
    return has_glob ? glob_i(name, filter) : str_contains_i(name, filter);
}

struct DiskFile {
    char path[MAX_PATH];
    char name[MAX_PATH];
    uint64_t size;
};
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

bool flip_dds_vertical(uint8_t* data, uint64_t size) {
    if (size < 128 || memcmp(data, "DDS ", 4) != 0 || read_u32(data + 4) != 124 || read_u32(data + 76) != 32)
        return false;
    const uint32_t height = read_u32(data + 12), width = read_u32(data + 16);
    if (!width || !height || memcmp(data + 84, "DX10", 4) == 0)
        return false;
    uint32_t block_bytes = 0;
    if (memcmp(data + 84, "DXT1", 4) == 0)
        block_bytes = 8;
    else if (memcmp(data + 84, "DXT5", 4) == 0)
        block_bytes = 16;
    else
        return false;
    const uint64_t blocks_w = (static_cast<uint64_t>(width) + 3) / 4,
                   blocks_h = (static_cast<uint64_t>(height) + 3) / 4, row_bytes = blocks_w * block_bytes;
    if (!row_bytes || row_bytes > size - 128 || blocks_h > (size - 128) / row_bytes)
        return false;
    uint8_t* image = data + 128;
    for (uint64_t y = 0; y < blocks_h; ++y)
        for (uint64_t x = 0; x < blocks_w; ++x) {
            uint8_t* block = image + y * row_bytes + x * block_bytes;
            if (block_bytes == 16) {
                uint64_t alpha = 0;
                for (uint32_t i = 0; i < 6; ++i)
                    alpha |= static_cast<uint64_t>(block[2 + i]) << (i * 8);
                uint64_t flipped = 0;
                for (uint32_t row = 0; row < 4; ++row)
                    for (uint32_t column = 0; column < 4; ++column) {
                        const uint32_t dst = 3 * (row * 4 + column), src = 3 * ((3 - row) * 4 + column);
                        flipped |= ((alpha >> src) & 7ull) << dst;
                    }
                for (uint32_t i = 0; i < 6; ++i)
                    block[2 + i] = static_cast<uint8_t>(flipped >> (i * 8));
                uint8_t t = block[12];
                block[12] = block[15];
                block[15] = t;
                t = block[13];
                block[13] = block[14];
                block[14] = t;
            } else {
                uint8_t t = block[4];
                block[4] = block[7];
                block[7] = t;
                t = block[5];
                block[5] = block[6];
                block[6] = t;
            }
        }
    uint8_t temp[16];
    for (uint64_t y = 0; y < blocks_h / 2; ++y)
        for (uint64_t x = 0; x < blocks_w; ++x) {
            uint8_t *a = image + y * row_bytes + x * block_bytes,
                    *b = image + (blocks_h - 1 - y) * row_bytes + x * block_bytes;
            memcpy(temp, a, block_bytes);
            memcpy(a, b, block_bytes);
            memcpy(b, temp, block_bytes);
        }
    return true;
}

bool append_file_rscf(Buffer* out, Str name, uint32_t type, uint32_t subtype, const char* path, Arena* scratch,
                      bool flip, Error* err) {
    ArenaMark mark = arena_mark(scratch);
    MappedFile f{};
    if (!map_file(path, &f, err))
        return false;
    if (f.size > 0xffffffffull) {
        unmap_file(&f);
        return fail(err, "resource exceeds 4 GiB: %s", path);
    }
    const uint8_t* data = f.data;
    if (flip && f.size) {
        uint8_t* copy = arena_array<uint8_t>(scratch, f.size, err, false);
        if (!copy) {
            unmap_file(&f);
            return false;
        }
        memcpy(copy, f.data, static_cast<size_t>(f.size));
        data = copy;
        uint64_t pixel_off = 0, row = 0, height = 0;
        Str ext = path_basename(str_from_c(path));
        if (flip_dds_vertical(copy, f.size)) {
        } else if (f.size >= 18 && str_ends_i(ext, str_lit(".tga")) && copy[2] == 2 &&
                   (copy[16] == 24 || copy[16] == 32)) {
            const uint32_t cmap = copy[1] ? read_u16(copy + 5) * ((copy[7] + 7) / 8) : 0;
            const uint32_t w = read_u16(copy + 12), h = read_u16(copy + 14);
            pixel_off = 18u + copy[0] + cmap;
            row = static_cast<uint64_t>(w) * (copy[16] / 8);
            height = h;
        } else if (f.size >= 54 && str_ends_i(ext, str_lit(".bmp")) && copy[0] == 'B' && copy[1] == 'M' &&
                   read_u32(copy + 14) >= 40 && read_u16(copy + 26) == 1 && read_u32(copy + 30) == 0 &&
                   (read_u16(copy + 28) == 24 || read_u16(copy + 28) == 32)) {
            const int32_t w = static_cast<int32_t>(read_u32(copy + 18)), h = static_cast<int32_t>(read_u32(copy + 22));
            if (w > 0 && h) {
                pixel_off = read_u32(copy + 10);
                row = align_up(static_cast<uint64_t>(w) * (read_u16(copy + 28) / 8), 4);
                height = h < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(h)) : static_cast<uint64_t>(h);
            }
        }
        if (row && height && pixel_off + row * height <= f.size) {
            uint8_t* temp = arena_array<uint8_t>(scratch, row, err, false);
            if (!temp) {
                unmap_file(&f);
                return false;
            }
            for (uint64_t y = 0; y < height / 2; ++y) {
                uint8_t *a = copy + pixel_off + y * row, *b = copy + pixel_off + (height - 1 - y) * row;
                memcpy(temp, a, static_cast<size_t>(row));
                memcpy(a, b, static_cast<size_t>(row));
                memcpy(b, temp, static_cast<size_t>(row));
            }
        }
    }
    const bool ok = append_rscf(out, name, type, subtype, data, static_cast<uint32_t>(f.size), err);
    unmap_file(&f);
    arena_reset(scratch, mark);
    return ok;
}

struct TextureSet {
    Vec<DiskFile> files;
};
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
                              scratch, false, err))
            return false;
    }
    // Global material table indexed exactly as the renderer expects (1000-based
    // exported material handles are folded back to their ordinal).
    uint32_t max_ord = 0;
    bool any = false;
    for (uint32_t i = 0; i < env.strip_count; ++i) {
        uint32_t m = env.strips[i].m_uMaterialResponseHashID, ord = m >= 1000 ? m - 1000 : m;
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
        uint32_t m = env.strips[i].m_uMaterialResponseHashID, ord = m >= 1000 ? m - 1000 : m;
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
    const char* files[] = {"fr.tga", "lf.tga", "bk.tga", "rt.tga", "up.tga", "ch_04_sky.bmp"};
    for (const char* fn : files) {
        char path[MAX_PATH];
        if (!join_path(path, MAX_PATH, cfg.sky_texture_dir, str_from_c(fn)))
            return fail(err, "sky file path too long");
        if (!file_exists(path))
            continue;
        char name[256];
        snprintf(name, sizeof(name), "\\graphics\\sky\\%s", fn);
        Str stem = path_stem(str_from_c(fn));
        const bool flip = cfg.sky_flip_faces.size && csv_has_str(cfg.sky_flip_faces, stem);
        if (!append_file_rscf(out, str_from_c(name), ASURA_RESOURCEFILE_TYPE_TEXTURE, 0, path, scratch, flip, err))
            return false;
    }
    return true;
}

} // namespace

namespace {

bool append_rsfl(Buffer* out, const Config& cfg, Arena* scratch, Error* err) {
    if (cfg.rsfl_from_pc) {
        ArenaMark mark = arena_mark(scratch);
        ChunkList donor{};
        if (!parse_chunks(cfg.rsfl_from_pc, &donor, scratch, err))
            return false;
        for (uint32_t i = 0; i < donor.count; ++i)
            if (donor.chunks[i].cid == ASURA_CHUNK_RESOURCEFILELIST) {
                const bool ok = append_chunk_copy(out, donor.chunks[i], err);
                unmap_file(&donor.file);
                arena_reset(scratch, mark);
                return ok;
            }
        unmap_file(&donor.file);
        arena_reset(scratch, mark);
        return fail(err, "RSFL donor contains no RSFL chunk: %s", cfg.rsfl_from_pc);
    }
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_RESOURCEFILELIST, 1, 0, err);
    append_u32(out, 0, err);
    return end_chunk(out, ch, err);
}

bool append_first_cid(Buffer* out, const char* path, uint32_t cid, Arena* scratch, bool required, bool* was_found,
                      Error* err) {
    if (was_found)
        *was_found = false;
    if (!path)
        return true;
    ArenaMark mark = arena_mark(scratch);
    ChunkList donor{};
    if (!parse_chunks(path, &donor, scratch, err))
        return false;
    bool found = false;
    for (uint32_t i = 0; i < donor.count; ++i)
        if (donor.chunks[i].cid == cid) {
            found = append_chunk_copy(out, donor.chunks[i], err);
            if (was_found)
                *was_found = found;
            break;
        }
    unmap_file(&donor.file);
    arena_reset(scratch, mark);
    if (!found && required)
        return fail(err, "donor '%s' contains no requested chunk", path);
    return !err->set;
}

bool append_filtered_rscf(Buffer* out, const Config& cfg, Arena* scratch, Error* err) {
    if (!cfg.rscf_from_pc)
        return true;
    ArenaMark mark = arena_mark(scratch);
    ChunkList donor{};
    if (!parse_chunks(cfg.rscf_from_pc, &donor, scratch, err))
        return false;
    uint32_t skipped = 0, added = 0;
    for (uint32_t i = 0; i < donor.count; ++i) {
        RscfInfo r{};
        if (!rscf_info(donor.chunks[i], &r))
            continue;
        if (str_ieq_c(r.name, cfg.env_name))
            continue;
        if (cfg.rscf_types.size && !csv_has_u32(cfg.rscf_types, r.type))
            continue;
        if (cfg.rscf_names.size && !csv_has_str(cfg.rscf_names, r.name))
            continue;
        if (!filter_name(r.name, cfg.rscf_name_filter))
            continue;
        if (skipped < cfg.rscf_skip) {
            ++skipped;
            continue;
        }
        if (cfg.rscf_limit_set && added >= cfg.rscf_limit)
            break;
        if (!append_chunk_copy(out, donor.chunks[i], err))
            break;
        ++added;
    }
    unmap_file(&donor.file);
    arena_reset(scratch, mark);
    return !err->set;
}

bool append_filtered_enti(Buffer* out, const Config& cfg, Arena* scratch, Error* err) {
    if (!cfg.enti_from_pc)
        return true;
    ArenaMark mark = arena_mark(scratch);
    ChunkList donor{};
    if (!parse_chunks(cfg.enti_from_pc, &donor, scratch, err))
        return false;
    for (uint32_t i = 0; i < donor.count; ++i) {
        const ChunkRef& ch = donor.chunks[i];
        if (ch.cid != ASURA_CHUNK_ENTITY || ch.size < sizeof(Asura_Chunk_Entity))
            continue;
        const uint32_t type =
            read_u16(ch.data + sizeof(Asura_Chunk_Header) + offsetof(Asura_Chunk_Entity_PayloadHeader, Classification));
        if (type == SnipeEntityClass_SpawnPoint && !cfg.enti_keep_spawnpoints)
            continue;
        if (cfg.enti_types.size && !csv_has_u32(cfg.enti_types, type))
            continue;
        if (!append_chunk_copy(out, ch, err))
            break;
    }
    unmap_file(&donor.file);
    arena_reset(scratch, mark);
    return !err->set;
}

bool append_smsg_boot(Buffer* out, const char* path, Arena* scratch, Error* err) {
    if (!path)
        return true;
    ArenaMark mark = arena_mark(scratch);
    ChunkList donor{};
    if (!parse_chunks(path, &donor, scratch, err))
        return false;
    int32_t first_enti = -1, smsg = -1;
    for (uint32_t i = 0; i < donor.count; ++i)
        if (donor.chunks[i].cid == ASURA_CHUNK_ENTITY) {
            first_enti = static_cast<int32_t>(i);
            break;
        }
    if (first_enti >= 0)
        for (int32_t i = first_enti - 1; i >= 0; --i)
            if (donor.chunks[i].cid == ASURA_CHUNK_STATICMESSAGES) {
                smsg = i;
                break;
            }
    if (smsg >= 0) {
        int32_t start = smsg;
        for (int32_t i = smsg - 1; i >= 0; --i) {
            uint32_t c = donor.chunks[i].cid;
            if (c == fourcc('S', 'H', 'P', 'D') || c == fourcc('S', 'H', 'A', 'P'))
                start = i;
            else
                break;
        }
        for (int32_t i = start; i <= smsg; ++i)
            append_chunk_copy(out, donor.chunks[i], err);
        for (int32_t i = smsg + 1; i < first_enti && donor.chunks[i].cid == ASURA_CHUNK_TEXTURENAMES; ++i)
            append_chunk_copy(out, donor.chunks[i], err);
    }
    unmap_file(&donor.file);
    arena_reset(scratch, mark);
    return !err->set;
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
                                            "stickgrenade", "tbomb",         "tnt"};
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

bool append_ambient(Buffer* out, const Config& cfg, Error* err) {
    if (!cfg.ambient_stream_path)
        return true;
    char path[28]{};
    Str s = str_from_c(cfg.ambient_stream_path);
    if (!str_contains_i(s, str_lit("\\")) && !str_contains_i(s, str_lit("/"))) {
        const char* pre = "Sounds\\Streams\\";
        if (strlen(pre) + s.size > 27)
            return fail(err, "ambient stream path exceeds 27 bytes");
        memcpy(path, pre, strlen(pre));
        memcpy(path + strlen(pre), s.data, s.size);
    } else {
        if (s.size > 27)
            return fail(err, "ambient stream path exceeds 27 bytes");
        for (uint32_t i = 0; i < s.size; ++i)
            path[i] = s.data[i] == '/' ? '\\' : s.data[i];
    }
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_STREAMINGBACKGROUNDSOUND, 1, 0, err);
    append_u32(out, 0, err);
    append_u32(out, 0, err);
    append_f32(out, cfg.ambient_volume, err);
    buffer_append(out, path, sizeof(path), err);
    return end_chunk(out, ch, err);
}

bool json_vec3_or(Json* object, const char* key, Asura_Vector_3 fallback, Asura_Vector_3* out) {
    Json* n = json_get(object, key);
    if (!n) {
        *out = fallback;
        return true;
    }
    float v[3];
    if (!json_floats(n, v, 3))
        return false;
    *out = {v[0], v[1], v[2]};
    return true;
}

bool append_lite_json(Buffer* out, const char* path, Arena* scratch, Error* err) {
    if (!path)
        return true;
    ArenaMark mark = arena_mark(scratch);
    MappedFile f{};
    if (!map_file(path, &f, err))
        return false;
    Json* root = json_parse(&f, scratch, err);
    if (!root || root->kind != JsonKind::Object) {
        unmap_file(&f);
        return fail(err, "lite JSON root must be an object");
    }
    Json* lights = json_get(root, "lights");
    uint32_t count = json_count(lights);
    if (!count)
        count = 1;
    ChunkMark ch = begin_chunk(out, ASURA_CHUNK_LIGHTS, 5, 0, err);
    append_u32(out, count, err);
    Asura_Vector_3 a, b, c;
    if (!json_vec3_or(root, "hdr_a", {80, 80, 80}, &a) || !json_vec3_or(root, "hdr_b", {.3f, .3f, .3f}, &b) ||
        !json_vec3_or(root, "hdr_c", {130, 100, 50}, &c)) {
        unmap_file(&f);
        return fail(err, "LITE header vectors must contain three numbers");
    }
    buffer_append(out, &a, sizeof(a), err);
    buffer_append(out, &b, sizeof(b), err);
    buffer_append(out, &c, sizeof(c), err);
    append_u32(out, static_cast<uint32_t>(json_integer(root, "hdr_c_flag", 1)), err);
    Json fallback{};
    fallback.kind = JsonKind::Object;
    Json* it = count == 1 && json_count(lights) == 0 ? &fallback : lights->child;
    for (uint32_t i = 0; i < count; ++i, it = it ? it->next : nullptr) {
        if (!it || it->kind != JsonKind::Object) {
            unmap_file(&f);
            return fail(err, "LITE lights must be objects");
        }
        Asura_Light rec{};
        Asura_Vector_3 colour{};
        if (!json_vec3_or(it, "pos", {0, 0, 0}, &rec.Position) ||
            !json_vec3_or(it, "dir", {0, 0, 0}, &rec.Direction) ||
            !json_vec3_or(it, "color_rgb255", {255, 255, 255}, &colour) ||
            !json_vec3_or(it, "repeat_pos", rec.Position, &rec.OldPosition)) {
            unmap_file(&f);
            return fail(err, "LITE vector must contain three numbers");
        }
        if (!json_get(it, "color_rgb255")) {
            Json* cn = json_get(it, "color");
            float cv[3];
            if (cn && json_floats(cn, cv, 3)) {
                colour = {cv[0], cv[1], cv[2]};
                if (fmax(fabs(colour.x), fmax(fabs(colour.y), fabs(colour.z))) <= 1.000001f) {
                    colour.x *= 255;
                    colour.y *= 255;
                    colour.z *= 255;
                }
            }
        }
        rec.R = colour.x;
        rec.G = colour.y;
        rec.B = colour.z;
        rec.Brightness = static_cast<float>(json_number(it, "intensity", 2.5));
        rec.Range = static_cast<float>(json_number(it, "radius", 1500));
        rec.m_fInnerRange = static_cast<float>(json_number(it, "inner_range", 0));
        rec.Angle = static_cast<float>(json_number(it, "cone_deg", 360));
        rec.m_uFlags = static_cast<uint32_t>(json_integer(it, "flags", ASURA_LIGHT_FLAG_AFFECTS_ENTITIES));
        rec.ShadowStrength = static_cast<float>(json_number(it, "shadow_strength", 0));
        rec.OldPosition = rec.Position;
        rec.OldRange = static_cast<float>(json_number(it, "old_range", rec.Range));
        rec.HasChanged = json_boolean(it, "has_changed", false);
        buffer_append(out, &rec, sizeof(rec), err);
    }
    const bool ok = end_chunk(out, ch, err);
    unmap_file(&f);
    arena_reset(scratch, mark);
    return ok;
}

bool append_spawnpoints(Buffer* out, const char* path, Arena* scratch, Error* err) {
    if (!path)
        return true;
    ArenaMark mark = arena_mark(scratch);
    MappedFile f{};
    if (!map_file(path, &f, err))
        return false;
    Json* root = json_parse(&f, scratch, err);
    if (!root || root->kind != JsonKind::Array) {
        unmap_file(&f);
        return fail(err, "spawnpoints JSON root must be an array");
    }
    uint32_t i = 0;
    for (Json* it = root->child; it; it = it->next, ++i) {
        if (it->kind != JsonKind::Object) {
            unmap_file(&f);
            return fail(err, "spawnpoint %u is not an object", i);
        }
        Snipe_ServerEntity_SpawnPoint_ChunkDataV0 data{};
        data.m_xPosition = {static_cast<float>(json_number(it, "x", NAN)),
                            static_cast<float>(json_number(it, "y", NAN)),
                            static_cast<float>(json_number(it, "z", NAN))};
        if (!isfinite(data.m_xPosition.x) || !isfinite(data.m_xPosition.y) || !isfinite(data.m_xPosition.z)) {
            unmap_file(&f);
            return fail(err, "spawnpoint %u needs x/y/z", i);
        }
        const float yaw = static_cast<float>(json_number(it, "yaw", 0) * 3.14159265358979323846 / 180.0),
                    pitch = static_cast<float>(json_number(it, "pitch", 0) * 3.14159265358979323846 / 180.0);
        data.m_xDirection = {static_cast<float>(cos(pitch) * sin(yaw)), static_cast<float>(sin(pitch)),
                             static_cast<float>(cos(pitch) * cos(yaw))};
        data.m_xEntity.Guid = static_cast<uint32_t>(json_integer(it, "guid", kSpawnGuidBase + i));
        data.m_xEntity.Classification = SnipeEntityClass_SpawnPoint;
        data.m_xEntity.m_usPadding =
            static_cast<uint16_t>(json_integer(it, "entity_padding", json_integer(it, "u16_unk", 0)));
        data.m_iSpawnIndex = static_cast<int32_t>(json_integer(it, "spawn_index", i));
        data.m_iPosture = static_cast<int32_t>(json_integer(it, "posture", 0));
        data.m_uTeamMask =
            static_cast<uint32_t>(json_integer(it, "team_mask", json_integer(it, "team", 1)));
        data.m_uGameModeMask = static_cast<uint32_t>(
            json_integer(it, "game_mode_mask", json_integer(it, "gamemode", 24)));
        data.m_fSpawnTimer = 5.0f;
        ChunkMark ch = begin_chunk(out, ASURA_CHUNK_ENTITY, 0, 0, err);
        buffer_append(out, &data, sizeof(data), err);
        end_chunk(out, ch, err);
    }
    if (!i) {
        unmap_file(&f);
        return fail(err, "spawnpoints JSON is empty");
    }
    unmap_file(&f);
    arena_reset(scratch, mark);
    return !err->set;
}

} // namespace

namespace {

struct SoundEntry {
    Str name;
    const char* file;
    Asura_Vector_3 position;
    float inner_radius, outer_radius;
    float legacy_volume_parameters[7];
    Asura_Vector_3 inner_cuboid_radius, outer_cuboid_radius;
    Asura_Bounding_Box retrigger_bounding_box;
    Asura_Quat orientation;
    uint32_t sound_resource_id, controller_guid, phonon_guid, flags;
    uint16_t controller_padding;
    bool emit_enti, active;
};
struct Sounds {
    SoundEntry* items;
    uint32_t count;
};

Json* json_first(Json* object, const char* a, const char* b = nullptr, const char* c = nullptr,
                 const char* d = nullptr) {
    Json* n = json_get(object, a);
    if (!n && b)
        n = json_get(object, b);
    if (!n && c)
        n = json_get(object, c);
    if (!n && d)
        n = json_get(object, d);
    return n;
}
bool json_array_or(Json* object, const char* a, const char* b, float* dst, uint32_t n, const float* defaults) {
    Json* v = json_first(object, a, b);
    if (!v) {
        memcpy(dst, defaults, n * sizeof(float));
        return true;
    }
    return json_floats(v, dst, n);
}

Str normalize_sound_name(Str raw, Arena* arena, Error* err) {
    raw = str_trim(raw);
    if (!raw.size) {
        fail(err, "sound resource name is empty");
        return {};
    }
    uint32_t sounds_at = 0xffffffffu;
    for (uint32_t i = 0; i + 6 <= raw.size; ++i)
        if (str_ieq({raw.data + i, 6}, str_lit("sounds")) &&
            (i == 0 || raw.data[i - 1] == '\\' || raw.data[i - 1] == '/') &&
            (i + 6 == raw.size || raw.data[i + 6] == '\\' || raw.data[i + 6] == '/')) {
            sounds_at = i;
            break;
        }
    const bool prefix = sounds_at == 0xffffffffu;
    Str src = prefix ? raw : Str{raw.data + sounds_at, raw.size - sounds_at};
    const uint32_t cap = src.size + (prefix ? 7 : 0) + 1;
    char* p = arena_array<char>(arena, cap, err, false);
    if (!p)
        return {};
    uint32_t at = 0;
    if (prefix) {
        memcpy(p, "sounds\\", 7);
        at = 7;
    }
    bool slash = false;
    for (uint32_t i = 0; i < src.size; ++i) {
        char c = src.data[i] == '/' ? '\\' : src.data[i];
        if (c == '\\') {
            if (!at || slash)
                continue;
            slash = true;
        } else
            slash = false;
        p[at++] = c;
    }
    while (at && p[at - 1] == '\\')
        --at;
    p[at] = 0;
    return {p, at};
}
const char* json_relative_file(Str raw, const char* json_path, Arena* arena, Error* err) {
    if (!raw.size)
        return nullptr;
    bool absolute = (raw.size >= 2 && raw.data[1] == ':') || raw.data[0] == '\\' || raw.data[0] == '/';
    uint32_t dir_len = 0;
    if (!absolute) {
        const uint32_t n = static_cast<uint32_t>(strlen(json_path));
        for (uint32_t i = 0; i < n; ++i)
            if (json_path[i] == '\\' || json_path[i] == '/')
                dir_len = i + 1;
    }
    char* p = arena_array<char>(arena, dir_len + raw.size + 1, err, false);
    if (!p)
        return nullptr;
    if (dir_len)
        memcpy(p, json_path, dir_len);
    for (uint32_t i = 0; i < raw.size; ++i)
        p[dir_len + i] = raw.data[i] == '/' ? '\\' : raw.data[i];
    p[dir_len + raw.size] = 0;
    return p;
}

bool load_sounds(const Config& cfg, Sounds* out, Arena* arena, Error* err) {
    memset(out, 0, sizeof(*out));
    if (!cfg.sounds_json)
        return true;
    MappedFile f{};
    if (!map_file(cfg.sounds_json, &f, err))
        return false;
    Json* root = json_parse(&f, arena, err);
    if (!root || root->kind != JsonKind::Array) {
        unmap_file(&f);
        return fail(err, "sounds JSON root must be an array");
    }
    const uint32_t count = json_count(root);
    if (!count) {
        unmap_file(&f);
        return fail(err, "sounds JSON is empty");
    }
    SoundEntry* items = arena_array<SoundEntry>(arena, count, err);
    if (!items) {
        unmap_file(&f);
        return false;
    }
    uint32_t next_id = 1, i = 0;
    const float da[7] = {0, 0, 0, 1, 1, 1, 1}, db[6] = {5, 5, 5, 10, 10, 10}, dc[6] = {0, 0, 0, 0, 0, 0},
                dd[4] = {0, 0, 0, 1};
    for (Json* it = root->child; it; it = it->next, ++i) {
        if (it->kind != JsonKind::Object) {
            unmap_file(&f);
            return fail(err, "sound %u is not an object", i);
        }
        SoundEntry& s = items[i];
        s.emit_enti = json_boolean(it, "emit_enti", json_boolean(it, "with_enti", false));
        const bool loop = json_boolean(it, "loop", !s.emit_enti);
        s.active = json_boolean(
            it, "active", json_boolean(it, "controller_active", json_boolean(it, "enabled", loop && s.emit_enti)));
        float pos[3];
        Json* pv = json_first(it, "pos", "position");
        if (pv) {
            if (!json_floats(pv, pos, 3)) {
                unmap_file(&f);
                return fail(err, "sound %u pos needs three numbers", i);
            }
            s.position = {pos[0], pos[1], pos[2]};
        } else {
            s.position = {static_cast<float>(json_number(it, "x", NAN)), static_cast<float>(json_number(it, "y", NAN)),
                          static_cast<float>(json_number(it, "z", NAN))};
            if (!isfinite(s.position.x) || !isfinite(s.position.y) || !isfinite(s.position.z)) {
                unmap_file(&f);
                return fail(err, "sound %u needs pos or x/y/z", i);
            }
        }
        float dist[2];
        Json* dv = json_first(it, "distance", "distances");
        if (dv) {
            if (!json_floats(dv, dist, 2)) {
                unmap_file(&f);
                return fail(err, "sound %u distance needs two numbers", i);
            }
            s.inner_radius = dist[0];
            s.outer_radius = dist[1];
        } else {
            s.inner_radius = static_cast<float>(
                json_number(it, "min_distance", json_number(it, "inner_radius", s.emit_enti ? 50 : 20)));
            s.outer_radius = static_cast<float>(
                json_number(it, "max_distance", json_number(it, "outer_radius", s.emit_enti ? 250 : 200)));
        }
        float cuboid[6], retrigger_box[6], orientation[4];
        if (!json_array_or(it, "sound_params_a", "params_a", s.legacy_volume_parameters, 7, da) ||
            !json_array_or(it, "sound_params_b", "params_b", cuboid, 6, db) ||
            !json_array_or(it, "sound_params_c", "params_c", retrigger_box, 6, dc) ||
            !json_array_or(it, "sound_params_d", "params_d", orientation, 4, dd)) {
            unmap_file(&f);
            return fail(err, "sound %u parameter array has the wrong length", i);
        }
        s.inner_cuboid_radius = {cuboid[0], cuboid[1], cuboid[2]};
        s.outer_cuboid_radius = {cuboid[3], cuboid[4], cuboid[5]};
        s.retrigger_bounding_box = {retrigger_box[0], retrigger_box[1], retrigger_box[2],
                                    retrigger_box[3], retrigger_box[4], retrigger_box[5]};
        s.orientation = {orientation[0], orientation[1], orientation[2], orientation[3]};
        s.controller_guid =
            static_cast<uint32_t>(json_integer(it, "controller_guid", json_integer(it, "guid", kSoundControllerGuidBase + i)));
        s.controller_padding = static_cast<uint16_t>(json_integer(
            it, "controller_padding", json_integer(it, "controller_u16_unk", json_integer(it, "u16_unk", 0x4974))));
        s.phonon_guid = static_cast<uint32_t>(json_integer(it, "phonon_guid", kPhononGuidBase + i));
        s.flags = static_cast<uint32_t>(json_integer(it, "flags", s.emit_enti ? (loop ? 3 : 2) : (loop ? 0x13 : 0x12)));
        Json* id = json_get(it, "sound_id");
        uint32_t requested = id ? static_cast<uint32_t>(json_integer(it, "sound_id", 0)) : 0;
        if (!requested) {
            Json* rid = json_first(it, "rscf_dummy", "resource_id", "resource_dummy");
            if (rid)
                requested = static_cast<uint32_t>(rid->kind == JsonKind::Number ? rid->number : 0);
        }
        s.sound_resource_id = requested ? requested : next_id++;
        if (s.sound_resource_id >= next_id)
            next_id = s.sound_resource_id + 1;
        Json* name = json_first(it, "rscf_name", "sound_name", "name");
        Json* file = json_first(it, "path", "file", "wav_path", "sound_path");
        if (name && name->kind == JsonKind::String)
            s.name = normalize_sound_name(name->string, arena, err);
        else if (file && file->kind == JsonKind::String)
            s.name = normalize_sound_name(file->string, arena, err);
        if (file && file->kind == JsonKind::String)
            s.file = json_relative_file(file->string, cfg.sounds_json, arena, err);
        if (!s.name.size && !id) {
            unmap_file(&f);
            return fail(err, "sound %u needs sound_id, resource name, or file", i);
        }
        if (err->set) {
            unmap_file(&f);
            return false;
        }
    }
    out->items = items;
    out->count = count;
    unmap_file(&f);
    return true;
}

bool append_sound_resources(Buffer* out, const Sounds& s, Arena* scratch, Error* err) {
    for (uint32_t i = 0; i < s.count; ++i)
        if (s.items[i].file) {
            if (!file_exists(s.items[i].file))
                return fail(err, "sound file not found: %s", s.items[i].file);
            if (!append_file_rscf(out, s.items[i].name, ASURA_RESOURCEFILE_TYPE_SOUND, s.items[i].sound_resource_id,
                                  s.items[i].file, scratch, false, err))
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

#ifndef ASURA_CONSTRUCT_NO_MAIN
int main(int argc, char** argv) {
    Error err{};
    Config cfg{};
    if (!parse_cli(argc, argv, &cfg, &err)) {
        if (err.set)
            console_format(true, "error: %s\n", err.message);
        return 2;
    }
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
    const char* lite = nullptr;
    bool lite_found = false;
    uint32_t modules = 0;
    int result = 1;
    if (!arena_init(&arena, cfg.arena_reserve, &err) || !arena_init(&scratch, cfg.arena_reserve, &err) ||
        !buffer_init(&output, cfg.output_reserve, &err))
        goto done;
    if (!map_file(cfg.obj, &obj_file, &err))
        goto done;
    if (!parse_obj(&obj_file, &obj, &arena, &err))
        goto done;
    if (!load_material_map(cfg, &material_map, &arena, &err))
        goto done;
    if (!load_shade_source(cfg, obj, &shade, &arena, &scratch, &err))
        goto done;
    if (!build_env(cfg, obj, material_map, shade.count ? &shade : nullptr, &arena, &scratch, &env, &err))
        goto done;
    env_payload = env.payload;
    if (!env_view(env_payload, &view, &arena, &err))
        goto done;
    if (view.module_count > kMaxAabbTreeObjects) {
        fail(&err, "generated Env has %u modules; the 2005 AABB tree supports at most %u", view.module_count,
             kMaxAabbTreeObjects);
        goto done;
    }
    if (!view.module_count) {
        fail(&err, "generated Env has no modules");
        goto done;
    }
    modules = view.module_count;
    metrics = arena_array<ModuleMetric>(&arena, modules, &err);
    if (!metrics)
        goto done;
    if (!load_sounds(cfg, &sounds, &arena, &err))
        goto done;
    buffer_append(&output, kAsuraMagic, sizeof(kAsuraMagic), &err);
    if (!append_fnfo(&output, &err) || !append_rsfl(&output, cfg, &scratch, &err))
        goto done;
    if (!append_weapon_support(&output, cfg, &scratch, &err) || !append_sky_resources(&output, cfg, &scratch, &err))
        goto done;
    if (!append_textures(&output, cfg, view, material_map, &arena, &scratch, &textures, &err))
        goto done;
    if (!append_rscf(&output, str_from_c(cfg.env_name), ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC,
                     ASURA_RESOURCEFILE_TYPE_PC_ENVIRONMENT, env_payload.base,
                     static_cast<uint32_t>(env_payload.size), &err))
        goto done;
    if (!append_filtered_rscf(&output, cfg, &scratch, &err) ||
        !append_sound_resources(&output, sounds, &scratch, &err) || !append_ambient(&output, cfg, &err))
        goto done;
    lite =
        cfg.lite_from_pc
            ? cfg.lite_from_pc
            : (cfg.smsg_from_pc ? cfg.smsg_from_pc
                                : (cfg.import_tex_from_pc ? cfg.import_tex_from_pc
                                                          : (cfg.rsfl_from_pc ? cfg.rsfl_from_pc : cfg.rscf_from_pc)));
    if (lite && !append_first_cid(&output, lite, ASURA_CHUNK_LIGHTS, &scratch, false, &lite_found, &err))
        goto done;
    if (!lite_found && cfg.lite_json && !append_lite_json(&output, cfg.lite_json, &scratch, &err))
        goto done;
    if (cfg.rscf_bootstrap_name &&
        !append_rscf_zero(&output, str_from_c(cfg.rscf_bootstrap_name), ASURA_RESOURCEFILE_TYPE_PLATFORMSPECIFIC,
                          cfg.rscf_bootstrap_subtype, cfg.rscf_bootstrap_payload_size, &err))
        goto done;
    if (!append_phon(&output, sounds, &err) ||
        !append_emod(&output, view, modules, cfg, material_map, &scratch, metrics, &err) ||
        !append_mlin(&output, metrics, modules, &err) ||
        !append_mrvb(&output, modules, &err) ||
        !append_nav1(&output, modules, &err))
        goto done;
    if (!append_smsg_boot(&output, cfg.smsg_from_pc, &scratch, &err) ||
        !append_filtered_enti(&output, cfg, &scratch, &err) || !append_sound_entities(&output, sounds, &err) ||
        !append_spawnpoints(&output, cfg.spawnpoints_json, &scratch, &err))
        goto done;
    if (!append_skyb(&output, &err) || !append_fog(&output, &err) || !append_wthr(&output, &err) ||
        buffer_append(&output, nullptr, sizeof(Asura_Chunk_Header), &err) == ~0ull)
        goto done;
    if (!write_entire_file(cfg.out, output.base, output.size, &err))
        goto done;
    console_format(false, "Wrote: %s (%llu bytes, %u Env modules, %u strips)\n", cfg.out,
                   static_cast<unsigned long long>(output.size), view.module_count, env.strip_count);
    console_write("MLIN: all-to-all\n");
    for (uint32_t i = 0; i < modules; ++i)
        console_format(false, "  module %u: collision vertices=%u, triangles=%u, links=%u\n", i,
                       metrics[i].vertex_count, metrics[i].triangle_count, metrics[i].link_count);
    result = 0;
done:
    if (result && err.set)
        console_format(true, "error: %s\n", err.message);
    unmap_file(&material_map.file);
    unmap_file(&obj_file);
    buffer_release(&env_payload);
    buffer_release(&output);
    arena_release(&scratch);
    arena_release(&arena);
    return result;
}
#endif
