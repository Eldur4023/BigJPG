// convert: file-format conversion for Lux Script -- the tables and command builders of VERT
// (github.com/VERT-sh/VERT, src/lib/converters/{magick,ffmpeg,pandoc}), minus the WebAssembly.
//
// VERT ships ImageMagick, FFmpeg and Pandoc compiled to WASM and drives them from the browser.
// Here the same three programs are the native ones on PATH, and this module is the part of VERT that
// is NOT the engine: which formats exist and in which direction, which tool handles a pair, the codec
// per container, the settings (metadata, bitrate, sample rate, channels, quality, ico size) and the
// per-format fix-ups (amv, mpeg, opus, gxf, mxf, divx, alac). It does not run anything: `command()`
// returns {bin, args} for the caller to start with `proc.start()`, so cancelling, timeouts and progress
// stay in the app, and no pool worker is held for a ten-minute video.
//
//   import convert
//   Json c = convert.command("in.wav", "out.mp3", { "bitrate": 192 })
//   if c["ok"]: int h = proc.start(c["bin"], c["args"], {})
//
//   convert.formats()               [{ext, kind, from, to}]  kind: image | audio | video | document
//   convert.kind("mp3")             "audio" ("" if unknown)
//   convert.targets("mp3")          every ext it can become, with the tools installed here
//   convert.can("mp3", "flac")      the same, for one pair
//   convert.tools()                 {ffmpeg, magick, pandoc}: the path of each, "" if missing
//   convert.command(in, out, opts)  {ok, error, tool, bin, args}; formats come from the two extensions
//
// opts (all optional): metadata (bool, keep tags/EXIF; default off), bitrate (kbps), sample_rate (Hz),
// channels (int, or "auto"; audio targets default to 2), quality (0-100, images), single_size (px, .ico).
#include <lux_script/builtin_module.hpp>

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

