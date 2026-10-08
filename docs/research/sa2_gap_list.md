# SA2 optimisations Futari lacks (2026-10-07)

Read-only survey of sa2-nx (`D:\sonic adventure 2\sa2-nx`, "S") against futari-nx ("F"), the user's request "look at all
of SA2's cpu optimisations ... lets do that". Ranked by expected payoff on the A57 against effort. Port each behind its
own key (docs: enhancement-toggles), verify on the PC, console A/B with the in-run A/B where it is ring work.

## Top 10

1. **Direct calls lost to non-hook mentions** (DONE 2026-10-07): `tools/perf_codegen_post.py` `hooked()` counted every
   82/88xxxxxx address in app/src, comments and `hot_functions.txt` included; memset (8207DA20, 435 call sites),
   8207AE60, 8207AF40 kept their weak-alias calls (sa2-nx round 4 had the same trap: 264 sites). Now comments, data
   files and `FUTARI_HOT_RECORD_DECL` are not scanned (37 addresses fewer, all comment mentions).
2. **`sa2_cpu_vertices_base_zero_r0`** (renderer): r0-reading vertex shaders get the base-zero binding too, with
   `g_VertexIndexBase = vmin - first_vertex`. SA2: vertex buffer binds 1.00 -> 0.00 per draw (~1 us NVK macro each).
   S `futari_native_draws.cpp` ~l.829, ~3862-3874. Futari's console: "1.00 vkCmdBindVertexBuffers per draw".
   Futari tried a rect base-zero earlier and reverted it (reads_r0 always true): this is the r0 variant.
3. **Native memset**: SA2 `S\app\src\cpu\hot\n_827EFEA0.h` is byte-identical in Futari (FNV-1a 0x54D201DC8A21DABA over
   160 bytes): default 8207DA20; cores 88073BE0 (palm_normal, hirow_normal), 88073BD0 (reco_normal, kiniro_normal),
   88083BD0 (palm_abnormal). Futari's memcpy Site pattern.
4. **`sa2_cpu_ring_target_memo`**: 4-slot memo before GetTarget/GetDepth (S `futari_native_targets.cpp` ~l.105,
   3260, 7605). SA2 console 1.2 % + 2.2 % of the ring.
5. **`sa2_cpu_ring_compare_uploaded`**: the dedupe compares against the uploaded cached block (S draws ~l.838, 3676,
   3931, 7541). SA2 ~2 % + 0.9 % of the ring. Needs upload_memory = 1 (Futari has it on the Switch).
6. **`sa2_gpu_push_once`** (DONE 2026-10-07): push constants once per recorded state (S draws ~l.796).
7. **`fp_moves_modeless`** (codegen): no FPCR switch for float moves. SDK files: include/rex/codegen/config.h,
   src/codegen/config.cpp, builders/builder_context.h, context.cpp, floating_point.cpp, memory.cpp,
   include/rex/ppc/diet_ops.h. Futari: mostly the audio mixer (sub_821536C0: 6 enables / 14 disables).
8. **Codegen diet B2**: non_volatile_as_local + non_argument_as_local (+ keep list, emit_call_handing_non_arguments).
   SA2 -5.6 % instructions, -8.8 % loads/stores. Futari's blocker (indirect calls to share_registers functions) is NOT
   solved in SA2. Tools: S tools/read_before_write.py (vector-store fix), non_argument_audit.py, vec_shiftout.py,
   call_site_liveness.py. Large.
9. **LTO + -fipa-pta + hot-function ordering** (S app/CMakeLists.txt l.182-263, hot_functions.ld, tools/hot_order.py;
   traps: KEEP(*(.crt0)) first / _start at 0, -ffp-contract=off -fno-strict-aliasing at the link, XXH_FORCE_MEMORY_ACCESS=0).
   Cores (ld -r) stay out of LTO. Also -O0 for *_register.cpp, tools/check_switch_build.py.
10. **Localizer smart sync** (S tools/localize_native.py, 344 lines vs Futari's 141): calls sync only what the callee can
    reach; without it copies with calls were +13-18 % slower.

## Others

- Ring (small): sa2_cpu_ring_bound_plan_memo; identity_accept_fast, entry_hash, estimator_reason, bias_seen (low).
- Deferred recording: SA2 found it pixel-identical (47-frame fade); its review of Futari's fade difference: 
  vkUpdateDescriptorSetWithTemplate not drained, descriptor updates bypassing the proxy table would reorder.
- SDK: thread reaper + stack pool (S threading_posix.cpp ~l.826-1059): Futari's console logs show no C0000017 /
  "heap has NO a" (checked 2026-10-07): not needed so far.
- Native Direct3D layer (in progress here): sa2_api_draws / clears / ring_zero_ahead, shader pair memo; ARM lessons for
  EndDrawDirect too (pointer cursor, NEON rev32, no thread_local in hot paths: Futari has a thread_local std::vector in
  its repack path, draws.cpp ~l.3733).
- D3D float-constant writers (S cpu/hot/n_d3d_constants.h): byte-identical in Futari, default 8209AA70 / 8209A998.

## SA2 tried and dropped

Copies with calls without smart sync (slower); a native save/restore helper; fetch_offset_zero (NVK does it);
fingerprint sampling (missed texture changes: SA2 sets futari_native_fingerprints_sampling = 0, Futari still 8);
EDS3 a loss on NVK; copy engine slower; MSAA hangs NVK; half precision inexact; PC timings misled (round 5: -21 % PC,
-38 % console per draw).

## Watch list

- **sa2_gpu_vs_fetch_reorder**: PORTED 2026-10-07 at the user's request (futari_gpu_vs_fetch_reorder, on): see
  docs/futari-notes.md "Vertex shaders Direct3D reorders".

## GPU round (2026-10-08, DONE)

Every applicable sa2-nx GPU item ported with its own key (docs/futari-notes.md "GPU round"): blend_skip,
texcoords_unswapped, no_texture_signs, identity_remap, index_base_held (no hits: rectangle lists), pass_narrow_end,
query_reset_written, clear_merge (#8), resolve_draw, prewarm_current_keys, linear_fetch_not_resolved, and the
translator/shader_common.h (#18, #19). Not applicable: structured_cf, resolve_alias, edram4_clear_color0, conversion
budget, zcull_1x_depth, msaa / render_scale, constants_dedupe (dropped), dedupe_sync_epochs (have sync fences).
