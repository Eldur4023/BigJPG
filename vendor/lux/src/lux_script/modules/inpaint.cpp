// inpaint: remove objects and watermarks with LaMa (resolution-robust large-mask inpainting), run on CPU by
// ONNX Runtime. The library is not linked: it is dlopen()ed on the first call, so the module is always
// compiled in and a missing libonnxruntime.so is an ordinary error. Only onnxruntime_c_api.h is vendored.
//
//   import inpaint
//   Json r = await inpaint.run("lama_fp32.onnx", "in.ppm", "mask.pgm", "out.ppm", { "dilate": 4 })
//   # r = {width, height, tiles}
//
// The files are binary PNM (P6 RGB image, P5 grey mask; white = remove) so the module needs no image codecs:
// the caller converts to and from them (ImageMagick). LaMa takes 512x512, so the region around the mask is cut
// out, scaled to 512 (at most 1:4; bigger masks are done in tiles), inpainted and pasted back ONLY where the
// mask was: the rest of the picture keeps its pixels. Blocks the I/O-pool thread for the whole run (seconds
// to minutes on CPU) and cannot be cancelled once started.
//
// opts (optional): dilate (px the mask grows, default 4: covers anti-aliased edges), threads (default: all),
// lib (path of libonnxruntime.so; default: the model's folder, then the system search path).
#include <lux_script/builtin_module.hpp>

#include <dlfcn.h>
#include <onnxruntime_c_api.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace lux_script {
namespace {

constexpr int kSide = 512;      // LaMa-ONNX is exported for a fixed 512x512
constexpr int kMaxScale = 4;    // a window is at most 4*512 px of picture per model call

// ── PNM ────────────────────────────────────────────────────────────────────

struct Pnm { int w = 0, h = 0, ch = 0; std::vector<unsigned char> px; };

bool read_pnm(const std::string& path, const char* magic, int ch, Pnm& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "cannot open " + path; return false; }
    std::string m; int maxv = 0;
    auto num = [&](int& v) {
        for (;;) {
            int c = f.peek();
            if (c == '#') { std::string skip; std::getline(f, skip); }
            else if (std::isspace(c)) f.get();
            else break;
        }
        return bool(f >> v);
    };
    f >> m;
    if (m != magic || !num(out.w) || !num(out.h) || !num(maxv) || maxv != 255 || out.w <= 0 || out.h <= 0 ||
        (long long)out.w * out.h > 400000000LL) { error = path + ": not a binary " + magic + " file with maxval 255"; return false; }
    f.get();
    out.ch = ch;
    out.px.resize((size_t)out.w * out.h * ch);
    f.read((char*)out.px.data(), (std::streamsize)out.px.size());
    if (!f) { error = path + ": truncated"; return false; }
    return true;
}

bool write_ppm(const std::string& path, const Pnm& img, std::string& error) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { error = "cannot write " + path; return false; }
    f << "P6\n" << img.w << " " << img.h << "\n255\n";
    f.write((const char*)img.px.data(), (std::streamsize)img.px.size());
    if (!f) { error = "cannot write " + path; return false; }
    return true;
}

// Grows the mask by r px (square element, two separable passes). Returns the bounding box in bx0..by1 (exclusive).
void dilate(Pnm& m, int r, int& bx0, int& by0, int& bx1, int& by1) {
    const int w = m.w, h = m.h;
    for (auto& v : m.px) v = v > 127 ? 255 : 0;
    if (r > 0) {
        std::vector<unsigned char> t(m.px.size(), 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (m.px[(size_t)y * w + x])
                    for (int xx = std::max(0, x - r); xx <= std::min(w - 1, x + r); ++xx) t[(size_t)y * w + xx] = 255;
        std::fill(m.px.begin(), m.px.end(), 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (t[(size_t)y * w + x])
                    for (int yy = std::max(0, y - r); yy <= std::min(h - 1, y + r); ++yy) m.px[(size_t)yy * w + x] = 255;
    }
    bx0 = w; by0 = h; bx1 = 0; by1 = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (m.px[(size_t)y * w + x]) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x + 1); by0 = std::min(by0, y); by1 = std::max(by1, y + 1); }
}

// ── ONNX Runtime ───────────────────────────────────────────────────────────

struct Ort {
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* mem = nullptr;
    std::string model, in_image, in_mask, out_name;
};

std::mutex g_mu;
Ort g_ort;      // one session kept between calls (loading 200 MB each time would dominate small jobs)

bool ok(const OrtApi* api, OrtStatus* st, std::string& error) {
    if (!st) return true;
    error = std::string("onnxruntime: ") + api->GetErrorMessage(st);
    api->ReleaseStatus(st);
    return false;
}

