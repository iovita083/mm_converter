// mm_converter.cpp
// Converts Project DIVA Mega Mix+ song mods to MicroMix+ format.
// Deps: zlib (FARC decompression). FFmpeg and UsmToolkit must be in PATH.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <iomanip>
#include <cstdarg>
#include <zlib.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <thread>
#include <mutex>
#include <atomic>

#pragma comment(lib, "comctl32.lib")

namespace fs = std::filesystem;

// ---- Thread-Safe UI Logging -------------------------------------------------

HWND g_hLog = NULL;
std::mutex g_LogMutex;

void GuiLog(const char* fmt, ...) {
    if (!g_hLog) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::string out(buf);
    size_t pos = 0;
    while ((pos = out.find("\n", pos)) != std::string::npos) {
        if (pos == 0 || out[pos - 1] != '\r') { out.replace(pos, 1, "\r\n"); pos += 2; }
        else pos++;
    }

    std::lock_guard<std::mutex> lock(g_LogMutex);
    int len = GetWindowTextLengthA(g_hLog);
    SendMessageA(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(g_hLog, EM_REPLACESEL, 0, (LPARAM)out.c_str());
}

// Redirect standard console outputs to our UI textbox
#define printf GuiLog
#define fprintf(stream, fmt, ...) GuiLog(fmt, ##__VA_ARGS__)

// ---- Helpers ----------------------------------------------------------------

static bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(),
        [](char a, char b) { return tolower(a) == tolower(b); });
}

static std::string to_lower(std::string s) {
    for (auto& c : s) c = (char)tolower(c);
    return s;
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, e - b + 1);
}

static std::string sanitize_token(const std::string& s) {
    size_t sp = s.find_first_of(" \t");
    return sp == std::string::npos ? s : s.substr(0, sp);
}

static std::string fmt_time(float v) {
    int trunc = (int)(v * 100.0f);
    std::ostringstream os;
    os << (trunc / 100) << "." << std::setw(2) << std::setfill('0') << (trunc % 100);
    return os.str();
}

