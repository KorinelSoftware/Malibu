// js/bytecode/bytecode.cpp
// CallSiteTable implementation. Bytecode is never mutated; deopt state lives here.

#include "malibu/js/bytecode/bytecode.h"

namespace malibu::js::bytecode {

void CallSiteTable::register_function(uint32_t /*func_id*/,
                                      const std::vector<CallSiteEntry>& sites) {
    std::lock_guard lock(mu_);
    for (const auto& site : sites) {
        sites_[site.call_site_id] = site;
    }
}

void CallSiteTable::deoptimize(uint32_t call_site_id) {
    std::lock_guard lock(mu_);
    auto it = sites_.find(call_site_id);
    if (it != sites_.end()) {
        it->second.deoptimized = true;
        it->second.deopt_epoch++;
    }
}

void CallSiteTable::reoptimize(uint32_t call_site_id) {
    std::lock_guard lock(mu_);
    auto it = sites_.find(call_site_id);
    if (it != sites_.end()) {
        it->second.deoptimized = false;
    }
}

bool CallSiteTable::is_deoptimized(uint32_t call_site_id) const {
    std::lock_guard lock(mu_);
    auto it = sites_.find(call_site_id);
    return it != sites_.end() && it->second.deoptimized;
}

uint32_t CallSiteTable::webcall_id_of(uint32_t call_site_id) const {
    std::lock_guard lock(mu_);
    auto it = sites_.find(call_site_id);
    return it != sites_.end() ? it->second.webcall_id : UINT32_MAX;
}

uint64_t CallSiteTable::epoch_of(uint32_t call_site_id) const {
    std::lock_guard lock(mu_);
    auto it = sites_.find(call_site_id);
    return it != sites_.end() ? it->second.deopt_epoch : 0;
}

} // namespace malibu::js::bytecode
