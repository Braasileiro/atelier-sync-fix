#pragma once

#include <Windows.h>

namespace atfix {
struct ShaderHash {
  DWORD value[4];
  bool operator==(const ShaderHash& other) const {
    bool eq = true;
    eq &= value[0] == other.value[0];
    eq &= value[1] == other.value[1];
    eq &= value[2] == other.value[2];
    eq &= value[3] == other.value[3];
    return eq;
  }
};

struct ShaderReplacement {
  ShaderHash originalHash;
  const void* newShader;
  SIZE_T newShaderLength;
};

struct ShaderReplacementList {
  const ShaderReplacement* replacements;
  SIZE_T count;
};

extern const ShaderReplacementList shaderReplacements;
extern const ShaderReplacementList shaderReplacementsAlphaToCoverage;
}
