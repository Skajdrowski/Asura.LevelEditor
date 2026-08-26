#pragma once

#include "LevelEditorDocument.h"

#include <cstdint>
#include <vector>

namespace editor {

struct EnvironmentRay {
    Asura_Vector_3 origin{};
    Asura_Vector_3 direction{};
};

struct EnvironmentRayHit {
    Asura_Vector_3 position{};
    Asura_Vector_3 normal{};
    float distance = 0.0f;
    uint32_t face_index = 0;
};

// A compact, immutable-after-build BVH used by viewport placement and dragging.
// The environment is treated as two-sided because authored levels commonly mix
// winding conventions, and editor placement should still find the visible face.
class EnvironmentRaycast {
public:
    void clear();
    void build(const Mesh& mesh);
    bool intersect(const Mesh& mesh, const EnvironmentRay& ray, EnvironmentRayHit* hit) const;
    bool empty() const { return nodes_.empty(); }

    struct Bounds {
        Asura_Vector_3 min{};
        Asura_Vector_3 max{};
    };

    struct Node {
        Bounds bounds{};
        uint32_t first = 0;
        uint32_t count = 0;
        uint32_t left = 0;
        uint32_t right = 0;
    };

private:
    uint32_t build_node(const Mesh& mesh, uint32_t first, uint32_t count);

    std::vector<uint32_t> faces_;
    std::vector<Node> nodes_;
};

} // namespace editor
