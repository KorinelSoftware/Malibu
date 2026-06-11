// image/webp.cpp
// WebP decode. libwebp is loaded *lazily* via dlopen so the binary has NO hard
// NEEDED dependency on libwebp.so — it launches even on hosts that lack it
// (WebP images are then skipped rather than crashing). Mirrors jpeg.cpp.
//
// WebP is ubiquitous on the modern web (Google, most CDNs, many sites serve it
// to capable UAs), so decoding it turns a large class of "missing image" boxes
// into real pictures.

#include "malibu/image/png.h"

#include <dlfcn.h>
#include <cstdlib>

namespace malibu::image {
namespace {

struct WebpApi {
    void* handle = nullptr;
    // int WebPGetInfo(const uint8_t* data, size_t size, int* w, int* h);
    using p_get_info  = int (*)(const uint8_t*, size_t, int*, int*);
    // uint8_t* WebPDecodeRGBA(const uint8_t* data, size_t size, int* w, int* h);
    using p_decode    = uint8_t* (*)(const uint8_t*, size_t, int*, int*);
    // void WebPFree(void* ptr);
    using p_free      = void (*)(void*);
    p_get_info get_info = nullptr;
    p_decode   decode   = nullptr;
    p_free     free_buf = nullptr;
    bool ok() const { return get_info && decode; }
};

const WebpApi& webp_api() {
    static WebpApi a = [] {
        WebpApi api;
        const char* names[] = {"libwebp.so.7", "libwebp.so.6", "libwebp.so"};
        for (const char* n : names) if ((api.handle = dlopen(n, RTLD_NOW | RTLD_GLOBAL))) break;
        if (!api.handle) return api;
        auto sym = [&](const char* n) { return dlsym(api.handle, n); };
        api.get_info = reinterpret_cast<WebpApi::p_get_info>(sym("WebPGetInfo"));
        api.decode   = reinterpret_cast<WebpApi::p_decode>(sym("WebPDecodeRGBA"));
        api.free_buf = reinterpret_cast<WebpApi::p_free>(sym("WebPFree"));
        return api;
    }();
    return a;
}

}  // namespace

DecodedImage decode_webp(const uint8_t* data, size_t len) {
    DecodedImage out;
    const WebpApi& a = webp_api();
    if (!a.ok() || len < 12) return out;
    int w = 0, h = 0;
    if (!a.get_info(data, len, &w, &h) || w <= 0 || h <= 0) return out;
    uint8_t* px = a.decode(data, len, &w, &h);
    if (!px) return out;
    out.width = w;
    out.height = h;
    out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    if (a.free_buf) a.free_buf(px); else std::free(px);
    out.ok = true;
    return out;
}

}  // namespace malibu::image