// Silent process execution that pipes standard output back to the GUI log
static void run(const std::string& cmd) {
    GuiLog("  $ %s\r\n", cmd.c_str());

    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE; // Hide console window
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi;
    std::string cmdMutable = cmd;
    if (CreateProcessA(NULL, &cmdMutable[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hWritePipe);
        char buffer[256];
        DWORD bytesRead;
        while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            GuiLog("%s", buffer);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    else {
        CloseHandle(hWritePipe);
        GuiLog("ERROR: Failed to execute command.\r\n");
    }
    CloseHandle(hReadPipe);
}

static uint32_t read_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t read_u32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static float read_f32le(const uint8_t* p) {
    uint32_t v = read_u32le(p);
    float f; memcpy(&f, &v, 4); return f;
}

// ---- FARC Archive -----------------------------------------------------------

struct FarcEntry {
    std::string name;
    uint32_t offset;
    uint32_t size;
    uint32_t uncompressed;
    bool compressed;
};

struct FarcArchive {
    std::vector<uint8_t> file_data;
    std::vector<FarcEntry> entries;

    bool load(const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        size_t sz = f.tellg(); f.seekg(0);
        file_data.resize(sz);
        f.read((char*)file_data.data(), sz);
        if (sz < 8) return false;

        char sig[5] = {};
        memcpy(sig, file_data.data(), 4);
        bool is_farc_uncompressed = (strcmp(sig, "FArc") == 0);
        bool is_farc_compressed = (strcmp(sig, "FArC") == 0);
        bool is_farc_dt = (strcmp(sig, "FARC") == 0);

        if (!is_farc_uncompressed && !is_farc_compressed && !is_farc_dt) {
            fprintf(stderr, "Not a FARC: %s (sig=%s)\n", path.c_str(), sig);
            return false;
        }

        const uint8_t* p = file_data.data();
        uint32_t header_size = read_u32be(p + 4) + 8;

        if (is_farc_dt) {
            uint32_t flags = read_u32be(p + 8);
            bool is_compressed = (flags & 2) != 0;
            bool is_encrypted = (flags & 4) != 0;
            if (is_encrypted) {
                fprintf(stderr, "Encrypted FARC not supported: %s\n", path.c_str());
                return false;
            }
            uint32_t entry_padding = read_u32be(p + 20);
            uint32_t header_padding = read_u32be(p + 24);
            size_t pos = 28 + header_padding;
            while (pos < header_size && pos < sz) {
                FarcEntry e;
                e.compressed = is_compressed;
                while (pos < sz && p[pos]) e.name += (char)p[pos++];
                pos++;
                if (pos + 12 > sz) break;
                e.offset = read_u32be(p + pos);     pos += 4;
                e.size = read_u32be(p + pos);     pos += 4;
                e.uncompressed = read_u32be(p + pos);     pos += 4;
                if (entry_padding > 0) pos += entry_padding;
                if (!is_compressed) e.size = e.uncompressed;
                entries.push_back(e);
            }
        }
        else {
            size_t pos = 12;
            while (pos < header_size && pos < sz) {
                FarcEntry e;
                e.compressed = is_farc_compressed;
                while (pos < sz && p[pos]) e.name += (char)p[pos++];
                pos++;
                if (pos + 8 > sz) break;
                e.offset = read_u32be(p + pos); pos += 4;
                e.size = read_u32be(p + pos); pos += 4;
                if (is_farc_compressed) {
                    e.uncompressed = read_u32be(p + pos); pos += 4;
                    if (e.uncompressed == 0) { e.compressed = false; e.uncompressed = e.size; }
                }
                else {
                    e.uncompressed = e.size;
                }
                entries.push_back(e);
            }
        }
        return true;
    }

    std::vector<uint8_t> extract(const std::string& name) const {
        for (auto& e : entries) {
            if (to_lower(e.name) == to_lower(name)) {
                const uint8_t* src = file_data.data() + e.offset;
                if (!e.compressed) {
                    return std::vector<uint8_t>(src, src + e.size);
                }
                std::vector<uint8_t> out(e.uncompressed);
                z_stream zs{};
                zs.next_in = const_cast<Bytef*>(src);
                zs.avail_in = e.size;
                zs.next_out = out.data();
                zs.avail_out = e.uncompressed;
                if (inflateInit2(&zs, 47) != Z_OK || inflate(&zs, Z_FINISH) < Z_OK) {
                    inflateEnd(&zs);
                    fprintf(stderr, "Failed to decompress %s\n", name.c_str());
                    return {};
                }
                inflateEnd(&zs);
                out.resize(zs.total_out);
                return out;
            }
        }
        return {};
    }

    const FarcEntry* find(const std::string& name) const {
        for (auto& e : entries)
            if (to_lower(e.name) == to_lower(name)) return &e;
        return nullptr;
    }
};

// ---- Sprite Set Binary (.bin) -----------------------------------------------
struct SpriteInfo {
    std::string name;
    uint32_t texture_index;
    float x, y, w, h;
};

struct TextureInfo {
    std::string name;
    int width, height;
    int format;
    std::vector<uint8_t> data;
    int chroma_width = 0, chroma_height = 0;
    std::vector<uint8_t> chroma_data;
    bool is_ycbcr = false;
};

struct SpriteSetBin {
    std::vector<SpriteInfo> sprites;
    std::vector<TextureInfo> textures;
};

static bool parse_subtexture(const uint8_t* txp4, size_t avail, uint32_t sub_idx,
    int& w, int& h, int& fmt, std::vector<uint8_t>& data)
{
    if (avail < 12u + (sub_idx + 1) * 4u) return false;
    uint32_t sub_off = read_u32le(txp4 + 12 + sub_idx * 4);
    if (sub_off + 24 > avail) return false;
    const uint8_t* sp = txp4 + sub_off;
    if (read_u32le(sp) != 0x02505854) return false;
    w = (int)read_u32le(sp + 4);
    h = (int)read_u32le(sp + 8);
    fmt = (int)read_u32le(sp + 12);
    uint32_t data_size = read_u32le(sp + 20);
    if (sub_off + 24 + data_size > avail) return false;
    data.assign(sp + 24, sp + 24 + data_size);
    return true;
}

static bool parse_texture(const uint8_t* txp4, size_t avail, TextureInfo& out) {
    if (avail < 16) return false;
    uint32_t sig = read_u32le(txp4);
    if (sig != 0x04505854 && sig != 0x05505854) return false;
    int sub_count = (int)read_u32le(txp4 + 4);
    uint32_t info = read_u32le(txp4 + 8);
    int mip_count = info & 0xFF;
    int array_size = (info >> 8) & 0xFF;
    if (array_size == 1 && mip_count != sub_count) mip_count = sub_count;

    int fmt0 = 0;
    if (!parse_subtexture(txp4, avail, 0, out.width, out.height, fmt0, out.data))
        return false;
    out.format = fmt0;

    if (fmt0 == 11 && array_size == 1 && mip_count == 2 && sub_count >= 2) {
        int cfmt = 0;
        if (parse_subtexture(txp4, avail, 1, out.chroma_width, out.chroma_height, cfmt, out.chroma_data))
            out.is_ycbcr = true;
    }
    return true;
}

static bool parse_bin(const std::vector<uint8_t>& bin, SpriteSetBin& out) {
    const uint8_t* p = bin.data();
    size_t sz = bin.size();
    if (sz < 32) return false;

    uint32_t tex_off = read_u32le(p + 4);
    int      tex_count = (int)read_u32le(p + 8);
    int      spr_count = (int)read_u32le(p + 12);
    uint32_t spr_off = read_u32le(p + 16);
    uint32_t tex_names_off = read_u32le(p + 20);
    uint32_t spr_names_off = read_u32le(p + 24);

    auto safe = [&](uint32_t off, size_t need) -> const uint8_t* {
        if ((size_t)off + need > sz) return nullptr;
        return p + off;
        };

    out.sprites.resize(spr_count);
    const uint8_t* sp = safe(spr_off, (size_t)spr_count * 40);
    if (!sp) return false;
    for (int i = 0; i < spr_count; i++) {
        const uint8_t* s = sp + i * 40;
        out.sprites[i].texture_index = read_u32le(s);
        out.sprites[i].x = read_f32le(s + 24);
        out.sprites[i].y = read_f32le(s + 28);
        out.sprites[i].w = read_f32le(s + 32);
        out.sprites[i].h = read_f32le(s + 36);
    }

    const uint8_t* snp = safe(spr_names_off, (size_t)spr_count * 4);
    if (snp) {
        for (int i = 0; i < spr_count; i++) {
            uint32_t str_off = read_u32le(snp + i * 4);
            const uint8_t* s = safe(str_off, 1);
            if (s) out.sprites[i].name = (const char*)s;
        }
    }

    out.textures.resize(tex_count);
    const uint8_t* txp3 = safe(tex_off, 12);
    if (!txp3 || read_u32le(txp3) != 0x03505854) return false;
    uint32_t txp3_count = read_u32le(txp3 + 4);
    if ((int)txp3_count < tex_count) tex_count = (int)txp3_count;
    const uint8_t* txp3_table = safe(tex_off + 12, (size_t)tex_count * 4);
    if (!txp3_table) return false;
    for (int i = 0; i < tex_count; i++) {
        uint32_t txp4_rel = read_u32le(txp3_table + i * 4);
        uint32_t txp4_abs = tex_off + txp4_rel;
        if (txp4_abs >= sz) continue;
        parse_texture(p + txp4_abs, sz - txp4_abs, out.textures[i]);
    }

    const uint8_t* tnp = safe(tex_names_off, (size_t)tex_count * 4);
    if (tnp) {
        for (int i = 0; i < tex_count; i++) {
            uint32_t str_off = read_u32le(tnp + i * 4);
            const uint8_t* s = safe(str_off, 1);
            if (s) out.textures[i].name = (const char*)s;
        }
    }

    return !out.sprites.empty();
}

// ---- DXT / Block Compression Decoding ---------------------------------------

static void decode_bc1_block(const uint8_t* src, uint8_t* dst, int dst_stride) {
    uint16_t c0 = (uint16_t)(src[0] | (src[1] << 8));
    uint16_t c1 = (uint16_t)(src[2] | (src[3] << 8));
    uint8_t r[4][3];
    auto unpack = [](uint16_t c, uint8_t* rgb) {
        rgb[0] = (uint8_t)((c >> 11 & 0x1F) * 255 / 31);
        rgb[1] = (uint8_t)((c >> 5 & 0x3F) * 255 / 63);
        rgb[2] = (uint8_t)((c & 0x1F) * 255 / 31);
        };
    unpack(c0, r[0]); unpack(c1, r[1]);
    if (c0 > c1) {
        for (int i = 0; i < 3; i++) r[2][i] = (2 * r[0][i] + r[1][i] + 1) / 3;
        for (int i = 0; i < 3; i++) r[3][i] = (r[0][i] + 2 * r[1][i] + 1) / 3;
    }
    else {
        for (int i = 0; i < 3; i++) r[2][i] = (r[0][i] + r[1][i]) / 2;
        r[3][0] = r[3][1] = r[3][2] = 0;
    }
    uint32_t idx = (uint32_t)(src[4]) | (src[5] << 8) | (src[6] << 16) | (src[7] << 24);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        int ci = (idx >> (2 * (y * 4 + x))) & 3;
        uint8_t* out = dst + y * dst_stride + x * 4;
        out[0] = r[ci][0]; out[1] = r[ci][1]; out[2] = r[ci][2];
        out[3] = (c0 <= c1 && ci == 3) ? 0 : 255;
    }
}

static void decode_bc3_alpha_block(const uint8_t* src, uint8_t* alpha_out) {
    uint8_t a0 = src[0], a1 = src[1];
    uint8_t av[8]; av[0] = a0; av[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; i++) av[i] = (uint8_t)(((8 - i) * a0 + (i - 1) * a1) / 7);
    }
    else {
        for (int i = 2; i < 6; i++) av[i] = (uint8_t)(((6 - i) * a0 + (i - 1) * a1) / 5);
        av[6] = 0; av[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= ((uint64_t)src[2 + i]) << (i * 8);
    for (int i = 0; i < 16; i++) alpha_out[i] = av[(bits >> (i * 3)) & 7];
}

static void decode_ati1_block(const uint8_t* src, uint8_t* ch_out) {
    decode_bc3_alpha_block(src, ch_out);
}

static void decode_bc5_channel(const uint8_t* src, int w, int h, std::vector<float>& ch0, std::vector<float>& ch1) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    ch0.resize(w * h); ch1.resize(w * h);
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        const uint8_t* b = src + (by * bw + bx) * 16;
        uint8_t a0[16], a1[16];
        decode_bc3_alpha_block(b, a0);
        decode_bc3_alpha_block(b + 8, a1);
        for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++) {
            int ox = bx * 4 + px, oy = by * 4 + py;
            if (ox >= w || oy >= h) continue;
            ch0[oy * w + ox] = a0[py * 4 + px] / 255.0f;
            ch1[oy * w + ox] = a1[py * 4 + px] / 255.0f;
        }
    }
}

