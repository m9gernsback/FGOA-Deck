# Mesa issue draft — cache-restored GL_UNIFORM resource with INACTIVE_UNIFORM_EXPLICIT_LOCATION crashes _mesa_program_get_resource_name

> 2026-09-23 二修：初版把崩点误判为 serialize.cpp 写路径；core 指令级复核后
> 真崩点 = `_mesa_program_get_resource_name`（shader_query.cpp），触发前提是
> 磁盘缓存命中恢复。本文为修正版。

---
**Title:** glsl/shader_query: SIGSEGV in _mesa_program_get_resource_name on cache-restored program whose GL_UNIFORM resource Data is INACTIVE_UNIFORM_EXPLICIT_LOCATION

## Summary

When the shader disk cache is enabled, a program restored from cache
(`LINKING_SKIPPED`) can have `ProgramResourceList[i].Data ==
INACTIVE_UNIFORM_EXPLICIT_LOCATION` (`(void*)-1`) for a `GL_UNIFORM` resource.
`st_link_shader()` unconditionally calls `_mesa_create_program_resource_hash()`
right after linking, which calls `_mesa_program_get_resource_name()` on every
resource; its `GL_UNIFORM` branch does `*out = RESOURCE_UNI(res)->name`
(24-byte struct copy) and dereferences `-1` → SIGSEGV (read at
`0xffffffffffffffff`) in a Mesa `util_queue` worker thread.

`MESA_SHADER_CACHE_DISABLE=true` fully avoids the crash (no cache → no
deserialize → sentinel never appears in the resource list).

## How the sentinel gets in (driver-independent, shared glsl/mesa code)

1. Program has explicit-location uniforms where `UniformRemapTable` ends up
   with `INACTIVE_UNIFORM_EXPLICIT_LOCATION` at a location that a *resource*
   references via `remap_location` (observed with a real-world game using
   heavy `layout(location=N)`; likely an active/inactive explicit-location
   collision, see gl_nir_linker.c:3019).
2. Cache write (fresh link) stores the resource as `uniform_remapped` +
   `remap_location` — legal.
3. Cache **read** (`read_program_resource_data`, serialize.cpp:1006) restores
   `res->Data = util_range_remap(remap_location, prog->UniformRemapTable)->ptr`,
   which legitimately yields the sentinel.
4. `_mesa_create_program_resource_hash()` (shader_query.cpp:2200) walks the
   restored list and crashes in `_mesa_program_get_resource_name()`.

Note the query side handles the sentinel everywhere else
(`uniform_query.cpp:263,1475,2050`; remap-table writers in
serialize.cpp:588/614) — only the resource-name accessors don't.

## Crash signature (coredump, Mesa 25.3.0, libgallium-25.3.0.so, radeonsi)

- PC: `libgallium+0x2f91d0`, `movdqu (%rax),%xmm0`; RAX = CR2 = `-1`
  (= `_mesa_program_get_resource_name` GL_UNIFORM case, jump-table idx 0 of the
  20-entry resource-type dispatch at function+0x25)
- Crash object: `gl_program_resource` array (stride 0x18); first entry
  `Type=0x92e1 (GL_UNIFORM)`, `Data=0xffffffffffffffff`; siblings valid
- Stack: `util_queue` worker → `st_link_shader` region →
  `_mesa_create_program_resource_hash` loop → fault
- Deterministic; same stop point every run (game blocks in
  `glLinkProgram`/`glGetProgramiv` status query while the worker dies)

## Suggested fix

Minimal (semantic: a nameless/inactive resource is skipped by both callers,
which already handle `false`):

```c
/* shader_query.cpp, _mesa_program_get_resource_name() */
   case GL_UNIFORM:
   case GL_BUFFER_VARIABLE:
      if (res->Type == GL_UNIFORM &&
          res->Data == INACTIVE_UNIFORM_EXPLICIT_LOCATION)
         return false;
      *out = RESOURCE_UNI(res)->name;
      return out->string != NULL;
```

Working patch vs 25.3.0: `deck-deploy/get-resource-name-sentinel-fix.patch`.

Adjacent latent issues worth fixing in the same MR:

- `_mesa_program_resource_name()` / `_mesa_program_resource_array_size()`
  dereference `RESOURCE_UNI(res)` the same way (reachable via GL program
  interface queries on a restored program).
- `write_program_resource_data()` (serialize.cpp) GL_UNIFORM branch derefs
  `->builtin`/`->name.string` — crashes if `glGetProgramBinary()` is called on
  a cache-restored program carrying the sentinel. Hardening patch:
  `deck-deploy/serialize-sentinel-hardening.patch` (adds a third
  `uniform_type` enum value so the sentinel round-trips).
- Alternatively (or additionally) `read_program_resource_data()` could skip
  restoring such resources entirely, matching fresh-link semantics (inactive
  uniforms never appear in a fresh resource list).

## Reproducer sketch

Needs a program whose `UniformRemapTable` contains the sentinel at a location
referenced by a live GL_UNIFORM resource, then a **second run** that hits the
disk cache (the crash is on the restore path, not the first link):

```glsl
/* VS */  layout(location=7) uniform vec4 used;   /* referenced */
/* FS */  layout(location=7) uniform vec4 dead;   /* never read -> inactive */
```

Link with cache enabled (run 1: populate), relaunch and link again
(run 2: `LINKING_SKIPPED` → hash rebuild → SIGSEGV).

## Environment

Mesa 25.3.0 (freedesktop flatpak GL extension), radeonsi (Steam Deck RDNA2
"VanGogh"), OpenGL 4.6 app (Fate/Grand Order Arcade via Wine). Also crashed
under zink at the same linklog stop point in earlier rounds, consistent with
shared glsl/mesa code. Confirmed present in main as of 2026-09-23.
