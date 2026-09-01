from pathlib import Path

import bpy


ALPHA_MODE = "CHANNEL_PACKED"
ENABLE_BACKFACE_CULLING = True


def set_dds_alpha_mode() -> int:
    changed = 0
    for image in bpy.data.images:
        if image.source != "FILE" or not image.filepath:
            continue

        filepath = Path(bpy.path.abspath(image.filepath))
        if filepath.suffix.lower() != ".dds":
            continue

        if image.alpha_mode != ALPHA_MODE:
            image.alpha_mode = ALPHA_MODE
            changed += 1

    return changed


def set_material_backface_culling() -> int:
    changed = 0
    for material in bpy.data.materials:
        if not material.name.startswith("mat_"):
            continue

        if material.use_backface_culling != ENABLE_BACKFACE_CULLING:
            material.use_backface_culling = ENABLE_BACKFACE_CULLING
            changed += 1

    return changed


if __name__ == "__main__":
    changed_image_count = set_dds_alpha_mode()
    changed_material_count = set_material_backface_culling()
    print(
        f"Asura OBJ: set {changed_image_count} loaded DDS texture(s) "
        f"to {ALPHA_MODE}; set backface culling to "
        f"{ENABLE_BACKFACE_CULLING} on {changed_material_count} material(s)."
    )
