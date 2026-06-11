// core/diagnostics/diagnostic_log.cpp
// Thread-safe diagnostic logging implementation.

#include "malibu/diagnostics/diagnostic_log.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace malibu::diagnostics {

DiagnosticLog& DiagnosticLog::instance() {
    static DiagnosticLog log;
    return log;
}

void DiagnosticLog::set_sink(std::unique_ptr<DiagnosticSink> sink) {
    std::lock_guard lock(mu_);
    sink_ = std::move(sink);
}

void DiagnosticLog::log(Level level, std::string_view subsystem, std::string_view msg,
                        std::string_view file, uint32_t line) {
    Entry entry;
    entry.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    entry.level = level;
    entry.subsystem = subsystem;
    entry.message = msg;
    entry.file = file;
    entry.line = line;
    
    std::lock_guard lock(mu_);
    if (sink_) {
        sink_->write(entry);
    }
}

void StderrSink::write(const Entry& entry) {
    static const char* level_names[] = {"DEBUG", "INFO", "WARNING", "ERROR"};
    
    std::ostringstream oss;
    oss << "[" << level_names[static_cast<uint8_t>(entry.level)] << "] "
        << "[" << entry.subsystem << "] "
        << entry.message
        << " (" << entry.file << ":" << entry.line << ")";
    
    std::cerr << oss.str() << std::endl;
}

} // namespace malibu::diagnostics