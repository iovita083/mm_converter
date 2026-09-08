#include "farc.h"
#include "converter.h"
#include "compression.h"

#include <cstring>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace
{
uint32_t read_u32be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

std::string to_lower(std::string s)
{
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
}

bool FarcArchive::load(const std::string& path) {
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
            GuiLog( "Not a FARC: %s (sig=%s)\n", path.c_str(), sig);
            return false;
        }

        const uint8_t* p = file_data.data();
        uint32_t header_size = read_u32be(p + 4) + 8;

        if (is_farc_dt) {
            uint32_t flags = read_u32be(p + 8);
            bool is_compressed = (flags & 2) != 0;
            bool is_encrypted = (flags & 4) != 0;
            if (is_encrypted) {
                GuiLog( "Encrypted FARC not supported: %s\n", path.c_str());
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

std::vector<uint8_t> FarcArchive::extract(const std::string& name) const {
        for (auto& e : entries) {
            if (to_lower(e.name) == to_lower(name)) {
                const uint8_t* src = file_data.data() + e.offset;
                if (!e.compressed) {
                    return std::vector<uint8_t>(src, src + e.size);
                }
                std::vector<uint8_t> out;
                if (!decompress_data(src, e.size, out, e.uncompressed)) {
                    GuiLog("Failed to decompress %s\n", name.c_str());
                    return {};
                }
                return out;
            }
        }
        return {};
    }

const FarcEntry* FarcArchive::find(const std::string& name) const {
        for (auto& e : entries)
            if (to_lower(e.name) == to_lower(name)) return &e;
        return nullptr;
    }
