#pragma once
// core/include/malibu/shader/glsl.h
// MalibuShader — a GLSL ES (OpenGL ES 2.0) shader interpreter.
//   GLSL source -> lexer -> parser -> AST -> tree-walk interpreter.
// Values are vecN (1..4 floats) or mat4 (16 floats). The GL rasterizer seeds
// inputs (attributes/uniforms/varyings) per invocation, runs main(), and reads
// outputs (gl_Position / gl_FragColor / varyings).

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace malibu::shader {

struct Val {
    float v[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0};
    int   n = 1;        // vector component count (1..4)
    bool  mat = false;  // true => 4x4 matrix (n ignored)
    static Val scalar(float x) { Val r; r.n = 1; r.v[0] = x; return r; }
};

struct Node;  // AST (opaque)

class Program {
public:
    Program();
    ~Program();

    // Compile GLSL source; returns false + fills `log` on a parse error.
    bool compile(const std::string& src, std::string& log);

    // Declared interface (for the rasterizer to wire up).
    const std::vector<std::string>& attributes() const { return attributes_; }
    const std::vector<std::string>& varyings() const { return varyings_; }
    const std::vector<std::string>& uniforms() const { return uniforms_; }

    // Samples a texture unit at (u,v), returning an RGBA Val. Provided by the
    // rasterizer so `texture2D(sampler, uv)` works without coupling to GL.
    using Sampler = std::function<Val(int unit, float u, float v)>;

    // Run main() with a fresh environment seeded by `inputs`. Outputs (and any
    // assigned varyings/gl_*) are returned in `out`.
    bool run(const std::map<std::string, Val>& inputs, std::map<std::string, Val>& out,
             const Sampler& sampler = {});

private:
    std::shared_ptr<Node>    root_;     // function/decl list
    std::vector<std::string> attributes_, varyings_, uniforms_;
};

} // namespace malibu::shader
