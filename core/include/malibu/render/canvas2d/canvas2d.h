#pragma once
// core/include/malibu/render/canvas2d/canvas2d.h
// Canvas 2D command list.

#include <vector>
#include <cstdint>
#include <string>

namespace malibu::render::canvas2d {

enum class CommandType {
    FillRect, StrokeRect, FillText, StrokeText,
    BeginPath, MoveTo, LineTo, Arc,
    Fill, Stroke, DrawImage, ClearRect,
    Save, Restore, Translate, Scale, Rotate, Transform,
};

struct Command {
    CommandType type;
    float args[12]; // variable args
    std::string str_arg; // for drawImage src, font, etc.
};

class Canvas2DCommandList {
public:
    void fill_rect(float x, float y, float w, float h);
    void stroke_rect(float x, float y, float w, float h);
    void fill_text(std::string_view text, float x, float y);
    void stroke_text(std::string_view text, float x, float y);
    void begin_path();
    void move_to(float x, float y);
    void line_to(float x, float y);
    void arc(float x, float y, float r, float start, float end);
    void fill();
    void stroke();
    void draw_image(const std::string& src, float x, float y, float w, float h);
    void clear_rect(float x, float y, float w, float h);
    void save();
    void restore();
    void translate(float x, float y);
    void scale(float x, float y);
    void rotate(float angle);
    void transform(float a, float b, float c, float d, float e, float f);
    
    const std::vector<Command>& commands() const;
    void clear();
private:
    std::vector<Command> commands_;
};

} // namespace malibu::render::canvas2d