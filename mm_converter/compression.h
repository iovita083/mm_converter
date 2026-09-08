#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

bool decompress_data(const uint8_t* input, size_t input_size,
    std::vector<uint8_t>& output, size_t expected_size = 0);

bool compress_data(const uint8_t* input, size_t input_size,
    std::vector<uint8_t>& output);
