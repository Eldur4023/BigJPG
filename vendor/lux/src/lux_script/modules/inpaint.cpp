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
// inpaint.video(model, "in.mp4", "mask.pgm", "out.mp4", opts) cleans a clip. The mask (full frame size) marks the
// thing on the frame at opts.t seconds; it is followed through every frame (a mark that moves is found again in
// each one: grey-level correlation, small first, then refined at full size) and cleaned in each. opts.mode "lama"
// (default: the model, slow) or "fast" (no network: edge-to-centre average fill, real time). ffmpeg and ffprobe do
// the decoding and encoding (opts.ffmpeg, opts.ffprobe); the audio is kept. opts.progress is a file that gets the
// percentage; the call stops (error "cancelled") when the file opts.cancel appears. All the frames of the
// clip are held at a reduced size to follow the mark (~400 MB at most, twice while the background is removed), so opts.max_frames (default 6000) bounds it.
//   r = {width, height, frames, fps, followed, tiles}   (followed: frames where the mark was found)
//
// opts (optional): dilate (px the mask grows, default 4: covers anti-aliased edges), threads (default: all),
// lib (path of libonnxruntime.so; default: the model's folder, then the system search path).
#include <lux_script/builtin_module.hpp>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <onnxruntime_c_api.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

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

// Inpaints `img` where `mask` (binary, already dilated; bbox bx0..by1 exclusive) is set, through windows of the
// model. Caller holds g_mu and has loaded the runtime. `tiles` counts the model calls.
bool inpaint_mask(Pnm& img, Pnm& mask, int bx0, int by0, int bx1, int by1, int& tiles, std::string& error) {
    // Window size: the bounding box plus a third of context on each side, rounded up to 512*k (k <= 4).
    const int need = std::max(bx1 - bx0, by1 - by0);
    const int k = clampi((int)std::ceil(need * 1.5 / kSide), 1, kMaxScale);
    const int win = kSide * k, margin = win / 6, step = win - 2 * margin;
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
            if (!run_window(img, mask, wx, wy, k, error)) return false;
            ++tiles;
        }
    return true;
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
    int tiles = 0;
    if (!inpaint_mask(img, mask, bx0, by0, bx1, by1, tiles, error)) { error = "inpaint.run(): " + error; return Value::null(); }
    if (!write_ppm(out_path, img, error)) { error = "inpaint.run(): " + error; return Value::null(); }
    Value::Dict d;
    d["width"] = Value::integer(img.w);
    d["height"] = Value::integer(img.h);
    d["tiles"] = Value::integer(tiles);
    return Value::dict(std::move(d));
}

// ── video ──────────────────────────────────────────────────────────────────
// ffmpeg is driven through pipes (no shell: every argument is one argv entry). Two decodes: a small grey one to
// follow the mark through the whole clip, then a full-size one whose frames are cleaned and sent to the encoder.

struct Child { pid_t pid = -1; int rd = -1, wr = -1; };