bool load_runtime(const std::string& model, const std::string& lib_opt, int threads, std::string& error) {
    if (g_ort.session && g_ort.model == model) return true;
    if (!g_ort.api) {
        std::vector<std::string> tries;
        if (!lib_opt.empty()) tries.push_back(lib_opt);
        auto slash = model.rfind('/');
        if (slash != std::string::npos) tries.push_back(model.substr(0, slash + 1) + "libonnxruntime.so");
        tries.push_back("libonnxruntime.so");
        tries.push_back("libonnxruntime.so.1");
        void* h = nullptr;
        for (auto& t : tries) if ((h = dlopen(t.c_str(), RTLD_NOW | RTLD_LOCAL))) break;
        if (!h) { error = "inpaint: libonnxruntime.so not found (tried: " + tries.front() + " ...)"; return false; }
        auto base = (const OrtApiBase* (*)())dlsym(h, "OrtGetApiBase");
        g_ort.api = base ? base()->GetApi(ORT_API_VERSION) : nullptr;
        if (!g_ort.api) { error = "inpaint: this libonnxruntime is older than 1.20"; return false; }
    }
    const OrtApi* api = g_ort.api;
    if (g_ort.session) { api->ReleaseSession(g_ort.session); g_ort.session = nullptr; }
    if (!g_ort.env && !ok(api, api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "lux", &g_ort.env), error)) return false;
    if (!g_ort.mem && !ok(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g_ort.mem), error)) return false;
    OrtSessionOptions* so = nullptr;
    if (!ok(api, api->CreateSessionOptions(&so), error)) return false;
    api->SetIntraOpNumThreads(so, threads);
    api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
    bool good = ok(api, api->CreateSession(g_ort.env, model.c_str(), so, &g_ort.session), error);
    api->ReleaseSessionOptions(so);
    if (!good) return false;
    OrtAllocator* al = nullptr;
    api->GetAllocatorWithDefaultOptions(&al);
    char *n0 = nullptr, *n1 = nullptr, *o0 = nullptr;
    if (!ok(api, api->SessionGetInputName(g_ort.session, 0, al, &n0), error) ||
        !ok(api, api->SessionGetInputName(g_ort.session, 1, al, &n1), error) ||
        !ok(api, api->SessionGetOutputName(g_ort.session, 0, al, &o0), error)) return false;
    g_ort.in_image = n0; g_ort.in_mask = n1; g_ort.out_name = o0;
    al->Free(al, n0); al->Free(al, n1); al->Free(al, o0);
    g_ort.model = model;
    return true;
}

// ── tiles ──────────────────────────────────────────────────────────────────

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// Box-average sampling of `k`x`k` source pixels (edge pixels repeat outside the picture).
inline float sample(const Pnm& im, int x, int y, int c) {
    return im.px[((size_t)clampi(y, 0, im.h - 1) * im.w + clampi(x, 0, im.w - 1)) * im.ch + c];
}

// One model call on the window whose top-left is (wx, wy) and side kSide*k. Pastes into `img` where `mask` is set,
// then clears those mask pixels. Returns false (with `error`) if the model fails.
bool run_window(Pnm& img, Pnm& mask, int wx, int wy, int k, std::string& error) {
    const OrtApi* api = g_ort.api;
    const size_t plane = (size_t)kSide * kSide;
    std::vector<float> in(3 * plane), mk(plane);
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x) {
            float acc[3] = {0, 0, 0}, m = 0;
            for (int dy = 0; dy < k; ++dy)
                for (int dx = 0; dx < k; ++dx) {
                    int sx = wx + x * k + dx, sy = wy + y * k + dy;
                    for (int c = 0; c < 3; ++c) acc[c] += sample(img, sx, sy, c);
                    m += (sx >= 0 && sy >= 0 && sx < img.w && sy < img.h) ? mask.px[(size_t)sy * img.w + sx] : 0;
                }
            for (int c = 0; c < 3; ++c) in[c * plane + (size_t)y * kSide + x] = acc[c] / (k * k * 255.f);
            mk[(size_t)y * kSide + x] = m / (k * k * 255.f) > 0.25f ? 1.f : 0.f;
        }
    int64_t s_img[4] = {1, 3, kSide, kSide}, s_mask[4] = {1, 1, kSide, kSide};
    OrtValue *vi = nullptr, *vm = nullptr, *vo = nullptr;
    bool good = ok(api, api->CreateTensorWithDataAsOrtValue(g_ort.mem, in.data(), in.size() * 4, s_img, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &vi), error) &&
                ok(api, api->CreateTensorWithDataAsOrtValue(g_ort.mem, mk.data(), mk.size() * 4, s_mask, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &vm), error);
    if (good) {
        const char* names[2] = {g_ort.in_image.c_str(), g_ort.in_mask.c_str()};
        const OrtValue* vals[2] = {vi, vm};
        const char* outn[1] = {g_ort.out_name.c_str()};
        good = ok(api, api->Run(g_ort.session, nullptr, names, vals, 2, outn, 1, &vo), error);
    }
    float* out = nullptr;
    if (good) good = ok(api, api->GetTensorMutableData(vo, (void**)&out), error);
    if (good) {
        // The export returns 0..255; a 0..1 export is detected by its range.
        float top = 0;
        for (size_t i = 0; i < 3 * plane; ++i) top = std::max(top, out[i]);
        const float gain = top <= 1.5f ? 255.f : 1.f;
        for (int y = 0; y < kSide * k; ++y)
            for (int x = 0; x < kSide * k; ++x) {
                int px = wx + x, py = wy + y;
                if (px < 0 || py < 0 || px >= img.w || py >= img.h || !mask.px[(size_t)py * img.w + px]) continue;
                // bilinear back from the 512 grid
                float fx = std::max(0.f, (x + 0.5f) / k - 0.5f), fy = std::max(0.f, (y + 0.5f) / k - 0.5f);
                int x0 = std::min((int)fx, kSide - 1), y0 = std::min((int)fy, kSide - 1), x1 = std::min(x0 + 1, kSide - 1), y1 = std::min(y0 + 1, kSide - 1);
                float ax = fx - x0, ay = fy - y0;
                for (int c = 0; c < 3; ++c) {
                    const float* p = out + c * plane;
                    float v = (p[(size_t)y0 * kSide + x0] * (1 - ax) + p[(size_t)y0 * kSide + x1] * ax) * (1 - ay) +
                              (p[(size_t)y1 * kSide + x0] * (1 - ax) + p[(size_t)y1 * kSide + x1] * ax) * ay;
                    img.px[((size_t)py * img.w + px) * 3 + c] = (unsigned char)clampi((int)std::lround(v * gain), 0, 255);
                }
            }
        for (int y = 0; y < kSide * k; ++y)
            for (int x = 0; x < kSide * k; ++x) {
                int px = wx + x, py = wy + y;
                if (px >= 0 && py >= 0 && px < img.w && py < img.h) mask.px[(size_t)py * img.w + px] = 0;
            }
    }
    if (vi) api->ReleaseValue(vi);
    if (vm) api->ReleaseValue(vm);
    if (vo) api->ReleaseValue(vo);
    return good;
}

