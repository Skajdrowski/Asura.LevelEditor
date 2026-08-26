#include "LevelEditorRaycast.h"

#include <algorithm>
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

Asura_Vector_3 triangle_centroid(const Mesh& mesh, uint32_t face_index) {
    const auto& face = mesh.faces[face_index];
    const Asura_Vector_3 a = mesh.positions[face[0]];
    const Asura_Vector_3 b = mesh.positions[face[1]];
    const Asura_Vector_3 c = mesh.positions[face[2]];
    return {(a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f};
}

bool ray_bounds(const EnvironmentRaycast::Bounds& bounds, const EnvironmentRay& ray, float maximum_distance) {
    float near_distance = 0.0f;
    float far_distance = maximum_distance;
    for (uint32_t axis = 0; axis < 3; ++axis) {
        const float origin = component(ray.origin, axis);
        const float direction = component(ray.direction, axis);
        const float minimum = component(bounds.min, axis);
        const float maximum = component(bounds.max, axis);
        if (fabsf(direction) <= 1.0e-12f) {
            if (origin < minimum || origin > maximum)
                return false;
            continue;
        }
        float first = (minimum - origin) / direction;
        float second = (maximum - origin) / direction;
        if (first > second)
            std::swap(first, second);
        near_distance = fmaxf(near_distance, first);
        far_distance = fminf(far_distance, second);
        if (near_distance > far_distance)
            return false;
    }
    return far_distance >= 0.0f;
}

bool ray_triangle(const Mesh& mesh, uint32_t face_index, const EnvironmentRay& ray, float maximum_distance,
                  float* distance, Asura_Vector_3* normal) {
    const auto& face = mesh.faces[face_index];
    const Asura_Vector_3 a = mesh.positions[face[0]];
    const Asura_Vector_3 b = mesh.positions[face[1]];
    const Asura_Vector_3 c = mesh.positions[face[2]];
    const Asura_Vector_3 ab = subtract(b, a);
    const Asura_Vector_3 ac = subtract(c, a);
    const Asura_Vector_3 p = cross_product(ray.direction, ac);
    const float determinant = dot_product(ab, p);
    if (fabsf(determinant) <= 1.0e-9f)
        return false;
    const float inverse_determinant = 1.0f / determinant;
    const Asura_Vector_3 from_a = subtract(ray.origin, a);
    const float u = dot_product(from_a, p) * inverse_determinant;
    if (u < -1.0e-6f || u > 1.000001f)
        return false;
    const Asura_Vector_3 q = cross_product(from_a, ab);
    const float v = dot_product(ray.direction, q) * inverse_determinant;
    if (v < -1.0e-6f || u + v > 1.000001f)
        return false;
    const float hit_distance = dot_product(ac, q) * inverse_determinant;
    if (hit_distance <= 1.0e-5f || hit_distance >= maximum_distance)
        return false;
    *distance = hit_distance;
    *normal = normalize(cross_product(ab, ac));
    return true;
}

} // namespace

void EnvironmentRaycast::clear() {
    faces_.clear();
    nodes_.clear();
}

void EnvironmentRaycast::build(const Mesh& mesh) {
    clear();
    faces_.reserve(mesh.faces.size());
    for (uint32_t face_index = 0; face_index < mesh.faces.size(); ++face_index) {
        const auto& face = mesh.faces[face_index];
        if (face[0] < mesh.positions.size() && face[1] < mesh.positions.size() && face[2] < mesh.positions.size())
            faces_.push_back(face_index);
    }
    if (faces_.empty())
        return;
    nodes_.reserve(faces_.size() * 2);
    build_node(mesh, 0, static_cast<uint32_t>(faces_.size()));
}

uint32_t EnvironmentRaycast::build_node(const Mesh& mesh, uint32_t first, uint32_t count) {
    constexpr uint32_t leaf_size = 12;
    const float infinity = std::numeric_limits<float>::infinity();
    Bounds bounds{{infinity, infinity, infinity}, {-infinity, -infinity, -infinity}};
    Asura_Vector_3 centroid_min{infinity, infinity, infinity};
    Asura_Vector_3 centroid_max{-infinity, -infinity, -infinity};
    for (uint32_t i = first; i < first + count; ++i) {
        const auto& face = mesh.faces[faces_[i]];
        for (uint32_t index : face) {
            const Asura_Vector_3 p = mesh.positions[index];
            bounds.min.x = fminf(bounds.min.x, p.x);
            bounds.min.y = fminf(bounds.min.y, p.y);
            bounds.min.z = fminf(bounds.min.z, p.z);
            bounds.max.x = fmaxf(bounds.max.x, p.x);
            bounds.max.y = fmaxf(bounds.max.y, p.y);
            bounds.max.z = fmaxf(bounds.max.z, p.z);
        }
        const Asura_Vector_3 centroid = triangle_centroid(mesh, faces_[i]);
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
                         return component(triangle_centroid(mesh, a), axis) <
                                component(triangle_centroid(mesh, b), axis);
                     });
    const uint32_t left = build_node(mesh, first, middle - first);
    const uint32_t right = build_node(mesh, middle, first + count - middle);
    nodes_[node_index].count = 0;
    nodes_[node_index].left = left;
    nodes_[node_index].right = right;
    return node_index;
}

bool EnvironmentRaycast::intersect(const Mesh& mesh, const EnvironmentRay& ray, EnvironmentRayHit* hit) const {
    if (!hit || nodes_.empty())
        return false;
    float closest = std::numeric_limits<float>::infinity();
    uint32_t closest_face = 0;
    Asura_Vector_3 closest_normal{};
    bool found = false;
    std::vector<uint32_t> stack;
    stack.reserve(64);
    stack.push_back(0);
    while (!stack.empty()) {
        const Node& node = nodes_[stack.back()];
        stack.pop_back();
        if (!ray_bounds(node.bounds, ray, closest))
            continue;
        if (node.count) {
            for (uint32_t i = node.first; i < node.first + node.count; ++i) {
                float distance = 0.0f;
                Asura_Vector_3 normal{};
                if (ray_triangle(mesh, faces_[i], ray, closest, &distance, &normal)) {
                    found = true;
                    closest = distance;
                    closest_face = faces_[i];
                    closest_normal = normal;
                }
            }
        } else {
            stack.push_back(node.left);
            stack.push_back(node.right);
        }
    }
    if (!found)
        return false;
    hit->distance = closest;
    hit->face_index = closest_face;
    hit->normal = closest_normal;
    hit->position = {ray.origin.x + ray.direction.x * closest, ray.origin.y + ray.direction.y * closest,
                     ray.origin.z + ray.direction.z * closest};
    return true;
}

} // namespace editor