namespace lux_script {
namespace {

enum class Kind { Image, Audio, Video, Document };
enum class Tool { None, Magick, Ffmpeg, Pandoc };

struct Fmt { const char* ext; Kind kind; bool from; bool to; };

// VERT's lists. Read-only formats are camera raws, HEIC and the like (ImageMagick decodes them, never encodes).
const Fmt kFormats[] = {
    // ── images (magick.svelte.ts) ───────────────────────────────────────────
    {"png", Kind::Image, 1, 1}, {"jpeg", Kind::Image, 1, 1}, {"jpg", Kind::Image, 1, 1}, {"webp", Kind::Image, 1, 1},
    {"gif", Kind::Image, 1, 1}, {"jxl", Kind::Image, 1, 1}, {"avif", Kind::Image, 1, 1}, {"ico", Kind::Image, 1, 1},
    {"bmp", Kind::Image, 1, 1}, {"cur", Kind::Image, 1, 1}, {"hdr", Kind::Image, 1, 1}, {"jpe", Kind::Image, 1, 1},
    {"mat", Kind::Image, 1, 1}, {"pbm", Kind::Image, 1, 1}, {"pfm", Kind::Image, 1, 1}, {"pgm", Kind::Image, 1, 1},
    {"pnm", Kind::Image, 1, 1}, {"ppm", Kind::Image, 1, 1}, {"tiff", Kind::Image, 1, 1}, {"tif", Kind::Image, 1, 1},
    {"jfif", Kind::Image, 1, 1}, {"psd", Kind::Image, 1, 1}, {"eps", Kind::Image, 0, 1},
    // VERT lists svg as writable; ImageMagick can only trace to it, so here it is input only.
    {"svg", Kind::Image, 1, 0},
    {"heic", Kind::Image, 1, 0}, {"heif", Kind::Image, 1, 0}, {"mpo", Kind::Image, 1, 0}, {"ani", Kind::Image, 1, 0},
    {"icns", Kind::Image, 1, 0}, {"dcm", Kind::Image, 1, 0}, {"qoi", Kind::Image, 1, 0}, {"xcf", Kind::Image, 1, 0},
    {"nef", Kind::Image, 1, 0}, {"cr2", Kind::Image, 1, 0}, {"arw", Kind::Image, 1, 0}, {"dng", Kind::Image, 1, 0},
    {"rw2", Kind::Image, 1, 0}, {"raf", Kind::Image, 1, 0}, {"orf", Kind::Image, 1, 0}, {"pef", Kind::Image, 1, 0},
    {"mos", Kind::Image, 1, 0}, {"raw", Kind::Image, 1, 0}, {"dcr", Kind::Image, 1, 0}, {"crw", Kind::Image, 1, 0},
    {"cr3", Kind::Image, 1, 0}, {"3fr", Kind::Image, 1, 0}, {"erf", Kind::Image, 1, 0}, {"mrw", Kind::Image, 1, 0},
    {"mef", Kind::Image, 1, 0}, {"nrw", Kind::Image, 1, 0}, {"srw", Kind::Image, 1, 0}, {"sr2", Kind::Image, 1, 0},
    {"srf", Kind::Image, 1, 0},
    // ── audio (ffmpeg.svelte.ts). VERT's qoa is left out: it needs its own encoder, not ffmpeg. ──
    {"mp3", Kind::Audio, 1, 1}, {"wav", Kind::Audio, 1, 1}, {"flac", Kind::Audio, 1, 1}, {"ogg", Kind::Audio, 1, 1},
    {"oga", Kind::Audio, 1, 1}, {"opus", Kind::Audio, 1, 1}, {"aac", Kind::Audio, 1, 1}, {"alac", Kind::Audio, 1, 1},
    {"m4a", Kind::Audio, 1, 1}, {"wma", Kind::Audio, 1, 1}, {"ac3", Kind::Audio, 1, 1}, {"aiff", Kind::Audio, 1, 1},
    {"aifc", Kind::Audio, 1, 1}, {"aif", Kind::Audio, 1, 1}, {"mp2", Kind::Audio, 1, 1}, {"au", Kind::Audio, 1, 1},
    {"m4b", Kind::Audio, 1, 1}, {"voc", Kind::Audio, 1, 1},
    {"mogg", Kind::Audio, 1, 0}, {"caf", Kind::Audio, 1, 0}, {"mp1", Kind::Audio, 1, 0}, {"mpc", Kind::Audio, 1, 0},
    {"dsd", Kind::Audio, 1, 0}, {"dsf", Kind::Audio, 1, 0}, {"dff", Kind::Audio, 1, 0}, {"mqa", Kind::Audio, 1, 0},
    // ── video: the containers in ffmpeg.codecs.ts ───────────────────────────
    {"mp4", Kind::Video, 1, 1}, {"mkv", Kind::Video, 1, 1}, {"mov", Kind::Video, 1, 1}, {"mts", Kind::Video, 1, 1},
    {"ts", Kind::Video, 1, 1}, {"m2ts", Kind::Video, 1, 1}, {"flv", Kind::Video, 1, 1}, {"f4v", Kind::Video, 1, 1},
    {"m4v", Kind::Video, 1, 1}, {"3gp", Kind::Video, 1, 1}, {"3g2", Kind::Video, 1, 1}, {"nut", Kind::Video, 1, 1},
    {"wmv", Kind::Video, 1, 1}, {"webm", Kind::Video, 1, 1}, {"ogv", Kind::Video, 1, 1}, {"avi", Kind::Video, 1, 1},
    {"divx", Kind::Video, 1, 1}, {"mpg", Kind::Video, 1, 1}, {"mpeg", Kind::Video, 1, 1}, {"vob", Kind::Video, 1, 1},
    {"mxf", Kind::Video, 1, 1}, {"gxf", Kind::Video, 1, 1}, {"h264", Kind::Video, 1, 1}, {"swf", Kind::Video, 1, 1},
    {"amv", Kind::Video, 1, 1}, {"asf", Kind::Video, 1, 1}, {"apng", Kind::Video, 1, 1},
    {"ogx", Kind::Video, 1, 0},   // VERT: not an audio-to-video target
    // ── documents (pandoc.svelte.ts) ────────────────────────────────────────
    {"docx", Kind::Document, 1, 1}, {"md", Kind::Document, 1, 1}, {"html", Kind::Document, 1, 1},
    {"rtf", Kind::Document, 1, 0}, {"json", Kind::Document, 1, 1}, {"rst", Kind::Document, 1, 1},
    {"epub", Kind::Document, 1, 1}, {"odt", Kind::Document, 1, 1}, {"docbook", Kind::Document, 1, 1},
    // VERT writes csv/tsv; pandoc only reads them.
    {"csv", Kind::Document, 1, 0}, {"tsv", Kind::Document, 1, 0},
};

const Fmt* find(const std::string& ext) {
    for (const Fmt& f : kFormats) if (ext == f.ext) return &f;
    return nullptr;
}

const char* kind_name(Kind k) {
    switch (k) { case Kind::Image: return "image"; case Kind::Audio: return "audio";
                 case Kind::Video: return "video"; default: return "document"; }
}

std::string ext_of(const std::string& path) {
    const size_t slash = path.find_last_of('/'), dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// A looping image (gif, webp) is read by ffmpeg when its other end is a video or audio.
bool animated_image(const std::string& e) { return e == "gif" || e == "webp"; }

// ── tools ──────────────────────────────────────────────────────────────────

std::string which(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        const std::string dir = p.substr(start, end - start);
        const std::string full = (dir.empty() ? "." : dir) + "/" + name;
        if (access(full.c_str(), X_OK) == 0) return full;
        start = end + 1;
    }
    return "";
}

std::string magick_bin() { std::string m = which("magick"); return m.empty() ? which("convert") : m; }

// Which tool converts `from` to `to`, whatever is installed. Tool::None = not a conversion we do.
Tool route(const Fmt* f, const Fmt* t, const std::string& from, const std::string& to) {
    if (!f || !t || !f->from || !t->to || from == to) return Tool::None;
    if (f->kind == Kind::Document || t->kind == Kind::Document)
        return f->kind == t->kind ? Tool::Pandoc : Tool::None;
    if (f->kind == Kind::Image && t->kind == Kind::Image) return Tool::Magick;
    auto media = [](const Fmt* x, const std::string& e) { return x->kind != Kind::Image || animated_image(e); };
    // A sound has nothing to draw into a gif, webp or apng.
    if (f->kind == Kind::Audio && (animated_image(to) || to == "apng")) return Tool::None;
    return media(f, from) && media(t, to) ? Tool::Ffmpeg : Tool::None;
}

// The binary that does the job here: ImageMagick falls back to ffmpeg for plain image-to-image.
Tool available(Tool t, std::string& bin) {
    if (t == Tool::Magick) {
        bin = magick_bin();
        if (!bin.empty()) return Tool::Magick;
        t = Tool::Ffmpeg;
    }
    bin = which(t == Tool::Ffmpeg ? "ffmpeg" : "pandoc");
    return bin.empty() ? Tool::None : t;
}

// ── settings ───────────────────────────────────────────────────────────────

struct Settings {
    bool metadata = false;
    long long bitrate = 0, sample_rate = 0, quality = -1, single_size = 0;
    std::string channels = "";     // "" = unset
    bool channels_auto = false;
};

bool read_settings(const Value::Dict& d, Settings& s, std::string& error) {
    auto num = [&](const char* key, long long& out, long long lo, long long hi) {
        auto it = d.find(key);
        if (it == d.end() || it->second.is_null()) return true;
        if (!it->second.is_int() || it->second.as_int() < lo || it->second.as_int() > hi) {
            error = std::string("convert.command(): ") + key + " must be an integer between " + std::to_string(lo) + " and " + std::to_string(hi);
            return false;
        }
        out = it->second.as_int();
        return true;
    };
    if (auto it = d.find("metadata"); it != d.end()) s.metadata = it->second.truthy();
    if (!num("bitrate", s.bitrate, 1, 10000) || !num("sample_rate", s.sample_rate, 1000, 384000) ||
        !num("quality", s.quality, 0, 100) || !num("single_size", s.single_size, 1, 256)) return false;
    if (auto it = d.find("channels"); it != d.end() && !it->second.is_null()) {
        const std::string c = lower(it->second.to_string());
        if (c == "" || c == "auto") s.channels_auto = true;
        else if (c.find_first_not_of("0123456789") != std::string::npos || std::stoi(c) < 1 || std::stoi(c) > 8) {
            error = "convert.command(): channels must be 1-8 or \"auto\"";
            return false;
        }
        else s.channels = c;
    }
    return true;
}

// ffmpeg.svelte.ts normalizeSettings: limits some containers cannot go past.
void normalize(const std::string& to, Settings& s) {
    if (to == "opus" && s.bitrate > 256) s.bitrate = 256;       // VERT also forces opus to mono; not done here
    if (to == "amv") { s.sample_rate = 22050; s.channels = "1"; s.channels_auto = false; s.bitrate = 32; }
    if (to == "mpg" || to == "mpeg" || to == "vob") {
        if (s.bitrate > 0) { long long p = 1; while (p * 2 <= s.bitrate) p *= 2; if (s.bitrate - p > p * 2 - s.bitrate) p *= 2; s.bitrate = p; }
    }
    if (to == "gxf") { s.sample_rate = 48000; s.channels = "1"; s.channels_auto = false; }
}

// ffmpeg.codecs.ts getCodecs: {video, audio} per output extension. "none" = no stream of that kind.
struct Codecs { const char* v; const char* a; };

Codecs codecs_for(const std::string& e, bool alac) {
    auto in = [&](std::initializer_list<const char*> l) { for (const char* x : l) if (e == x) return true; return false; };
    if (in({"mp4", "mkv", "mov", "mts", "ts", "m2ts", "flv", "f4v", "m4v", "3gp", "3g2", "nut"})) return {"libx264", "aac"};
    if (e == "wmv" || e == "asf") return {"wmv2", "wmav2"};
    if (e == "webm") return {"libvpx", "libvorbis"};
    if (e == "ogv" || e == "ogx") return {"libtheora", "libvorbis"};
    if (e == "avi" || e == "divx") return {"mpeg4", "libmp3lame"};
    if (e == "mpg" || e == "mpeg" || e == "vob") return {"mpeg2video", "mp2"};
    if (e == "mxf" || e == "gxf") return {"mpeg2video", "pcm_s16le"};
    if (e == "h264") return {"libx264", "none"};
    if (e == "swf") return {"flv1", "mp3"};
    if (e == "amv") return {"amv", "adpcm_ima_amv"};
    if (e == "mp3" || e == "mp2") return {"none", e == "mp3" ? "libmp3lame" : "mp2"};
    if (e == "flac") return {"none", "flac"};
    if (e == "wav") return {"none", "pcm_s16le"};
    if (e == "ogg" || e == "oga") return {"none", "libvorbis"};
    if (e == "opus") return {"none", "libopus"};
    if (e == "aac" || e == "m4b") return {"none", "aac"};
    if (e == "m4a") return {"none", alac ? "alac" : "aac"};
    if (e == "alac") return {"none", "alac"};
    if (e == "wma") return {"none", "wmav2"};
    if (e == "aiff" || e == "aifc" || e == "aif") return {"none", "pcm_s16be"};
    if (e == "au") return {"none", "pcm_mulaw"};
    if (e == "voc") return {"none", "pcm_u8"};
    if (e == "ac3") return {"none", "ac3"};
    if (e == "gif") return {"gif", "none"};
    if (e == "webp") return {"libwebp", "none"};
    if (e == "apng") return {"apng", "none"};
    return {"copy", "copy"};
}

using Args = std::vector<std::string>;
void add(Args& a, std::initializer_list<const char*> l) { for (const char* x : l) a.push_back(x); }

Args ffmpeg_args(const std::string& in, const std::string& out, const std::string&, const std::string& to,
                 const Fmt* f, const Fmt* t, Settings s) {
    normalize(to, s);
    Args a;
    const bool audio_in = f->kind == Kind::Audio;
    const bool audio_out = t->kind == Kind::Audio;
    const bool anim_out = to == "gif" || to == "webp" || to == "apng";
    add(a, {"-y", "-v", "error"});
    // Audio into a video container: a black frame to hang the sound on (VERT's avWithBg).
    if (audio_in && !audio_out) {
        add(a, {"-f", "lavfi", "-i", "color=c=black:s=640x360:r=2"});
        a.push_back("-i"); a.push_back(in);
        add(a, {"-shortest", "-map", "0:v:0"});
        if (!anim_out) add(a, {"-map", "1:a:0"});
    } else {
        a.push_back("-i"); a.push_back(in);
    }
    if (!s.metadata) add(a, {"-map_metadata", "-1", "-map_chapters", "-1"});
    const Codecs c = codecs_for(to, false);
    const bool keep_art = audio_out && s.metadata && (to == "m4a" || to == "m4b");
    if (audio_out) {
        if (!audio_in) add(a, {"-map", "0:a:0"});                       // video -> audio: its sound track
        else if (keep_art) add(a, {"-c:v", "copy"});                    // m4a keeps its cover
        else add(a, {"-vn"});
        a.push_back("-c:a"); a.push_back(c.a);
    } else {
        a.push_back("-c:v"); a.push_back(c.v);
        const std::string v = c.v;
        if (v == "libx264") {
            if (audio_in) add(a, {"-preset", "ultrafast", "-crf", "18", "-tune", "stillimage"});   // VERT's toArgs
            else add(a, {"-preset", "veryfast", "-crf", "20"});
            add(a, {"-pix_fmt", "yuv420p"});
        }
        if (std::string(c.a) == "none" || anim_out) add(a, {"-an"});
        else { a.push_back("-c:a"); a.push_back(c.a); }
        if (to == "mp4" || to == "mov" || to == "m4v" || to == "3gp") add(a, {"-movflags", "+faststart"});
        if (to == "amv") { add(a, {"-r", "3"}); a.push_back("-block_size"); a.push_back(std::to_string(22050 / 3)); }
        if (to == "mxf") add(a, {"-ar", "48000"});
    }
    const bool has_audio = audio_out || (std::string(c.a) != "none" && !anim_out);
    if (has_audio) {
        if (s.bitrate > 0) { a.push_back("-b:a"); a.push_back(std::to_string(s.bitrate) + "k"); }
        if (s.sample_rate > 0) { a.push_back("-ar"); a.push_back(std::to_string(s.sample_rate)); }
        std::string ch = s.channels;
        if (ch.empty() && !s.channels_auto && audio_out) ch = "2";      // VERT's default
        if (!ch.empty()) { a.push_back("-ac"); a.push_back(ch); }
        add(a, {"-strict", "experimental"});
    }
    if (to == "mxf") add(a, {"-strict", "unofficial"});
    if (to == "divx") add(a, {"-f", "avi"});          // output formats the extension does not name
    if (to == "alac") add(a, {"-f", "ipod"});
    a.push_back(out);
    return a;
}

bool no_alpha(const std::string& e) {
    return e == "jpg" || e == "jpeg" || e == "jpe" || e == "jfif" || e == "bmp" || e == "pbm" || e == "pgm" ||
           e == "ppm" || e == "pnm" || e == "hdr" || e == "eps";
}

Args magick_args(const std::string& in, const std::string& out, const std::string& from, const std::string& to,
                 const Settings& s) {
    Args a;
    if (from == "svg") add(a, {"-background", "none", "-density", "192"});
    // First frame only, unless an animation goes to another animated format.
    const bool anim = animated_image(from) && animated_image(to);
    a.push_back(anim ? in : in + "[0]");
    if (!s.metadata) a.push_back("-strip");
    if (no_alpha(to)) add(a, {"-background", "white", "-alpha", "remove", "-alpha", "off"});
    if (s.quality >= 0) { a.push_back("-quality"); a.push_back(std::to_string(s.quality)); }
    if (to == "ico" || to == "cur") {
        if (s.single_size > 0) { a.push_back("-resize"); a.push_back(std::to_string(s.single_size) + "x" + std::to_string(s.single_size)); }
        else { a.push_back("-define"); a.push_back("icon:auto-resize=256,128,64,48,32,16"); }
    }
    a.push_back(out);
    return a;
}

// ffmpeg for plain images, when ImageMagick is not installed.
Args ffmpeg_image_args(const std::string& in, const std::string& out, const std::string& to, const Settings& s) {
    Args a;
    add(a, {"-y", "-v", "error"});
    a.push_back("-i"); a.push_back(in);
    if (!s.metadata) add(a, {"-map_metadata", "-1"});
    if (s.quality >= 0 && (to == "jpg" || to == "jpeg" || to == "jpe" || to == "jfif")) {
        a.push_back("-q:v"); a.push_back(std::to_string(2 + (100 - s.quality) * 29 / 100));
    }
    if (!animated_image(to)) add(a, {"-frames:v", "1"});
    a.push_back(out);
    return a;
}

// pandoc.svelte.ts: format names for -f / -t.
std::string pandoc_name(const std::string& e, bool writer) {
    if (e == "md") return writer ? "markdown" : "markdown";
    if (e == "html") return writer ? "html5" : "html";
    return e;
}

Args pandoc_args(const std::string& in, const std::string& out, const std::string& from, const std::string& to) {
    Args a{"-f", pandoc_name(from, false), "-t", pandoc_name(to, true)};
    if (to == "html") a.push_back("--standalone");
    a.push_back("-o"); a.push_back(out);
    a.push_back(in);
    return a;
}

// ── module functions ───────────────────────────────────────────────────────

Value make_dict(std::initializer_list<std::pair<const char*, Value>> kv) {
    Value::Dict d;
    for (const auto& p : kv) d[p.first] = p.second;
    return Value::dict(std::move(d));
}

Value fn_formats(NativeCtx&, std::vector<Value>&, std::string&) {
    Value::List out;
    for (const Fmt& f : kFormats)
        out.push_back(make_dict({{"ext", Value::str(f.ext)}, {"kind", Value::str(kind_name(f.kind))},
                                   {"from", Value::boolean(f.from)}, {"to", Value::boolean(f.to)}}));
    return Value::list(std::move(out));
}

Value fn_kind(NativeCtx&, std::vector<Value>& a, std::string&) {
    const Fmt* f = find(lower(a[0].as_str()));
    return Value::str(f ? kind_name(f->kind) : "");
}

bool can(const std::string& from, const std::string& to) {
    std::string bin;
    return available(route(find(from), find(to), from, to), bin) != Tool::None;
}

Value fn_can(NativeCtx&, std::vector<Value>& a, std::string&) {
    return Value::boolean(can(lower(a[0].as_str()), lower(a[1].as_str())));
}

Value fn_targets(NativeCtx&, std::vector<Value>& a, std::string&) {
    const std::string from = lower(a[0].as_str());
    Value::List out;
    for (const Fmt& t : kFormats) if (can(from, t.ext)) out.push_back(Value::str(t.ext));
    return Value::list(std::move(out));
}

Value fn_tools(NativeCtx&, std::vector<Value>&, std::string&) {
    return make_dict({{"ffmpeg", Value::str(which("ffmpeg"))}, {"magick", Value::str(magick_bin())},
                        {"pandoc", Value::str(which("pandoc"))}});
}

Value fail(const std::string& why) {
    return make_dict({{"ok", Value::boolean(false)}, {"error", Value::str(why)}, {"tool", Value::str("")},
                        {"bin", Value::str("")}, {"args", Value::list()}});
}

Value fn_command(NativeCtx&, std::vector<Value>& a, std::string& error) {
    const std::string in = a[0].as_str(), out = a[1].as_str();
    const std::string from = ext_of(in), to = ext_of(out);
    const Fmt *f = find(from), *t = find(to);
    if (!f) return fail("unknown input format ." + from);
    if (!t) return fail("unknown output format ." + to);
    const Tool wanted = route(f, t, from, to);
    if (wanted == Tool::None) return fail("cannot convert ." + from + " to ." + to);
    Settings s;
    if (a.size() > 2 && !read_settings(a[2].as_dict(), s, error)) return Value::null();
    std::string bin;
    const Tool tool = available(wanted, bin);
    if (tool == Tool::None)
        return fail(std::string(wanted == Tool::Pandoc ? "pandoc" : "ffmpeg") + " is not installed");
    Args args;
    const char* name;
    if (tool == Tool::Pandoc)      { args = pandoc_args(in, out, from, to); name = "pandoc"; }
    else if (tool == Tool::Magick) { args = magick_args(in, out, from, to, s); name = "magick"; }
    else if (f->kind == Kind::Image && t->kind == Kind::Image) { args = ffmpeg_image_args(in, out, to, s); name = "ffmpeg"; }
    else                           { args = ffmpeg_args(in, out, from, to, f, t, s); name = "ffmpeg"; }
    Value::List list;
    for (auto& x : args) list.push_back(Value::str(std::move(x)));
    return make_dict({{"ok", Value::boolean(true)}, {"error", Value::str("")}, {"tool", Value::str(name)},
                        {"bin", Value::str(bin)}, {"args", Value::list(std::move(list))}});
}

} // namespace

LUX_MODULE(convert, {
    {"formats", ">l",     fn_formats},
    {"kind",    "s>s",    fn_kind},
    {"targets", "s>l",    fn_targets},
    {"can",     "ss>b",   fn_can},
    {"tools",   ">d",     fn_tools},
    {"command", "ss|d>d", fn_command},
})

} // namespace lux_script