static float bilinear(const std::vector<float>& buf, int bw, int bh, float u, float v) {
    float fx = u * bw - 0.5f, fy = v * bh - 0.5f;
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1, y1 = y0 + 1;
    float tx = fx - x0, ty = fy - y0;
    auto get = [&](int x, int y) { return buf[std::max(0, std::min(y, bh - 1)) * bw + std::max(0, std::min(x, bw - 1))]; };
    return (get(x0, y0) * (1 - tx) + get(x1, y0) * tx) * (1 - ty) + (get(x0, y1) * (1 - tx) + get(x1, y1) * tx) * ty;
}

static std::vector<uint8_t> decode_texture(const TextureInfo& tex) {
    int w = tex.width, h = tex.height;
    std::vector<uint8_t> rgba(w * h * 4, 0xFF);
    int fmt = tex.format;

    if (tex.is_ycbcr && !tex.chroma_data.empty()) {
        std::vector<float> lY, lA, cCb, cCr;
        decode_bc5_channel(tex.data.data(), w, h, lY, lA);
        decode_bc5_channel(tex.chroma_data.data(), tex.chroma_width, tex.chroma_height, cCb, cCr);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            float Y = lY[y * w + x];
            float A = lA[y * w + x];
            float Cb = bilinear(cCb, tex.chroma_width, tex.chroma_height, u, v) * 1.003922f - 0.503929f;
            float Cr = bilinear(cCr, tex.chroma_width, tex.chroma_height, u, v) * 1.003922f - 0.503929f;
            auto clamp01 = [](float f) { return std::max(0.0f, std::min(1.0f, f)); };
            uint8_t R = (uint8_t)(clamp01(Y + 1.5748f * Cr) * 255.0f);
            uint8_t G = (uint8_t)(clamp01(Y - 0.1873f * Cb - 0.4681f * Cr) * 255.0f);
            uint8_t B = (uint8_t)(clamp01(Y + 1.8556f * Cb) * 255.0f);
            uint8_t Ao = (uint8_t)(clamp01(A) * 255.0f);
            int dst_y = h - 1 - y;
            int idx = (dst_y * w + x) * 4;
            rgba[idx + 0] = R; rgba[idx + 1] = G; rgba[idx + 2] = B; rgba[idx + 3] = Ao;
        }
        return rgba;
    }
    if (fmt == 2) {
        for (int y = 0; y < h; y++) {
            int src_y = h - 1 - y;
            for (int x = 0; x < w; x++) {
                size_t si = (src_y * w + x) * 4;
                size_t di = (y * w + x) * 4;
                if (si + 3 >= tex.data.size()) continue;
                rgba[di + 0] = tex.data[si + 0];
                rgba[di + 1] = tex.data[si + 1];
                rgba[di + 2] = tex.data[si + 2];
                rgba[di + 3] = tex.data[si + 3];
            }
        }
        return rgba;
    }
    if (fmt == 1) {
        for (int y = 0; y < h; y++) {
            int src_y = h - 1 - y;
            for (int x = 0; x < w; x++) {
                size_t si = (src_y * w + x) * 3;
                size_t di = (y * w + x) * 4;
                if (si + 2 >= tex.data.size()) continue;
                rgba[di + 0] = tex.data[si + 0];
                rgba[di + 1] = tex.data[si + 1];
                rgba[di + 2] = tex.data[si + 2];
                rgba[di + 3] = 255;
            }
        }
        return rgba;
    }

    bool is_dxt1 = (fmt == 6 || fmt == 7);
    bool is_dxt3 = (fmt == 8);
    bool is_dxt5 = (fmt == 9);
    bool is_ati1 = (fmt == 10);
    bool is_ati2 = (fmt == 11);

    if (!is_dxt1 && !is_dxt3 && !is_dxt5 && !is_ati1 && !is_ati2) {
        fprintf(stderr, "Unsupported texture format %d\n", fmt);
        return rgba;
    }

    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    int block_size = (is_dxt1 || is_ati1) ? 8 : 16;
    const uint8_t* src = tex.data.data();

    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        int bi = by * bw + bx;
        const uint8_t* b = src + bi * block_size;
        uint8_t block_rgba[4 * 4 * 4] = {};
        if (is_dxt1) {
            decode_bc1_block(b, block_rgba, 16);
        }
        else if (is_dxt3) {
            decode_bc1_block(b + 8, block_rgba, 16);
            for (int i = 0; i < 16; i++) {
                int nibble = (i % 2 == 0) ? (b[i / 2] & 0xF) : (b[i / 2] >> 4);
                block_rgba[i * 4 + 3] = (uint8_t)(nibble * 17);
            }
        }
        else if (is_dxt5) {
            uint8_t alphas[16];
            decode_bc3_alpha_block(b, alphas);
            decode_bc1_block(b + 8, block_rgba, 16);
            for (int i = 0; i < 16; i++) block_rgba[i * 4 + 3] = alphas[i];
        }
        else if (is_ati1) {
            uint8_t ch[16];
            decode_ati1_block(b, ch);
            for (int i = 0; i < 16; i++) { block_rgba[i * 4 + 0] = ch[i]; block_rgba[i * 4 + 3] = 255; }
        }
        else if (is_ati2) {
            uint8_t ch0[16], ch1[16];
            decode_ati1_block(b, ch0);
            decode_ati1_block(b + 8, ch1);
            for (int i = 0; i < 16; i++) {
                block_rgba[i * 4 + 0] = ch0[i];
                block_rgba[i * 4 + 1] = ch1[i];
                block_rgba[i * 4 + 2] = 0;
                block_rgba[i * 4 + 3] = 255;
            }
        }
        for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++) {
            int ox = bx * 4 + px, oy = by * 4 + py;
            if (ox >= w || oy >= h) continue;
            int flipped_y = h - 1 - oy;
            int dst_idx = (flipped_y * w + ox) * 4;
            int src_idx = (py * 4 + px) * 4;
            rgba[dst_idx + 0] = block_rgba[src_idx + 0];
            rgba[dst_idx + 1] = block_rgba[src_idx + 1];
            rgba[dst_idx + 2] = block_rgba[src_idx + 2];
            rgba[dst_idx + 3] = block_rgba[src_idx + 3];
        }
    }
    return rgba;
}

// ---- Minimal PNG Writer -----------------------------------------------------

