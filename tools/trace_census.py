#!/usr/bin/env python3
"""T-015: D3D9 census from a binary trace — what the Metal backend actually has to support.

  .venv/bin/python tools/trace_census.py TRACE.bin [--name menu] [--shaders DIR]

Outputs
  re/d3d9_census_<name>.json   machine-readable census (no asset data: counts and enums only)
  stdout                       markdown summary for docs/RE_NOTES.md
  DIR/<sha1>.{vs,ps}           with --shaders: the unique shader bytecode blobs (game data ->
                               keep under artifacts/, never commit)
"""
import argparse
import hashlib
import json
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import trace_reader as tr  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

RS = {7: "ZENABLE", 8: "FILLMODE", 9: "SHADEMODE", 14: "ZWRITEENABLE", 15: "ALPHATESTENABLE", 16: "LASTPIXEL",
      19: "SRCBLEND", 20: "DESTBLEND", 22: "CULLMODE", 23: "ZFUNC", 24: "ALPHAREF", 25: "ALPHAFUNC",
      26: "DITHERENABLE", 27: "ALPHABLENDENABLE", 28: "FOGENABLE", 29: "SPECULARENABLE", 34: "FOGCOLOR",
      35: "FOGTABLEMODE", 36: "FOGSTART", 37: "FOGEND", 38: "FOGDENSITY", 48: "RANGEFOGENABLE",
      52: "STENCILENABLE", 53: "STENCILFAIL", 54: "STENCILZFAIL", 55: "STENCILPASS", 56: "STENCILFUNC",
      57: "STENCILREF", 58: "STENCILMASK", 59: "STENCILWRITEMASK", 60: "TEXTUREFACTOR", 136: "CLIPPING",
      137: "LIGHTING", 139: "AMBIENT", 140: "FOGVERTEXMODE", 141: "COLORVERTEX", 142: "LOCALVIEWER",
      143: "NORMALIZENORMALS", 151: "VERTEXBLEND", 152: "CLIPPLANEENABLE", 154: "POINTSIZE",
      155: "POINTSIZE_MIN", 156: "POINTSPRITEENABLE", 157: "POINTSCALEENABLE", 161: "MULTISAMPLEANTIALIAS",
      162: "MULTISAMPLEMASK", 166: "POINTSIZE_MAX", 168: "COLORWRITEENABLE", 171: "BLENDOP",
      174: "SCISSORTESTENABLE", 175: "SLOPESCALEDEPTHBIAS", 176: "ANTIALIASEDLINEENABLE",
      185: "TWOSIDEDSTENCILMODE", 186: "CCW_STENCILFAIL", 187: "CCW_STENCILZFAIL", 188: "CCW_STENCILPASS",
      189: "CCW_STENCILFUNC", 190: "COLORWRITEENABLE1", 191: "COLORWRITEENABLE2", 192: "COLORWRITEENABLE3",
      193: "BLENDFACTOR", 194: "SRGBWRITEENABLE", 195: "DEPTHBIAS", 206: "SEPARATEALPHABLENDENABLE",
      207: "SRCBLENDALPHA", 208: "DESTBLENDALPHA", 209: "BLENDOPALPHA"}
RS.update({128 + i: f"WRAP{i}" for i in range(8)})
RS.update({198 + i: f"WRAP{8 + i}" for i in range(8)})
SAMP = {1: "ADDRESSU", 2: "ADDRESSV", 3: "ADDRESSW", 4: "BORDERCOLOR", 5: "MAGFILTER", 6: "MINFILTER",
        7: "MIPFILTER", 8: "MIPMAPLODBIAS", 9: "MAXMIPLEVEL", 10: "MAXANISOTROPY", 11: "SRGBTEXTURE",
        12: "ELEMENTINDEX", 13: "DMAPOFFSET"}
