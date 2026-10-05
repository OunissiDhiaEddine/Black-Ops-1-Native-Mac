// Direct3D 9 Shader Model 2/3 bytecode -> Metal Shading Language (ADR-005 option a).
// Scope = what the trace census shows the game uses (docs/RE_NOTES.md § D3D9 census): vs_3_0 /
// ps_3_0, 25 + 34 opcodes, float constants, 2D / cube / volume samplers.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bo1::sm3 {

enum class Stage : uint8_t { Vertex, Pixel };

// D3DDECLUSAGE
enum Usage : uint8_t {
    kPosition, kBlendWeight, kBlendIndices, kNormal, kPSize, kTexCoord, kTangent, kBinormal,
    kTessFactor, kPositionT, kColor, kFog, kDepth, kSample,
};
const char *usage_name(uint8_t usage);

struct Semantic {
    uint8_t usage = 0, index = 0;
    uint16_t reg = 0;      // v# (inputs) or o# (vertex outputs)
    uint8_t mask = 0xF;    // declared write mask
};

struct SamplerDecl {
    uint8_t reg = 0;       // s#
    uint8_t type = 2;      // 2 = 2D, 3 = cube, 4 = volume
};

struct Options {
    // Samplers that are bound to depth textures and must be sampled with comparison (D3D9
    // "hardware shadow map" behaviour). Bit n = s<n>. Decided by the pipeline, not the shader.
    uint32_t shadow_samplers = 0;
    std::string entry = "main0";
};

struct Result {
    bool ok = false;
    std::string error;                 // first unsupported construct / malformed token
    Stage stage = Stage::Vertex;
    uint8_t major = 0, minor = 0;
    std::string msl;
    std::vector<Semantic> inputs;      // VS: vertex attributes (attribute index = reg); PS: varyings
    std::vector<Semantic> outputs;     // VS: varyings written
    std::vector<SamplerDecl> samplers; // texture/sampler index = reg
    uint32_t float_consts_used = 0;    // highest c# referenced + 1 (0 with relative addressing: all)
    bool uses_relative_consts = false;
    bool writes_depth = false, uses_vpos = false, uses_vface = false, uses_discard = false;
    std::string creator;               // compiler banner from the CTAB, if any
    uint32_t instruction_count = 0;
};

// `code` = the token stream as passed to CreateVertexShader / CreatePixelShader (incl. END).
Result translate(std::span<const uint32_t> code, const Options &opts = {});

// Layout shared by generated shaders and the Metal backend.
inline constexpr unsigned kConstBufferIndex = 30;  // [[buffer(30)]] in both stages
inline constexpr unsigned kMaxFloatConsts = 256;
// Varyings every vertex shader writes and any pixel shader may read: TEXCOORD0..15, COLOR0..1.
// Unwritten ones are zero. (Fixed superset for now: Metal requires fragment inputs to exist in the
// vertex output; per-pair linking can replace this if varying cost shows up in profiles.)
inline constexpr unsigned kTexCoordVaryings = 16, kColorVaryings = 2;

} // namespace bo1::sm3
