#include "LevelEditorSoundTriggers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

using namespace asura;
using namespace asura::level;

namespace editor {
namespace {
constexpr uint16_t kVolumeClass = 0x8010; // MCP2 factory 0x5A5106, ctor 0x594880.
constexpr uint16_t kEmptyBlock = 0xffff;
constexpr size_t kMaxBlocks = 30000; // MCP2 0x459850: set 0 [0,29999], set 1 [30000,59999].
struct Message { uint16_t id = 0; uint32_t to = 0; std::vector<uint8_t> data; };
using Blocks = std::vector<std::vector<Message>>;

template<class T> void emit(std::vector<uint8_t>* out, const T& value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    out->insert(out->end(), p, p + sizeof(value));
}
struct Reader {
    const uint8_t* p;
    size_t left;
    template<class T> bool get(T* value) {
        if (left < sizeof(T)) return false;
        memcpy(value, p, sizeof(T)); p += sizeof(T); left -= sizeof(T); return true;
    }
};
uint32_t message_set(const ChunkRef& chunk) {
    if (chunk.cid != ASURA_CHUNK_STATICMESSAGES || chunk.size < 20) return UINT32_MAX;
    if (chunk.version < 2) return 0;
    return chunk.size >= 24 ? read_u32(chunk.data + 20) : UINT32_MAX;
}
bool read_blocks(const ChunkRef& chunk, Blocks* blocks) {
    if (chunk.version > 2 || chunk.size < 20) return false;
    const uint32_t count = read_u32(chunk.data + 16);
    const size_t start = chunk.version == 2 ? 24 : 20;
    if (count > kMaxBlocks || chunk.size < start || count > chunk.size - start) return false;
    const uint8_t* counts = chunk.data + start;
    Reader r{counts + count, chunk.size - start - count};
    blocks->resize(count);
    for (size_t b = 0; b < count; ++b) {
        for (uint32_t i = 0; i < counts[b]; ++i) {
            Message msg;
            uint16_t size;
            if (!r.get(&msg.id) || !r.get(&size) || !r.get(&msg.to)) return false;
            // v0's second word is discarded, not a payload size (0x43B3BA).
            if (chunk.version == 0) size = 0;
            const size_t padded = (size + 3u) & ~3u;
            if (r.left < padded) return false;
            msg.data.assign(r.p, r.p + size);
            r.p += padded; r.left -= padded;
            (*blocks)[b].push_back(std::move(msg));
        }
    }
    // A chunk may end with alignment padding, never an unparsed message.
    return r.left <= 3;
}
void finish_chunk(std::vector<uint8_t>* bytes) {
    while (bytes->size() & 3) bytes->push_back(0);
    const uint32_t size = static_cast<uint32_t>(bytes->size());
    memcpy(bytes->data() + 4, &size, 4);
}
void write_blocks(const Blocks& blocks, std::vector<uint8_t>* out) {
    emit(out, Asura_Chunk_Header{ASURA_CHUNK_STATICMESSAGES, 0, 2, 0});
    emit(out, static_cast<uint32_t>(blocks.size())); emit(out, uint32_t{0});
    for (const auto& block : blocks) out->push_back(static_cast<uint8_t>(block.size()));
    for (const auto& block : blocks) for (const auto& msg : block) {
        emit(out, msg.id); emit(out, static_cast<uint16_t>(msg.data.size())); emit(out, msg.to);
        out->insert(out->end(), msg.data.begin(), msg.data.end());
        // Payload padding is based on payload length, not absolute stream offset.
        out->insert(out->end(), (4 - (msg.data.size() & 3)) & 3, 0);
    }
    finish_chunk(out);
}
struct WireTrigger {
    uint32_t guid = 0;
    uint16_t enter = kEmptyBlock, exit = kEmptyBlock;
    Asura_Bounding_Box bounds{};
    uint32_t flags = 0;
};
bool read_trigger(const ChunkRef& chunk, WireTrigger* t) {
    // ENTI header + GUID/class + Activatable v2 + Trigger v4 + Snipe volume v3.
    if (chunk.cid != ASURA_CHUNK_ENTITY || chunk.version != 0 || chunk.size != 88 ||
        read_u16(chunk.data + 20) != kVolumeClass || read_u32(chunk.data + 24) != 2 ||
        read_u32(chunk.data + 28) != 1 || read_u32(chunk.data + 32) != 4 ||
        read_u32(chunk.data + 48) != 3 || read_u32(chunk.data + 76) != 0 ||
        read_u32(chunk.data + 80) != 999) return false;
    if (read_u16(chunk.data + 36) != kEmptyBlock || read_u16(chunk.data + 38) != kEmptyBlock ||
        read_u16(chunk.data + 42) != kEmptyBlock) return false;
    t->guid = read_u32(chunk.data + 16);
    t->enter = read_u16(chunk.data + 40); t->exit = read_u16(chunk.data + 44);
    memcpy(&t->bounds, chunk.data + 52, 24);
    t->flags = read_u32(chunk.data + 84);
    return (t->flags & ~2u) == 0; // Only the once-per-level flag is authored here.
}
bool single_message(const Blocks& blocks, uint16_t index, uint16_t id, uint32_t guid) {
    return index < blocks.size() && blocks[index].size() == 1 &&
           blocks[index][0].id == id && blocks[index][0].to == guid && blocks[index][0].data.empty();
}
bool matches_sound(const Blocks& blocks, const WireTrigger& trigger, uint32_t guid) {
    return single_message(blocks, trigger.enter, 5, guid) &&
           (trigger.exit == kEmptyBlock || single_message(blocks, trigger.exit, 4, guid));
}
void write_trigger(const Entity& sound, uint32_t guid, uint16_t enter, uint16_t exit,
                   std::vector<uint8_t>* out) {
    std::vector<uint8_t> bytes;
    emit(&bytes, Asura_Chunk_Header{ASURA_CHUNK_ENTITY, 88, 0, 0});
    emit(&bytes, Asura_Chunk_Entity_PayloadHeader{guid, kVolumeClass, 0});
    emit(&bytes, uint32_t{2}); emit(&bytes, uint32_t{1}); // Activatable: starts active.
    emit(&bytes, uint32_t{4});
    for (uint16_t block : {kEmptyBlock, kEmptyBlock, enter, kEmptyBlock, exit, uint16_t{0}})
        emit(&bytes, block);
    emit(&bytes, uint32_t{3}); emit(&bytes, sound_trigger_bounds(sound));
    emit(&bytes, uint32_t{0}); // Any living player, server list (0x594C6A).
    emit(&bytes, uint32_t{999}); // No specific entity.
    emit(&bytes, sound.sound_trigger_once ? uint32_t{2} : uint32_t{0});
    out->insert(out->end(), bytes.begin(), bytes.end());
}
bool finite_bounds(const Asura_Bounding_Box& b) {
    return std::isfinite(b.MinX) && std::isfinite(b.MaxX) && b.MinX < b.MaxX &&
           std::isfinite(b.MinY) && std::isfinite(b.MaxY) && b.MinY < b.MaxY &&
           std::isfinite(b.MinZ) && std::isfinite(b.MaxZ) && b.MinZ < b.MaxZ;
}
} // namespace

Asura_Bounding_Box sound_trigger_bounds(const Entity& e) {
    const auto& s = e.sound_trigger_size; const auto& o = e.sound_trigger_offset;
    return {e.position.x + o.x - s.x/2, e.position.x + o.x + s.x/2,
            e.position.y + o.y - s.y/2, e.position.y + o.y + s.y/2,
            e.position.z + o.z - s.z/2, e.position.z + o.z + s.z/2};
}

bool prepare_sound_triggers(const Document& doc, SoundTriggerExport* out, Error* err) {
    *out = {};
    Blocks blocks;
    std::set<uint32_t> used;
    for (const auto& entity : doc.entities) used.insert(entity.guid);
    uint32_t candidate = kToolCreatedGuidFirst;
    for (const auto& entity : doc.entities) {
        if (entity.kind != EntityKind::Sound || !entity.sound_trigger_enabled) continue;
        if (!entity.guid || entity.guid == 999 || !finite_bounds(sound_trigger_bounds(entity)))
            return fail(err, "Sound '%s': a valid controller GUID and positive, finite trigger size are required.", entity.name.c_str());
        if (entity.sound_trigger_once && entity.sound_stop_on_exit)
            return fail(err, "Sound '%s': Stop on exit requires a trigger that rearms after exit.", entity.name.c_str());
        const size_t needed = entity.sound_stop_on_exit ? 2 : 1;
        if (blocks.size() + needed > kMaxBlocks)
            return fail(err, "The level has no free static-message blocks for a sound trigger.");
        const uint16_t enter = static_cast<uint16_t>(blocks.size());
        blocks.push_back({Message{5, entity.guid, {}}});
        uint16_t exit = kEmptyBlock;
        if (entity.sound_stop_on_exit) {
            exit = static_cast<uint16_t>(blocks.size());
            blocks.push_back({Message{4, entity.guid, {}}});
        }
        while (candidate <= kToolCreatedGuidLast && used.count(candidate)) ++candidate;
        if (candidate > kToolCreatedGuidLast) return fail(err, "No free GUID remains for a sound trigger.");
        const uint32_t guid = candidate++;
        used.insert(guid);
        write_trigger(entity, guid, enter, exit, &out->entities);
    }
    if (!blocks.empty()) write_blocks(blocks, &out->messages);
    return true;
}
void import_sound_triggers(const ChunkList& source, Document* doc) {
    Blocks blocks; bool found = false;
    for (uint32_t i = 0; i < source.count; ++i) if (message_set(source.chunks[i]) == 0) {
        if (found || !read_blocks(source.chunks[i], &blocks)) return;
        found = true;
    }
    for (uint32_t i = 0; i < source.count; ++i) {
        WireTrigger t;
        if (!read_trigger(source.chunks[i], &t) || !finite_bounds(t.bounds)) continue;
        for (auto& e : doc->entities) {
            if (e.kind != EntityKind::Sound || !e.sound_has_controller || e.sound_trigger_enabled ||
                !matches_sound(blocks, t, e.guid) || ((t.flags & 2) && t.exit != kEmptyBlock)) continue;
            e.sound_trigger_enabled = true; e.sound_trigger_once = (t.flags & 2) != 0;
            e.sound_stop_on_exit = t.exit != kEmptyBlock; e.sound_trigger_source_guid = t.guid;
            const auto& b = t.bounds;
            e.sound_trigger_size = {b.MaxX-b.MinX, b.MaxY-b.MinY, b.MaxZ-b.MinZ};
            e.sound_trigger_offset = {(b.MinX+b.MaxX)/2-e.position.x, (b.MinY+b.MaxY)/2-e.position.y,
                                      (b.MinZ+b.MaxZ)/2-e.position.z};
            break;
        }
    }
}

bool append_sound_trigger_export(Buffer* out, const SoundTriggerExport& prepared, Error* err) {
    if (!prepared.messages.empty()) buffer_append(out, prepared.messages.data(), prepared.messages.size(), err);
    if (!prepared.entities.empty()) buffer_append(out, prepared.entities.data(), prepared.entities.size(), err);
    return !err->set;
}
} // namespace editor
