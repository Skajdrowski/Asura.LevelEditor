from pathlib import Path
import json
import re

import bpy
from bpy.app.handlers import persistent
from bpy.props import BoolProperty, CollectionProperty, IntProperty, StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper


UNASSIGNED_INDEX = -1
MAX_BLENDER_INT = 2_147_483_647
_AUTO_REFRESH_RUNNING = False

SURFACE_TYPES = (
    (1, "Concrete"),
    (4, "Metal"),
    (5, "Water"),
    (6, "Wood"),
    (7, "Human body"),
    (8, "?Wrecked car"),
    (10, "Dirt"),
    (11, "Grass"),
    (12, "Gravel"),
    (13, "Wet"),
)

BLENDING_FLAGS = (
    (1, "Additive pass (SRC_ALPHA / ONE)"),
    (2, "Alpha texture"),
    (4, "?Detail mapping via detail.dds"),
    (0x80, "Spheremap1.dds reflection"),
    (0x400, "Scrolling texture"),
    (0x1000, "Light shaft depth"),
    (0x4000, "Affected by rain"),
)

COLLISION_FLAGS = (
    (0x20, "Ignore dynamic objects only"),
    (0x40, "Ignore bullets only"),
    (0x200, "Ignore bullets & grenades only"),
    (0x400, "Include backface"),
)


def image_texture_basename(image):
    """Return the resource-ish file name for a Blender image."""
    if image is None:
        return ""

    filepath = getattr(image, "filepath", "") or ""
    if filepath:
        try:
            return Path(bpy.path.abspath(filepath)).name
        except (OSError, ValueError):
            pass

    return getattr(image, "name", "") or ""


def detected_material_texture_name(material):
    """Find the most likely diffuse/base-color image used by a material."""
    if material is None or not material.use_nodes or material.node_tree is None:
        return ""

    nodes = material.node_tree.nodes

    # Prefer an image connected directly to a Principled BSDF Base Color input.
    for node in nodes:
        if node.type != "BSDF_PRINCIPLED":
            continue
        base_color = node.inputs.get("Base Color")
        if base_color is None:
            continue
        for link in base_color.links:
            source = link.from_node
            if source is not None and source.type == "TEX_IMAGE" and source.image is not None:
                return image_texture_basename(source.image)

    # Fall back to the first image-texture node with an image assigned.
    image_nodes = [
        node for node in nodes if node.type == "TEX_IMAGE" and node.image is not None
    ]
    image_nodes.sort(key=lambda node: (node.name.casefold(), node.name))
    if image_nodes:
        return image_texture_basename(image_nodes[0].image)

    return ""


def scene_used_materials(scene=None):
    """Return unique materials actually used by mesh polygons in a scene."""
    scene = scene or bpy.context.scene
    if scene is None:
        return []

    result = []
    seen = set()
    for obj in scene.objects:
        if obj.type != "MESH" or obj.data is None:
            continue

        slots = obj.material_slots
        used_slot_indices = {polygon.material_index for polygon in obj.data.polygons}
        for slot_index in sorted(used_slot_indices):
            if slot_index < 0 or slot_index >= len(slots):
                continue
            material = slots[slot_index].material
            if material is None:
                continue
            key = material.as_pointer()
            if key in seen:
                continue
            seen.add(key)
            result.append(material)

    return result


def auto_assign_material_textures(only_missing=True, scene=None):
    """Populate Asura texture names for materials actually used in the scene."""
    changed = 0
    for material in scene_used_materials(scene):
        if only_missing and material.asura_texture_name.strip():
            continue
        texture_name = detected_material_texture_name(material)
        if texture_name:
            material.asura_texture_name = texture_name
            changed += 1
        elif not only_missing:
            material.asura_texture_name = ""
    return changed


