#pragma once

#include "LevelEditorDocument.h"

namespace editor {

void finish_mesh_bounds(Mesh* mesh);
Asura_Vector_3 rotate_by_quaternion(Asura_Vector_3 value, const Asura_Quat& rotation);

} // namespace editor
