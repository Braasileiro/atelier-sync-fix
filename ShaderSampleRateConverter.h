#pragma once

#include <windows.h>

namespace atfix {
// Returns whether or not the given shader should be converted to use sample rate shading
bool shouldUseSampleRate(const void* data, SIZE_T length);
// Returns a pointer to a blob of length `length` that is the given shader converted to use sample rate shading
// The caller should free the pointer with `free`
void* convertShaderToSampleRate(const void* data, SIZE_T length);
} // namespace atfix