bool spawn(const std::vector<std::string>& argv, bool pipe_out, bool pipe_in, Child& c, std::string& error) {
    int po[2] = {-1, -1}, pi[2] = {-1, -1};
    if ((pipe_out && pipe2(po, O_CLOEXEC) != 0) || (pipe_in && pipe2(pi, O_CLOEXEC) != 0)) { error = "cannot create a pipe"; return false; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (pipe_in) posix_spawn_file_actions_adddup2(&fa, pi[0], 0); else posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (pipe_out) posix_spawn_file_actions_adddup2(&fa, po[1], 1); else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);   // nothing of the server leaks into ffmpeg
    std::vector<char*> args;
    for (auto& s : argv) args.push_back(const_cast<char*>(s.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0].c_str(), &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (po[1] >= 0) close(po[1]);
    if (pi[0] >= 0) close(pi[0]);
    if (rc != 0) {
        if (po[0] >= 0) close(po[0]);
        if (pi[1] >= 0) close(pi[1]);
        error = "cannot run " + argv[0] + ": " + std::strerror(rc);
        return false;
    }
    c.pid = pid; c.rd = po[0]; c.wr = pi[1];
    return true;
}

// Closes the pipes, optionally kills, waits. Returns the exit code (-1 if it did not exit normally).
int reap(Child& c, bool kill_it) {
    if (c.rd >= 0) close(c.rd);
    if (c.wr >= 0) close(c.wr);
    c.rd = c.wr = -1;
    if (c.pid <= 0) return -1;
    if (kill_it) kill(c.pid, SIGKILL);
    int st = 0;
    waitpid(c.pid, &st, 0);
    c.pid = -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// 1 = n bytes read, 0 = clean EOF before the first byte, -1 = short read or error.
int read_n(int fd, unsigned char* p, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r > 0) got += (size_t)r;
        else if (r == 0) break;
        else if (errno != EINTR) return -1;
    }
    return got == n ? 1 : got == 0 ? 0 : -1;
}

bool write_n(int fd, const unsigned char* p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w > 0) { p += w; n -= (size_t)w; }
        else if (w < 0 && errno != EINTR) return false;
    }
    return true;
}

std::string slurp(int fd) {
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r > 0) out.append(buf, (size_t)r);
        else if (r == 0 || errno != EINTR) break;
    }
    return out;
}

// Reads one binary P6 picture from a pipe.
bool read_ppm_fd(int fd, Pnm& out) {
    auto token = [&](std::string& t) {
        t.clear();
        unsigned char c;
        for (;;) {
            if (read_n(fd, &c, 1) != 1) return false;
            if (!std::isspace(c)) break;
        }
        do { t += (char)c; if (read_n(fd, &c, 1) != 1) return false; } while (!std::isspace(c));
        return true;
    };
    std::string m, w, h, mv;
    if (!token(m) || m != "P6" || !token(w) || !token(h) || !token(mv) || mv != "255") return false;
    out.w = std::atoi(w.c_str()); out.h = std::atoi(h.c_str()); out.ch = 3;
    if (out.w <= 0 || out.h <= 0 || (long long)out.w * out.h > 400000000LL) return false;
    out.px.resize((size_t)out.w * out.h * 3);
    return read_n(fd, out.px.data(), out.px.size()) == 1;
}

// strtod follows the process locale (GTK sets it: "69,3" in Spanish), ffprobe always prints a dot.
double parse_num(const std::string& s) {
    double v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}

double parse_rate(const std::string& s) {
    auto slash = s.find('/');
    double n = parse_num(s.substr(0, slash)), d = slash == std::string::npos ? 1.0 : parse_num(s.substr(slash + 1));
    return d > 0 ? n / d : 0;
}

// ── tracking: weighted zero-mean normalised cross-correlation on grey frames ──
// The template is the grey level of the marked pixels only (background around a moving mark changes).

struct Tmpl { int bw = 0, bh = 0; std::vector<int> dx, dy; std::vector<float> tc; double tvar = 0; };
struct Hit { int x = 0, y = 0; double s = -1; };
using Pts = std::vector<std::pair<int, int>>;

// bx/by receive the bounding-box origin of `pts`. At most `max_pts` of them are kept (an even subsample).
Tmpl make_tmpl(const unsigned char* g, int fw, const Pts& pts, int max_pts, int& bx, int& by) {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (auto& p : pts) { x0 = std::min(x0, p.first); y0 = std::min(y0, p.second); x1 = std::max(x1, p.first); y1 = std::max(y1, p.second); }
    Tmpl t;
    t.bw = x1 - x0 + 1; t.bh = y1 - y0 + 1; bx = x0; by = y0;
    const size_t stride = std::max<size_t>(1, (pts.size() + max_pts - 1) / max_pts);
    double sum = 0;
    for (size_t i = 0; i < pts.size(); i += stride) {
        t.dx.push_back(pts[i].first - x0); t.dy.push_back(pts[i].second - y0);
        float v = g[(size_t)pts[i].second * fw + pts[i].first];
        t.tc.push_back(v); sum += v;
    }
    const double mean = sum / t.tc.size();
    for (auto& v : t.tc) { v -= (float)mean; t.tvar += (double)v * v; }
    return t;
}

