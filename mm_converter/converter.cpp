#include "converter.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    const size_t kLyricLineMaxChars = 80;

    bool ends_with_ci(const std::string& s, const std::string& suffix)
    {
        if (suffix.size() > s.size()) return false;
        return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(),
            [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); });
    }

    std::string fmt_time(float v)
    {
        int trunc = (int)(v * 100.0f);
        std::ostringstream os;
        os << (trunc / 100) << "." << std::setw(2) << std::setfill('0') << (trunc % 100);
        return os.str();
    }

    std::string sanitize_folder_name(const std::string& name)
    {
        std::string result;
        for (size_t i = 0; i < name.length();) {
            unsigned char c = (unsigned char)name[i];
            if (c >= 0x80) {
                result += '_';
                i++;
                while (i < name.length() && ((unsigned char)name[i] & 0xC0) == 0x80)
                    i++;
            }
            else if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') {
                result += '_';
                i++;
            }
            else {
                result += c;
                i++;
            }
        }
        return result;
    }

    struct ResolvedVocal
    {
        std::string file_name;
        std::string display_name;
    };

    struct DifficultyResults
    {
        bool easy = false;
        bool normal = false;
        bool hard = false;
        bool extreme = false;
        bool exextreme = false;
    };

    std::string strip_rom_prefix(std::string path)
    {
        if (path.rfind("rom/", 0) == 0)
            path = path.substr(4);
        return path;
    }

    bool copy_ogg(const fs::path& rom, const fs::path& song_out,
        const std::string& src_rel, const std::string& dst_name)
    {
        fs::path src_ogg = rom / strip_rom_prefix(src_rel);
        if (!fs::exists(src_ogg)) {
            GuiLog("  WARN: OGG not found: %s\n", src_ogg.string().c_str());
            return false;
        }

        fs::copy_file(src_ogg, song_out / dst_name, fs::copy_options::overwrite_existing);
        GuiLog("  OGG: %s -> %s\n", src_ogg.filename().string().c_str(), dst_name.c_str());
        return true;
    }

    std::map<int, ResolvedVocal> resolve_vocals(
        const PvEntry& pv, const fs::path& rom, const fs::path& song_out)
    {
        std::map<int, ResolvedVocal> vocals;
        std::string display_en = !pv.song_name_en.empty() ? pv.song_name_en : pv.song_name;

        bool have_base_ogg = false;
        if (!pv.song_file_name.empty())
            have_base_ogg = copy_ogg(rom, song_out, pv.song_file_name, "song.ogg");

        for (const auto& [idx, as] : pv.another_songs) {
            std::string display = !as.vocal_disp_name_en.empty() ? as.vocal_disp_name_en
                : !as.name_en.empty() ? as.name_en
                : !as.vocal_disp_name.empty() ? as.vocal_disp_name
                : as.name;

            if (idx == 0) {
                if (display.empty())
                    display = display_en;

                if (!have_base_ogg && !as.song_file_name.empty())
                    have_base_ogg = copy_ogg(rom, song_out, as.song_file_name, "song.ogg");

                if (have_base_ogg)
                    vocals[0] = { "song.ogg", display };
                continue;
            }

            if (display.empty())
                display = "Vocal " + std::to_string(idx);

            if (!as.song_file_name.empty()) {
                std::string dst = "song_vocal" + std::to_string(idx) + ".ogg";
                if (copy_ogg(rom, song_out, as.song_file_name, dst))
                    vocals[idx] = { dst, display };
            }
            else if (have_base_ogg) {
                vocals[idx] = { "song.ogg", display };
            }
        }

        if (have_base_ogg && vocals.find(0) == vocals.end())
            vocals[0] = { "song.ogg", display_en };

        return vocals;
    }

    void copy_video(const PvEntry& pv, const fs::path& rom, const fs::path& song_out, bool skip_video)
    {
        if (pv.movie_file_name.empty() || skip_video)
            return;

        std::string rel = strip_rom_prefix(pv.movie_file_name);
        fs::path rel_path = rel;
        rel_path.replace_extension(".usm");
        fs::path src_usm = rom / rel_path;

        if (!fs::exists(src_usm)) {
            src_usm = rom / rel;
            if (!fs::exists(src_usm))
                src_usm = "";
        }

        if (!src_usm.empty() && fs::exists(src_usm)) {
            GuiLog("  USM: %s\n", src_usm.filename().string().c_str());
            std::error_code ec;
            fs::copy_file(src_usm, song_out / "song.usm",
                fs::copy_options::overwrite_existing, ec);
            if (ec)
                GuiLog("  WARN: Failed to copy USM: %s\n", ec.message().c_str());
        }
        else {
            GuiLog("  WARN: USM not found (tried %s)\n", rel.c_str());
        }
    }

    bool copy_difficulty(const PvDifficulty& diff, const std::string& dest_name,
        const fs::path& rom, const fs::path& song_out)
    {
        if (!diff.present || diff.script_file.empty())
            return false;

        fs::path src = rom / strip_rom_prefix(diff.script_file);
        if (fs::exists(src)) {
            fs::copy_file(src, song_out / dest_name, fs::copy_options::overwrite_existing);
            GuiLog("  DSC: %s -> %s\n", src.filename().string().c_str(), dest_name.c_str());
            return true;
        }

        GuiLog("  WARN: DSC not found, skipping difficulty: %s\n", src.string().c_str());
        return false;
    }

    DifficultyResults copy_difficulties(const PvEntry& pv, const fs::path& rom, const fs::path& song_out)
    {
        DifficultyResults result;
        result.easy = copy_difficulty(pv.easy, "song_easy.dsc", rom, song_out);
        result.normal = copy_difficulty(pv.normal, "song_normal.dsc", rom, song_out);
        result.hard = copy_difficulty(pv.hard, "song_hard.dsc", rom, song_out);
        result.extreme = copy_difficulty(pv.extreme, "song_extreme.dsc", rom, song_out);
        result.exextreme = copy_difficulty(pv.exextreme, "song_exextreme.dsc", rom, song_out);
        return result;
    }

    bool write_lyrics(const PvEntry& pv, const fs::path& song_out, const DifficultyResults& diffs)
    {
        if (pv.lyric_lines.empty() && pv.lyric_en_lines.empty())
            return false;

        fs::path lyric_dsc;
        if (diffs.exextreme) lyric_dsc = song_out / "song_exextreme.dsc";
        else if (diffs.extreme) lyric_dsc = song_out / "song_extreme.dsc";
        else if (diffs.hard) lyric_dsc = song_out / "song_hard.dsc";
        else if (diffs.normal) lyric_dsc = song_out / "song_normal.dsc";
        else if (diffs.easy) lyric_dsc = song_out / "song_easy.dsc";

        if (lyric_dsc.empty()) {
            GuiLog("  WARN: lyric text present but no DSC available for timing, skipping lyrics.lrc\n");
            return false;
        }

        auto cues = extract_lyric_cues(lyric_dsc);
        auto line_text = [&](int idx) -> const std::string* {
            auto en = pv.lyric_en_lines.find(idx);
            if (en != pv.lyric_en_lines.end() && !en->second.empty()) return &en->second;

            auto native = pv.lyric_lines.find(idx);
            if (native != pv.lyric_lines.end() && !native->second.empty()) return &native->second;
            return nullptr;
            };

        std::ostringstream lrc;
        size_t line_count = 0;

        for (size_t i = 0; i < cues.size(); i++) {
            const auto& cue = cues[i];
            if (cue.lyric_index == 0)
                continue;

            const std::string* text = line_text(cue.lyric_index);
            if (!text)
                continue;

            double end_time = (i + 1 < cues.size())
                ? cues[i + 1].time_seconds
                : cue.time_seconds + 3.0;
            double duration = std::max(0.1, end_time - cue.time_seconds);

            for (auto& part : wrap_lyric_line(*text, kLyricLineMaxChars, duration)) {
                lrc << fmt_lrc_time(cue.time_seconds + part.time_offset) << part.text << "\n";
                line_count++;
            }
        }

        std::string lrc_str = lrc.str();
        if (lrc_str.empty())
            return false;

        std::ofstream lrc_f(song_out / "lyrics.lrc", std::ios::binary);
        lrc_f << lrc_str;
        GuiLog("  LRC: lyrics.lrc written (%zu lines)\n", line_count);
        return true;
    }

    std::string find_sprite_farc(const fs::path& mod_root, int pv_id)
    {
        char pv_id_str[16];
        snprintf(pv_id_str, sizeof(pv_id_str), "%d", pv_id);

        std::string farc_name = std::string("spr_sel_pv") + pv_id_str + ".farc";
        fs::path farc_path = mod_root / "rom" / "2d" / farc_name;
        if (fs::exists(farc_path))
            return farc_path.string();

        char padded4[8];
        snprintf(padded4, sizeof(padded4), "%04d", pv_id);
        farc_name = std::string("spr_sel_pv") + padded4 + ".farc";
        farc_path = mod_root / "rom" / "2d" / farc_name;
        return farc_path.string();
    }

    bool process_sprite(const SpriteInfo& spi, const std::string& target_file,
        bool trim_image, const SpriteSetBin& spr,
        std::vector<std::vector<uint8_t>>& decoded_textures,
        const fs::path& song_out)
    {
        uint32_t ti = spi.texture_index;
        if (ti >= decoded_textures.size())
            return false;

        // Lazy load texture on demand
        if (decoded_textures[ti].empty()) {
            decoded_textures[ti] = decode_texture(spr.textures[ti]);
        }

        if (decoded_textures[ti].empty())
            return false;

        int tw = spr.textures[ti].width;
        int th = spr.textures[ti].height;

        if (target_file == "logo.png") {
            int sx = std::max(0, std::min((int)spi.x, tw - 1));
            int sy = std::max(0, std::min((int)spi.y, th - 1));
            int sw = std::max(1, std::min((int)spi.w, tw - sx));
            int sh = std::max(1, std::min((int)spi.h, th - sy));
            std::vector<uint8_t> crop(sw * sh * 4);

            for (int y = 0; y < sh; y++) {
                memcpy(crop.data() + y * sw * 4,
                    decoded_textures[ti].data() + (sy + y) * tw * 4 + sx * 4,
                    sw * 4);
            }

            if (is_effectively_blank(crop, sw, sh)) {
                GuiLog("  IMG: logo.png skipped (sprite is effectively blank/transparent)\n");
                return false;
            }
        }

        fs::path out_img = song_out / target_file;
        bool ok = trim_image
            ? save_sprite_png_trimmed(out_img.string(), decoded_textures[ti], tw, th,
                (int)spi.x, (int)spi.y, (int)spi.w, (int)spi.h)
            : save_sprite_png(out_img.string(), decoded_textures[ti], tw, th,
                (int)spi.x, (int)spi.y, (int)spi.w, (int)spi.h);

        if (ok)
            GuiLog("  IMG: %s (%dx%d from tex%d)\n",
                target_file.c_str(), (int)spi.w, (int)spi.h, ti);

        return ok;
    }

    void extract_images(const fs::path& mod_root, const fs::path& song_out, int pv_id,
        std::string& jk_png_name, std::string& bg_png_name, std::string& logo_png_name)
    {
        fs::path farc_path = find_sprite_farc(mod_root, pv_id);
        if (!fs::exists(farc_path)) {
            GuiLog("  INFO: No FARC found for pv_%d (sprites skipped)\n", pv_id);
            return;
        }

        GuiLog("  FARC: %s\n", farc_path.filename().string().c_str());

        FarcArchive farc;
        if (!farc.load(farc_path.string()))
            return;

        std::string bin_name;
        for (const auto& entry : farc.entries) {
            if (ends_with_ci(entry.name, ".bin")) {
                bin_name = entry.name;
                break;
            }
        }
        if (bin_name.empty())
            return;

        auto bin_data = farc.extract(bin_name);
        if (bin_data.empty())
            return;

        SpriteSetBin spr;
        if (!parse_bin(bin_data, spr)) {
            GuiLog("  WARN: Failed to parse bin: %s\n", bin_name.c_str());
            return;
        }

        // Pre-allocate empty containers (lazy loading initialized)
        std::vector<std::vector<uint8_t>> decoded_textures(spr.textures.size());

        std::string pv_id_str = std::to_string(pv_id);
        std::string jk_target = "SONG_JK" + pv_id_str;
        std::string bg_target = "SONG_BG" + pv_id_str;
        std::string logo_target = "SONG_LOGO" + pv_id_str;
        bool skipped_bg_due_to_image = false;

        // First, check for "IMAGE" override (retains early exit condition)
        for (const auto& spi : spr.sprites) {
            std::string upper_name = spi.name;
            for (auto& c : upper_name)
                c = (char)toupper((unsigned char)c);

            if (upper_name == "IMAGE") {
                uint32_t ti = spi.texture_index;
                if (ti >= decoded_textures.size())
                    continue;

                if (decoded_textures[ti].empty()) {
                    decoded_textures[ti] = decode_texture(spr.textures[ti]);
                }

                if (decoded_textures[ti].empty())
                    continue;

                int tw = spr.textures[ti].width;
                int th = spr.textures[ti].height;
                fs::path out_img = song_out / "bg.png";
                if (save_sprite_png(out_img.string(), decoded_textures[ti], tw, th,
                    (int)spi.x, (int)spi.y, (int)spi.w, (int)spi.h)) {
                    bg_png_name = "bg.png";
                    skipped_bg_due_to_image = true;
                    GuiLog("  IMG: bg.png (Possible High-res override from other sprite, %dx%d from tex%d)\n",
                        (int)spi.w, (int)spi.h, ti);
                    break;
                }
            }
        }

        // Consolidated single loop replacing separate target and fallback passes
        for (const auto& spi : spr.sprites) {
            std::string upper_name = spi.name;
            for (auto& c : upper_name)
                c = (char)toupper((unsigned char)c);

            // Check primary targets
            if (jk_png_name.empty() && upper_name == jk_target) {
                if (process_sprite(spi, "jk.png", true, spr, decoded_textures, song_out))
                    jk_png_name = "jk.png";
            }
            else if (!skipped_bg_due_to_image && bg_png_name.empty() && upper_name == bg_target) {
                if (process_sprite(spi, "bg.png", false, spr, decoded_textures, song_out))
                    bg_png_name = "bg.png";
            }
            else if (logo_png_name.empty() && upper_name == logo_target) {
                if (process_sprite(spi, "logo.png", false, spr, decoded_textures, song_out))
                    logo_png_name = "logo.png";
            }

            // Check general fallback targets if still empty
            if (jk_png_name.empty() && upper_name == "SONG_JK001") {
                if (process_sprite(spi, "jk.png", true, spr, decoded_textures, song_out))
                    jk_png_name = "jk.png";
            }
            else if (!skipped_bg_due_to_image && bg_png_name.empty() && upper_name == "SONG_BG001") {
                if (process_sprite(spi, "bg.png", false, spr, decoded_textures, song_out))
                    bg_png_name = "bg.png";
            }
            else if (logo_png_name.empty() && upper_name == "SONG_LOGO001") {
                if (process_sprite(spi, "logo.png", false, spr, decoded_textures, song_out))
                    logo_png_name = "logo.png";
            }
        }
    }

    void write_song_ini(const fs::path& song_out, const PvEntry& pv,
        const DifficultyResults& diffs, const std::map<int, ResolvedVocal>& vocals,
        const std::string& bg_png_name, const std::string& jk_png_name,
        const std::string& logo_png_name, bool has_lyrics)
    {
        auto diff_level = [](const PvDifficulty& d, bool ok) -> std::string {
            if (!ok || d.level.empty()) return "";
            return pv_level_to_float(d.level);
            };

        std::ostringstream ini;
        ini << "difficulty.easy.level=" << diff_level(pv.easy, diffs.easy) << "\n";
        ini << "difficulty.easy.script_file_name=" << (diffs.easy ? "song_easy.dsc" : "") << "\n\n";
        ini << "difficulty.normal.level=" << diff_level(pv.normal, diffs.normal) << "\n";
        ini << "difficulty.normal.script_file_name=" << (diffs.normal ? "song_normal.dsc" : "") << "\n\n";
        ini << "difficulty.hard.level=" << diff_level(pv.hard, diffs.hard) << "\n";
        ini << "difficulty.hard.script_file_name=" << (diffs.hard ? "song_hard.dsc" : "") << "\n\n";
        ini << "difficulty.extreme.level=" << diff_level(pv.extreme, diffs.extreme) << "\n";
        ini << "difficulty.extreme.script_file_name=" << (diffs.extreme ? "song_extreme.dsc" : "") << "\n\n";
        ini << "difficulty.exextreme.level=" << diff_level(pv.exextreme, diffs.exextreme) << "\n";
        ini << "difficulty.exextreme.script_file_name=" << (diffs.exextreme ? "song_exextreme.dsc" : "") << "\n\n";

        bool has_mp4 = fs::exists(song_out / "song.mp4");
        bool has_usm = fs::exists(song_out / "song.usm");
        ini << "movie_file_name=" << (has_mp4 ? "song.mp4" : has_usm ? "song.usm" : "") << "\n";

        for (const auto& [idx, vocal] : vocals) {
            ini << "vocal." << idx << ".file_name=" << vocal.file_name << "\n";
            ini << "vocal." << idx << ".name=" << vocal.display_name << "\n";
        }

        ini << "background_file_name=" << bg_png_name << "\n";
        ini << "jacket_file_name=" << jk_png_name << "\n";
        ini << "logo_file_name=" << logo_png_name << "\n";
        ini << "lyrics_file=" << (has_lyrics ? "lyrics.lrc" : "") << "\n\n";

        std::string display_en = !pv.song_name_en.empty() ? pv.song_name_en : pv.song_name;
        ini << "songinfo.name=" << display_en << "\n";
        if (!pv.arranger.empty()) ini << "songinfo.arranger=" << pv.arranger << "\n";
        if (!pv.illustrator.empty()) ini << "songinfo.illustrator=" << pv.illustrator << "\n";
        if (!pv.lyrics.empty()) ini << "songinfo.lyrics=" << pv.lyrics << "\n";
        if (!pv.music.empty()) ini << "songinfo.music=" << pv.music << "\n";
        ini << "songinfo.bpm=" << pv.bpm << "\n";
        ini << "songinfo.date=" << pv.date << "\n";
        ini << "songinfo.previewplaytime=" << fmt_time(pv.sabi_play) << "\n";
        ini << "songinfo.previewstarttime=" << fmt_time(pv.sabi_start) << "\n";
        ini << "songinfo.slide=1\n";

        std::ofstream ini_f(song_out / "song.ini");
        ini_f << ini.str();
        GuiLog("  INI: song.ini written\n");
    }
}