def material_name_index(name):
    """Return an index embedded in a material name, or None if there is none.

    Imported Asura materials commonly use names such as ``mat_123``.  For
    less strict names, use the last run of decimal digits in the name.
    """
    exact = re.fullmatch(r"\s*mat[_ .-]*(\d+)\s*", name, flags=re.IGNORECASE)
    if exact:
        value = int(exact.group(1))
        return value if value <= MAX_BLENDER_INT else None

    matches = re.findall(r"\d+", name)
    if not matches:
        return None

    value = int(matches[-1])
    return value if value <= MAX_BLENDER_INT else None


def auto_assign_material_indices(only_unassigned=False, scene=None):
    """Assign material indices deterministically.

    Materials with a number in their name keep that number.  Materials with
    no number are assigned the lowest unused indices in case-insensitive
    alphabetical order.  When ``only_unassigned`` is true, existing manual
    assignments are kept and only missing values are filled.
    """
    materials = scene_used_materials(scene)
    targets = [
        material
        for material in materials
        if not only_unassigned
        or (
            int(material.asura_material_index) == UNASSIGNED_INDEX
            and not material.asura_skip_auto_assign
        )
    ]

    used = {
        int(material.asura_material_index)
        for material in materials
        if material not in targets and int(material.asura_material_index) != UNASSIGNED_INDEX
    }

    with_embedded_index = []
    alphabetical = []
    for material in targets:
        embedded = material_name_index(material.name)
        if embedded is None:
            alphabetical.append(material)
        else:
            with_embedded_index.append((material, embedded))

    with_embedded_index.sort(key=lambda item: (item[0].name.casefold(), item[0].name))
    for material, index in with_embedded_index:
        material.asura_material_index = index
        used.add(index)

    alphabetical.sort(key=lambda material: (material.name.casefold(), material.name))
    next_index = 0
    for material in alphabetical:
        while next_index in used:
            next_index += 1
        material.asura_material_index = next_index
        used.add(next_index)
        next_index += 1

    return len(targets)


def material_index_updated(material, _context):
    """Treat an explicit -1 as a request to keep this material unassigned."""
    if _AUTO_REFRESH_RUNNING:
        return
    material.asura_skip_auto_assign = (
        int(material.asura_material_index) == UNASSIGNED_INDEX
    )


def active_material(context):
    """Return the material represented by the current Blender context."""
    material = getattr(context, "material", None)
    if isinstance(material, bpy.types.Material):
        return material

    obj = getattr(context, "object", None)
    if obj is not None:
        material = getattr(obj, "active_material", None)
        if isinstance(material, bpy.types.Material):
            return material

    space = getattr(context, "space_data", None)
    pin_id = getattr(space, "pin_id", None) if space is not None else None
    if isinstance(pin_id, bpy.types.Material):
        return pin_id

    return None


def assigned_materials(scene=None):
    """Return assigned, actually-used scene materials in deterministic order."""
    result = []
    for material in scene_used_materials(scene):
        index = int(material.asura_material_index)
        if index != UNASSIGNED_INDEX:
            result.append((material, index))
    result.sort(key=lambda item: (item[1], item[0].name.casefold(), item[0].name))
    return result


def tag_asura_ui_redraw():
    """Redraw UI regions that can display Asura material properties."""
    window_manager = getattr(bpy.context, "window_manager", None)
    if window_manager is None:
        return
    for window in window_manager.windows:
        screen = window.screen
        if screen is None:
            continue
        for area in screen.areas:
            if area.type in {"VIEW_3D", "PROPERTIES"}:
                area.tag_redraw()


def refresh_scene_assignments(scene=None):
    """Fill missing Asura indices/textures and immediately refresh the GUI."""
    global _AUTO_REFRESH_RUNNING
    if _AUTO_REFRESH_RUNNING:
        return 0, 0

    scene = scene or getattr(bpy.context, "scene", None)
    if scene is None:
        return 0, 0

    _AUTO_REFRESH_RUNNING = True
    try:
        index_count = auto_assign_material_indices(only_unassigned=True, scene=scene)
        texture_count = auto_assign_material_textures(only_missing=True, scene=scene)
    finally:
        _AUTO_REFRESH_RUNNING = False

    if index_count or texture_count:
        tag_asura_ui_redraw()
    return index_count, texture_count


