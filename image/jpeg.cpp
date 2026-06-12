// image/jpeg.cpp — JPEG decode (libjpeg) to RGBA8. Outside core/.
//
// libjpeg is loaded *lazily* via dlopen so the binary has NO hard NEEDED
// dependency on libjpeg.so.62 — it launches even on hosts that lack it (JPEG
// images are simply skipped), and uses JPEG whenever the library is present
// under any common soname. This avoids cross-distro soname mismatches.
#include "malibu/image/png.h"   // shares DecodedImage

#include <csetjmp>
#include <cstdio>
#include <dlfcn.h>
#include <jpeglib.h>

namespace malibu::image {
namespace {
struct ErrMgr { jpeg_error_mgr pub; jmp_buf jmp; };
void on_error(j_common_ptr cinfo) { longjmp(reinterpret_cast<ErrMgr*>(cinfo->err)->jmp, 1); }

// Resolved libjpeg entry points (null until/unless the library loads).
struct JpegApi {
    using p_std_error      = jpeg_error_mgr* (*)(jpeg_error_mgr*);
    using p_CreateDecomp   = void (*)(j_decompress_ptr, int, size_t);
    using p_mem_src        = void (*)(j_decompress_ptr, const unsigned char*, unsigned long);
    using p_read_header    = int (*)(j_decompress_ptr, boolean);
    using p_start          = boolean (*)(j_decompress_ptr);
    using p_read_scanlines = JDIMENSION (*)(j_decompress_ptr, JSAMPARRAY, JDIMENSION);
    using p_finish         = boolean (*)(j_decompress_ptr);
    using p_destroy        = void (*)(j_decompress_ptr);

    p_std_error      std_error = nullptr;
    p_CreateDecomp   create    = nullptr;
    p_mem_src        mem_src   = nullptr;
    p_read_header    read_hdr  = nullptr;
    p_start          start     = nullptr;
    p_read_scanlines scanlines = nullptr;
    p_finish         finish    = nullptr;
    p_destroy        destroy   = nullptr;
    bool ok = false;
};

const JpegApi& jpeg_api() {
    static JpegApi api = [] {
        JpegApi a;
        const char* names[] = {"libjpeg.so.62", "libjpeg.so.8", "libjpeg.so", "libjpeg-turbo.so"};
        void* h = nullptr;
        for (const char* n : names) if ((h = dlopen(n, RTLD_NOW | RTLD_GLOBAL))) break;
        if (!h) return a;
        auto sym = [&](const char* n) { return dlsym(h, n); };
        a.std_error = reinterpret_cast<JpegApi::p_std_error>(sym("jpeg_std_error"));
        a.create    = reinterpret_cast<JpegApi::p_CreateDecomp>(sym("jpeg_CreateDecompress"));
        a.mem_src   = reinterpret_cast<JpegApi::p_mem_src>(sym("jpeg_mem_src"));
        a.read_hdr  = reinterpret_cast<JpegApi::p_read_header>(sym("jpeg_read_header"));
        a.start     = reinterpret_cast<JpegApi::p_start>(sym("jpeg_start_decompress"));
        a.scanlines = reinterpret_cast<JpegApi::p_read_scanlines>(sym("jpeg_read_scanlines"));
        a.finish    = reinterpret_cast<JpegApi::p_finish>(sym("jpeg_finish_decompress"));
        a.destroy   = reinterpret_cast<JpegApi::p_destroy>(sym("jpeg_destroy_decompress"));
        a.ok = a.std_error && a.create && a.mem_src && a.read_hdr && a.start && a.scanlines && a.finish && a.destroy;
        return a;
    }();
    return api;
}
}  // namespace

DecodedImage decode_jpeg(const uint8_t* data, size_t len) {
    DecodedImage out;
    if (!data || len < 2 || data[0] != 0xFF || data[1] != 0xD8) return out;  // SOI marker
    const JpegApi& jp = jpeg_api();
    if (!jp.ok) return out;   // libjpeg not available — skip JPEG gracefully

    jpeg_decompress_struct cinfo;
    ErrMgr jerr;
    cinfo.err = jp.std_error(&jerr.pub);
    jerr.pub.error_exit = on_error;
    if (setjmp(jerr.jmp)) { jp.destroy(&cinfo); return DecodedImage{}; }

    jp.create(&cinfo, JPEG_LIB_VERSION, sizeof(cinfo));
    jp.mem_src(&cinfo, const_cast<unsigned char*>(data), static_cast<unsigned long>(len));
    if (jp.read_hdr(&cinfo, TRUE) != JPEG_HEADER_OK) { jp.destroy(&cinfo); return out; }
    cinfo.out_color_space = JCS_RGB;
    jp.start(&cinfo);

    int w = static_cast<int>(cinfo.output_width), h = static_cast<int>(cinfo.output_height);
    int comps = cinfo.output_components;
    out.width = w; out.height = h;
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 255);

    std::vector<uint8_t> row(static_cast<size_t>(w) * comps);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* rp = row.data();
        jp.scanlines(&cinfo, &rp, 1);
        size_t y = cinfo.output_scanline - 1;
        for (int x = 0; x < w; ++x) {
            size_t di = (y * w + x) * 4, si = static_cast<size_t>(x) * comps;
            out.rgba[di] = row[si]; out.rgba[di + 1] = row[si + (comps > 1 ? 1 : 0)];
            out.rgba[di + 2] = row[si + (comps > 2 ? 2 : 0)]; out.rgba[di + 3] = 255;
        }
    }
    jp.finish(&cinfo);
    jp.destroy(&cinfo);
    out.ok = true;
    return out;
}

// Detects the format by magic and decodes (PNG / JPEG / WebP / SVG / GIF).
DecodedImage decode_image(const uint8_t* data, size_t len) {
    if (len >= 8 && data[0] == 0x89 && data[1] == 0x50) return decode_png(data, len);   // \x89PNG
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xD8) return decode_jpeg(data, len);  // JPEG SOI
    if (len >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P')          // RIFF....WEBP
        return decode_webp(data, len);
    if (len >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
        (data[4] == '7' || data[4] == '9') && data[5] == 'a')                            // GIF87a / GIF89a
        return decode_gif(data, len);
    if (len >= 4) {
        if (data[0] == '<' && data[1] == 's' && data[2] == 'v' && data[3] == 'g') return decode_svg(data, len, 100, 100); // <svg
        if (data[0] == '<' && data[1] == '?' && data[2] == 'x' && data[3] == 'm') {
            size_t svg_pos = 0;
            while (svg_pos + 4 < len && !(data[svg_pos] == '<' && data[svg_pos+1] == 's' && data[svg_pos+2] == 'v' && data[svg_pos+3] == 'g')) svg_pos++;
            if (svg_pos + 4 < len) return decode_svg(data + svg_pos, len - svg_pos, 100, 100); // <?xml ... <svg
        }
    }
    return decode_png(data, len);  // best effort
}

} // namespace malibu::image
