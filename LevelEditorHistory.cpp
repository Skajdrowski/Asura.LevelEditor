#include "LevelEditorHistory.h"

#include <algorithm>
#include <bit>
#include <iterator>
#include <utility>

namespace editor {
namespace {

bool equal(float a, float b) {
    return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
}

bool equal(const Asura_Vector_3& a, const Asura_Vector_3& b) {
    return equal(a.x, b.x) && equal(a.y, b.y) && equal(a.z, b.z);
}

bool equal(const Asura_Quat& a, const Asura_Quat& b) {
    return equal(a.x, b.x) && equal(a.y, b.y) && equal(a.z, b.z) && equal(a.w, b.w);
}

bool equal(const Asura_Bounding_Box& a, const Asura_Bounding_Box& b) {
    return equal(a.MinX, b.MinX) && equal(a.MaxX, b.MaxX) && equal(a.MinY, b.MinY) &&
           equal(a.MaxY, b.MaxY) && equal(a.MinZ, b.MinZ) && equal(a.MaxZ, b.MaxZ);
}

bool equal(const Asura_Light& a, const Asura_Light& b) {
    return equal(a.Position, b.Position) && equal(a.Direction, b.Direction) && equal(a.R, b.R) &&
           equal(a.G, b.G) && equal(a.B, b.B) && equal(a.Brightness, b.Brightness) &&
           equal(a.Range, b.Range) && equal(a.m_fInnerRange, b.m_fInnerRange) &&
           equal(a.Angle, b.Angle) && equal(a.ShadowStrength, b.ShadowStrength) &&
           equal(a.m_xBoundingBox, b.m_xBoundingBox) && a.m_uFlags == b.m_uFlags &&
           equal(a.BrightnessOverRange, b.BrightnessOverRange) &&
           equal(a.OldPosition, b.OldPosition) && equal(a.OldRange, b.OldRange) &&
           a.HasChanged == b.HasChanged;
}

bool equal(const Asura_Chunk_Phonons_PhononDataV9& a,
           const Asura_Chunk_Phonons_PhononDataV9& b) {
    if (a.m_uSoundResourceID != b.m_uSoundResourceID || !equal(a.m_xPosition, b.m_xPosition) ||
        !equal(a.m_fInnerRadius, b.m_fInnerRadius) ||
        !equal(a.m_fOuterRadius, b.m_fOuterRadius) ||
        a.m_uFlags != b.m_uFlags || !equal(a.m_xInnerCuboidRadius, b.m_xInnerCuboidRadius) ||
        !equal(a.m_xOuterCuboidRadius, b.m_xOuterCuboidRadius) || a.m_uGuid != b.m_uGuid ||
        !equal(a.m_xRetriggerBoundingBox, b.m_xRetriggerBoundingBox) ||
        !equal(a.m_xOrient, b.m_xOrient)) {
        return false;
    }
    for (size_t i = 0; i < std::size(a.m_afLegacyVolumeParameters); ++i) {
        if (!equal(a.m_afLegacyVolumeParameters[i], b.m_afLegacyVolumeParameters[i]))
            return false;
    }
    return true;
}

bool equal(const Entity& a, const Entity& b) {
    return a.kind == b.kind && a.name == b.name && equal(a.position, b.position) &&
           equal(a.rotation, b.rotation) && a.guid == b.guid && equal(a.value_a, b.value_a) &&
           equal(a.value_b, b.value_b) && a.value_u32_a == b.value_u32_a &&
           a.value_u32_b == b.value_u32_b && a.sound_name == b.sound_name &&
           a.sound_file == b.sound_file && a.sound_loop == b.sound_loop &&
           equal(a.light, b.light) && a.spawn_source_record == b.spawn_source_record &&
           a.spawn_index == b.spawn_index && a.spawn_posture == b.spawn_posture &&
           equal(a.spawn_timer, b.spawn_timer) && equal(a.spawn_direction, b.spawn_direction) &&
           a.entity_padding == b.entity_padding &&
           a.sound_source_record == b.sound_source_record &&
           a.sound_has_controller == b.sound_has_controller &&
           a.sound_controller_active == b.sound_controller_active &&
           a.sound_controller_padding == b.sound_controller_padding &&
           equal(a.sound_phonon, b.sound_phonon) &&
           a.source_entity_record == b.source_entity_record &&
           a.source_entity_classification == b.source_entity_classification &&
           equal(a.source_bounds, b.source_bounds) &&
           a.pickup_has_template == b.pickup_has_template &&
           a.pickup_skin_id == b.pickup_skin_id && a.pickup_anim_id == b.pickup_anim_id &&
           a.pickup_anim_file_id == b.pickup_anim_file_id && a.pickup_body == b.pickup_body &&
           a.static_object_has_template == b.static_object_has_template &&
           a.static_object_body == b.static_object_body &&
           a.barrier_source_record == b.barrier_source_record &&
           a.barrier_source_chunk == b.barrier_source_chunk &&
           a.barrier_source_module == b.barrier_source_module &&
           a.barrier_source_component == b.barrier_source_component &&
           a.barrier_collision_flags == b.barrier_collision_flags &&
           a.barrier_collision_material == b.barrier_collision_material;
}

bool equal(const PickupTemplate& a, const PickupTemplate& b) {
    return a.item_id == b.item_id && equal(a.health, b.health) && a.file_id == b.file_id &&
           a.skin_id == b.skin_id && a.anim_id == b.anim_id &&
           a.anim_file_id == b.anim_file_id && a.entity_padding == b.entity_padding &&
           a.body == b.body;
}

bool equal(const StaticObjectTemplate& a, const StaticObjectTemplate& b) {
    return a.file_id == b.file_id && a.resource_name == b.resource_name &&
           a.donor_path == b.donor_path && a.entity_padding == b.entity_padding &&
           a.body == b.body;
}

bool equal(const SkyboxSettings& a, const SkyboxSettings& b) {
    return a.chunk_version == b.chunk_version && equal(a.red, b.red) &&
           equal(a.green, b.green) && equal(a.blue, b.blue) &&
           equal(a.orientation_radians, b.orientation_radians) &&
           a.texture_paths == b.texture_paths && a.draw_clouds == b.draw_clouds &&
           a.back_texture_is_front_upside_down == b.back_texture_is_front_upside_down &&
           a.right_texture_is_left_upside_down == b.right_texture_is_left_upside_down &&
           a.source_record == b.source_record;
}

template <typename T, typename Predicate>
bool equal_vectors(const std::vector<T>& a, const std::vector<T>& b, Predicate predicate) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), predicate);
}