@persistent
def asura_load_post(_dummy):
    """Populate the visible fields immediately after opening a .blend file."""
    refresh_scene_assignments(getattr(bpy.context, "scene", None))


@persistent
def asura_depsgraph_update_post(scene, depsgraph):
    """Keep missing assignments current when relevant scene data changes."""
    if _AUTO_REFRESH_RUNNING:
        return

    updates = tuple(depsgraph.updates)

    # Dense face selection in Edit Mesh mode marks the mesh as updated on
    # every selection change.  Do not rescan every polygon for those events.
    # A full refresh still happens when leaving Edit mode, loading the file,
    # and exporting the material map.
    if getattr(bpy.context, "mode", "") == "EDIT_MESH":
        noisy_types = (bpy.types.Mesh, bpy.types.Object)
        if updates and all(isinstance(update.id, noisy_types) for update in updates):
            return

    relevant_types = (
        bpy.types.Material,
        bpy.types.Mesh,
        bpy.types.Image,
        bpy.types.NodeTree,
    )
    if not any(isinstance(update.id, relevant_types) for update in updates):
        return

    refresh_scene_assignments(scene)


class ASURA_PG_reference_item(bpy.types.PropertyGroup):
    value: IntProperty()
    text: StringProperty()


def _fill_reference_collection(collection, values):
    collection.clear()
    for value, text in values:
        item = collection.add()
        item.value = value
        item.text = text


def ensure_reference_lists(window_manager):
    """Populate the static scrolling-list entries used by the Asura panel."""
    if window_manager is None:
        return
    if len(window_manager.asura_surface_items) != len(SURFACE_TYPES):
        _fill_reference_collection(window_manager.asura_surface_items, SURFACE_TYPES)
    if len(window_manager.asura_blending_items) != len(BLENDING_FLAGS):
        _fill_reference_collection(window_manager.asura_blending_items, BLENDING_FLAGS)
    if len(window_manager.asura_collision_items) != len(COLLISION_FLAGS):
        _fill_reference_collection(window_manager.asura_collision_items, COLLISION_FLAGS)


class ASURA_OT_set_surface_type(bpy.types.Operator):
    bl_idname = "material.asura_set_surface_type"
    bl_label = "Set Asura Surface Type"
    bl_options = {"REGISTER", "UNDO"}

    value: IntProperty()

    @classmethod
    def poll(cls, context):
        return active_material(context) is not None

    def execute(self, context):
        material = active_material(context)
        if material is None:
            return {"CANCELLED"}
        material.asura_surface_type = self.value
        return {"FINISHED"}


class ASURA_OT_toggle_blending_flag(bpy.types.Operator):
    bl_idname = "material.asura_toggle_blending_flag"
    bl_label = "Toggle Asura Blending Flag"
    bl_options = {"REGISTER", "UNDO"}

    flag: IntProperty()

    @classmethod
    def poll(cls, context):
        return active_material(context) is not None

    def execute(self, context):
        material = active_material(context)
        if material is None:
            return {"CANCELLED"}
        material.asura_blending_flags ^= self.flag
        return {"FINISHED"}


class ASURA_OT_toggle_collision_flag(bpy.types.Operator):
    bl_idname = "material.asura_toggle_collision_flag"
    bl_label = "Toggle Asura Collision Flag"
    bl_options = {"REGISTER", "UNDO"}

    flag: IntProperty()

    @classmethod
    def poll(cls, context):
        return active_material(context) is not None

    def execute(self, context):
        material = active_material(context)
        if material is None:
            return {"CANCELLED"}
        material.asura_collision_flags ^= self.flag
        return {"FINISHED"}


