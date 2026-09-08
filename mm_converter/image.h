#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SpriteInfo
{
    std::string name;
    uint32_t texture_index = 0;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct TextureInfo
{
    std::string name;
    int width = 0;
    int height = 0;
    int format = 0;
    std::vector<uint8_t> data;
    int chroma_width = 0;
    int chroma_height = 0;
    std::vector<uint8_t> chroma_data;
    bool is_ycbcr = false;
};

struct SpriteSetBin
{
    std::vector<SpriteInfo> sprites;
    std::vector<TextureInfo> textures;
};

bool parse_bin(const std::vector<uint8_t>& bin, SpriteSetBin& out);
std::vector<uint8_t> decode_texture(const TextureInfo& t);
bool save_sprite_png(const std::string& path, const std::vector<uint8_t>& tex, int tw, int th,
    int sx, int sy, int sw, int sh);
bool save_sprite_png_trimmed(const std::string& path, const std::vector<uint8_t>& tex, int tw, int th,
    int sx, int sy, int sw, int sh);
bool is_effectively_blank(const std::vector<uint8_t>& rgba, int w, int h);