FMT = {0: "UNKNOWN", 20: "R8G8B8", 21: "A8R8G8B8", 22: "X8R8G8B8", 23: "R5G6B5", 24: "X1R5G5B5", 25: "A1R5G5B5",
       26: "A4R4G4B4", 28: "A8", 31: "A2B10G10R10", 32: "A8B8G8R8", 33: "X8B8G8R8", 34: "G16R16",
       35: "A2R10G10B10", 36: "A16B16G16R16", 50: "L8", 51: "A8L8", 60: "V8U8", 63: "Q8W8V8U8", 70: "D16_LOCKABLE",
       71: "D32", 73: "D15S1", 75: "D24S8", 77: "D24X8", 79: "D24X4S4", 80: "D16", 81: "L16", 82: "D32F_LOCKABLE",
       83: "D24FS8", 101: "INDEX16", 102: "INDEX32", 111: "R16F", 112: "G16R16F", 113: "A16B16G16R16F",
       114: "R32F", 115: "G32R32F", 116: "A32B32G32R32F"}
POOL = {0: "DEFAULT", 1: "MANAGED", 2: "SYSTEMMEM", 3: "SCRATCH"}
USAGE = {0x1: "RENDERTARGET", 0x2: "DEPTHSTENCIL", 0x8: "WRITEONLY", 0x10: "SOFTWAREPROCESSING",
         0x20: "DONOTCLIP", 0x40: "POINTS", 0x200: "DYNAMIC", 0x400: "AUTOGENMIPMAP", 0x4000: "DMAP"}
PRIM = {1: "POINTLIST", 2: "LINELIST", 3: "LINESTRIP", 4: "TRIANGLELIST", 5: "TRIANGLESTRIP", 6: "TRIANGLEFAN"}
DECLTYPE = ["FLOAT1", "FLOAT2", "FLOAT3", "FLOAT4", "D3DCOLOR", "UBYTE4", "SHORT2", "SHORT4", "UBYTE4N",
            "SHORT2N", "SHORT4N", "USHORT2N", "USHORT4N", "UDEC3", "DEC3N", "FLOAT16_2", "FLOAT16_4", "UNUSED"]
DECLUSAGE = ["POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
             "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"]
QUERY = {1: "VCACHE", 2: "RESOURCEMANAGER", 3: "VERTEXSTATS", 4: "EVENT", 8: "OCCLUSION", 9: "TIMESTAMP",
         10: "TIMESTAMPDISJOINT", 11: "TIMESTAMPFREQ"}
SIO = {0: "nop", 1: "mov", 2: "add", 3: "sub", 4: "mad", 5: "mul", 6: "rcp", 7: "rsq", 8: "dp3", 9: "dp4",
       10: "min", 11: "max", 12: "slt", 13: "sge", 14: "exp", 15: "log", 16: "lit", 17: "dst", 18: "lrp",
       19: "frc", 20: "m4x4", 21: "m4x3", 22: "m3x4", 23: "m3x3", 24: "m3x2", 25: "call", 26: "callnz",
       27: "loop", 28: "ret", 29: "endloop", 30: "label", 31: "dcl", 32: "pow", 33: "crs", 34: "sgn",
       35: "abs", 36: "nrm", 37: "sincos", 38: "rep", 39: "endrep", 40: "if", 41: "ifc", 42: "else",
       43: "endif", 44: "break", 45: "breakc", 46: "mova", 47: "defb", 48: "defi", 64: "texcoord",
       65: "texkill", 66: "texld", 78: "expp", 79: "logp", 80: "cnd", 81: "def", 88: "cmp", 89: "bem",
       90: "dp2add", 91: "dsx", 92: "dsy", 93: "texldd", 94: "setp", 95: "texldl", 96: "breakp"}
SAMPLER_TYPE = {0: "unknown", 2: "2d", 3: "cube", 4: "volume"}


def fmt_name(v):
    if v in FMT:
        return FMT[v]
    b = struct.pack("<I", v)
    return b.decode("ascii") if all(32 < c < 127 for c in b) else f"0x{v:x}"


def usage_name(v):
    return "|".join(n for bit, n in USAGE.items() if v & bit) or "0"