double zncc(const unsigned char* g, int fw, const Tmpl& t, int px, int py) {
    const size_t n = t.tc.size();
    double si = 0, sii = 0, sit = 0;
    for (size_t k = 0; k < n; ++k) {
        double v = g[(size_t)(py + t.dy[k]) * fw + px + t.dx[k]];
        si += v; sii += v * v; sit += v * t.tc[k];
    }
    const double vi = sii - si * si / n;
    if (vi < n * 0.5 || t.tvar < n * 0.5) return -1;   // flat on either side: nothing to correlate
    return sit / std::sqrt(vi * t.tvar);
}

// Best position of the template's top-left corner with x in [x0,x1], y in [y0,y1] (clipped so it fits the frame).
Hit search(const unsigned char* g, int fw, int fh, const Tmpl& t, int x0, int x1, int y0, int y1, int stride) {
    Hit best;
    x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(fw - t.bw, x1); y1 = std::min(fh - t.bh, y1);
    for (int y = y0; y <= y1; y += stride)
        for (int x = x0; x <= x1; x += stride) {
            double s = zncc(g, fw, t, x, y);
            if (s > best.s) { best.s = s; best.x = x; best.y = y; }
        }
    return best;
}

struct Track { int x = 0, y = 0; bool held = true; };

// Follows the mark from frame `at` (where it sits at bx,by) to both ends. A frame whose best match scores under
// 0.5 keeps the predicted position (the mark may be hidden for a moment); after three such frames the whole
// frame is searched, which finds a mark that jumped (a logo that hops between corners).
std::vector<Track> track_all(const std::vector<std::vector<unsigned char>>& fr, int fw, int fh, int at, const Tmpl& t, int bx, int by,
                             const std::function<bool(int)>& alive) {
    std::vector<Track> out(fr.size());
    out[at] = { bx, by, false };
    const int R = 8;
    for (int dir : {1, -1}) {
        double px = bx, py = by, vx = 0, vy = 0;
        int lost = 0;
        for (int i = at + dir; i >= 0 && i < (int)fr.size(); i += dir) {
            if (!alive(i)) return out;
            const unsigned char* g = fr[i].data();
            int cx = (int)std::lround(px + vx), cy = (int)std::lround(py + vy);
            cx = clampi(cx, 0, std::max(0, fw - t.bw)); cy = clampi(cy, 0, std::max(0, fh - t.bh));
            Hit h = search(g, fw, fh, t, cx - R, cx + R, cy - R, cy + R, 1);
            if (h.s >= 0.5) {
                vx = 0.5 * vx + 0.5 * (h.x - px); vy = 0.5 * vy + 0.5 * (h.y - py);
                px = h.x; py = h.y; lost = 0;
                out[i] = { h.x, h.y, false };
            } else {
                ++lost; px = cx; py = cy; vx *= 0.7; vy *= 0.7;
                if (lost >= 3 && lost % 3 == 0) {
                    Hit gl = search(g, fw, fh, t, 0, fw, 0, fh, 2);
                    if (gl.s > 0) gl = search(g, fw, fh, t, gl.x - 1, gl.x + 1, gl.y - 1, gl.y + 1, 1);
                    if (gl.s >= 0.75) { px = gl.x; py = gl.y; vx = vy = 0; lost = 0; out[i] = { gl.x, gl.y, false }; continue; }
                }
                out[i] = { cx, cy, true };
            }
        }
    }
    return out;
}

