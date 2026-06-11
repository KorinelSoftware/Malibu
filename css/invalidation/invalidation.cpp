// css/invalidation/invalidation.cpp
// Dirty-node tracking and subtree recomputation.

#include "malibu/css/invalidation/invalidation.h"
#include "malibu/dom/document.h"

namespace malibu::css {

void StyleInvalidator::mark_dirty(malibu::NodeHandle h, InvalidationReason /*reason*/) {
    if (h.is_null()) return;
    if (dirty_.insert(key(h)).second) dirty_list_.push_back(h);
}

bool StyleInvalidator::is_dirty(malibu::NodeHandle h) const {
    return dirty_.find(key(h)) != dirty_.end();
}

void StyleInvalidator::recompute(malibu::dom::Document& doc, StyleResolver& resolver) {
    // Recompute each dirty node's subtree. A dirty node nested under another
    // dirty node is still safe to recompute (idempotent); we skip ones whose
    // ancestor is also dirty to avoid redundant work.
    for (malibu::NodeHandle h : dirty_list_) {
        bool ancestor_dirty = false;
        const malibu::dom::NodeCore* c = doc.core(h);
        malibu::NodeHandle p = c ? c->parent : malibu::NodeHandle::null_handle();
        while (!p.is_null()) {
            if (dirty_.find(key(p)) != dirty_.end()) { ancestor_dirty = true; break; }
            const malibu::dom::NodeCore* pc = doc.core(p);
            p = pc ? pc->parent : malibu::NodeHandle::null_handle();
        }
        if (!ancestor_dirty) resolver.resolve_subtree(doc, h);
    }
    clear();
    dirty_list_.clear();
}

} // namespace malibu::css
