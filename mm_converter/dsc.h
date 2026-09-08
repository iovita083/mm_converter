#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct LyricCue
{
    double time_seconds = 0.0;
    int lyric_index = 0;
};

struct WrappedLyricPart
{
    std::string text;
    double time_offset = 0.0;
};

std::vector<LyricCue> extract_lyric_cues(const fs::path& dsc_path);
std::vector<WrappedLyricPart> wrap_lyric_line(const std::string& text, size_t max_chars, double duration);
std::string fmt_lrc_time(double t);