static uint32_t crc32_png(uint32_t crc, const uint8_t* buf, size_t len) {
    static uint32_t tbl[256] = {};
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            tbl[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = tbl[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void write_u32be_v(std::vector<uint8_t>& v, uint32_t n) {
    v.push_back(n >> 24); v.push_back(n >> 16); v.push_back(n >> 8); v.push_back(n);
}

static void write_chunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data, uint32_t len) {
    write_u32be_v(out, len);
    size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data, data + len);
    uint32_t crc = crc32_png(0, out.data() + crc_start, 4 + len);
    write_u32be_v(out, crc);
}

static bool save_png(const std::string& path, const uint8_t* rgba, int w, int h) {
    std::vector<uint8_t> raw;
    raw.reserve((w * 4 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + y * w * 4, rgba + y * w * 4 + w * 4);
    }
    uLongf clen = compressBound((uLong)raw.size());
    std::vector<uint8_t> compressed(clen);
    if (compress2(compressed.data(), &clen, raw.data(), (uLong)raw.size(), 9) != Z_OK)
        return false;
    compressed.resize(clen);

    std::vector<uint8_t> out;
    const uint8_t sig[] = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A };
    out.insert(out.end(), sig, sig + 8);

    uint8_t ihdr[13];
    ihdr[0] = w >> 24; ihdr[1] = w >> 16; ihdr[2] = w >> 8; ihdr[3] = w;
    ihdr[4] = h >> 24; ihdr[5] = h >> 16; ihdr[6] = h >> 8; ihdr[7] = h;
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    write_chunk(out, "IHDR", ihdr, 13);
    write_chunk(out, "IDAT", compressed.data(), (uint32_t)compressed.size());
    write_chunk(out, "IEND", nullptr, 0);

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((char*)out.data(), out.size());
    return true;
}

static bool save_sprite_png(const std::string& path,
    const std::vector<uint8_t>& rgba, int tex_w, int tex_h,
    int sx, int sy, int sw, int sh)
{
    sx = std::max(0, std::min(sx, tex_w - 1));
    sy = std::max(0, std::min(sy, tex_h - 1));
    sw = std::max(1, std::min(sw, tex_w - sx));
    sh = std::max(1, std::min(sh, tex_h - sy));

    std::vector<uint8_t> crop(sw * sh * 4);
    for (int y = 0; y < sh; y++)
        memcpy(crop.data() + y * sw * 4, rgba.data() + (sy + y) * tex_w * 4 + sx * 4, sw * 4);
    return save_png(path, crop.data(), sw, sh);
}

static bool save_sprite_png_trimmed(const std::string& path,
    const std::vector<uint8_t>& rgba, int tex_w, int tex_h,
    int sx, int sy, int sw, int sh)
{
    sx = std::max(0, std::min(sx, tex_w - 1));
    sy = std::max(0, std::min(sy, tex_h - 1));
    sw = std::max(1, std::min(sw, tex_w - sx));
    sh = std::max(1, std::min(sh, tex_h - sy));

    std::vector<uint8_t> crop(sw * sh * 4);
    for (int y = 0; y < sh; y++)
        memcpy(crop.data() + y * sw * 4, rgba.data() + (sy + y) * tex_w * 4 + sx * 4, sw * 4);

    int tmin = sh, tmax = -1, lmin = sw, lmax = -1;
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) {
        if (crop[(y * sw + x) * 4 + 3] > 0) {
            if (y < tmin) tmin = y; if (y > tmax) tmax = y;
            if (x < lmin) lmin = x; if (x > lmax) lmax = x;
        }
    }
    if (tmax < 0) return save_png(path, crop.data(), sw, sh);

    int cw = lmax - lmin + 1, ch = tmax - tmin + 1;
    int side = std::max(cw, ch);

    std::vector<uint8_t> square(side * side * 4, 0);
    int ox = (side - cw) / 2, oy = (side - ch) / 2;
    for (int y = 0; y < ch; y++)
        memcpy(square.data() + ((oy + y) * side + ox) * 4,
            crop.data() + ((tmin + y) * sw + lmin) * 4, cw * 4);

    return save_png(path, square.data(), side, side);
}

// ---- mod_pv_db.txt Parser ---------------------------------------------------

struct PvDifficulty {
    std::string level;
    std::string script_file;
    bool present = false;
};

struct PvEntry {
    int id = -1;
    std::string song_name;
    std::string song_name_en;
    std::string song_name_reading;
    std::string bpm;
    std::string date;
    std::string lyrics;
    std::string music;
    std::string arranger;
    std::string song_file_name;
    std::string movie_file_name;
    float sabi_start = 0, sabi_play = 30;
    PvDifficulty easy, normal, hard, extreme, exextreme;
};

static std::string pv_level_to_float(const std::string& lvl) {
    if (lvl.size() < 10) return "";
    std::string ww = lvl.substr(6, 2);
    std::string d = lvl.substr(9, 1);
    int whole = std::stoi(ww);
    return std::to_string(whole) + "." + d;
}

static std::map<int, PvEntry> parse_pv_db(const std::string& path) {
    std::ifstream f(path);
    std::map<int, PvEntry> result;
    if (!f) return result;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key.substr(0, 3) != "pv_") continue;
        size_t dot = key.find('.', 3);
        if (dot == std::string::npos) continue;
        int id;
        try { id = std::stoi(key.substr(3, dot - 3)); }
        catch (...) { continue; }

        PvEntry& e = result[id];
        e.id = id;
        std::string rest = key.substr(dot + 1);

        if (rest == "bpm")                       e.bpm = val;
        else if (rest == "date")                 e.date = sanitize_token(val);
        else if (rest == "song_name")            e.song_name = val;
        else if (rest == "song_name_en")         e.song_name_en = val;
        else if (rest == "song_name_reading")    e.song_name_reading = val;
        else if (rest == "song_file_name")       e.song_file_name = val;
        else if (rest == "movie_file_name") {
            fs::path p(val);
            p.replace_extension(".usm");
            e.movie_file_name = p.string();
        }
        else if (rest == "sabi.start_time") { try { e.sabi_start = std::stof(val); } catch (...) {} }
        else if (rest == "sabi.play_time") { try { e.sabi_play = std::stof(val); } catch (...) {} }
        else if (rest == "songinfo.lyrics")      e.lyrics = val;
        else if (rest == "songinfo.music")       e.music = val;
        else if (rest == "songinfo.arranger")    e.arranger = val;
        else if (rest.rfind("difficulty.", 0) == 0) {
            std::string d_rest = rest.substr(11);
            auto get_diff = [&](const std::string& t) -> PvDifficulty* {
                if (t == "easy")    return &e.easy;
                if (t == "normal")  return &e.normal;
                if (t == "hard")    return &e.hard;
                if (t == "extreme") return &e.extreme;
                return nullptr;
                };
            size_t d2 = d_rest.find('.');
            if (d2 == std::string::npos) continue;
            std::string dtype = d_rest.substr(0, d2);
            std::string d_rest2 = d_rest.substr(d2 + 1);

            if (d_rest2 == "length") {
                if (dtype == "easy")    e.easy.present = (val != "0");
                else if (dtype == "normal")  e.normal.present = (val != "0");
                else if (dtype == "hard")    e.hard.present = (val != "0");
                else if (dtype == "extreme") e.extreme.present = (val != "0");
            }
            else {
                size_t d3 = d_rest2.find('.');
                if (d3 == std::string::npos) continue;
                int idx = std::stoi(d_rest2.substr(0, d3));
                std::string field = d_rest2.substr(d3 + 1);

                if (dtype == "extreme" && idx == 1 && field == "attribute.extra" && val == "1") {
                    e.exextreme.present = true;
                }
                else if (dtype == "extreme" && idx == 1 && field == "level") {
                    e.exextreme.level = val;
                }
                else if (dtype == "extreme" && idx == 1 && field == "script_file_name") {
                    e.exextreme.script_file = val;
                }
                else if (idx == 0) {
                    PvDifficulty* diff = get_diff(dtype);
                    if (!diff) continue;
                    if (field == "level")            diff->level = val;
                    else if (field == "script_file_name") diff->script_file = val;
                }
            }
        }
    }
    return result;
}

