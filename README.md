# Malibu

A modern, high-performance web engine built with C++20, targeting full modern page compatibility.

## Features

- **JavaScript Engine** — Custom JS engine with bytecode compiler, interpreter, JIT-ready IR, typed arrays, and DOM bindings
- **CSS Engine** — Full CSS parser, style resolver, computed style system
- **HTML/DOM** — HTML parser, DOM tree construction, event handling
- **Layout & Render** — Layout engine, display list, Vulkan/OpenGL rendering pipeline
- **Image Decoding** — JPEG, PNG, GIF support
- **WebAssembly** — WASM runtime integration
- **Storage** — Cookie store, Web Storage API
- **Networking** — HTTP client with cURL backend, WebCall ABI
- **Security** — Permissions, sandboxing policies
- **Canvas 2D / WebGL** — GPU-accelerated graphics
- **Text & Fonts** — Text shaping, font loading
- **Platform** — Linux, Windows, macOS, Android, iOS support
- **Integrations** — malibu-shell (standalone), malibu-view (embedding), malibu-browser, malibu-electron, malibu-cef, malibu-app-runtime

## Building

```bash
cmake -B build -G Ninja
cmake --build build
```

### Dependencies

- C++20 compiler (Clang 16+ or GCC 13+)
- CMake 3.25+
- Vulkan SDK (optional, for GPU rendering)
- cURL (for HTTP networking)
- simdjson (vendored or system)
- googletest (vendored automatically)

## Project Structure

- `core/` — Engine-agnostic data structures, utilities, and interfaces
- `js/` — JavaScript engine (parser, compiler, runtime, heap)
- `css/` — CSS parser, style resolution
- `html/` — HTML parser, DOM
- `render/` — Display list, GPU rendering
- `layout/` — Box tree, layout engine
- `dom/` — DOM bindings and event system
- `image/` — Image codecs (JPEG, PNG, GIF)
- `network/` — HTTP client, networking
- `storage/` — Web Storage, cookies
- `security/` — Permissions, sandboxing
- `wasm/` — WebAssembly runtime
- `canvas/` — Canvas 2D API
- `gl/` — OpenGL abstraction layer
- `host/` — Platform host implementations
- `platform/` — OS-specific abstractions
- `malibu-shell/` — Standalone browser shell
- `malibu-view/` — Embeddable view widget
- `malibu-browser/` — Full browser application
- `malibu-electron/` — Electron integration
- `malibu-cef/` — CEF integration
- `malibu-app-runtime/` — Application runtime
- `tests/` — Unit and integration tests
- `tools/` — CLI utilities
- `examples/` — Example applications

## License

[Specify your license here]
