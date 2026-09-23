#include "LevelEditorGeometry.h"

#include <cmath>

namespace editor {

void finish_mesh_bounds(Mesh* mesh) {
    if (mesh->positions.empty()) {
        mesh->min = mesh->max = mesh->center = {};
        mesh->radius = 25.0f;
        return;
    }
    mesh->min = mesh->max = mesh->positions[0];
    for (const Asura_Vector_3& position : mesh->positions) {
        mesh->min.x = fminf(mesh->min.x, position.x);
        mesh->min.y = fminf(mesh->min.y, position.y);
        mesh->min.z = fminf(mesh->min.z, position.z);
        mesh->max.x = fmaxf(mesh->max.x, position.x);
        mesh->max.y = fmaxf(mesh->max.y, position.y);
        mesh->max.z = fmaxf(mesh->max.z, position.z);
    }
    mesh->center = {(mesh->min.x + mesh->max.x) * .5f, (mesh->min.y + mesh->max.y) * .5f,
                    (mesh->min.z + mesh->max.z) * .5f};
    const float dx = mesh->max.x - mesh->min.x;
    const float dy = mesh->max.y - mesh->min.y;
    const float dz = mesh->max.z - mesh->min.z;
    mesh->radius = fmaxf(5.0f, sqrtf(dx * dx + dy * dy + dz * dz) * .5f);
}

Asura_Vector_3 rotate_by_quaternion(Asura_Vector_3 value, const Asura_Quat& rotation) {
    const Asura_Vector_3 q{rotation.x, rotation.y, rotation.z};
    const Asura_Vector_3 twice_cross{2.0f * (q.y * value.z - q.z * value.y),
                                     2.0f * (q.z * value.x - q.x * value.z),
                                     2.0f * (q.x * value.y - q.y * value.x)};
    const Asura_Vector_3 q_cross_twice{q.y * twice_cross.z - q.z * twice_cross.y,
                                       q.z * twice_cross.x - q.x * twice_cross.z,
                                       q.x * twice_cross.y - q.y * twice_cross.x};
    return {value.x + rotation.w * twice_cross.x + q_cross_twice.x,
            value.y + rotation.w * twice_cross.y + q_cross_twice.y,
            value.z + rotation.w * twice_cross.z + q_cross_twice.z};
}

} // namespace editor