class ASURA_UL_surface_types(bpy.types.UIList):
    def draw_item(self, context, layout, _data, item, _icon, _active_data, _active_propname, _index):
        material = active_material(context)
        selected = material is not None and material.asura_surface_type == item.value
        op = layout.operator(
            ASURA_OT_set_surface_type.bl_idname,
            text=f"{item.value}: {item.text}",
            icon="RADIOBUT_ON" if selected else "RADIOBUT_OFF",
            emboss=False,
        )
        op.value = item.value


class ASURA_UL_blending_flags(bpy.types.UIList):
    def draw_item(self, context, layout, _data, item, _icon, _active_data, _active_propname, _index):
        material = active_material(context)
        selected = material is not None and bool(material.asura_blending_flags & item.value)
        op = layout.operator(
            ASURA_OT_toggle_blending_flag.bl_idname,
            text=f"0x{item.value:X} ({item.value}): {item.text}",
            icon="CHECKBOX_HLT" if selected else "CHECKBOX_DEHLT",
            emboss=False,
        )
        op.flag = item.value


class ASURA_UL_collision_flags(bpy.types.UIList):
    def draw_item(self, context, layout, _data, item, _icon, _active_data, _active_propname, _index):
        material = active_material(context)
        selected = material is not None and bool(material.asura_collision_flags & item.value)
        op = layout.operator(
            ASURA_OT_toggle_collision_flag.bl_idname,
            text=f"0x{item.value:X} ({item.value}): {item.text}",
            icon="CHECKBOX_HLT" if selected else "CHECKBOX_DEHLT",
            emboss=False,
        )
        op.flag = item.value


def draw_material_flag_lists(layout, context, material):
    """Draw the notes.txt material categories as compact scrolling lists."""
    window_manager = context.window_manager
    ensure_reference_lists(window_manager)

    box = layout.box()
    box.label(text=f"Surface Type ({material.asura_surface_type})")
    box.template_list(
        "ASURA_UL_surface_types",
        "",
        window_manager,
        "asura_surface_items",
        window_manager,
        "asura_surface_list_index",
        rows=4,
    )

    box = layout.box()
    box.label(text=f"Blending Flags: 0x{material.asura_blending_flags:X} ({material.asura_blending_flags})")
    box.template_list(
        "ASURA_UL_blending_flags",
        "",
        window_manager,
        "asura_blending_items",
        window_manager,
        "asura_blending_list_index",
        rows=4,
    )

    box = layout.box()
    box.label(text=f"Collision Flags: 0x{material.asura_collision_flags:X} ({material.asura_collision_flags})")
    box.template_list(
        "ASURA_UL_collision_flags",
        "",
        window_manager,
        "asura_collision_items",
        window_manager,
        "asura_collision_list_index",
        rows=4,
    )


class ASURA_OT_set_material_index(bpy.types.Operator):
    bl_idname = "material.asura_set_material_index"
    bl_label = "Set Asura Material Index"
    bl_description = "Assign the original Asura material index to the active Blender material"
    bl_options = {"REGISTER", "UNDO"}

    index: IntProperty(
        name="Asura Material Index",
        description="Original Asura material index; use -1 to leave this material out of the map",
        default=UNASSIGNED_INDEX,
        min=UNASSIGNED_INDEX,
        max=MAX_BLENDER_INT,
    )

    @classmethod
    def poll(cls, context):
        return active_material(context) is not None

    def invoke(self, context, event):
        material = active_material(context)
        if material is None:
            return {"CANCELLED"}
        self.index = int(material.asura_material_index)
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        material = active_material(context)
        if material is None:
            self.report({"ERROR"}, "No active material")
            return {"CANCELLED"}

        material.asura_material_index = self.index
        if self.index == UNASSIGNED_INDEX:
            self.report({"INFO"}, f"Cleared Asura material index from '{material.name}'")
        else:
            self.report({"INFO"}, f"'{material.name}' -> Asura material {self.index}")
        return {"FINISHED"}


