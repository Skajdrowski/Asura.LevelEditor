#pragma once
#include <cstddef>
#include "AsuraEnums.h"

struct Asura_Vector_2 {
    float x, y;
};

struct Asura_Vector_3 {
    float x, y, z;
};

struct Asura_RGBA {
    float m_fR, m_fG, m_fB, m_fA;
};

struct Asura_Quat {
    float x, y, z, w;
};

struct Asura_Bounding_Box {
    float MinX;
    float MaxX;
    float MinY;
    float MaxY;
    float MinZ;
    float MaxZ;
};

struct Asura_Chunk_Header {
    union {
        ASURA_CHUNKID ID;
        char TextID[4];
    };
    int32_t Size;
    int32_t Version;
    int32_t Flags;
};

struct Asura_Chunk_ResourceFile : Asura_Chunk_Header {
    uint32_t Type;
    uint32_t SubType;
    uint32_t FileSize;
};

struct Asura_Chunk_Environment_ModuleList : Asura_Chunk_Header {
    int32_t m_iNumberOfModules;
};

struct Asura_Chunk_Navigation : Asura_Chunk_Header {
    int32_t m_iNumWaypoints;
    int32_t m_iNumCoverpoints;
};

struct Asura_Chunk_Entity : Asura_Chunk_Header {
    uint32_t Guid;
    uint16_t Classification;
    uint16_t m_usPadding;
};

struct Asura_Chunk_StaticMessages : Asura_Chunk_Header {
    int32_t NumberOfMessageBlocks;
};

struct Asura_Chunk_TextureNames : Asura_Chunk_Header {
    int32_t NumberOfTextures;
};

struct Asura_Chunk_TextureFlags : Asura_Chunk_Header {
    int32_t NumberOfTextures;
};

// Sniper Elite 2005 PC MTRL v1 wire entry. The target reads this exact
// 12-byte record in Asura_Chunk_Material::Process (0x440440), converts the
// serialized texture index through the active TEXT table, and exposes the low
// byte of m_uProjectFlags to gameplay material-response queries.
struct Asura_PC_Material_V1 {
    int32_t m_iOriginalTextureIndex;
    uint32_t m_uFlags;
    // Exact MCP1/PDB field name. On the 2005 target, gameplay material-
    // response queries interpret its low byte as a surface/material type.
    uint32_t m_uProjectFlags;
};

struct Asura_Chunk_ResourceFileList : Asura_Chunk_Header {
    uint32_t uNumEntries;
};

struct Asura_Chunk_StreamingBackgroundSound : Asura_Chunk_Header {
    int32_t m_iNumberOfSounds;
    int32_t m_uSBSFlags;
};

struct Asura_Chunk_SkyBox : Asura_Chunk_Header {
    float m_fRed;
    float m_fGreen;
    float m_fBlue;
};

// The PC SKYB v7 payload begins with this fixed prefix.  It is followed by
// eight NUL-terminated texture paths, each padded to a four-byte boundary, and
// only then by Asura_Chunk_SkyBox_TrailingFlagsV7.  The paths are deliberately
// not represented as a fixed-size field here. Version 1 has no paths; v2 has
// six, v3/v4 seven, and v5-v7 eight. DrawClouds was introduced in v4, the
// newer face permutation in v6, and the cube-shaped permutation in v7. The
// PDB names for the final two fields describe their texture reuse exactly.
struct Asura_Chunk_SkyBox_PayloadPrefixV7 {
    float m_fRed;
    float m_fGreen;
    float m_fBlue;
    float m_fOrientationAroundYAxis;
};

struct Asura_Chunk_SkyBox_TrailingFlagsV7 {
    uint32_t m_bDrawClouds;
    uint32_t m_bBackTextureIsFrontUpsideDown;
    uint32_t m_bRightTextureIsLeftUpsideDown;
};

enum : uint32_t {
    ASURA_SKYBOX_V2_TEXTURE_PATH_COUNT = 6,
    ASURA_SKYBOX_V3_V4_TEXTURE_PATH_COUNT = 7,
    ASURA_SKYBOX_V5_V7_TEXTURE_PATH_COUNT = 8
};

static_assert(sizeof(Asura_Chunk_SkyBox_PayloadPrefixV7) == 0x10,
              "IDA-recovered SKYB v7 fixed prefix changed");
static_assert(sizeof(Asura_Chunk_SkyBox_TrailingFlagsV7) == 0x0C,
              "IDA-recovered SKYB v7 trailing flags changed");

struct Asura_Environment_Module {
    uint32_t m_uRegion;
    uint32_t m_uReqdRegions;
    uint32_t m_uModuleFlags;
};

