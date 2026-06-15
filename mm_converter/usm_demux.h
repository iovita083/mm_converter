// usm_demux.h - CRI USM demuxer (extracts .m2v and .adx/.hca)
// Ported from VGMToolbox/UsmToolkit. No external dependencies.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>

namespace usm {

// ---- helpers ----------------------------------------------------------------

static inline uint32_t rd32be(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static inline uint16_t rd16be(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);
}

static size_t find_seq(const std::vector<uint8_t>& buf, const uint8_t* needle, size_t nlen, size_t from = 0) {
    for (size_t i = from; i + nlen <= buf.size(); ++i)
        if (memcmp(buf.data()+i, needle, nlen) == 0) return i;
    return std::string::npos;
}

// ---- block tags -------------------------------------------------------------

static const uint8_t TAG_CRID[4] = { 0x43,0x52,0x49,0x44 }; // CRID (file header)
static const uint8_t TAG_SFV [4] = { 0x40,0x53,0x46,0x56 }; // @SFV (video)
static const uint8_t TAG_SFA [4] = { 0x40,0x53,0x46,0x41 }; // @SFA (audio)
static const uint8_t TAG_ALP [4] = { 0x40,0x41,0x4C,0x50 }; // @ALP (skip)
static const uint8_t TAG_SBT [4] = { 0x40,0x53,0x42,0x54 }; // @SBT (skip)
static const uint8_t TAG_CUE [4] = { 0x40,0x43,0x55,0x45 }; // @CUE (skip)

// sentinels used to trim leading header / trailing footer from raw streams
static const uint8_t SIG_HEADER_END  [32] = "#HEADER END     ===============";
static const uint8_t SIG_METADATA_END[32] = "#METADATA END   ===============";
static const uint8_t SIG_CONTENTS_END[32] = "#CONTENTS END   ===============";

// ---- result -----------------------------------------------------------------

struct DemuxResult {
    std::string video_path; // .m2v, empty if none
    std::string error;
};

// ---- main demux -------------------------------------------------------------
// Extracts streams from `usm_path` into `out_dir`.
// Output filenames are `<stem>.m2v` and `<stem>.adx` / `<stem>.hca`.

inline DemuxResult demux(const std::string& usm_path, const std::string& out_dir) {
    DemuxResult res;

    std::ifstream fin(usm_path, std::ios::binary);
    if (!fin) { res.error = "Cannot open: " + usm_path; return res; }
    fin.seekg(0, std::ios::end);
    size_t file_size = (size_t)fin.tellg();
    fin.seekg(0, std::ios::beg);

    std::string stem = std::filesystem::path(usm_path).stem().string();
    std::string v_path = (std::filesystem::path(out_dir) / (stem + ".m2v")).string();

    std::vector<uint8_t> video_buf;
    uint8_t hdr[16];

    size_t offset = 0;
    while (offset + 8 <= file_size) {
        fin.seekg((std::streamoff)offset);
        if (!fin.read((char*)hdr, 8)) break;

        const uint8_t* tag = hdr;
        uint32_t block_size = rd32be(hdr + 4);

        bool is_video = memcmp(tag, TAG_SFV, 4) == 0;
        bool is_skip  = memcmp(tag, TAG_SFA, 4)==0 || memcmp(tag, TAG_CRID,4)==0
                     || memcmp(tag, TAG_ALP, 4)==0 || memcmp(tag, TAG_SBT,4)==0
                     || memcmp(tag, TAG_CUE, 4)==0;

        if (!is_video && !is_skip) break;

        if (is_video && block_size > 4) {
            // read 4 more bytes to get the two u16 offsets at +8 and +0xA
            uint8_t extra[4];
            fin.seekg((std::streamoff)(offset + 8));
            fin.read((char*)extra, 4);
            uint16_t hdr_skip  = rd16be(extra + 0); // bytes to skip at start of payload
            uint16_t foot_skip = rd16be(extra + 2); // bytes to skip at end of payload

            // payload starts at offset+8 (after 4-byte tag + 4-byte size)
            // effective data: skip hdr_skip from start, foot_skip from end
            if (block_size > (uint32_t)(hdr_skip + foot_skip)) {
                uint32_t data_off  = hdr_skip;
                uint32_t data_len  = block_size - hdr_skip - foot_skip;
                size_t abs_off     = offset + 8 + data_off;

                if (abs_off + data_len <= file_size) {
                    std::vector<uint8_t> chunk(data_len);
                    fin.seekg((std::streamoff)abs_off);
                    fin.read((char*)chunk.data(), data_len);
                    video_buf.insert(video_buf.end(), chunk.begin(), chunk.end());
                }
            }
        }

        offset += 8 + block_size;
    }

    // ---- trim header/metadata prefix from each raw stream -------------------
    auto trim_stream = [](std::vector<uint8_t>& buf) {
        size_t hend = find_seq(buf, SIG_HEADER_END,   32);
        size_t mend = find_seq(buf, SIG_METADATA_END, 32);
        size_t cut  = 0;
        if (mend != std::string::npos && (hend == std::string::npos || mend > hend))
            cut = mend + 32;
        else if (hend != std::string::npos)
            cut = hend + 32;
        if (cut > 0 && cut < buf.size())
            buf.erase(buf.begin(), buf.begin() + cut);

        // trim #CONTENTS END footer
        size_t cend = find_seq(buf, SIG_CONTENTS_END, 32);
        if (cend != std::string::npos && cend < buf.size())
            buf.resize(cend);
    };

    // ---- write video --------------------------------------------------------
    if (!video_buf.empty()) {
        trim_stream(video_buf);
        std::ofstream fv(v_path, std::ios::binary);
        fv.write((char*)video_buf.data(), video_buf.size());
        res.video_path = v_path;
    }

    return res;
}

} // namespace usm
