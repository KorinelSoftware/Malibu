#pragma once
// core/include/malibu/gl/gl.h
// MalibuGL — a from-scratch WebGL-1-style rasterizer (OpenGL ES 2.0 semantics).
// State machine + command execution; GLSL ES shaders are run by MalibuShader
// (an interpreter). No GPU/ANGLE dependency: it rasterizes into an RGBA bitmap
// the host composites. This is the <canvas>.getContext("webgl") backend.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace malibu::gl {

// GL enum subset.
enum : uint32_t {
    GL_FRAGMENT_SHADER = 0x8B30, GL_VERTEX_SHADER = 0x8B31,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_FLOAT = 0x1406,
    GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x0100,
    GL_TRIANGLES = 0x0004, GL_TRIANGLE_STRIP = 0x0005, GL_TRIANGLE_FAN = 0x0006,
    GL_POINTS = 0x0000, GL_LINES = 0x0001,
    GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82,
    GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE0 = 0x84C0, GL_RGBA = 0x1908, GL_RGB = 0x1907,
    GL_NEAREST = 0x2600, GL_LINEAR = 0x2601,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_CLAMP_TO_EDGE = 0x812F,
};

struct Texture { int w = 0, h = 0; std::vector<uint8_t> rgba; bool linear = true; };

struct Shader { uint32_t type = 0; std::string src; bool compiled = false; std::string log; std::shared_ptr<void> program; };
struct Program { std::vector<uint32_t> shaders; bool linked = false; std::string log;
                 std::shared_ptr<void> vs, fs; };
struct Buffer { std::vector<uint8_t> data; };
struct VertexAttrib { bool enabled = false; int size = 4; uint32_t buffer = 0; int stride = 0; int offset = 0; };

class Context {
public:
    Context(int width, int height);
    ~Context();

    int width() const { return w_; }
    int height() const { return h_; }
    const std::vector<uint8_t>& pixels() const { return color_; }  // RGBA

    // shaders / programs
    uint32_t createShader(uint32_t type);
    void     shaderSource(uint32_t s, const std::string& src);
    void     compileShader(uint32_t s);
    bool     getShaderParameter(uint32_t s, uint32_t pname);
    std::string getShaderInfoLog(uint32_t s);
    uint32_t createProgram();
    void     attachShader(uint32_t p, uint32_t s);
    void     linkProgram(uint32_t p);
    bool     getProgramParameter(uint32_t p, uint32_t pname);
    std::string getProgramInfoLog(uint32_t p);
    void     useProgram(uint32_t p);

    // buffers / attributes
    uint32_t createBuffer();
    void     bindBuffer(uint32_t target, uint32_t buf);
    void     bufferData(uint32_t target, const uint8_t* bytes, size_t len);
    int      getAttribLocation(uint32_t p, const std::string& name);
    void     enableVertexAttribArray(uint32_t loc);
    void     vertexAttribPointer(uint32_t loc, int size, uint32_t type, bool norm, int stride, int offset);

    // textures
    uint32_t createTexture();
    void     bindTexture(uint32_t target, uint32_t tex);
    void     activeTexture(uint32_t unit);
    void     texImage2D(int w, int h, const uint8_t* rgba, size_t len);
    void     texParameteri(uint32_t pname, int value);

    // uniforms
    int  getUniformLocation(uint32_t p, const std::string& name);
    void uniform1f(int loc, float x);
    void uniform2f(int loc, float x, float y);
    void uniform3f(int loc, float x, float y, float z);
    void uniform4f(int loc, float x, float y, float z, float w);
    void uniform1i(int loc, int x);
    void uniformMatrix4fv(int loc, const float* m);

    // per-frame
    void clearColor(float r, float g, float b, float a);
    void clear(uint32_t mask);
    void viewport(int x, int y, int w, int h);
    void drawArrays(uint32_t mode, int first, int count);

private:
    int w_, h_;
    std::vector<uint8_t> color_;            // RGBA framebuffer
    float clear_[4] = {0, 0, 0, 0};
    int vp_[4];

    std::map<uint32_t, Shader>  shaders_;
    std::map<uint32_t, Program> programs_;
    std::map<uint32_t, Buffer>  buffers_;
    std::map<uint32_t, Texture> textures_;
    std::map<uint32_t, uint32_t> unit_texture_;       // texture unit -> texture id
    uint32_t active_unit_ = 0;
    std::map<uint32_t, VertexAttrib> attribs_;       // by location
    std::map<std::string, std::vector<float>> uniforms_;   // by "prog:name"
    std::map<std::pair<uint32_t,std::string>, int> attrib_locs_;

    uint32_t next_id_ = 1;
    uint32_t cur_program_ = 0;
    uint32_t bound_array_buffer_ = 0;
    uint32_t uniform_next_ = 1;
    std::map<int, std::string> uniform_loc_names_;     // loc -> "prog:name"

    void put(int x, int y, float r, float g, float b, float a);
};

} // namespace malibu::gl
