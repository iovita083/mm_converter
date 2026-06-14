// mm_converter.cpp
// Converts Project DIVA Mega Mix+ song mods to MicroMix+ format.
// Deps: zlib (FARC decompression), libpng (image output), stb_image_write (png fallback)
// FFmpeg must be in PATH for USM->MP4 conversion.
// UsmToolkit (dotnet) must be in PATH or specified via --usmtoolkit for USM demux.

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
#include <zlib.h>

namespace fs = std::filesystem;

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

// Strip anything after the first whitespace (removes inline comments like "(FORMAT IS ...)")
static std::string sanitize_token(const std::string& s) {
    size_t sp = s.find_first_of(" \t");
    return sp == std::string::npos ? s : s.substr(0, sp);
}

// Truncate float to 2 decimal places (no rounding)
static std::string fmt_time(float v) {
    int trunc = (int)(v * 100.0f);
    std::ostringstream os;
    os << (trunc / 100) << "." << std::setw(2) << std::setfill('0') << (trunc % 100);
    return os.str();
}

static void run(const std::string& cmd) {
    printf("  $ %s\n", cmd.c_str());
    system(cmd.c_str());
}

// Read little-endian integers from raw buffer
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
// Supports FArc (uncompressed) and FArC (gzip-compressed) -- the DT variants used by MM+.

