# Creating a New Level

To create a brand-new level from scratch, first design your level geometry and assign it's materials with .DDS texture format in **Blender** and export it as a **Wavefront OBJ (`.obj`)** file. The exported OBJ can then be opened in the **Asura Level Editor** and converted into a Sniper Elite compatible `.PC` level.

The **Asura Material Map** Blender extension assigns Asura material indices, texture names, surface types, blending flags and collision flags to export the material map used by Asura levels.

> Blender is the only supported application for the material workflow.
>
> Although **Asura Level Editor** reads standard Wavefront OBJ geometry, material setups created in other 3D modelling applications are not supported.

The usual workflow for a new level is:

1. Create and texture the level geometry in **Blender**.
2. Configure the materials using the **Asura Material Map** extension.
3. Export the level geometry as a **Wavefront OBJ**.
4. Export the corresponding **material map** via Blender addon.
5. Open the OBJ inside **Asura Level Editor**.
6. Import material map and select the texture folder containing textures used by your level geometry in **.DDS format**.
7. Select weapons donor .PC for weapon models and pickups.
8. Add gameplay entities and other level properties.
9. Export the finished level and overwrite one of the Sniper Elite **`.PC`**  multiplayer levels.

<img width="1920" height="1032" alt="Level about to be exported." src="https://github.com/user-attachments/assets/e76a60f8-0749-4a6a-b96f-e1fd5403da26" />


You can save your work into Level Editor's exclusive **.alev** project format and comeback to it later.

# Asura entities

There are 5 server-side entities: Spawn, Object, Pickup, Sound, Indoor Zone and 1 client-side entity Light.

> Server-side means they will appear for remote players independently of their currently loaded level.
> While client-side means remote players must have a matching level with the host's currently loaded level. Otherwise inconsistencies might occur.

> **Note:** Server-sided entities have internal limit of **99999** entities per map. Not generally proven, but exceeding this limit might carry **fatal consequences to currently hosted game**.

## Spawn

Player spawnpoints. Level Editor can render them in viewport as T-Posing characters, depending on their team assignment.

There are 4 team choices: Teamless (Deathmatch-only), Germany, Russia and Camera-only (Used for screen before player spawns)

Spawns also have their **Game mode gate**, which allows to create game mode exclusive spawnpoints.

<img width="270" height="222" alt="Spawn properties" src="https://github.com/user-attachments/assets/8e0f9e4f-89cc-42fa-b697-585275ac5ec7" />


> Level Editor lists **Single-Player** and **Cooperative** game modes for compatibility with stock .PC levels.
> They shouldn't be used for **Multiplayer levels**.

## Object

Static geometry objects. Not really differentiating from level geometry, but could be used as a collision hotfix for remote players who play on outdated level.

Objects can be exported to **Wavefront OBJ** when selected, so they can be set up into level geometry.

## Pickup

**Pickup** entities are collectible inventory items, such as:

- Medkits
- Rifle ammunition
- PPSH submachine guns

## Sound

**Sound** entities are invisible sound controllers.

They can play custom **PCM (`.wav`)** audio within a defined range around player.

The sound's position and audible range can be configured in the Level Editor, but their scripted trigger activation is not yet reversed

## Indoor zone

A invislbe **Bounding Box** which upon player's contact, triggers a movement speed limit. Recommended for tight areas.

## Light

Invisible entity, emitting shadow-less omnidirectional light inside a defined range on visible entities.

<img width="766" height="543" alt="Light properties" src="https://github.com/user-attachments/assets/2fedc7f8-1ccc-455f-bdda-93de913cd0b9" />


In light properties, user can define: 

- Color in RGB
- Range
- Brightness/Shadow strength
- **Bounding Box**, which limits it's emission inside the defined box.
- **Shadow Volume**, which makes **Light** emit darkness instead.

> The rest of property options aren't fully reversed yet. Meaning most of current options have less to no effect in-game.

> Levels need to have atleast one **Light** entity, otherwise every visible in-game entity will be black.

# Asura Skybox

> Asura Engine renders Skybox as a big cube. Though some texture dimensions can change it's geometry, which isn't well reversed yet.

In skybox properties, user can define:

- Color tint in RGB
- Orientation (Rotation) around the level
- Skybox internal chunk versioning (This might get obsoleted in the future)

User can specify his own skybox textures via specifying their folder or use imported .PC skybox.

> Skybox texture paths by default need to be internally placed inside 'graphics\sky' directory. 
> Level Editor already takes care of it, but removing '\sky\' from their path will make Asura Engine omit Skybox textures.

<img width="816" height="558" alt="Skybox properties" src="https://github.com/user-attachments/assets/06fe0840-b3bc-4191-b805-bd38f108f9f7" />


# Editing original levels

**Asura Level Editor** offers availability of importing stock .PC levels from game's root folder, which allows modification of their content, but only things Level Editor can parse.

> Level Editor is mainly designed for multiplayer levels, so some in-game stuff from them might be missing in editor itself.
