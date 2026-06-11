// tests/test_gl.cpp — MalibuGL + MalibuShader: the triangle ritual + GLSL interp.
#include <gtest/gtest.h>
#include "malibu/gl/gl.h"
#include "malibu/shader/glsl.h"

using namespace malibu::gl;

TEST(MalibuShader, EvaluatesVecConstructorsAndSwizzles) {
    malibu::shader::Program p;
    std::string log;
    ASSERT_TRUE(p.compile(
        "attribute vec3 color;\n"
        "varying vec4 outc;\n"
        "void main() { outc = vec4(color.bgr, 0.5 + 0.5); }", log)) << log;
    std::map<std::string, malibu::shader::Val> in, out;
    malibu::shader::Val c; c.n = 3; c.v[0] = 0.1f; c.v[1] = 0.2f; c.v[2] = 0.3f;
    in["color"] = c;
    ASSERT_TRUE(p.run(in, out));
    auto& o = out["outc"];
    EXPECT_NEAR(o.v[0], 0.3f, 1e-5);   // .b
    EXPECT_NEAR(o.v[1], 0.2f, 1e-5);   // .g
    EXPECT_NEAR(o.v[2], 0.1f, 1e-5);   // .r
    EXPECT_NEAR(o.v[3], 1.0f, 1e-5);
}

TEST(MalibuGL, RendersRgbTriangle) {
    Context gl(64, 64);
    auto vs = gl.createShader(GL_VERTEX_SHADER);
    gl.shaderSource(vs, "attribute vec2 position; attribute vec3 color; varying vec3 vColor;"
                        "void main(){ vColor = color; gl_Position = vec4(position, 0.0, 1.0); }");
    gl.compileShader(vs);
    ASSERT_TRUE(gl.getShaderParameter(vs, GL_COMPILE_STATUS)) << gl.getShaderInfoLog(vs);
    auto fs = gl.createShader(GL_FRAGMENT_SHADER);
    gl.shaderSource(fs, "precision mediump float; varying vec3 vColor;"
                        "void main(){ gl_FragColor = vec4(vColor, 1.0); }");
    gl.compileShader(fs);
    ASSERT_TRUE(gl.getShaderParameter(fs, GL_COMPILE_STATUS));
    auto prog = gl.createProgram();
    gl.attachShader(prog, vs); gl.attachShader(prog, fs);
    gl.linkProgram(prog); gl.useProgram(prog);

    float data[] = { 0.0f, 0.8f, 1,0,0,  -0.8f,-0.8f, 0,1,0,  0.8f,-0.8f, 0,0,1 };
    auto buf = gl.createBuffer();
    gl.bindBuffer(GL_ARRAY_BUFFER, buf);
    gl.bufferData(GL_ARRAY_BUFFER, reinterpret_cast<const uint8_t*>(data), sizeof(data));
    int posLoc = gl.getAttribLocation(prog, "position");
    int colLoc = gl.getAttribLocation(prog, "color");
    EXPECT_EQ(posLoc, 0); EXPECT_EQ(colLoc, 1);
    gl.enableVertexAttribArray(posLoc); gl.vertexAttribPointer(posLoc, 2, GL_FLOAT, false, 20, 0);
    gl.enableVertexAttribArray(colLoc); gl.vertexAttribPointer(colLoc, 3, GL_FLOAT, false, 20, 8);
    gl.viewport(0, 0, 64, 64);
    gl.clearColor(0, 0, 0, 1); gl.clear(GL_COLOR_BUFFER_BIT);
    gl.drawArrays(GL_TRIANGLES, 0, 3);

    const auto& px = gl.pixels();
    // Center-ish pixel should be a non-black, color-blended fragment.
    auto at = [&](int x, int y, int c) { return px[(y * 64 + x) * 4 + c]; };
    int cx = 32, cy = 36;
    EXPECT_GT(at(cx, cy, 0) + at(cx, cy, 1) + at(cx, cy, 2), 60);   // lit
    EXPECT_EQ(at(cx, cy, 3), 255);
    // A corner is background (outside the triangle).
    EXPECT_LT(at(2, 2, 0) + at(2, 2, 1) + at(2, 2, 2), 10);
}
