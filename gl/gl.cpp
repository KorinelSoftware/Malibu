// gl/gl.cpp — MalibuGL: WebGL-1 state machine + triangle rasterizer over
// MalibuShader. Renders into an RGBA bitmap.
#include "malibu/gl/gl.h"
#include "malibu/shader/glsl.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace malibu::gl {
using SP = shader::Program;   // (gl::Program is a different type — the GL program object)
using shader::Val;

Context::Context(int width, int height) : w_(width), h_(height), color_(static_cast<size_t>(width)*height*4, 0) {
    vp_[0] = 0; vp_[1] = 0; vp_[2] = width; vp_[3] = height;
}
Context::~Context() = default;

uint32_t Context::createShader(uint32_t type) { uint32_t id = next_id_++; shaders_[id] = Shader{type, "", false, "", nullptr}; return id; }
void Context::shaderSource(uint32_t s, const std::string& src) { if (shaders_.count(s)) shaders_[s].src = src; }
void Context::compileShader(uint32_t s) {
    auto it = shaders_.find(s); if (it == shaders_.end()) return;
    auto prog = std::make_shared<SP>();
    std::string log;
    it->second.compiled = prog->compile(it->second.src, log);
    it->second.log = log;
    it->second.program = prog;
}
bool Context::getShaderParameter(uint32_t s, uint32_t) { return shaders_.count(s) && shaders_[s].compiled; }
std::string Context::getShaderInfoLog(uint32_t s) { return shaders_.count(s) ? shaders_[s].log : ""; }

uint32_t Context::createProgram() { uint32_t id = next_id_++; programs_[id] = Program{}; return id; }
void Context::attachShader(uint32_t p, uint32_t s) { if (programs_.count(p)) programs_[p].shaders.push_back(s); }
void Context::linkProgram(uint32_t p) {
    auto it = programs_.find(p); if (it == programs_.end()) return;
    it->second.linked = true;
    for (uint32_t sid : it->second.shaders) {
        auto& sh = shaders_[sid];
        if (sh.type == GL_VERTEX_SHADER) it->second.vs = sh.program;
        if (sh.type == GL_FRAGMENT_SHADER) it->second.fs = sh.program;
        if (!sh.compiled) it->second.linked = false;
    }
}
bool Context::getProgramParameter(uint32_t p, uint32_t) { return programs_.count(p) && programs_[p].linked; }
std::string Context::getProgramInfoLog(uint32_t p) { return programs_.count(p) ? programs_[p].log : ""; }
void Context::useProgram(uint32_t p) { cur_program_ = p; }

uint32_t Context::createBuffer() { uint32_t id = next_id_++; buffers_[id] = Buffer{}; return id; }
void Context::bindBuffer(uint32_t target, uint32_t buf) { if (target == GL_ARRAY_BUFFER) bound_array_buffer_ = buf; }
void Context::bufferData(uint32_t, const uint8_t* bytes, size_t len) {
    if (!bound_array_buffer_) return;
    buffers_[bound_array_buffer_].data.assign(bytes, bytes + len);
}
int Context::getAttribLocation(uint32_t p, const std::string& name) {
    auto key = std::make_pair(p, name);
    auto it = attrib_locs_.find(key);
    if (it != attrib_locs_.end()) return it->second;
    // Assign locations in declaration order of the vertex shader's attributes.
    if (programs_.count(p) && programs_[p].vs) {
        auto vs = std::static_pointer_cast<SP>(programs_[p].vs);
        const auto& attrs = vs->attributes();
        for (size_t i = 0; i < attrs.size(); ++i) if (attrs[i] == name) { attrib_locs_[key] = (int)i; return (int)i; }
    }
    return -1;
}
void Context::enableVertexAttribArray(uint32_t loc) { attribs_[loc].enabled = true; }
void Context::vertexAttribPointer(uint32_t loc, int size, uint32_t, bool, int stride, int offset) {
    auto& a = attribs_[loc]; a.size = size; a.buffer = bound_array_buffer_; a.stride = stride; a.offset = offset;
}

uint32_t Context::createTexture() { uint32_t id = next_id_++; textures_[id] = Texture{}; return id; }
void Context::activeTexture(uint32_t unit) { active_unit_ = unit >= GL_TEXTURE0 ? unit - GL_TEXTURE0 : unit; }
void Context::bindTexture(uint32_t target, uint32_t tex) { if (target == GL_TEXTURE_2D) unit_texture_[active_unit_] = tex; }
void Context::texImage2D(int w, int h, const uint8_t* rgba, size_t len) {
    uint32_t tex = unit_texture_.count(active_unit_) ? unit_texture_[active_unit_] : 0;
    if (!tex) return;
    Texture& t = textures_[tex];
    t.w = w; t.h = h; t.rgba.assign(rgba, rgba + len);
    t.rgba.resize(static_cast<size_t>(w) * h * 4, 255);
}
void Context::texParameteri(uint32_t pname, int value) {
    uint32_t tex = unit_texture_.count(active_unit_) ? unit_texture_[active_unit_] : 0;
    if (tex && (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER))
        textures_[tex].linear = (value == GL_LINEAR);
}