// What a moving mark adds to a frame: the frame minus its temporal median over +-24 frames (the median forgets
// whatever covers a pixel for less than half of those frames, so a still background cancels and the mark stays;
// the mark itself must move at least a few px per second to be forgotten). Stored as 128 + difference.
void remove_background(std::vector<std::vector<unsigned char>>& fr) {
    const int N = (int)fr.size();
    if (N < 3) return;
    static const int offs[9] = {-24, -16, -8, -4, 0, 4, 8, 16, 24};
    const size_t px = fr[0].size();
    std::vector<std::vector<unsigned char>> out(N, std::vector<unsigned char>(px));
    for (int i = 0; i < N; ++i) {
        const unsigned char* src[9];
        for (int k = 0; k < 9; ++k) {
            int j = i + offs[k];
            if (j < 0 || j >= N) j = i - offs[k];   // reflect at the ends
            src[k] = fr[clampi(j, 0, N - 1)].data();
        }
        for (size_t p = 0; p < px; ++p) {
            unsigned char v[9];
            for (int k = 0; k < 9; ++k) v[k] = src[k][p];
            std::nth_element(v, v + 4, v + 9);
            out[i][p] = (unsigned char)clampi(128 + (int)fr[i][p] - (int)v[4], 0, 255);
        }
    }
    fr.swap(out);
}

void to_grey(const Pnm& im, std::vector<unsigned char>& g) {
    g.resize((size_t)im.w * im.h);
    for (size_t i = 0; i < g.size(); ++i) g[i] = (unsigned char)((im.px[i * 3] * 77 + im.px[i * 3 + 1] * 151 + im.px[i * 3 + 2] * 28) >> 8);
}

// Fast mode: no network. Unknown pixels take, ring by ring from the edge inward, the weighted mean of the known
// ones within 2 px. Smooth and blurry, but real time.
void fill_classic(Pnm& img, const Pnm& mask, int bx0, int by0, int bx1, int by1) {
    const int rx0 = std::max(0, bx0 - 2), ry0 = std::max(0, by0 - 2), rx1 = std::min(img.w, bx1 + 2), ry1 = std::min(img.h, by1 + 2);
    const int rw = rx1 - rx0, rh = ry1 - ry0;
    std::vector<char> unk((size_t)rw * rh, 0);
    std::vector<int> todo;
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x)
            if (mask.px[(size_t)(ry0 + y) * img.w + rx0 + x]) { unk[(size_t)y * rw + x] = 1; todo.push_back(y * rw + x); }
    std::vector<int> next, done;
    std::vector<unsigned char> vals;
    while (!todo.empty()) {
        next.clear(); done.clear(); vals.clear();
        for (int idx : todo) {
            const int x = idx % rw, y = idx / rw;
            float acc[3] = {0, 0, 0}, ws = 0;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    int xx = x + dx, yy = y + dy;
                    if ((!dx && !dy) || xx < 0 || yy < 0 || xx >= rw || yy >= rh || unk[(size_t)yy * rw + xx]) continue;
                    float w = 1.f / (dx * dx + dy * dy);
                    const unsigned char* p = &img.px[((size_t)(ry0 + yy) * img.w + rx0 + xx) * 3];
                    for (int c = 0; c < 3; ++c) acc[c] += w * p[c];
                    ws += w;
                }
            if (ws > 0) { done.push_back(idx); for (int c = 0; c < 3; ++c) vals.push_back((unsigned char)std::lround(acc[c] / ws)); }
            else next.push_back(idx);
        }
        if (done.empty()) break;
        for (size_t i = 0; i < done.size(); ++i) {
            unk[done[i]] = 0;
            std::memcpy(&img.px[((size_t)(ry0 + done[i] / rw) * img.w + rx0 + done[i] % rw) * 3], &vals[i * 3], 3);
        }
        todo.swap(next);
    }
}

