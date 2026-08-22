# Asura 2005 Level Editor

`AsuraLevelEditor.exe` is a native Win32 desktop front end for the existing OBJ-to-Env constructor. It renders the transformed OBJ, authors spawn points, lights, sounds, and donor-backed physical entities, stores the working level in a binary `.alev` project, and exports one final target-game `.PC`. Entity JSON is not used by this path.

## Why object import is donor-backed

An Asura object in a level is two related records, not one imported mesh:

1. Resource chunks load the visual/collision asset. In Sniper Elite 2005 this includes `RSCF` platform subtype 2 (`Asura_Chunk_ResourceFile_PC_Object`) plus shape, hierarchy, material, and texture support chunks.
2. An `ENTI` chunk creates the gameplay object. Its 24-byte header contains a GUID and 16-bit classification; everything after that is a versioned, class-specific payload.

The payload is substantially different for a pickup, static object, building, assassination target, and other project entities. Inventing a short “generic object” payload therefore creates invalid entities. The editor imports a known-good target-game ENTI as a template, preserves its unknown state, assigns a new GUID, and patches the common physical-object position/quaternion base. It also copies the donor's object support resources into the exported level before emitting the cloned entity.

This works for target pickup entities such as weapons, ammunition, and medkits (classification 8), and for static/physical barricade-like entities when a placed instance exists in a donor `.PC`.

## IDA results

### Port 13338 — Sniper Elite 2005 target, no PDB

- `sub_43F250`, identified by `Asura_Chunk_Loaders.cpp`, is the top-level chunk dispatcher.
- `sub_441310` handles `RSCF`. Type 0 enters the PC platform dispatcher `sub_49E340`.
- `sub_49E340` switches on the PC platform subtype. The target meanings are 0 Environment, 1 Character, 2 Object, 3 ReplaceableSounds, 4 unhandled/skipped, and 5 ObjectHierarchy. This is not the later PDB's Xbox 360 subtype enum.
- `sub_49E420` is the subtype-1 PC Character reader used by `MPChars.asr`. After a padded character/skin name it reads four `uint32_t` values, creates a vertex buffer with a 64-byte stride and a 16-bit index buffer, then uploads `vertexCount * 64` and `indexCount * 2` bytes. The first value is `indexCount - 2`, proving that the index data is one stitched triangle strip whose degenerate windows join its sections.
- The `russian_soldier9` character resource contains 2,064 vertices, 4,683 strip indices, and 2,743 nondegenerate triangles. `german_soldier5` contains 2,136 vertices, 4,895 strip indices, and 2,835 nondegenerate triangles. Position and normal are the first two three-float fields of each target vertex. Their Y coordinates use the target's negative-up convention, so the preview applies the same game-to-editor Y conversion as placed entities.
- The target Environment reader uses 12-byte module records, 20-byte strip records, and 36-byte vertices. Draw calls prove the strip fields are triangle count, start index, material-response hash, lowest vertex used, and vertex count. These target-only records are named `Asura_PC_EnvironmentRenderer_*`; the PDB's 52-byte Xbox 360 strip is not wire-compatible.
- `sub_49E810` identifies itself through assertions as `Asura_Chunk_ResourceFile_PC_Object.cpp`. It resolves a shape, reads the object counts/state, creates GPU vertex and index buffers, and links an optional lower-LOD shape.
- `sub_43C470` reads the complete 32-byte version-0 fog record. The later PDB version adds an extra graph-fog point, but confirms the shared names `xColour`, `fNearPlane`, `fFarPlane`, `fValueAtFarPlane`, and `fSkyboxValue`.
- `sub_43EB50` is the `LITE` loader. For chunk versions 3 through 5 it allocates and reads `0x6C` bytes per `Asura_Light`; `sub_43EF00` then copies the fields through `m_uFlags` at offset `0x50`, maintains the old-position/range fields, and sets `HasChanged` at `0x68`.
- Target light-flag use proves the little-endian wire masks: `HasCorona=0x01`, `UseBoundingBox=0x20`, and `IsShadowVolume=0x40`. The intervening flags follow the reversed low-bit ordering of the Xbox PDB declaration, so the source stores a raw `uint32_t` plus explicit PC masks instead of compiler-dependent bitfields.
- `sub_43B0D0` reads an ENTI body and calls `sub_43AC20`.
- `sub_43AC20` is the engine entity factory; project classifications fall through to the Sniper factory at `sub_5A4B00`.
- `sub_43B110` is the ENTI serializer. The exact wire header is `ENTI`, total size, version, flags, GUID, `uint16 classification`, `uint16 padding`, followed by the virtual class writer. The full fixed header is 24 bytes.
- Project classification 8 calls `sub_57D590`, the `Snipe_ServerEntity_PickupObject` construction path. Classification `0x803A` reaches `sub_591A40`, whose source assertion names the class `Snipe_ServerEntity_SpawnPoint`; `0x804F` is specifically `Snipe_ServerEntity_AssassinationTarget`, not a generic building class.
- The `0x803A` constructor accepts payload version 0 and reads position, direction, spawn index, posture, team mask, game-mode mask, and a spawn timer. Target checks at `sub_591570`/`sub_591580` prove the two bit masks; the update path at `sub_591560` proves the final value is a timer, not the previously guessed activation radius.
- The activatable base reader accepts version 2 and reads its active flag before `Asura_ServerEntity_SoundController` reads its own version 0 and phonon GUID. These are separate version fields in the ENTI payload.
- The class-8 target writer reaches `sub_57C270` (project pickup), `sub_466560` (Asura pickup), `sub_57EDB0` (Sniper static object), `sub_57B570` (Sniper physical object), and `sub_465A00` (Asura physical object).
- In that 2005 pickup layout the common Asura physical base is version 7. In the complete ENTI chunk, position starts at offset `0x64` and quaternion starts at `0x70`. Other physical classes can have a different prefix, so the editor locates the final aligned version-7 + finite position + unit-quaternion signature instead of assuming the pickup offsets for every class.
- Source-path strings and readers at `sub_57A370`, `sub_57B5F0`, `sub_57C2F0`, and `sub_57EE10` independently identify Building, PhysicalObject, PickupObject, and StaticObject serialization layers.

