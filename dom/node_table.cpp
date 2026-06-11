// dom/node_table.cpp
// NodeHandle slab allocator implementation.

#include "malibu/dom/node_table.h"
#include "malibu/diagnostics/diagnostic_log.h"
#include <algorithm>
#include <sstream>

namespace malibu::dom {

NodeHandle NodeTable::alloc(NodeCore* node) {
    std::lock_guard lock(mu_);
    
    uint32_t index;
    if (!free_list_.empty()) {
        index = free_list_.back();
        free_list_.pop_back();
        slots_[index].generation++;
    } else {
        index = static_cast<uint32_t>(slots_.size());
        slots_.emplace_back();
    }
    
    NodeSlot& slot = slots_[index];
    slot.state = NodeState::AliveConnected;
    slot.pin_count = 0;
    slot.node_type = node ? node->node_type : 0;
    slot.node = node;
    
    if (node) {
        node->handle = NodeHandle{index, slot.generation};
    }
    
    return NodeHandle{index, slot.generation};
}

void NodeTable::free(NodeHandle h) {
    std::lock_guard lock(mu_);
    
    if (h.is_null() || h.index >= slots_.size()) {
        std::ostringstream oss;
        oss << "Attempt to free invalid NodeHandle: index=" << h.index << " gen=" << h.generation;
        MALIBU_LOG(WARNING, "dom", oss.str());
        return;
    }
    
    NodeSlot& slot = slots_[h.index];
    if (slot.generation != h.generation) {
        std::ostringstream oss;
        oss << "Attempt to free invalid NodeHandle: index=" << h.index << " gen=" << h.generation;
        MALIBU_LOG(WARNING, "dom", oss.str());
        return;
    }
    
    if (slot.pin_count > 0) {
        std::ostringstream oss;
        oss << "Attempt to free pinned NodeHandle: index=" << h.index << " pin_count=" << slot.pin_count;
        MALIBU_LOG(WARNING, "dom", oss.str());
        return;
    }
    
    if (slot.node) {
        slot.node = nullptr;
    }
    
    slot.state = NodeState::Dead;
    slot.node_type = 0;
    free_list_.push_back(h.index);
}

NodeSlot* NodeTable::resolve(NodeHandle h) {
    std::lock_guard lock(mu_);
    
    if (h.is_null() || h.index >= slots_.size()) {
        return nullptr;
    }
    
    NodeSlot& slot = slots_[h.index];
    if (slot.generation != h.generation) {
        std::ostringstream oss;
        oss << "Stale NodeHandle detected: index=" << h.index 
            << " expected_gen=" << slot.generation << " got_gen=" << h.generation;
        MALIBU_LOG(DEBUG, "dom", oss.str());
        return nullptr;
    }
    
    if (slot.state == NodeState::Dead) {
        return nullptr;
    }
    
    return &slot;
}

void NodeTable::pin(NodeHandle h) {
    std::lock_guard lock(mu_);
    
    if (h.is_null() || h.index >= slots_.size()) {
        return;
    }
    
    NodeSlot& slot = slots_[h.index];
    if (slot.generation != h.generation) {
        return;
    }
    
    if (slot.state != NodeState::Dead) {
        slot.pin_count++;
    }
}

void NodeTable::unpin(NodeHandle h) {
    std::lock_guard lock(mu_);
    
    if (h.is_null() || h.index >= slots_.size()) {
        return;
    }
    
    NodeSlot& slot = slots_[h.index];
    if (slot.generation != h.generation) {
        return;
    }
    
    if (slot.state != NodeState::Dead && slot.pin_count > 0) {
        slot.pin_count--;
    }
}

NodeState NodeTable::state_of(NodeHandle h) const {
    std::lock_guard lock(mu_);
    
    if (h.is_null() || h.index >= slots_.size()) {
        return NodeState::Dead;
    }
    
    const NodeSlot& slot = slots_[h.index];
    if (slot.generation != h.generation) {
        return NodeState::Dead;
    }
    
    return slot.state;
}

void NodeTable::set_state(NodeHandle h, NodeState s) {
    std::lock_guard lock(mu_);
    
    if (h.is_null() || h.index >= slots_.size()) {
        return;
    }
    
    NodeSlot& slot = slots_[h.index];
    if (slot.generation != h.generation) {
        return;
    }
    
    if (slot.state != NodeState::Dead) {
        slot.state = s;
    }
}

} // namespace malibu::dom