Value fn_run(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const std::string model = a[0].as_str(), in_path = a[1].as_str(), mask_path = a[2].as_str(), out_path = a[3].as_str();
    int r = 4, threads = (int)std::max(1u, std::thread::hardware_concurrency());
    std::string lib;
    if (a.size() > 4 && !a[4].is_null()) {
        auto& d = a[4].as_dict();
        if (auto it = d.find("dilate"); it != d.end()) r = clampi((int)it->second.as_int(), 0, 64);
        if (auto it = d.find("threads"); it != d.end()) threads = clampi((int)it->second.as_int(), 1, 256);
        if (auto it = d.find("lib"); it != d.end()) lib = it->second.as_str();
    }
    Pnm img, mask;
    if (!read_pnm(in_path, "P6", 3, img, error) || !read_pnm(mask_path, "P5", 1, mask, error)) { error = "inpaint.run(): " + error; return Value::null(); }
    if (img.w != mask.w || img.h != mask.h) { error = "inpaint.run(): the mask and the image differ in size"; return Value::null(); }
    int bx0, by0, bx1, by1;
    dilate(mask, r, bx0, by0, bx1, by1);
    if (bx1 <= bx0) { error = "inpaint.run(): the mask is empty"; return Value::null(); }

    std::lock_guard<std::mutex> lock(g_mu);
    if (!load_runtime(model, lib, threads, error)) { error = "inpaint.run(): " + error; return Value::null(); }

    // Window size: the bounding box plus a third of context on each side, rounded up to 512*k (k <= 4).
    const int need = std::max(bx1 - bx0, by1 - by0);
    const int k = clampi((int)std::ceil(need * 1.5 / kSide), 1, kMaxScale);
    const int win = kSide * k, margin = win / 6, step = win - 2 * margin;
    int tiles = 0;
    for (int ty = by0 - margin; ty < by1 + margin; ty += step)
        for (int tx = bx0 - margin; tx < bx1 + margin; tx += step) {
            // centre a lone window on the box instead of hanging it off its corner
            int wx = tx, wy = ty;
            if (bx1 - bx0 + 2 * margin <= win) wx = (bx0 + bx1 - win) / 2;
            if (by1 - by0 + 2 * margin <= win) wy = (by0 + by1 - win) / 2;
            bool any = false;
            for (int y = std::max(0, wy + margin); y < std::min(img.h, wy + win - margin) && !any; ++y)
                for (int x = std::max(0, wx + margin); x < std::min(img.w, wx + win - margin); ++x)
                    if (mask.px[(size_t)y * img.w + x]) { any = true; break; }
            if (!any) continue;
            if (!run_window(img, mask, wx, wy, k, error)) { error = "inpaint.run(): " + error; return Value::null(); }
            ++tiles;
        }
    if (!write_ppm(out_path, img, error)) { error = "inpaint.run(): " + error; return Value::null(); }
    Value::Dict d;
    d["width"] = Value::integer(img.w);
    d["height"] = Value::integer(img.h);
    d["tiles"] = Value::integer(tiles);
    return Value::dict(std::move(d));
}

} // namespace

LUX_MODULE(inpaint, {
    {"run", "ssss|D", fn_run, /*is_async=*/true},
})

} // namespace lux_script
