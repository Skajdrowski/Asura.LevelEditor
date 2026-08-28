#pragma once

#include "AsuraLevelCore.h"

#include <array>
#include <string>
#include <vector>

namespace editor {

inline constexpr size_t kPickupBodySize = sizeof(Snipe_ServerEntity_Pickup_ChunkDataV0);

enum class EntityKind : uint32_t {
    SpawnPoint,
    Light,
    Sound,
    Pickup,
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
    std::array<uint8_t, kPickupBodySize> pickup_body{};
};

struct PickupTemplate {
    uint32_t item_id = 0;
    float health = 0;
    uint32_t file_id = 0;
    uint32_t skin_id = 0;
    uint32_t anim_id = 0;
    uint32_t anim_file_id = 0;
    uint16_t entity_padding = 0;
    std::array<uint8_t, kPickupBodySize> body{};
};

struct SkyboxSettings {
    uint32_t chunk_version = 7;
    float red = 255.0f;
    float green = 230.0f;
    float blue = 200.0f;
    float orientation_radians = 3.107175588607788f;
    std::array<std::string, ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT> texture_paths{
        "", "\\sky\\fr.tga", "\\sky\\lf.tga", "\\sky\\bk.tga", "\\sky\\rt.tga",
        "\\sky\\up.tga", "\\sky\\ch_04_sky.bmp", "\\sky\\ch_04_sky.bmp"};
    bool draw_clouds = true;
    // These are independent target compatibility flags, not SKYB versions.
    // Retail v7 levels legitimately contain either flag, both, or neither.
    bool back_texture_is_front_upside_down = false;
    bool right_texture_is_left_upside_down = false;
    // Imported SKYB chunks are rebuilt from these fields during source-PC
    // export. Older projects leave this false and preserve their source chunk.
    bool source_record = false;
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
    uint32_t next_guid = asura::level::kToolCreatedGuidFirst;
    Asura_Vector_3 light_header_a{80, 80, 80};
    Asura_Vector_3 light_header_b{.3f, .3f, .3f};
    Asura_Vector_3 light_header_c{130, 100, 50};
    uint32_t light_header_flag = 1;
    SkyboxSettings skybox;
    bool rain_enabled = false;
    bool weather_source_record = false;
    // Target path consumed by the independent SBSN streaming ambience system,
    // for example "Sounds\\Streams\\m1_karl1.wav".  An empty path disables
    // the default stream while retaining any imported regional sound records.
    std::string ambient_stream_path;
    float ambient_volume = 1.0f;
    bool ambient_source_record = false;
    // True only when every source physical-object ENTI was imported. This
    // makes absence from entities an intentional deletion during export.
    bool source_pickup_inventory_complete = false;
    bool dirty = false;
};

struct Mesh {
    std::vector<Asura_Vector_3> positions;
    // Rendering attributes parallel to positions. Importers keep exact OBJ/PC
    // UVs and prelit diffuse values; an empty vector means the attribute was
    // unavailable and the renderer supplies a conservative fallback.
    std::vector<Asura_Vector_3> normals;
    std::vector<Asura_Vector_2> texcoords;
    std::vector<uint32_t> diffuse_abgr;
    std::vector<std::array<uint32_t, 3>> faces;
    // Target ENV original material index parallel to faces (-1 means none).
    std::vector<int32_t> face_materials;
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

} // namespace editor
