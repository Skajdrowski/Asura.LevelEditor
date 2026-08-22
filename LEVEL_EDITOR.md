# Asura 2005 Level Editor

`AsuraLevelEditor.exe` is a native Win32 desktop front end for the existing OBJ-to-Env constructor. It renders the transformed OBJ, authors spawn points, lights, and sounds, stores the working level in a binary `.alev` project, and exports one final target-game `.PC`. Entity JSON is not used by this path.

## IDA results

### Port 13338 — Sniper Elite 2005 target, no PDB

- `sub_43F250`, identified by `Asura_Chunk_Loaders.cpp`, is the top-level chunk dispatcher.
- `sub_441690` loads target `SKYB` version 7 and `sub_49CEE0` resolves all eight texture slots. Slot 0 is the empty lower face, slots 1–5 are `fr`, `lf`, `bk`, `rt`, and `up`, and slots 6–7 are the cloud textures. `sub_49D000` renders the six cube faces, centering them on the camera, substituting white for the empty lower face, and assigning `(1,1)`, `(1,0)`, `(0,0)`, `(0,1)` to each face's four ordered corners. `sub_423210` builds the cloud layer's tessellated sphere, and `sub_49D600` flattens and offsets it into a camera-centered ellipsoid with repeating spherical UVs and two animated samples at `time/64` and `time/128`. The continuous cloud surface spans the upper cube-face boundaries and hides their cutoff. The editor viewport reproduces that slot layout, face table, cloud geometry, and animation rates; its cloud sampler uses an equivalent-density pole-safe projection to avoid exposing the target mesh's longitude singularity as radial wedges when the editor camera looks through it.
- `sub_441310` handles `RSCF`. Type 0 enters the PC platform dispatcher `sub_49E340`.
- `sub_49E340` switches on the PC platform subtype. The target meanings are 0 Environment, 1 Character, 2 Object, 3 ReplaceableSounds, 4 unhandled/skipped, and 5 ObjectHierarchy. This is not the later PDB's Xbox 360 subtype enum.
- `sub_49E420` is the subtype-1 PC Character reader used by `MPChars.asr`. After a padded character/skin name it reads four `uint32_t` values, creates a vertex buffer with a 64-byte stride and a 16-bit index buffer, then uploads `vertexCount * 64` and `indexCount * 2` bytes. The first value is `indexCount - 2`, proving that the index data is one stitched triangle strip whose degenerate windows join its sections.
- The `russian_soldier9` character resource contains 2,064 vertices, 4,683 strip indices, and 2,743 nondegenerate triangles. `german_soldier5` contains 2,136 vertices, 4,895 strip indices, and 2,835 nondegenerate triangles. Position and normal are the first two three-float fields of each target vertex. Their Y coordinates use the target's negative-up convention, so the preview applies the same game-to-editor Y conversion as placed entities.
- The target Environment reader uses 12-byte module records, 20-byte strip records, and 36-byte vertices. Draw calls prove the strip fields are triangle count, start index, material-response hash, lowest vertex used, and vertex count. These target-only records are named `Asura_PC_EnvironmentRenderer_*`; the PDB's 52-byte Xbox 360 strip is not wire-compatible.
- `sub_43C470` reads the complete 32-byte version-0 fog record. The later PDB version adds an extra graph-fog point, but confirms the shared names `xColour`, `fNearPlane`, `fFarPlane`, `fValueAtFarPlane`, and `fSkyboxValue`.
- `sub_43EB50` is the `LITE` loader. For chunk versions 3 through 5 it allocates and reads `0x6C` bytes per `Asura_Light`; `sub_43EF00` then copies the fields through `m_uFlags` at offset `0x50`, maintains the old-position/range fields, and sets `HasChanged` at `0x68`.
- Target light-flag use proves the little-endian wire masks: `HasCorona=0x01`, `UseBoundingBox=0x20`, and `IsShadowVolume=0x40`. The intervening flags follow the reversed low-bit ordering of the Xbox PDB declaration, so the source stores a raw `uint32_t` plus explicit PC masks instead of compiler-dependent bitfields.
- `sub_43B0D0` reads an ENTI body and calls `sub_43AC20`.
- `sub_43AC20` is the engine entity factory; project classifications fall through to the Sniper factory at `sub_5A4B00`.
- `sub_43B110` is the ENTI serializer. The exact wire header is `ENTI`, total size, version, flags, GUID, `uint16 classification`, `uint16 padding`, followed by the virtual class writer. The full fixed header is 24 bytes.
- Project classification `0x803A` reaches `sub_591A40`, whose source assertion names the class `Snipe_ServerEntity_SpawnPoint`.
- The `0x803A` constructor accepts payload version 0 and reads position, direction, spawn index, posture, team mask, game-mode mask, and a spawn timer. Target checks at `sub_591570`/`sub_591580` prove the two bit masks; the update path at `sub_591560` proves the final value is a timer, not the previously guessed activation radius.
- The activatable base reader accepts version 2 and reads its active flag before `Asura_ServerEntity_SoundController` reads its own version 0 and phonon GUID. These are separate version fields in the ENTI payload.

### Port 13337 — 2008 PDB-rich `AvP3_Retail.xex`

