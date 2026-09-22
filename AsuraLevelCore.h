#pragma once

#include "AsuraStructs.h"
#include "asura_base.hpp"

// Shared, source-level interface for the 2005 level file implementation.
// Wire-format records remain in AsuraStructs.h, where their layouts mirror
// the structures recovered from the target and reference IDBs.
namespace asura::level {

inline constexpr uint8_t kAsuraMagic[8] = {'A', 's', 'u', 'r', 'a', ' ', ' ', ' '};
inline constexpr uint32_t kMaxAabbTreeObjects = 65535;
inline constexpr uint32_t kToolCreatedGuidFirst = 0x186a0;
inline constexpr uint32_t kToolCreatedGuidLast = 0x30d3f;
inline constexpr uint32_t kSoundControllerGuidBase = 0x18c40;

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

inline uint32_t read_u32(const void* data) {
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

inline uint16_t read_u16(const void* data) {
    uint16_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

inline float read_f32(const void* data) {
    float value;
    memcpy(&value, data, sizeof(value));
    return value;
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

struct RscfInfo {
    uint32_t type;
    uint32_t subtype;
    uint32_t payload_size;
    Str name;
    const uint8_t* payload;
};

struct ChunkMark {
    uint64_t start;
};

struct Config {
    const char* obj;
    const char* env_name;
    bool flip_y, flip_z;
    bool allow_unknown_materials;
    uint32_t diffuse_abgr;
    uint32_t max_prim_count;
    uint32_t max_collision_polys;
    float auto_block_xz_cell;
    const char* material_map;
    const char* texture_dir;
    const char* texture_prefix;
    const char* sky_texture_dir;
    const char* sky_texture_paths[ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT];
    const char* weapon_from_pc;
    uint64_t arena_reserve;
    uint64_t output_reserve;
};

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

struct VertexKey {
    uint32_t v, vt, vn;
};

struct MaterialMap {
    MappedFile file;
    Json* root;
    const uint32_t* default_collision_flags = nullptr;
    uint32_t default_collision_flag_count = 0;
    // Imported geometry can carry flags per face, independently of materials.
    const void* collision_flag_context = nullptr;
    uint32_t (*resolve_collision_flags)(const void*, uint32_t, const Asura_Vector_3*, uint32_t) = nullptr;
};

struct EnvBuild {
    Buffer payload;
    uint32_t modules;
    uint32_t strip_count;
};

struct EnvView {
    const uint8_t* data;
    uint32_t size;
    const Asura_PC_EnvironmentRenderer_Module* modules;
    const Asura_PC_EnvironmentRenderer_Strip* strips;
    uint32_t module_count, strip_count, block_count;
    const uint8_t** blocks;
    uint32_t* block_sizes;
};

struct ModuleMetric {
    Asura_Vector_3 translation;
    uint32_t vertex_count, triangle_count, link_count;
};

struct DiskFile {
    char path[MAX_PATH];
    char name[MAX_PATH];
    uint64_t size;
};

struct TextureSet {
    Vec<DiskFile> files;
};

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

bool parse_chunks(const char* path, ChunkList* out, Arena* arena, Error* err);
bool parse_chunks(const wchar_t* path, ChunkList* out, Arena* arena, Error* err);
Str padded_string_at(const uint8_t* data, uint32_t size, uint32_t at);
bool rscf_info(const ChunkRef& chunk, RscfInfo* out);
ChunkMark begin_chunk(Buffer* out, ASURA_CHUNKID cid, uint32_t version, uint32_t flags, Error* err);
bool end_chunk(Buffer* out, ChunkMark mark, Error* err);
bool append_chunk_copy(Buffer* out, const ChunkRef& chunk, Error* err);
bool mark_material_support(const ChunkList& chunks, uint32_t before_chunk,
                            uint8_t* wanted, Error* err);
bool append_rscf(Buffer* out, Str name, uint32_t type, uint32_t subtype, const void* payload,
                 uint32_t payload_size, Error* err, uint32_t version = 0);
bool patch_fnfo_file_size(Buffer* out, Error* err);

bool initialize_editor_config(const char* obj_path, Config* out, Error* err);
Asura_Vector_3 transform_vec(Asura_Vector_3 value, const Config& config);
bool transform_reverses_winding(const Config& config);
bool parse_obj(MappedFile* file, ObjData* out, Arena* arena, Error* err);
int32_t resolve_obj_index(int32_t raw, uint32_t count);
bool face_keys(const ObjData& obj, const ObjFace& face, VertexKey keys[3], Error* err);
bool load_material_map(const Config& config, MaterialMap* out, Arena* arena, Error* err);
bool resolve_material(const MaterialMap& materials, Str name, bool allow_unknown, uint32_t* out, Error* err);
Str material_texture_name(const MaterialMap& materials, uint32_t material_ordinal);
uint32_t material_override(const MaterialMap& materials, const char* section, uint32_t material_ordinal,
                           uint32_t fallback);
bool build_env(const Config& config, const ObjData& obj, const MaterialMap& materials,
               Arena* arena, Arena* scratch, EnvBuild* out, Error* err);
bool env_view(const Buffer& payload, EnvView* out, Arena* arena, Error* err);

bool append_fnfo(Buffer* out, Error* err);
bool append_text(Buffer* out, Str* names, uint32_t count, Error* err);
bool append_txfl(Buffer* out, uint32_t count, Error* err);
bool append_mtrl(Buffer* out, const int32_t* tex, const uint32_t* flags, const uint32_t* surface,
                 uint32_t count, Error* err);
bool append_rsfl(Buffer* out, Error* err);
bool append_weapon_support(Buffer* out, const Config& config, Arena* scratch, Error* err);
bool append_sky_resources(Buffer* out, const Config& config, Arena* scratch, Error* err);
bool append_textures(Buffer* out, const Config& config, const EnvView& env, const MaterialMap& materials,
                     Arena* arena, Arena* scratch, TextureSet* textures, Error* err);
bool append_sound_resources(Buffer* out, const Sounds& sounds, Arena* scratch, Error* err);
bool append_phon(Buffer* out, const Sounds& sounds, Error* err);
bool append_emod(Buffer* out, const EnvView& env, uint32_t modules, const Config& config,
                 const MaterialMap& materials, Arena* scratch, ModuleMetric* metrics, Error* err);
bool append_mlin(Buffer* out, ModuleMetric* metrics, uint32_t module_count, Error* err);
bool append_mrvb(Buffer* out, uint32_t module_count, Error* err);
bool append_nav1(Buffer* out, uint32_t module_count, Error* err);
bool append_sound_entities(Buffer* out, const Sounds& sounds, Error* err);
bool append_fog(Buffer* out, Error* err);
bool append_wthr(Buffer* out, Error* err);
bool text_name_matches_resource(Str text, Str resource);
bool list_files(const char* directory, Arena* arena, Vec<DiskFile>* files, Error* err);

} // namespace asura::level