def parse_shader(code: bytes):
    """Return (kind, version, instruction Counter, sampler-type Counter, dcl usages, has_ctab, creator)."""
    toks = struct.unpack_from(f"<{len(code) // 4}I", code)
    ver = toks[0]
    kind = "vs" if (ver >> 16) == 0xFFFE else "ps"
    version = f"{kind}_{(ver >> 8) & 0xFF}_{ver & 0xFF}"
    ops, samplers, usages = Counter(), Counter(), Counter()
    ctab, creator = False, ""
    i = 1
    while i < len(toks):
        t = toks[i]
        op = t & 0xFFFF
        if op == 0xFFFF:
            break
        if op == 0xFFFE:
            n = (t >> 16) & 0x7FFF
            if n >= 1 and toks[i + 1] == 0x42415443:          # 'CTAB'
                ctab = True
                base = (i + 2) * 4
                creator_off = struct.unpack_from("<I", code, base + 4)[0]
                end = code.find(b"\0", base + creator_off)
                creator = code[base + creator_off:end].decode("latin-1", "replace")
            i += 1 + n
            continue
        length = (t >> 24) & 0xF
        ops[SIO.get(op, f"op{op}")] += 1
        if op == 31 and length >= 2:                        # dcl
            dcl, dst = toks[i + 1], toks[i + 2]
            regtype = ((dst >> 28) & 7) | ((dst >> 8) & 0x18)
            if regtype == 10:                               # sampler
                samplers[SAMPLER_TYPE.get((dcl >> 27) & 0xF, "?")] += 1
            else:
                u = dcl & 0x1F
                usages[f"{DECLUSAGE[u] if u < len(DECLUSAGE) else u}{(dcl >> 16) & 0xF}"] += 1
        i += 1 + length
    return kind, version, ops, samplers, usages, ctab, creator


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--name", default="run")
    ap.add_argument("--shaders", help="directory to write unique shader blobs to (game data)")
    args = ap.parse_args()
    ifaces = tr.load_interfaces()
    names = [[f"{com}::{m}" for m in meth] for com, meth in ifaces]

    calls = Counter()
    frames = 0
    rs_vals, samp_vals = defaultdict(Counter), defaultdict(Counter)
    samp_stages, tex_stages = Counter(), Counter()
    tex, voltex, cubetex, vbs, ibs, rts, dss, plain = Counter(), Counter(), Counter(), Counter(), Counter(), Counter(), Counter(), Counter()
    decl_elems, decl_shapes = Counter(), Counter()
    shaders = {}
    shader_versions, shader_ops, shader_samplers, shader_creators = Counter(), Counter(), Counter(), Counter()
    vs_inputs, ps_inputs = Counter(), Counter()
    shader_sizes = defaultdict(list)
    prim_types, prims_per_draw = Counter(), []
    vs_const, ps_const = Counter(), Counter()
    max_vs_reg = max_ps_reg = 0
    clear_flags, stretch, queries, query_issue = Counter(), Counter(), Counter(), Counter()
    rt_index, streams, stream_strides = Counter(), Counter(), Counter()
    viewports, lock_flags = Counter(), defaultdict(Counter)
    upload_bytes = Counter()
    frame_calls, frame_draws, frame_prims = Counter(), Counter(), Counter()
    obj_kind = {}
    rt_formats_bound = Counter()
    surf_desc = {}
    present_pp = None
    failures = Counter()

    for r in tr.read_trace(args.trace):
        if r.depth:
            continue                                   # made by the implementation, not the game
        n = names[r.iface][r.slot]
        calls[n] += 1
        w = r.words
        blobs = dict(r.blobs)
        frame_calls[frames] += 1
        if r.has_result and n.split("::")[1] not in ("AddRef", "Release") and r.result & 0x80000000:
            failures[(n, f"0x{r.result:08x}")] += 1
        if r.iface == 2 and r.slot == 3:
            frames += 1
        elif r.iface == 0 and r.slot == 16:
            if 8 in blobs:
                present_pp = struct.unpack_from("<14I", blobs[8])
        elif r.iface == 1:
            s = r.slot
            if s == 57:
                rs_vals[w[1]][w[2]] += 1
            elif s == 69:
                samp_vals[w[2]][w[3]] += 1
                samp_stages[w[1]] += 1
            elif s == 65:
                tex_stages[w[1]] += 1
            elif s == 23:
                tex[(fmt_name(w[5]), usage_name(w[4]), POOL.get(w[6], w[6]), "mips" if w[3] != 1 else "nomips")] += 1
                upload_bytes["CreateTexture"] += 0
            elif s == 24:
                voltex[(w[1], w[2], w[3], w[4], fmt_name(w[6]), usage_name(w[5]), POOL.get(w[7], w[7]))] += 1
            elif s == 25:
                cubetex[(w[1], w[2], fmt_name(w[4]), usage_name(w[3]), POOL.get(w[5], w[5]))] += 1
            elif s == 26:
                vbs[(usage_name(w[2]), POOL.get(w[4], w[4]), "fvf" if w[3] else "decl")] += 1
            elif s == 27:
                ibs[(fmt_name(w[3]), usage_name(w[2]), POOL.get(w[4], w[4]))] += 1
            elif s == 28:
                rts[(w[1], w[2], fmt_name(w[3]), w[4])] += 1
            elif s == 29:
                dss[(w[1], w[2], fmt_name(w[3]), w[4])] += 1
            elif s == 36:
                plain[(w[1], w[2], fmt_name(w[3]), POOL.get(w[4], w[4]))] += 1
            elif s == 37:
                rt_index[w[1]] += 1
            elif s == 43:
                clear_flags[w[3]] += 1
            elif s == 34:
                stretch[("src_rect" if w[2] else "full", "dst_rect" if w[4] else "full", w[5])] += 1
            elif s == 47 and 3 in blobs:
                viewports[struct.unpack_from("<4I", blobs[3])] += 1
            elif s == 82:
                prim_types[PRIM.get(w[1], w[1])] += 1
                prims_per_draw.append(w[6])
                frame_draws[frames] += 1
                frame_prims[frames] += w[6]
            elif s in (81, 83, 84):
                prim_types[f"{names[1][s].split('::')[1]}:{PRIM.get(w[1], w[1])}"] += 1
                frame_draws[frames] += 1
            elif s == 86 and 7 in blobs:
                el = blobs[7]
                shape = []
                for i in range(0, len(el) - 8, 8):
                    stream, offset, typ, method, usage, uidx = struct.unpack_from("<HHBBBB", el, i)
                    tn = DECLTYPE[typ] if typ < len(DECLTYPE) else str(typ)
                    un = DECLUSAGE[usage] if usage < len(DECLUSAGE) else str(usage)
                    decl_elems[(un, tn)] += 1
                    shape.append(f"s{stream}+{offset}:{un}{uidx}:{tn}")
                decl_shapes[" ".join(shape)] += 1
            elif s in (91, 106) and 6 in blobs:
                code = bytes(blobs[6])
                h = hashlib.sha1(code).hexdigest()
                if h not in shaders:
                    kind, version, ops, samplers, usages, ctab, creator = parse_shader(code)
                    shaders[h] = kind
                    shader_versions[version] += 1
                    shader_ops.update({f"{kind}:{k}": v for k, v in ops.items()})
                    for k in ops:
                        shader_ops[f"{kind}#shaders_using:{k}"] += 1
                    shader_samplers.update({f"{kind}:{k}": v for k, v in samplers.items()})
                    (vs_inputs if kind == "vs" else ps_inputs).update(usages)
                    shader_creators[creator or ("no CTAB" if not ctab else "?")] += 1
                    shader_sizes[kind].append(len(code))
                    if args.shaders:
                        d = Path(args.shaders)
                        d.mkdir(parents=True, exist_ok=True)
                        (d / f"{h}.{kind}").write_bytes(code)
            elif s == 94:
                vs_const[w[3]] += 1
                max_vs_reg = max(max_vs_reg, w[1] + w[3])
            elif s == 109:
                ps_const[w[3]] += 1
                max_ps_reg = max(max_ps_reg, w[1] + w[3])
            elif s == 100:
                streams[w[1]] += 1
                stream_strides[w[4]] += 1
            elif s == 118:
                queries[QUERY.get(w[1], w[1])] += 1
        elif r.iface == 9 and r.slot == 6:
            query_issue[w[1]] += 1
        if 9 in blobs and n.rsplit("::", 1)[1].startswith("Unlock"):
            d = tr.lock_desc(blobs[9])
            lock_flags[n.split("::")[0]][(d["lock_flags"], d["dumped"])] += 1
            upload_bytes[n.split("::")[0]] += d["size"] if d["dumped"] in (1, 2) else 0

    steady = [f for f in frame_calls if frame_draws.get(f, 0) > 0]
    last = steady[-300:] if steady else []
    avg = lambda c: (sum(c.get(f, 0) for f in last) / len(last)) if last else 0  # noqa: E731
    census = {
        "trace": Path(args.trace).name, "frames": frames, "game_calls": sum(calls.values()),
        "calls": dict(calls.most_common()),
        "device_methods_used": sorted(k.split("::")[1] for k in calls if k.startswith("IDirect3DDevice9::")),
        "per_frame_last300": {"calls": round(avg(frame_calls)), "draws": round(avg(frame_draws)),
                              "primitives": round(avg(frame_prims))},
        "render_states": {RS.get(k, str(k)): {"sets": sum(v.values()), "distinct_values": len(v),
                                               "top_values": [[f"0x{a:x}", c] for a, c in v.most_common(6)]}
                          for k, v in sorted(rs_vals.items())},
        "sampler_states": {SAMP.get(k, str(k)): {"sets": sum(v.values()),
                                                 "top_values": [[f"0x{a:x}", c] for a, c in v.most_common(6)]}
                           for k, v in sorted(samp_vals.items())},
        "sampler_stages_used": {str(k): v for k, v in sorted(samp_stages.items())},
        "texture_stages_used": {str(k): v for k, v in sorted(tex_stages.items())},
        "textures_2d": [[*k, c] for k, c in tex.most_common()],
        "textures_volume": [[*k, c] for k, c in voltex.most_common()],
        "textures_cube": [[*k, c] for k, c in cubetex.most_common()],
        "vertex_buffers": [[*k, c] for k, c in vbs.most_common()],
        "index_buffers": [[*k, c] for k, c in ibs.most_common()],
        "render_targets": [[*k, c] for k, c in rts.most_common()],
        "depth_stencils": [[*k, c] for k, c in dss.most_common()],
        "offscreen_plain": [[*k, c] for k, c in plain.most_common()],
        "set_render_target_index": {str(k): v for k, v in rt_index.items()},
        "clear_flags": {f"0x{k:x}": v for k, v in clear_flags.items()},
        "stretch_rect": [[*k, c] for k, c in stretch.most_common()],
        "viewports": [[*k, c] for k, c in viewports.most_common(12)],
        "draws": dict(prim_types),
        "prims_per_draw": {"max": max(prims_per_draw, default=0),
                           "median": sorted(prims_per_draw)[len(prims_per_draw) // 2] if prims_per_draw else 0},
        "vertex_decl_elements": [[u, t, c] for (u, t), c in decl_elems.most_common()],
        "vertex_decl_distinct_layouts": len(decl_shapes),
        "stream_indices": {str(k): v for k, v in streams.items()},
        "stream_strides": {str(k): v for k, v in sorted(stream_strides.items())},
        "shaders_unique": {"vs": sum(1 for k in shaders.values() if k == "vs"),
                           "ps": sum(1 for k in shaders.values() if k == "ps")},
        "shader_versions": dict(shader_versions),
        "shader_bytes": {k: {"min": min(v), "max": max(v), "total": sum(v)} for k, v in shader_sizes.items()},
        "shader_compilers": dict(shader_creators),
        "shader_instructions": dict(sorted(shader_ops.items(), key=lambda kv: -kv[1])),
        "shader_sampler_types": dict(shader_samplers),
        "vs_input_semantics": dict(vs_inputs.most_common()),
        "ps_input_semantics": dict(ps_inputs.most_common()),
        "vs_constant_set_sizes": {str(k): v for k, v in sorted(vs_const.items())}, "vs_constant_max_register": max_vs_reg,
        "ps_constant_set_sizes": {str(k): v for k, v in sorted(ps_const.items())}, "ps_constant_max_register": max_ps_reg,
        "queries_created": dict(queries), "query_issue_flags": {str(k): v for k, v in query_issue.items()},
        "lock_flags_dumped": {k: {f"0x{f:x}/{d}": c for (f, d), c in v.most_common()} for k, v in lock_flags.items()},
        "upload_bytes_recorded": dict(upload_bytes),
        "failed_calls": [[n, hr, c] for (n, hr), c in failures.most_common()],
        "present_parameters_words": list(present_pp) if present_pp else None,
    }
    out = ROOT / "re" / f"d3d9_census_{args.name}.json"
    out.write_text(json.dumps(census, indent=1) + "\n")

    p = print
    p(f"### D3D9 census — `{args.name}` ({census['trace']}: {frames} frames, {census['game_calls']:,} game calls)\n")
    pf = census["per_frame_last300"]
    p(f"- Last 300 rendered frames: **{pf['calls']:,} calls, {pf['draws']:,} draws, {pf['primitives']:,} primitives per frame**.")
    p(f"- Device methods used ({len(census['device_methods_used'])}): {', '.join(census['device_methods_used'])}.")
    p(f"- Draw calls: {census['draws']}; primitives per draw median {census['prims_per_draw']['median']}, max {census['prims_per_draw']['max']}.")
    p(f"- Shaders: {census['shaders_unique']} unique; versions {census['shader_versions']}; compilers {census['shader_compilers']}.")
    p(f"- VS constants: max register {max_vs_reg}; PS constants: max register {max_ps_reg}.")
    p(f"- Shader sampler types: {census['shader_sampler_types']}.")
    ops = census["shader_instructions"]
    for kind in ("vs", "ps"):
        used = sorted(((k.split(':', 1)[1], v) for k, v in ops.items() if k.startswith(kind + ":")), key=lambda kv: -kv[1])
        p(f"- {kind.upper()} instructions ({len(used)} distinct): " + ", ".join(f"{k} {v:,}" for k, v in used))
    p(f"- VS input semantics: {census['vs_input_semantics']}")
    p(f"- Vertex declaration element types: " + ", ".join(f"{u}:{t} x{c}" for u, t, c in census["vertex_decl_elements"]))
    p(f"- Distinct vertex layouts: {census['vertex_decl_distinct_layouts']}; streams used {census['stream_indices']}; strides {census['stream_strides']}.")
    p("- 2D textures (format, usage, pool, mips, count): " + "; ".join(" ".join(map(str, t)) for t in census["textures_2d"]))
    p("- Volume textures (w,h,d,levels,format,usage,pool,count): " + "; ".join(" ".join(map(str, t)) for t in census["textures_volume"]))
    p("- Cube textures (edge,levels,format,usage,pool,count): " + "; ".join(" ".join(map(str, t)) for t in census["textures_cube"]))
    p("- Render targets (w,h,format,msaa,count): " + "; ".join(" ".join(map(str, t)) for t in census["render_targets"]))
    p("- Depth-stencil surfaces: " + "; ".join(" ".join(map(str, t)) for t in census["depth_stencils"]))
    p(f"- Vertex buffers: {census['vertex_buffers']}; index buffers: {census['index_buffers']}.")
    p(f"- SetRenderTarget indices: {census['set_render_target_index']}; Clear flags: {census['clear_flags']}; StretchRect: {census['stretch_rect']}.")
    p(f"- Render states set ({len(census['render_states'])}): " + ", ".join(
        f"{k}({v['distinct_values']})" for k, v in census["render_states"].items()))
    p(f"- Sampler states set: " + ", ".join(f"{k}{[a for a, _ in v['top_values']]}" for k, v in census["sampler_states"].items()))
    p(f"- Sampler stages: {sorted(map(int, census['sampler_stages_used']))}; texture stages: {sorted(map(int, census['texture_stages_used']))}.")
    p(f"- Queries created: {census['queries_created']}; failed calls: {census['failed_calls'][:6]}.")
    p(f"\n(json: {out.relative_to(ROOT)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
