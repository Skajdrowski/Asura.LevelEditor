#include "LevelEditorInternal.h"

#include <initializer_list>
#include <new>
#include <unordered_map>

using namespace asura;
using namespace asura::level;

namespace editor {

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

struct GpuPuppetRange {
    uint32_t start_vertex = 0;
    uint32_t vertex_count = 0;
    const SpawnPuppetMaterial* material = nullptr;
    bool selected = false;
    bool two_sided = false;
};

struct GpuModelTexture {
    std::string name;
    uint64_t fingerprint = 0;
    ID3D11ShaderResourceView* view = nullptr;
};

struct GpuEntitySnapshot {
    const SpawnPuppet* model = nullptr;
    const SpawnPuppetVertex* vertices = nullptr;
    const std::array<uint16_t, 3>* faces = nullptr;
    const SpawnPuppetMaterial* materials = nullptr;
    size_t face_count = 0;
    EntityKind kind = EntityKind::SpawnPoint;
    Asura_Vector_3 position{};
    Asura_Vector_3 rotation{};
    Asura_Vector_3 spawn_direction{};
    Asura_Bounding_Box source_bounds{};
    Asura_Bounding_Box light_bounds{};
    float sound_range = 0.0f;
    float light_range = 0.0f;
    uint32_t value_u32_a = 0;
    uint32_t light_flags = 0;
    uint32_t selection_order = 0;
};

struct GpuModelLookupSnapshot {
    uint32_t id = 0;
    const SpawnPuppet* model = nullptr;
    const std::array<uint16_t, 3>* faces = nullptr;
    size_t face_count = 0;
    bool operator==(const GpuModelLookupSnapshot&) const = default;
};

bool same_vec3(const Asura_Vector_3& a, const Asura_Vector_3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool same_bounds(const Asura_Bounding_Box& a, const Asura_Bounding_Box& b) {
    return a.MinX == b.MinX && a.MaxX == b.MaxX && a.MinY == b.MinY && a.MaxY == b.MaxY &&
           a.MinZ == b.MinZ && a.MaxZ == b.MaxZ;
}

bool same_entity_snapshot(const GpuEntitySnapshot& a, const GpuEntitySnapshot& b) {
    return a.model == b.model && a.kind == b.kind && same_vec3(a.position, b.position) &&
           same_vec3(a.rotation, b.rotation) && same_vec3(a.spawn_direction, b.spawn_direction) &&
           same_bounds(a.source_bounds, b.source_bounds) && same_bounds(a.light_bounds, b.light_bounds) &&
           a.sound_range == b.sound_range && a.light_range == b.light_range &&
           a.value_u32_a == b.value_u32_a && a.light_flags == b.light_flags &&
           a.selection_order == b.selection_order &&
           a.vertices == b.vertices && a.faces == b.faces && a.materials == b.materials &&
           a.face_count == b.face_count;
}

bool same_model_lookup_snapshots(const std::vector<GpuModelLookupSnapshot>& a,
                                 const std::vector<GpuModelLookupSnapshot>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!(a[i] == b[i]))
            return false;
    return true;
}

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
    ID3D11RasterizerState* rasterizer_cull_back = nullptr;
    ID3D11RasterizerState* rasterizer_no_cull = nullptr;
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
    std::vector<GpuModelTexture> model_textures;
    std::vector<GpuVertex> rain_scratch;
    std::vector<GpuVertex> puppet_scratch;
    std::vector<GpuPuppetRange> puppet_range_scratch;
    std::vector<GpuVertex> overlay_scratch;
    std::vector<uint32_t> entity_selection_order_scratch;
    std::vector<const SpawnPuppet*> entity_models_scratch;
    std::vector<GpuEntitySnapshot> entity_snapshot;
    std::vector<GpuEntitySnapshot> entity_snapshot_scratch;
    std::vector<GpuModelLookupSnapshot> pickup_lookup_snapshot;
    std::vector<GpuModelLookupSnapshot> pickup_lookup_snapshot_scratch;
    std::vector<GpuModelLookupSnapshot> static_lookup_snapshot;
    std::vector<GpuModelLookupSnapshot> static_lookup_snapshot_scratch;
    std::unordered_map<uint32_t, const SpawnPuppet*> pickup_lookup;
    std::unordered_map<uint32_t, const SpawnPuppet*> static_lookup;
    uint32_t cached_unselected_puppet_count = 0;
    uint32_t cached_selected_puppet_count = 0;
    uint32_t cached_entity_start = 0;
    uint32_t cached_selected_entity_start = 0;
    uint32_t cached_overlay_vertex_count = 0;
    float cached_overlay_mesh_radius = -1.0f;
    bool cached_puppets_ready = false;
    bool cached_overlay_ready = false;
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

void gpu_release_object(IUnknown* object) {
    if (object)
        object->Release();
}

#define gpu_release(object) do { gpu_release_object(object); (object) = nullptr; } while (false)

HRESULT gpu_compile_shader(const char* source, size_t source_size, const char* entry,
                           const char* target, ID3DBlob** output) {
    ID3DBlob* errors = nullptr;
    const HRESULT result = D3DCompile(source, source_size, nullptr, nullptr, nullptr, entry, target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, output, &errors);
    gpu_release(errors);
    return result;
}

void gpu_release_skybox_textures() {
    for (ID3D11ShaderResourceView*& face : gpu.skybox_faces)
        gpu_release(face);
    gpu_release(gpu.skybox_cloud);
    gpu.skybox_active = false;
}

void gpu_release_model_textures() {
    for (GpuModelTexture& texture : gpu.model_textures)
        gpu_release(texture.view);
    gpu.model_textures.clear();
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

ID3D11ShaderResourceView* gpu_model_texture_view(const SpawnPuppetMaterial* material) {
    if (!material || material->texture_bytes.empty())
        return gpu.white_texture;
    for (const GpuModelTexture& cached : gpu.model_textures) {
        if (cached.fingerprint == material->texture_fingerprint && cached.name == material->texture_name)
            return cached.view ? cached.view : gpu.white_texture;
    }

    GpuModelTexture cached;
    cached.name = material->texture_name;
    cached.fingerprint = material->texture_fingerprint;
    std::string ignored_error;
    gpu_create_dds_view_from_memory(material->texture_bytes.data(), material->texture_bytes.size(),
                                    material->texture_name.c_str(), &cached.view, &ignored_error);
    gpu.model_textures.push_back(std::move(cached));
    ID3D11ShaderResourceView* view = gpu.model_textures.back().view;
    return view ? view : gpu.white_texture;
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


bool pc_texture_resource(const ChunkList& chunks, Str texture_name, RscfInfo* output);
bool gpu_has_material_flag(uint32_t mask, bool require_texture);

std::string parent_folder_of(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

void texture_search_roots(const std::string& level_path, std::string (&roots)[5]) {
    char module_path[MAX_PATH * 4]{};
    GetModuleFileNameA(nullptr, module_path, static_cast<DWORD>(sizeof(module_path)));
    roots[1] = parent_folder_of(module_path);
    roots[2] = parent_folder_of(parent_folder_of(roots[1]));
    roots[3] = parent_folder_of(level_path);
    roots[4] = parent_folder_of(roots[3]);
}

bool gpu_load_global_texture(const char* relative_path, const std::string& level_path,
                             ID3D11ShaderResourceView** output, std::string* why) {
    if (*output)
        return true;
    std::string roots[5];
    texture_search_roots(level_path, roots);
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
    const size_t slash = level_path.find_last_of("\\/");
    const std::string basename =
        slash == std::string::npos ? level_path : level_path.substr(slash + 1);
    std::vector<std::string> stems;
    const auto append_stem = [&stems](std::string stem) {
        if (stem.empty())
            return;
        for (const std::string& existing : stems)
            if (existing == stem)
                return;
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

    std::string roots[5];
    texture_search_roots(level_path, roots);
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
                std::string roots[5];
                texture_search_roots(pc_path, roots);
                const std::string candidates[] = {
                    "SpecialFX\\splash.dds",
                    "SpecialFX\\splash.bmp",
                    roots[1] + "\\SpecialFX\\splash.dds",
                    roots[2] + "\\SpecialFX\\splash.dds",
                    roots[3] + "\\SpecialFX\\splash.dds",
                    roots[4] + "\\SpecialFX\\splash.dds",
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
        const bool needs_detail = gpu_has_material_flag(0x4u, true);
        const bool needs_spheremap = gpu_has_material_flag(0x80u, true);
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

bool gpu_has_material_flag(uint32_t mask, bool require_texture) {
    for (const GpuMaterialRange& range : gpu.material_ranges) {
        if ((!require_texture || range.texture) && (range.material_flags & mask) != 0)
            return true;
    }
    return false;
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

bool gpu_reload_environment_textures(std::string* why, uint32_t* loaded_count,
                                     uint32_t* missing_count) {
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
        const bool needs_splash = gpu_has_material_flag(0x4000u, false);
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

namespace {

struct SkyboxTextureCandidate {
    std::string source_path;
    std::string target_stem;
    int semantic_slot = -1;
    bool assigned = false;
};

std::string lowercase_ascii(std::string value) {
    for (char& c : value)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return value;
}

bool skybox_name_has_token(const std::string& stem, const char* wanted) {
    size_t at = 0;
    while (at < stem.size()) {
        while (at < stem.size() && !((stem[at] >= 'a' && stem[at] <= 'z') ||
                                     (stem[at] >= '0' && stem[at] <= '9')))
            ++at;
        const size_t begin = at;
        while (at < stem.size() && ((stem[at] >= 'a' && stem[at] <= 'z') ||
                                    (stem[at] >= '0' && stem[at] <= '9')))
            ++at;
        if (stem.compare(begin, at - begin, wanted) == 0)
            return true;
    }
    return false;
}

int skybox_semantic_slot(const std::string& raw_stem) {
    const std::string stem = lowercase_ascii(raw_stem);
    const auto has = [&stem](std::initializer_list<const char*> names) {
        for (const char* name : names)
            if (skybox_name_has_token(stem, name))
                return true;
        return false;
    };
    // SKYB slots are lower, front, left, back, right, upper, cloud A/B.
    // Directional aliases are only a convenience; unrecognised DDS names are
    // still assigned deterministically below.
    if (has({"bottom", "down", "dn", "lower", "ny"}))
        return 0;
    if (has({"front", "forward", "fr", "ft", "pz"}))
        return 1;
    if (has({"left", "lf", "lt", "nx"}))
        return 2;
    if (has({"back", "backward", "rear", "bk", "nz"}))
        return 3;
    if (has({"right", "rt", "px"}))
        return 4;
    if (has({"upper", "top", "up", "py"}))
        return 5;
    if (stem.find("cloud") != std::string::npos ||
        (skybox_name_has_token(stem, "ch") && skybox_name_has_token(stem, "sky")))
        return 6;
    return -1;
}

void assign_skybox_candidate(SkyboxTextureScan* scan, uint32_t slot,
                             SkyboxTextureCandidate* candidate) {
    scan->source_paths[slot] = candidate->source_path;
    scan->texture_paths[slot] = "\\sky\\" + candidate->target_stem;
    candidate->assigned = true;
}

} // namespace

bool scan_skybox_texture_folder(const std::string& directory, SkyboxTextureScan* scan,
                                std::string* why) {
    if (!scan) {
        if (why)
            *why = "No skybox scan destination was supplied.";
        return false;
    }
    *scan = {};
    if (directory.empty()) {
        if (why)
            *why = "No skybox texture folder was selected.";
        return false;
    }
    char search[MAX_PATH * 4]{};
    if (!join_path(search, sizeof(search), directory.c_str(), str_from_c("*")))
        return why ? (*why = "The skybox texture folder path is too long.", false) : false;
    WIN32_FIND_DATAA entry{};
    HANDLE find = FindFirstFileA(search, &entry);
    if (find == INVALID_HANDLE_VALUE) {
        if (why)
            *why = "Could not scan the selected skybox texture folder.";
        return false;
    }
    std::vector<SkyboxTextureCandidate> candidates;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char candidate[MAX_PATH * 4]{};
        if (!join_path(candidate, sizeof(candidate), directory.c_str(), str_from_c(entry.cFileName)))
            continue;
        std::ifstream file(candidate, std::ios::binary);
        char magic[4]{};
        file.read(magic, sizeof(magic));
        if (file.gcount() != sizeof(magic) || memcmp(magic, "DDS ", sizeof(magic)) != 0)
            continue;
        std::string target_stem = entry.cFileName;
        // MCP2's resource hash stops at the first dot. Use that same basename
        // for generated SKYB/RSCF names so extensions (including fake .tga or
        // .bmp suffixes on DDS payloads) never participate in the lookup.
        const size_t dot = target_stem.find('.');
        if (dot != std::string::npos)
            target_stem.resize(dot);
        if (target_stem.empty())
            continue;
        const auto duplicate = std::find_if(
            candidates.begin(), candidates.end(), [&target_stem](const SkyboxTextureCandidate& existing) {
                return _stricmp(existing.target_stem.c_str(), target_stem.c_str()) == 0;
            });
        if (duplicate != candidates.end()) {
            FindClose(find);
            if (why)
                *why = "Two DDS files have the same target basename (extensions are ignored): " +
                       target_stem;
            return false;
        }
        candidates.push_back({candidate, target_stem, skybox_semantic_slot(target_stem)});
    } while (FindNextFileA(find, &entry));
    FindClose(find);
    if (candidates.empty()) {
        if (why)
            *why = "The selected folder contains no files with DDS payloads.";
        return false;
    }
    if (candidates.size() > ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT) {
        if (why)
            *why = "The selected folder contains more than eight DDS textures; use a folder containing only one skybox.";
        return false;
    }
    // There are at most eight candidates from here on. A tiny insertion sort
    // avoids instantiating the general introsort machinery for this fixed-small
    // editor list while preserving the exact previous comparator/order.
    for (size_t i = 1; i < candidates.size(); ++i) {
        SkyboxTextureCandidate candidate = std::move(candidates[i]);
        size_t at = i;
        while (at) {
            const SkyboxTextureCandidate& previous = candidates[at - 1];
            const int order = _stricmp(previous.target_stem.c_str(), candidate.target_stem.c_str());
            if (order < 0 || (order == 0 && previous.target_stem <= candidate.target_stem))
                break;
            candidates[at] = std::move(candidates[at - 1]);
            --at;
        }
        candidates[at] = std::move(candidate);
    }
    scan->file_count = static_cast<uint32_t>(candidates.size());

    bool slot_used[ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT]{};
    for (SkyboxTextureCandidate& candidate : candidates) {
        if (candidate.semantic_slot < 0 || candidate.semantic_slot >= 6 ||
            slot_used[candidate.semantic_slot])
            continue;
        assign_skybox_candidate(scan, static_cast<uint32_t>(candidate.semantic_slot), &candidate);
        slot_used[candidate.semantic_slot] = true;
    }
    for (SkyboxTextureCandidate& candidate : candidates) {
        if (candidate.assigned || candidate.semantic_slot != 6)
            continue;
        const uint32_t slot = !slot_used[6] ? 6u : 7u;
        if (slot_used[slot])
            continue;
        assign_skybox_candidate(scan, slot, &candidate);
        slot_used[slot] = true;
    }

    size_t remaining = 0;
    for (const SkyboxTextureCandidate& candidate : candidates)
        remaining += !candidate.assigned;
    size_t free_non_lower_faces = 0;
    for (uint32_t slot = 1; slot < 6; ++slot)
        free_non_lower_faces += !slot_used[slot];
    const bool need_lower_face = !slot_used[0] && remaining > free_non_lower_faces;
    std::vector<uint32_t> free_slots;
    if (need_lower_face)
        free_slots.push_back(0);
    for (uint32_t slot = 1; slot < 6; ++slot)
        if (!slot_used[slot])
            free_slots.push_back(slot);
    if (!need_lower_face && !slot_used[0])
        free_slots.push_back(0);
    for (uint32_t slot = 6; slot < ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT; ++slot)
        if (!slot_used[slot])
            free_slots.push_back(slot);

    size_t free_index = 0;
    for (SkyboxTextureCandidate& candidate : candidates) {
        if (candidate.assigned)
            continue;
        if (free_index >= free_slots.size()) {
            if (why)
                *why = "Could not assign every discovered DDS texture to a SKYB slot.";
            return false;
        }
        assign_skybox_candidate(scan, free_slots[free_index++], &candidate);
    }
    // The target exposes two independently scrolling cloud slots. Reusing one
    // discovered cloud in both slots preserves the common single-cloud setup.
    if (!scan->source_paths[6].empty() && scan->source_paths[7].empty()) {
        scan->source_paths[7] = scan->source_paths[6];
        scan->texture_paths[7] = scan->texture_paths[6];
    }
    return true;
}

std::string skybox_texture_hash_stem(const std::string& path) {
    if (path.empty())
        return {};
    const Str source{path.data(), static_cast<uint32_t>(path.size())};
    const Str name = path_basename(source);
    uint32_t length = name.size;
    for (uint32_t i = 0; i < name.size; ++i)
        if (name.data[i] == '.') {
            length = i;
            break;
        }
    return {name.data, length};
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
    SkyboxTextureScan scan{};
    if (!scan_skybox_texture_folder(directory, &scan, why))
        return false;
    gpu.skybox_tint = {1, 1, 1, 1};
    bool has_explicit_paths = false;
    for (const std::string& texture_path : g.document.skybox.texture_paths)
        has_explicit_paths |= !texture_path.empty();
    const auto source_for = [&scan](const std::string& texture_path) -> const std::string* {
        const std::string wanted = skybox_texture_hash_stem(texture_path);
        if (wanted.empty())
            return nullptr;
        for (uint32_t candidate = 0; candidate < ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT; ++candidate) {
            if (_stricmp(skybox_texture_hash_stem(scan.texture_paths[candidate]).c_str(), wanted.c_str()) == 0)
                return &scan.source_paths[candidate];
        }
        return nullptr;
    };
    ID3D11ShaderResourceView* next[6]{};
    ID3D11ShaderResourceView* next_cloud = nullptr;
    uint32_t found = 0;
    for (uint32_t slot = 0; slot < _countof(next); ++slot) {
        const std::string* source = has_explicit_paths
                                        ? source_for(g.document.skybox.texture_paths[slot])
                                        : &scan.source_paths[slot];
        if (!source || source->empty())
            continue;
        ++found;
        if (!gpu_create_dds_view(source->c_str(), &next[slot], why)) {
            for (ID3D11ShaderResourceView*& face : next)
                gpu_release(face);
            return false;
        }
    }
    for (uint32_t slot = 6; slot < ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT && !next_cloud; ++slot) {
        const std::string* source = has_explicit_paths
                                        ? source_for(g.document.skybox.texture_paths[slot])
                                        : &scan.source_paths[slot];
        if (!source || source->empty())
            continue;
        ++found;
        if (!gpu_create_dds_view(source->c_str(), &next_cloud, why)) {
            for (ID3D11ShaderResourceView*& face : next)
                gpu_release(face);
            return false;
        }
    }
    if (!found) {
        if (why)
            *why = "The selected folder contains DDS files, but none match the editable SKYB resource names.";
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

bool gpu_load_pc_skybox(const std::string& pc_path, const SkyboxSettings& settings, std::string* why) {
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
    PcSkyboxInfo source_info{};
    if (ok)
        ok = pc_skybox_info(chunks, &source_info, &err);
    if (ok)
        ok = gpu_rebuild_skybox_vertices(settings.orientation_radians,
                                         settings.back_texture_is_front_upside_down,
                                         settings.right_texture_is_left_upside_down, why);

    ID3D11ShaderResourceView* next_faces[6]{};
    ID3D11ShaderResourceView* next_cloud = nullptr;
    uint32_t loaded = 0;
    for (uint32_t slot = 0; ok && slot < 6; ++slot) {
        const std::string& texture_path = settings.texture_paths[slot];
        const Str texture_name{texture_path.data(), static_cast<uint32_t>(texture_path.size())};
        RscfInfo resource{};
        if (!pc_texture_resource(chunks, texture_name, &resource))
            continue;
        const std::string label(texture_name.data, texture_name.size);
        ok = gpu_create_dds_view_from_memory(resource.payload, resource.payload_size, label.c_str(),
                                             &next_faces[slot], why);
        loaded += ok;
    }
    if (ok && settings.draw_clouds) {
        RscfInfo resource{};
        uint32_t cloud_slot = 6;
        const std::string* texture_path = &settings.texture_paths[cloud_slot];
        Str texture_name{texture_path->data(), static_cast<uint32_t>(texture_path->size())};
        bool have_cloud = pc_texture_resource(chunks, texture_name, &resource);
        if (!have_cloud) {
            cloud_slot = 7;
            texture_path = &settings.texture_paths[cloud_slot];
            texture_name = {texture_path->data(), static_cast<uint32_t>(texture_path->size())};
            have_cloud = pc_texture_resource(chunks, texture_name, &resource);
        }
        if (have_cloud) {
            const std::string label(texture_name.data, texture_name.size);
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
        gpu.skybox_tint = {std::clamp(settings.red / 255.0f, 0.0f, 1.0f),
                           std::clamp(settings.green / 255.0f, 0.0f, 1.0f),
                           std::clamp(settings.blue / 255.0f, 0.0f, 1.0f), 1.0f};
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
    gpu_release_model_textures();
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
    gpu_release(gpu.rasterizer_no_cull);
    gpu_release(gpu.rasterizer_cull_back);
    gpu_release(gpu.camera_buffer);
    gpu_release(gpu.input_layout);
    gpu_release(gpu.rain_pixel_shader);
    gpu_release(gpu.environment_pixel_shader);
    gpu_release(gpu.pixel_shader);
    gpu_release(gpu.vertex_shader);
    gpu_release(gpu.swap_chain);
    gpu_release(gpu.context);
    gpu_release(gpu.device);
    // Reconstruct in place instead of move-assigning a temporary renderer.
    // Every COM pointer above is already null; this only tears down the STL
    // caches and restores the exact default member state without instantiating
    // move-assignment for every container member in Release.
    gpu.~GpuRenderer();
    new (&gpu) GpuRenderer;
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
Texture2D modelTexture : register(t0);
SamplerState modelSampler : register(s0);
float4 PSMain(VSOutput input) : SV_TARGET {
    if (input.color.a > 0.5)
        return input.color;
    float4 albedo = modelTexture.Sample(modelSampler, input.uv);
    if (albedo.a <= 10.0 / 255.0)
        discard;
    float light = 0.28 + 0.72 * abs(dot(normalize(input.normal), normalize(float3(-0.35, 0.8, -0.45))));
    float3 baseColor = dot(abs(input.color.rgb), float3(1.0, 1.0, 1.0)) > 0.001
                           ? input.color.rgb
                           : float3(0.32, 0.39, 0.43);
    return float4(baseColor * albedo.rgb * light, 1.0);
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
             *rain_ps_blob = nullptr;
    result = gpu_compile_shader(shader_source, sizeof(shader_source) - 1, "VSMain", "vs_4_0", &vs_blob);
    if (FAILED(result)) {
        gpu_release(vs_blob);
        gpu_shutdown();
        return false;
    }
    result = gpu_compile_shader(shader_source, sizeof(shader_source) - 1, "PSMain", "ps_4_0", &ps_blob);
    if (SUCCEEDED(result))
        result = gpu_compile_shader(shader_source, sizeof(shader_source) - 1, "EnvPSMain", "ps_4_0",
                                    &environment_ps_blob);
    if (SUCCEEDED(result))
        result = gpu_compile_shader(shader_source, sizeof(shader_source) - 1, "RainPSMain", "ps_4_0",
                                    &rain_ps_blob);
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
    result = gpu_compile_shader(shader_source, sizeof(shader_source) - 1, "SkyVSMain", "vs_4_0", &sky_vs_blob);
    if (SUCCEEDED(result))
        result = gpu_compile_shader(shader_source, sizeof(shader_source) - 1, "SkyPSMain", "ps_4_0", &sky_ps_blob);
    if (SUCCEEDED(result))
        result = gpu_compile_shader(shader_source, sizeof(shader_source) - 1, "SkyCloudPSMain", "ps_4_0",
                                    &sky_cloud_ps_blob);
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
    raster_desc.CullMode = D3D11_CULL_BACK;
    raster_desc.FrontCounterClockwise = FALSE;
    raster_desc.DepthClipEnable = TRUE;
    raster_desc.MultisampleEnable = TRUE;
    if (FAILED(gpu.device->CreateRasterizerState(&raster_desc, &gpu.rasterizer_cull_back))) {
        gpu_shutdown();
        return false;
    }
    raster_desc.CullMode = D3D11_CULL_NONE;
    if (FAILED(gpu.device->CreateRasterizerState(&raster_desc, &gpu.rasterizer_no_cull))) {
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
    gpu_release_model_textures();
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
    std::sort(face_order.begin(), face_order.end(), [](uint32_t a, uint32_t b) {
        const int32_t material_a = a < g.mesh.face_materials.size() ? g.mesh.face_materials[a] : -1;
        const int32_t material_b = b < g.mesh.face_materials.size() ? g.mesh.face_materials[b] : -1;
        return material_a != material_b ? material_a < material_b : a < b;
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

GpuVertex gpu_model_vertex(const SpawnPuppetVertex& source, const Entity& entity,
                           DirectX::XMFLOAT4 color) {
    const Asura_Vector_3 position = spawn_puppet_view_position(source.position, entity);
    const Asura_Vector_3 normal = normalized(spawn_puppet_view_vector(source.normal, entity));
    return {{position.x, position.y, position.z}, {normal.x, normal.y, normal.z}, color,
            {source.texcoord.x, source.texcoord.y}};
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

void gpu_commit_model_lookup(size_t reserve_count,
                             std::vector<GpuModelLookupSnapshot>* cached,
                             std::vector<GpuModelLookupSnapshot>* scratch,
                             std::unordered_map<uint32_t, const SpawnPuppet*>* lookup) {
    if (same_model_lookup_snapshots(*cached, *scratch))
        return;
    lookup->clear();
    lookup->reserve(reserve_count);
    for (const GpuModelLookupSnapshot& entry : *scratch)
        if (entry.face_count)
            lookup->emplace(entry.id, entry.model);
    cached->swap(*scratch);
}

void gpu_refresh_entity_model_lookup() {
    gpu.pickup_lookup_snapshot_scratch.clear();
    gpu.pickup_lookup_snapshot_scratch.reserve(g.pickup_models.size());
    for (const PickupModel& entry : g.pickup_models) {
        const SpawnPuppet* model = &entry.mesh;
        gpu.pickup_lookup_snapshot_scratch.push_back(
            {entry.skin_id, model, model->faces.data(), model->faces.size()});
    }
    gpu_commit_model_lookup(g.pickup_models.size(), &gpu.pickup_lookup_snapshot,
                            &gpu.pickup_lookup_snapshot_scratch, &gpu.pickup_lookup);

    gpu.static_lookup_snapshot_scratch.clear();
    gpu.static_lookup_snapshot_scratch.reserve(g.static_object_models.size());
    for (const StaticObjectModel& entry : g.static_object_models) {
        const SpawnPuppet* model = &entry.mesh;
        gpu.static_lookup_snapshot_scratch.push_back(
            {entry.file_id, model, model->faces.data(), model->faces.size()});
    }
    gpu_commit_model_lookup(g.static_object_models.size(), &gpu.static_lookup_snapshot,
                            &gpu.static_lookup_snapshot_scratch, &gpu.static_lookup);
}

const SpawnPuppet* gpu_entity_render_model(const Entity& entity) {
    if (entity.kind == EntityKind::SpawnPoint)
        return spawn_puppet_for_team(entity.value_u32_a);
    if (entity.kind == EntityKind::Pickup) {
        const auto found = gpu.pickup_lookup.find(entity.pickup_skin_id);
        return found == gpu.pickup_lookup.end() ? nullptr : found->second;
    }
    if (entity.kind == EntityKind::StaticObject) {
        const auto direct = gpu.static_lookup.find(entity.value_u32_b);
        if (direct != gpu.static_lookup.end())
            return direct->second;
        const auto fallback = gpu.pickup_lookup.find(entity.pickup_skin_id);
        return fallback == gpu.pickup_lookup.end() ? nullptr : fallback->second;
    }
    return nullptr;
}

GpuEntitySnapshot gpu_entity_snapshot(const Entity& entity, const SpawnPuppet* model,
                                      uint32_t selection_order) {
    GpuEntitySnapshot snapshot{};
    snapshot.model = model;
    if (model) {
        snapshot.vertices = model->vertices.data();
        snapshot.faces = model->faces.data();
        snapshot.materials = model->materials.data();
        snapshot.face_count = model->faces.size();
    }
    snapshot.kind = entity.kind;
    snapshot.position = entity.position;
    snapshot.rotation = entity.rotation;
    snapshot.spawn_direction = entity.spawn_direction;
    snapshot.source_bounds = entity.source_bounds;
    snapshot.sound_range = entity.value_b;
    snapshot.value_u32_a = entity.value_u32_a;
    snapshot.selection_order = selection_order;
    if (entity.kind == EntityKind::Light) {
        snapshot.light_range = entity.light.Range;
        snapshot.light_flags = entity.light.m_uFlags;
        snapshot.light_bounds = entity.light.m_xBoundingBox;
    }
    return snapshot;
}

bool same_entity_snapshots(const std::vector<GpuEntitySnapshot>& a,
                           const std::vector<GpuEntitySnapshot>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!same_entity_snapshot(a[i], b[i]))
            return false;
    return true;
}

void build_gpu_rain_vertices(const Asura_Vector_3& camera_position,
                             const Asura_Vector_3& right,
                             const Asura_Vector_3& up,
                             const Asura_Vector_3& forward,
                             float vertical_fov, float aspect,
                             float animation_seconds,
                             std::vector<GpuVertex>* vertices) {
    constexpr uint32_t layer_count = 6;
    const float nearest = fmaxf(g.camera.distance * .002f,
                                std::clamp(g.camera.distance * .055f, 1.5f, 12.0f));
    vertices->clear();
    vertices->reserve(layer_count * 6);
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
        vertices->insert(vertices->end(), {a, b, c, a, c, d});
    }
}

bool gpu_render_rain(const Asura_Vector_3& camera_position, const Asura_Vector_3& right,
                     const Asura_Vector_3& up, const Asura_Vector_3& forward,
                     float vertical_fov, float aspect, float animation_seconds) {
    gpu.rain_vertex_count = 0;
    gpu.rain_frame_drawn = false;
    if (!g.document.rain_enabled || !gpu.rain_texture || !gpu.rain_pixel_shader)
        return false;
    std::vector<GpuVertex>& vertices = gpu.rain_scratch;
    build_gpu_rain_vertices(camera_position, right, up, forward,
                            vertical_fov, aspect, animation_seconds, &vertices);
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
        gpu.context->PSSetShaderResources(0, 1, &texture);
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

void append_gpu_spawn_puppet(const Entity& entity, const SpawnPuppet* puppet, bool selected,
                             std::vector<GpuVertex>* output,
                             std::vector<GpuPuppetRange>* ranges = nullptr) {
    if (!puppet)
        return;

    const uint32_t start_vertex = static_cast<uint32_t>(output->size());

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
        for (uint16_t index : face)
            output->push_back(gpu_model_vertex(puppet->vertices[index], entity, color));
    }
    if (ranges && output->size() > start_vertex)
        ranges->push_back(
            {start_vertex, static_cast<uint32_t>(output->size()) - start_vertex, nullptr, selected, true});
}

void append_gpu_entity_model(const Entity& entity, const SpawnPuppet* model, bool selected,
                             std::vector<GpuVertex>* output,
                             std::vector<GpuPuppetRange>* ranges = nullptr) {
    if (!model)
        return;
    const bool two_sided = entity.kind == EntityKind::Pickup;
    for (uint32_t face_index = 0; face_index < model->faces.size(); ++face_index) {
        const auto& face = model->faces[face_index];
        const int32_t material_index = face_index < model->face_materials.size()
                                           ? model->face_materials[face_index]
                                           : -1;
        const SpawnPuppetMaterial* material = material_index >= 0 &&
                                                       static_cast<uint32_t>(material_index) < model->materials.size()
                                                   ? &model->materials[material_index]
                                                   : nullptr;
        const bool textured = material && !material->texture_bytes.empty();
        DirectX::XMFLOAT4 color = entity.kind == EntityKind::StaticObject
                                      ? DirectX::XMFLOAT4{.12f, .62f, .48f, 0}
                                      : DirectX::XMFLOAT4{.72f, .31f, .08f, 0};
        if (textured)
            color = selected ? DirectX::XMFLOAT4{1.0f, .82f, .58f, 0}
                             : DirectX::XMFLOAT4{1.0f, 1.0f, 1.0f, 0};
        else if (selected)
            color = entity.kind == EntityKind::StaticObject ? DirectX::XMFLOAT4{.42f, 1.0f, .82f, 0}
                                                             : DirectX::XMFLOAT4{1.0f, .68f, .22f, 0};
        const uint32_t start_vertex = static_cast<uint32_t>(output->size());
        for (uint16_t index : face)
            output->push_back(gpu_model_vertex(model->vertices[index], entity, color));
        if (ranges) {
            if (!ranges->empty() && ranges->back().material == material &&
                ranges->back().selected == selected &&
                ranges->back().two_sided == two_sided &&
                ranges->back().start_vertex + ranges->back().vertex_count == start_vertex) {
                ranges->back().vertex_count += 3;
            } else {
                ranges->push_back({start_vertex, 3, material, selected, two_sided});
            }
        }
    }
}

void append_gpu_entity_geometry(const Entity& entity, const SpawnPuppet* model, bool selected,
                                std::vector<GpuVertex>* vertices,
                                std::vector<GpuPuppetRange>* ranges) {
    if (entity.kind == EntityKind::SpawnPoint)
        append_gpu_spawn_puppet(entity, model, selected, vertices, ranges);
    else if (entity.kind == EntityKind::Pickup || entity.kind == EntityKind::StaticObject)
        append_gpu_entity_model(entity, model, selected, vertices, ranges);
}

DirectX::XMFLOAT4 gpu_overlay_entity_color(EntityKind kind, bool selected) {
    if (selected)
        return {1, 1, 1, 1};
    switch (kind) {
    case EntityKind::SpawnPoint: return {.25f, .9f, .45f, 1};
    case EntityKind::Light: return {1, .86f, .2f, 1};
    case EntityKind::Sound: return {.2f, .7f, 1, 1};
    case EntityKind::Pickup: return {1, .45f, .18f, 1};
    case EntityKind::StaticObject: return {.2f, .8f, .65f, 1};
    case EntityKind::AssassinationTarget: return {1, .15f, .25f, 1};
    case EntityKind::PositionMarker: return {.75f, .35f, 1, 1};
    case EntityKind::BuildingVolume: return {.15f, .88f, 1, 1};
    default: return {1, .45f, .25f, 1};
    }
}

void append_gpu_gizmo(std::vector<GpuVertex>* vertices, const std::vector<LightGizmoLine>& lines,
                      DirectX::XMFLOAT4 color) {
    for (const LightGizmoLine& line : lines) {
        vertices->push_back(gpu_line_vertex(line.a, color));
        vertices->push_back(gpu_line_vertex(line.b, color));
    }
}

void append_gpu_marker(std::vector<GpuVertex>* vertices, Asura_Vector_3 position, float size,
                       DirectX::XMFLOAT4 color) {
    const Asura_Vector_3 axes[] = {{size, 0, 0}, {0, size, 0}, {0, 0, size}};
    for (Asura_Vector_3 axis : axes) {
        vertices->push_back(gpu_line_vertex(sub(position, axis), color));
        vertices->push_back(gpu_line_vertex(add(position, axis), color));
    }
}

void append_box_edges(const Asura_Vector_3 (&corners)[8], std::vector<LightGizmoLine>* lines) {
    for (int corner = 0; corner < 8; ++corner)
        for (int axis = 0; axis < 3; ++axis) {
            const int other = corner ^ (1 << axis);
            if (corner < other)
                append_light_gizmo_line(lines, corners[corner], corners[other]);
        }
}

void append_view_bounding_box(const Asura_Bounding_Box& bounds, std::vector<LightGizmoLine>* lines) {
    const float values[] = {bounds.MinX, bounds.MaxX, bounds.MinY,
                            bounds.MaxY, bounds.MinZ, bounds.MaxZ};
    for (float value : values)
        if (!isfinite(value))
            return;
    Asura_Vector_3 corners[8]{};
    for (int corner = 0; corner < 8; ++corner) {
        corners[corner] = {corner & 1 ? bounds.MaxX : bounds.MinX,
                           corner & 2 ? bounds.MaxY : bounds.MinY,
                           corner & 4 ? bounds.MaxZ : bounds.MinZ};
    }
    append_box_edges(corners, lines);
}

void append_oriented_bounds_gizmo(const Entity& entity, std::vector<LightGizmoLine>* lines) {
    const Asura_Bounding_Box& bounds = entity.source_bounds;
    const float values[] = {bounds.MinX, bounds.MaxX, bounds.MinY, bounds.MaxY,
                            bounds.MinZ, bounds.MaxZ, entity.position.x, entity.position.y,
                            entity.position.z, entity.rotation.x, entity.rotation.y,
                            entity.rotation.z};
    for (float value : values)
        if (!isfinite(value))
            return;
    if (bounds.MinX > bounds.MaxX || bounds.MinY > bounds.MaxY || bounds.MinZ > bounds.MaxZ)
        return;

    const Asura_Vector_3 source_center{(bounds.MinX + bounds.MaxX) * .5f,
                                       (bounds.MinY + bounds.MaxY) * .5f,
                                       (bounds.MinZ + bounds.MaxZ) * .5f};
    const Asura_Quat orientation = euler_quaternion(entity.rotation);
    Asura_Vector_3 corners[8]{};
    for (int corner = 0; corner < 8; ++corner) {
        const Asura_Vector_3 source_corner{corner & 1 ? bounds.MaxX : bounds.MinX,
                                           corner & 2 ? bounds.MaxY : bounds.MinY,
                                           corner & 4 ? bounds.MaxZ : bounds.MinZ};
        const Asura_Vector_3 local = sub(source_corner, source_center);
        corners[corner] = entity_view_position(
            add(entity.position, rotate_by_quaternion(local, orientation)));
    }
    append_box_edges(corners, lines);
}

void gpu_draw_puppet_ranges(const std::vector<GpuPuppetRange>& ranges, bool selected) {
    gpu.context->PSSetSamplers(0, 1, &gpu.environment_sampler);
    ID3D11RasterizerState* bound_rasterizer = nullptr;
    ID3D11ShaderResourceView* bound_texture = nullptr;
    bool rasterizer_bound = false;
    bool texture_bound = false;
    for (const GpuPuppetRange& range : ranges) {
        if (range.selected != selected || !range.vertex_count)
            continue;
        ID3D11RasterizerState* rasterizer = range.two_sided || !g.backface_culling
                                               ? gpu.rasterizer_no_cull
                                               : gpu.rasterizer_cull_back;
        if (!rasterizer_bound || rasterizer != bound_rasterizer) {
            gpu.context->RSSetState(rasterizer);
            bound_rasterizer = rasterizer;
            rasterizer_bound = true;
        }
        ID3D11ShaderResourceView* texture = gpu_model_texture_view(range.material);
        if (!texture_bound || texture != bound_texture) {
            gpu.context->PSSetShaderResources(0, 1, &texture);
            bound_texture = texture;
            texture_bound = true;
        }
        gpu.context->Draw(range.vertex_count, range.start_vertex);
    }
    ID3D11ShaderResourceView* none = nullptr;
    gpu.context->PSSetShaderResources(0, 1, &none);
}

void gpu_draw_overlay_range(uint32_t start_vertex, uint32_t vertex_count,
                            ID3D11DepthStencilState* depth_state) {
    if (!vertex_count)
        return;
    const UINT stride = sizeof(GpuVertex), offset = 0;
    gpu.context->OMSetDepthStencilState(depth_state, 0);
    gpu.context->IASetVertexBuffers(0, 1, &gpu.overlay_vertices, &stride, &offset);
    gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    gpu.context->Draw(vertex_count, start_vertex);
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
    gpu.context->RSSetState(gpu.rasterizer_no_cull);
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

    ID3D11RasterizerState* scene_rasterizer =
        g.backface_culling ? gpu.rasterizer_cull_back : gpu.rasterizer_no_cull;
    gpu.context->RSSetState(scene_rasterizer);

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
        gpu.context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        for (const GpuMaterialRange& range : gpu.material_ranges) {
            if (gpu_material_uses_alpha(range))
                continue;
            ID3D11ShaderResourceView* texture = range.texture ? range.texture : gpu.white_texture;
            const EnvironmentMaterialConstants material_constants =
                environment_material_constants(range.fallback_color, range.texture != nullptr,
                                               range.has_material_color, 0.0f, false, false);
            gpu.context->UpdateSubresource(gpu.environment_material_buffer, 0, nullptr, &material_constants, 0, 0);
            gpu.context->PSSetShaderResources(2, 1, &texture);
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
    gpu.context->RSSetState(gpu.rasterizer_no_cull);
    gpu_render_rain(camera_position, right, up, forward, fov_y,
                    static_cast<float>(width) / height, rain_animation_time);
    gpu.context->RSSetState(scene_rasterizer);
    gpu.context->PSSetShader(gpu.pixel_shader, nullptr, 0);

    // Entity geometry is camera-independent. Resolve models and selection once,
    // then only rebuild the transformed CPU/GPU buffers when entity-visible
    // state changes. Large retail levels otherwise transform and upload every
    // ObjectHierarchy triangle again for every orbit/pan redraw.
    gpu_refresh_entity_model_lookup();
    const size_t entity_count = g.document.entities.size();
    std::vector<uint32_t>& selection_order = gpu.entity_selection_order_scratch;
    selection_order.assign(entity_count, 0);
    for (size_t order = 0; order < g.selected_entities.size(); ++order) {
        const int selected_index = g.selected_entities[order];
        if (valid_entity_index(selected_index))
            selection_order[static_cast<size_t>(selected_index)] = static_cast<uint32_t>(order + 1);
    }

    std::vector<const SpawnPuppet*>& entity_models = gpu.entity_models_scratch;
    entity_models.resize(entity_count);
    for (size_t i = 0; i < entity_count; ++i)
        entity_models[i] = gpu_entity_render_model(g.document.entities[i]);

    std::vector<GpuEntitySnapshot>& next_snapshot = gpu.entity_snapshot_scratch;
    next_snapshot.resize(entity_count);
    for (size_t i = 0; i < entity_count; ++i)
        next_snapshot[i] = gpu_entity_snapshot(g.document.entities[i], entity_models[i], selection_order[i]);

    if (gpu.cached_overlay_mesh_radius != g.mesh.radius ||
        !same_entity_snapshots(gpu.entity_snapshot, next_snapshot)) {
        std::vector<GpuVertex>& puppet_vertices = gpu.puppet_scratch;
        std::vector<GpuPuppetRange>& puppet_ranges = gpu.puppet_range_scratch;
        puppet_vertices.clear();
        puppet_ranges.clear();
        size_t puppet_vertex_count = 0;
        for (const GpuEntitySnapshot& snapshot : next_snapshot)
            if (snapshot.model && snapshot.face_count <= (SIZE_MAX - puppet_vertex_count) / 3)
                puppet_vertex_count += snapshot.face_count * 3;
        puppet_vertices.reserve(puppet_vertex_count);
        for (size_t i = 0; i < entity_count; ++i)
            if (!selection_order[i] && entity_models[i])
                append_gpu_entity_geometry(g.document.entities[i], entity_models[i], false,
                                           &puppet_vertices, &puppet_ranges);
        gpu.cached_unselected_puppet_count = static_cast<uint32_t>(puppet_vertices.size());
        for (int selected_index : g.selected_entities)
            if (valid_entity_index(selected_index) && entity_models[static_cast<size_t>(selected_index)])
                append_gpu_entity_geometry(g.document.entities[selected_index],
                                           entity_models[static_cast<size_t>(selected_index)], true,
                                           &puppet_vertices, &puppet_ranges);
        gpu.cached_selected_puppet_count =
            static_cast<uint32_t>(puppet_vertices.size()) - gpu.cached_unselected_puppet_count;
        gpu.cached_puppets_ready = !puppet_vertices.empty() &&
                                   gpu_update_dynamic_vertices(&gpu.puppet_vertices, &gpu.puppet_capacity,
                                                               puppet_vertices) &&
                                   gpu.puppet_vertices;

        std::vector<GpuVertex>& overlay = gpu.overlay_scratch;
        overlay.clear();
        const float extent = fmaxf(50.0f, g.mesh.radius * 1.5f);
        const float step = fmaxf(1.0f, powf(10.0f, floorf(log10f(extent / 10.0f))));
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
                        entity_count * (6 + kCameraSpawnArrowLines * 2 + kLightBoundingBoxLines * 2) +
                        gizmo_line_capacity * 2);
        for (int i = -lines; i <= lines; ++i) {
            const auto color = i == 0 ? major : minor;
            overlay.push_back(gpu_line_vertex({i * step, 0, -extent}, color));
            overlay.push_back(gpu_line_vertex({i * step, 0, extent}, color));
            overlay.push_back(gpu_line_vertex({-extent, 0, i * step}, color));
            overlay.push_back(gpu_line_vertex({extent, 0, i * step}, color));
        }
        gpu.cached_entity_start = static_cast<uint32_t>(overlay.size());
        gpu.cached_selected_entity_start = gpu.cached_entity_start;
        const float marker = fmaxf(.35f, g.mesh.radius * .008f);
        std::vector<LightGizmoLine> entity_gizmo;
        entity_gizmo.reserve(kMaximumLightGizmoLines);
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0; i < entity_count; ++i) {
                const bool selected = selection_order[i] != 0;
                if (selected != (pass == 1))
                    continue;
                const Entity& entity = g.document.entities[i];
                const DirectX::XMFLOAT4 color = gpu_overlay_entity_color(entity.kind, selected);
                if (entity.kind == EntityKind::BuildingVolume) {
                    entity_gizmo.clear();
                    append_oriented_bounds_gizmo(entity, &entity_gizmo);
                    append_gpu_gizmo(&overlay, entity_gizmo, color);
                    continue;
                }
                if (entity.kind == EntityKind::SpawnPoint && (entity.value_u32_a & SnipeSpawnTeam_Camera)) {
                    entity_gizmo.clear();
                    append_camera_spawn_arrow(entity, selected ? marker * 1.6f : marker, &entity_gizmo);
                    append_gpu_gizmo(&overlay, entity_gizmo, color);
                }
                if (selected && (entity.kind == EntityKind::Light || entity.kind == EntityKind::Sound)) {
                    entity_gizmo.clear();
                    if (entity.kind == EntityKind::Light)
                        append_light_gizmo(entity, &entity_gizmo);
                    else
                        append_sound_gizmo(entity, &entity_gizmo);
                    append_gpu_gizmo(&overlay, entity_gizmo,
                                     entity.kind == EntityKind::Light
                                         ? DirectX::XMFLOAT4{1, .92f, .35f, 1}
                                         : DirectX::XMFLOAT4{.25f, .8f, 1, 1});
                }
                if (!entity_models[i])
                    append_gpu_marker(&overlay, entity_view_position(entity.position),
                                      selected ? marker * 1.6f : marker, color);
            }
            if (pass == 0)
                gpu.cached_selected_entity_start = static_cast<uint32_t>(overlay.size());
        }
        gpu.cached_overlay_vertex_count = static_cast<uint32_t>(overlay.size());
        gpu.cached_overlay_ready = gpu_update_overlay(overlay) && gpu.overlay_vertices;
        gpu.cached_overlay_mesh_radius = g.mesh.radius;
        gpu.entity_snapshot.swap(next_snapshot);
    }
    std::vector<GpuPuppetRange>& puppet_ranges = gpu.puppet_range_scratch;
    const uint32_t unselected_puppet_count = gpu.cached_unselected_puppet_count;
    const uint32_t selected_puppet_count = gpu.cached_selected_puppet_count;
    const bool puppets_ready = gpu.cached_puppets_ready;
    const uint32_t entity_start = gpu.cached_entity_start;
    const uint32_t selected_entity_start = gpu.cached_selected_entity_start;
    const bool overlay_ready = gpu.cached_overlay_ready;
    if (overlay_ready)
        gpu_draw_overlay_range(0, entity_start, gpu.depth_enabled);
    // Unselected models and markers retain the environment depth buffer and
    // therefore disappear naturally behind walls and terrain.
    if (puppets_ready && unselected_puppet_count) {
        gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
        gpu.context->IASetVertexBuffers(0, 1, &gpu.puppet_vertices, &stride, &offset);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu_draw_puppet_ranges(puppet_ranges, false);
    }
    if (overlay_ready)
        gpu_draw_overlay_range(entity_start, selected_entity_start - entity_start, gpu.depth_enabled);
    // Only the selected model discards scene depth. Drawing with depth enabled
    // after the clear preserves the model's own self-occlusion.
    if (puppets_ready && selected_puppet_count) {
        gpu.context->ClearDepthStencilView(gpu.depth_view, D3D11_CLEAR_DEPTH, 1, 0);
        gpu.context->OMSetDepthStencilState(gpu.depth_enabled, 0);
        gpu.context->IASetVertexBuffers(0, 1, &gpu.puppet_vertices, &stride, &offset);
        gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gpu_draw_puppet_ranges(puppet_ranges, true);
    }
    if (overlay_ready)
        gpu_draw_overlay_range(selected_entity_start,
                               gpu.cached_overlay_vertex_count - selected_entity_start,
                               gpu.depth_disabled);
    gpu.swap_chain->Present(0, 0);
}

bool gpu_ready() { return gpu.ready; }
bool gpu_has_skybox_cloud() { return gpu.skybox_cloud != nullptr; }
bool gpu_has_rain_texture() { return gpu.rain_texture != nullptr; }

void gpu_set_skybox_tint(const SkyboxSettings& skybox) {
    gpu.skybox_tint = {std::clamp(skybox.red / 255.0f, 0.0f, 1.0f),
                       std::clamp(skybox.green / 255.0f, 0.0f, 1.0f),
                       std::clamp(skybox.blue / 255.0f, 0.0f, 1.0f), 1.0f};
}

void gpu_set_environment_wet_weather(bool enabled) {
    gpu.environment_wet_weather = enabled;
}

} // namespace editor
