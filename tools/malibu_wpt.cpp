// tools/malibu_wpt.cpp
// Web Platform Tests runner: loads a testharness.js-based WPT test in a headless
// MalibuView, serving /resources/* and relative resources from a local WPT
// checkout, and reports the per-subtest pass/fail collected via a completion
// callback. Used as the "compatible with the whole web" thermometer.
//
//   malibu_wpt <wpt_root> <test.html>
// Prints: "RESULT <harness_status> <pass> <fail>" or "RESULT ERROR ...".

#include "malibu/view/view.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {
std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// Registered as /resources/testharnessreport.js: captures results into a global.
const char* kReport = R"JS(
globalThis.__wpt = { hs: "registered", pass: 0, fail: 0, names: [] };
try { setup({ output: false, debug: false }); } catch (e) {}
add_completion_callback(function (tests, status) {
  var pass = 0, fail = 0, names = [];
  for (var i = 0; i < tests.length; i++) {
    if (tests[i].status === 0) pass++;
    else { fail++; names.push(tests[i].name + ": " + (tests[i].message || tests[i].status)); }
  }
  globalThis.__wpt = { hs: status.status, pass: pass, fail: fail, names: names };
});
)JS";
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: " << argv[0] << " <wpt_root> <test.html>\n"; return 2; }
    std::string root = argv[1];
    std::string test = argv[2];

    std::string html = read_file(test);
    if (html.empty()) { std::cout << "RESULT ERROR cannot-read\n"; return 1; }

    // Derive the test's URL path relative to the WPT root so root-relative
    // resource URLs (/resources/...) and document-relative ones resolve.
    std::string rel = test;
    if (rel.rfind(root, 0) == 0) rel = rel.substr(root.size());
    if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
    std::string base = "http://wpt/" + rel;

    malibu::view::View view;
    view.set_request_handler([&](const std::string& url, malibu::network::FetchResponse& out) -> bool {
        // Map http://wpt/<path> back to the local checkout.
        std::string path = url;
        auto s = path.find("http://wpt/");
        if (s == 0) path = path.substr(std::string("http://wpt").size());  // keep leading '/'
        if (path.find("testharnessreport.js") != std::string::npos) {
            std::string body = kReport;
            out.body.assign(body.begin(), body.end()); out.status = 200; return true;
        }
        std::string file = root + path;
        std::string body = read_file(file);
        if (body.empty()) return false;
        out.body.assign(body.begin(), body.end()); out.status = 200; return true;
    });

    if (!view.load_html(html, base)) { std::cout << "RESULT ERROR load-failed\n"; return 1; }
    view.engine().run_event_loop();

    std::string r = view.eval_js("(globalThis.__wpt ? (__wpt.hs + ' ' + __wpt.pass + ' ' + __wpt.fail) : 'NONE 0 0')");
    // eval_js returns JSON; strip surrounding quotes if present.
    if (r.size() >= 2 && r.front() == '"' && r.back() == '"') r = r.substr(1, r.size() - 2);
    std::cout << "RESULT " << r << "\n";
    // MALIBU_WPT_NAMES=1: dump the failing subtests (name + message) for triage.
    if (std::getenv("MALIBU_WPT_NAMES")) {
        std::string names = view.eval_js("(globalThis.__wpt && __wpt.names ? __wpt.names.join('\\n') : '')");
        if (names.size() >= 2 && names.front() == '"' && names.back() == '"') names = names.substr(1, names.size() - 2);
        std::cout << names << "\n";
    }
    return 0;
}
