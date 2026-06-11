#pragma once
// core/include/malibu/diagnostics/diagnostic_log.h
// Thread-safe diagnostic logging system.

#include <cstdint>
#include <string_view>
#include <memory>
#include <mutex>

namespace malibu::diagnostics {

enum class Level : uint8_t { DEBUG, INFO, WARNING, ERROR };

struct Entry {
    uint64_t timestamp_ms;
    Level    level;
    std::string_view subsystem;
    std::string        message;
    std::string_view   file;
    uint32_t           line;
};

class DiagnosticSink {
public:
    virtual ~DiagnosticSink() = default;
    virtual void write(const Entry&) = 0;
};

class StderrSink : public DiagnosticSink {
public:
    void write(const Entry&) override;
};

class DiagnosticLog {
public:
    static DiagnosticLog& instance();
    void set_sink(std::unique_ptr<DiagnosticSink>);
    void log(Level, std::string_view subsystem, std::string_view msg,
             std::string_view file, uint32_t line);
private:
    std::mutex mu_;
    std::unique_ptr<DiagnosticSink> sink_;
};

#define MALIBU_LOG(level, subsystem, msg) \
    malibu::diagnostics::DiagnosticLog::instance().log( \
        malibu::diagnostics::Level::level, subsystem, msg, __FILE__, __LINE__)

} // namespace malibu::diagnostics