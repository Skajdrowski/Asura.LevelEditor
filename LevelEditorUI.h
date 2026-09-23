#pragma once

#include "LevelEditorAppState.h"

#include <windows.h>

#include <string>
#include <vector>

namespace editor {

enum ControlId : int {
    ID_OPEN_OBJ = 100,
    ID_OPEN_PC,
    ID_OPEN_PROJECT,
    ID_SAVE_PROJECT,
    ID_EXPORT_PC,
    ID_EXPORT_OBJ,
    ID_MATERIAL_MAP,
    ID_EXPORT_MATERIAL_MAP,
    ID_TEXTURE_DIR,
    ID_WEAPONS_DONOR,
    ID_OBJECT_DONOR,
    ID_SKYBOX_TEXTURES,
    ID_TOGGLE_RAIN,
    ID_TOGGLE_BACKFACE_CULLING,
    ID_AMBIENCE_PROPERTIES,
    ID_ENTITY_LIST,
    ID_ADD_SPAWN,
    ID_ADD_LIGHT,
    ID_ADD_SOUND,
    ID_ADD_PICKUP,
    ID_ADD_STATIC_OBJECT,
    ID_IMPORT_STATIC_OBJECT,
    ID_OBJECT_PROPERTIES,
    ID_ADD_BUILDING_VOLUME,
    ID_ADD_SOUND_REGION,
    ID_ADD_COLLISION_BARRIER,
    ID_DELETE_ENTITY,
    ID_UNDO,
    ID_REDO,
    ID_COPY_ENTITY,
    ID_PASTE_ENTITY,
    ID_APPLY_INSPECTOR,
    ID_BROWSE_SOUND,
    ID_NAME,
    ID_POS_X,
    ID_POS_Y,
    ID_POS_Z,
    ID_ROT_X,
    ID_ROT_Y,
    ID_ROT_Z,
    ID_VALUE_A,
    ID_VALUE_B,
    ID_VALUE_C,
    ID_PICKUP_ITEM,
    ID_LIGHT_PROPERTIES,
    ID_SOUND_LOOP,
    ID_SOUND_PREVIEW,
    ID_SOUND_PROPERTIES,
    ID_SPAWN_TEAM_FIRST,
    ID_SPAWN_TEAM_LAST = ID_SPAWN_TEAM_FIRST + 3,
    ID_SPAWN_GAME_MODE_FIRST,
    ID_SPAWN_GAME_MODE_LAST = ID_SPAWN_GAME_MODE_FIRST + 5,
    ID_STATUS,
    ID_VIEWPORT,
};

bool load_spawn_puppets();
LRESULT CALLBACK viewport_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK light_properties_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK sound_properties_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK skybox_properties_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK ambience_properties_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK object_properties_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

bool open_obj_path(const std::string& path);
bool open_pc_path(const std::string& path);
bool load_document_preview(Document* document, Mesh* mesh, std::string* why,
                           std::vector<PickupModel>* pickup_models,
                           std::vector<StaticObjectModel>* object_models);
bool reload_skybox_preview(bool show_warning);
bool focused_edit_owns_clipboard_shortcut(const MSG& message);

void set_single_selection_state(int index);
void frame_mesh();
void reset_history(bool mark_as_saved);
void refresh_list();
void refresh_inspector();
void update_title();
void set_status(const char* text);

} // namespace editor