// Sniper Elite 2005 PC environment-renderer wire records. The PDB-rich build
// has the corresponding Xbox360 renderer types, but their layouts differ.
struct Asura_PC_EnvironmentRenderer_Module {
    uint32_t m_uNumberOfStrips;
    uint32_t m_uFirstStrip;
    uint32_t m_uBufferIndex;
};

struct Asura_PC_EnvironmentRenderer_Strip {
    uint32_t m_uNumberOfTriangles;
    uint32_t m_uStartIndex;
    int32_t m_iOriginalMaterialIndex;
    uint32_t m_uLowestVertexUsed;
    uint32_t m_uNumberOfVertices;
};

struct Asura_PC_EnvironmentRenderer_Vertex {
    Asura_Vector_3 m_xPosition;
    Asura_Vector_3 m_xNormal;
    uint32_t m_uDiffuse;
    Asura_Vector_2 m_xUV;
};

static_assert(sizeof(Asura_PC_EnvironmentRenderer_Module) == 0x0C,
              "IDA-recovered PC Env module layout changed");
static_assert(sizeof(Asura_PC_EnvironmentRenderer_Strip) == 0x14,
              "IDA-recovered PC Env strip layout changed");
static_assert(offsetof(Asura_PC_EnvironmentRenderer_Strip, m_iOriginalMaterialIndex) == 0x08,
              "PC Env original-material index moved");
static_assert(sizeof(Asura_PC_EnvironmentRenderer_Vertex) == 0x24,
              "IDA-recovered PC Env vertex layout changed");
static_assert(offsetof(Asura_PC_EnvironmentRenderer_Vertex, m_uDiffuse) == 0x18,
              "PC Env packed diffuse colour moved");

struct Asura_Chunk_Environment_ModuleList_EntryV6 {
    Asura_Vector_3 m_xTranslation;
    Asura_Environment_Module m_xModule;
    uint32_t m_uCollisionDataSize;
};

// Version-zero FOG is RGBA plus four scalar parameters.  The 2005 loader reads
// all 32 bytes; later PDB symbols identify the scalar meanings.
struct Asura_Chunk_Fog_ChunkDataV0 {
    Asura_RGBA xColour;
    float fNearPlane;
    float fFarPlane;
    float fValueAtFarPlane;
    float fSkyboxValue;
};

struct OldVersionExtraHeaderData {
    uint32_t uNumShelters;
    uint32_t auPad[5];
};

struct Asura_Chunk_WeatherSystem_ChunkDataV6 {
    uint32_t s_uSunFlags;
    Asura_Vector_3 s_xSunPos;
    float s_fSunColourR;
    float s_fSunColourG;
    float s_fSunColourB;
    float m_afLegacyParameters[12];
};

struct Asura_Light {
    Asura_Vector_3 Position;
    // Retained on disk for authoring compatibility.  The 2005 PC static-photon
    // path is an omnidirectional point light and does not consume Direction.
    Asura_Vector_3 Direction;
    float R;
    float G;
    float B;
    float Brightness;
    float Range;
    // InnerRange and Angle are likewise not used by that PC point-light path.
    float m_fInnerRange;
    float Angle;
    float ShadowStrength;
    Asura_Bounding_Box m_xBoundingBox;
    uint32_t m_uFlags;
    // Runtime cache/derived field. Asura_Light::Set deliberately skips +0x54,
    // so a serialized value is not authoritative and should not be edited as
    // an independent brightness control.
    float BrightnessOverRange;
    Asura_Vector_3 OldPosition;
    float OldRange;
    bool HasChanged;
};
static_assert(sizeof(Asura_Light) == 0x6C, "IDA-recovered Asura_Light layout changed");
static_assert(offsetof(Asura_Light, Direction) == 0x0C, "Asura_Light::Direction moved");
static_assert(offsetof(Asura_Light, Brightness) == 0x24, "Asura_Light::Brightness moved");
static_assert(offsetof(Asura_Light, Range) == 0x28, "Asura_Light::Range moved");
static_assert(offsetof(Asura_Light, m_xBoundingBox) == 0x38, "Asura_Light bounds moved");
static_assert(offsetof(Asura_Light, m_uFlags) == 0x50, "Asura_Light flags moved");
static_assert(offsetof(Asura_Light, BrightnessOverRange) == 0x54,
              "Asura_Light derived brightness cache moved");

struct Asura_Chunk_Phonons_PhononDataV9 {
    uint32_t m_uSoundResourceID;
    Asura_Vector_3 m_xPosition;
    float m_fInnerRadius;
    float m_fOuterRadius;
    float m_afLegacyVolumeParameters[7];
    uint32_t m_uFlags;
    Asura_Vector_3 m_xInnerCuboidRadius;
    Asura_Vector_3 m_xOuterCuboidRadius;
    uint32_t m_uGuid;
    Asura_Bounding_Box m_xRetriggerBoundingBox;
    Asura_Quat m_xOrient;
};
static_assert(sizeof(Asura_Chunk_Phonons_PhononDataV9) == 0x7C,
              "IDA-recovered phonon v9 layout changed");