class ASURA_OT_clear_material_index(bpy.types.Operator):
    bl_idname = "material.asura_clear_material_index"
    bl_label = "Clear Asura Material Index"
    bl_description = "Mark the active material as unassigned so it is omitted from the JSON map"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        material = active_material(context)
        return material is not None and material.asura_material_index != UNASSIGNED_INDEX

    def execute(self, context):
        material = active_material(context)
        if material is None:
            return {"CANCELLED"}
        material.asura_material_index = UNASSIGNED_INDEX
        return {"FINISHED"}


class ASURA_OT_load_material_map(bpy.types.Operator, ImportHelper):
    bl_idname = "import_scene.asura_material_map"
    bl_label = "Load Asura Material Map"
    bl_description = "Load Asura material indices, texture names, surface types, and flags from JSON"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".json"
    filter_glob: StringProperty(default="*.json", options={"HIDDEN"})

    def execute(self, context):
        global _AUTO_REFRESH_RUNNING

        try:
            input_path = Path(bpy.path.abspath(self.filepath))
            with input_path.open("r", encoding="utf-8") as source:
                material_map = json.load(source)
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            self.report({"ERROR"}, f"Could not load material map: {exc}")
            return {"CANCELLED"}

        if not isinstance(material_map, dict):
            self.report({"ERROR"}, "Material map root must be a JSON object")
            return {"CANCELLED"}

        texture_by_index = material_map.get("texture_by_material_index", {})
        surface_by_index = material_map.get("surface_type_by_material_index", {})
        blending_by_index = material_map.get("transparency_flag_by_material_index", {})
        collision_by_index = material_map.get("collision_flags", {})
        sections = (
            texture_by_index,
            surface_by_index,
            blending_by_index,
            collision_by_index,
        )
        if not all(isinstance(section, dict) for section in sections):
            self.report({"ERROR"}, "Material-map metadata sections must be JSON objects")
            return {"CANCELLED"}

        reserved_keys = {
            "texture_by_material_index",
            "surface_type_by_material_index",
            "transparency_flag_by_material_index",
            "collision_flags",
            "orig_to_handle",
            "handle_to_texname",
        }
        direct_map = {
            key.casefold(): value
            for key, value in material_map.items()
            if isinstance(key, str)
            and key not in reserved_keys
            and isinstance(value, int)
            and not isinstance(value, bool)
        }

        materials = scene_used_materials(context.scene)
        loaded = 0
        unmatched = 0
        _AUTO_REFRESH_RUNNING = True
        try:
            for material in materials:
                index = direct_map.get(material.name.casefold())
                if index is None:
                    # Rich maps exported by the level editor may not contain
                    # direct name -> index entries.  mat_<index> names still
                    # give us an unambiguous way to connect them.
                    index = material_name_index(material.name)
                if index is None or index < 0 or index > MAX_BLENDER_INT:
                    unmatched += 1
                    continue

                material.asura_skip_auto_assign = False
                material.asura_material_index = index
                key = str(index)

                texture_name = texture_by_index.get(key)
                if isinstance(texture_name, str):
                    material.asura_texture_name = texture_name

                surface_type = surface_by_index.get(key)
                if isinstance(surface_type, int) and not isinstance(surface_type, bool):
                    material.asura_surface_type = max(0, min(surface_type, MAX_BLENDER_INT))

                blending_flags = blending_by_index.get(key)
                if isinstance(blending_flags, int) and not isinstance(blending_flags, bool):
                    material.asura_blending_flags = max(0, min(blending_flags, MAX_BLENDER_INT))

                collision_flags = collision_by_index.get(key)
                if isinstance(collision_flags, int) and not isinstance(collision_flags, bool):
                    material.asura_collision_flags = max(0, min(collision_flags, MAX_BLENDER_INT))

                loaded += 1
        finally:
            _AUTO_REFRESH_RUNNING = False

        tag_asura_ui_redraw()
        if not loaded:
            self.report({"WARNING"}, "No used scene materials matched this material map")
            return {"FINISHED"}

        suffix = f"; {unmatched} used material(s) unmatched" if unmatched else ""
        self.report({"INFO"}, f"Loaded Asura data for {loaded} material(s){suffix}")
        return {"FINISHED"}


