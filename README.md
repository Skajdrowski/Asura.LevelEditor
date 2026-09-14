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

# Asura entities

There are 5 server-side entities: Spawn, Object, Pickup, Sound, Indoor Zone and 1 client-side entity Light.

## Spawn

Player spawnpoints. Level Editor can render them in viewport as T-Posing characters, depending on their team assignment.

There are 4 team choices: Teamless (Deathmatch specific), Germany, Russia and Camera-only (Used for screen before player spawns)

Spawns also have their **Game mode gate**, which allows to create game mode exclusive spawnpoints.

<img width="270" height="222" alt="Spawn properties" src="https://github.com/user-attachments/assets/8e0f9e4f-89cc-42fa-b697-585275ac5ec7" />


> Level Editor lists **Single-Player** and **Cooperative** game modes to preserve their values from stock .PC levels.
>
> They shouldn't be used for **Multiplayer levels**.

## Object

Static geometry objects. They get affected by **Light** entities and they do not participate in prebaked vertex lighting. They can be used as a collision hotfix for remote players who play on outdated level.

Objects can be exported to **Wavefront OBJ** when selected, so they can be separated from Entities and set up into level geometry with their lighting baked.

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

<img width="546" height="431" alt="Audio properties" src="https://github.com/user-attachments/assets/d0817f56-1409-4837-966f-a59f98484165" />

## Indoor zone

A invisible **Bounding Box** which upon player's contact, triggers a movement speed limit inside the box. Recommended for tight areas.

## Light

Invisible entity, emitting shadow-less omnidirectional light inside a defined range on visible entities.

In light properties, user can define:

- Color in RGB
- Range
- Brightness/Shadow strength
- **Affects entities:** Enables static entity lighting with linear attenuation to Range.
- **Use Bounding Box:** Tests overlap with entity's bounds.
- **Shadow Volume:** Makes **Light** emit darkness instead.

<img width="766" height="543" alt="Light properties" src="https://github.com/user-attachments/assets/d336cc4b-4d7c-4b7f-af85-a52bc69ee59d" />

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