// dirty is derived from content revisions and is intentionally excluded.
bool equal_document_content(const Document& a, const Document& b) {
    return a.project_path == b.project_path && a.obj_path == b.obj_path &&
           a.source_pc_path == b.source_pc_path && a.output_path == b.output_path &&
           a.material_map == b.material_map && a.texture_dir == b.texture_dir &&
           a.weapons_donor == b.weapons_donor && a.object_donors == b.object_donors &&
           a.sky_texture_dir == b.sky_texture_dir &&
           equal_vectors(a.entities, b.entities,
                         [](const Entity& x, const Entity& y) { return equal(x, y); }) &&
           equal_vectors(a.pickup_templates, b.pickup_templates,
                         [](const PickupTemplate& x, const PickupTemplate& y) {
                             return equal(x, y);
                         }) &&
           equal_vectors(a.static_object_templates, b.static_object_templates,
                         [](const StaticObjectTemplate& x, const StaticObjectTemplate& y) {
                             return equal(x, y);
                         }) &&
           a.next_guid == b.next_guid && equal(a.light_header_a, b.light_header_a) &&
           equal(a.light_header_b, b.light_header_b) && equal(a.light_header_c, b.light_header_c) &&
           a.light_header_flag == b.light_header_flag && equal(a.skybox, b.skybox) &&
           a.rain_enabled == b.rain_enabled &&
           a.weather_source_record == b.weather_source_record &&
           a.ambient_stream_path == b.ambient_stream_path &&
           equal(a.ambient_volume, b.ambient_volume) &&
           a.ambient_source_record == b.ambient_source_record &&
           a.source_pickup_inventory_complete == b.source_pickup_inventory_complete &&
           a.source_static_object_inventory_complete == b.source_static_object_inventory_complete &&
           a.source_barrier_inventory_complete == b.source_barrier_inventory_complete;
}

