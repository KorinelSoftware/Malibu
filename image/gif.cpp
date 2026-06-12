// image/gif.cpp — GIF decode (libgif) to RGBA8. Outside core/.
//
// libgif is loaded *lazily* via dlopen so the binary has NO hard NEEDED
// dependency on libgif.so — it launches even on hosts that lack it (GIF
// images are simply skipped), and uses GIF whenever the library is present
// under any common soname. This avoids cross-distro soname mismatches.
#include "malibu/image/png.h"   // shares DecodedImage

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <gif_lib.h>

namespace malibu::image {

namespace {

struct GifApi {
    using p_DGifOpen = GifFileType* (*)(void*, InputFunc, int*);
    using p_DGifSlurp = int (*)(GifFileType*);
    using p_DGifCloseFile = int (*)(GifFileType*, int*);
    using p_DGifGetLine = int (*)(GifFileType*, GifPixelType*, int);

    p_DGifOpen dgif_open = nullptr;
    p_DGifSlurp dgif_slurp = nullptr;
    p_DGifCloseFile dgif_close = nullptr;
    p_DGifGetLine dgif_get_line = nullptr;
    bool ok = false;
};

const GifApi& gif_api() {
    static GifApi api = [] {
        GifApi a;
        const char* names[] = {"libgif.so.7", "libgif.so", "libgif.so.6", "libgif.so.5"};
        void* h = nullptr;
        for (const char* n : names) if ((h = dlopen(n, RTLD_NOW | RTLD_GLOBAL))) break;
        if (!h) return a;
        auto sym = [&](const char* n) { return dlsym(h, n); };
        a.dgif_open = reinterpret_cast<GifApi::p_DGifOpen>(sym("DGifOpen"));
        a.dgif_slurp = reinterpret_cast<GifApi::p_DGifSlurp>(sym("DGifSlurp"));
        a.dgif_close = reinterpret_cast<GifApi::p_DGifCloseFile>(sym("DGifCloseFile"));
        a.dgif_get_line = reinterpret_cast<GifApi::p_DGifGetLine>(sym("DGifGetLine"));
        a.ok = a.dgif_open && a.dgif_slurp && a.dgif_close && a.dgif_get_line;
        return a;
    }();
    return api;
}

struct MemSource { const uint8_t* p; size_t left; };
static int mem_read(GifFileType* gif, GifByteType* buf, int len) {
    MemSource* s = static_cast<MemSource*>(gif->UserData);
    int k = len < static_cast<int>(s->left) ? len : static_cast<int>(s->left);
    if (k > 0) { std::memcpy(buf, s->p, k); s->p += k; s->left -= k; }
    return k;
}

} // namespace

DecodedImage decode_gif(const uint8_t* data, size_t len) {
    DecodedImage out;
    const GifApi& gi = gif_api();
    if (!gi.ok) return out;   // libgif not available — skip GIF gracefully

    if (!data || len < 6) return out;
    if (!(data[0]=='G' && data[1]=='I' && data[2]=='F' && data[3]=='8' && (data[4]=='7' || data[4]=='9') && data[5]=='a')) return out;

    MemSource src{data, len};
    int err = 0;
    GifFileType* gif = gi.dgif_open(&src, mem_read, &err);
    if (!gif) return out;

    if (gi.dgif_slurp(gif) != GIF_OK) {
        gi.dgif_close(gif, &err);
        return out;
    }

    int w = gif->SWidth;
    int h = gif->SHeight;
    if (w <= 0 || h <= 0) {
        gi.dgif_close(gif, &err);
        return out;
    }

    out.width = w;
    out.height = h;
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 0);

    // Simple frame decode (first frame only, no disposal/animation handling)
    ColorMapObject* cmap = gif->Image.ColorMap ? gif->Image.ColorMap : gif->SColorMap;
    if (!cmap) {
        gi.dgif_close(gif, &err);
        return out;
    }

    std::vector<uint8_t> line(w);
    for (int y = 0; y < h; ++y) {
        if (gi.dgif_get_line(gif, line.data(), w) != GIF_OK) break;
        size_t row = static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            GifPixelType idx = line[x];
            if (idx < cmap->ColorCount) {
                GifColorType c = cmap->Colors[idx];
                out.rgba[row + x * 4] = c.Red;
                out.rgba[row + x * 4 + 1] = c.Green;
                out.rgba[row + x * 4 + 2] = c.Blue;
                out.rgba[row + x * 4 + 3] = 255;
            }
        }
    }

    gi.dgif_close(gif, &err);
    out.ok = true;
    return out;
}

} // namespace malibu::image