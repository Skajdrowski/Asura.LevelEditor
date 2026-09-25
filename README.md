# Creating a New Level

To create a brand-new level from scratch, first design your level geometry and assign it's materials with .DDS texture format and export it as a **Wavefront OBJ (`.obj`)** file. The exported OBJ can then be opened in the **Asura Level Editor** and converted into a Sniper Elite compatible `.PC` level.

The **Asura Material Map** Blender extension assigns Asura material indices, texture names, surface types, blending flags and collision flags to export the material map used by Asura levels.

> Blender is the only supported application for the material workflow.
>
> Although **Asura Level Editor** reads standard Wavefront OBJ geometry, material setups created in other 3D modelling applications are not supported.

Every level geometry has pre-baked vertex color lighting. **Blender** can bake vertex colors via **Cycles** rendering engine and export it within the level into **Wavefront OBJ**.

The usual workflow for a new level is:

1. Create, texture the level geometry and bake it's vertex lighting in **Blender**.
2. Configure the materials using the **Asura Material Map** extension.
3. Export the level geometry as a **Wavefront OBJ**.
4. Export the corresponding **material map** via Blender addon.
5. Open the OBJ inside **Asura Level Editor**.
6. Import material map and select the texture folder containing textures used by your level geometry in **.DDS format**.
7. Select weapons donor .PC for weapon models and pickups.
8. Add light & spawn entities and configure their properties.
9. Export the finished level and overwrite one of the Sniper Elite **`.PC`**  multiplayer levels.

<img width="1920" height="1032" alt="Level about to be exported." src="https://github.com/user-attachments/assets/e76a60f8-0749-4a6a-b96f-e1fd5403da26" />


You can save your work into Level Editor's own **.alev** project format and comeback to it later.

# Asura materials

Each material is identified by an **Asura material index** and level geometry faces reference that index.
The material index then defines the texture and material behaviour used by the game.

**Asura Material Map** Blender extension stores the following data for each material used by the scene:

- **Material Index:** Numeric Asura material ID referenced by the exported level geometry.
- **Texture Name:** Name of the corresponding texture resource. Level textures are supplied in **DDS** format.
- **Surface Type:** Material ID. Game responds to them by footsteps or bullet impacts.
- **Blending Flags:** Texture rendering flags.
- **Collision Flags:** Collision behaviour flags.

These properties are available in Blender under **Material Properties > Asura Material** and in the **Asura > Material Map** sidebar.
The extension automatically fills missing material indices and texture names for materials which are actually used by mesh faces.
A material named `mat_123` keeps index `123`; materials without an index in their name receive the lowest unused index alphabetically.
Clearing an Asura material index leaves that material out of the exported map.

> Asura material indices are independent from Blender material-slot indices.
>
> Material names in a material map **are not** case-sensitive.

## Surface types

Each material has one surface type:

| Value | Surface type |
| ---: | --- |
| `1` | Concrete |
| `2` | Glass |
| `4` | Metal |
| `5` | Water |
| `6` | Wood |
| `7` | Human body |
| `8` | Wrecked car |
| `10` | Dirt |
| `11` | Grass |
| `12` | Gravel |
| `13` | Wet |

## Blending flags

Blending bit flags per material.

>Textures need to have corresponding alpha packed channels for some of these flags to have visible results in-game.

| Flag | Effect |
| ---: | --- |
| `0x1` | Additive |
| `0x2` | Alpha texture |
| `0x4` | Detail mapping via `detail.dds` |
| `0x80` | `Spheremap1.dds` reflection |
| `0x400` | Scrolling texture |
| `0x1000` | Light shaft depth |
| `0x4000` | Affected by rain |

## Collision flags

Collision bit flags per material.

| Flag | Effect |
| ---: | --- |
| `0x20` | Ignore entities only |
| `0x40` | Ignore bullets only |
| `0x200` | Ignore bullets & grenades only |
| `0x400` | Include backface |

## Material map

The Blender extension exports a JSON material map consumed by **Asura Level Editor**. Material names map to their Asura indices, while the remaining sections attach texture names and flags to those indices. For example:

```json
{
  "Brick": 0,
  "Window": 1,
  "texture_by_material_index": {
    "0": "brick.dds",
    "1": "window.dds"
  },
  "surface_type_by_material_index": {
    "0": 1,
    "1": 2
  },
  "transparency_flag_by_material_index": {
    "0": 0,
    "1": 2
  },
  "collision_flags": {
    "0": 0,
    "1": 1024
  }
}
```

The same map can be loaded back into Blender or imported into **Asura Level Editor**. When exporting a custom level, Level Editor uses it to resolve OBJ material names to Asura material indices, to build the level's material, rendering and collision data.

# Asura entities

Level Editor supports 5 server-side entities: Spawn, Object, Pickup, Sound, Indoor Zone
and 2 client-side entities: Light and Ambience region

>Collision barriers are separate level collision geometry.

## Spawn

Player spawnpoints. Level Editor can render them in viewport as T-Posing characters, depending on their team assignment.

There are 4 team choices: Teamless (Deathmatch specific), Germany, Russia and Camera-only (Used for screen before player spawns)

