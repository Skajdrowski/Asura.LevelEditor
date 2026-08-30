#include "LevelEditorInternal.h"

using namespace editor;

int APIENTRY WinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPSTR command_line, _In_ int show) {
    if (__argc == 4 && strcmp(__argv[1], "--pack") == 0) {
        Document document;
        std::string why;
        return load_project(&document, __argv[2], &why) &&
                       pack_document(document, __argv[3], &why)
                   ? 0
                   : 2;
    }

    load_spawn_puppets();

    WNDCLASSEXA viewport_class{};
    viewport_class.cbSize = sizeof(viewport_class);
    viewport_class.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    viewport_class.lpfnWndProc = viewport_proc;
    viewport_class.hInstance = instance;
    viewport_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    viewport_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    viewport_class.lpszClassName = "Asura2005Viewport";
    if (!RegisterClassExA(&viewport_class))
        return 1;

    WNDCLASSEXA light_properties_class{};
    light_properties_class.cbSize = sizeof(light_properties_class);
    light_properties_class.lpfnWndProc = light_properties_proc;
    light_properties_class.hInstance = instance;
    light_properties_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    light_properties_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH));
    light_properties_class.lpszClassName = "Asura2005LightProperties";
    if (!RegisterClassExA(&light_properties_class))
        return 1;

    WNDCLASSEXA skybox_properties_class{};
    skybox_properties_class.cbSize = sizeof(skybox_properties_class);
    skybox_properties_class.lpfnWndProc = skybox_properties_proc;
    skybox_properties_class.hInstance = instance;
    skybox_properties_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    skybox_properties_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH));
    skybox_properties_class.lpszClassName = "Asura2005SkyboxProperties";
    if (!RegisterClassExA(&skybox_properties_class))
        return 1;

    WNDCLASSEXA ambience_properties_class{};
    ambience_properties_class.cbSize = sizeof(ambience_properties_class);
    ambience_properties_class.lpfnWndProc = ambience_properties_proc;
    ambience_properties_class.hInstance = instance;
    ambience_properties_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    ambience_properties_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH));
    ambience_properties_class.lpszClassName = "Asura2005AmbienceProperties";
    if (!RegisterClassExA(&ambience_properties_class))
        return 1;

    WNDCLASSEXA window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH));
    window_class.lpszClassName = "Asura2005LevelEditor";
    if (!RegisterClassExA(&window_class))
        return 1;

    HWND window = CreateWindowExA(
        0, window_class.lpszClassName, "Asura 2005 Level Editor",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1400, 720,
        nullptr, nullptr, instance, nullptr);
    if (!window)
        return 1;

    ShowWindow(window, show);
    UpdateWindow(window);

    if (command_line && *command_line) {
        std::string path = command_line;
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
            path = path.substr(1, path.size() - 2);
        const size_t dot = path.find_last_of('.');
        const std::string extension = dot == std::string::npos ? "" : path.substr(dot);
        if (_stricmp(extension.c_str(), ".obj") == 0) {
            open_obj_path(path);
        } else if (_stricmp(extension.c_str(), ".pc") == 0) {
            open_pc_path(path);
        } else if (_stricmp(extension.c_str(), ".alev") == 0) {
            std::string why;
            Document document;
            if (load_project(&document, path.c_str(), &why)) {
                g.document = std::move(document);
                set_single_selection_state(g.document.entities.empty() ? -1 : 0);
                g.pending_kind = -1;
                const bool skybox_loaded = reload_skybox_preview(true);
                load_document_preview(&g.document, &g.mesh, &why, &g.pickup_models,
                                      &g.static_object_models);
                frame_mesh();
                reset_history(!g.document.dirty);
                refresh_list();
                refresh_inspector();
                update_title();
                set_status(skybox_loaded ? "Project loaded."
                                         : "Project loaded; skybox preview is unavailable.");
            }
        }
    }

    const ACCEL accelerator_entries[] = {
        {FVIRTKEY | FCONTROL, 'Z', ID_UNDO},
        {FVIRTKEY | FCONTROL, 'Y', ID_REDO},
        {FVIRTKEY | FCONTROL, 'C', ID_COPY_ENTITY},
        {FVIRTKEY | FCONTROL, 'V', ID_PASTE_ENTITY},
        {FVIRTKEY, VK_DELETE, ID_DELETE_ENTITY},
    };
    HACCEL accelerators = CreateAcceleratorTableA(
        const_cast<LPACCEL>(accelerator_entries), static_cast<int>(_countof(accelerator_entries)));

    MSG message{};
    while (GetMessageA(&message, nullptr, 0, 0) > 0) {
        if (!accelerators || focused_edit_owns_clipboard_shortcut(message) ||
            !TranslateAcceleratorA(window, accelerators, &message)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
    if (accelerators)
        DestroyAcceleratorTable(accelerators);
    return static_cast<int>(message.wParam);
}
