#include "dsc.h"
#include "converter.h"

#include <cstdint>
#include <fstream>
#include <cstdio>

namespace
{
uint32_t read_u32le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
}

// ---- DSC lyric cue extraction ------------------------------------------------
// Reads TIME/LYRIC ops directly from a .dsc chart to recover the timing for
// each lyric line index, so we can emit a proper timed .lrc.
// DSC op stream (FT format, after the 4-byte signature): repeating
// [op_id: i32le][params: i32le * param_cnt], op_id 0 = END.
// TIME (op 1, 1 param): DivaTime, an i32 scaled by 100000 units/second.
// LYRIC (op 24, 2 params): [lyric line index, color] (-1 color = default; index 0 = clear line)

static const int kDscOpParamCount[107] = {
    /*0*/0,/*1*/1,/*2*/4,/*3*/2,/*4*/2,/*5*/2,/*6*/7,/*7*/4,/*8*/2,/*9*/6,
    /*10*/2,/*11*/1,/*12*/6,/*13*/2,/*14*/1,/*15*/1,/*16*/3,/*17*/2,/*18*/3,/*19*/5,
    /*20*/5,/*21*/4,/*22*/4,/*23*/5,/*24*/2,/*25*/0,/*26*/2,/*27*/4,/*28*/2,/*29*/2,
    /*30*/1,/*31*/21,/*32*/0,/*33*/3,/*34*/2,/*35*/5,/*36*/1,/*37*/1,/*38*/7,/*39*/1,
    /*40*/1,/*41*/2,/*42*/1,/*43*/2,/*44*/1,/*45*/2,/*46*/3,/*47*/3,/*48*/1,/*49*/2,
    /*50*/2,/*51*/3,/*52*/6,/*53*/6,/*54*/1,/*55*/1,/*56*/2,/*57*/3,/*58*/1,/*59*/2,
    /*60*/2,/*61*/4,/*62*/4,/*63*/1,/*64*/2,/*65*/1,/*66*/2,/*67*/1,/*68*/1,/*69*/3,
    /*70*/3,/*71*/3,/*72*/2,/*73*/1,/*74*/9,/*75*/3,/*76*/2,/*77*/4,/*78*/2,/*79*/3,
    /*80*/2,/*81*/24,/*82*/1,/*83*/2,/*84*/1,/*85*/3,/*86*/1,/*87*/3,/*88*/4,/*89*/1,
    /*90*/2,/*91*/6,/*92*/3,/*93*/2,/*94*/3,/*95*/3,/*96*/4,/*97*/1,/*98*/1,/*99*/3,
    /*100*/3,/*101*/4,/*102*/1,/*103*/3,/*104*/3,/*105*/8,/*106*/2
};


// Returns the ordered list of TIME/LYRIC cues found in a .dsc file, or an
// empty list if the file couldn't be read or parsed.
std::vector<LyricCue> extract_lyric_cues(const fs::path& dsc_path) {
    std::vector<LyricCue> cues;

    std::ifstream f(dsc_path, std::ios::binary | std::ios::ate);
    if (!f) return cues;
    size_t sz = (size_t)f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data(sz);
    f.read((char*)data.data(), sz);
    if (sz < 4) return cues;

    size_t pos = 4; // skip signature
    int32_t currentTime = 0;

    while (pos + 4 <= sz) {
        int32_t op_id = (int32_t)read_u32le(data.data() + pos);
        pos += 4;

        if (op_id == 0) break; // END

        if (op_id < 0 || op_id >= (int)(sizeof(kDscOpParamCount) / sizeof(kDscOpParamCount[0]))) {
            GuiLog( "  WARN: Unknown DSC op id %d, stopping lyric scan\n", op_id);
            break;
        }
        int param_cnt = kDscOpParamCount[op_id];
        if (pos + (size_t)param_cnt * 4 > sz) break;

        if (op_id == 1) { // TIME
            currentTime = (int32_t)read_u32le(data.data() + pos);
        }
        else if (op_id == 24) { // LYRIC
            int32_t lyric_index = (int32_t)read_u32le(data.data() + pos);
            cues.push_back({ currentTime / 100000.0, lyric_index });
        }

        pos += (size_t)param_cnt * 4;
    }

    return cues;
}

std::string fmt_lrc_time(double t) {
    int minutes = (int)(t / 60.0);
    double seconds = t - minutes * 60.0;
    char buf[16];
    snprintf(buf, sizeof(buf), "[%02d:%05.2f]", minutes, seconds);
    return buf;
}

// The lyric row only fits ~90 characters before overflowing off-screen
// (empirically confirmed in-game); wrap anything longer onto extra timed
// lines, splitting the available display time proportionally by length.


// Splits `text` into chunks of at most maxChars, breaking on spaces so words
// stay whole. `duration` is the total time the original line would have been
// shown for; each chunk gets a share of that proportional to its length, so
// longer chunks linger a bit longer instead of every chunk getting equal time.
std::vector<WrappedLyricPart> wrap_lyric_line(const std::string& text, size_t maxChars, double duration) {
    std::vector<std::string> chunks;
    size_t start = 0;
    while (start < text.size()) {
        if (text.size() - start <= maxChars) {
            chunks.push_back(text.substr(start));
            break;
        }
        size_t breakAt = text.rfind(' ', start + maxChars);
        if (breakAt == std::string::npos || breakAt <= start) {
            // No good break point (single very long word) �� hard split.
            breakAt = start + maxChars;
        }
        chunks.push_back(text.substr(start, breakAt - start));
        start = (breakAt < text.size() && text[breakAt] == ' ') ? breakAt + 1 : breakAt;
    }
    if (chunks.empty()) chunks.push_back(text);

    size_t totalChars = 0;
    for (auto& c : chunks) totalChars += c.size();
    if (totalChars == 0) totalChars = 1;

    std::vector<WrappedLyricPart> out;
    double offset = 0.0;
    for (auto& c : chunks) {
        out.push_back({ c, offset });
        offset += duration * ((double)c.size() / (double)totalChars);
    }
    return out;
}

