#pragma once

#include "LevelEditorDocument.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace editor {

// Snapshot-based editor history.  A content revision is separate from a
// history entry so selection-only transactions remain undoable without making
// the document dirty.
class LevelEditorHistory {
public:
    explicit LevelEditorHistory(size_t maximum_entries = 64);

    // Starts a new history epoch for a newly-created or newly-loaded document.
    // When mark_as_saved is true, this state is the clean save point.
    void reset(Document* document, int selected_index = -1, bool mark_as_saved = true);

    // Drops undo/redo and any open transaction while retaining the current
    // content/save revisions and clipboard.
    void clear_history();

    bool begin(const Document& document, int selected_index);
    bool commit(Document* document, int selected_index);
    void cancel_transaction();
    bool transaction_active() const;

    bool can_undo() const;
    bool can_redo() const;
    bool undo(Document* document, int* selected_index);
    bool redo(Document* document, int* selected_index);

    void mark_saved(Document* document);
    bool is_dirty() const;
    uint64_t current_revision() const;
    uint64_t saved_revision() const;

    // The clipboard deliberately contains only authorable entities.  Paste is
    // an atomic history transaction and selects the newly appended clone.
    bool copy(const Document& document, int selected_index, std::string* error = nullptr);
    bool can_paste() const;
    bool paste(Document* document, int* selected_index, std::string* error = nullptr);
    void clear_clipboard();

private:
    struct Snapshot {
        Document document;
        int selected_index = -1;
        uint64_t content_revision = 1;
    };

    void ensure_initialized(const Document& document);
    Snapshot capture(const Document& document, int selected_index) const;
    void push_bounded(std::vector<Snapshot>* stack, Snapshot snapshot);
    void restore(const Snapshot& snapshot, Document* document, int* selected_index);
    void synchronize_dirty(Document* document) const;

    size_t maximum_entries_ = 64;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    std::optional<Snapshot> transaction_start_;
    std::optional<Entity> clipboard_;
    uint64_t current_revision_ = 1;
    uint64_t saved_revision_ = 1;
    uint64_t next_revision_ = 2;
    bool initialized_ = false;
};

} // namespace editor
