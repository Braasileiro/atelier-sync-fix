#pragma once

#include <windows.h>

namespace atfix {
// A pointer to (unowned) data
struct Buffer {
  const void* data;
  SIZE_T length;
};

// Returns whether or not the given shader should be converted to use sample rate shading
bool shouldUseSampleRate(const void* data, SIZE_T length);
// Returns a pointer to a blob of length `length` that is the given shader converted to use sample rate shading
// The caller should free the pointer with `free`
void* convertShaderToSampleRate(const void* data, SIZE_T length);

// Returns a shader to replace the given shader, or a null buffer if no replacements exist for the given shader
Buffer getReplacementShader(const void* data, SIZE_T length);

// Returns a shader to use alpha to coverage if supported, otherwise a null buffer
Buffer getAlphaToCoverageShader(const void* data, SIZE_T length);
} // namespace atfix
