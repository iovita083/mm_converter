#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct FarcEntry
{
    std::string name;
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t uncompressed = 0;
    bool compressed = false;
};

struct FarcArchive
{
    std::vector<uint8_t> file_data;
    std::vector<FarcEntry> entries;

    bool load(const std::string& path);
    std::vector<uint8_t> extract(const std::string& name) const;
    const FarcEntry* find(const std::string& name) const;
};