bool authorable(EntityKind kind) {
    return kind == EntityKind::SpawnPoint || kind == EntityKind::Light ||
           kind == EntityKind::Sound || kind == EntityKind::Pickup ||
           kind == EntityKind::StaticObject || kind == EntityKind::BuildingVolume ||
           kind == EntityKind::InvisibleBarrier;
}

void set_error(std::string* error, const char* message) {
    if (error)
        *error = message;
}

bool find_fresh_guids(const Document& document, size_t requested,
                      std::vector<uint32_t>* guids, uint32_t* next_guid) {
    constexpr uint32_t first = asura::level::kToolCreatedGuidFirst;
    constexpr uint32_t last = asura::level::kToolCreatedGuidLast;
    constexpr uint32_t count = last - first + 1;

    guids->clear();
    if (!requested || requested > count)
        return false;
    guids->reserve(requested);
    uint32_t candidate = document.next_guid;
    if (candidate < first || candidate > last)
        candidate = first;
    for (size_t wanted = 0; wanted < requested; ++wanted) {
        bool found = false;
        for (uint32_t attempt = 0; attempt < count; ++attempt) {
            const bool used_by_document = std::any_of(
                document.entities.begin(), document.entities.end(),
                [candidate](const Entity& entity) { return entity.guid == candidate; });
            const bool already_allocated =
                std::find(guids->begin(), guids->end(), candidate) != guids->end();
            if (!used_by_document && !already_allocated) {
                guids->push_back(candidate);
                candidate = candidate == last ? first : candidate + 1;
                found = true;
                break;
            }
            candidate = candidate == last ? first : candidate + 1;
        }
        if (!found) {
            guids->clear();
            return false;
        }
    }
    *next_guid = candidate;
    return true;
}

void canonicalize_clone(Entity* entity) {
    const Asura_Vector_3 bounds_size{
        entity->source_bounds.MaxX - entity->source_bounds.MinX,
        entity->source_bounds.MaxY - entity->source_bounds.MinY,
        entity->source_bounds.MaxZ - entity->source_bounds.MinZ};
    entity->source_entity_record = false;
    entity->source_entity_classification = 0;
    entity->source_bounds = {};
    switch (entity->kind) {
    case EntityKind::SpawnPoint:
        entity->spawn_source_record = false;
        entity->spawn_index = 0;
        entity->spawn_posture = 0;
        entity->spawn_timer = 5.0f;
        entity->entity_padding = 0;
        // rotation, spawn_direction, team mask, and game-mode mask are the
        // author-facing gameplay state and intentionally remain unchanged.
        break;
    case EntityKind::Sound:
        entity->sound_source_record = false;
        entity->sound_has_controller = false;
        entity->sound_controller_active = true;
        entity->sound_controller_padding = 0x4974;
        entity->sound_phonon = {};
        break;
    case EntityKind::Pickup:
        entity->source_entity_classification = SnipeEntityClass_Pickup;
        // pickup_has_template, pickup_body, IDs, padding, and the user-visible
        // asset properties are the recovered authored template and stay intact.
        break;
    case EntityKind::StaticObject:
        entity->source_entity_classification = SnipeEntityClass_StaticObject;
        break;
    case EntityKind::BuildingVolume:
        entity->source_entity_classification = SnipeEntityClass_BuildingVolume;
        entity->source_bounds = {
            entity->position.x - bounds_size.x * .5f, entity->position.x + bounds_size.x * .5f,
            entity->position.y - bounds_size.y * .5f, entity->position.y + bounds_size.y * .5f,
            entity->position.z - bounds_size.z * .5f, entity->position.z + bounds_size.z * .5f};
        break;
    case EntityKind::InvisibleBarrier:
        entity->barrier_source_record = false;
        entity->barrier_source_chunk = 0xffffffffu;
        entity->barrier_source_module = 0xffffffffu;
        entity->barrier_source_component = 0xffffffffu;
        entity->source_bounds = {
            entity->position.x - bounds_size.x * .5f, entity->position.x + bounds_size.x * .5f,
            entity->position.y - bounds_size.y * .5f, entity->position.y + bounds_size.y * .5f,
            entity->position.z - bounds_size.z * .5f, entity->position.z + bounds_size.z * .5f};
        break;
    case EntityKind::Light:
    case EntityKind::AssassinationTarget:
    case EntityKind::PositionMarker:
        break;
    }
}

} // namespace