class ASURA_OT_export_material_map(bpy.types.Operator, ExportHelper):
    bl_idname = "export_scene.asura_material_map"
    bl_label = "Export Asura Material Map"
    bl_description = "Export assigned Blender material names as a Level Editor JSON material map"

    filename_ext = ".json"
    filter_glob: StringProperty(default="*.json", options={"HIDDEN"})

    def invoke(self, context, event):
        if not self.filepath:
            if bpy.data.filepath:
                blend_path = Path(bpy.data.filepath)
                self.filepath = str(blend_path.with_name(f"{blend_path.stem}_materials.json"))
            else:
                self.filepath = "asura_material_map.json"
        return ExportHelper.invoke(self, context, event)

    def execute(self, context):
        refresh_scene_assignments(context.scene)
        materials = assigned_materials(scene=context.scene)
        if not materials:
            self.report({"ERROR"}, "No materials have an Asura material index assigned")
            return {"CANCELLED"}

        # The editor performs material-name lookup case-insensitively.  Refuse
        # a map Blender could write but the editor could not resolve uniquely.
        names = {}
        for material, _index in materials:
            folded = material.name.casefold()
            previous = names.get(folded)
            if previous is not None and previous != material.name:
                self.report(
                    {"ERROR"},
                    f"Material names '{previous}' and '{material.name}' differ only by case",
                )
                return {"CANCELLED"}
            names[folded] = material.name

        material_map = {material.name: index for material, index in materials}

        texture_by_index = {}
        missing_textures = []
        for material, index in materials:
            texture_name = material.asura_texture_name.strip()
            if not texture_name:
                missing_textures.append(material.name)
                continue

            key = str(index)
            previous = texture_by_index.get(key)
            if previous is not None and previous.casefold() != texture_name.casefold():
                self.report(
                    {"ERROR"},
                    f"Asura index {index} is used by materials with different textures: "
                    f"'{previous}' and '{texture_name}'",
                )
                return {"CANCELLED"}
            texture_by_index[key] = texture_name

        if texture_by_index:
            material_map["texture_by_material_index"] = texture_by_index

        surface_by_index = {}
        blending_by_index = {}
        collision_by_index = {}
        for material, index in materials:
            key = str(index)
            metadata = (
                (surface_by_index, int(material.asura_surface_type), "surface type"),
                (blending_by_index, int(material.asura_blending_flags), "blending flags"),
                (collision_by_index, int(material.asura_collision_flags), "collision flags"),
            )
            for target, value, label in metadata:
                previous = target.get(key)
                if previous is not None and previous != value:
                    self.report(
                        {"ERROR"},
                        f"Asura index {index} is used by materials with different {label}: "
                        f"{previous} and {value}",
                    )
                    return {"CANCELLED"}
                target[key] = value

        material_map["surface_type_by_material_index"] = surface_by_index
        material_map["transparency_flag_by_material_index"] = blending_by_index
        material_map["collision_flags"] = collision_by_index

        try:
            output_path = Path(bpy.path.abspath(self.filepath))
            with output_path.open("w", encoding="utf-8", newline="\n") as output:
                json.dump(material_map, output, ensure_ascii=False, indent=2)
                output.write("\n")
        except OSError as exc:
            self.report({"ERROR"}, f"Could not write material map: {exc}")
            return {"CANCELLED"}

        duplicate_indices = len({index for _material, index in materials}) != len(materials)
        suffix_parts = []
        if duplicate_indices:
            suffix_parts.append("some materials share an Asura index")
        if missing_textures:
            suffix_parts.append(f"{len(missing_textures)} material(s) have no texture name")
        suffix = f" ({'; '.join(suffix_parts)})" if suffix_parts else ""
        self.report(
            {"INFO"},
            f"Exported {len(materials)} material mappings, {len(texture_by_index)} texture mappings, "
            f"and Asura material flags{suffix}",
        )
        return {"FINISHED"}