- The PDB local types confirm a 16-byte `Asura_Chunk_Header`: the anonymous `ID`/`TextID[4]` union is at `0x00`, followed by `Size`, `Version`, and `Flags` at `0x04`, `0x08`, and `0x0C`.
- `Asura_Skybox::Platform_Initialise` at `0x823FB270`, `Asura_Skybox::Platform_Render` at `0x823FB6A0`, and `CreateVerticesForFace` at `0x823FAF30` confirm the camera-centered six-face construction. Its later-build base-face UV order differs from the exact 2005 PC order above, so it is reference geometry rather than the viewport's UV source.
- `Asura_Bounding_Box` is six interleaved scalar bounds—`MinX`, `MaxX`, `MinY`, `MaxY`, `MinZ`, `MaxZ`—rather than two three-component vectors. Target assertions independently name `MinX` and `MaxX`, so the 24-byte layout is shared by both builds.
- `Asura_Chunk_Lights::Process` at `0x821FA448` accepts version 5, allocates `0x6C` bytes per light, and reads exactly that many bytes. `Asura_Light::Set` at `0x821FA380` confirms `R/G/B` at `0x18..0x20`, the flags union at `0x50`, old position/range at `0x58`/`0x64`, and `HasChanged` at `0x68`.
- The PDB's 170 `ASURA_CHUNKID` and 87 `ASURA_ENTITY_CLASSIFICATION` members match the local declarations exactly. The target dispatch maps every one of its 63 explicit/boundary chunk IDs into that chunk enum, but later-only members remain reference metadata rather than target support claims.
- The PDB does not contain `ASURA_RESOURCEFILE_TYPE_ID_PC`. It contains the Xbox 360 platform enum instead (`Environment`, `Character`, `Object`, `ObjectHierarchy`, `XPR`, `Botanicals`, `MaterialResponse`, `Carpet`, `StaticDecals`, and `StreamingSounds`), so it must not be used to number the target's PC resource subtypes.
- The PDB expresses the light flags as Xbox bitfields. On the little-endian target the corresponding raw masks run from `HasCorona=0x01` through `IsShadowVolume=0x40`, so the source intentionally serializes a `uint32_t` rather than relying on compiler bitfield order.

Every serialized structure used by the constructor/editor is now guarded by a size assertion, with critical offsets asserted separately. Shared types use the PDB names and layouts; target-only PC/versioned wire records carry explicit platform or version suffixes.

## Editor workflow

1. Build `LevelEditor.vcxproj`, or run `x64\Release\AsuraLevelEditor.exe`.
2. Choose **Open OBJ**. The viewport shows an upright, non-mirrored authoring view using `(x, y, -z)`. Final target-game Env vertex data uses the required `(x, -y, -z)` conversion. Gameplay entity values—including sounds, lights, spawnpoints, and CLI JSON—remain raw game coordinates, where negative Y is higher. The viewport negates entity Y only for marker display, so a negative inspector Y appears above the ground plane without changing the packed value.
3. Optionally choose a material-map JSON and a texture folder. Choose **Weapons donor** to select a target-game `.PC` whose weapon, ammunition, pickup, texture, and sound support resources should be imported; this is the editor equivalent of `--weapon-rscf-from-pc`. Choose **Skybox textures** to import the sky textures from a folder, equivalent to `--sky-texture-dir`, and render them immediately in the Direct3D viewport. The static face basenames are `fr`, `lf`, `bk`, `rt`, and `up`; `ch_04_sky` is a separate animated cloud texture, not a sixth cube face. The viewport wraps two scrolling samples of it around a continuous cloud dome so it crosses and conceals the upper horizontal-face cutoffs, as in the game. Filename extensions are ignored by both preview and export because every texture contains DDS data. The preview verifies the DDS signature, and the intentionally empty lower face uses MCP2's white fallback. These are construction inputs; entity placement itself is not JSON-backed.
4. Keep the target `MPChars.asr` in the editor's working directory, beside the executable, or two folders above an `x64\Release` executable. Choose **+ Spawn**, **+ Light**, or **+ Sound**, then click the viewport's Y=0 ground plane. A sound's WAV and looping state are selected in its inspector.
5. Edit position, direction/rotation, and type-specific properties in the inspector. A selected light also exposes an **All light properties...** window for every field in `Asura_Light`: RGB colour, brightness/ranges, angle, shadow strength, bounding box, raw and named flags, brightness-over-range, old position/range, and changed state. Selected entities can also be dragged across the ground plane.
6. Save a binary `.alev` project and choose **Export .PC**.

The current `.alev` format is version 6 so the resource-folder paths, weapons-donor path, complete light records, and sound looping state survive project save/load. Version-1 through version-5 projects remain readable; unsupported legacy entity records are omitted, and older sounds default to looping.

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

The hidden GPU regression mode accepts an optional sky folder: `AsuraLevelEditor.exe --gpu-smoke level.obj sky-folder`.

## Current boundaries

- Spawn puppets are static editor previews of the character resource's authored vertex positions. They do not add `MPChars.asr` to exported levels and do not run the game's animation/skeleton system.
- The OBJ viewport uses a D3D11 child swap chain. Positions, averaged normals, and triangle indices are uploaded once when the OBJ changes; orbit, pan, zoom, selection, and entity movement update only a small camera/overlay buffer and issue an indexed GPU draw. The earlier cached GDI renderer remains as a device-initialization fallback. Dense meshes are not sparsely sampled. The final render/collision data still comes from the existing constructor, so the exported geometry is identical to the CLI path.

## Verification

The Release x64 editor and the original Release x64 constructor both build successfully. `--smoke-pack testdata\minimal.obj testdata\editor-smoke.PC` was used to exercise the editor's in-memory path. The output contained, in order, `RSCF(Env)`, editor-authored `LITE`, environment/module chunks, editor-authored `ENTI 0x803A`, sky/fog/weather chunks, and the 16-byte Asura terminator.