LevelEditorHistory::LevelEditorHistory(size_t maximum_entries)
    : maximum_entries_(std::max<size_t>(1, maximum_entries)) {}

void LevelEditorHistory::reset(Document* document, int selected_index, bool mark_as_saved) {
    if (!document)
        return;
    undo_.clear();
    redo_.clear();
    transaction_start_.reset();
    current_revision_ = 1;
    saved_revision_ = mark_as_saved ? current_revision_ : 0;
    next_revision_ = 2;
    initialized_ = true;
    (void)selected_index;
    synchronize_dirty(document);
}

void LevelEditorHistory::clear_history() {
    undo_.clear();
    redo_.clear();
    transaction_start_.reset();
}

void LevelEditorHistory::ensure_initialized(const Document& document) {
    if (initialized_)
        return;
    current_revision_ = 1;
    saved_revision_ = document.dirty ? 0 : current_revision_;
    next_revision_ = 2;
    initialized_ = true;
}

LevelEditorHistory::Snapshot LevelEditorHistory::capture(const Document& document,
                                                         int selected_index) const {
    Snapshot snapshot;
    snapshot.document = document;
    snapshot.selected_index = selected_index;
    snapshot.content_revision = current_revision_;
    return snapshot;
}

void LevelEditorHistory::push_bounded(std::vector<Snapshot>* stack, Snapshot snapshot) {
    stack->push_back(std::move(snapshot));
    if (stack->size() > maximum_entries_)
        stack->erase(stack->begin(), stack->begin() + (stack->size() - maximum_entries_));
}

void LevelEditorHistory::synchronize_dirty(Document* document) const {
    if (document)
        document->dirty = saved_revision_ == 0 || current_revision_ != saved_revision_;
}

bool LevelEditorHistory::begin(const Document& document, int selected_index) {
    if (transaction_start_)
        return false;
    ensure_initialized(document);
    transaction_start_ = capture(document, selected_index);
    return true;
}

bool LevelEditorHistory::commit(Document* document, int selected_index) {
    if (!document || !transaction_start_)
        return false;

    const bool document_changed = !equal_document_content(transaction_start_->document, *document);
    const bool selection_changed = transaction_start_->selected_index != selected_index;
    if (!document_changed && !selection_changed) {
        transaction_start_.reset();
        synchronize_dirty(document);
        return false;
    }

    push_bounded(&undo_, std::move(*transaction_start_));
    transaction_start_.reset();
    redo_.clear();
    if (document_changed)
        current_revision_ = next_revision_++;
    synchronize_dirty(document);
    return true;
}

void LevelEditorHistory::cancel_transaction() {
    transaction_start_.reset();
}

bool LevelEditorHistory::transaction_active() const {
    return transaction_start_.has_value();
}

bool LevelEditorHistory::can_undo() const {
    return !transaction_start_ && !undo_.empty();
}

bool LevelEditorHistory::can_redo() const {
    return !transaction_start_ && !redo_.empty();
}

void LevelEditorHistory::restore(const Snapshot& snapshot, Document* document, int* selected_index) {
    *document = snapshot.document;
    current_revision_ = snapshot.content_revision;
    if (selected_index) {
        *selected_index = snapshot.selected_index;
        if (*selected_index < -1 || *selected_index >= static_cast<int>(document->entities.size()))
            *selected_index = -1;
    }
    synchronize_dirty(document);
}

bool LevelEditorHistory::undo(Document* document, int* selected_index) {
    if (!document || !selected_index || !can_undo())
        return false;
    push_bounded(&redo_, capture(*document, *selected_index));
    Snapshot snapshot = std::move(undo_.back());
    undo_.pop_back();
    restore(snapshot, document, selected_index);
    return true;
}