// ---- Conversion Logic -------------------------------------------------------

enum class FfmpegEncoder { CPU, NVENC };
enum class FfmpegQuality { Low, Normal, High };

struct ConvOptions {
    std::string usmtoolkit = "UsmToolkit";
    bool skip_video = false;
    bool verbose = false;
    FfmpegEncoder encoder = FfmpegEncoder::NVENC;
    FfmpegQuality quality = FfmpegQuality::Normal;
};

static void convert_song(const fs::path& mod_root, const PvEntry& pv,
    const fs::path& out_root, const ConvOptions& opts)
{
    char pv_id_str[16];
    snprintf(pv_id_str, sizeof(pv_id_str), "%d", pv.id);
    std::string song_folder_name;
    std::string display_name = !pv.song_name_en.empty() ? pv.song_name_en : pv.song_name;
    if (display_name.empty()) display_name = std::string("pv_") + pv_id_str;
    for (char c : display_name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') song_folder_name += '_';
        else song_folder_name += c;
    }
    song_folder_name = "pv_" + std::string(pv_id_str) + "_" + song_folder_name;

    fs::path song_out = out_root / song_folder_name;
    fs::create_directories(song_out);

    printf("\n[pv_%s] %s -> %s\n", pv_id_str, display_name.c_str(), song_out.string().c_str());

    fs::path rom = mod_root / "rom";

    if (!pv.song_file_name.empty()) {
        std::string rel = pv.song_file_name;
        if (rel.rfind("rom/", 0) == 0) rel = rel.substr(4);
        fs::path src_ogg = rom / rel;
        if (fs::exists(src_ogg)) {
            fs::copy_file(src_ogg, song_out / "song.ogg", fs::copy_options::overwrite_existing);
            printf("  OGG: %s\n", src_ogg.filename().string().c_str());
        }
        else {
            fprintf(stderr, "  WARN: OGG not found: %s\n", src_ogg.string().c_str());
        }
    }

    if (!pv.movie_file_name.empty() && !opts.skip_video) {
        std::string rel = pv.movie_file_name;
        if (rel.rfind("rom/", 0) == 0) rel = rel.substr(4);
        fs::path rel_path = rel;
        rel_path.replace_extension(".usm");
        fs::path src_usm = rom / rel_path;
        if (!fs::exists(src_usm)) {
            src_usm = rom / rel;
            if (!fs::exists(src_usm)) src_usm = "";
        }
        if (!src_usm.empty() && fs::exists(src_usm)) {
            printf("  USM: %s\n", src_usm.filename().string().c_str());
            std::string tmp_dir = (song_out / "_tmp_usm").string();
            fs::create_directories(tmp_dir);
            std::string usm_str = src_usm.string();
            fs::path usm_copy = fs::path(tmp_dir) / src_usm.filename();
            fs::copy_file(src_usm, usm_copy, fs::copy_options::overwrite_existing);
            run(opts.usmtoolkit + " extract \"" + usm_copy.string() + "\"");

            std::string m2v_path, adx_path;
            for (auto& de : fs::directory_iterator(tmp_dir)) {
                std::string fn = to_lower(de.path().filename().string());
                if (ends_with_ci(fn, ".m2v")) m2v_path = de.path().string();
                if (ends_with_ci(fn, ".adx") || ends_with_ci(fn, ".hca") ||
                    ends_with_ci(fn, ".wav") || ends_with_ci(fn, ".aac"))
                    adx_path = de.path().string();
            }
            if (!m2v_path.empty()) {
                std::string out_mp4 = (song_out / "song.mp4").string();
                std::string ffcmd = "ffmpeg -y -i \"" + m2v_path + "\"";
                if (!adx_path.empty()) ffcmd += " -i \"" + adx_path + "\"";

                if (opts.encoder == FfmpegEncoder::NVENC) {
                    // quality: low=cq28, normal=cq20, high=cq14
                    int cq = (opts.quality == FfmpegQuality::Low) ? 28
                        : (opts.quality == FfmpegQuality::High) ? 14 : 20;
                    ffcmd += " -c:v h264_nvenc -preset p4 -cq " + std::to_string(cq) + " -pix_fmt yuv420p";
                }
                else {
                    // quality: low=crf28, normal=crf20, high=crf14
                    int crf = (opts.quality == FfmpegQuality::Low) ? 28
                        : (opts.quality == FfmpegQuality::High) ? 14 : 20;
                    ffcmd += " -c:v libx264 -preset medium -crf " + std::to_string(crf) + " -pix_fmt yuv420p";
                }

                if (!adx_path.empty()) ffcmd += " -c:a aac -b:a 192k";
                ffcmd += " \"" + out_mp4 + "\"";
                run(ffcmd);
            }
            else {
                fprintf(stderr, "  WARN: USM demux produced no m2v in %s\n", tmp_dir.c_str());
            }
            fs::remove_all(tmp_dir);
        }
        else {
            fprintf(stderr, "  WARN: USM not found (tried %s)\n", rel.c_str());
        }
    }

    auto copy_dsc = [&](const PvDifficulty& diff, const std::string& dest_name) {
        if (!diff.present || diff.script_file.empty()) return;
        std::string rel = diff.script_file;
        if (rel.rfind("rom/", 0) == 0) rel = rel.substr(4);
        fs::path src = rom / rel;
        if (fs::exists(src)) {
            fs::copy_file(src, song_out / dest_name, fs::copy_options::overwrite_existing);
            printf("  DSC: %s -> %s\n", src.filename().string().c_str(), dest_name.c_str());
        }
        else {
            fprintf(stderr, "  WARN: DSC not found: %s\n", src.string().c_str());
        }
        };
    copy_dsc(pv.easy, "song_easy.dsc");
    copy_dsc(pv.normal, "song_normal.dsc");
    copy_dsc(pv.hard, "song_hard.dsc");
    copy_dsc(pv.extreme, "song_extreme.dsc");
    copy_dsc(pv.exextreme, "song_exextreme.dsc");

    char pv_id_padded[8];
    snprintf(pv_id_padded, sizeof(pv_id_padded), "%d", pv.id);
    std::string farc_name = std::string("spr_sel_pv") + pv_id_padded + ".farc";
    fs::path farc_path = mod_root / "rom" / "2d" / farc_name;
    if (!fs::exists(farc_path)) {
        char padded4[8];
        snprintf(padded4, sizeof(padded4), "%04d", pv.id);
        farc_name = std::string("spr_sel_pv") + padded4 + ".farc";
        farc_path = mod_root / "rom" / "2d" / farc_name;
    }

    std::string jk_png_name, bg_png_name, logo_png_name;

    if (fs::exists(farc_path)) {
        printf("  FARC: %s\n", farc_path.filename().string().c_str());
        FarcArchive farc;
        if (farc.load(farc_path.string())) {
            std::string bin_name;
            for (auto& e : farc.entries) {
                if (ends_with_ci(e.name, ".bin")) { bin_name = e.name; break; }
            }
            if (!bin_name.empty()) {
                auto bin_data = farc.extract(bin_name);
                if (!bin_data.empty()) {
                    SpriteSetBin spr;
                    if (parse_bin(bin_data, spr)) {
                        std::vector<std::vector<uint8_t>> decoded_textures;
                        for (auto& t : spr.textures)
                            decoded_textures.push_back(decode_texture(t));

                        char pvs[16]; snprintf(pvs, sizeof(pvs), "%d", pv.id);
                        std::string jk_target = "SONG_JK" + std::string(pvs);
                        std::string bg_target = "SONG_BG" + std::string(pvs);
                        std::string logo_target = "SONG_LOGO" + std::string(pvs);

                        bool skipped_bg_due_to_image = false;

                        for (auto& spi : spr.sprites) {
                            std::string upper_name = spi.name;
                            for (auto& c : upper_name) c = (char)toupper(c);

                            if (upper_name == "IMAGE") {
                                uint32_t ti = spi.texture_index;
                                if (ti >= decoded_textures.size() || decoded_textures[ti].empty()) continue;
                                int tw = spr.textures[ti].width, th = spr.textures[ti].height;
                                fs::path out_img = song_out / "bg.png";
                                if (save_sprite_png(out_img.string(), decoded_textures[ti], tw, th, (int)spi.x, (int)spi.y, (int)spi.w, (int)spi.h)) {
                                    bg_png_name = "bg.png";
                                    skipped_bg_due_to_image = true;
                                    printf("  IMG: bg.png (High-res override from 'IMAGE' sprite, %dx%d from tex%d)\n", (int)spi.w, (int)spi.h, ti);
                                    break;
                                }
                            }
                        }

                        auto process_sprite = [&](const SpriteInfo& spi, const std::string& target_file, bool trim_image) {
                            uint32_t ti = spi.texture_index;
                            if (ti >= decoded_textures.size() || decoded_textures[ti].empty()) return false;
                            int tw = spr.textures[ti].width, th = spr.textures[ti].height;
                            fs::path out_img = song_out / target_file;
                            bool ok = trim_image
                                ? save_sprite_png_trimmed(out_img.string(), decoded_textures[ti], tw, th, (int)spi.x, (int)spi.y, (int)spi.w, (int)spi.h)
                                : save_sprite_png(out_img.string(), decoded_textures[ti], tw, th, (int)spi.x, (int)spi.y, (int)spi.w, (int)spi.h);
                            if (ok) {
                                printf("  IMG: %s (%dx%d from tex%d)\n", target_file.c_str(), (int)spi.w, (int)spi.h, ti);
                                return true;
                            }
                            return false;
                            };

                        for (auto& spi : spr.sprites) {
                            std::string upper_name = spi.name;
                            for (auto& c : upper_name) c = (char)toupper(c);

                            if (upper_name == jk_target && jk_png_name.empty()) {
                                if (process_sprite(spi, "jk.png", true)) jk_png_name = "jk.png";
                            }
                            else if (upper_name == bg_target && bg_png_name.empty() && !skipped_bg_due_to_image) {
                                if (process_sprite(spi, "bg.png", false)) bg_png_name = "bg.png";
                            }
                            else if (upper_name == logo_target && logo_png_name.empty()) {
                                if (process_sprite(spi, "logo.png", false)) logo_png_name = "logo.png";
                            }
                        }

                        for (auto& spi : spr.sprites) {
                            std::string upper_name = spi.name;
                            for (auto& c : upper_name) c = (char)toupper(c);

                            if (upper_name == "SONG_JK001" && jk_png_name.empty()) {
                                if (process_sprite(spi, "jk.png", true)) jk_png_name = "jk.png";
                            }
                            else if (upper_name == "SONG_BG001" && bg_png_name.empty() && !skipped_bg_due_to_image) {
                                if (process_sprite(spi, "bg.png", false)) bg_png_name = "bg.png";
                            }
                            else if (upper_name == "SONG_LOGO001" && logo_png_name.empty()) {
                                if (process_sprite(spi, "logo.png", false)) logo_png_name = "logo.png";
                            }
                        }
                    }
                    else {
                        fprintf(stderr, "  WARN: Failed to parse bin: %s\n", bin_name.c_str());
                    }
                }
            }
        }
    }
    else {
        printf("  INFO: No FARC found for pv_%d (sprites skipped)\n", pv.id);
    }

    std::string display_en = !pv.song_name_en.empty() ? pv.song_name_en : pv.song_name;

    auto diff_level = [](const PvDifficulty& d) -> std::string {
        if (!d.present || d.level.empty()) return "";
        return pv_level_to_float(d.level);
        };
    std::ostringstream ini;
    ini << "difficulty.easy.level=" << diff_level(pv.easy) << "\n";
    ini << "difficulty.easy.script_file_name=" << (pv.easy.present ? "song_easy.dsc" : "") << "\n";
    ini << "\n";
    ini << "difficulty.normal.level=" << diff_level(pv.normal) << "\n";
    ini << "difficulty.normal.script_file_name=" << (pv.normal.present ? "song_normal.dsc" : "") << "\n";
    ini << "\n";
    ini << "difficulty.hard.level=" << diff_level(pv.hard) << "\n";
    ini << "difficulty.hard.script_file_name=" << (pv.hard.present ? "song_hard.dsc" : "") << "\n";
    ini << "\n";
    ini << "difficulty.extreme.level=" << diff_level(pv.extreme) << "\n";
    ini << "difficulty.extreme.script_file_name=" << (pv.extreme.present ? "song_extreme.dsc" : "") << "\n";
    ini << "\n";
    ini << "difficulty.exextreme.level=" << diff_level(pv.exextreme) << "\n";
    ini << "difficulty.exextreme.script_file_name=" << (pv.exextreme.present ? "song_exextreme.dsc" : "") << "\n";
    ini << "\n";

    bool has_mp4 = fs::exists(song_out / "song.mp4");
    ini << "movie_file_name=" << (has_mp4 ? "song.mp4" : "") << "\n";
    ini << "song_file_name=" << (fs::exists(song_out / "song.ogg") ? "song.ogg" : "") << "\n";
    ini << "background_file_name=" << bg_png_name << "\n";
    ini << "jacket_file_name=" << jk_png_name << "\n";
    ini << "logo_file_name=" << logo_png_name << "\n";
    ini << "\n";
    ini << "songinfo.name=" << display_en << "\n";
    if (!pv.arranger.empty()) ini << "songinfo.arranger=" << pv.arranger << "\n";
    if (!pv.lyrics.empty())   ini << "songinfo.lyrics=" << pv.lyrics << "\n";
    if (!pv.music.empty())    ini << "songinfo.music=" << pv.music << "\n";
    ini << "songinfo.bpm=" << pv.bpm << "\n";
    ini << "songinfo.date=" << pv.date << "\n";
    ini << "songinfo.previewplaytime=" << fmt_time(pv.sabi_play) << "\n";
    ini << "songinfo.previewstarttime=" << fmt_time(pv.sabi_start) << "\n";
    ini << "songinfo.slide=1\n";

    std::ofstream ini_f(song_out / "song.ini");
    ini_f << ini.str();
    printf("  INI: song.ini written\n");
}


