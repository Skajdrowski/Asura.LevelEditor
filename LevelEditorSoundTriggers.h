#pragma once
#include "LevelEditorDocument.h"
#include <utility>

namespace editor {

struct SoundTriggerExport {
    std::vector<uint8_t> messages;
    std::vector<uint8_t> entities;
    std::vector<uint32_t> replaced_guids;
    std::vector<std::pair<uint32_t, uint32_t>> controller_trigger_guids;
    bool replace_message_set_zero = false;
};

Asura_Bounding_Box sound_trigger_bounds(const Entity& sound);
bool prepare_sound_triggers(const Document& document, const asura::level::ChunkList* source,
                            SoundTriggerExport* output, asura::Error* error);
void import_sound_triggers(const asura::level::ChunkList& chunks, Document* document);
bool replaces_sound_trigger_chunk(const asura::level::ChunkRef& chunk,
                                  const SoundTriggerExport& output);
bool append_sound_trigger_export(asura::Buffer* out, const SoundTriggerExport& prepared,
                                 asura::Error* error);

} // namespace editor
