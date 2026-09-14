#pragma once
#include "LevelEditorDocument.h"

namespace editor {

struct SoundTriggerExport {
    std::vector<uint8_t> messages;
    std::vector<uint8_t> entities;
};

Asura_Bounding_Box sound_trigger_bounds(const Entity& sound);
bool prepare_sound_triggers(const Document& document, SoundTriggerExport* output, asura::Error* error);
void import_sound_triggers(const asura::level::ChunkList& chunks, Document* document);
bool append_sound_trigger_export(asura::Buffer* out, const SoundTriggerExport& prepared,
                                 asura::Error* error);

} // namespace editor