bool LevelEditorHistory::redo(Document* document, int* selected_index) {
    if (!document || !selected_index || !can_redo())
        return false;
    push_bounded(&undo_, capture(*document, *selected_index));
    Snapshot snapshot = std::move(redo_.back());
    redo_.pop_back();
    restore(snapshot, document, selected_index);
    return true;
}

void LevelEditorHistory::mark_saved(Document* document) {
    if (!document)
        return;
    ensure_initialized(*document);
    saved_revision_ = current_revision_;
    synchronize_dirty(document);
}

bool LevelEditorHistory::is_dirty() const {
    return saved_revision_ == 0 || current_revision_ != saved_revision_;
}

uint64_t LevelEditorHistory::current_revision() const {
    return current_revision_;
}

uint64_t LevelEditorHistory::saved_revision() const {
    return saved_revision_;
}

bool LevelEditorHistory::copy(const Document& document, const std::vector<int>& selected_indices,
                              std::string* error) {
    if (selected_indices.empty()) {
        set_error(error, "Select one or more entities to copy.");
        return false;
    }
    std::vector<Entity> next;
    next.reserve(selected_indices.size());
    std::vector<int> seen;
    seen.reserve(selected_indices.size());
    std::optional<EntityKind> kind;
    for (int selected_index : selected_indices) {
        if (selected_index < 0 || selected_index >= static_cast<int>(document.entities.size())) {
            set_error(error, "The selection contains an invalid entity.");
            return false;
        }
        if (std::find(seen.begin(), seen.end(), selected_index) != seen.end())
            continue;
        seen.push_back(selected_index);
        const Entity& entity = document.entities[static_cast<size_t>(selected_index)];
        if (!authorable(entity.kind)) {
            set_error(error, "Imported targets and position markers cannot be authored or pasted.");
            return false;
        }
        if (kind && *kind != entity.kind) {
            set_error(error, "Copying multiple entities is limited to one entity type.");
            return false;
        }
        kind = entity.kind;
        next.push_back(entity);
    }
    if (next.empty()) {
        set_error(error, "Select one or more entities to copy.");
        return false;
    }
    clipboard_ = std::move(next);
    if (error)
        error->clear();
    return true;
}

bool LevelEditorHistory::can_paste() const {
    return !clipboard_.empty() && !transaction_start_;
}

bool LevelEditorHistory::paste(Document* document, int* selected_index,
                               std::vector<int>* pasted_indices, std::string* error) {
    if (!document || !selected_index || !pasted_indices) {
        set_error(error, "Paste requires a document and selection.");
        return false;
    }
    pasted_indices->clear();
    if (clipboard_.empty()) {
        set_error(error, "The clipboard does not contain authorable entities.");
        return false;
    }
    if (transaction_start_) {
        set_error(error, "Finish the active edit before pasting.");
        return false;
    }

    std::vector<uint32_t> guids;
    uint32_t following_guid = 0;
    if (!find_fresh_guids(*document, clipboard_.size(), &guids, &following_guid)) {
        set_error(error, "Not enough free editor GUIDs remain for the pasted entities.");
        return false;
    }

    if (!begin(*document, *selected_index)) {
        set_error(error, "Could not start the paste transaction.");
        return false;
    }

    pasted_indices->reserve(clipboard_.size());
    for (size_t index = 0; index < clipboard_.size(); ++index) {
        Entity clone = clipboard_[index];
        clone.guid = guids[index];
        canonicalize_clone(&clone);
        document->entities.push_back(std::move(clone));
        pasted_indices->push_back(static_cast<int>(document->entities.size() - 1));
    }
    document->next_guid = following_guid;
    *selected_index = pasted_indices->back();
    const bool committed = commit(document, *selected_index);
    if (!committed) {
        cancel_transaction();
        set_error(error, "Paste did not change the document.");
        return false;
    }
    if (error)
        error->clear();
    return true;
}

void LevelEditorHistory::clear_clipboard() {
    clipboard_.clear();
}

} // namespace editor
