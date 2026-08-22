#pragma once
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
    uint32_t m_uMaterialResponseHashID;
    uint32_t m_uLowestVertexUsed;
    uint32_t m_uNumberOfVertices;
};

struct Asura_PC_EnvironmentRenderer_Vertex {
    Asura_Vector_3 m_xPosition;
    Asura_Vector_3 m_xNormal;
    uint32_t m_uDiffuse;
    Asura_Vector_2 m_xUV;
};

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
    Asura_Vector_3 Direction;
    float R;
    float G;
    float B;
    float Brightness;
    float Range;
    float m_fInnerRange;
    float Angle;
    float ShadowStrength;
    Asura_Bounding_Box m_xBoundingBox;
    uint32_t m_uFlags;
    float BrightnessOverRange;
    Asura_Vector_3 OldPosition;
    float OldRange;
    bool HasChanged;
};

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

struct Asura_Chunk_Entity_PayloadHeader {
    uint32_t Guid;
    uint16_t Classification;
    uint16_t m_usPadding;
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