int Context::getUniformLocation(uint32_t p, const std::string& name) {
    std::string key = std::to_string(p) + ":" + name;
    for (auto& [loc, n] : uniform_loc_names_) if (n == key) return loc;
    int loc = (int)uniform_next_++; uniform_loc_names_[loc] = key; return loc;
}
void Context::uniform1f(int loc, float x) { uniforms_[uniform_loc_names_[loc]] = {x}; }
void Context::uniform2f(int loc, float x, float y) { uniforms_[uniform_loc_names_[loc]] = {x, y}; }
void Context::uniform3f(int loc, float x, float y, float z) { uniforms_[uniform_loc_names_[loc]] = {x, y, z}; }
void Context::uniform4f(int loc, float x, float y, float z, float w) { uniforms_[uniform_loc_names_[loc]] = {x, y, z, w}; }
void Context::uniform1i(int loc, int x) { uniforms_[uniform_loc_names_[loc]] = {(float)x}; }
void Context::uniformMatrix4fv(int loc, const float* m) { uniforms_[uniform_loc_names_[loc]] = std::vector<float>(m, m + 16); }

void Context::clearColor(float r, float g, float b, float a) { clear_[0]=r; clear_[1]=g; clear_[2]=b; clear_[3]=a; }
void Context::clear(uint32_t mask) {
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;
    uint8_t r = (uint8_t)std::clamp(clear_[0]*255.f,0.f,255.f), g = (uint8_t)std::clamp(clear_[1]*255.f,0.f,255.f),
            b = (uint8_t)std::clamp(clear_[2]*255.f,0.f,255.f), a = (uint8_t)std::clamp(clear_[3]*255.f,0.f,255.f);
    for (size_t i = 0; i + 3 < color_.size(); i += 4) { color_[i]=r; color_[i+1]=g; color_[i+2]=b; color_[i+3]=a; }
}
void Context::viewport(int x, int y, int w, int h) { vp_[0]=x; vp_[1]=y; vp_[2]=w; vp_[3]=h; }

void Context::put(int x, int y, float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    size_t i = (static_cast<size_t>(y) * w_ + x) * 4;
    float da = color_[i+3]/255.f;
    float oa = a + da*(1-a);
    auto ch = [&](int k, float sc){ float dc = color_[i+k]/255.f; float oc = oa<=0?0:(sc*a + dc*da*(1-a))/oa; color_[i+k] = (uint8_t)std::clamp(oc*255.f,0.f,255.f); };
    ch(0,r); ch(1,g); ch(2,b);
    color_[i+3] = (uint8_t)std::clamp(oa*255.f,0.f,255.f);
}

