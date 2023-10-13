#include "ShaderSampleRateConverter.h"

#include "Shaders.h"
#include "DXBCChecksum.h"
#include "impl.h"

namespace atfix {

struct ShaderPatch {
  struct Entry {
    DWORD offset;
    USHORT length;
    BOOLEAN fromOriginal;
  };
  const DWORD* replacementData;
  const Entry* entries;
  SIZE_T numEntries;
  template <SIZE_T N>
  constexpr ShaderPatch(const DWORD* replacementData, const Entry(&entries)[N])
    : replacementData(replacementData), entries(entries), numEntries(N)
  {
  }
};

extern const ShaderPatch alphaToCoverageObjectShader;

struct ShaderHeader {
  DWORD magic;
  ShaderHash hash;
  DWORD unk;
  DWORD size;
  DWORD nChunks;
  DWORD chunkOffsets[];
};

struct DXBCChunk {
  DWORD magic;
  DWORD length;
  DWORD data[];
};

bool shouldUseSampleRate(const void* data, SIZE_T length) {
  const ShaderHeader* header = static_cast<const ShaderHeader*>(data);
  static const ShaderHash sampleRateShaders[] = {
    {0x621ea95e, 0xdd49b404, 0x4c6d8ab0, 0x9f557299}, // Clothing
    {0x91a929f9, 0xbcc81d7b, 0xbd937e87, 0xcc2ea392}, // Clothing Edges
    {0x3458bad3, 0xf59e41b0, 0x3897bb39, 0x39644d6e}, // Hair
    {0x4171a662, 0x1df203a2, 0x10f5d54c, 0xbb2c9cfe}, // Face
    {0xb169a00d, 0xa921903c, 0x25c56cb2, 0x2a150784}, // Face Edges
    {0x80701da2, 0x06ff6fad, 0xcc743be0, 0xbcceaf98}, // Eyes
    {0x66f3f5e9, 0x6b52c044, 0x6a99bd8a, 0x133fe74a}, // Eyebrows
    {0x352edfef, 0x8a7e1aa0, 0xb1e9c972, 0xd9f84b10}, // Accessories
    {0x576b6302, 0xf209d2fa, 0x542b7c05, 0xb45b37af}, // Accessory Edges
    {0x19cce3ab, 0x52dfbaf0, 0x302d710c, 0xd90bc983}, // Character Transparent
//  {0x0cd1b9e5, 0x22e7069e, 0x476455ff, 0x98bfd850}, // Semi-transparent objects (e.g. grass) [expensive, gets used for a lot of non-transparent objects too]
//  {0xd74438d8, 0xa2667a70, 0x5c3cae10, 0x1944d91e}, // Semi-transparent background (e.g. background tree leaves)
  };
  for (const ShaderHash& hash : sampleRateShaders)
    if (config.ssaaCharacters && hash == header->hash)
      return true;
  return config.ssaaAll;
}

enum DXShaderOpcode {
  OP_DCL_INPUT_PS = 0x62,
  OP_DCL_INPUT_PS_SIV = 0x64,
};

enum DXInterpolationMode {
  INTERPOLATION_MODE_UNDEFINED = 0,
  INTERPOLATION_MODE_CONSTANT = 1,
  INTERPOLATION_MODE_LINEAR = 2,
  INTERPOLATION_MODE_LINEAR_CENTROID = 3,
  INTERPOLATION_MODE_LINEAR_NOPERSPECTIVE = 4,
  INTERPOLATION_MODE_LINEAR_NOPERSPECTIVE_CENTROID = 5,
  INTERPOLATION_MODE_LINEAR_SAMPLE = 6,
  INTERPOLATION_MODE_LINEAR_NOPERSPECTIVE_SAMPLE = 7,
};

void* convertShaderToSampleRate(const void* data, SIZE_T length) {
  void* out = malloc(length);
  if (!out)
    return nullptr;
  memcpy(out, data, length);
  ShaderHeader* header = static_cast<ShaderHeader*>(out);
  DWORD nChunks = header->nChunks;
  for (DWORD i = 0; i < nChunks; i++) {
    DXBCChunk* chunk = reinterpret_cast<DXBCChunk*>(static_cast<BYTE*>(out) + header->chunkOffsets[i]);
    DWORD* end = reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(chunk) + chunk->length);
    if (chunk->magic == MAKEFOURCC('S', 'H', 'E', 'X')) {
      DWORD* insns = &chunk->data[2];
      while (insns < end) {
        DWORD insn = *insns;
        DWORD opcode = insn & 0xff;
        DWORD length = (insn >> 24) & 0x1f;
        if (!length)
          length = insns[1];
        if (!length) {
          free(out);
          return nullptr;
        }
        if (opcode == OP_DCL_INPUT_PS || opcode == OP_DCL_INPUT_PS_SIV) {
          DWORD interpolation = (insn >> 11) & 0xf;
          DWORD old = interpolation;
          if (interpolation == INTERPOLATION_MODE_LINEAR)
            interpolation = INTERPOLATION_MODE_LINEAR_SAMPLE;
          if (interpolation == INTERPOLATION_MODE_LINEAR_NOPERSPECTIVE)
            interpolation = INTERPOLATION_MODE_LINEAR_NOPERSPECTIVE_SAMPLE;
          insn &= ~(DWORD)(0xf << 11);
          insn |= interpolation << 11;
          *insns = insn;
        }
        insns += length;
      }
    } else if (chunk->magic == MAKEFOURCC('S', 'T', 'A', 'T')) {
      chunk->data[28] = 1; // Sample frequency flag
    }
  }
  CalculateDXBCChecksum(static_cast<BYTE*>(out), length, header->hash.value);
  return out;
}

static Buffer findReplacement(const void* data, SIZE_T length, ShaderReplacementList list) {
  const ShaderHeader* header = static_cast<const ShaderHeader*>(data);
  for (SIZE_T i = 0; i < list.count; i++) {
    if (header->hash == list.replacements[i].originalHash)
      return { list.replacements[i].newShader, list.replacements[i].newShaderLength };
  }
  return { nullptr, 0 };
}

Buffer getReplacementShader(const void* data, SIZE_T length) {
  return findReplacement(data, length, shaderReplacements);
}

Buffer getAlphaToCoverageShader(const void* data, SIZE_T length) {
  ShaderReplacementList list;
  if (config.msaaSamples < 2) {
      return {};
  } else if (!config.sampleRateAlpha) {
    list = shaderReplacementsAlphaToCoverage0;
  } else if (config.msaaSamples > 8) {
    list = shaderReplacementsAlphaToCoverage16;
  } else if (config.msaaSamples > 4) {
    list = shaderReplacementsAlphaToCoverage8;
  } else if (config.msaaSamples > 2) {
    list = shaderReplacementsAlphaToCoverage4;
  } else {
    list = shaderReplacementsAlphaToCoverage2;
  }
  return findReplacement(data, length, list);
}

} // namespace atfix