void convert_song(const fs::path& mod_root, const PvEntry& pv,
    const fs::path& out_root, const ConvOptions& opts)
{
    char pv_id_str[16];
    snprintf(pv_id_str, sizeof(pv_id_str), "%d", pv.id);

    std::string display_name = !pv.song_name_en.empty() ? pv.song_name_en : pv.song_name;
    if (display_name.empty())
        display_name = std::string("pv_") + pv_id_str;

    std::string song_folder_name = "pv_" + std::string(pv_id_str) + "_" + sanitize_folder_name(display_name);
    std::string modpack_folder_name = sanitize_folder_name(mod_root.filename().string());
    if (modpack_folder_name.empty())
        modpack_folder_name = "mod";

    fs::path song_out = out_root / modpack_folder_name / song_folder_name;
    fs::create_directories(song_out);

    GuiLog("\n[pv_%s] %s -> %s\n", pv_id_str, display_name.c_str(), song_out.string().c_str());

    fs::path rom = mod_root / "rom";
    auto vocals = resolve_vocals(pv, rom, song_out);
    copy_video(pv, rom, song_out, opts.skip_video);
    auto diffs = copy_difficulties(pv, rom, song_out);

    bool has_lyrics = write_lyrics(pv, song_out, diffs);

    std::string jk_png_name;
    std::string bg_png_name;
    std::string logo_png_name;
    extract_images(mod_root, song_out, pv.id, jk_png_name, bg_png_name, logo_png_name);

    write_song_ini(song_out, pv, diffs, vocals,
        bg_png_name, jk_png_name, logo_png_name, has_lyrics);
}