Value fn_video(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const std::string model = a[0].as_str(), in_video = a[1].as_str(), mask_path = a[2].as_str(), out_path = a[3].as_str();
    int r = 4, threads = (int)std::max(1u, std::thread::hardware_concurrency()), max_frames = 6000;
    double t_sec = 0;
    bool fast = false;
    std::string lib, ffmpeg = "ffmpeg", ffprobe, progress_path, cancel_path;
    if (a.size() > 4 && !a[4].is_null()) {
        auto& d = a[4].as_dict();
        if (auto it = d.find("dilate"); it != d.end()) r = clampi((int)it->second.as_int(), 0, 64);
        if (auto it = d.find("threads"); it != d.end()) threads = clampi((int)it->second.as_int(), 1, 256);
        if (auto it = d.find("max_frames"); it != d.end()) max_frames = (int)it->second.as_int();
        if (auto it = d.find("t"); it != d.end()) t_sec = std::max(0.0, it->second.as_float());
        if (auto it = d.find("mode"); it != d.end()) fast = it->second.as_str() == "fast";
        if (auto it = d.find("lib"); it != d.end()) lib = it->second.as_str();
        if (auto it = d.find("ffmpeg"); it != d.end() && !it->second.as_str().empty()) ffmpeg = it->second.as_str();
        if (auto it = d.find("ffprobe"); it != d.end()) ffprobe = it->second.as_str();
        if (auto it = d.find("progress"); it != d.end()) progress_path = it->second.as_str();
        if (auto it = d.find("cancel"); it != d.end()) cancel_path = it->second.as_str();
    }
    if (ffprobe.empty()) {
        auto slash = ffmpeg.rfind('/');
        ffprobe = slash == std::string::npos ? "ffprobe" : ffmpeg.substr(0, slash + 1) + "ffprobe";
    }
    auto fail = [&](const std::string& m) { error = "inpaint.video(): " + m; return Value::null(); };
    signal(SIGPIPE, SIG_IGN);   // a dead encoder is an error here, not a signal
    int last_pct = -1;
    auto report = [&](double pct) {
        int p = (int)pct;
        if (progress_path.empty() || p == last_pct) return;
        last_pct = p;
        std::ofstream(progress_path, std::ios::trunc) << p;
    };
    auto alive = [&](int) { return cancel_path.empty() || access(cancel_path.c_str(), F_OK) != 0; };

    // The mask (full size) and the clip's rate and length.
    Pnm mask;
    if (!read_pnm(mask_path, "P5", 1, mask, error)) return fail(error);
    const int W = mask.w, H = mask.h;
    for (auto& v : mask.px) v = v > 127 ? 255 : 0;
    Pts pts;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (mask.px[(size_t)y * W + x]) pts.push_back({x, y});
    if (pts.empty()) return fail("the mask is empty");

    std::string fps_s;
    double fps = 0, dur = 0;
    {
        Child c;
        if (!spawn({ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=avg_frame_rate,r_frame_rate:format=duration",
                    "-of", "default=nw=1", in_video}, true, false, c, error)) return fail(error);
        std::string o = slurp(c.rd);
        reap(c, false);
        std::string avg, rr;
        std::istringstream ss(o);
        for (std::string line; std::getline(ss, line);) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "avg_frame_rate") avg = v; else if (k == "r_frame_rate") rr = v; else if (k == "duration") dur = parse_num(v);
        }
        fps_s = parse_rate(avg) > 0 ? avg : rr;
        fps = parse_rate(fps_s);
        if (fps <= 0 || fps > 240) return fail("cannot read the frame rate of the video (is it a video?)");
    }
    const int est = std::max(1, (int)std::lround(dur * fps) + 2);
    const std::string vf = "fps=" + fps_s;

    // The marked frame at full size.
    int at = clampi((int)std::lround(t_sec * fps), 0, est - 1);
    Pnm ref;
    auto grab = [&](int idx) {
        Child c;
        std::string e;
        if (!spawn({ffmpeg, "-v", "error", "-i", in_video, "-an", "-vf", vf + ",select=eq(n\\," + std::to_string(idx) + ")", "-frames:v", "1",
                    "-f", "image2pipe", "-c:v", "ppm", "-"}, true, false, c, e)) return false;
        bool ok = read_ppm_fd(c.rd, ref);
        reap(c, true);
        return ok;
    };
    if (!grab(at)) return fail("cannot decode the video");
    if (ref.w != W || ref.h != H)
        return fail("the mask is " + std::to_string(W) + "x" + std::to_string(H) + " but the video is " + std::to_string(ref.w) + "x" + std::to_string(ref.h));

    // Tracking scale: the mark should still be >= ~10 px, the frames must fit in memory, and 480 px wide is plenty.
    int x0 = W, y0 = H, x1 = 0, y1 = 0;
    for (auto& p : pts) { x0 = std::min(x0, p.first); x1 = std::max(x1, p.first); y0 = std::min(y0, p.second); y1 = std::max(y1, p.second); }
    int s = std::max(1, std::min((W + 479) / 480, std::min(x1 - x0 + 1, y1 - y0 + 1) / 10));
    while (s < 64 && (double)est * (W / s) * (H / s) > 4e8) ++s;
    const int TW = std::max(1, W / s), TH = std::max(1, H / s);
    if (est > max_frames) return fail("the video has too many frames (" + std::to_string(est) + ", limit " + std::to_string(max_frames) + ")");

    // Pass 1: small grey frames.
    std::vector<std::vector<unsigned char>> small;
    {
        Child c;
        if (!spawn({ffmpeg, "-v", "error", "-i", in_video, "-an", "-vf", vf + ",scale=" + std::to_string(TW) + ":" + std::to_string(TH) + ":flags=area",
                    "-f", "rawvideo", "-pix_fmt", "gray", "-"}, true, false, c, error)) return fail(error);
        for (;;) {
            if (!alive(0)) { reap(c, true); return fail("cancelled"); }
            small.emplace_back((size_t)TW * TH);
            int rc = read_n(c.rd, small.back().data(), small.back().size());
            if (rc != 1) { small.pop_back(); if (rc < 0) { reap(c, true); return fail("the video ended in the middle of a frame"); } break; }
            if ((int)small.size() > max_frames) { reap(c, true); return fail("the video has too many frames (limit " + std::to_string(max_frames) + ")"); }
            report(std::min(10.0, 10.0 * small.size() / est));
        }
        if (reap(c, false) != 0 && small.empty()) return fail("ffmpeg could not decode the video");
    }
    const int N = (int)small.size();
    if (N == 0) return fail("the video has no frames");
    if (at >= N) { at = N - 1; if (!grab(at)) return fail("cannot decode the video"); }

    // Template from the marked frame, at the tracking scale and at full size (to refine the position).
    remove_background(small);
    std::vector<unsigned char> grey_ref;
    to_grey(ref, grey_ref);
    std::vector<int> cnt((size_t)TW * TH, 0);
    for (auto& p : pts) if (p.first / s < TW && p.second / s < TH) ++cnt[(size_t)(p.second / s) * TW + p.first / s];
    Pts lows, lows_any;
    for (int y = 0; y < TH; ++y)
        for (int x = 0; x < TW; ++x) {
            int c = cnt[(size_t)y * TW + x];
            if (c) lows_any.push_back({x, y});
            if (c * 2 >= s * s) lows.push_back({x, y});
        }
    if (lows.empty()) lows = lows_any;
    if (lows.empty()) return fail("the mask lies outside the picture");
    int lbx = 0, lby = 0, fbx = 0, fby = 0;
    Tmpl tl = make_tmpl(small[at].data(), TW, lows, 1500, lbx, lby);
    Tmpl tf = make_tmpl(grey_ref.data(), W, pts, 3000, fbx, fby);
    std::vector<Track> trk = track_all(small, TW, TH, at, tl, lbx, lby, alive);
    if (!alive(0)) return fail("cancelled");

    // Pass 2: clean every frame at full size.
    std::unique_lock<std::mutex> lock(g_mu, std::defer_lock);
    if (!fast) {
        lock.lock();
        if (!load_runtime(model, lib, threads, error)) return fail(error);
    }
    Child dec, enc;
    if (!spawn({ffmpeg, "-v", "error", "-i", in_video, "-an", "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"}, true, false, dec, error)) return fail(error);
    if (!spawn({ffmpeg, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", std::to_string(W) + "x" + std::to_string(H), "-framerate", fps_s,
                "-i", "-", "-i", in_video, "-map", "0:v", "-map", "1:a?", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-c:v", "libx264", "-preset", "veryfast",
                "-crf", "18", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", "-shortest", out_path}, false, true, enc, error)) {
        reap(dec, true);
        return fail(error);
    }
    Pnm frame, m;
    frame.w = W; frame.h = H; frame.ch = 3; frame.px.resize((size_t)W * H * 3);
    m.w = W; m.h = H; m.ch = 1; m.px.resize((size_t)W * H);
    std::vector<unsigned char> grey;
    int done = 0, held = 0, tiles = 0;
    std::string why;
    for (; done < N; ++done) {
        if (!alive(0)) { why = "cancelled"; break; }
        int rc = read_n(dec.rd, frame.px.data(), frame.px.size());
        if (rc != 1) break;   // the second decode ended early: keep what there is
        const Track& tr = trk[done];
        // Offset of the mark from the marked frame: the small-scale shift, refined at full size.
        int ox = (tr.x - lbx) * s, oy = (tr.y - lby) * s;
        if (!tr.held) ++held;   // (counted as followed)
        if (!tr.held && s > 1) {
            to_grey(frame, grey);
            Hit h = search(grey.data(), W, H, tf, fbx + ox - s - 1, fbx + ox + s + 1, fby + oy - s - 1, fby + oy + s + 1, 1);
            if (h.s >= 0.35) { ox = h.x - fbx; oy = h.y - fby; }
        }
        std::fill(m.px.begin(), m.px.end(), 0);
        for (auto& p : pts) {
            int x = p.first + ox, y = p.second + oy;
            if (x >= 0 && y >= 0 && x < W && y < H) m.px[(size_t)y * W + x] = 255;
        }
        int bx0, by0, bx1, by1;
        dilate(m, r, bx0, by0, bx1, by1);
        if (bx1 > bx0) {
            if (fast) fill_classic(frame, m, bx0, by0, bx1, by1);
            else if (!inpaint_mask(frame, m, bx0, by0, bx1, by1, tiles, why)) break;
        }
        if (!write_n(enc.wr, frame.px.data(), frame.px.size())) { why = "the encoder stopped (ffmpeg/libx264 missing?)"; break; }
        report(10.0 + 90.0 * (done + 1) / N);
    }
    reap(dec, !why.empty());
    int ec = reap(enc, !why.empty());
    if (!why.empty()) { if (why != "cancelled") { std::remove(out_path.c_str()); } return fail(why); }
    if (done == 0 || ec != 0) { std::remove(out_path.c_str()); return fail("ffmpeg could not write the cleaned video"); }
    report(100);
    Value::Dict d;
    d["width"] = Value::integer(W);
    d["height"] = Value::integer(H);
    d["frames"] = Value::integer(done);
    d["fps"] = Value::real(fps);
    d["followed"] = Value::integer(held);
    d["tiles"] = Value::integer(tiles);
    return Value::dict(std::move(d));
}

} // namespace

LUX_MODULE(inpaint, {
    {"run",   "ssss|D", fn_run,   /*is_async=*/true},
    {"video", "ssss|D", fn_video, /*is_async=*/true},
})

} // namespace lux_script