struct FarcEntry {
    std::string name;
    uint32_t offset;
    uint32_t size;          // on-disk (possibly compressed)
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
            // FARC has extra flags/padding fields; we only support non-encrypted DT
            uint32_t flags = read_u32be(p + 8);
            bool is_compressed = (flags & 2) != 0;
            bool is_encrypted = (flags & 4) != 0;
            if (is_encrypted) {
                fprintf(stderr, "Encrypted FARC not supported: %s\n", path.c_str());
                return false;
            }
            // alignment at +16, entry_padding at +20, header_padding at +24, count=0
            uint32_t entry_padding = read_u32be(p + 20);
            uint32_t header_padding = read_u32be(p + 24);
            size_t pos = 28 + header_padding;
            while (pos < header_size && pos < sz) {
                FarcEntry e;
                e.compressed = is_compressed;
                while (pos < sz && p[pos]) e.name += (char)p[pos++];
                pos++; // null
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
            // FArc / FArC: alignment at +8, then entries
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

    // Extract entry to a vector. Returns empty on failure.
    std::vector<uint8_t> extract(const std::string& name) const {
        for (auto& e : entries) {
            if (to_lower(e.name) == to_lower(name)) {
                const uint8_t* src = file_data.data() + e.offset;
                if (!e.compressed) {
                    return std::vector<uint8_t>(src, src + e.size);
                }
                // gzip decompress (windowBits=47 = 15+32, handles gzip and zlib)
                std::vector<uint8_t> out(e.uncompressed);
                z_stream zs{};
                zs.next_in  = const_cast<Bytef*>(src);
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

    // Returns nullptr if not found
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
    // YCbCr only: second sub-texture (chroma, half-size BC5)
    int chroma_width = 0, chroma_height = 0;
    std::vector<uint8_t> chroma_data;
    bool is_ycbcr = false;
};

struct SpriteSetBin {
    std::vector<SpriteInfo> sprites;
    std::vector<TextureInfo> textures;
};

// (section-format structs removed; Mega Mix+ bins are flat format)


// Flat-format .bin layout (Mega Mix+):
//   [0]  u32 signature (ignored)
//   [4]  u32 texturesOffset    -- offset to TXP3 TextureSet, relative to file start
//   [8]  i32 textureCount
//   [12] i32 spriteCount
//   [16] u32 spritesOffset     -- relative to file start
//   [20] u32 texNamesOffset    -- relative to file start, array of u32 string offsets
//   [24] u32 sprNamesOffset    -- relative to file start, array of u32 string offsets
//   [28] u32 sprModesOffset    -- relative to file start (unused here)
//
// TXP3 TextureSet at texturesOffset:
//   [0] u32 0x03505854, [4] u32 count, [8] u32 countInfo
//   [12] count x u32 offsets -> TXP4, each relative to TXP3 block start
//
// TXP4 Texture at (txp3_base + offset):
//   [0] u32 0x04505854, [4] u32 subCount, [8] u32 info (mips|array<<8)
//   [12] subCount x u32 offsets -> TXP2, each relative to TXP4 block start
//
// TXP2 SubTexture at (txp4_base + offset):
//   [0] u32 0x02505854, [4] u32 width, [8] u32 height, [12] u32 format,
//   [16] u32 id, [20] u32 dataSize, [24..] pixel data

static bool parse_subtexture(const uint8_t* txp4, size_t avail, uint32_t sub_idx,
    int& w, int& h, int& fmt, std::vector<uint8_t>& data)
{
    if (avail < 12u + (sub_idx + 1) * 4u) return false;
    uint32_t sub_off = read_u32le(txp4 + 12 + sub_idx * 4);
    if (sub_off + 24 > avail) return false;
    const uint8_t* sp = txp4 + sub_off;
    if (read_u32le(sp) != 0x02505854) return false;
    w   = (int)read_u32le(sp + 4);
    h   = (int)read_u32le(sp + 8);
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

    // YCbCr: ATI2, array_size==1, mip_count==2 (luma mip0, chroma mip1)
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

    // sig at [0] intentionally ignored
    uint32_t tex_off      = read_u32le(p + 4);
    int      tex_count    = (int)read_u32le(p + 8);
    int      spr_count    = (int)read_u32le(p + 12);
    uint32_t spr_off      = read_u32le(p + 16);
    uint32_t tex_names_off = read_u32le(p + 20);
    uint32_t spr_names_off = read_u32le(p + 24);

    auto safe = [&](uint32_t off, size_t need) -> const uint8_t* {
        if ((size_t)off + need > sz) return nullptr;
        return p + off;
    };

    // Sprites
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

    // Sprite names
    const uint8_t* snp = safe(spr_names_off, (size_t)spr_count * 4);
    if (snp) {
        for (int i = 0; i < spr_count; i++) {
            uint32_t str_off = read_u32le(snp + i * 4);
            const uint8_t* s = safe(str_off, 1);
            if (s) out.sprites[i].name = (const char*)s;
        }
    }

    // Textures via TXP3 block
    out.textures.resize(tex_count);
    const uint8_t* txp3 = safe(tex_off, 12);
    if (!txp3 || read_u32le(txp3) != 0x03505854) return false;
    uint32_t txp3_count = read_u32le(txp3 + 4);
    if ((int)txp3_count < tex_count) tex_count = (int)txp3_count;
    const uint8_t* txp3_table = safe(tex_off + 12, (size_t)tex_count * 4);
    if (!txp3_table) return false;
    for (int i = 0; i < tex_count; i++) {
        uint32_t txp4_rel = read_u32le(txp3_table + i * 4); // relative to txp3 start
        uint32_t txp4_abs = tex_off + txp4_rel;
        if (txp4_abs >= sz) continue;
        parse_texture(p + txp4_abs, sz - txp4_abs, out.textures[i]);
    }

    // Texture names
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

// (parse_sprc_data and parse_mtxd_data removed; Mega Mix+ bins are flat format)

// ---- DXT / Block Compression Decoding ---------------------------------------
// Minimal DXT1/DXT3/DXT5/ATI2 decoder, outputs RGBA8.
// ATI2 (BC5 two-channel) is used for YCbCr � we handle it specially.

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

static void decode_bc3_alpha_block(const uint8_t* src, uint8_t* alpha_out /*16 bytes*/) {
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

// Decode one channel from ATI2/BC5 (rg block)
static void decode_ati1_block(const uint8_t* src, uint8_t* ch_out /*16 bytes*/) {
    decode_bc3_alpha_block(src, ch_out);
}



// Decode one BC5 channel block (same as ATI1/BC4)
static void decode_bc5_channel(const uint8_t* src, int w, int h, std::vector<float>& ch0, std::vector<float>& ch1) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    ch0.resize(w * h); ch1.resize(w * h);
    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        const uint8_t* b = src + (by * bw + bx) * 16;
        uint8_t a0[16], a1[16];
        decode_bc3_alpha_block(b,     a0);
        decode_bc3_alpha_block(b + 8, a1);
        for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++) {
            int ox = bx*4+px, oy = by*4+py;
            if (ox >= w || oy >= h) continue;
            ch0[oy*w+ox] = a0[py*4+px] / 255.0f;
            ch1[oy*w+ox] = a1[py*4+px] / 255.0f;
        }
    }
}

// Bilinear sample of a float buffer at sub-pixel coords
static float bilinear(const std::vector<float>& buf, int bw, int bh, float u, float v) {
    float fx = u * bw - 0.5f, fy = v * bh - 0.5f;
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1, y1 = y0 + 1;
    float tx = fx - x0, ty = fy - y0;
    auto get = [&](int x, int y) { return buf[std::max(0,std::min(y,bh-1))*bw + std::max(0,std::min(x,bw-1))]; };
    return (get(x0,y0)*(1-tx) + get(x1,y0)*tx)*(1-ty) + (get(x0,y1)*(1-tx) + get(x1,y1)*tx)*ty;
}

// Decode a full texture to RGBA8. Returns empty on failure.
static std::vector<uint8_t> decode_texture(const TextureInfo& tex) {
    int w = tex.width, h = tex.height;
    std::vector<uint8_t> rgba(w * h * 4, 0xFF);
    int fmt = tex.format;
    // fmt: DXT1=6, DXT1a=7, DXT3=8, DXT5=9, ATI1=10, ATI2=11, RGBA8=2, RGB8=1

    // YCbCr: luma BC5 (R=Y, G=alpha) + half-res chroma BC5 (R=Cb, G=Cr), Rec.709
    if (tex.is_ycbcr && !tex.chroma_data.empty()) {
        std::vector<float> lY, lA, cCb, cCr;
        decode_bc5_channel(tex.data.data(), w, h, lY, lA);
        decode_bc5_channel(tex.chroma_data.data(), tex.chroma_width, tex.chroma_height, cCb, cCr);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            float Y  = lY[y*w+x];
            float A  = lA[y*w+x];
            float Cb = bilinear(cCb, tex.chroma_width, tex.chroma_height, u, v) * 1.003922f - 0.503929f;
            float Cr = bilinear(cCr, tex.chroma_width, tex.chroma_height, u, v) * 1.003922f - 0.503929f;
            auto clamp01 = [](float f) { return std::max(0.0f, std::min(1.0f, f)); };
            uint8_t R = (uint8_t)(clamp01(Y + 1.5748f * Cr) * 255.0f);
            uint8_t G = (uint8_t)(clamp01(Y - 0.1873f * Cb - 0.4681f * Cr) * 255.0f);
            uint8_t B = (uint8_t)(clamp01(Y + 1.8556f * Cb) * 255.0f);
            uint8_t Ao = (uint8_t)(clamp01(A) * 255.0f);
            int dst_y = h - 1 - y; // bottom-up, same as RGBA8
            int idx = (dst_y*w+x)*4;
            rgba[idx+0] = R; rgba[idx+1] = G; rgba[idx+2] = B; rgba[idx+3] = Ao;
        }
        return rgba;
    }
    if (fmt == 2) { // RGBA8 - bytes on disk: R,G,B,A, stored bottom-up
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
    if (fmt == 1) { // RGB8 - bytes on disk: R,G,B, stored bottom-up
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
    bool is_ati2 = (fmt == 11); // YCbCr when 2 sub-textures (handled as BC5 here)

    if (!is_dxt1 && !is_dxt3 && !is_dxt5 && !is_ati1 && !is_ati2) {
        fprintf(stderr, "Unsupported texture format %d\n", fmt);
        return rgba; // return solid magenta-ish
    }

    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    int block_size = (is_dxt1 || is_ati1) ? 8 : 16;
    const uint8_t* src = tex.data.data();

    for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
        int bi = by * bw + bx;
        const uint8_t* b = src + bi * block_size;
        // temp 4x4 block
        uint8_t block_rgba[4 * 4 * 4] = {};
        if (is_dxt1) {
            decode_bc1_block(b, block_rgba, 16);
        }
        else if (is_dxt3) {
            // 8 bytes alpha (4-bit per pixel), then 8 bytes DXT1 color
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
            // Plain BC5 (non-YCbCr): store RG, fill B=0, A=255
            uint8_t ch0[16], ch1[16];
            decode_ati1_block(b, ch0);
            decode_ati1_block(b + 8, ch1);
            for (int i = 0; i < 16; i++) {
                block_rgba[i*4+0] = ch0[i];
                block_rgba[i*4+1] = ch1[i];
                block_rgba[i*4+2] = 0;
                block_rgba[i*4+3] = 255;
            }
        }
        // Copy block to output, flipped (textures are stored upside-down)
        for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++) {
            int ox = bx * 4 + px, oy = by * 4 + py;
            if (ox >= w || oy >= h) continue;
            // Flip vertically: source row 0 -> dest row (h-1-oy)
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

// ---- Minimal PNG Writer (IDAT with zlib) ------------------------------------
// Writes an RGBA PNG without external libpng dependency.

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
    // Build raw scanlines with filter byte 0
    std::vector<uint8_t> raw;
    raw.reserve((w * 4 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0); // filter none
        raw.insert(raw.end(), rgba + y * w * 4, rgba + y * w * 4 + w * 4);
    }
    // Compress with zlib
    uLongf clen = compressBound((uLong)raw.size());
    std::vector<uint8_t> compressed(clen);
    if (compress2(compressed.data(), &clen, raw.data(), (uLong)raw.size(), 9) != Z_OK)
        return false;
    compressed.resize(clen);

    std::vector<uint8_t> out;
    // PNG signature
    const uint8_t sig[] = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A };
    out.insert(out.end(), sig, sig + 8);

    // IHDR
    uint8_t ihdr[13];
    ihdr[0] = w >> 24; ihdr[1] = w >> 16; ihdr[2] = w >> 8; ihdr[3] = w;
    ihdr[4] = h >> 24; ihdr[5] = h >> 16; ihdr[6] = h >> 8; ihdr[7] = h;
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // RGBA
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    write_chunk(out, "IHDR", ihdr, 13);
    write_chunk(out, "IDAT", compressed.data(), (uint32_t)compressed.size());
    write_chunk(out, "IEND", nullptr, 0);

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((char*)out.data(), out.size());
    return true;
}

// Crop a region from a decoded RGBA texture and save as PNG
static bool save_sprite_png(const std::string& path,
    const std::vector<uint8_t>& rgba, int tex_w, int tex_h,
    int sx, int sy, int sw, int sh)
{
    // Clamp
    sx = std::max(0, std::min(sx, tex_w - 1));
    sy = std::max(0, std::min(sy, tex_h - 1));
    sw = std::max(1, std::min(sw, tex_w - sx));
    sh = std::max(1, std::min(sh, tex_h - sy));

    std::vector<uint8_t> crop(sw * sh * 4);
    for (int y = 0; y < sh; y++)
        memcpy(crop.data() + y * sw * 4, rgba.data() + (sy + y) * tex_w * 4 + sx * 4, sw * 4);
    return save_png(path, crop.data(), sw, sh);
}

// Crop, trim transparent border, center content in a square canvas.
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

    // Tight bbox
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

    // Center trimmed content on a square transparent canvas
    std::vector<uint8_t> square(side * side * 4, 0);
    int ox = (side - cw) / 2, oy = (side - ch) / 2;
    for (int y = 0; y < ch; y++)
        memcpy(square.data() + ((oy + y) * side + ox) * 4,
               crop.data() + ((tmin + y) * sw + lmin) * 4, cw * 4);

    return save_png(path, square.data(), side, side);
}

// ---- mod_pv_db.txt Parser ---------------------------------------------------

struct PvDifficulty {
    std::string level;       // e.g. "PV_LV_07_5"
    std::string script_file; // e.g. "rom/script/pv_0001_extreme.dsc"
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
    std::string song_file_name;  // rom/sound/song/pv_####.ogg
    std::string movie_file_name; // rom/movie/pv_####.mp4  (but actual file is .usm)
    float sabi_start = 0, sabi_play = 30;
    PvDifficulty easy, normal, hard, extreme, exextreme;
};

static std::string pv_level_to_float(const std::string& lvl) {
    // PV_LV_07_5 -> "7.5", PV_LV_10_0 -> "10.0"
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

        // Parse key: pv_NNNN.field.field...
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
        else if (rest == "movie_file_name")      e.movie_file_name = val;
        else if (rest == "sabi.start_time") { try { e.sabi_start = std::stof(val); } catch (...) {} }
        else if (rest == "sabi.play_time") { try { e.sabi_play = std::stof(val); } catch (...) {} }
        else if (rest == "songinfo.lyrics")      e.lyrics = val;
        else if (rest == "songinfo.music")       e.music = val;
        else if (rest == "songinfo.arranger")    e.arranger = val;
        else if (rest.rfind("difficulty.", 0) == 0) {
            // difficulty.<type>.<index>.<field>
            std::string d_rest = rest.substr(11);
            auto get_diff = [&](const std::string& t) -> PvDifficulty* {
                if (t == "easy")    return &e.easy;
                if (t == "normal")  return &e.normal;
                if (t == "hard")    return &e.hard;
                if (t == "extreme") return &e.extreme;
                return nullptr;
                };
            // find type
            size_t d2 = d_rest.find('.');
            if (d2 == std::string::npos) continue;
            std::string dtype = d_rest.substr(0, d2);
            std::string d_rest2 = d_rest.substr(d2 + 1);

            // d_rest2 = "0.field" or "length"
            if (d_rest2 == "length") {
                if (dtype == "easy")    e.easy.present = (val != "0");
                else if (dtype == "normal")  e.normal.present = (val != "0");
                else if (dtype == "hard")    e.hard.present = (val != "0");
                else if (dtype == "extreme") e.extreme.present = (val != "0");
            }
            else {
                // "0.level" / "0.script_file_name" / "1.attribute.extra" etc.
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

struct ConvOptions {
    std::string usmtoolkit = "UsmToolkit"; // executable name/path
    bool skip_video = false;
    bool verbose = false;
};

static void convert_song(const fs::path& mod_root, const PvEntry& pv,
    const fs::path& out_root, const ConvOptions& opts)
{
    char pv_id_str[16];
    snprintf(pv_id_str, sizeof(pv_id_str), "%d", pv.id);
    // Sanitize name for folder
    std::string song_folder_name;
    std::string display_name = !pv.song_name_en.empty() ? pv.song_name_en : pv.song_name;
    if (display_name.empty()) display_name = std::string("pv_") + pv_id_str;
    // Replace chars unsafe in paths
    for (char c : display_name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') song_folder_name += '_';
        else song_folder_name += c;
    }
    song_folder_name = "pv_" + std::string(pv_id_str) + "_" + song_folder_name;

    fs::path song_out = out_root / song_folder_name;
    fs::create_directories(song_out);

    printf("\n[pv_%s] %s -> %s\n", pv_id_str, display_name.c_str(), song_out.string().c_str());

    // rom base
    fs::path rom = mod_root / "rom";

    // --- Copy OGG ---
    if (!pv.song_file_name.empty()) {
        // Normalize rom-relative path
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

    // --- Convert USM -> MP4 ---
    if (!pv.movie_file_name.empty() && !opts.skip_video) {
        std::string rel = pv.movie_file_name;
        if (rel.rfind("rom/", 0) == 0) rel = rel.substr(4);
        // The actual file is .usm not .mp4
        fs::path rel_path = rel;
        rel_path.replace_extension(".usm");
        fs::path src_usm = rom / rel_path;
        if (!fs::exists(src_usm)) {
            // Try with .usm directly
            src_usm = rom / rel;
            if (!fs::exists(src_usm)) src_usm = "";
        }
        if (!src_usm.empty() && fs::exists(src_usm)) {
            printf("  USM: %s\n", src_usm.filename().string().c_str());
            // Step 1: demux USM -> m2v + adx using UsmToolkit
            std::string tmp_dir = (song_out / "_tmp_usm").string();
            fs::create_directories(tmp_dir);
            std::string usm_str = src_usm.string();
            // UsmToolkit extract: extracts to same dir as input, so copy usm first
            fs::path usm_copy = fs::path(tmp_dir) / src_usm.filename();
            fs::copy_file(src_usm, usm_copy, fs::copy_options::overwrite_existing);
            run(opts.usmtoolkit + " extract \"" + usm_copy.string() + "\"");
            // Find resulting m2v
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
                ffcmd += " -c:v h264_nvenc -preset p4 -cq 20 -pix_fmt yuv420p";
                if (!adx_path.empty()) ffcmd += " -c:a aac -b:a 192k";
                ffcmd += " \"" + out_mp4 + "\"";
                run(ffcmd);
            }
            else {
                fprintf(stderr, "  WARN: USM demux produced no m2v in %s\n", tmp_dir.c_str());
            }
            // Cleanup temp
            fs::remove_all(tmp_dir);
        }
        else {
            fprintf(stderr, "  WARN: USM not found (tried %s)\n", rel.c_str());
        }
    }

    // --- Copy DSC files ---
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

    // --- Extract sprites from spr_sel_pv###.farc ---
    char pv_id_padded[8];
    snprintf(pv_id_padded, sizeof(pv_id_padded), "%d", pv.id);
    std::string farc_name = std::string("spr_sel_pv") + pv_id_padded + ".farc";
    fs::path farc_path = mod_root / "rom" / "2d" / farc_name;
    if (!fs::exists(farc_path)) {
        // Try with 4-digit zero-padded
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
            // Find the .bin inside
            std::string bin_name;
            for (auto& e : farc.entries) {
                if (ends_with_ci(e.name, ".bin")) { bin_name = e.name; break; }
            }
            if (!bin_name.empty()) {
                auto bin_data = farc.extract(bin_name);
                if (!bin_data.empty()) {
                    SpriteSetBin spr;
                    if (parse_bin(bin_data, spr)) {
                        // Decode all textures
                        std::vector<std::vector<uint8_t>> decoded_textures;
                        for (auto& t : spr.textures)
                            decoded_textures.push_back(decode_texture(t));

                        // Find and save named sprites
                        char pvs[16]; snprintf(pvs, sizeof(pvs), "%d", pv.id);
                        std::string jk_target = "SONG_JK" + std::string(pvs);
                        std::string bg_target = "SONG_BG" + std::string(pvs);
                        std::string logo_target = "SONG_LOGO" + std::string(pvs);

                        for (auto& spi : spr.sprites) {
                            std::string upper_name = spi.name;
                            for (auto& c : upper_name) c = (char)toupper(c);

                            std::string* dest_var = nullptr;
                            std::string dest_file;
                            if (upper_name == jk_target) { dest_var = &jk_png_name;   dest_file = "jk.png"; }
                            else if (upper_name == bg_target) { dest_var = &bg_png_name;   dest_file = "bg.png"; }
                            else if (upper_name == logo_target) { dest_var = &logo_png_name; dest_file = "logo.png"; }
                            else continue;

                            uint32_t ti = spi.texture_index;
                            if (ti >= decoded_textures.size() || decoded_textures[ti].empty()) continue;
                            int tw = spr.textures[ti].width, th = spr.textures[ti].height;
                            fs::path out_img = song_out / dest_file;
                            bool ok = (dest_file == "jk.png")
                                ? save_sprite_png_trimmed(out_img.string(), decoded_textures[ti],
                                      tw, th, (int)spi.x, (int)spi.y, (int)spi.w, (int)spi.h)
                                : save_sprite_png(out_img.string(), decoded_textures[ti],
                                      tw, th, (int)spi.x, (int)spi.y, (int)spi.w, (int)spi.h);
                            if (ok) {
                                *dest_var = dest_file;
                                printf("  IMG: %s (%dx%d from tex%d)\n",
                                    dest_file.c_str(), (int)spi.w, (int)spi.h, ti);
                            }
                            else {
                                fprintf(stderr, "  WARN: Failed to save %s\n", dest_file.c_str());
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

    // --- Generate song.ini ---
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
    ini << "songinfo.previewplaytime="  << fmt_time(pv.sabi_play)  << "\n";
    ini << "songinfo.previewstarttime=" << fmt_time(pv.sabi_start) << "\n";
    ini << "songinfo.slide=1\n";

    std::ofstream ini_f(song_out / "song.ini");
    ini_f << ini.str();
    printf("  INI: song.ini written\n");
}

// ---- Main -------------------------------------------------------------------

static void print_usage(const char* argv0) {
    printf("Usage: %s <mod_folder> <output_folder> [options]\n", argv0);
    printf("Options:\n");
    printf("  --usmtoolkit <path>   Path to UsmToolkit executable (default: UsmToolkit)\n");
    printf("  --skip-video          Skip USM->MP4 conversion\n");
    printf("  --pv <id>             Only convert specific PV ID (repeatable)\n");
    printf("  --verbose             Verbose output\n");
}

int main(int argc, char* argv[]) {
    if (argc < 3) { print_usage(argv[0]); return 1; }

    std::string mod_path = argv[1];
    std::string out_path = argv[2];
    ConvOptions opts;
    std::vector<int> filter_ids;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--skip-video") == 0) opts.skip_video = true;
        else if (strcmp(argv[i], "--verbose") == 0) opts.verbose = true;
        else if (strcmp(argv[i], "--usmtoolkit") == 0 && i + 1 < argc) opts.usmtoolkit = argv[++i];
        else if (strcmp(argv[i], "--pv") == 0 && i + 1 < argc) filter_ids.push_back(std::stoi(argv[++i]));
    }

    fs::path mod_root = mod_path;
    fs::path out_root = out_path;

    if (!fs::exists(mod_root)) { fprintf(stderr, "Mod folder not found: %s\n", mod_path.c_str()); return 1; }

    // Find mod_pv_db.txt (could be in mod_root or mod_root/rom)
    fs::path pv_db_path;
    for (auto& candidate : {
        mod_root / "rom" / "mod_pv_db.txt",
        mod_root / "mod_pv_db.txt",
        mod_root / "rom" / "pv_db.txt"
        }) {
        if (fs::exists(candidate)) { pv_db_path = candidate; break; }
    }
    if (pv_db_path.empty()) {
        fprintf(stderr, "mod_pv_db.txt not found in %s\n", mod_path.c_str());
        return 1;
    }
    printf("Reading: %s\n", pv_db_path.string().c_str());

    auto pvs = parse_pv_db(pv_db_path.string());
    if (pvs.empty()) { fprintf(stderr, "No PV entries found.\n"); return 1; }
    printf("Found %zu PV entries.\n", pvs.size());

    fs::create_directories(out_root);

    for (auto& [id, pv] : pvs) {
        if (!filter_ids.empty() && std::find(filter_ids.begin(), filter_ids.end(), id) == filter_ids.end())
            continue;
        convert_song(mod_root, pv, out_root, opts);
    }

    printf("\nDone.\n");
    return 0;
}
