#pragma once

#include "dsc.h"
#include "farc.h"
#include "image.h"
#include "pv_db.h"

#include <filesystem>

namespace fs = std::filesystem;

struct ConvOptions
{
    bool skip_video = false;
    bool verbose = false;
};

void GuiLog(const char* fmt, ...);
void convert_song(const fs::path& mod_root, const PvEntry& pv,
    const fs::path& out_root, const ConvOptions& opts);