class ASURA_PT_material_properties(bpy.types.Panel):
    bl_label = "Asura Material"
    bl_idname = "ASURA_PT_material_properties"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "material"

    @classmethod
    def poll(cls, context):
        return active_material(context) is not None

    def draw(self, context):
        layout = self.layout
        material = active_material(context)
        if material is None:
            layout.label(text="No active material")
            return

        layout.prop(material, "asura_material_index", text="Material Index")
        layout.prop(material, "asura_texture_name", text="Texture Name")
        draw_material_flag_lists(layout, context, material)
        row = layout.row(align=True)
        row.operator(ASURA_OT_set_material_index.bl_idname, text="Set...")
        row.operator(ASURA_OT_clear_material_index.bl_idname, text="Clear")
        row = layout.row(align=True)
        row.operator(ASURA_OT_load_material_map.bl_idname, text="Load Material Map...")
        row.operator(ASURA_OT_export_material_map.bl_idname, text="Export Material Map...")


class ASURA_PT_view3d_sidebar(bpy.types.Panel):
    bl_label = "Material Map"
    bl_idname = "ASURA_PT_view3d_sidebar"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Asura"

    def draw(self, context):
        layout = self.layout
        material = active_material(context)

        if material is None:
            layout.label(text="Select an object with a material.", icon="INFO")
        else:
            layout.label(text=material.name, icon="MATERIAL")
            layout.prop(material, "asura_material_index", text="Material Index")
            layout.prop(material, "asura_texture_name", text="Texture Name")
            draw_material_flag_lists(layout, context, material)
            row = layout.row(align=True)
            row.operator(ASURA_OT_set_material_index.bl_idname, text="Set...")
            row.operator(ASURA_OT_clear_material_index.bl_idname, text="Clear")

        layout.separator()
        row = layout.row(align=True)
        row.operator(ASURA_OT_load_material_map.bl_idname, text="Load Map...")
        row.operator(ASURA_OT_export_material_map.bl_idname, text="Export Map...")


def draw_asura_material_menu(layout, context):
    material = active_material(context)
    if material is None:
        return

    layout.separator()
    index = int(material.asura_material_index)
    text = "Asura Material Index: Unassigned" if index == UNASSIGNED_INDEX else f"Asura Material Index: {index}"
    row = layout.row()
    row.enabled = False
    row.label(text=text, icon="MATERIAL")
    layout.operator(ASURA_OT_set_material_index.bl_idname, text="Set Asura Material Index...")
    if index != UNASSIGNED_INDEX:
        layout.operator(ASURA_OT_clear_material_index.bl_idname, text="Clear Asura Material Index")
    layout.operator(ASURA_OT_load_material_map.bl_idname, text="Load Asura Material Map...")
    layout.operator(ASURA_OT_export_material_map.bl_idname, text="Export Asura Material Map...")


def draw_material_context_menu(self, context):
    draw_asura_material_menu(self.layout, context)


def draw_object_context_menu(self, context):
    draw_asura_material_menu(self.layout, context)


def draw_file_export(self, context):
    self.layout.operator(
        ASURA_OT_export_material_map.bl_idname,
        text="Asura Material Map (.json)",
    )


def draw_file_import(self, context):
    self.layout.operator(
        ASURA_OT_load_material_map.bl_idname,
        text="Asura Material Map (.json)",
    )