// ---- Native Win32 GUI Implementation ----------------------------------------

HWND g_hMain, g_hEditMod, g_hEditOut, g_hSearch, g_hList, g_hRunBtn;
HWND g_hChkSkipVideo, g_hCmbQuality, g_hCmbEncoder;

// Try every drive letter for "X:\SteamLibrary\steamapps\common\Hatsune Miku Project DIVA Mega Mix Plus\mods"
static std::string AutoDetectModsFolder() {
    const char* rel = "SteamLibrary\\steamapps\\common\\Hatsune Miku Project DIVA Mega Mix Plus\\mods";
    for (char d = 'A'; d <= 'Z'; d++) {
        std::string path = std::string(1, d) + ":\\" + rel;
        if (fs::is_directory(path)) return path;
    }
    return "";
}

struct LoadedSong {
    int pv_id;
    std::string name;
    std::string mod_root;
    bool selected = true;
};

std::vector<LoadedSong> g_Songs;
std::atomic<bool> g_IsRunning = false;

std::string BrowseFolder(HWND owner, const char* title) {
    IFileOpenDialog* pfd;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD dwOptions;
        pfd->GetOptions(&dwOptions);
        pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);

        int titleLen = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
        std::wstring wTitle(titleLen, 0);
        MultiByteToWideChar(CP_UTF8, 0, title, -1, &wTitle[0], titleLen);
        pfd->SetTitle(wTitle.c_str());

        if (SUCCEEDED(pfd->Show(owner))) {
            IShellItem* psi;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszPath;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                    int size = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, NULL, 0, NULL, NULL);
                    std::string result(size - 1, 0);
                    WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, &result[0], size, NULL, NULL);
                    CoTaskMemFree(pszPath);
                    psi->Release();
                    pfd->Release();
                    return result;
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    return "";
}

