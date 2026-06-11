#pragma once
// core/include/malibu/security/csp_enforcer.h
// Content Security Policy enforcement (Task 13 / Requirement 14.2, 14.3).
//
// A resource load is checked BEFORE any bytes are fetched. With no policy set,
// all loads are permitted (Req 14.2). Violations are recorded; when a report
// endpoint is configured a report is emitted asynchronously without affecting
// the (already-made) block decision.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "origin.h"

namespace malibu::security {

class CspEnforcer {
public:
    enum class Directive {
        DefaultSrc, ScriptSrc, StyleSrc, ImgSrc, ConnectSrc, FontSrc,
        FrameSrc, MediaSrc, ObjectSrc, ChildSrc, WorkerSrc, ManifestSrc,
        PrefetchSrc, FormAction, FrameAncestors, NavigateTo, ReportUri, ReportTo
    };

    struct Violation {
        Directive   directive;
        std::string blocked_url;
        std::string document_url;
    };

    // The document's own origin, used to resolve the 'self' source expression.
    void set_document_origin(const Origin& origin) { document_origin_ = origin; }

    // Parses a CSP header value (directives separated by ';').
    void set_policy(std::string_view policy);

    [[nodiscard]] bool has_policy() const noexcept { return has_policy_; }

    // True iff a load of `url` for `directive` is allowed by the policy.
    [[nodiscard]] bool allows(Directive directive, std::string_view url) const;

    // Records a violation (and would POST a report asynchronously if a report
    // endpoint is configured). Never blocks or throws.
    void report_violation(Directive directive, std::string_view blocked_url,
                          std::string_view document_url);

    [[nodiscard]] size_t violation_count() const noexcept { return violations_.size(); }
    [[nodiscard]] const std::vector<Violation>& violations() const noexcept { return violations_; }
    [[nodiscard]] const std::string& report_uri() const noexcept { return policy_.report_uri; }

private:
    const std::vector<std::string>& list_for(Directive d) const;
    bool source_list_allows(const std::vector<std::string>& list, std::string_view url) const;

    struct Policy {
        std::vector<std::string> default_src;
        std::vector<std::string> script_src;
        std::vector<std::string> style_src;
        std::vector<std::string> img_src;
        std::vector<std::string> connect_src;
        std::vector<std::string> font_src;
        std::vector<std::string> frame_src;
        std::vector<std::string> media_src;
        std::vector<std::string> object_src;
        std::vector<std::string> child_src;
        std::vector<std::string> worker_src;
        std::vector<std::string> manifest_src;
        std::vector<std::string> prefetch_src;
        std::vector<std::string> form_action;
        std::vector<std::string> frame_ancestors;
        std::vector<std::string> navigate_to;
        std::string report_uri;
        std::string report_to;
    } policy_;

    Origin                 document_origin_;
    bool                   has_policy_ = false;
    std::vector<Violation> violations_;
};

} // namespace malibu::security
