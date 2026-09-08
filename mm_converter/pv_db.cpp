#include "pv_db.h"
#include "converter.h"

#include <fstream>
#include <string>
#include <string_view>
#include <charconv>
#include <map>

namespace
{
    std::string_view trim_view(std::string_view s) {
        const size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) return {};
        const size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    int parse_int(std::string_view sv, int default_value = 0) {
        sv = trim_view(sv);
        int value = default_value;
        std::from_chars(sv.data(), sv.data() + sv.size(), value);
        return value;
    }

    float parse_float(std::string_view sv, float default_value = 0.0f) {
        sv = trim_view(sv);
        float value = default_value;
        std::from_chars(sv.data(), sv.data() + sv.size(), value);
        return value;
    }

    std::string sanitize_token(std::string_view sv) {
        size_t sp = sv.find_first_of(" \t");
        return std::string(sp == std::string_view::npos ? sv : sv.substr(0, sp));
    }
}

// ---- mod_pv_db.txt Parser ---------------------------------------------------

std::string pv_level_to_float(const std::string& lvl) {
    if (lvl.size() < 10) return "";
    std::string_view ww = std::string_view(lvl).substr(6, 2);
    std::string_view d = std::string_view(lvl).substr(9, 1);
    int whole = parse_int(ww);
    return std::to_string(whole) + "." + std::string(d);
}

// Some pv_db lyric lines contain a literal "\n" as a manual line-break hack for the real game's lyric display. 
// Our (and apparently the real game's) lyric row only has room for one line, so this just causes the text to 
// overflow past the right edge. Collapse it into a single line instead of splitting it.
static std::string sanitize_lyric_line(std::string_view sv) {
    std::string val(sv);
    size_t pos = 0;
    while ((pos = val.find("\\n", pos)) != std::string::npos) {
        val.replace(pos, 2, " ");
        pos += 1;
    }
    return val;
}

std::map<int, PvEntry> parse_pv_db(const std::string& path) {
    std::ifstream f(path);
    std::map<int, PvEntry> result;
    if (!f) return result;

    std::string line;
    while (std::getline(f, line)) {
        std::string_view line_v = trim_view(line);
        if (line_v.empty() || line_v[0] == '#') continue;

        auto eq = line_v.find('=');
        if (eq == std::string_view::npos) continue;

        std::string_view key = trim_view(line_v.substr(0, eq));
        std::string_view val = trim_view(line_v.substr(eq + 1));

        if (key.substr(0, 3) != "pv_") continue;
        size_t dot = key.find('.', 3);
        if (dot == std::string_view::npos) continue;

        int id = parse_int(key.substr(3, dot - 3), -1);
        if (id < 0) continue;

        PvEntry& e = result[id];
        e.id = id;
        std::string_view rest = key.substr(dot + 1);

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
        else if (rest == "sabi.start_time")     e.sabi_start = parse_float(val, e.sabi_start);
        else if (rest == "sabi.play_time")      e.sabi_play = parse_float(val, e.sabi_play);
        else if (rest == "songinfo.lyrics")      e.lyrics = val;
        else if (rest == "songinfo.music")       e.music = val;
        else if (rest == "songinfo.arranger")    e.arranger = val;
        else if (rest == "songinfo.illustrator") e.illustrator = val;
        else if (rest.rfind("lyric.", 0) == 0) {
            int lyric_idx = parse_int(rest.substr(6), -1);
            if (lyric_idx >= 0) {
                e.lyric_lines[lyric_idx] = sanitize_lyric_line(val);
            }
        }
        else if (rest.rfind("lyric_en.", 0) == 0) {
            int lyric_idx = parse_int(rest.substr(9), -1);
            if (lyric_idx >= 0) {
                e.lyric_en_lines[lyric_idx] = sanitize_lyric_line(val);
            }
        }
        else if (rest.rfind("another_song.", 0) == 0) {
            std::string_view as_rest = rest.substr(13);
            size_t as_dot = as_rest.find('.');
            if (as_dot != std::string_view::npos) {
                int idx = parse_int(as_rest.substr(0, as_dot), -1);
                if (idx >= 0) {
                    std::string_view field = as_rest.substr(as_dot + 1);
                    PvAnotherSong& as = e.another_songs[idx];
                    if (field == "name")                    as.name = val;
                    else if (field == "name_en")             as.name_en = val;
                    else if (field == "vocal_disp_name")     as.vocal_disp_name = val;
                    else if (field == "vocal_disp_name_en")  as.vocal_disp_name_en = val;
                    else if (field == "song_file_name")      as.song_file_name = val;
                }
            }
        }
        else if (rest.rfind("difficulty.", 0) == 0) {
            std::string_view d_rest = rest.substr(11);
            auto get_diff = [&](std::string_view t) -> PvDifficulty* {
                if (t == "easy")    return &e.easy;
                if (t == "normal")  return &e.normal;
                if (t == "hard")    return &e.hard;
                if (t == "extreme") return &e.extreme;
                return nullptr;
                };

            size_t d2 = d_rest.find('.');
            if (d2 == std::string_view::npos) continue;
            std::string_view dtype = d_rest.substr(0, d2);
            std::string_view d_rest2 = d_rest.substr(d2 + 1);

            if (d_rest2 == "length") {
                if (dtype == "easy")         e.easy.present = (val != "0");
                else if (dtype == "normal")  e.normal.present = (val != "0");
                else if (dtype == "hard")    e.hard.present = (val != "0");
                else if (dtype == "extreme") e.extreme.present = (val != "0");
            }
            else {
                size_t d3 = d_rest2.find('.');
                if (d3 == std::string_view::npos) continue;
                int idx = parse_int(d_rest2.substr(0, d3), -1);
                if (idx < 0) continue;

                std::string_view field = d_rest2.substr(d3 + 1);

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