### Port 13337 — 2008 PDB-rich `AvP3_Retail.xex`

- The PDB local types confirm a 16-byte `Asura_Chunk_Header`: the anonymous `ID`/`TextID[4]` union is at `0x00`, followed by `Size`, `Version`, and `Flags` at `0x04`, `0x08`, and `0x0C`.
- `Asura_Bounding_Box` is six interleaved scalar bounds—`MinX`, `MaxX`, `MinY`, `MaxY`, `MinZ`, `MaxZ`—rather than two three-component vectors. Target assertions independently name `MinX` and `MaxX`, so the 24-byte layout is shared by both builds.
- `Asura_Chunk_Lights::Process` at `0x821FA448` accepts version 5, allocates `0x6C` bytes per light, and reads exactly that many bytes. `Asura_Light::Set` at `0x821FA380` confirms `R/G/B` at `0x18..0x20`, the flags union at `0x50`, old position/range at `0x58`/`0x64`, and `HasChanged` at `0x68`.
- The PDB's 170 `ASURA_CHUNKID` and 87 `ASURA_ENTITY_CLASSIFICATION` members match the local declarations exactly. The target dispatch maps every one of its 63 explicit/boundary chunk IDs into that chunk enum, but later-only members remain reference metadata rather than target support claims.
- The PDB does not contain `ASURA_RESOURCEFILE_TYPE_ID_PC`. It contains the Xbox 360 platform enum instead (`Environment`, `Character`, `Object`, `ObjectHierarchy`, `XPR`, `Botanicals`, `MaterialResponse`, `Carpet`, `StaticDecals`, and `StreamingSounds`), so it must not be used to number the target's PC resource subtypes.
- The PDB expresses the light flags as Xbox bitfields. On the little-endian target the corresponding raw masks run from `HasCorona=0x01` through `IsShadowVolume=0x40`, so the source intentionally serializes a `uint32_t` rather than relying on compiler bitfield order.

Every serialized structure used by the constructor/editor is now guarded by a size assertion, with critical offsets asserted separately. Shared types use the PDB names and layouts; target-only PC/versioned wire records carry explicit platform or version suffixes. The common conclusion from both builds is: import the resource bundle and instantiate it with a real class payload. The target-compatible way to do that without reconstructing every private class is template cloning.

## Editor workflow