std::string find_pv_db(const std::string& mod_root) {
    fs::path root(mod_root);
    for (const auto& rel : { "rom/mod_pv_db.txt", "mod_pv_db.txt", "rom/pv_db.txt" }) {
        fs::path p = root / rel;
        if (fs::exists(p) && !fs::is_directory(p)) return p.string();
    }
    return "";
}

std::vector<std::string> collect_mod_roots(const std::string& path) {
    if (!find_pv_db(path).empty()) return { path };
    std::vector<std::string> roots;
    try {
        for (auto& entry : fs::directory_iterator(path)) {
            if (entry.is_directory() && !find_pv_db(entry.path().string()).empty()) {
                roots.push_back(entry.path().string());
            }
        }
    }
    catch (...) {}
    return roots;
}

void ApplyFilter() {
    ListView_DeleteAllItems(g_hList);
    char query[256];
    GetWindowTextA(g_hSearch, query, 256);
    std::string sq = to_lower(query);

    int row = 0;
    for (size_t i = 0; i < g_Songs.size(); i++) {
        char label[512];
        snprintf(label, sizeof(label), "[pv_%04d]  %s  (%s)",
            g_Songs[i].pv_id, g_Songs[i].name.c_str(), fs::path(g_Songs[i].mod_root).filename().string().c_str());

        if (!sq.empty() && to_lower(label).find(sq) == std::string::npos) continue;

        LVITEMA lvi = { 0 };
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = row++;
        lvi.pszText = label;
        lvi.lParam = (LPARAM)i;
        ListView_InsertItem(g_hList, &lvi);
        ListView_SetCheckState(g_hList, lvi.iItem, g_Songs[i].selected);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // Inside case WM_CREATE:
        HFONT hFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        HFONT hLogFont = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");

        // Change the parameter name from 'h' to 'height' to avoid redefinition conflicts
        auto createCtrl = [&](const char* cls, const char* txt, DWORD style, int x, int y, int width, int height, int id) -> HWND {
            HWND handle = CreateWindowExA(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, width, height, hwnd, (HMENU)(INT_PTR)id, NULL, NULL);

            // Ensure we send the message to the correct HWND
            SendMessageA(handle, WM_SETFONT, (WPARAM)(id == 100 ? hLogFont : hFont), TRUE);
            return handle;
            };

        createCtrl("STATIC", "Mods Folder:", 0, 10, 15, 90, 20, 0);
        g_hEditMod = createCtrl("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 100, 10, 400, 25, 1);
        createCtrl("BUTTON", "Browse...", 0, 510, 10, 80, 25, 2);

        createCtrl("STATIC", "Output Folder:", 0, 10, 45, 90, 20, 0);
        g_hEditOut = createCtrl("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 100, 40, 400, 25, 3);
        createCtrl("BUTTON", "Browse...", 0, 510, 40, 80, 25, 4);

        createCtrl("STATIC", "Quality:", 0, 10, 73, 50, 20, 0);
        g_hCmbQuality = createCtrl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 60, 70, 90, 100, 11);
        SendMessageA(g_hCmbQuality, CB_ADDSTRING, 0, (LPARAM)"Low");
        SendMessageA(g_hCmbQuality, CB_ADDSTRING, 0, (LPARAM)"Normal");
        SendMessageA(g_hCmbQuality, CB_ADDSTRING, 0, (LPARAM)"High");
        SendMessageA(g_hCmbQuality, CB_SETCURSEL, 1, 0); // default Normal

        createCtrl("STATIC", "Encoder:", 0, 162, 73, 55, 20, 0);
        g_hCmbEncoder = createCtrl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 218, 70, 110, 100, 12);
        SendMessageA(g_hCmbEncoder, CB_ADDSTRING, 0, (LPARAM)"H.264 (CPU)");
        SendMessageA(g_hCmbEncoder, CB_ADDSTRING, 0, (LPARAM)"H.264 (NVENC)");
        SendMessageA(g_hCmbEncoder, CB_SETCURSEL, 1, 0); // default NVENC

        g_hChkSkipVideo = createCtrl("BUTTON", "Skip movie encode", BS_AUTOCHECKBOX, 340, 74, 150, 20, 5);

        createCtrl("STATIC", "Search:", 0, 10, 105, 50, 20, 0);
        g_hSearch = createCtrl("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 60, 100, 200, 25, 6);
        createCtrl("BUTTON", "Select All", 0, 270, 100, 80, 25, 7);
        createCtrl("BUTTON", "Deselect All", 0, 360, 100, 80, 25, 8);

        g_hList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS, 10, 130, 580, 200, hwnd, (HMENU)9, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SendMessage(g_hList, WM_SETFONT, (WPARAM)hFont, TRUE);
        LVCOLUMNA col = { LVCF_WIDTH, 0, 550 };
        ListView_InsertColumn(g_hList, 0, &col);

        g_hRunBtn = createCtrl("BUTTON", "Convert Selected", 0, 10, 340, 150, 30, 10);

        g_hLog = createCtrl("EDIT", "", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 10, 380, 580, 130, 100);
        return 0;
    }

    case WM_COMMAND: {
        if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == 6) ApplyFilter();

        switch (LOWORD(wParam)) {
        case 2: {
            std::string path = BrowseFolder(hwnd, "Select Parent Mods Folder");
            if (!path.empty()) {
                SetWindowTextA(g_hEditMod, path.c_str());
                g_Songs.clear();
                auto roots = collect_mod_roots(path);
                if (roots.empty()) MessageBoxA(hwnd, "No mods found.", "Warning", MB_ICONWARNING);
                for (auto& root : roots) {
                    auto pvs = parse_pv_db(find_pv_db(root));
                    for (auto& [id, pv] : pvs) {
                        std::string name = !pv.song_name_en.empty() ? pv.song_name_en : pv.song_name;
                        if (name.empty()) name = "pv_" + std::to_string(id);
                        g_Songs.push_back({ id, name, root, true });
                    }
                }
                GuiLog("Loaded %zu songs from %zu mod(s).\r\n", g_Songs.size(), roots.size());
                ApplyFilter();
            }
            break;
        }
        case 4: {
            std::string path = BrowseFolder(hwnd, "Select Output Folder");
            if (!path.empty()) SetWindowTextA(g_hEditOut, path.c_str());
            break;
        }
        case 7:
        case 8: {
            bool state = (LOWORD(wParam) == 7);
            int count = ListView_GetItemCount(g_hList);
            for (int i = 0; i < count; i++) {
                ListView_SetCheckState(g_hList, i, state);
                LVITEMA lvi = { 0 }; lvi.mask = LVIF_PARAM; lvi.iItem = i;
                ListView_GetItem(g_hList, &lvi);
                g_Songs[lvi.lParam].selected = state;
            }
            break;
        }
        case 10: {
            if (g_IsRunning) break;

            int count = ListView_GetItemCount(g_hList);
            for (int i = 0; i < count; i++) {
                LVITEMA lvi = { 0 }; lvi.mask = LVIF_PARAM; lvi.iItem = i;
                ListView_GetItem(g_hList, &lvi);
                g_Songs[lvi.lParam].selected = ListView_GetCheckState(g_hList, i);
            }

            char outPath[512];
            GetWindowTextA(g_hEditOut, outPath, 512);
            if (strlen(outPath) == 0) {
                MessageBoxA(hwnd, "Specify output folder first.", "Error", MB_ICONERROR);
                break;
            }

            bool skipVideo = SendMessage(g_hChkSkipVideo, BM_GETCHECK, 0, 0) == BST_CHECKED;
            int qualSel = (int)SendMessage(g_hCmbQuality, CB_GETCURSEL, 0, 0);
            int encSel = (int)SendMessage(g_hCmbEncoder, CB_GETCURSEL, 0, 0);

            std::vector<LoadedSong> jobs;
            for (auto& s : g_Songs) if (s.selected) jobs.push_back(s);
            if (jobs.empty()) {
                MessageBoxA(hwnd, "Select at least one song.", "Info", MB_ICONINFORMATION);
                break;
            }

            g_IsRunning = true;
            EnableWindow(g_hRunBtn, FALSE);
            std::string outStr = outPath;

            std::thread([jobs, outStr, skipVideo, qualSel, encSel]() {
                ConvOptions opts;
                opts.skip_video = skipVideo;
                opts.quality = (qualSel == 0) ? FfmpegQuality::Low : (qualSel == 2) ? FfmpegQuality::High : FfmpegQuality::Normal;
                opts.encoder = (encSel == 0) ? FfmpegEncoder::CPU : FfmpegEncoder::NVENC;

                int done = 0;
                for (const auto& job : jobs) {
                    GuiLog("\r\n--- Processing [pv_%04d] %s ---\r\n", job.pv_id, job.name.c_str());
                    auto pvs = parse_pv_db(find_pv_db(job.mod_root));
                    if (pvs.count(job.pv_id)) {
                        convert_song(job.mod_root, pvs[job.pv_id], outStr, opts);
                    }
                    done++;
                    GuiLog("Progress: %d / %zu songs processed.\r\n", done, jobs.size());
                }
                GuiLog("\r\nAll selected tasks finished.\r\n");
                g_IsRunning = false;
                EnableWindow(g_hRunBtn, TRUE);
                }).detach();
            break;
        }
        }
        break;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam), h = HIWORD(lParam);
        MoveWindow(g_hEditMod, 100, 10, w - 200, 25, TRUE);
        MoveWindow(GetDlgItem(hwnd, 2), w - 90, 10, 80, 25, TRUE);
        MoveWindow(g_hEditOut, 100, 40, w - 200, 25, TRUE);
        MoveWindow(GetDlgItem(hwnd, 4), w - 90, 40, 80, 25, TRUE);

        // Row 3: Quality | Encoder | Skip checkbox (right-anchored)
        MoveWindow(g_hCmbQuality, 60, 70, 90, 100, TRUE);
        MoveWindow(g_hCmbEncoder, 218, 70, 110, 100, TRUE);
        MoveWindow(g_hChkSkipVideo, 218 + 110 + 10, 74, 150, 20, TRUE);

        MoveWindow(g_hList, 10, 130, w - 20, h - 280, TRUE);
        ListView_SetColumnWidth(g_hList, 0, w - 40);

        MoveWindow(g_hRunBtn, 10, h - 145, 150, 30, TRUE);
        MoveWindow(g_hLog, 10, h - 105, w - 20, 95, TRUE);
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    const char* clsName = "MmConverterGuiClass";
    WNDCLASSEXA wc = { sizeof(WNDCLASSEXA) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.lpszClassName = clsName;
    RegisterClassExA(&wc);

    g_hMain = CreateWindowExA(0, clsName, "mm_converter GUI", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 560, NULL, NULL, hInstance, NULL);

    {
        std::string detected = AutoDetectModsFolder();
        if (!detected.empty()) {
            SetWindowTextA(g_hEditMod, detected.c_str());
            g_Songs.clear();
            auto roots = collect_mod_roots(detected);
            if (roots.empty()) {
                MessageBoxA(g_hMain, "No mods found.", "Warning", MB_ICONWARNING);
            }
            else {
                for (auto& root : roots) {
                    auto pvs = parse_pv_db(find_pv_db(root));
                    for (auto& [id, pv] : pvs) {
                        std::string name = !pv.song_name_en.empty() ? pv.song_name_en : pv.song_name;
                        if (name.empty()) name = "pv_" + std::to_string(id);
                        g_Songs.push_back({ id, name, root, true });
                    }
                }
                GuiLog("Loaded %zu songs from %zu mod(s).\r\n", g_Songs.size(), roots.size());
                ApplyFilter();
            }
        }
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}