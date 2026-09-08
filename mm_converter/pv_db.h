#pragma once

#include <map>
#include <string>

struct PvDifficulty
{
    std::string level;
    std::string script_file;
    bool present = false;
};

struct PvAnotherSong
{
    std::string name;
    std::string name_en;
    std::string vocal_disp_name;
    std::string vocal_disp_name_en;
    std::string song_file_name;
};

struct PvEntry
{
    int id = -1;
    std::string song_name;
    std::string song_name_en;
    std::string song_name_reading;
    std::string bpm;
    std::string date;
    std::string lyrics;
    std::string music;
    std::string arranger;
    std::string illustrator;
    std::string song_file_name;
    std::string movie_file_name;
    float sabi_start = 0;
    float sabi_play = 30;
    PvDifficulty easy, normal, hard, extreme, exextreme;
    std::map<int, std::string> lyric_lines;
    std::map<int, std::string> lyric_en_lines;
    std::map<int, PvAnotherSong> another_songs;
};

std::string pv_level_to_float(const std::string& level);
std::map<int, PvEntry> parse_pv_db(const std::string& path);
