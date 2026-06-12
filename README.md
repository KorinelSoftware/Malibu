![Malibu Banner](tools/malibu-banner.png)

# Malibu

A modern web engine built from scratch in C++20.

Malibu is an experimental browser engine focused on performance, simplicity, and architectural innovation. Unlike traditional browser architectures, Malibu treats the web platform as a structured operating environment where JavaScript, the DOM, rendering, networking, and platform APIs are integrated through a unified architecture.

## Features

- Modern HTML, CSS and JavaScript support
- Custom JavaScript engine (MalibuJS)
- Unified Object Graph (UOG)
- WebCall ABI architecture
- Software and GPU rendering backends
- Canvas2D support
- WebGL support
- WebAssembly support
- Embedded view API (MalibuView)
- Cross-platform design

## Architecture

Malibu is built around several core technologies:

### MalibuJS
A custom JavaScript engine featuring:

- Register-based virtual machine
- NaN-boxed values
- Generational garbage collection
- Test262 compatibility

### Unified Object Graph (UOG)

Instead of maintaining separate JavaScript and DOM heaps, Malibu uses a unified memory model where web objects coexist within the same graph.

Benefits:

- No wrapper synchronization
- Faster object access
- Reduced memory overhead
- Simpler garbage collection

### WebCall ABI

Web APIs are exposed through a lightweight ABI layer similar to operating system syscalls.

This provides:

- Lower API overhead
- Clear browser-engine boundaries
- Better optimization opportunities
- Simplified embedding

## Current Status

Malibu is under active development.

Implemented:

- HTML parser
- DOM tree
- CSS parser
- Style system
- Layout engine
- Flexbox
- Tables
- Canvas2D
- WebGL
- WebAssembly runtime
- JavaScript engine
- Embedded browser view

In Progress:

- SVG rendering
- Advanced CSS support
- Additional Web APIs
- GPU acceleration improvements
- Web platform compatibility

## Building

```bash
git clone https://github.com/korinel/malibu.git
cd malibu

cmake -B build -G Ninja
cmake --build build
