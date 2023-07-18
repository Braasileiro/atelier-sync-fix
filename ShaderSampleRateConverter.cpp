#include "ShaderSampleRateConverter.h"

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

static Buffer applyPatch(const void* data, SIZE_T length, ShaderPatch patch) {
  SIZE_T ndwords = 0;
  const DWORD* original = static_cast<const DWORD*>(data);
  for (SIZE_T i = 0; i < patch.numEntries; i++)
    ndwords += patch.entries[i].length;
  Buffer out;
  out.length = ndwords * sizeof(DWORD);
  out.data = malloc(out.length);
  if (!out.data) {
    out.length = 0;
    return out;
  }
  BYTE* write = static_cast<BYTE*>(out.data);
  for (SIZE_T i = 0; i < patch.numEntries; i++) {
    const ShaderPatch::Entry& entry = patch.entries[i];
    SIZE_T len = entry.length * sizeof(DWORD);
    const DWORD* base = entry.fromOriginal ? original : patch.replacementData;
    memcpy(write, base + entry.offset, len);
    write += len;
  }
  CalculateDXBCChecksum(static_cast<BYTE*>(out.data), out.length, static_cast<ShaderHeader*>(out.data)->hash.value);
  return out;
}

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
//  {0x0cd1b9e5, 0x22e7069e, 0x476455ff, 0x98bfd850}, // Semi-transparent objects (e.g. grass) [expensive, gets used for a lot of non-transparent objects too]
    {0xd74438d8, 0xa2667a70, 0x5c3cae10, 0x1944d91e}, // Semi-transparent background (e.g. background tree leaves)
  };
  for (const ShaderHash& hash : sampleRateShaders)
    if (hash == header->hash)
      return true;
  if (config.ssaaTransparentObjects && header->hash == ShaderHash({ 0x0cd1b9e5, 0x22e7069e, 0x476455ff, 0x98bfd850 }))
      return true;
  return false;
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

Buffer convertShaderToAlphaToCoverage(const void* data, SIZE_T length) {
  const ShaderHeader* header = static_cast<const ShaderHeader*>(data);
  const ShaderPatch* patch = nullptr;
  if (header->hash == ShaderHash({ 0x0cd1b9e5, 0x22e7069e, 0x476455ff, 0x98bfd850 }))
    patch = &alphaToCoverageObjectShader;
  if (!patch)
    return {nullptr, 0};
  return applyPatch(data, length, *patch);
}

static constexpr DWORD alphaToCoverageObjectShaderReplacementData[] = {
  // [ 0] Shader header @ 0
  0x43425844, 0x0cd1b9e5, 0x22e7069e, 0x476455ff, 0x98bfd850, 0x00000001, 0x00003414, 0x00000005,
  0x00000034, 0x0000038c, 0x0000045c, 0x00000490, 0x00003378,
  // [13] SHEX data length info @ 0x494
  0x2ee0, 0x50, 0xbb8,
  // [16] Number of temporary registers @ 0x560
  9,
  // [17] Alpha to coverage calculation @ 0x61c replaces 0x4c
  0x0500007b, 0x00100012, 8, 0x0010003a, 2,                            // deriv_rtx_fine r8.x, r2.w
  0x0500007d, 0x00100022, 8, 0x0010003a, 2,                            // deriv_rty_fine r8.y, r2.w
  0x09000000, 0x00100012, 8, 0x8010000a, 0x81, 8, 0x8010001a, 0x81, 8, // add r8.x, |r8.x|, |r8.y|
  0x09000000, 0x00100022, 8, 0x0010003a, 2, 0x8020801a, 0x41, 0, 0,    // add r8.y, r2.w, -vATest
  0x0700000e, 0x00100012, 8, 0x0010001a, 8, 0x0010000a, 8,             // div r8.x, r8.y, r8.x
  0x07002000, 0x00100012, 8, 0x0010000a, 8, 0x00004001, 0x3f000000,    // add_sat r8.x, r8.x, l(0.5)
  0x0300000d, 0x0010000a, 8,                                           // discard_z r8.x
  // [62] Move final alpha value into o0 @ 0x32f8
  0x05000036, 0x00102082, 0, 0x0010000a, 8,                            // mov o0.w, r8.x
};

static constexpr ShaderPatch::Entry alphaToCoverageObjectShaderReplacementEntries[] = {
  {   0,   13, FALSE}, // Header
  {  13,  280, TRUE},
  {  13,    3, FALSE}, // SHEX data length info
  { 296,   48, TRUE},
  {  16,    1, FALSE}, // # tmp registers
  { 345,   46, TRUE},
  {  17,   45, FALSE}, // Alpha coverage code
  { 410, 2852, TRUE},
  {  62,    5, FALSE}, // Final alpha write
  {3267,   40, TRUE},
};

constexpr ShaderPatch alphaToCoverageObjectShader(alphaToCoverageObjectShaderReplacementData, alphaToCoverageObjectShaderReplacementEntries);

} // namespace atfix
