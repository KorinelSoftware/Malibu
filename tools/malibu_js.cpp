// tools/malibu_js.cpp
// Minimal JS CLI runner for the MalibuJS engine. Evaluates each file argument in
// sequence sharing ONE engine/global (so a Test262 harness prelude + test run
// together), then drains the event loop. Exits 0 if everything ran without an
// uncaught error; prints "ERROR: <message>" and exits 1 on the first failure.
// Used by the Test262 conformance driver.

#include "malibu/js/engine.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    malibu::js::Engine engine;
    for (int i = 1; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        if (!f) { std::cout << "ERROR: cannot open " << argv[i] << "\n"; return 3; }
        std::stringstream ss; ss << f.rdbuf();
        auto r = engine.evaluate(ss.str(), argv[i]);
        if (!r.ok) {
            std::cout << "ERROR: " << r.error << "\n";
            return 1;
        }
    }
    engine.run_event_loop();
    return 0;
}
