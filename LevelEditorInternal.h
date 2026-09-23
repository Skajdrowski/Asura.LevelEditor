#pragma once

#include "LevelEditorAppState.h"
#include "LevelEditorExport.h"
#include "LevelEditorGeometry.h"
#include "LevelEditorHistory.h"
#include "LevelEditorImport.h"
#include "LevelEditorProject.h"
#include "LevelEditorRaycast.h"
#include "LevelEditorRendering.h"
#include "LevelEditorUI.h"

#include <commdlg.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <fstream>
#include <string>
#include <vector>

namespace editor {

inline DWORD executable_folder(wchar_t* folder, DWORD capacity) {
    if (!folder || !capacity)
        return 0;
    const DWORD length = GetModuleFileNameW(nullptr, folder, capacity);
    if (!length || length >= capacity) {
        folder[0] = 0;
        return 0;
    }
    wchar_t* slash = nullptr;
    for (wchar_t* at = folder; *at; ++at)
        if (*at == L'\\' || *at == L'/')
            slash = at;
    if (!slash) {
        folder[0] = 0;
        return 0;
    }
    *slash = 0;
    return static_cast<DWORD>(slash - folder);
}

} // namespace editor