void Context::drawArrays(uint32_t mode, int first, int count) {
    auto pit = programs_.find(cur_program_);
    if (pit == programs_.end() || !pit->second.vs || !pit->second.fs) return;
    auto vs = std::static_pointer_cast<SP>(pit->second.vs);
    auto fs = std::static_pointer_cast<SP>(pit->second.fs);

    // Common uniforms for this program (strip the "prog:" key prefix).
    std::map<std::string, Val> uniforms;
    std::string pref = std::to_string(cur_program_) + ":";
    for (auto& [key, vals] : uniforms_) {
        if (key.rfind(pref, 0) != 0) continue;
        std::string name = key.substr(pref.size());
        Val v; v.n = std::min<int>((int)vals.size(), 4);
        if (vals.size() == 16) { v.mat = true; for (int i = 0; i < 16; i++) v.v[i] = vals[i]; }
        else for (int i = 0; i < v.n; i++) v.v[i] = vals[i];
        uniforms[name] = v;
    }

    const auto& attrNames = vs->attributes();

    // texture2D sampler over bound textures (nearest, v-flipped, clamped).
    SP::Sampler sampler = [this](int unit, float u, float v) -> Val {
        Val white; white.n = 4; white.v[0]=white.v[1]=white.v[2]=white.v[3]=1;
        auto uit = unit_texture_.find((uint32_t)unit);
        if (uit == unit_texture_.end()) return white;
        auto tit = textures_.find(uit->second);
        if (tit == textures_.end() || tit->second.rgba.empty() || tit->second.w == 0) return white;
        const Texture& t = tit->second;
        int x = (int)(u * t.w), y = (int)((1.0f - v) * t.h);
        x = std::max(0, std::min(t.w - 1, x)); y = std::max(0, std::min(t.h - 1, y));
        size_t i = (static_cast<size_t>(y) * t.w + x) * 4;
        Val r; r.n = 4;
        for (int k = 0; k < 4; k++) r.v[k] = t.rgba[i + k] / 255.f;
        return r;
    };

    // Read attribute `loc` for vertex `vtx` from its bound buffer.
    auto read_attr = [&](int loc, int vtx) -> Val {
        Val r; auto ait = attribs_.find(loc);
        if (ait == attribs_.end() || !ait->second.enabled) return r;
        const VertexAttrib& va = ait->second;
        auto bit = buffers_.find(va.buffer);
        if (bit == buffers_.end()) return r;
        const uint8_t* base = bit->second.data.data();
        int stride = va.stride ? va.stride : va.size * 4;
        size_t off = (size_t)va.offset + (size_t)vtx * stride;
        r.n = va.size;
        for (int i = 0; i < va.size; i++) {
            if (off + i*4 + 4 <= bit->second.data.size()) std::memcpy(&r.v[i], base + off + i*4, 4);
        }
        return r;
    };

    struct VOut { float clip[4]; std::map<std::string, Val> varys; };
    auto run_vertex = [&](int vtx) -> VOut {
        std::map<std::string, Val> in = uniforms;
        for (size_t l = 0; l < attrNames.size(); ++l) in[attrNames[l]] = read_attr((int)l, vtx);
        std::map<std::string, Val> out;
        vs->run(in, out);
        VOut vo;
        Val pos = out.count("gl_Position") ? out["gl_Position"] : Val{};
        for (int i = 0; i < 4; i++) vo.clip[i] = pos.v[i];
        if (pos.n < 4 && !pos.mat) vo.clip[3] = pos.n >= 4 ? pos.v[3] : (pos.v[3] == 0 ? 1.f : pos.v[3]);
        for (auto& vn : vs->varyings()) if (out.count(vn)) vo.varys[vn] = out[vn];
        return vo;
    };

    auto to_screen = [&](const float clip[4], float& sx, float& sy) {
        float w = clip[3] == 0 ? 1 : clip[3];
        float ndcx = clip[0]/w, ndcy = clip[1]/w;
        sx = (ndcx*0.5f + 0.5f) * vp_[2] + vp_[0];
        sy = (1.0f - (ndcy*0.5f + 0.5f)) * vp_[3] + vp_[1];
    };

    auto raster_tri = [&](VOut& A, VOut& B, VOut& C) {
        float ax, ay, bx, by, cx, cy;
        to_screen(A.clip, ax, ay); to_screen(B.clip, bx, by); to_screen(C.clip, cx, cy);
        int minx = std::max(0, (int)std::floor(std::min({ax,bx,cx})));
        int maxx = std::min(w_-1, (int)std::ceil(std::max({ax,bx,cx})));
        int miny = std::max(0, (int)std::floor(std::min({ay,by,cy})));
        int maxy = std::min(h_-1, (int)std::ceil(std::max({ay,by,cy})));
        float area = (bx-ax)*(cy-ay) - (cx-ax)*(by-ay);
        if (std::fabs(area) < 1e-6f) return;
        for (int y = miny; y <= maxy; ++y) for (int x = minx; x <= maxx; ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((bx-px)*(cy-py) - (cx-px)*(by-py)) / area;
            float w1 = ((cx-px)*(ay-py) - (ax-px)*(cy-py)) / area;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            // Interpolate varyings and run the fragment shader.
            std::map<std::string, Val> fin = uniforms;
            for (auto& [name, va] : A.varys) {
                Val v; v.n = va.n; v.mat = va.mat;
                const Val& vb = B.varys.count(name) ? B.varys[name] : va;
                const Val& vc = C.varys.count(name) ? C.varys[name] : va;
                for (int i = 0; i < (va.mat?16:va.n); i++) v.v[i] = w0*va.v[i] + w1*vb.v[i] + w2*vc.v[i];
                fin[name] = v;
            }
            std::map<std::string, Val> fout;
            fs->run(fin, fout, sampler);
            Val col = fout.count("gl_FragColor") ? fout["gl_FragColor"] : Val{};
            put(x, y, col.v[0], col.v[1], col.v[2], col.n >= 4 ? col.v[3] : 1.f);
        }
    };

    std::vector<VOut> verts;
    for (int i = 0; i < count; ++i) verts.push_back(run_vertex(first + i));
    if (mode == GL_TRIANGLES) {
        for (size_t i = 0; i + 2 < verts.size(); i += 3) raster_tri(verts[i], verts[i+1], verts[i+2]);
    } else if (mode == GL_TRIANGLE_STRIP) {
        for (size_t i = 0; i + 2 < verts.size(); ++i) raster_tri(verts[i], verts[i+1], verts[i+2]);
    } else if (mode == GL_TRIANGLE_FAN) {
        for (size_t i = 1; i + 1 < verts.size(); ++i) raster_tri(verts[0], verts[i], verts[i+1]);
    }
}

} // namespace malibu::gl
