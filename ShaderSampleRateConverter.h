#pragma once

#include <windows.h>

namespace atfix {
struct Buffer {
  void* data;
  SIZE_T length;
};

// Returns whether or not the given shader should be converted to use sample rate shading
bool shouldUseSampleRate(const void* data, SIZE_T length);
// Returns a pointer to a blob of length `length` that is the given shader converted to use sample rate shading
// The caller should free the pointer with `free`
void* convertShaderToSampleRate(const void* data, SIZE_T length);

// Converts a shader to use alpha to coverage if supported, else returns a null buffer
// The caller should free the buffer with `free`
Buffer convertShaderToAlphaToCoverage(const void* data, SIZE_T length);
} // namespace atfix
