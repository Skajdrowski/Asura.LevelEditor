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

bool prepare_sound_triggers(const Document& doc, const ChunkList* source,
                            SoundTriggerExport* out, Error* err) {
    *out = {};
    bool needed = false;
    for (const auto& e : doc.entities)
        needed |= e.kind == EntityKind::Sound && (e.sound_trigger_enabled || e.sound_trigger_source_guid);
    if (!needed && !source) return true;
    Blocks blocks;
    std::set<uint32_t> used, source_sounds;
    bool found_set = false;
    if (source) for (uint32_t i = 0; i < source->count; ++i) {
        const auto& c = source->chunks[i];
        if (c.cid == ASURA_CHUNK_ENTITY && c.size >= 24) used.insert(read_u32(c.data + 16));
        if (c.cid == ASURA_CHUNK_ENTITY && c.size >= 40 &&
            read_u16(c.data + 20) == AsuraEntityClass_SoundController)
            source_sounds.insert(read_u32(c.data + 16));
        if (c.cid == ASURA_CHUNK_STATICMESSAGES && message_set(c) == UINT32_MAX) {
            if (!needed) return true;
            return fail(err, "Cannot extend a malformed static-message chunk.");
        }
        if (message_set(c) == 0) {
            if (found_set || !read_blocks(c, &blocks)) {
                if (!needed) return true;
                return fail(err, "Cannot extend this level's static-message set safely (duplicate or unsupported data).");
            }
            found_set = true;
        }
    }
    // A deleted source sound no longer carries its trigger GUID in Document.
    // Remove only the same simple, sound-only trigger shape that import owns;
    // arbitrary game scripts remain untouched and existing blocks stay valid.
    if (source) for (uint32_t i = 0; i < source->count; ++i) {
        WireTrigger t;
        if (!read_trigger(source->chunks[i], &t) || t.enter >= blocks.size() ||
            blocks[t.enter].size() != 1) continue;
        const uint32_t sound_guid = blocks[t.enter][0].to;
        if (!source_sounds.count(sound_guid) || !matches_sound(blocks, t, sound_guid)) continue;
        const bool exists = std::any_of(doc.entities.begin(), doc.entities.end(), [&](const Entity& e) {
            return e.kind == EntityKind::Sound && e.guid == sound_guid;
        });
        if (!exists) { out->replaced_guids.push_back(t.guid); needed = true; }
    }
    if (!needed) return true;
    for (const auto& e : doc.entities) used.insert(e.guid);
    uint32_t candidate = kToolCreatedGuidFirst;
    for (const auto& e : doc.entities) {
        if (e.kind != EntityKind::Sound) continue;
        WireTrigger previous;
        bool replace = false;
        if (source && e.sound_trigger_source_guid) for (uint32_t i = 0; i < source->count; ++i) {
            WireTrigger t;
            if (read_trigger(source->chunks[i], &t) && t.guid == e.sound_trigger_source_guid &&
                matches_sound(blocks, t, e.guid)) { previous = t; replace = true; break; }
        }
        if (e.sound_trigger_source_guid && source && !replace)
            return fail(err, "Sound '%s': its original trigger or messages changed; reopen the source level.", e.name.c_str());
        if (replace) out->replaced_guids.push_back(previous.guid);
        if (!e.sound_trigger_enabled) continue;
        if (!e.guid || e.guid == 999 || !finite_bounds(sound_trigger_bounds(e)))
            return fail(err, "Sound '%s': a valid controller GUID and positive, finite trigger size are required.", e.name.c_str());
        if (e.sound_trigger_once && e.sound_stop_on_exit)
            return fail(err, "Sound '%s': Stop on exit requires a trigger that rearms after exit.", e.name.c_str());
        // Append fresh blocks: existing blocks can also be referenced by unrelated scripts.
        // Reuse our previous blocks only if their contents already match the action.
        auto block_for = [&](uint16_t old, uint16_t id) -> uint16_t {
            if (replace && single_message(blocks, old, id, e.guid)) return old;
            if (blocks.size() >= kMaxBlocks) return kEmptyBlock;
            const uint16_t index = static_cast<uint16_t>(blocks.size());
            blocks.push_back({Message{id, e.guid, {}}}); return index;
        };
        const uint16_t enter = block_for(previous.enter, 5);
        const uint16_t exit = e.sound_stop_on_exit ? block_for(previous.exit, 4) : kEmptyBlock;
        if (enter == kEmptyBlock || (e.sound_stop_on_exit && exit == kEmptyBlock))
            return fail(err, "The level has no free static-message blocks for a sound trigger.");
        uint32_t guid = previous.guid;
        if (!replace) {
            while (candidate <= kToolCreatedGuidLast && used.count(candidate)) ++candidate;
            if (candidate > kToolCreatedGuidLast) return fail(err, "No free GUID remains for a sound trigger.");
            guid = candidate++; used.insert(guid);
        }
        write_trigger(e, guid, enter, exit, &out->entities);
        out->controller_trigger_guids.emplace_back(e.guid, guid);
    }
    out->replace_message_set_zero = true;
    write_blocks(blocks, &out->messages);
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

bool replaces_sound_trigger_chunk(const ChunkRef& c, const SoundTriggerExport& out) {
    if (out.replace_message_set_zero && message_set(c) == 0) return true;
    return c.cid == ASURA_CHUNK_ENTITY && c.size >= 24 &&
           std::find(out.replaced_guids.begin(), out.replaced_guids.end(), read_u32(c.data+16)) != out.replaced_guids.end();
}
bool append_sound_trigger_export(Buffer* out, const SoundTriggerExport& prepared, Error* err) {
    if (!prepared.messages.empty()) buffer_append(out, prepared.messages.data(), prepared.messages.size(), err);
    if (!prepared.entities.empty()) buffer_append(out, prepared.entities.data(), prepared.entities.size(), err);
    return !err->set;
}
} // namespace editor
