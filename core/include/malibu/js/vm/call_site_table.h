#pragma once
// core/include/malibu/js/vm/call_site_table.h
// Call site deoptimization tracking.

#include <cstdint>
#include <vector>
#include <unordered_map>
#include "bytecode.h"

namespace malibu::js::vm {

class CallSiteTable {
public:
    void register_function(uint32_t func_id, const std::vector<CallSiteEntry>& sites);
    void deoptimize(uint32_t call_site_id);
    void reoptimize(uint32_t call_site_id);
    bool is_deoptimized(uint32_t call_site_id) const noexcept;
private:
    std::unordered_map<uint32_t, CallSiteEntry> sites_;
};

} // namespace malibu::js::vm