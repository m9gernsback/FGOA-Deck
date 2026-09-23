# Mesa issue draft — zink: lower_bindless_instr OOB read on coord-less tex ops

---

**Title:** zink: crash in lower_bindless_instr on bindless textureSize (coord-less tex op reads tex->src[-1])

## Summary

`lower_bindless_instr()` in `src/gallium/drivers/zink/zink_compiler.c` crashes with an out-of-bounds read (`tex->src[-1]`) when a **bindless texture query instruction that has no coordinate source** (e.g. `textureSize()` → `nir_texop_txs`) is lowered.

The "pad coord" fix for Warhammer 40k (adding missing coordinate components) assumes every bindless tex instruction has a `nir_tex_src_coord` source. Query-style ops (`txs`, `lod`, `query_levels`, `texture_samples`) never have one, so `nir_tex_instr_src_index()` returns -1, which is then used as an unsigned array index.

## Affected code

Current main (line numbers from main @ 2026-09-22, same code in 25.3.0 at line 4404):

```c
/* rewrite bindless instructions as array deref instructions */
static bool
lower_bindless_instr(nir_builder *b, nir_instr *in, void *data)
{
   ...
      unsigned needed_components = glsl_get_sampler_coordinate_components(glsl_without_array(var->type));
      unsigned c = nir_tex_instr_src_index(tex, nir_tex_src_coord);   /* <-- -1 for txs */
      unsigned coord_components = nir_src_num_components(tex->src[c].src);  /* <-- tex->src[-1]: OOB */
      if (coord_components < needed_components) {
         nir_def *def = nir_pad_vector(b, tex->src[c].src.ssa, needed_components);
         nir_src_rewrite(&tex->src[c].src, def);
         tex->coord_components = needed_components;
      }
      return true;
```

## Suggested fix

Query-style instructions have no coord to pad, so skipping is semantically safe:

```c
      unsigned needed_components = glsl_get_sampler_coordinate_components(glsl_without_array(var->type));
      int c = nir_tex_instr_src_index(tex, nir_tex_src_coord);
      if (c >= 0) {
         unsigned coord_components = nir_src_num_components(tex->src[c].src);
         if (coord_components < needed_components) {
            nir_def *def = nir_pad_vector(b, tex->src[c].src.ssa, needed_components);
            nir_src_rewrite(&tex->src[c].src, def);
            tex->coord_components = needed_components;
         }
      }
      return true;
```

## Minimal reproducer

Any GLSL shader that calls `textureSize()` on a bindless sampler, run through zink, e.g. a vertex shader:

```glsl
#version 450
#extension GL_ARB_bindless_texture : require

layout(bindless_sampler) uniform sampler2D t;

void main()
{
    gl_Position = vec4(vec2(textureSize(t, 0)), 0.0, 1.0);
}
```

In the wild (Fate/Grand Order Arcade, where this was found), the sampler is constructed from a raw `uint64_t` handle: `textureSize(sampler2D(handle_bits), 0)` — both forms produce `nir_texop_txs` with a `nir_tex_src_texture_handle` source and **no** `nir_tex_src_coord` source.

## How it manifests

SIGSEGV while zink compiles the shader. In our case the compile runs on zink's `util_queue` worker thread (`cache_get_thread` / `optimized_compile_job`), so the process dies on a background thread mid-`glLinkProgram`, which made the failure look like a mysterious silent exit.

## Forensic evidence (from a coredump, Mesa 25.3.0, libgallium-25.3.0.so)

- Crash PC: `lower_bindless_instr+0x1b4`, instruction `mov 0x18(%rax),%r9`
- `rax = tex->src + 0x27ffffffd8`, where `0x27ffffffd8 = (uint64_t)(int)-1 * sizeof(nir_tex_src)` (0x28) — the compiler materialized the -1 index as a `movabs $0x27ffffffd8,%rcx` constant; fault address CR2 = rax+0x18
- Faulting object in memory: `nir_tex_instr` with `instr.type = nir_instr_type_tex` (3), `sampler_dim = GLSL_SAMPLER_DIM_2D` (1), `dest_type = nir_type_int32`, `op = nir_texop_txs` (8), `num_srcs = 3`, src types = { `nir_tex_src_texture_handle` (16), `nir_tex_src_sampler_handle` (17), `nir_tex_src_lod` (5) } — no coord source

## Environment where found

Mesa 25.3.0 (freedesktop flatpak runtime GL extension), zink on RADV (Steam Deck, RDNA2), OpenGL 4.6 game using `GL_ARB_bindless_texture` with `textureSize()` on a bindless sampler in the vertex stage.

Confirmed still present in main as of 2026-09-22.
