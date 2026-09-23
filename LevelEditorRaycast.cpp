#include "LevelEditorRaycast.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace editor {
namespace {

Asura_Vector_3 subtract(Asura_Vector_3 a, Asura_Vector_3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Asura_Vector_3 cross_product(Asura_Vector_3 a, Asura_Vector_3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot_product(Asura_Vector_3 a, Asura_Vector_3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Asura_Vector_3 normalize(Asura_Vector_3 value) {
    const float length_squared = dot_product(value, value);
    if (length_squared <= 1.0e-20f)
        return {};
    const float inverse_length = 1.0f / sqrtf(length_squared);
    return {value.x * inverse_length, value.y * inverse_length, value.z * inverse_length};
}

float component(Asura_Vector_3 value, uint32_t axis) {
    return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
}

struct PreparedRay {
    EnvironmentRay ray{};
    Asura_Vector_3 inverse_direction{};
    bool parallel[3]{};
};

PreparedRay prepare_ray(const EnvironmentRay& ray) {
    PreparedRay prepared{};
    prepared.ray = ray;
    const float directions[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
    float* inverses[3] = {&prepared.inverse_direction.x, &prepared.inverse_direction.y,
                          &prepared.inverse_direction.z};
    for (uint32_t axis = 0; axis < 3; ++axis) {
        prepared.parallel[axis] = fabsf(directions[axis]) <= 1.0e-12f;
        *inverses[axis] = prepared.parallel[axis] ? 0.0f : 1.0f / directions[axis];
    }
    return prepared;
}

bool ray_bounds(const EnvironmentRaycast::Bounds& bounds, const PreparedRay& ray,
                float maximum_distance, float* entry_distance) {
    float near_distance = 0.0f;
    float far_distance = maximum_distance;
    for (uint32_t axis = 0; axis < 3; ++axis) {
        const float origin = component(ray.ray.origin, axis);
        const float minimum = component(bounds.min, axis);
        const float maximum = component(bounds.max, axis);
        if (ray.parallel[axis]) {
            if (origin < minimum || origin > maximum)
                return false;
            continue;
        }
        const float inverse_direction = component(ray.inverse_direction, axis);
        float first = (minimum - origin) * inverse_direction;
        float second = (maximum - origin) * inverse_direction;
        if (first > second)
            std::swap(first, second);
        near_distance = fmaxf(near_distance, first);
        far_distance = fminf(far_distance, second);
        if (near_distance > far_distance)
            return false;
    }
    if (far_distance < 0.0f)
        return false;
    if (entry_distance)
        *entry_distance = near_distance;
    return true;
}

bool ray_triangle(const EnvironmentRaycast::Triangle& triangle, const EnvironmentRay& ray,
                  float maximum_distance, float* distance) {
    const Asura_Vector_3 p = cross_product(ray.direction, triangle.edge_b);
    const float determinant = dot_product(triangle.edge_a, p);
    if (fabsf(determinant) <= 1.0e-9f)
        return false;
    const float inverse_determinant = 1.0f / determinant;
    const Asura_Vector_3 from_a = subtract(ray.origin, triangle.origin);
    const float u = dot_product(from_a, p) * inverse_determinant;
    if (u < -1.0e-6f || u > 1.000001f)
        return false;
    const Asura_Vector_3 q = cross_product(from_a, triangle.edge_a);
    const float v = dot_product(ray.direction, q) * inverse_determinant;
    if (v < -1.0e-6f || u + v > 1.000001f)
        return false;
    const float hit_distance = dot_product(triangle.edge_b, q) * inverse_determinant;
    if (hit_distance <= 1.0e-5f || hit_distance >= maximum_distance)
        return false;
    *distance = hit_distance;
    return true;
}

} // namespace

void EnvironmentRaycast::clear() {
    faces_.clear();
    triangles_.clear();
    nodes_.clear();
}

void EnvironmentRaycast::build(const Mesh& mesh) {
    clear();
    faces_.reserve(mesh.faces.size());
    triangles_.reserve(mesh.faces.size());
    std::vector<Asura_Vector_3> centroids;
    centroids.reserve(mesh.faces.size());
    std::vector<Bounds> triangle_bounds;
    triangle_bounds.reserve(mesh.faces.size());
    for (uint32_t face_index = 0; face_index < mesh.faces.size(); ++face_index) {
        const auto& face = mesh.faces[face_index];
        if (face[0] >= mesh.positions.size() || face[1] >= mesh.positions.size() ||
            face[2] >= mesh.positions.size())
            continue;
        const Asura_Vector_3 a = mesh.positions[face[0]];
        const Asura_Vector_3 b = mesh.positions[face[1]];
        const Asura_Vector_3 c = mesh.positions[face[2]];
        const Asura_Vector_3 edge_a = subtract(b, a);
        const Asura_Vector_3 edge_b = subtract(c, a);
        faces_.push_back(static_cast<uint32_t>(triangles_.size()));
        triangles_.push_back({a, edge_a, edge_b, face_index});
        centroids.push_back({a.x + (edge_a.x + edge_b.x) / 3.0f,
                             a.y + (edge_a.y + edge_b.y) / 3.0f,
                             a.z + (edge_a.z + edge_b.z) / 3.0f});
        Bounds bounds{
            {fminf(a.x, fminf(b.x, c.x)),
             fminf(a.y, fminf(b.y, c.y)),
             fminf(a.z, fminf(b.z, c.z))},
            {fmaxf(a.x, fmaxf(b.x, c.x)),
             fmaxf(a.y, fmaxf(b.y, c.y)),
             fmaxf(a.z, fmaxf(b.z, c.z))},
        };
        // ray_triangle intentionally accepts a 1e-6 barycentric edge tolerance.
        // Keep the broad-phase bounds at least as conservative, otherwise a ray
        // accepted just outside an edge/vertex can be incorrectly culled here.
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const float extent = component(bounds.max, axis) - component(bounds.min, axis);
            const float padding = fmaxf(1.0e-6f, extent * 2.0e-6f);
            if (axis == 0) { bounds.min.x -= padding; bounds.max.x += padding; }
            else if (axis == 1) { bounds.min.y -= padding; bounds.max.y += padding; }
            else { bounds.min.z -= padding; bounds.max.z += padding; }
        }
        triangle_bounds.push_back(bounds);
    }
    if (faces_.empty())
        return;
    // Every split child has at least six faces with leaf_size=12, so a full
    // binary tree needs at most 2*ceil(N/6)-1 nodes. Avoid reserving two nodes
    // per triangle for large retail environments.
    const size_t maximum_leaves = std::max<size_t>(1, (faces_.size() + 5) / 6);
    nodes_.reserve(maximum_leaves * 2 - 1);
    build_node(0, static_cast<uint32_t>(faces_.size()), centroids.data(), triangle_bounds.data());
}

uint32_t EnvironmentRaycast::build_node(uint32_t first, uint32_t count,
                                        const Asura_Vector_3* centroids,
                                        const Bounds* triangle_bounds) {
    constexpr uint32_t leaf_size = 12;
    const float infinity = std::numeric_limits<float>::infinity();
    Bounds bounds{{infinity, infinity, infinity}, {-infinity, -infinity, -infinity}};
    Asura_Vector_3 centroid_min{infinity, infinity, infinity};
    Asura_Vector_3 centroid_max{-infinity, -infinity, -infinity};
    for (uint32_t i = first; i < first + count; ++i) {
        const Bounds& triangle = triangle_bounds[faces_[i]];
        bounds.min.x = fminf(bounds.min.x, triangle.min.x);
        bounds.min.y = fminf(bounds.min.y, triangle.min.y);
        bounds.min.z = fminf(bounds.min.z, triangle.min.z);
        bounds.max.x = fmaxf(bounds.max.x, triangle.max.x);
        bounds.max.y = fmaxf(bounds.max.y, triangle.max.y);
        bounds.max.z = fmaxf(bounds.max.z, triangle.max.z);
        const Asura_Vector_3 centroid = centroids[faces_[i]];
        centroid_min.x = fminf(centroid_min.x, centroid.x);
        centroid_min.y = fminf(centroid_min.y, centroid.y);
        centroid_min.z = fminf(centroid_min.z, centroid.z);
        centroid_max.x = fmaxf(centroid_max.x, centroid.x);
        centroid_max.y = fmaxf(centroid_max.y, centroid.y);
        centroid_max.z = fmaxf(centroid_max.z, centroid.z);
    }

    const uint32_t node_index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back({bounds, first, count, 0, 0});
    if (count <= leaf_size)
        return node_index;

    const Asura_Vector_3 extent{centroid_max.x - centroid_min.x, centroid_max.y - centroid_min.y,
                                centroid_max.z - centroid_min.z};
    const uint32_t axis = extent.y > extent.x && extent.y >= extent.z ? 1 : extent.z > extent.x ? 2 : 0;
    if (component(extent, axis) <= 1.0e-7f)
        return node_index;

    const uint32_t middle = first + count / 2;
    std::nth_element(faces_.begin() + first, faces_.begin() + middle, faces_.begin() + first + count,
                     [&](uint32_t a, uint32_t b) {
                         return component(centroids[a], axis) < component(centroids[b], axis);
                     });
    const uint32_t left = build_node(first, middle - first, centroids, triangle_bounds);
    const uint32_t right = build_node(middle, first + count - middle, centroids, triangle_bounds);
    nodes_[node_index].count = 0;
    nodes_[node_index].left = left;
    nodes_[node_index].right = right;
    return node_index;
}

bool EnvironmentRaycast::intersect(const EnvironmentRay& ray, EnvironmentRayHit* hit) const {
    if (!hit || nodes_.empty())
        return false;
    float closest = std::numeric_limits<float>::infinity();
    uint32_t closest_triangle = 0;
    bool found = false;

    struct StackEntry {
        uint32_t node = 0;
        float entry_distance = 0.0f;
    };
    // build_node always halves non-leaf ranges, so the depth is bounded by the
    // number of bits in the uint32_t face count. This comfortably avoids a heap
    // allocation on every mouse-move raycast.
    std::array<StackEntry, 64> stack{};
    uint32_t stack_size = 0;
    const PreparedRay prepared = prepare_ray(ray);
    float root_entry = 0.0f;
    if (!ray_bounds(nodes_[0].bounds, prepared, closest, &root_entry))
        return false;
    stack[stack_size++] = {0, root_entry};

    while (stack_size) {
        const StackEntry entry = stack[--stack_size];
        if (entry.entry_distance >= closest)
            continue;
        const Node& node = nodes_[entry.node];
        if (node.count) {
            for (uint32_t i = node.first; i < node.first + node.count; ++i) {
                float distance = 0.0f;
                const uint32_t triangle_index = faces_[i];
                if (ray_triangle(triangles_[triangle_index], ray, closest, &distance)) {
                    found = true;
                    closest = distance;
                    closest_triangle = triangle_index;
                }
            }
        } else {
            float left_entry = 0.0f, right_entry = 0.0f;
            const bool hit_left = ray_bounds(nodes_[node.left].bounds, prepared, closest, &left_entry);
            const bool hit_right = ray_bounds(nodes_[node.right].bounds, prepared, closest, &right_entry);
            if (hit_left && hit_right) {
                const bool left_first = left_entry <= right_entry;
                const StackEntry near_entry = left_first ? StackEntry{node.left, left_entry}
                                                         : StackEntry{node.right, right_entry};
                const StackEntry far_entry = left_first ? StackEntry{node.right, right_entry}
                                                        : StackEntry{node.left, left_entry};
                stack[stack_size++] = far_entry;
                stack[stack_size++] = near_entry;
            } else if (hit_left) {
                stack[stack_size++] = {node.left, left_entry};
            } else if (hit_right) {
                stack[stack_size++] = {node.right, right_entry};
            }
        }
    }
    if (!found)
        return false;
    const Triangle& triangle = triangles_[closest_triangle];
    hit->distance = closest;
    hit->face_index = triangle.face_index;
    hit->normal = normalize(cross_product(triangle.edge_a, triangle.edge_b));
    hit->position = {ray.origin.x + ray.direction.x * closest, ray.origin.y + ray.direction.y * closest,
                     ray.origin.z + ray.direction.z * closest};
    return true;
}

} // namespace editor