Spawns also have their **Game mode gate**, which allows to create game mode exclusive spawnpoints.

<img width="270" height="222" alt="Spawn properties" src="https://github.com/user-attachments/assets/8e0f9e4f-89cc-42fa-b697-585275ac5ec7" />


> Level Editor lists **Single-Player** and **Cooperative** game modes to preserve their values from stock .PC levels.
>
> They shouldn't be used for **Multiplayer levels**.

## Object

Physical geometry objects hold only one material, one texture and one blending/collision flag for their entire geometry.

>Objects **do not** participate in level's prebaked vertex lighting.
>They receive light only from **Light** entities.

They can be used as a collision hotfix for remote players who play on an outdated level
and remain invisible if remote players do not posses it's assigned texture in their level.

<img width="666" height="431" alt="Object properties" src="https://github.com/user-attachments/assets/2039c343-5a9d-48b4-836d-7fb6ba666315" />

User can import his own objects from **Wavefront OBJ** and assign their surface type and blending/collision flags, like level geometry.
And also export to **Wavefront OBJ** when selected with their original level coordinates, so they can be separated from Entities and unified with level geometry.

## Pickup

**Pickup** entities are collectible inventory items, such as:

- Med-kits
- Rifle ammunition
- PPSH submachine gun

## Sound

**Sound** entities are invisible sound controllers.

Each controller can play single imported **PCM `.wav`** audio within a defined range around the players.

**Audio properties:**

- **Loop:** Controls repeated playback independently of the trigger.
- **Play when the level starts:** Starts playing audio immediately on level load.
- **Use bounding box:** Audio starts playing when any player enters it's **Bounding Box**.
- **Trigger once:** Prevents subsequent audio triggers. Otherwise, the audio controller rearms itself after all living players leave defined box.
- **Stop when all players leave:** Stops playback when there's no player inside the box.
- **Volume:** Sets playback volume from 0 to 1. Min and Max are used for randomization (For example Min=0 and Max=1 will randomize volume between 0 and 1)
- **Pitch** Changes playback pitch and speed. Min and Max are used for randomization.

<img width="546" height="431" alt="Audio properties" src="https://github.com/user-attachments/assets/d0817f56-1409-4837-966f-a59f98484165" />

## Indoor zone

A invisible **Bounding Box** which upon player's contact, triggers a movement speed limit inside the box. Recommended for tight areas.

## Light

Invisible entity, emitting shadow-less omnidirectional light inside a defined range on visible entities.

In light properties, user can define:

- Color in RGB
- Range
- Brightness or Shadow strength
- **Affects entities:** Enables static entity lighting with linear attenuation to **Range**
- **Use Bounding Box:** Tests overlap with entity's bounds against **Bounding Box**
- **Shadow Volume:** Makes **Light** emit darkness instead

<img width="766" height="543" alt="Light properties" src="https://github.com/user-attachments/assets/d336cc4b-4d7c-4b7f-af85-a52bc69ee59d" />

## Ambience

A sound stream of level's environment. Sniper Elite fetches ambience sound files from game's root in **Bink audio (`.bik`)** format.

<img width="526" height="228" alt="Ambience properties" src="https://github.com/user-attachments/assets/27711479-9a8e-455d-9b00-ed56521cf299" />

In levels you can specify default ambience played everywhere with it's default volume or their predefined **Ambience region** bounding boxes.

<img width="526" height="371" alt="Ambience region properties" src="https://github.com/user-attachments/assets/68faec36-7dca-40f9-a27d-c9cbc6d69fbb" />

Regions affect default ambience by either changing it's sound stream or volume when player's camera overlap one of them. Their outer box defines volume fade size.
Both inner and outer box the same size means **no fade**

## Collision barrier

Invisible level's collision faces, containing material index and a collision flag.

Editor defines them as **Bounding Box** and automatically overwrites material index as `Wrecked Car (8)` and coll flag `Ignore bullets & grenades only (0x200)`

# Asura Skybox

In skybox properties, user can define:

- Color tint in RGB
- Orientation (Rotation) around the level
- Face placement

User can specify his own skybox textures via specifying their folder or use imported internal .PC skybox.

<img width="816" height="558" alt="Skybox properties" src="https://github.com/user-attachments/assets/06fe0840-b3bc-4191-b805-bd38f108f9f7" />

> Skybox texture paths by default need to be internally placed inside 'graphics\sky' directory. 
> Level Editor already takes care of it, but removing '\sky\' from their path will make Asura Engine omit Skybox textures.

# Weather

'Rain' checkmark toggles rain particles. Their texture gets loaded from game's directory, depending on which level the game loader takes. (mp_01a uses rain_01a.dds etc.)

To get level geometry react to rain droplets, it's material needs to have assigned blending flag 'Affected by rain (0x4000)' in material map.

# Editing original levels

**Asura Level Editor** offers availability of importing stock .PC levels from game's root folder, which allows preview and modification of their content Level Editor can parse.

> Level Editor is mainly designed for multiplayer levels, so some in-game stuff from single-player ones might be missing in editor itself.