static_assert(offsetof(Asura_Chunk_Phonons_PhononDataV9, m_uFlags) == 0x34,
              "PHON v9 flags moved");
static_assert(offsetof(Asura_Chunk_Phonons_PhononDataV9, m_xRetriggerBoundingBox) == 0x54,
              "PHON v9 retrigger bounds moved");
static_assert(offsetof(Asura_Chunk_Phonons_PhononDataV9, m_xOrient) == 0x6C,
              "PHON v9 orientation moved");

struct Asura_Chunk_Entity_PayloadHeader {
    uint32_t Guid;
    uint16_t Classification;
    uint16_t m_usPadding;
};

// Sniper Elite 2005 physical-object wire data.  The target executable's
// version-7 reader consumes this exact 68-byte predecessor of the later
// ChunkDataV10 record present in the PDB-rich reference build.
struct Asura_ServerEntity_PhysicalObject_ChunkDataV7 {
    Asura_Vector_3 m_xPosition;
    Asura_Quat m_xOrientation;
    float m_fHealth;
    uint32_t m_uFileID;
    uint32_t m_uSkinID;
    uint32_t m_uAnimID;
    uint32_t m_uAnimFileID;
    int32_t m_iAnimFlags;
    int32_t m_iBBIndex;
    uint32_t m_uStateBits;
    uint32_t m_uPhysicalObjectFlags;
    float m_fAnimTimer;
};

// ENTI classification 0x0008 payload, excluding the common eight-byte ENTI
// GUID/classification header.  Its writer is a chain of Pickup v2, static
// object v3, Snipe physical object v7, and Asura physical object v7 records.
struct Snipe_ServerEntity_Pickup_ChunkDataV0 {
    int32_t m_iPickupVersion;
    uint32_t m_uPickupClassID;
    uint32_t m_uPickupFlags;
    int32_t m_iAsuraPickupVersion;
    uint32_t m_uItemID;
    uint32_t m_uPickupPropertyA;
    uint32_t m_uPickupPropertyB;
    int32_t m_iStaticObjectVersion;
    uint32_t m_uStaticObjectFlags;
    int32_t m_iAsuraStaticObjectVersion;
    int32_t m_iPhysicalObjectVersion;
    uint32_t m_uTeam;
    uint32_t m_uSnipePhysicalFlags;
    uint32_t m_uSnipePhysicalPropertyA;
    uint32_t m_uSnipePhysicalPropertyB;
    uint32_t m_uSnipePhysicalPropertyC;
    uint32_t m_uSnipePhysicalPropertyD;
    uint32_t m_uSnipePhysicalPropertyE;
    int32_t m_iAsuraPhysicalObjectVersion;
    Asura_ServerEntity_PhysicalObject_ChunkDataV7 m_xPhysicalObject;
    uint32_t m_uNumLinksToBlock;
};

struct Asura_ServerEntity_SoundController_ChunkDataV0 {
    Asura_Chunk_Entity_PayloadHeader m_xEntity;
    int32_t m_iActivatableVersion;
    uint32_t m_bActive;
    int32_t m_iVersion;
    uint32_t m_uPhononGuid;
};

struct Snipe_ServerEntity_SpawnPoint_ChunkDataV0 {
    Asura_Chunk_Entity_PayloadHeader m_xEntity;
    int32_t m_iVersion;
    Asura_Vector_3 m_xPosition;
    Asura_Vector_3 m_xDirection;
    int32_t m_iSpawnIndex;
    int32_t m_iPosture;
    uint32_t m_uTeamMask;
    uint32_t m_uGameModeMask;
    float m_fSpawnTimer;
};
static_assert(sizeof(Snipe_ServerEntity_SpawnPoint_ChunkDataV0) == 0x38,
              "IDA-recovered spawn-point v0 layout changed");
static_assert(offsetof(Snipe_ServerEntity_SpawnPoint_ChunkDataV0, m_xDirection) == 0x18,
              "spawn camera-forward vector moved");
static_assert(offsetof(Snipe_ServerEntity_SpawnPoint_ChunkDataV0, m_uTeamMask) == 0x2C,
              "spawn team mask moved");
static_assert(offsetof(Snipe_ServerEntity_SpawnPoint_ChunkDataV0, m_uGameModeMask) == 0x30,
              "spawn game-mode mask moved");
static_assert(offsetof(Snipe_ServerEntity_SpawnPoint_ChunkDataV0, m_fSpawnTimer) == 0x34,
              "spawn timer moved");
