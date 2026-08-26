#pragma once

#include "LevelEditorDocument.h"

#include <string>

namespace editor {

Asura_Light legacy_editor_light(const Entity& entity);
bool save_project(const Document& document, const char* path, std::string* why);
bool load_project(Document* document, const char* path, std::string* why);

} // namespace editor
