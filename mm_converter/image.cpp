#include "image.h"
#include "converter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <array>
#include <cstdint>
#include "compression.h"

namespace
{
    uint32_t read_u32le(const uint8_t* p)
    {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    float read_f32le(const uint8_t* p)
    {
        uint32_t v = read_u32le(p);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
}

// ---- Sprite Set Binary (.bin) -----------------------------------------------

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

bool parse_bin(const std::vector<uint8_t>& bin, SpriteSetBin& out) {
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
    uint8_t r[4][3]{};
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
    uint8_t av[8]{}; av[0] = a0; av[1] = a1;
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

std::vector<uint8_t> decode_texture(const TextureInfo& tex) {
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
        GuiLog("Unsupported texture format %d\n", fmt);
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
    const size_t row_bytes = size_t(w) * 4;
    std::vector<uint8_t> raw;
    raw.reserve((row_bytes + 1) * size_t(h));

    // PNG filtering makes the DEFLATE stream much smaller for images with
    // smooth areas, gradients, and repeated colors. Pick the cheapest filter
    // independently for every row. All 5 candidate rows are computed in one
    // pass over the pixels (rather than one pass per filter) since they all
    // read the same left/up/up_left neighbors, and only the winning row is
    // ever copied into the output.
    std::array<std::vector<uint8_t>, 5> candidates;
    for (auto& c : candidates) c.resize(row_bytes);
    std::vector<uint8_t> previous(row_bytes, 0);

    for (int y = 0; y < h; ++y) {
        const uint8_t* current = rgba + size_t(y) * row_bytes;
        std::array<uint64_t, 5> score{};

        for (size_t x = 0; x < row_bytes; ++x) {
            const uint8_t cur = current[x];
            const uint8_t left = x >= 4 ? current[x - 4] : 0;
            const uint8_t up = previous[x];
            const uint8_t up_left = x >= 4 ? previous[x - 4] : 0;

            const uint8_t v0 = cur;
            const uint8_t v1 = uint8_t(cur - left);
            const uint8_t v2 = uint8_t(cur - up);
            const uint8_t v3 = uint8_t(cur - ((uint16_t(left) + uint16_t(up)) / 2));

            const int p = int(left) + int(up) - int(up_left);
            const int pa = std::abs(p - int(left));
            const int pb = std::abs(p - int(up));
            const int pc = std::abs(p - int(up_left));
            const uint8_t predictor = pa <= pb && pa <= pc ? left : (pb <= pc ? up : up_left);
            const uint8_t v4 = uint8_t(cur - predictor);

            candidates[0][x] = v0;
            candidates[1][x] = v1;
            candidates[2][x] = v2;
            candidates[3][x] = v3;
            candidates[4][x] = v4;

            // Sum the magnitude of the signed residual. Smaller residuals
            // generally produce a substantially better DEFLATE stream.
            score[0] += std::abs(int8_t(v0));
            score[1] += std::abs(int8_t(v1));
            score[2] += std::abs(int8_t(v2));
            score[3] += std::abs(int8_t(v3));
            score[4] += std::abs(int8_t(v4));
        }

        uint8_t best_filter = 0;
        for (uint8_t filter = 1; filter <= 4; ++filter)
            if (score[filter] < score[best_filter])
                best_filter = filter;

        raw.push_back(best_filter);
        raw.insert(raw.end(), candidates[best_filter].begin(), candidates[best_filter].end());
        previous.assign(current, current + row_bytes);
    }
    std::vector<uint8_t> compressed;
    if (!compress_data(raw.data(), raw.size(), compressed))
        return false;

    std::vector<uint8_t> out;
    const uint8_t sig[] = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A };
    out.insert(out.end(), sig, sig + 8);

    uint8_t ihdr[13]{};
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

bool save_sprite_png(const std::string& path,
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

bool save_sprite_png_trimmed(const std::string& path,
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

// Some mod logo sprites are effectively blank (real game requires all sprites so authors make logos
// blank if they can't find one). Treat anything below both a per-pixel alpha threshold and a minimum-coverage 
// threshold as blank, so we can skip writing a logo file that would just be invisible in-game.
bool is_effectively_blank(const std::vector<uint8_t>& rgba, int w, int h) {
    const uint8_t kAlphaThreshold = 32;      // ~12.5% opacity; below this, treat as noise
    const double kMinCoverageFrac = 0.001;   // at least 0.1% of pixels must be meaningfully opaque

    size_t meaningfulPixels = 0;
    size_t totalPixels = (size_t)w * (size_t)h;
    for (size_t i = 0; i < totalPixels; i++) {
        if (rgba[i * 4 + 3] >= kAlphaThreshold)
            meaningfulPixels++;
    }
    return totalPixels == 0 || ((double)meaningfulPixels / (double)totalPixels) < kMinCoverageFrac;
}