1. Build `LevelEditor.vcxproj`, or run `x64\Release\AsuraLevelEditor.exe`.
2. Choose **Open OBJ**. The viewport shows an upright, non-mirrored authoring view using `(x, y, -z)`. Final target-game Env vertex data uses the required `(x, -y, -z)` conversion. Gameplay entity values—including sounds, lights, spawnpoints, imported objects, and CLI JSON—remain raw game coordinates, where negative Y is higher. The viewport negates entity Y only for marker display, so a negative inspector Y appears above the ground plane without changing the packed value.
3. Optionally choose a material-map JSON and a texture folder. These are environment construction inputs; entity placement itself is not JSON-backed.
4. Keep the target `MPChars.asr` in the editor's working directory, beside the executable, or two folders above an `x64\Release` executable. Choose **+ Spawn**, **+ Light**, or **+ Sound**, then click the viewport's Y=0 ground plane. A sound's WAV and looping state are selected in its inspector.
5. For a barricade, weapon, ammo box, medkit, or another physical object, choose **Import object**, select a target-game donor `.PC`, then choose an importable ENTI from the popup by classification/GUID. Click the viewport to place it.
6. Edit position, direction/rotation, and type-specific properties in the inspector. A selected light also exposes an **All light properties...** window for every field in `Asura_Light`: RGB colour, brightness/ranges, angle, shadow strength, bounding box, raw and named flags, brightness-over-range, old position/range, and changed state. Selected entities can also be dragged across the ground plane.
7. Save a binary `.alev` project and choose **Export .PC**.

The current `.alev` format is version 3 so complete light records and sound looping state survive project save/load. Version-1 and version-2 projects remain readable; older sounds default to looping, matching the previous editor's export behavior.

The selected light is visualized in the viewport at its exact range and with an arrow showing its direction. An angle of 360 degrees draws a complete three-ring globe; 180 degrees draws a direction-facing hemisphere; narrower angles draw progressively tighter spherical cone sectors. Unselected lights retain only their normal entity markers so the viewport stays uncluttered.

The selected sound is visualized with a blue wire globe at its maximum radius. Unselected sounds retain only their normal entity markers.

Spawnpoints with team mask `5` (Russian + Deathmatch) render the actual `russian_soldier9` mesh decoded from `MPChars.asr`; team mask `3` (German + Deathmatch) renders `german_soldier5`. The puppet is lit, faction-tinted, anchored at the spawn position, and rotated by the entity's full pitch/yaw/roll transform. Like the other entity markers, it remains visible through level geometry, while a separate puppet depth pass still gives the character correct self-occlusion. Clicking anywhere inside its projected bounds selects it. A newly placed spawn defaults to mask `5`. An unsupported mask or unavailable/incompatible archive retains the old marker as an explicit fallback.

Viewport controls:

- Right-drag: orbit
- Middle-drag: pan
- Mouse wheel: zoom
- Left-click: select/place
- Left-drag a marker: move it in X/Z

The executable also has a noninteractive pack entry point for automation:

```text
AsuraLevelEditor.exe --pack project.alev output.PC
```

## Current boundaries

- An imported gameplay object needs a donor instance from the 2005 target. The newer PDB game is structural documentation, not a binary payload donor; its class versions and renderer resources are not target-compatible.
- The import scanner deliberately accepts physical-object payloads with the target's version-7 transform base. Nonphysical logic entities are excluded instead of being patched at a guessed offset.
- Resource import currently copies the selected donor's object/hierarchy/shape/material/texture support families and all non-environment RSCFs. A very large donor level can therefore make the result larger than a hand-trimmed asset pack.
- Spawn puppets are static editor previews of the character resource's authored vertex positions. They do not add `MPChars.asr` to exported levels and do not run the game's animation/skeleton system.
- The OBJ viewport uses a D3D11 child swap chain. Positions, averaged normals, and triangle indices are uploaded once when the OBJ changes; orbit, pan, zoom, selection, and entity movement update only a small camera/overlay buffer and issue an indexed GPU draw. The earlier cached GDI renderer remains as a device-initialization fallback. Dense meshes are not sparsely sampled. The final render/collision data still comes from the existing constructor, so the exported geometry is identical to the CLI path.

## Verification

The Release x64 editor and the original Release x64 constructor both build successfully. `--smoke-pack testdata\minimal.obj testdata\editor-smoke.PC` was used to exercise the editor's in-memory path. The output contained, in order, `RSCF(Env)`, editor-authored `LITE`, environment/module chunks, editor-authored `ENTI 0x803A`, sky/fog/weather chunks, and the 16-byte Asura terminator.