CLASSES = (
    ASURA_PG_reference_item,
    ASURA_OT_set_surface_type,
    ASURA_OT_toggle_blending_flag,
    ASURA_OT_toggle_collision_flag,
    ASURA_UL_surface_types,
    ASURA_UL_blending_flags,
    ASURA_UL_collision_flags,
    ASURA_OT_set_material_index,
    ASURA_OT_clear_material_index,
    ASURA_OT_load_material_map,
    ASURA_OT_export_material_map,
    ASURA_PT_material_properties,
    ASURA_PT_view3d_sidebar,
)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)

    bpy.types.WindowManager.asura_surface_items = CollectionProperty(type=ASURA_PG_reference_item)
    bpy.types.WindowManager.asura_blending_items = CollectionProperty(type=ASURA_PG_reference_item)
    bpy.types.WindowManager.asura_collision_items = CollectionProperty(type=ASURA_PG_reference_item)
    bpy.types.WindowManager.asura_surface_list_index = IntProperty(default=0, min=0)
    bpy.types.WindowManager.asura_blending_list_index = IntProperty(default=0, min=0)
    bpy.types.WindowManager.asura_collision_list_index = IntProperty(default=0, min=0)

    bpy.types.Material.asura_skip_auto_assign = BoolProperty(
        name="Skip Asura Auto Assignment",
        description="Keep this material unassigned during automatic refresh",
        default=False,
        options={"HIDDEN"},
    )
    bpy.types.Material.asura_material_index = IntProperty(
        name="Asura Material Index",
        description="Original Asura material index used by Asura Level Editor",
        default=UNASSIGNED_INDEX,
        min=UNASSIGNED_INDEX,
        max=MAX_BLENDER_INT,
        update=material_index_updated,
    )
    bpy.types.Material.asura_texture_name = StringProperty(
        name="Asura Texture Name",
        description=(
            "Texture resource/file name associated with this Asura material index; "
            "auto-detected from the material's image texture"
        ),
        default="",
    )
    bpy.types.Material.asura_surface_type = IntProperty(
        name="Asura Surface Type",
        description="Asura surface type written to surface_type_by_material_index",
        default=1,
        min=0,
        max=MAX_BLENDER_INT,
    )
    bpy.types.Material.asura_blending_flags = IntProperty(
        name="Asura Blending Flags",
        description="Asura blending/transparency bit flags",
        default=0,
        min=0,
        max=MAX_BLENDER_INT,
    )
    bpy.types.Material.asura_collision_flags = IntProperty(
        name="Asura Collision Flags",
        description="Asura collision bit flags",
        default=0,
        min=0,
        max=MAX_BLENDER_INT,
    )

    ensure_reference_lists(getattr(bpy.context, "window_manager", None))

    bpy.types.MATERIAL_MT_context_menu.append(draw_material_context_menu)
    bpy.types.VIEW3D_MT_object_context_menu.append(draw_object_context_menu)
    bpy.types.TOPBAR_MT_file_import.append(draw_file_import)
    bpy.types.TOPBAR_MT_file_export.append(draw_file_export)

    if asura_load_post not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(asura_load_post)
    if asura_depsgraph_update_post not in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.append(asura_depsgraph_update_post)

    # Do not wait for an export or scene edit before filling the panel.
    refresh_scene_assignments(getattr(bpy.context, "scene", None))


def unregister():
    if asura_depsgraph_update_post in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(asura_depsgraph_update_post)
    if asura_load_post in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(asura_load_post)

    bpy.types.TOPBAR_MT_file_export.remove(draw_file_export)
    bpy.types.TOPBAR_MT_file_import.remove(draw_file_import)
    bpy.types.VIEW3D_MT_object_context_menu.remove(draw_object_context_menu)
    bpy.types.MATERIAL_MT_context_menu.remove(draw_material_context_menu)

    del bpy.types.Material.asura_texture_name
    del bpy.types.Material.asura_collision_flags
    del bpy.types.Material.asura_blending_flags
    del bpy.types.Material.asura_surface_type
    del bpy.types.Material.asura_material_index
    del bpy.types.Material.asura_skip_auto_assign

    del bpy.types.WindowManager.asura_collision_list_index
    del bpy.types.WindowManager.asura_blending_list_index
    del bpy.types.WindowManager.asura_surface_list_index
    del bpy.types.WindowManager.asura_collision_items
    del bpy.types.WindowManager.asura_blending_items
    del bpy.types.WindowManager.asura_surface_items

    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
