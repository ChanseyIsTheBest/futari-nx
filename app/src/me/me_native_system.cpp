// The native graphics system: an IGraphicsSystem that the app installs in config.graphics, so the Xenos emulation
// plugin is not loaded and no guest memory is watched. The game keeps its own Direct3D; a ring thread reads the PM4
// command list it writes, keeps a copy of the GPU registers, returns the read pointer, delivers interrupts, writes
// SCRATCH_REG/MEM_WRITE/EVENT_WRITE values the game waits for, and counts draws, copies and Swaps. Draws, resolves
// and Swaps are handed to the modules in src/native/futari (TargetsNative, DrawsVulkan, ShadersNative)
// with the native shader package (futari_shaders.mesp next to the executable).
//
// The app uses it when futari_renderer_native is true (see futari_app.h).

#include "me_native_system.h"
#include "futari_api.h"
#include "me_verify.h"
#include "native/futari_native.h"
#include "me_shader_identity.h"
#include "me_vertex_fetch_reorder.h"
#include "me_shader_candidate_policy.h"
#include "me_native_draw_extent_estimator.h"
#include "me_pm4_runs.h"
#include "me_object_table.h"
#include "me_record_table.h"
#include "me_native_ps_no_kill.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/cvar.h>
#if defined(__ARM_NEON)
#include "me_platform_compat.h"  // banjo-nx: NEON on aarch64, SIMDe elsewhere
#endif
#include <rex/sys_counters.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>

#include "futari/futari_deferred_recording.h"
#include "futari/futari_native_targets.h"
#include "futari/futari_native_draws.h"  // g_synchronizations_ring
#include "futari/futari_native_shaders.h"
#include "futari/futari_shader_library.h"

namespace futari::renderer {
extern const ShadersNative* g_active_library;  // me_futari_glue.cpp
}
namespace banjo::renderdoc {
bool Start(const char* path_template);  // banjo_renderdoc.cpp
bool End();
}
extern "C" uint32_t g_banjo_last_reject_cause;  // futari_native_draws.cpp (C linkage)

// banjo-nx: defined here (masseffect-nx defined it in me_audio_hooks.cpp, which hooks Mass Effect's audio).
REXCVAR_DEFINE_BOOL(futari_vblank_sleep_exact, true, "Futari",
                    "Native renderer: the vblank thread sleeps until the exact next vblank instead of 1 ms steps")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(banjo_renderdoc_capture_s, 0, "Nuts&Bolts/Graphics",
                     "Diagnostic: this many seconds after the renderer starts, ask RenderDoc (when its layer is loaded) "
                     "to capture one frame into capturas/rdc next to the exe (0 = off)");
REXCVAR_DEFINE_INT32(banjo_trace_frame_s, 0, "Nuts&Bolts/Graphics",
                     "Diagnostic: this many seconds after the renderer starts, log one whole frame: every resolve, "
                     "every render-target change and every draw that samples a resolve destination (0 = off)");

// Read by FutariApp::PatchCoalescedHashForLocalTesting (futari_app.h). The Switch has no environment
// variables: debug runs set it in futari.toml.
REXCVAR_DEFINE_STRING(futari_coalesced_sha1, "", "Futari",
                      "Local testing: SHA-1 (40 hex digits) of a modified Coalesced.ini (tools/coalesced.py, e.g. "
                      "no-startup-movies) accepted instead of the disc's; the same as FUTARI_COALESCED_SHA1")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// futari-nx: the Switch has no environment variables either, so the discovery dump of shaders the library lacks
// (FUTARI_SHADER_DISCOVERY on the PC) is a cvar there; futari_switch.toml turns it on.
REXCVAR_DEFINE_STRING(futari_shader_misses, "", "Futari",
                      "Folder (relative = next to the executable) where the native renderer writes the microcode of "
                      "every shader its library does not have, once each (<stage>_<hash>.ucode + .txt, input of "
                      "tools/wrap_discovered.py). Empty = off. FUTARI_SHADER_DISCOVERY takes precedence")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_shaders_index, true, "Futari",
                    "If futari_shaders.mesp.idx matches the package, load only that index (entry table and "
                    "original containers) and read each shader's SPIR-V from the package on first use; otherwise, or "
                    "with false, load the whole package as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_native_repeated_identity, true, "Futari",
                    "Native renderer: a shader IM_LOAD identical (same address and microcode) to the stage's current "
                    "program keeps its identification instead of hashing and matching again. false = always")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_native_edram4_several_rect, true, "Futari",
                    "Native renderer: auto-index rect lists of 2..8 rectangles (D3D Clear of several regions) are "
                    "proven per rectangle, so mode 4 skips the transfers they overwrite. false = only single rects")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_renderer_native, false,  /* banjo-nx: off until it draws Banjo correctly */ "Futari",
                    "Switch: use the native renderer (true) or ReXGlue's Xenos emulation (false, needs "
                    "gpu_plugin = \"xenos\")")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_native_edram4_stencil_inert, true, "Futari",
                    "EDRAM mode 4: a draw whose stencil test is ALWAYS and writes nothing counts as stencil-off "
                    "(full depth overwrite proof; lazy stencil kept where it is)");
REXCVAR_DEFINE_BOOL(futari_native_edram4_center_ogl, true, "Futari",
                    "EDRAM mode 4 proofs: honor OpenGL pixel centers (PA_SU_VTX_CNTL.pix_center) instead of always "
                    "assuming D3D centers (which lost the first column/row of full-screen passes)");
REXCVAR_DEFINE_BOOL(futari_native_edram4_ps_no_kill, true, "Futari",
                    "EDRAM mode 4 proofs: a pixel shader with no KILL anywhere (fetches and branches allowed) cannot "
                    "discard, not only straight-line ALU shaders");
REXCVAR_DEFINE_BOOL(futari_native_edram4_vs_alu, true, "Futari",
                    "EDRAM mode 4 proofs: the rectangle estimator admits vertex shaders that compute the position "
                    "with ALU math and (non-relative) constants, run on the live constant bank");
REXCVAR_DEFINE_BOOL(futari_native_edram4_quad_triangles, true, "Futari",
                    "EDRAM mode 4 proofs: a full-screen quad drawn as two triangles (6-vertex list / 4-vertex strip) "
                    "that are exactly an axis-aligned rectangle split on a diagonal is proven like a rectangle");
REXCVAR_DEFINE_BOOL(futari_native_edram4_clip_inside, true, "Futari",
                    "EDRAM mode 4 proofs: a rectangle drawn with clipping on (no user planes) and every vertex inside "
                    "the clip volume, with an XY viewport transform, is proven like an unclipped screen-space one");
REXCVAR_DEFINE_BOOL(futari_native_registers_table, true, "Futari",
                    "Draw records (game thread -> ring) in a fixed bucket table instead of a std::map: no "
                    "allocation per draw under the shared lock")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_native_fast_registers, true, "Futari",
                    "Ring: runs of register writes into the constant/fetch range skip the per-word special-register "
                    "checks and bump their generation once per run (exact)");
REXCVAR_DEFINE_BOOL(futari_native_identity_memo, true, "Futari",
                    "Shader identification memo by microcode content (type, XXH3, size): a program loaded again "
                    "after others skips the library matching; the draw-time identity guard is unchanged");
REXCVAR_DEFINE_BOOL(futari_native_edram4_exact_edges, true, "Futari",
                    "EDRAM mode 4: a proven rectangle whose edges lie exactly on pixel boundaries (D3D pixel "
                    "centers, no window offset, w = 1) bounds the draw by itself, without the 1-pixel pad; the pad "
                    "made syncs transfer whole tile rows/columns the draw never touches");
REXCVAR_DEFINE_BOOL(futari_native_edram4_overwrite, true, "Futari",
                    "EDRAM mode 4: skip the ownership transfer of tiles that a proven full-screen rectangle "
                    "overwrites completely (depth ALWAYS without stencil, or opaque full-mask color)");
// Ring-thread CPU switches: every one keeps the old path and checks itself against it for the first
// futari_native_verify_n uses (a DIFFERENCE line in the log and the old path for the rest of the session).
REXCVAR_DEFINE_BOOL(futari_native_flat_identity, false, "Futari",
                    "Ring: shader identity checks (VS fetch-patch match, PS microcode match) memoized in small "
                    "direct-mapped tables keyed by (library entry, load generation) instead of an unordered_map and a "
                    "word-by-word compare per use; exact (pure function of the loaded microcode and the entry)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_native_fast_pair, false, "Futari",
                    "Ring: draw/record pairing without the cold cache lines and locks: record start addresses in a "
                    "compact array (the best record of a packet is found in two 32-byte rows), object -> library entry "
                    "through a lock-free table, no draw-queue mutex per unpaired draw, prefetch while draining")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_native_raw_microcode, false, "Futari",
                    "Ring: IM_LOAD of the same address and size as the last load of that stage compares the guest "
                    "words against a raw copy of the last load (one memcmp) instead of byte-swapping them first; "
                    "exact (the swap is a bijection)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_native_pm4_fast, false, "Futari",
                    "Ring: runs of constant/fetch register writes (type 0, SET_CONSTANT2/SET_SHADER_CONSTANTS, "
                    "LOAD_ALU_CONSTANT) are byte-swapped, compared and stored four words at a time with NEON "
                    "straight from the guest words; same register contents and generation bumps")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DECLARE(bool, futari_cpu_verify);
// futari-nx test of futari_gpu_vs_fetch_reorder (2026-10-07): Futari's content has no vertex shader Direct3D loads with
// its fetches reordered, so the path is exercised by making some: a loaded vertex program has two of its fetches
// swapped when sa2-nx's proof says the swapped program computes the same (me_vertex_fetch_reorder.h), exactly what
// Direct3D's patcher does for such declarations. Every such load must then be identified by IdentifyReordered and draw
// the same pixels. A test switch, not an enhancement.
REXCVAR_DEFINE_BOOL(futari_diag_vs_reorder_swap, false, "Futari",
                    "Test: swap two fetches of loaded vertex shaders where the reorder proof allows it (what Direct3D's "
                    "patcher does for some declarations), to exercise futari_gpu_vs_fetch_reorder; draws must not change")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// futari-nx: three per-draw costs of the ring thread that the PC profiler found (caller attribution): a copy of the
// whole register file per rectangle list (ProveRectangleList), a fmt::format key per draw for the "bounded draw
// extent" diagnostic and one per declined EDRAM4 depth clear redirect. Same results and same log lines.
REXCVAR_DEFINE_BOOL(futari_shader_note_fast, false, "Futari",
                    "Game thread: NoteShaderObject without the diagnostic's mutex once its 32 lines are logged, and its "
                    "finished-object cache shared (atomics) instead of thread_local; same results")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(futari_pc_swap_limit_hz, 0, "Futari",
                     "Test runs: hold each guest swap of the native renderer to this rate (60 = console pacing on the "
                     "PC, whose present never blocks); 0 = off")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_ring_no_per_draw_copies, false, "Futari",
                    "Ring thread: no register-file copy per rectangle list, no string formatting per draw for the "
                    "extent / EDRAM4 diagnostics (counted by value, formatted at report time); same results and logs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);  // futari_enhancements.cpp
// futari-nx, the direct renderer step 1 (sa2-nx's sa2_cpu_direct_state, ported 2026-10-07): Direct3D's per-draw state
// packets reach the ring as a host record (one NOP packet in the stream instead of the packets to parse). See namespace
// direct below.
REXCVAR_DEFINE_BOOL(futari_cpu_direct_state, false, "Futari",
                    "Direct renderer: Direct3D's per-draw state packets become a host record the ring applies with the "
                    "same register functions at the same place in the stream (one NOP packet instead of the packets to "
                    "parse; self-checked)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);  // futari_enhancements.cpp
// futari-nx, the native Direct3D layer's ring of records (sa2-nx round 11, sa2_api_ring_zero_ahead): see
// ApiRecordPublish.
REXCVAR_DEFINE_BOOL(futari_api_ring_zero_ahead, false, "Futari",
                    "Native Direct3D layer: the ring of records' next 4 KB is allocated in the cache without a memory "
                    "read (ARM DC ZVA; a memset elsewhere) before the head reaches it (verify mode checks every block)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);  // futari_enhancements.cpp
// Measurement (banjo-nx's method: sequential A/B runs are useless while other ports' tests share the PC): one ring
// enhancement (futari_ab_key) alternates on and off every N seconds within one run, and the ring's work is split by
// phase. Not an enhancement: a test switch, 0 = off. Keys that support it: futari_cpu_direct_state,
// futari_ring_edram_memo, futari_ring_rectangle_fast (each asks AbOff with its own name).
REXCVAR_DEFINE_INT32(futari_ab_s, 0, "Futari",
                     "Measurement: futari_ab_key on and off every N seconds in the same run; the report line "
                     "'[native] A/B' gives the ring's work per draw of each phase (0 = off)")
    .range(0, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(futari_ab_key, "futari_cpu_direct_state", "Futari",
                      "Measurement: the enhancement futari_ab_s alternates")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Measurement (sa2-nx's census idea, 2026-10-07): what changes between two consecutive draws, by register group. It
// says how many draws differ from the previous one only in their vertices (VGT range and vertex fetch constants): the
// draws a batching step could merge. Off by default; a diagnostic (one register-file compare per draw).
// futari-nx (2026-10-07, build E console profile: PairDraw 1.7 % of the ring's self time): Futari's hooks make no
// Draw* records (NoteDrawCall), so every draw's record lookups (address table, replay map, queue) find nothing. While no
// record was ever made they are skipped: the same result (unpaired), checked by the record counter on every draw.
REXCVAR_DEFINE_BOOL(futari_ring_pairing_skip, false, "Futari",
                    "Ring thread: while the game has made no Draw* record at all, the per-draw record lookups (which can "
                    "only fail) are skipped; the draw is unpaired as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);  // futari_enhancements.cpp
// futari-nx (2026-10-07): the draw extent estimator interprets the vertex shader on the CPU for every rect-list draw
// (ProveRectangleList) and for the EDRAM height estimate (build E console profile: Estimate 1.8 % of the ring, plus
// the interpreter). Its walk over the microcode is a straight line whose state and decisions depend on the microcode
// and the configuration only (data and register checks can only reject earlier). A rejection in that walk therefore
// holds for every draw with the same loaded vertex shader: it is remembered until the next VS load
// (vs_identity_generation_). Futari: every rect-list proof was rejected ("fetch-format", "partial-position-export").
REXCVAR_DEFINE_BOOL(futari_ring_extent_memo, false, "Futari",
                    "Ring thread: a vertex shader the extent estimator rejects for a reason of its microcode alone is "
                    "not interpreted again until another vertex shader is loaded (the first 4096 skips, every one "
                    "with futari_cpu_verify, run the estimator and must reject too)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);  // futari_enhancements.cpp
REXCVAR_DEFINE_BOOL(futari_ring_sync_fences, false, "Futari",
                    "Ring thread: every guest-visible fence (EVENT_WRITE_SHD, MEM_WRITE, INTERRUPT) and WAIT_REG_MEM "
                    "ends the renderer's same-frame reuse by address (index cache, vertex dedupe): correctness "
                    "(banjo-nx's finding), at the cost of fewer reuses")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);  // futari_enhancements.cpp
REXCVAR_DEFINE_BOOL(futari_draw_census, false, "Futari",
                    "Measurement: count, per draw, which register groups changed since the previous draw "
                    "('[native] draw census' lines); costs a register-file compare per draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(futari_native_verify_n, 2048, "Futari",
                     "Self-check of the switches above: the old and the new path are both evaluated for the first N "
                     "uses and compared; any difference logs DIFFERENCE and turns that switch off. 0 = no check")
    .range(0, 2147483647)  // futari-nx: futari_cpu_verify checks every use
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(futari_native_vs_identity_tolerance, 0, "Futari",
                     "Experimental: a vertex shader the ring loaded may differ from its library candidate (same size, "
                     "fetches still checked) in up to N ALU/CF words and still be drawn with that candidate. 0 = "
                     "exact identity (the draw is dropped when no candidate matches, as before). Try 4-16 when the "
                     "log shows 'unresolved draw shader pairs' (e.g. VS CD057930742AFE84, 120 words, one draw per "
                     "frame in the pause menu); see the '[native] VS word diff' lines")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me::native {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;

// GPU MMIO window and register file.
constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kRegisterCount = 0x5003;
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegScratchUmsk = 0x01DC;
constexpr uint32_t kRegScratchAddr = 0x01DD;
constexpr uint32_t kRegScratch0 = 0x0578;
constexpr uint32_t kRegScratch7 = 0x057F;
constexpr uint32_t kRegCoherStatusHost = 0x0A31;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
// Display gamma ramp registers. Native presentation owns the Xenos output path, so it must mirror
// the table written by Direct3D just like the SDK command processor does.
constexpr uint32_t kRegGammaFirst = 0x1921;       // DC_LUT_RW_MODE
constexpr uint32_t kRegGammaIndex = 0x1922;       // DC_LUT_RW_INDEX
constexpr uint32_t kRegGammaSequential = 0x1923;  // DC_LUT_SEQ_COLOR
constexpr uint32_t kRegGamma30 = 0x1925;          // DC_LUT_30_COLOR
constexpr uint32_t kRegGammaMask = 0x1927;        // DC_LUT_WRITE_EN_MASK
constexpr uint32_t kRegGammaLast = 0x1927;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;
constexpr uint32_t kRegVgtEventInitiator = 0x21F9;
constexpr uint32_t kRegVgtDmaBase = 0x21FA;
constexpr uint32_t kRegVgtDmaSize = 0x21FB;
constexpr uint32_t kRegVgtDrawInitiator = 0x21FC;
constexpr uint32_t kRegRbModeControl = 0x2208;
constexpr uint32_t kRegRbSampleCountAddr = 0x2325;
constexpr int kMaxIndirectDepth = 4;
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);
constexpr uint16_t kExtentMax = 2048 >> 3;
// Registers whose change invalidates viewport/raster state.
constexpr uint32_t kRegViewportFirst = 0x2080;
constexpr uint32_t kRegViewportLast = 0x2302;
constexpr uint32_t kRegFetchFirst = 0x4800;
constexpr uint32_t kRegFetchLast = 0x48BF;
inline bool IsViewportRegister(uint32_t i) {
  if (i - kRegViewportFirst > kRegViewportLast - kRegViewportFirst) return false;
  return i <= 0x2082 || (i >= 0x210F && i <= 0x2114) || (i >= 0x2204 && i <= 0x2206) || i == 0x2302;
}

// Big-endian words of a command buffer; the ring wraps (power-of-two size), an indirect buffer
// does not.
struct Reader {
  const uint8_t* base = nullptr;
  uint32_t physical = 0;  // physical address of word 0 (to key packets by address)
  uint32_t mask = 0;
  uint32_t pos = 0;
  uint32_t end = 0;
  uint32_t Pending() const { return mask ? ((end - pos) & mask) : (end - pos); }
  uint32_t Peek() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t v = Peek();
    Advance(1);
    return v;
  }
  void Advance(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compare(uint32_t info, uint32_t value, uint32_t ref) {
  switch (info & 0x7) {
    case 0x0: return false;
    case 0x1: return value < ref;
    case 0x2: return value <= ref;
    case 0x3: return value == ref;
    case 0x4: return value != ref;
    case 0x5: return value >= ref;
    case 0x6: return value > ref;
    default: return true;
  }
}

// Physical microcode address -> library entry, filled on the game thread by NoteShaderObject and read
// by the ring thread on IM_LOAD.
std::mutex g_by_address_mutex;
std::unordered_map<uint32_t, const futari::renderer::ShaderEntry*> g_by_address;
std::unordered_set<uint32_t> g_objects_seen;
// Immediate loads (IM_LOAD_IMMEDIATE) carry no address: they are found by the XXH3 of the microcode
// (host order) of the objects the game binds, the VS one taken at draw time (already patched). Hashes
// that two different entries share are counted and dropped.
std::unordered_map<uint64_t, const futari::renderer::ShaderEntry*> g_by_code;
std::unordered_set<uint32_t> g_code_mapped;  // objects whose (current) microcode is in g_by_code
std::unordered_map<uint32_t, const futari::renderer::ShaderEntry*> g_entry_of_object;
uint64_t g_code_conflicts = 0;

// futari_native_fast_pair: g_entry_of_object (written by the game thread under g_by_address_mutex, never erased)
// mirrored in a lock-free table the ring reads without the mutex (me_object_table.h).
ObjectEntryTable<const futari::renderer::ShaderEntry*> g_objects;
inline void PublishObject(uint32_t object, const futari::renderer::ShaderEntry* entry) { g_objects.Publish(object, entry); }
inline bool LookupObject(uint32_t object, const futari::renderer::ShaderEntry*& entry) { return g_objects.Lookup(object, entry); }

// Draw records from the game thread (NoteDrawCall).
struct DrawRecord {
  uint32_t type, r5, r6, r7, vs, ps;
  uint32_t start = 0;  // physical address of Direct3D's command buffer write pointer before the call
};
// The ring runs several frames behind the game thread (~800 records measured), so records are never aged
// out; the window search drops the ones skipped when a later one matches.

// Index/vertex count the ring carries for `prims` primitives of a Xenos primitive type
// (sub_82228568 takes a primitive count and multiplies it, mullw at +175).
uint32_t CountForPrimitives(uint32_t type, uint32_t prims) {
  switch (type) {
    case 1: return prims;             // point list
    case 2: return prims * 2;         // line list
    case 3: return prims + 1;         // line strip
    case 4: return prims * 3;         // triangle list
    case 5: case 6: return prims + 2; // triangle fan, strip
    case 8: return prims * 3;         // rectangle list
    case 13: return prims * 4;        // quad list
    default: return prims;
  }
}
std::mutex g_draw_mutex;
std::deque<DrawRecord> g_draw_queue;
// The same records keyed by where Direct3D was about to write (device+48, the command buffer write
// pointer): the ring finds the record of a DRAW_INDX by its packet address, whatever the order.
std::map<uint32_t, DrawRecord> g_draw_by_address;
// The same index without allocations (futari_native_registers_table): 8 KB address buckets of 8 records;
// a lookup checks the packet's bucket and the previous one (records are at most 0x2000 below the packet).
// (me_record_table.h: the buckets, with a dense array of start addresses beside the slots for futari_native_fast_pair.)
DrawRecordTable<DrawRecord> g_records;
using DrawRecordSlot = DrawRecordTable<DrawRecord>::Slot;
bool RecordTable() {
  static const bool table = REXCVAR_GET(futari_native_registers_table);
  return table;
}
// Draw records from the game thread to the ring thread without the shared mutex (the record table is then
// owned by the ring thread alone): a bounded multi-producer queue (Vyukov) of (record, start). The ring
// drains it into g_record_table before every lookup. The ring runs a few frames behind (~800 records),
// so 16384 slots never fill in practice; a full queue makes the producer wait.
struct QueuedRecord {
  std::atomic<uint64_t> sequence{0};
  DrawRecord record;
  uint32_t start = 0;
};
constexpr uint64_t kRecordQueueSize = 16384;
std::vector<QueuedRecord> g_record_queue(kRecordQueueSize);
std::atomic<uint64_t> g_record_queue_tail{0};  // next slot to fill (producers)
uint64_t g_record_queue_head = 0;               // next slot to drain (ring thread only)
bool g_record_queue_ready = [] {
  for (uint64_t i = 0; i < kRecordQueueSize; ++i) g_record_queue[i].sequence.store(i, std::memory_order_relaxed);
  return true;
}();

bool g_record_fast = false;  // futari_native_fast_pair (set when the graphics system starts)

void InsertRecordInTable(DrawRecord&& record, uint32_t start) {
  g_records.Insert(std::move(record), start, g_record_fast);
}

void PushRecord(DrawRecord&& record, uint32_t start) {
  uint64_t pos = g_record_queue_tail.load(std::memory_order_relaxed);
  for (;;) {
    QueuedRecord& q = g_record_queue[pos & (kRecordQueueSize - 1)];
    const uint64_t seq = q.sequence.load(std::memory_order_acquire);
    if (seq == pos) {
      if (g_record_queue_tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
        q.record = std::move(record);
        q.start = start;
        q.sequence.store(pos + 1, std::memory_order_release);
        return;
      }
    } else if (seq < pos) {
      std::this_thread::yield();  // full: the ring thread has not drained yet
      pos = g_record_queue_tail.load(std::memory_order_relaxed);
    } else {
      pos = g_record_queue_tail.load(std::memory_order_relaxed);
    }
  }
}

// Ring thread: moves every queued record into the table, in order.
void DrainRecords() {
  for (;;) {
    QueuedRecord& q = g_record_queue[g_record_queue_head & (kRecordQueueSize - 1)];
    if (q.sequence.load(std::memory_order_acquire) != g_record_queue_head + 1) return;
    if (g_record_fast) {  // the next records were written by the game thread's core: start pulling their lines
      __builtin_prefetch(&g_record_queue[(g_record_queue_head + 1) & (kRecordQueueSize - 1)]);
      __builtin_prefetch(&g_record_queue[(g_record_queue_head + 2) & (kRecordQueueSize - 1)]);
    }
    InsertRecordInTable(std::move(q.record), q.start);
    q.sequence.store(g_record_queue_head + kRecordQueueSize, std::memory_order_release);
    ++g_record_queue_head;
  }
}
std::atomic<bool> g_native_active{false};
std::atomic<uint64_t> g_draw_records{0};

// futari_cpu_direct_state (the direct renderer, step 1: Direct3D's per-draw state without PM4 on the ring; sa2-nx's
// design, docs/direct-renderer.md there).
//
// Direct3D's draw functions flush the dirty state as PM4 packets (register runs, constants, fetch constants, the
// occasional shader load, alignment fillers) and then write the draw packet. Right after the original draw function
// returns, the hook (EndDrawDirect) decodes the packets it wrote before the draw packet into a host record: register
// runs already in host byte order, and the few other packets (shader loads, waits) as they were. The packets are then
// covered by one NOP packet whose payload names the record; the draw packet stays where it was, and so does every
// address Direct3D keeps. The ring applies the record when it reaches the NOP, with the same register functions the
// packets would have gone through, at the same place in the stream: the same register file at every draw.
//
// Left as it was (and processed as PM4, as before): a draw during which anything was submitted or the command buffer
// changed segment (the ring may already be reading those words), packets the converter does not know, predicated
// packets, a NOP with payload in the first three words (inline data another packet may point at).
//
// Checked: the first 4096 records (all of them in verify mode) are re-encoded and compared with the packets Direct3D
// wrote, word for word (SameAsWritten; DIFFERENCE: no more records), and the ring finds every record at its place in
// the order (a marker without its record: an error, and no more records). futari-nx addition: a command buffer range
// the GPU runs twice (predicated tiling replays; Futari has none so far) finds its records again, because the ring
// releases the records only at the frame's Swap; the report counts such replays.
namespace direct {
constexpr uint32_t kMagic = 0x46545244u;  // 'FTRD', first payload word of the marker NOP
// The native Direct3D layer's records (futari_api_*, src/me/futari_api_internal.h): Direct3D's work done natively, its
// ops written straight into this ring; the marker NOP [0xC0011000]['FTRA'][seq] sits in fresh command buffer space, so
// nothing lies under it (no region to check, no words to restore). Same ring, numbering and release as 'FTRD'.
constexpr uint32_t kApiMagic = 0x46545241u;  // 'FTRA'
constexpr uint32_t kRingWords = 1u << 21;  // 8 MB of records (the ring runs a few frames behind)
enum Op : uint32_t { kRun = 1, kRunSingle = 2, kType1 = 3, kType3 = 4 };
std::atomic<uint64_t> g_wptr_writes{0};   // guest writes of CP_RB_WPTR (WriteMmio)
// Set by the draw thread while it checks g_wptr_writes and writes a marker; a submission (WriteMmio) waits for it to
// clear before it publishes the write pointer. Either the draw thread sees the submission (and leaves its packets) or
// the submission sees the marker being written (and publishes it whole): seq_cst on both sides.
std::atomic<bool> g_marking{false};
std::unique_ptr<uint32_t[]> g_ring;       // records, word-indexed modulo kRingWords
std::atomic<uint64_t> g_head{0};          // producer: next free word (monotonic)
std::atomic<uint64_t> g_tail{0};          // consumer: first word not yet released
uint64_t g_seq = 0;                       // producer: records written
std::atomic<int32_t> g_checks_left{4096};  // records still carrying the original packets for the decoder's check
std::atomic<bool> g_failed{false};        // a check found a difference: no more records
// Statistics (report): converted draws and why the others were left.
std::atomic<uint64_t> g_converted{0}, g_words_covered{0}, g_skip_submitted{0}, g_skip_unknown{0}, g_skip_small{0},
    g_skip_nop_head{0}, g_skip_no_draw{0}, g_skip_segment{0}, g_skip_full{0}, g_checked{0}, g_check_differences{0},
    g_kick_waits{0}, g_calls{0}, g_skip_thread{0};
std::atomic<uint32_t> g_unknown_opcode{0};  // the last type-3 opcode that stopped a conversion (report)
// futari_api_*: ReserveApiRecord's open reservation (its first word, monotonic, and g_head then), and the statistics.
uint64_t g_api_reserved = ~0ull, g_api_reserved_head = ~0ull;
std::atomic<uint64_t> g_api_records{0}, g_api_record_words{0}, g_api_zero_checks{0}, g_api_zero_differences{0};
constexpr uint32_t kPad = 0xFFFFFFFFu;     // the rest of the ring's last words is unused: continue at word 0
// The draw thread's counters: one writer, a plain load and store (no read-modify-write per draw); only a report.
inline void Bump(std::atomic<uint64_t>& c, uint64_t n) { c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }

// futari_ab_s's measure of the ring's work. On the PC, the thread's own CPU cycles (QueryThreadCycleTime): the
// PC runs other ports' builds and tests at the same time, and wall time counts their preemptions too. On the Switch
// (the game alone on its cores), nanoseconds.
#if defined(_WIN32)
extern "C" __declspec(dllimport) int __stdcall QueryThreadCycleTime(void* thread, unsigned long long* cycles);
extern "C" __declspec(dllimport) void* __stdcall GetCurrentThread();
inline uint64_t AbTicks() {
  unsigned long long cycles = 0;
  QueryThreadCycleTime(GetCurrentThread(), &cycles);
  return cycles;
}
constexpr const char* kAbUnit = "kcycles";
constexpr double kAbScale = 1e3;
#else
inline uint64_t AbTicks() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}
constexpr const char* kAbUnit = "us";
constexpr double kAbScale = 1e3;
#endif

// futari_ab_s: 1 in the "off" half of each period of 2N seconds (the same clock on both threads).
inline int AbPhase(int seconds) {
  return int((std::chrono::steady_clock::now().time_since_epoch() / std::chrono::seconds(seconds)) & 1);
}

inline uint8_t* Guest(uint8_t* base, uint32_t address) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);  // the generated code's REX_PHYS_HOST_OFFSET
#else
  return base + address;
#endif
}

// The type-3 packets a record may carry before the draw packet: no effect but their own on the register file and the
// shader state, in stream order (the ring runs them through Type3 with their original words). Each handler read
// (2026-10-07): NOP, IM_LOAD / IM_LOAD_IMMEDIATE (shader state), SET_CONSTANT / SET_CONSTANT2 /
// SET_SHADER_CONSTANTS / LOAD_ALU_CONSTANT (register runs), INVALIDATE_STATE (nothing), WAIT_REG_MEM (a wait at the
// same place in the stream), REG_RMW (a register), EVENT_WRITE (VGT_EVENT_INITIATOR).
inline bool Carried(uint32_t opcode) {
  switch (opcode) {
    case xenos::PM4_NOP:
    case xenos::PM4_IM_LOAD:
    case xenos::PM4_IM_LOAD_IMMEDIATE:
    case xenos::PM4_SET_CONSTANT:
    case xenos::PM4_SET_CONSTANT2:
    case xenos::PM4_SET_SHADER_CONSTANTS:
    case xenos::PM4_LOAD_ALU_CONSTANT:
    case xenos::PM4_INVALIDATE_STATE:
    case xenos::PM4_WAIT_REG_MEM:
    case xenos::PM4_REG_RMW:
    case xenos::PM4_EVENT_WRITE:  // a register write (VGT_EVENT_INITIATOR)
      return true;
    default:
      return false;
  }
}
inline bool IsDraw(uint32_t opcode) {
  return opcode == xenos::PM4_DRAW_INDX || opcode == xenos::PM4_DRAW_INDX_2 || opcode == xenos::PM4_DRAW_INDX_BIN ||
         opcode == xenos::PM4_DRAW_INDX_2_BIN;
}

// A record: [0] its number, [1] its size in words, [2..4] the region's first three words as Direct3D wrote them (host
// order; the marker covers them in memory), then the operations.
constexpr uint32_t kHeader = 5;

// Re-encodes the record's operations as packets and compares them with the region's words (zero words, type-2 fillers
// and NOPs aside). word(p) is the host-order word at address p. The draw thread checks what Direct3D wrote (the
// decoder's check); the ring checks the region again when it applies the record (sasrt-nx found Direct3D patching
// packets after the draw call in its game: such a record would no longer match).
template <typename Word>
bool SameAsWritten(Word word, uint32_t first, uint32_t draw_at, const uint32_t* ops, size_t size) {
  size_t o = 0;
  for (uint32_t p = first; p < draw_at;) {
    const uint32_t h = word(p);
    const uint32_t type = h >> 30;
    if ((type == 0 && !h) || type == 2) { p += 4; continue; }
    if (type == 3 && ((h >> 8) & 0x7F) == xenos::PM4_NOP) { p += 4 * (2 + ((h >> 16) & 0x3FFF)); continue; }
    if (o >= size) return false;
    const uint32_t tag = ops[o], kind = tag & 0xFF, count = tag >> 8;
    if (type == 0) {
      const uint32_t n = ((h >> 16) & 0x3FFF) + 1;
      if (kind != (((h >> 15) & 1) ? kRunSingle : kRun) || count != n || ops[o + 1] != (h & 0x7FFF)) return false;
      for (uint32_t i = 0; i < n; ++i)
        if (ops[o + 2 + i] != word(p + 4 + 4 * i)) return false;
      o += 2 + n;
      p += 4 * (1 + n);
    } else if (type == 1) {
      if (kind != kType1 || ops[o + 1] != h || ops[o + 2] != word(p + 4) || ops[o + 3] != word(p + 8)) return false;
      o += 4;
      p += 12;
    } else {
      const uint32_t n = ((h >> 16) & 0x3FFF) + 1;
      if (kind != kType3 || count != n || ops[o + 1] != h || ops[o + 2] != (p & 0x1FFFFFFFu)) return false;
      for (uint32_t i = 0; i < n; ++i)  // the payload is kept in guest byte order
        if (__builtin_bswap32(ops[o + 3 + i]) != word(p + 4 + 4 * i)) return false;
      o += 3 + n;
      p += 4 * (1 + n);
    }
  }
  return o == size;
}
}  // namespace direct

uint64_t CodeHash(const uint8_t* big_endian, uint32_t words) {
  std::vector<uint32_t> host(words);
  for (uint32_t i = 0; i < words; ++i) host[i] = rex::memory::load_and_swap<uint32_t>(big_endian + i * 4);
  return XXH3_64bits(host.data(), host.size() * sizeof(uint32_t));
}

// Whole-library index by unmasked microcode (host order), unique hashes only: for immediate loads of
// shaders the game never binds through the device (Direct3D's own clear/copy shaders).
std::unordered_map<uint64_t, const futari::renderer::ShaderEntry*> g_library_by_code;

bool OriginalMicrocode(const futari::renderer::ShaderEntry& entry, uint32_t& virtual_size,
                       uint32_t& offset, uint32_t& bytes, std::vector<uint32_t>* words = nullptr) {
  if (!entry.shader || entry.shader->original.size() < 28) return false;
  const auto& o = entry.shader->original;
  virtual_size = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
  const uint32_t shader_header = rex::memory::load_and_swap<uint32_t>(o.data() + 24);
  if (virtual_size > o.size() || shader_header + 8 > virtual_size) return false;
  offset = rex::memory::load_and_swap<uint32_t>(o.data() + shader_header);
  bytes = rex::memory::load_and_swap<uint32_t>(o.data() + shader_header + 4);
  if (!bytes || (bytes & 3) || size_t(virtual_size) + offset + bytes > o.size()) return false;
  if (words) {
    words->resize(bytes / 4);
    for (size_t i = 0; i < words->size(); ++i) {
      (*words)[i] = rex::memory::load_and_swap<uint32_t>(o.data() + virtual_size + offset + i * 4);
    }
  }
  return true;
}

// Direct3D's clear/copy/video shaders are loaded with IM_LOAD_IMMEDIATE after link-time rewriting.
// Their original containers are in the library, but the exact ring microcode is not. Materialize a
// stable container from the closest register-linked template so the normal offline translator can add
// the variant on the next run. Package shaders (with a constant table) are deliberately not candidates.
void DumpMissingImmediate(const futari::renderer::ShadersNative& library, bool vertex,
                          const std::vector<uint32_t>& microcode) {
  const char* folder = std::getenv("FUTARI_SHADER_MISSING");
  if (!folder || !*folder || microcode.empty()) return;
  const uint64_t code_hash = XXH3_64bits(microcode.data(), microcode.size() * sizeof(uint32_t)) ^
                             (vertex ? 1ull : 0ull);
  static std::mutex mutex;
  static std::unordered_set<uint64_t> dumped;
  std::lock_guard<std::mutex> lock(mutex);
  if (!dumped.insert(code_hash).second) return;

  const futari::renderer::ShaderEntry* best = nullptr;
  uint64_t best_score = UINT64_MAX;
  uint32_t best_vsize = 0, best_offset = 0, best_bytes = 0, best_different = 0;
  for (uint32_t n = 0;; ++n) {
    const auto* candidate = library.PerNumber(n);
    if (!candidate) break;
    if (candidate->vertices != vertex || !candidate->binding_per_register) continue;
    uint32_t vsize = 0, offset = 0, bytes = 0;
    std::vector<uint32_t> original;
    if (!OriginalMicrocode(*candidate, vsize, offset, bytes, &original) ||
        original.size() != microcode.size()) {
      continue;
    }
    uint32_t different = 0;
    uint64_t bits = 0;
    for (size_t i = 0; i < original.size(); ++i) {
      const uint32_t delta = original[i] ^ microcode[i];
      different += delta != 0;
      bits += std::popcount(delta);
    }
    const uint64_t score = uint64_t(different) * 1000 + bits;
    if (score < best_score) {
      best = candidate;
      best_score = score;
      best_vsize = vsize;
      best_offset = offset;
      best_bytes = bytes;
      best_different = different;
    }
  }
  if (!best || !best->shader) {
    REXLOG_WARN("[native] no internal {} template for an immediate shader of {} words",
                vertex ? "VS" : "PS", microcode.size());
    return;
  }
  std::vector<uint8_t> container = best->shader->original;
  for (size_t i = 0; i < microcode.size(); ++i) {
    rex::memory::store_and_swap<uint32_t>(container.data() + best_vsize + best_offset + i * 4,
                                          microcode[i]);
  }
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const uint64_t h = XXH3_64bits(container.data(), container.size());
  const auto path = std::filesystem::path(folder) /
                    fmt::format("{}_{:016x}.bin", vertex ? "vs" : "ps", h);
  if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
    std::fwrite(container.data(), 1, container.size(), f);
    std::fclose(f);
    REXLOG_INFO("[native] immediate {} variant kept: {} (template n{}, {} of {} words changed)",
                vertex ? "VS" : "PS", path.string(), best->number, best_different,
                best_bytes / 4);
  }
}

// Lossless discovery artifact for every shader which the current library cannot identify. Unlike
// DumpMissingImmediate this deliberately does not borrow a container or guess an identity: the file
// is the exact big-endian microcode from the ring plus a small human-readable sidecar. It is input
// for instruction-aware classification of Direct3D's internal shaders.
// FUTARI_SHADER_DISCOVERY, else futari_shader_misses (relative to the executable); empty = no dump.
static std::string DiscoveryFolder() {
  if (const char* env = std::getenv("FUTARI_SHADER_DISCOVERY"); env && *env) return env;
  const std::string value = REXCVAR_GET(futari_shader_misses);
  if (value.empty()) return {};
  std::filesystem::path path(value);
  if (path.is_relative()) path = rex::filesystem::GetExecutableFolder() / path;
  return path.string();
}

void DumpRawMicrocode(bool vertex, uint32_t address, const std::vector<uint32_t>& microcode) {
  static const std::string folder_value = DiscoveryFolder();
  const char* const folder = folder_value.c_str();
  if (!*folder || microcode.empty()) return;
  const uint64_t hash = XXH3_64bits(microcode.data(), microcode.size() * sizeof(uint32_t));
  const uint64_t key = hash ^ (vertex ? 1ull : 0ull);
  static std::mutex mutex;
  static std::unordered_set<uint64_t> dumped;
  std::lock_guard<std::mutex> lock(mutex);
  if (!dumped.insert(key).second) return;

  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const std::string stem = fmt::format("{}_{:016x}", vertex ? "vs" : "ps", hash);
  const auto raw_path = std::filesystem::path(folder) / (stem + ".ucode");
  if (std::FILE* f = std::fopen(raw_path.string().c_str(), "wb")) {
    for (uint32_t word : microcode) {
      uint8_t bytes[4] = {uint8_t(word >> 24), uint8_t(word >> 16), uint8_t(word >> 8),
                          uint8_t(word)};
      std::fwrite(bytes, 1, sizeof(bytes), f);
    }
    std::fclose(f);
  }
  const auto text_path = std::filesystem::path(folder) / (stem + ".txt");
  if (std::FILE* f = std::fopen(text_path.string().c_str(), "w")) {
    std::fprintf(f, "stage=%s\naddress=%08X\nwords=%zu\nhash=%016llX\n", vertex ? "vs" : "ps",
                 address, microcode.size(), static_cast<unsigned long long>(hash));
    for (size_t i = 0; i < microcode.size(); ++i) std::fprintf(f, "%04zu %08X\n", i, microcode[i]);
    std::fclose(f);
  }
  REXLOG_INFO("[native] raw unidentified {} kept: {} ({} words)", vertex ? "VS" : "PS",
              raw_path.string(), microcode.size());
}

void BuildLibraryCodeIndex(const futari::renderer::ShadersNative& library) {
  std::unordered_set<uint64_t> shared;
  auto register_link_score = [](const futari::renderer::ShaderEntry* e) {
    if (!e || !e->binding_per_register || !e->shader) return uint32_t(0);
    // Current raw-PS wrappers carry all 16 register-linked interpolator mappings after the
    // 32-byte shader header. Older discovery wrappers stopped at the header, so XenosRecomp
    // initialized the missing inputs to zero (in particular UI alpha). Prefer the complete
    // wrapper when several containers contain byte-for-byte identical microcode.
    if (!e->vertices && e->shader->original.size() >= 28) {
      const auto& o = e->shader->original;
      const uint32_t vsize = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
      const uint32_t sh = rex::memory::load_and_swap<uint32_t>(o.data() + 24);
      if (sh <= vsize && vsize - sh >= 32 + 16 * 4) return uint32_t(3);
    }
    return uint32_t(2);
  };
  for (uint32_t n = 0;; ++n) {
    const futari::renderer::ShaderEntry* e = library.PerNumber(n);
    if (!e) break;
    if (!e->shader) continue;
    const auto& o = e->shader->original;
    // sa2-nx: which container each entry number is (XXH3 of the container = the vs_/ps_<hash>.bin name of the
    // dumped ones), to read the n<N> of the logs.
    REXLOG_INFO("[native] library n{}: {}{} container {:016x} ({} bytes)", n, e->vertices ? "VS" : "PS",
                e->binding_per_register ? " register-linked" : "", XXH3_64bits(o.data(), o.size()), o.size());
    if (o.size() < 28) continue;
    const uint32_t vsize = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
    const uint32_t sh = rex::memory::load_and_swap<uint32_t>(o.data() + 24);
    if (sh + 8 > vsize || vsize > o.size()) continue;
    const uint32_t off = rex::memory::load_and_swap<uint32_t>(o.data() + sh);
    const uint32_t bytes = rex::memory::load_and_swap<uint32_t>(o.data() + sh + 4);
    if (size_t(vsize) + off + bytes > o.size() || !bytes) continue;
    const uint64_t h = CodeHash(o.data() + vsize + off, bytes / 4) ^ (e->vertices ? 1 : 0);
    auto [slot, inserted] = g_library_by_code.emplace(h, e);
    if (!inserted) {
      shared.insert(h);
      if (register_link_score(e) > register_link_score(slot->second)) slot->second = e;
    }
  }
  // Hashes several containers share are kept (first entry): identical microcode reads the same
  // constant and fetch registers, only the constant table names differ.
  REXLOG_INFO("[native] library microcode index: {} entries, {} shared by more than one container",
              g_library_by_code.size(), shared.size());
}

const futari::renderer::ShaderEntry* ByCode(const std::vector<uint32_t>& microcode, bool vertex,
    const futari::renderer::ShaderEntry** observed = nullptr) {
  const uint64_t h = XXH3_64bits(microcode.data(), microcode.size() * sizeof(uint32_t));
  const futari::renderer::ShaderEntry* entry = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_by_address_mutex);
    auto it = g_by_code.find(h);
    if (it != g_by_code.end()) entry = it->second;
  }
  if (!entry) {
    auto it = g_library_by_code.find(h ^ (vertex ? 1 : 0));
    if (it != g_library_by_code.end()) entry = it->second;
  }
  if (observed) *observed = entry;  // Preserve rejected candidates for bounded diagnostics.
  if (!entry) return nullptr;
  // VS fetch-patch compatibility remains checked by IdentifyShader, unchanged.
  if (vertex) return entry->vertices ? entry : nullptr;
  if (!PixelShaderIdentityMatches(
      {ShaderIdentityStage::Pixel, microcode},
      {entry->vertices ? ShaderIdentityStage::Vertex : ShaderIdentityStage::Pixel,
       entry->microcode})) return nullptr;
  return entry;
}
std::atomic<const futari::renderer::ShadersNative*> g_library{nullptr};
// Vertex shaders seen only after Direct3D patched their microcode (sub_8222F1C8) are found by the
// virtual part of their container (header, constant table, vertex elements); hashes shared by more
// than one library entry are left out.
std::unordered_map<uint64_t, const futari::renderer::ShaderEntry*> g_vs_by_virtual;
bool g_vs_by_virtual_built = false;

void BuildVirtualIndex(const futari::renderer::ShadersNative* library) {
  std::unordered_set<uint64_t> shared;
  for (uint32_t n = 0;; ++n) {
    const futari::renderer::ShaderEntry* e = library->PerNumber(n);
    if (!e) break;
    if (!e->vertices || !e->shader || e->shader->original.size() < 8) continue;
    const auto& o = e->shader->original;
    const uint32_t vsize = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
    if (vsize > o.size()) continue;
    const uint64_t h = XXH3_64bits(o.data(), vsize);
    if (!g_vs_by_virtual.emplace(h, e).second) shared.insert(h);
  }
  for (uint64_t h : shared) g_vs_by_virtual.erase(h);
  g_vs_by_virtual_built = true;
}

const futari::renderer::ShaderEntry* ByAddress(uint32_t physical) {
  std::lock_guard<std::mutex> lock(g_by_address_mutex);
  auto it = g_by_address.find(physical);
  return it == g_by_address.end() ? nullptr : it->second;
}

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeGraphicsSystem() : registers_(kRegisterCount, 0) {}
  ~NativeGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) return X_STATUS_SUCCESS;
    app_context_ = app_context;
    if (!provider_) {
      // The XenosRecomp SPIR-V needs device features that must be requested at creation.
      rex::cvar::SetFlagByName("vulkan_native_shader_features", "true");
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
      if (!provider_) {
        REXLOG_ERROR("[native] could not create the Vulkan device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[native] could not create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[native] native graphics system: no Xenos emulation");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registered_) {
      mmio_registered_ = memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, this,
                                                        &MmioRead, &MmioWrite);
      if (!mmio_registered_) {
        REXLOG_ERROR("[native] could not register the GPU MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) return X_STATUS_SUCCESS;
    g_vs_identity_alu_tolerance.store(uint32_t(REXCVAR_GET(futari_native_vs_identity_tolerance)),
                                      std::memory_order_relaxed);
    identity_flat_ = REXCVAR_GET(futari_native_flat_identity);
    g_record_fast = pairing_fast_ = REXCVAR_GET(futari_native_fast_pair);
    raw_microcode_ = REXCVAR_GET(futari_native_raw_microcode);
    pm4_fast_ = REXCVAR_GET(futari_native_pm4_fast);
    check_identity_ = check_pairing_ = check_raw_ = check_pm4_ = check_objects_ =
        uint32_t(std::max<int32_t>(0, REXCVAR_GET(futari_native_verify_n)));
    verify_initial_ = check_identity_;
    if (identity_flat_ || pairing_fast_ || raw_microcode_ || pm4_fast_)
      REXLOG_INFO("[native] ring CPU switches: flat identity {}, fast pairing {}, raw microcode compare {}, fast PM4 "
                  "runs {}; self-check of the first {} uses of each", identity_flat_, pairing_fast_, raw_microcode_,
                  pm4_fast_, check_identity_);
    g_native_active.store(true, std::memory_order_release);
    const char* lib = std::getenv("FUTARI_SHADER_LIBRARY");
    const auto lib_path = lib && *lib ? std::filesystem::path(lib)
                                      : rex::filesystem::GetExecutableFolder() / "futari_shaders.mesp";
    if (shaders_.Load(lib_path, REXCVAR_GET(futari_shaders_index))) {
      futari::renderer::g_active_library = &shaders_;
      BuildLibraryCodeIndex(shaders_);
      g_library.store(&shaders_, std::memory_order_release);
      REXLOG_INFO("[native] shader library {}", lib_path.string());
    } else {
      REXLOG_WARN("[native] no shader library at {}: nothing will be drawn", lib_path.string());
    }
    start_ = last_report_ = Clock::now();
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU VSync native");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU ring native");
    ring_thread_->Create();
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[native] interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    ring_generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[native] ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_writeback_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    g_native_active.store(false, std::memory_order_release);
    if (active_.exchange(false)) {
      { std::lock_guard<std::mutex> lock(ring_mutex_); }
      ring_cv_.notify_all();
      if (ring_thread_) {
        ring_thread_->Wait(0, 0, 0, nullptr);
        ring_thread_.reset();
      }
      if (vblank_thread_) {
        vblank_thread_->Wait(0, 0, 0, nullptr);
        vblank_thread_.reset();
      }
      Report(true);
    }
    draw_extent_estimator_.reset();
    extent_diagnostics_.clear();
    targets_.reset();
    if (presenter_) {
      if (app_context_) app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- MMIO ---
  static uint32_t MmioRead(void*, void* ctx, uint32_t address) {
    return static_cast<NativeGraphicsSystem*>(ctx)->ReadMmio(address);
  }
  static void MmioWrite(void*, void* ctx, uint32_t address, uint32_t value) {
    static_cast<NativeGraphicsSystem*>(ctx)->WriteMmio(address, value);
  }

  uint32_t ReadMmio(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming: return 0x08100748;
      case kRegRbBcControl: return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus: return 1;
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return (std::min(uint32_t(mode.display_width), uint32_t(0x0FFF)) << 16) |
               std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      default: break;
    }
    return r < kRegisterCount ? registers_[r] : 0;
  }

  void WriteMmio(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      // futari_cpu_direct_state: every submission, whoever makes it. A draw hook only rewrites its own packets when
      // none happened during the draw call (the ring may already be reading a submitted segment); one being written
      // right now is published whole (g_marking).
      direct::g_wptr_writes.fetch_add(1, std::memory_order_seq_cst);
      if (direct::g_marking.load(std::memory_order_seq_cst)) [[unlikely]] {
        direct::g_kick_waits.fetch_add(1, std::memory_order_relaxed);
        for (uint32_t spin = 0; direct::g_marking.load(std::memory_order_seq_cst); ++spin) {
          if (spin > 64) rex::thread::MaybeYield();
        }
      }
      // Wake the ring thread only when it waits (an empty lock + notify on every kick cost the render
      // thread ~2 %). seq_cst on both sides (here and ring_waiting_ in RingLoop): either the ring thread's
      // predicate sees the new pointer or this load sees it waiting; its 5 ms timeout bounds any miss.
      write_pointer_.store(value, std::memory_order_seq_cst);
      if (ring_waiting_.load(std::memory_order_seq_cst)) {
        { std::lock_guard<std::mutex> lock(ring_mutex_); }
        ring_cv_.notify_one();
      }
    }
    if (r < kRegisterCount) registers_[r] = value;  // no side effects through MMIO
  }

  void NoteGammaRampWrite(uint32_t index, uint32_t value) {
    ++gamma_writes_;
    if (index == kRegGammaIndex) {
      gamma_component_ = 0;
    } else if (index == kRegGammaMask) {
      gamma_mask_ = value & 0b111;
    } else if (index == kRegGammaSequential) {
      const uint32_t i = registers_[kRegGammaIndex] & 0xFF;
      if (gamma_mask_ & (UINT32_C(1) << (2 - gamma_component_))) {
        gamma_ramp_[i][gamma_component_] = uint16_t((value >> 6) & 0x3FF);
        ++gamma_version_;
      }
      if (++gamma_component_ >= 3) {
        gamma_component_ = 0;
        registers_[kRegGammaIndex] =
            (registers_[kRegGammaIndex] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
      }
    } else if (index == kRegGamma30) {
      const uint32_t i = registers_[kRegGammaIndex] & 0xFF;
      if (gamma_mask_ & 0b001) gamma_ramp_[i][2] = uint16_t(value & 0x3FF);
      if (gamma_mask_ & 0b010) gamma_ramp_[i][1] = uint16_t((value >> 10) & 0x3FF);
      if (gamma_mask_ & 0b100) gamma_ramp_[i][0] = uint16_t((value >> 20) & 0x3FF);
      if (gamma_mask_) ++gamma_version_;
      gamma_component_ = 0;
      registers_[kRegGammaIndex] =
          (registers_[kRegGammaIndex] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
    }
  }

  // --- Registers written by the ring ---
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) return;
    if (index == kRegCoherStatusHost) value |= 0x80000000u;
    if (registers_[index] != value) {  // generations: what a draw must re-upload
      if (index >= 0x4000 && index < 0x4800) {
        ++(index < 0x4400 ? gen_constants_vs_ : gen_constants_ps_);
      } else if (index >= kRegFetchFirst && index <= kRegFetchLast) {
        ++gen_fetch_;
      } else if (IsViewportRegister(index)) {
        ++gen_viewport_;
      }
    }
    if (index >= kRegGammaFirst && index <= kRegGammaLast) {
      NoteGammaRampWrite(index, value);
    }
    registers_[index] = value;
    if (index >= kRegScratch0 && index <= kRegScratch7) {
      const uint32_t n = index - kRegScratch0;
      if ((1u << n) & registers_[kRegScratchUmsk]) {
        rex::memory::store_and_swap<uint32_t>(
            memory_->TranslatePhysical(registers_[kRegScratchAddr] + n * 4), value);
      }
    }
  }

  // A run of register words into the constant/fetch range (0x4000-0x48BF), the bulk of every draw's writes:
  // no per-word special-register checks, and each generation bumped once if any word of its part changed.
  template <typename Next>
  void WriteConstantRun(uint32_t index, uint32_t count, Next next) {
    if (!REXCVAR_GET(futari_native_fast_registers) || index < 0x4000 ||
        uint64_t(index) + count > uint64_t(kRegFetchLast) + 1) {
      for (uint32_t i = 0; i < count; ++i) WriteRegister(index + i, next());
      return;
    }
    bool vs = false, ps = false, fetch = false;
    uint32_t* regs = registers_.data();
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t r = index + i, v = next();
      if (regs[r] != v) {
        regs[r] = v;
        if (r < 0x4400) vs = true;
        else if (r < 0x4800) ps = true;
        else fetch = true;
      }
    }
    if (vs) ++gen_constants_vs_;
    if (ps) ++gen_constants_ps_;
    if (fetch) ++gen_fetch_;
  }

  // WriteConstantRun over host-order words (futari_cpu_direct_state's records): each part of the constant/fetch range
  // (VS constants, PS constants, fetch constants) compared and copied whole, its generation bumped once if it changed,
  // as WriteConstantRun does word by word. Outside that range (or with the fast path off) every word goes through
  // WriteRegister, as there.
  void WriteConstantRunHost(uint32_t index, uint32_t count, const uint32_t* v) {
    if (!REXCVAR_GET(futari_native_fast_registers) || index < 0x4000 ||
        uint64_t(index) + count > uint64_t(kRegFetchLast) + 1) {
      for (uint32_t i = 0; i < count; ++i) WriteRegister(index + i, v[i]);
      return;
    }
    uint32_t* regs = registers_.data();
    while (count) {
      const uint32_t part_end = index < 0x4400 ? 0x4400u : index < 0x4800 ? 0x4800u : kRegFetchLast + 1;
      const uint32_t n = std::min(count, part_end - index);
      if (std::memcmp(regs + index, v, size_t(n) * 4) != 0) {
        std::memcpy(regs + index, v, size_t(n) * 4);
        ++(index < 0x4400 ? gen_constants_vs_ : index < 0x4800 ? gen_constants_ps_ : gen_fetch_);
      }
      index += n;
      v += n;
      count -= n;
    }
  }

  // The count guest words (big-endian, as the Reader sees them) at the reader position, if they are contiguous in
  // memory (indirect buffers never wrap; the ring only at its end).
  static const uint32_t* ContiguousWords(const Reader& data, uint32_t count) {
    if (data.mask && uint64_t(data.pos) + count > uint64_t(data.mask) + 1) return nullptr;
    return reinterpret_cast<const uint32_t*>(data.base + size_t(data.pos) * 4);
  }

  // WriteConstantRun over guest words (big-endian) in memory: the constant/fetch range (0x4000-0x48BF) is compared and
  // stored four words at a time; each generation is bumped once if any word of its part changed. Outside that range
  // (or with the fast path off) every word goes through WriteRegister as before.
  void WriteConstantRunRaw(uint32_t index, uint32_t count, const uint32_t* guest) {
    if (!REXCVAR_GET(futari_native_fast_registers) || index < 0x4000 ||
        uint64_t(index) + count > uint64_t(kRegFetchLast) + 1) {
      for (uint32_t i = 0; i < count; ++i) WriteRegister(index + i, __builtin_bswap32(guest[i]));
      return;
    }
    uint32_t* regs = registers_.data();
    std::vector<uint32_t> before;
    const bool check = check_pm4_ > 0;
    if (check) {
      --check_pm4_;
      before.assign(regs + index, regs + index + count);
    }
    __builtin_prefetch(guest + count);
    __builtin_prefetch(guest + count + 16);
    const RunChange change = ApplyRunRaw(regs, index, count, guest);
    if (change.vs) ++gen_constants_vs_;
    if (change.ps) ++gen_constants_ps_;
    if (change.fetch) ++gen_fetch_;
    if (check) {  // the original loop on the saved words
      std::vector<uint32_t> expected(before);
      uint32_t i = 0;
      const RunChange want = ApplyRunReference(expected.data(), index, count,
                                               [&] { return __builtin_bswap32(guest[i++]); });
      if (!(want == change) || !std::equal(expected.begin(), expected.end(), regs + index)) {
        ::me::native::VerifyDifference();
        REXLOG_ERROR("[native] DIFFERENCE: fast PM4 register run (index {:04X} count {}): generations vs {}/{} ps {}/{} "
                     "fetch {}/{} or values differ; the old path from now on", index, count, change.vs, want.vs, change.ps,
                     want.ps, change.fetch, want.fetch);
        pm4_fast_ = false;
        for (uint32_t k = 0; k < count; ++k) regs[index + k] = __builtin_bswap32(guest[k]);
      }
    }
  }

  uint32_t ReadMemory(uint32_t address) const {
    uint32_t v;
    std::memcpy(&v, memory_->TranslatePhysical(address & ~3u), 4);
    return xenos::GpuSwap(v, static_cast<xenos::Endian>(address & 0x3));
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    const uint32_t v = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~3u), &v, 4);
  }

  // --- Threads ---
  void Interrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) return;
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
    interrupts_.fetch_add(1, std::memory_order_relaxed);
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    uint64_t last_swap = rex::kernel::xboxkrnl::GuestSwapCount();
    while (active_.load(std::memory_order_acquire)) {
      if (rex::kernel::xboxkrnl::VblankPerSwap()) {  // deterministic runs (cvar vblank_per_swap, see the SDK)
        if (rex::kernel::xboxkrnl::WaitSwapForVblank(last_swap, active_)) {
          counter_.fetch_add(1, std::memory_order_relaxed);
          Interrupt(0, 2);
        }
        next = Clock::now() + interval;
        continue;
      }
      const auto now = Clock::now();
      if (now - next > std::chrono::milliseconds(250)) next = now;
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      if (REXCVAR_GET(futari_vblank_sleep_exact)) {
        // Sleep to the next vblank instead of waking up 1000 times a second (60 of them useful).
        const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(next - Clock::now());
        rex::thread::Sleep(std::clamp(wait, std::chrono::microseconds(200), std::chrono::microseconds(50000)));
      } else {
        rex::thread::Sleep(std::chrono::milliseconds(1));
      }
      REX_SYS_COUNT(rex::syscount::kVblankTick, 1);
    }
    return 0;
  }

  int RingLoop() {
    uint32_t read = 0;
    uint32_t generation = ring_generation_.load(std::memory_order_acquire);
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_waiting_.store(true, std::memory_order_seq_cst);
        ring_cv_.wait_for(lock, std::chrono::milliseconds(5), [&] {
          const uint32_t words = ring_words_.load(std::memory_order_acquire);
          return !active_.load(std::memory_order_acquire) ||
                 (words && (write_pointer_.load(std::memory_order_seq_cst) & (words - 1)) != read);
        });
        ring_waiting_.store(false, std::memory_order_relaxed);
      }
      if (!active_.load(std::memory_order_acquire)) break;
      Report(false);
      const uint32_t gen = ring_generation_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read = 0;
      }
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) continue;
      const uint32_t written = write_pointer_.load(std::memory_order_acquire) & (words - 1);
      if (written == read) continue;
      Reader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.physical = base;
      reader.mask = words - 1;
      reader.pos = read;
      reader.end = written;
      const auto work_start = Clock::now();  // futari-nx (sa2-nx's ring timer): the ring's own work per Swap
      const uint64_t wait_before = ring_wait_ns_, draws_before = draws_;
      const uint64_t ticks_before = direct_ab_s_ > 0 ? direct::AbTicks() : 0;
      while (reader.Pending()) {
        if (!Packet(reader, 0)) break;  // partial packet: the rest comes with the next WPTR
      }
      const uint64_t work_ns =
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - work_start).count());
      ring_busy_ns_ += work_ns;
      if (direct_ab_s_ > 0) [[unlikely]] {  // futari_ab_s: this stretch's work goes to its phase
        const int phase = direct::AbPhase(direct_ab_s_);
#if defined(_WIN32)
        ab_ns_[phase] += direct::AbTicks() - ticks_before;  // cycles: the waits' sleeps cost none
        (void)wait_before;
#else
        ab_ns_[phase] += direct::AbTicks() - ticks_before - (ring_wait_ns_ - wait_before);
#endif
        ab_draws_[phase] += draws_ - draws_before;
      }
      read = reader.pos;
      const uint32_t writeback = read_writeback_.load(std::memory_order_acquire);
      if (writeback) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), read);
      }
      NotifyRingProgress();
      futari::renderer::deferred::Flush();  // Batched mode: the worker must not wait for the next WPTR
    }
    return 0;
  }

  // --- PM4 ---
  bool Packet(Reader& reader, int depth) {
    const uint32_t packet = reader.Peek();
    uint32_t count = 0;
    switch (packet >> 30) {
      case 0: count = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0; break;
      case 1: count = 2; break;
      case 2: count = 0; break;
      default: count = ((packet >> 16) & 0x3FFF) + 1; break;
    }
    if (reader.Pending() < count + 1) return false;
    if (pm4_fast_) {  // the stream was written by the game thread's core: pull the lines ahead of the parse
      const uint32_t ahead = reader.pos + count + 1 + 48;
      __builtin_prefetch(reader.base + size_t(reader.mask ? (ahead & reader.mask) : ahead) * 4);
    }
    packet_address_ = reader.physical + reader.pos * 4;
    reader.Advance(1);
    Reader data = reader;
    reader.Advance(count);
    ++packets_;
    if (!packet) return true;
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool single = (packet >> 15) & 0x1;
        ++type0_packets_;
        type0_words_ += count;
        type0_single_ += single;
        if (single) {
          for (uint32_t i = 0; i < count; ++i) WriteRegister(index, data.Read());
        } else if (const uint32_t* words = pm4_fast_ ? ContiguousWords(data, count) : nullptr) {
          WriteConstantRunRaw(index, count, words);
        } else {
          WriteConstantRun(index, count, [&] { return data.Read(); });
        }
        break;
      }
      case 1: {
        const uint32_t v1 = data.Read(), v2 = data.Read();
        WriteRegister(packet & 0x7FF, v1);
        WriteRegister((packet >> 11) & 0x7FF, v2);
        break;
      }
      case 2: break;
      default: Type3(data, packet, count, depth); break;
    }
    return true;
  }

  static uint32_t ConstantBase(uint32_t type) {
    switch (type) {
      case 0: return 0x4000;  // ALU
      case 1: return 0x4800;  // fetch
      case 2: return 0x4900;  // bool
      case 3: return 0x4908;  // loop
      case 4: return 0x2000;  // registers
      default: return UINT32_MAX;
    }
  }

  void Type3(Reader& data, uint32_t packet, uint32_t words, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    ++opcodes_[opcode];
    opcode_words_[opcode] += words;
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      // A draw the predicate drops still had its Draw* record: consume it so the pairing stays in step.
      if ((opcode == xenos::PM4_DRAW_INDX || opcode == xenos::PM4_DRAW_INDX_2) &&
          words >= (opcode == xenos::PM4_DRAW_INDX ? 2u : 1u)) {
        if (opcode == xenos::PM4_DRAW_INDX) data.Read();
        const uint32_t initiator = data.Read();
        const uint32_t saved = registers_[kRegVgtDrawInitiator];
        registers_[kRegVgtDrawInitiator] = initiator;
        PairDraw();
        registers_[kRegVgtDrawInitiator] = saved;
        ++draws_predicated_;
      }
      return;
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        NoteGuestSync();
        if (words < 1) break;
        const uint32_t cpus = data.Read();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) Interrupt(1, cpu);
        }
        break;
      }
      case xenos::PM4_XE_SWAP:
        if (words >= 4) {
          data.Read();
          data.Read();  // frontbuffer
          swap_width_ = data.Read();
          swap_height_ = data.Read();
        }
        ++swaps_;
        ReleaseDirectRecords();  // futari_cpu_direct_state: this frame's records are done (replays included)
        NoteGuestSync();
        counter_.fetch_add(1, std::memory_order_relaxed);
        TraceSwap();
        Present();
        LimitSwaps();
        break;
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (words < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        if (depth < kMaxIndirectDepth) {
          Reader r;
          r.base = memory_->TranslatePhysical(address);
          r.physical = address;
          r.end = length;
          while (r.Pending()) {
            if (!Packet(r, depth + 1)) break;
          }
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        NoteGuestSync();
        if (words < 5) break;
        const uint32_t info = data.Read(), poll = data.Read(), ref = data.Read(), mask = data.Read();
        const auto wait_start = Clock::now();
        const auto limit = wait_start + kWaitRegMemMax;
        // The time spent waiting for the game (not ring work), from the first unmet check (sa2-nx's ring timer).
        struct WaitTime {
          uint64_t& total;
          std::chrono::steady_clock::time_point start;
          bool waited = false;
          ~WaitTime() {
            if (waited)
              total += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
          }
        } wait_time{ring_wait_ns_, wait_start};
        for (;;) {
          if (!(info & 0x10) && poll == kRegCoherStatusHost && (registers_[poll] & 0x80000000u)) {
            registers_[poll] = 0;  // MakeCoherent: no shared memory to synchronize
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (Compare(info, value & mask, ref)) break;
          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            if (waits_timed_out_++ == 0) {
              REXLOG_WARN("[native] WAIT_REG_MEM not met ({} {:08X} ref {:08X} mask {:08X})",
                          (info & 0x10) ? "memory" : "register", poll, ref, mask);
            }
            break;
          }
          wait_time.waited = true;
          rex::thread::Sleep(std::chrono::microseconds(50));
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (words < 3) break;
        const uint32_t info = data.Read(), and_v = data.Read(), or_v = data.Read();
        uint32_t v = Register(info & 0x1FFF);
        v &= ((info >> 31) & 0x1) ? Register(and_v & 0x1FFF) : and_v;
        v |= ((info >> 30) & 0x1) ? Register(or_v & 0x1FFF) : or_v;
        WriteRegister(info & 0x1FFF, v);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        NoteGuestSync();  // sasrt-nx: every guest-visible write
        if (words < 2) break;
        const uint32_t reg = data.Read(), address = data.Read();
        WriteMemory(address, Register(reg));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        NoteGuestSync();
        if (words < 1) break;
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < words; ++i, address += 4) WriteMemory(address, data.Read());
        break;
      }
      case xenos::PM4_COND_WRITE: {
        NoteGuestSync();  // sasrt-nx: every guest-visible write
        if (words < 6) break;
        const uint32_t info = data.Read(), poll = data.Read(), ref = data.Read(), mask = data.Read();
        const uint32_t dest = data.Read(), value = data.Read();
        const uint32_t v = (info & 0x10) ? ReadMemory(poll) : Register(poll);
        if (Compare(info, v & mask, ref)) {
          if (info & 0x100) {
            WriteMemory(dest, value);
          } else {
            WriteRegister(dest, value);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (words < 1) break;
        WriteRegister(kRegVgtEventInitiator, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        NoteGuestSync();
        if (words < 3) break;
        const uint32_t initiator = data.Read(), address = data.Read(), value = data.Read();
        WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1) ? counter_.load(std::memory_order_relaxed)
                                                       : value);
        // The D3D fence word the game's wait loop compares (sub_8222C768 / sub_8222FA98): wake it now, not
        // at the end of the ring segment.
        NotifyRingProgress();
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        NoteGuestSync();  // sasrt-nx: every guest-visible write
        if (words < 2) break;
        const uint32_t initiator = data.Read(), address = data.Read();
        WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
        const uint16_t extent[] = {0, kExtentMax, 0, kExtentMax, 0, 1};
        uint8_t* dest = memory_->TranslatePhysical(address & ~3u);
        for (size_t i = 0; i < std::size(extent); ++i) {
          rex::memory::store_and_swap<uint16_t>(dest + i * 2, extent[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        NoteGuestSync();  // sasrt-nx: every guest-visible write
        if (words < 1) break;
        WriteRegister(kRegVgtEventInitiator, data.Read() & 0x3F);
        // D3D9 occlusion queries on Xbox 360:
        // Issue(D3DISSUE_BEGIN): RB_SAMPLE_COUNT_ADDR points to query->begin (+0x20).
        // Issue(D3DISSUE_END):   RB_SAMPLE_COUNT_ADDR points to query->end   (+0x00).
        // D3D places the byte-swapped sentinel 0xFFFFFEED in the end structure before EVENT_WRITE_ZPD.
        const uint32_t address = Register(kRegRbSampleCountAddr);
        if (address) {
          auto* counts =
              memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
          const uint32_t finished = rex::byte_swap(uint32_t(0xFFFFFEED));
          const bool end = counts->ZPass_A == finished || counts->ZPass_B == finished ||
                           counts->ZFail_A == finished || counts->ZFail_B == finished;
          std::memset(counts, 0, sizeof(*counts));
          // SDK fallback: begin = 0, end = 1000 samples.
          if (end) counts->Total_A = counts->ZPass_A = 1000;
          static uint32_t query_reports = 0;
          if (query_reports++ < 30) {
            REXLOG_INFO("[native] ZPD {} at {:08X}: {} samples", end ? "end" : "begin", address,
                        uint32_t(counts->ZPass_A));
          }
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (words < 1) break;
        const uint32_t type_index = data.Read();
        const uint32_t base = ConstantBase((type_index >> 16) & 0xFF);
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 1; i < words; ++i) WriteRegister(index + i - 1, data.Read());
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        if (words < 1) break;
        const uint32_t index = data.Read() & 0xFFFF;
        if (const uint32_t* raw = pm4_fast_ ? ContiguousWords(data, words - 1) : nullptr) {
          WriteConstantRunRaw(index, words - 1, raw);
        } else {
          WriteConstantRun(index, words - 1, [&] { return data.Read(); });
        }
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        if (words < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t n = data.Read() & 0xFFF;
        const uint32_t base = ConstantBase((type_index >> 16) & 0xFF);
        if (base != UINT32_MAX) {
          const uint8_t* src = memory_->TranslatePhysical(address);
          const uint32_t index = base + (type_index & 0x7FF);
          uint32_t i = 0;
          if (pm4_fast_) {
            WriteConstantRunRaw(index, n, reinterpret_cast<const uint32_t*>(src));
          } else {
            WriteConstantRun(index, n, [&] { return rex::memory::load_and_swap<uint32_t>(src + size_t(i++) * 4); });
          }
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (words >= 1) bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (words >= 1) bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (words >= 1) bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (words >= 1) bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK:
        if (words >= 2) {
          const uint64_t hi = data.Read(), lo = data.Read();
          bin_mask_ = (hi << 32) | lo;
        }
        break;
      case xenos::PM4_SET_BIN_SELECT:
        if (words >= 2) {
          const uint64_t hi = data.Read(), lo = data.Read();
          bin_select_ = (hi << 32) | lo;
        }
        break;
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        const uint32_t skip = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (words < skip + 1) break;
        if (skip) data.Read();
        const uint32_t initiator = data.Read();
        WriteRegister(kRegVgtDrawInitiator, initiator);
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA) && words >= skip + 3) {
          WriteRegister(kRegVgtDmaBase, data.Read());
          WriteRegister(kRegVgtDmaSize, data.Read());
        }
        if ((Register(kRegRbModeControl) & 0x7) == uint32_t(xenos::EdramMode::kCopy)) {
          ++copies_;
          resolve_destinations_.insert(Register(rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE) & 0x1FFFF000u);
          if (trace_phase_ == 1) TraceCopy();
          Copy();
        } else {
          ++draws_;
          if (depth > 0) ++draws_indirect_;
          PairDraw();
          if (census_) [[unlikely]] Census();
          trace_result_ = -1;
          if (draw_vs_ && (draw_ps_ || (Register(kRegRbModeControl) & 0x7) == 5)) {
            Draw();
          } else {
            ++draws_unidentified_;
            ++missing_draw_pairs_[{draw_vs_ ? 0 : current_vs_hash_, draw_ps_ ? 0 : current_ps_hash_}];
          }
          if (trace_phase_ == 1) TraceDraw();
        }
        break;
      }
      case xenos::PM4_IM_LOAD: {
        if (words < 2) break;
        const uint32_t address_type = data.Read();
        const uint32_t size = data.Read() & 0xFFFF;
        im_load_address_ = address_type & 0x1FFFFFFCu;
        const uint8_t* src = memory_->TranslatePhysical(address_type & ~3u);
        const uint32_t load_type = address_type & 0x3;
        if (raw_microcode_ && !diag_reorder_swap_ && load_type <= 1 && ReloadsSameMicrocode(load_type, src, size))
          break;
        microcode_.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
          microcode_[i] = rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
        }
        if (diag_reorder_swap_ && load_type == 0) [[unlikely]] DiagSwapFetches(microcode_);
        IdentifyShader(load_type);
        if (raw_microcode_ && load_type <= 1) {
          // Every path of IdentifyShader leaves the stage's microcode equal to this load: keep its raw words.
          auto& raw = load_type == 1 ? raw_ps_ : raw_vs_;
          raw.assign(reinterpret_cast<const uint32_t*>(src), reinterpret_cast<const uint32_t*>(src) + size);
          (load_type == 1 ? raw_ps_valid_ : raw_vs_valid_) = true;
        }
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        if (words < 2) break;
        const uint32_t type = data.Read();
        const uint32_t size = data.Read() & 0xFFFF;
        if (size > words - 2) break;
        im_load_address_ = 0;  // immediate: the microcode is in the ring
        microcode_.resize(size);
        for (uint32_t i = 0; i < size; ++i) microcode_[i] = data.Read();
        if (diag_reorder_swap_ && (type & 0x3) == 0) [[unlikely]] DiagSwapFetches(microcode_);
        IdentifyShader(type & 0x3);
        if ((type & 0x3) == 0) raw_vs_valid_ = false;  // the raw copy no longer matches the stage's microcode
        else if ((type & 0x3) == 1) raw_ps_valid_ = false;
        break;
      }
      case xenos::PM4_NOP:
        // futari_cpu_direct_state: the NOP that covers a draw's state packets names the host record that replaces them.
        if (words >= 2 && data.Peek() == direct::kMagic) {
          const uint32_t marker = packet_address_;  // the NOP's physical address; it spans words + 1 words
          data.Advance(1);
          ApplyDirectRecord(data.Read(), depth, marker, words + 1);
        } else if (words >= 2 && data.Peek() == direct::kApiMagic) {  // futari_api_*: a native Direct3D call's record
          data.Advance(1);
          ApplyApiRecord(data.Read(), depth);
        }
        break;
      default: break;  // nothing to do without drawing
    }
  }

  // futari_cpu_direct_state, ring thread: the record a marker NOP names, applied where its packets were. Register runs
  // go through the same register writes as in Packet(); type-3 packets through Type3 with their original words and
  // address. Records come in the order of their markers (one stream, written under the game's own Direct3D
  // serialization); the records of markers the ring never reached (none so far) are passed on the way. Records stay
  // until the frame's Swap (ReleaseDirectRecords): a range the GPU runs again (a tiling replay) finds them there.
  // Returns the ring position of the record (an index into direct::g_ring), or UINT64_MAX.
  uint64_t FindDirectRecord(uint64_t from, uint64_t to, uint32_t seq, bool count_passed) {
    using namespace direct;
    constexpr uint64_t mask = kRingWords - 1;
    const uint32_t* ring = g_ring.get();
    uint64_t at = from;
    while (ring && at < to) {
      const uint32_t s = ring[at & mask];
      if (s == kPad) {
        at = (at | mask) + 1;
        continue;
      }
      if (s == seq) return at;
      if (int32_t(s - seq) > 0) break;  // every record left is newer
      at += ring[(at & mask) + 1];
      if (count_passed) ++direct_skipped_;
    }
    return UINT64_MAX;
  }

  void ApplyDirectRecord(uint32_t seq, int depth, uint32_t marker, uint32_t region) {
    using namespace direct;
    constexpr uint64_t mask = kRingWords - 1;
    const uint64_t head = g_head.load(std::memory_order_acquire);
    uint64_t at = FindDirectRecord(direct_cursor_, head, seq, true);
    bool replay = false;
    if (at == UINT64_MAX) {
      // A replayed range (the same markers again in this frame): its records are still held since the last Swap.
      at = FindDirectRecord(g_tail.load(std::memory_order_relaxed), direct_cursor_, seq, false);
      replay = at != UINT64_MAX;
    }
    if (at == UINT64_MAX) {
      // A lost record: the packets under its marker are gone.
      if (direct_lost_++ < 8)
        REXLOG_ERROR("[native] direct-state record {} not found: its draw state is missing; no more records", seq);
      g_failed.store(true, std::memory_order_relaxed);
      return;
    }
    const uint32_t* rec = g_ring.get() + (at & mask);
    const uint32_t total = rec[1];
    // The ring's own check (the first records, all of them in verify mode): the region in memory, its first three words
    // from the record, still decodes to the record. A difference: Direct3D (or the game) changed the packets after the
    // draw call, which this design cannot follow; no more records.
    if (direct_ring_checks_left_ > 0 || direct_verify_) {
      if (direct_ring_checks_left_ > 0) --direct_ring_checks_left_;
      ++direct_ring_checks_;
      const auto word = [&](uint32_t p) -> uint32_t {
        const uint32_t i = (p - marker) / 4;
        return i < 3 ? rec[2 + i] : rex::memory::load_and_swap<uint32_t>(memory_->TranslatePhysical(p));
      };
      if (!SameAsWritten(word, marker, marker + 4 * region, rec + kHeader, total - kHeader)) {
        ++direct_ring_differences_;
        g_failed.store(true, std::memory_order_relaxed);
        if (direct_ring_differences_ <= 8)
          REXLOG_ERROR("[native] DIFFERENCE: direct-state record {} no longer matches its region {:08X} ({} words) "
                       "when the ring applies it (packets changed after the draw call); no more records", seq, marker,
                       region);
      }
    }
    ApplyOps(rec + kHeader, rec + total, depth);
    if (replay) {
      ++direct_replayed_;
    } else {
      ++direct_applied_;
      direct_cursor_ = at + total;
    }
    // The region becomes Direct3D's packets again (sasrt-nx): a range the GPU runs a second time, or anything that
    // reads the command buffer back, finds plain PM4. Only over our own marker.
    uint8_t* const m = memory_->TranslatePhysical(marker);
    if (rex::memory::load_and_swap<uint32_t>(m + 4) == kMagic && rex::memory::load_and_swap<uint32_t>(m + 8) == seq) {
      for (uint32_t i = 0; i < 3; ++i) rex::memory::store_and_swap<uint32_t>(m + 4 * i, rec[2 + i]);
    }
    // A long stretch without a Swap (loading): release what was applied once it holds a quarter of the ring, so the
    // game thread keeps finding room (a replay of a range released this way would be lost and reported).
    if (direct_cursor_ - g_tail.load(std::memory_order_relaxed) > kRingWords / 4) ReleaseDirectRecords();
  }

  // futari_api_*: the record of a native Direct3D call, at its 'FTRA' marker. Found like a direct-state record (held
  // until the Swap, so a replayed range finds it again); no region under the marker, so nothing to check or restore.
  void ApplyApiRecord(uint32_t seq, int depth) {
    using namespace direct;
    constexpr uint64_t mask = kRingWords - 1;
    const uint64_t head = g_head.load(std::memory_order_acquire);
    uint64_t at = FindDirectRecord(direct_cursor_, head, seq, true);
    bool replay = false;
    if (at == UINT64_MAX) {
      at = FindDirectRecord(g_tail.load(std::memory_order_relaxed), direct_cursor_, seq, false);
      replay = at != UINT64_MAX;
    }
    if (at == UINT64_MAX) {
      if (api_lost_++ < 8)
        REXLOG_ERROR("[native] native Direct3D record {} not found: that call's state and draws are missing", seq);
      return;
    }
    const uint32_t* rec = g_ring.get() + (at & mask);
    const uint32_t total = rec[1];
    ApplyOps(rec + kHeader, rec + total, depth);
    if (replay) {
      ++direct_replayed_;
    } else {
      ++api_applied_;
      direct_cursor_ = at + total;
    }
    if (direct_cursor_ - g_tail.load(std::memory_order_relaxed) > kRingWords / 4) ReleaseDirectRecords();
  }
  uint64_t api_applied_ = 0, api_lost_ = 0;

  // A record's operations, in order: register runs through the same register writes as Packet(), type-3 packets
  // through Type3 with their original words and address.
  void ApplyOps(const uint32_t* op, const uint32_t* const end, int depth) {
    using namespace direct;
    while (op < end) {
      const uint32_t tag = *op++;
      const uint32_t count = tag >> 8;
      switch (tag & 0xFF) {
        case kRun: {
          const uint32_t index = *op++;
          if (direct_verify_) [[unlikely]] {
            VerifiedRunHost(index, count, op);
          } else {
            WriteConstantRunHost(index, count, op);
          }
          op += count;
          break;
        }
        case kRunSingle: {
          const uint32_t index = *op++;
          for (uint32_t i = 0; i < count; ++i) WriteRegister(index, op[i]);
          op += count;
          break;
        }
        case kType1: {
          const uint32_t h = op[0];
          WriteRegister(h & 0x7FF, op[1]);
          WriteRegister((h >> 11) & 0x7FF, op[2]);
          op += 3;
          break;
        }
        case kType3: {
          const uint32_t h = op[0];
          packet_address_ = op[1];
          Reader r;
          r.base = reinterpret_cast<const uint8_t*>(op + 2);
          r.physical = op[1] + 4;
          r.mask = 0;
          r.pos = 0;
          r.end = count;
          Type3(r, h, count, depth);
          op += 2 + count;
          break;
        }
        default:
          op = end;  // cannot happen: written by EndDrawDirect / the native Direct3D layer
          break;
      }
    }
  }
  // futari_cpu_verify: every register run of a record goes through WriteConstantRunHost and, from the same register
  // file, through WriteConstantRun (what Packet() does with the packet's words); the register range and the generations
  // must come out the same. The reference's result is kept.
  void VerifiedRunHost(uint32_t index, uint32_t count, const uint32_t* v) {
    if (!REXCVAR_GET(futari_native_fast_registers) || index < 0x4000 ||
        uint64_t(index) + count > uint64_t(kRegFetchLast) + 1) {
      // Outside the constant range both are the same WriteRegister loop (whose side effects, gamma ramp and scratch
      // registers, must not run twice).
      WriteConstantRunHost(index, count, v);
      return;
    }
    uint32_t* regs = registers_.data();
    const uint32_t n = std::min<uint32_t>(count, index < kRegisterCount ? kRegisterCount - index : 0);
    const std::vector<uint32_t> before(regs + index, regs + index + n);
    const uint64_t gens[4] = {gen_constants_vs_, gen_constants_ps_, gen_fetch_, gen_viewport_};
    WriteConstantRunHost(index, count, v);
    const std::vector<uint32_t> host(regs + index, regs + index + n);
    const uint64_t host_gens[4] = {gen_constants_vs_, gen_constants_ps_, gen_fetch_, gen_viewport_};
    std::copy(before.begin(), before.end(), regs + index);
    gen_constants_vs_ = gens[0], gen_constants_ps_ = gens[1], gen_fetch_ = gens[2], gen_viewport_ = gens[3];
    uint32_t i = 0;
    WriteConstantRun(index, count, [&] { return v[i++]; });
    ++direct_run_checks_;
    if (!std::equal(host.begin(), host.end(), regs + index) || host_gens[0] != gen_constants_vs_ ||
        host_gens[1] != gen_constants_ps_ || host_gens[2] != gen_fetch_ || host_gens[3] != gen_viewport_) {
      ++direct_run_differences_;  // counted in "[verify] ... differences" by the report (ReportVerify)
      if (direct_run_differences_ <= 8)
        REXLOG_ERROR("[native] DIFFERENCE: direct-state register run {:04X} x{} applied from the record differs from "
                     "the PM4 path's (values or generations)", index, count);
    }
  }
  const bool direct_verify_ = REXCVAR_GET(futari_cpu_verify);
  const int direct_ab_s_ = REXCVAR_GET(futari_ab_s);
  uint64_t ab_ns_[2] = {}, ab_draws_[2] = {}, ab_reported_ns_[2] = {}, ab_reported_draws_[2] = {};
  int32_t direct_ring_checks_left_ = 4096;
  uint64_t direct_ring_checks_ = 0, direct_ring_differences_ = 0, report_direct_ring_checks_ = 0,
           report_direct_ring_differences_ = 0;
  uint64_t direct_run_checks_ = 0, direct_run_differences_ = 0, report_direct_run_checks_ = 0,
           report_direct_run_differences_ = 0;
  // At every Swap: the records applied so far go back to the game thread.
  void ReleaseDirectRecords() { direct::g_tail.store(direct_cursor_, std::memory_order_release); }
  uint64_t direct_cursor_ = 0;  // ring position after the last record applied (the next one expected)
  uint64_t direct_applied_ = 0, direct_lost_ = 0, direct_skipped_ = 0, direct_replayed_ = 0;
  uint64_t report_direct_checked_ = 0, report_direct_check_differences_ = 0, report_direct_applied_ = 0,
           report_direct_lost_ = 0;

  // futari_draw_census: which register groups changed since the previous draw (see the cvar).
  enum CensusGroup {
    kCensusOther, kCensusWindow, kCensusVgt, kCensusRender, kCensusMisc, kCensusVsConst, kCensusPsConst,
    kCensusVertexFetch, kCensusTextureFetch, kCensusBoolLoop, kCensusShaders, kCensusGroups
  };
  void Census() {
    const uint32_t* r = registers_.data();
    if (census_prev_.size() != registers_.size()) {
      census_prev_ = registers_;
      census_vs_ = draw_vs_;
      census_ps_ = draw_ps_;
      return;
    }
    const uint32_t* q = census_prev_.data();
    uint32_t mask = 0;
    const auto range = [&](uint32_t from, uint32_t to, CensusGroup g) {
      if (std::memcmp(r + from, q + from, size_t(to - from) * 4) != 0) mask |= 1u << g;
    };
    range(0, 0x2000, kCensusOther);
    range(0x2000, 0x2100, kCensusWindow);
    // VGT: the draw initiator and the index base/size change with every draw; the rest of the group is state.
    range(0x2100, kRegVgtDrawInitiator, kCensusVgt);
    range(kRegVgtDrawInitiator + 1, 0x2200, kCensusVgt);
    range(0x2200, 0x2400, kCensusRender);
    range(0x2400, 0x4000, kCensusMisc);
    range(0x4000, 0x4400, kCensusVsConst);
    range(0x4400, 0x4800, kCensusPsConst);
    for (uint32_t slot = 0; slot < 32; ++slot) {
      const uint32_t at = kRegFetchFirst + slot * 6;
      if (std::memcmp(r + at, q + at, 24) == 0) continue;
      mask |= 1u << ((r[at] & 3) == 2 ? kCensusTextureFetch : kCensusVertexFetch);
    }
    range(0x4900, kRegisterCount, kCensusBoolLoop);
    if (draw_vs_ != census_vs_ || draw_ps_ != census_ps_) mask |= 1u << kCensusShaders;
    std::memcpy(census_prev_.data(), r, registers_.size() * 4);
    census_vs_ = draw_vs_;
    census_ps_ = draw_ps_;
    ++census_draws_;
    for (uint32_t g = 0; g < kCensusGroups; ++g) census_group_[g] += (mask >> g) & 1;
    ++census_masks_[mask];
    // Merge candidates: nothing changed but the vertices (vertex fetch constants; the initiator and index range).
    if ((mask & ~(1u << kCensusVertexFetch)) == 0) ++census_vertices_only_;
    if (mask == 0) ++census_nothing_;
  }
  void ReportCensus() {
    if (!census_ || !census_draws_) return;
    static const char* const kNames[kCensusGroups] = {"other<2000", "2000-20FF", "VGT", "render 2200-23FF",
                                                      "2400-3FFF", "VS const", "PS const", "vertex fetch",
                                                      "texture fetch", "bool/loop", "shaders"};
    std::string groups;
    for (uint32_t g = 0; g < kCensusGroups; ++g)
      groups += fmt::format(" {} {:.1f}%,", kNames[g], 100.0 * double(census_group_[g]) / double(census_draws_));
    std::vector<std::pair<uint64_t, uint32_t>> top;
    for (const auto& [m, n] : census_masks_) top.push_back({n, m});
    std::sort(top.rbegin(), top.rend());
    std::string masks;
    for (size_t i = 0; i < top.size() && i < 8; ++i) {
      masks += fmt::format(" [{:.1f}%:", 100.0 * double(top[i].first) / double(census_draws_));
      if (!top[i].second) masks += " nothing";
      for (uint32_t g = 0; g < kCensusGroups; ++g)
        if (top[i].second >> g & 1) masks += fmt::format(" {}", kNames[g]);
      masks += "]";
    }
    REXLOG_INFO("[native] draw census (cumulative, {} draws): changed since the previous draw:{} | only the vertices "
                "changed: {:.1f}% (nothing at all: {:.1f}%) | most common sets:{}",
                census_draws_, groups, 100.0 * double(census_vertices_only_) / double(census_draws_),
                100.0 * double(census_nothing_) / double(census_draws_), masks);
  }
  const bool census_ = REXCVAR_GET(futari_draw_census);
  std::vector<uint32_t> census_prev_;
  const futari::renderer::ShaderEntry* census_vs_ = nullptr;
  const futari::renderer::ShaderEntry* census_ps_ = nullptr;
  uint64_t census_draws_ = 0, census_vertices_only_ = 0, census_nothing_ = 0, census_group_[kCensusGroups] = {};
  std::unordered_map<uint32_t, uint64_t> census_masks_;

  // banjo-nx's finding (2026-10-07): nothing in this ring bumped g_synchronizations_ring, so the renderer's same-frame
  // reuse by address (the index cache above 16 KB, the vertex dedupe's large ranges) was bounded only by the frame.
  // A fence the guest can observe (EVENT_WRITE_SHD, MEM_WRITE, INTERRUPT) or a wait it makes the GPU do (WAIT_REG_MEM)
  // is where it may learn that a buffer was consumed and rewrite it: in stream order, everything recorded before is
  // forgotten there (only fewer reuses; exact).
  void NoteGuestSync() {
    static const bool on = REXCVAR_GET(futari_ring_sync_fences);
    if (on) futari::renderer::g_synchronizations_ring.fetch_add(1, std::memory_order_relaxed);
  }

  uint32_t Register(uint32_t index) const { return index < kRegisterCount ? registers_[index] : 0; }

  // --- Native drawing ---
  bool EnsureTargets() {
    if (!targets_ && !targets_failed_) {
      targets_ = futari::renderer::TargetsNative::Create(provider_ ? provider_->vulkan_device() : nullptr,
                                                        memory_);
      if (!targets_) {
        targets_failed_ = true;
        REXLOG_ERROR("[native] could not create the native render targets");
      }
    }
    return targets_ != nullptr;
  }

  bool ComputeVertexIdentity(const futari::renderer::ShaderEntry* candidate) const {
    if (VertexShaderIdentityMatches(
            {ShaderIdentityStage::Vertex, vs_microcode_},
            {candidate->vertices ? ShaderIdentityStage::Vertex : ShaderIdentityStage::Pixel, candidate->microcode},
            candidate->elements, [](const auto& element) { return element.instruction; }))
      return true;
    // futari_gpu_vs_fetch_reorder: only the entry IdentifyShader chose for this very load, and only while the proof
    // holds on its words (futari::renderer::FetchReordered).
    return candidate == vs_reorder_entry_ && futari::renderer::FetchReordered(*candidate, vs_microcode_);
  }
  bool ComputePixelIdentity(const futari::renderer::ShaderEntry* candidate) const {
    return PixelShaderIdentityMatches(
        {ShaderIdentityStage::Pixel, ps_microcode_},
        {candidate->vertices ? ShaderIdentityStage::Vertex : ShaderIdentityStage::Pixel, candidate->microcode});
  }
  // Direct-mapped memo cell of a library entry for one stage's current load generation.
  struct IdentityCell {
    const futari::renderer::ShaderEntry* candidate = nullptr;
    uint64_t generation = 0;
    bool result = false;
  };
  static constexpr size_t kIdentityCells = 1024;
  static size_t IdentityCellOf(const void* entry) {
    uintptr_t x = uintptr_t(entry) >> 4;
    x ^= x >> 10;
    return size_t(x) & (kIdentityCells - 1);
  }
  // futari::renderer::FetchCoherent(*entry, vs_microcode_), memoized per VS load generation (a pure function of both).
  bool FetchCoherent(const futari::renderer::ShaderEntry* entry) {
    if (!identity_flat_) return futari::renderer::FetchCoherent(*entry, vs_microcode_);
    IdentityCell& cell = fc_cells_[IdentityCellOf(entry)];
    if (cell.candidate != entry || cell.generation != vs_identity_generation_) {
      cell = {entry, vs_identity_generation_, futari::renderer::FetchCoherent(*entry, vs_microcode_)};
    } else if (check_identity_ > 0) {
      --check_identity_;
      if (cell.result != futari::renderer::FetchCoherent(*entry, vs_microcode_)) {
        ::me::native::VerifyDifference();
        REXLOG_ERROR("[native] DIFFERENCE: flat fetch-coherence memo disagrees with the direct check (entry n{}); the old "
                     "path from now on", entry->number);
        identity_flat_ = false;
        return futari::renderer::FetchCoherent(*entry, vs_microcode_);
      }
    }
    return cell.result;
  }
  bool MatchesPixelShaderIdentity(const futari::renderer::ShaderEntry* candidate) {
    if (!candidate) return false;
    if (!identity_flat_) return ComputePixelIdentity(candidate);
    IdentityCell& cell = ps_cells_[IdentityCellOf(candidate)];
    if (cell.candidate != candidate || cell.generation != ps_identity_generation_) {
      cell = {candidate, ps_identity_generation_, ComputePixelIdentity(candidate)};
    } else if (check_identity_ > 0) {
      --check_identity_;
      if (cell.result != ComputePixelIdentity(candidate)) {
        ::me::native::VerifyDifference();
        REXLOG_ERROR("[native] DIFFERENCE: flat PS identity memo disagrees with the direct compare (entry n{}); the "
                     "old path from now on", candidate->number);
        identity_flat_ = false;
        return ComputePixelIdentity(candidate);
      }
    }
    return cell.result;
  }

  bool MatchesVertexShaderIdentity(const futari::renderer::ShaderEntry* candidate) {
    if (!candidate) return false;
    if (identity_flat_) {
      IdentityCell& cell = vs_cells_[IdentityCellOf(candidate)];
      if (cell.candidate != candidate || cell.generation != vs_identity_generation_) {
        cell = {candidate, vs_identity_generation_, ComputeVertexIdentity(candidate)};
      } else if (check_identity_ > 0) {
        --check_identity_;
        if (cell.result != ComputeVertexIdentity(candidate)) {
          ::me::native::VerifyDifference();
          REXLOG_ERROR("[native] DIFFERENCE: flat VS identity memo disagrees with the direct compare (entry n{}); the "
                       "old path from now on", candidate->number);
          identity_flat_ = false;
          return ComputeVertexIdentity(candidate);
        }
      }
      return cell.result;
    }
    auto& cached = vs_identity_cache_[candidate];
    if (cached.first != vs_identity_generation_) {
      cached.first = vs_identity_generation_;
      cached.second = VertexShaderIdentityMatches(
          {ShaderIdentityStage::Vertex, vs_microcode_},
          {candidate->vertices ? ShaderIdentityStage::Vertex : ShaderIdentityStage::Pixel,
           candidate->microcode}, candidate->elements,
          [](const auto& element) { return element.instruction; });
    }
    return cached.second;
  }

  // Once per (loaded program, candidate): every differing word between the loaded VS and the library candidate
  // (same size only), so an unresolved shader can be understood or added to the library without a debugger.
  void LogVertexWordDiff(const futari::renderer::ShaderEntry& candidate) {
    if (!candidate.vertices || vs_microcode_.empty() || vs_microcode_.size() != candidate.microcode.size() ||
        vs_diff_logged_.size() >= 40) return;
    const uint64_t key = current_vs_hash_ ^ (uint64_t(candidate.number) * 0x9E3779B97F4A7C15ull);
    if (!vs_diff_logged_.insert(key).second) return;
    std::string diffs;
    size_t count = 0;
    for (size_t w = 0; w < vs_microcode_.size(); ++w) {
      if (vs_microcode_[w] == candidate.microcode[w]) continue;
      if (++count <= 24)
        diffs += fmt::format(" [w{} i{}.{} {:08X}>{:08X}]", w, w / 3, w % 3, vs_microcode_[w], candidate.microcode[w]);
    }
    std::string declared;
    for (const auto& element : candidate.elements) declared += fmt::format(" {}", element.instruction);
    REXLOG_WARN("[native] VS word diff loaded_hash={:016X} words={} candidate n{} differing_words={} "
                "(loaded>library, first 24) declared_fetch_instr:{} diffs:{}",
                current_vs_hash_, vs_microcode_.size(), candidate.number, count, declared, diffs);
  }
  std::unordered_set<uint64_t> vs_diff_logged_;

  bool AcceptVertexShaderIdentity(const futari::renderer::ShaderEntry* candidate,
                                  const char* provenance) {
    if (!candidate) return false;
    if (MatchesVertexShaderIdentity(candidate)) return true;
    DumpLoadedVertexVariant(*candidate);
    LogVertexWordDiff(*candidate);
    const uint64_t mismatch = ++vs_identity_mismatches_;
    if (mismatch <= 32) {
      // Diagnostic only: mirror the predicate's rejection order without relaxing
      // any field. Loaded words are raw; the library words are already masked.
      constexpr uint32_t masks[3]{0x0007FFFFu, 0x80000FFFu, 0x80000000u};
      const char* reason = "unknown";
      size_t failed_word = SIZE_MAX;
      uint32_t failed_mask = UINT32_MAX;
      if (!candidate->vertices) reason = "wrong-stage";
      else if (vs_microcode_.empty()) reason = "empty-loaded";
      else if (vs_microcode_.size() != candidate->microcode.size()) reason = "word-count";
      else {
        bool failed = false;
        for (const auto& element : candidate->elements) {
          const size_t start = size_t(element.instruction) * 3;
          if (start + 3 > candidate->microcode.size()) {
            reason = "declared-fetch-out-of-bounds";
            failed_word = start;
            failed = true;
            break;
          }
          if ((candidate->microcode[start] & 0x1Fu) != 0u) {
            reason = "declared-fetch-opcode";
            failed_word = start;
            failed = true;
            break;
          }
          for (size_t lane = 0; lane < 3; ++lane) {
            if ((candidate->microcode[start + lane] & masks[lane]) !=
                candidate->microcode[start + lane]) {
              reason = "candidate-not-normalized";
              failed_word = start + lane;
              failed_mask = masks[lane];
              failed = true;
              break;
            }
          }
          if (failed) break;
          if (!VertexFetchSwizzleRepresentable(candidate->microcode[start + 1] & 0xFFFu,
                                               vs_microcode_[start + 1] & 0xFFFu)) {
            reason = "unrepresentable-fetch-swizzle";
            failed_word = start + 1;
            failed_mask = 0xFFFu;
            failed = true;
            break;
          }
        }
        if (!failed) {
          for (size_t word = 0; word < vs_microcode_.size(); ++word) {
            uint32_t mask = UINT32_MAX;
            for (const auto& element : candidate->elements) {
              const size_t start = size_t(element.instruction) * 3;
              if (word >= start && word - start < 3) {
                mask = word - start == 1 ? 0x80000000u : masks[word - start];
                break;
              }
            }
            if ((vs_microcode_[word] & mask) != (candidate->microcode[word] & mask)) {
              reason = mask == UINT32_MAX ? "non-fetch-word" : "masked-fetch-word";
              failed_word = word;
              failed_mask = mask;
              break;
            }
          }
        }
      }
      const uint32_t raw = failed_word < vs_microcode_.size() ? vs_microcode_[failed_word] : 0;
      const uint32_t selected = failed_word < candidate->microcode.size()
                                    ? candidate->microcode[failed_word] : 0;
      std::string declared;
      for (const auto& element : candidate->elements)
        declared += fmt::format(" {}", element.instruction);
      REXLOG_WARN("[native] VS identity detail mismatch={} reason={} word={} instruction={} lane={} "
                  "loaded={:08X} selected={:08X} mask={:08X} masked_loaded={:08X} declared_triples={}",
                  mismatch, reason, failed_word, failed_word / 3, failed_word % 3,
                  raw, selected, failed_mask, raw & failed_mask, declared);
    }
    if (mismatch <= 32 || (mismatch & 255) == 0) {
      const uint64_t selected_hash = XXH3_64bits(candidate->microcode.data(),
                                                candidate->microcode.size() * sizeof(uint32_t));
      REXLOG_WARN("[native] VS identity mismatch {} draw={} packet={:08X} "
                  "provenance={} loaded_host_hash={:016X} loaded_words={} selected_n={} "
                  "selected_stage={} selected_normalized_host_hash={:016X} selected_words={} "
                  "selected_container={:016X}",
                  mismatch, draws_, packet_address_, provenance,
                  current_vs_hash_, vs_microcode_.size(), candidate->number,
                  candidate->vertices ? "VS" : "PS", selected_hash, candidate->microcode.size(),
                  candidate->shader ? candidate->shader->fingerprint : 0);
    }
    return false;
  }

  const futari::renderer::ShaderEntry* PreferVertexShaderCandidate(
      const futari::renderer::ShaderEntry* current,
      const futari::renderer::ShaderEntry* candidate, const char* provenance) {
    // Keep diagnostics/variant discovery independent of the rollout policy.
    if (!candidate || !AcceptVertexShaderIdentity(candidate, provenance)) return current;
    return ShouldReplaceVertexCandidate(current != nullptr, MatchesVertexShaderIdentity(current),
                                        true, MatchesVertexShaderIdentity(candidate),
                                        false)
               ? candidate : current;
  }

  bool AcceptPixelShaderIdentity(const futari::renderer::ShaderEntry* candidate,
                                  const char* provenance) {
    if (!candidate) return false;
    const bool matches = MatchesPixelShaderIdentity(candidate);
    if (matches) return true;
    const uint64_t mismatch = ++ps_identity_mismatches_;
    if (mismatch <= 32 || (mismatch & 255) == 0) {
      const uint64_t selected_hash = XXH3_64bits(candidate->microcode.data(),
                                                candidate->microcode.size() * sizeof(uint32_t));
      REXLOG_WARN("[native] PS identity mismatch {} draw={} packet={:08X} "
                  "provenance={} loaded_host_hash={:016X} loaded_words={} selected_n={} "
                  "selected_stage={} selected_host_hash={:016X} selected_words={} "
                  "selected_container={:016X}",
                  mismatch, draws_, packet_address_, provenance,
                  current_ps_hash_, ps_microcode_.size(), candidate->number,
                  candidate->vertices ? "VS" : "PS", selected_hash, candidate->microcode.size(),
                  candidate->shader ? candidate->shader->fingerprint : 0);
    }
    return false;
  }

  // futari_native_raw_microcode: IdentifyShader's "same program reloaded from the same address" test on the raw
  // guest words. True = nothing to do (the stage keeps its entry, hashes and generations).
  bool ReloadsSameMicrocode(uint32_t type, const uint8_t* src, uint32_t size) {
    if (!REXCVAR_GET(futari_native_repeated_identity)) return false;
    const auto& raw = type == 1 ? raw_ps_ : raw_vs_;
    const auto* previous_entry = type == 1 ? ps_ : vs_;
    if (!previous_entry || !(type == 1 ? raw_ps_valid_ : raw_vs_valid_) || raw.size() != size ||
        (type == 1 ? last_ps_load_address_ : last_vs_load_address_) != im_load_address_ ||
        std::memcmp(raw.data(), src, size_t(size) * 4) != 0) {
      return false;
    }
    if (check_raw_ > 0) {
      --check_raw_;
      const auto& swapped = type == 1 ? ps_microcode_ : vs_microcode_;
      bool equal = swapped.size() == size;
      for (uint32_t i = 0; equal && i < size; ++i)
        equal = swapped[i] == rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
      if (!equal) {
        ::me::native::VerifyDifference();
        REXLOG_ERROR("[native] DIFFERENCE: raw microcode compare said 'same program' ({} words, type {}, address "
                     "{:08X}) but the swapped words differ; the old path from now on", size, type, im_load_address_);
        raw_microcode_ = false;
        return false;
      }
    }
    ++shader_loads_reused_;
    return true;
  }

  void IdentifyShader(uint32_t type) {
    if (type > 1) return;
    // The same program reloaded from the same address (D3D re-sends it for most draws): keep the identified
    // entry, the hashes and the generations, instead of copying, hashing twice and matching again.
    if (REXCVAR_GET(futari_native_repeated_identity)) {
      const auto& previous = type == 1 ? ps_microcode_ : vs_microcode_;
      const auto* previous_entry = type == 1 ? ps_ : vs_;
      if (previous_entry && previous.size() == microcode_.size() &&
          (type == 1 ? last_ps_load_address_ : last_vs_load_address_) == im_load_address_ &&
          std::memcmp(previous.data(), microcode_.data(), microcode_.size() * sizeof(uint32_t)) == 0) {
        ++shader_loads_reused_;
        return;
      }
    }
    (type == 1 ? last_ps_load_address_ : last_vs_load_address_) = im_load_address_;
    // Memo by content (futari_native_identity_memo): the same program loaded again after others, from any
    // address, gets the entry identified before; the draw-time guard still checks it against this microcode.
    const uint64_t memo_hash = XXH3_64bits(microcode_.data(), microcode_.size() * sizeof(uint32_t));
    const uint64_t memo_key = memo_hash ^ (uint64_t(type) << 63) ^ (uint64_t(microcode_.size()) << 40);
    if (REXCVAR_GET(futari_native_identity_memo)) {
      const auto it = identity_memo_.find(memo_key);
      if (it != identity_memo_.end()) {
        if (type == 1) {
          ps_microcode_ = microcode_;
          ++ps_identity_generation_;
          current_ps_hash_ = memo_hash;
          ps_ = it->second;
        } else {
          vs_microcode_ = microcode_;
          current_vs_hash_ = memo_hash;
          ++vs_identity_generation_;
          vs_ = it->second;
          vs_reorder_entry_ =
              !vs_reorder_memo_keys_.empty() && vs_reorder_memo_keys_.count(memo_key) ? it->second : nullptr;
          gen_vs_ = ++gen_microcode_;
        }
        ++shaders_identified_;
        ++identity_memo_hits_;
        return;
      }
    }
    if (type == 1) {
      ps_microcode_ = microcode_;
      ++ps_identity_generation_;
      current_ps_hash_ = memo_hash;
    } else {
      // Identity checks below must see THIS IM_LOAD, not the previous VS load.
      vs_microcode_ = microcode_;
      current_vs_hash_ = memo_hash;
      ++vs_identity_generation_;
      vs_reorder_entry_ = nullptr;
    }
    // A VS address identifies the object, not necessarily the variant currently in the ring: D3D
    // rewrites vertex fetches for each declaration. Prefer the actual loaded microcode (exact, then
    // the library's masked-fetch matcher), and accept the address entry only if it is compatible.
    const futari::renderer::ShaderEntry* by_address =
        im_load_address_ ? ByAddress(im_load_address_) : nullptr;
    const futari::renderer::ShaderEntry* entry = nullptr;
    if (type == 0) {
      const futari::renderer::ShaderEntry* observed = nullptr;
      entry = ByCode(microcode_, true, &observed);
      if (observed && !AcceptVertexShaderIdentity(observed, "identify-code")) entry = nullptr;
      if (entry && (entry->microcode.size() != microcode_.size() ||
                    !futari::renderer::FetchCoherent(*entry, microcode_))) {
        entry = nullptr;
      }
      // Guard OFF is permission for a last-resort legacy fallback, not for a
      // stale fast/address candidate to hide a proven current program.
      if (!MatchesVertexShaderIdentity(entry) && shaders_.loaded()) {
        const auto* matched = shaders_.Identify(true, microcode_);
        entry = PreferVertexShaderCandidate(entry, matched, "identify-library");
      }
      if (by_address && (!entry || (!MatchesVertexShaderIdentity(entry) &&
                                   MatchesVertexShaderIdentity(by_address))) &&
          by_address->microcode.size() == microcode_.size() &&
          futari::renderer::FetchCoherent(*by_address, microcode_)) {
        entry = PreferVertexShaderCandidate(entry, by_address, "identify-address");
      }
      if (entry == by_address && entry) ++shaders_by_address_;
      else if (entry) ++shaders_by_code_;
      // futari_gpu_vs_fetch_reorder (sa2-nx): no entry is this program in its own fetch order. Direct3D's patcher may
      // have moved the declared fetches to each other's slots for the vertex declaration (me_vertex_fetch_reorder.h):
      // the library shader the proof holds for becomes this load's entry, and the one candidate the identity checks
      // accept for it. Identity results memoized before were made without it: a new generation.
      if (!entry && shaders_.loaded()) {
        if (const auto* reordered = shaders_.IdentifyReordered(microcode_)) {
          vs_reorder_entry_ = reordered;
          ++vs_identity_generation_;
          if (MatchesVertexShaderIdentity(reordered)) {
            entry = reordered;
            ++shaders_reordered_;
            if (REXCVAR_GET(futari_native_identity_memo)) vs_reorder_memo_keys_.insert(memo_key);
          } else {
            vs_reorder_entry_ = nullptr;
          }
        }
      }
    } else {
      if (by_address &&
          AcceptPixelShaderIdentity(by_address, "identify-address")) {
        entry = by_address;
        ++shaders_by_address_;
      }
      if (!entry) {
        const futari::renderer::ShaderEntry* observed = nullptr;
        const auto* code_entry = ByCode(microcode_, false, &observed);
        const bool accepted = observed && AcceptPixelShaderIdentity(observed, "identify-code");
        if (code_entry && accepted) {
          entry = code_entry;
          ++shaders_by_code_;
        }
      }
      if (!entry && shaders_.loaded()) {
        const auto* matched = shaders_.Identify(false, microcode_);
        if (matched && AcceptPixelShaderIdentity(matched, "identify-library")) entry = matched;
      }
    }
    (type == 0 ? vs_ : ps_) = entry;
    (type == 0 ? current_vs_hash_ : current_ps_hash_) = memo_hash;
    if (entry && REXCVAR_GET(futari_native_identity_memo)) {
      if (identity_memo_.size() > 8192) {
        identity_memo_.clear();
        vs_reorder_memo_keys_.clear();
        if (type == 0 && entry == vs_reorder_entry_) vs_reorder_memo_keys_.insert(memo_key);
      }
      identity_memo_.emplace(memo_key, entry);
    }
    if (type == 0) {
      gen_vs_ = ++gen_microcode_;
    }
    ++(entry ? shaders_identified_ : shaders_unidentified_);
    if (!entry) {
      ++(im_load_address_ ? (type == 0 ? miss_vs_addr_ : miss_ps_addr_) : miss_immediate_);
      DumpRawMicrocode(type == 0, im_load_address_, microcode_);
      if (!im_load_address_ && shaders_.loaded()) {
        DumpMissingImmediate(shaders_, type == 0, microcode_);
      }
    }
    if (!entry && logged_misses_ < 12) {  // diagnostic: what the ring loads that the library lacks
      ++logged_misses_;
      std::string keys;
      {
        std::lock_guard<std::mutex> lock(g_by_address_mutex);
        for (const auto& [k, v] : g_by_address) {
          if ((k >> 12) == (im_load_address_ >> 12)) keys += fmt::format(" {:08X}", k);
        }
      }
      REXLOG_INFO("[native] unidentified {} IM_LOAD at {:08X} (keys in page:{}): {} words, {:08X} {:08X} {:08X} {:08X}",
                  type == 0 ? "VS" : "PS", im_load_address_, keys, microcode_.size(), microcode_.size() > 0 ? microcode_[0] : 0,
                  microcode_.size() > 1 ? microcode_[1] : 0, microcode_.size() > 2 ? microcode_[2] : 0,
                  microcode_.size() > 3 ? microcode_[3] : 0);
    }
  }

  // Opt-in discovery only, not permission to select a mismatching program. Keep
  // the SAME-size template's complete physical payload, including DEF literals;
  // changing only its instruction bytes makes offline translation auditable.
  void DumpLoadedVertexVariant(const futari::renderer::ShaderEntry& candidate) {
    static const char* const folder = std::getenv("FUTARI_VERTEX_VARIANTS");
    if (!folder || !*folder || !candidate.vertices || !candidate.shader || vs_microcode_.empty() ||
        vs_microcode_.size() != candidate.microcode.size() || dumped_vertex_variants_.size() >= 64) return;
    const auto& original = candidate.shader->original;
    if (original.size() < 28 || original.size() > 8u * 1024 * 1024) return;
    const uint32_t flags = rex::memory::load_and_swap<uint32_t>(original.data());
    const uint32_t virtual_bytes = rex::memory::load_and_swap<uint32_t>(original.data() + 4);
    const uint32_t physical_bytes = rex::memory::load_and_swap<uint32_t>(original.data() + 8);
    const uint32_t header = rex::memory::load_and_swap<uint32_t>(original.data() + 24);
    if ((flags & 0xFFFFFF00u) != 0x102A1100u || !(flags & 1u) || virtual_bytes > original.size() ||
        physical_bytes > original.size() - virtual_bytes || header > virtual_bytes ||
        virtual_bytes - header < 8) return;
    const uint32_t code_offset = rex::memory::load_and_swap<uint32_t>(original.data() + header);
    const uint32_t code_bytes = rex::memory::load_and_swap<uint32_t>(original.data() + header + 4);
    if (code_bytes != vs_microcode_.size() * 4 || code_offset > physical_bytes ||
        code_bytes > physical_bytes - code_offset) return;
    const uint64_t loaded_hash = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * 4);
    const uint64_t key = XXH3_64bits_withSeed(original.data(), original.size(), loaded_hash);
    if (!dumped_vertex_variants_.insert(key).second) return;
    std::vector<uint8_t> container(original);
    for (size_t i = 0; i < vs_microcode_.size(); ++i)
      rex::memory::store_and_swap<uint32_t>(container.data() + virtual_bytes + code_offset + i * 4,
                                          vs_microcode_[i]);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) { REXLOG_WARN("[native] vertex variant discovery directory failed: {}", ec.message()); return; }
    const auto path = std::filesystem::path(folder) / fmt::format("vs_from_n{}_{:016x}.bin", candidate.number, key);
    const auto pending_path = std::filesystem::path(path.string() + ".pending");
    bool good = false;
    if (auto* file = std::fopen(pending_path.string().c_str(), "wbx")) {
      good = std::fwrite(container.data(), 1, container.size(), file) == container.size();
      if (std::fclose(file) != 0) good = false;
    }
    if (good) {
      std::filesystem::create_hard_link(pending_path, path, ec);
      good = !ec;
    }
    REXLOG_INFO("[native] vertex variant discovery written={} template_n={} loaded_host_hash={:016X} "
                "words={} (complete template retained, NOT verified compatible): {}",
                good, candidate.number, loaded_hash, vs_microcode_.size(), path.string());
  }

  // With FUTARI_SHADER_MISSING=<folder>: the container of a Direct3D vertex shader as the ring loaded
  // it (rewritten for the vertex declaration), made of the object's container header and the ring's
  // microcode, once per microcode. shaders/tools/build_shader_spirv.sh adds it to the library.
  void DumpRewrittenVertexShader(const futari::renderer::ShaderEntry& object_entry) {
    static const char* const folder = std::getenv("FUTARI_SHADER_MISSING");
    if (!folder || !*folder || !object_entry.shader || vs_microcode_.empty()) return;
    const uint64_t code = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
    if (!dumped_rewritten_.insert(code).second) return;
    const auto& o = object_entry.shader->original;
    if (o.size() < 28) return;
    const uint32_t vsize = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
    const uint32_t sh = rex::memory::load_and_swap<uint32_t>(o.data() + 24);
    if (vsize > o.size() || sh + 8 > vsize) return;
    const uint32_t bytes = uint32_t(vs_microcode_.size() * 4);
    std::vector<uint8_t> container(o.begin(), o.begin() + vsize);
    rex::memory::store_and_swap<uint32_t>(container.data() + 8, bytes);   // physical size
    rex::memory::store_and_swap<uint32_t>(container.data() + sh, 0);      // microcode offset
    rex::memory::store_and_swap<uint32_t>(container.data() + sh + 4, bytes);
    container.resize(size_t(vsize) + bytes);
    for (size_t i = 0; i < vs_microcode_.size(); ++i) {
      rex::memory::store_and_swap<uint32_t>(container.data() + vsize + i * 4, vs_microcode_[i]);
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const auto path = std::filesystem::path(folder) /
                      fmt::format("vs_{:016x}.bin", XXH3_64bits(container.data(), container.size()));
    if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
      std::fwrite(container.data(), 1, container.size(), f);
      std::fclose(f);
      REXLOG_INFO("[native] rewritten Direct3D vertex shader kept: {}", path.string());
    }
  }

  // The library entries of the VS and PS objects a record names (nullptr = unknown object).
  void LookupObjectEntries(uint32_t object_vs, uint32_t object_ps, const futari::renderer::ShaderEntry*& vs,
                           const futari::renderer::ShaderEntry*& ps) {
    vs = ps = nullptr;
    if (pairing_fast_ && LookupObject(object_vs, vs) && LookupObject(object_ps, ps)) {
      if (check_objects_ > 0) {
        --check_objects_;
        const futari::renderer::ShaderEntry* want_vs = nullptr;
        const futari::renderer::ShaderEntry* want_ps = nullptr;
        {
          std::lock_guard<std::mutex> lock(g_by_address_mutex);
          auto a = g_entry_of_object.find(object_vs);
          auto b = g_entry_of_object.find(object_ps);
          if (a != g_entry_of_object.end()) want_vs = a->second;
          if (b != g_entry_of_object.end()) want_ps = b->second;
        }
        if (want_vs != vs || want_ps != ps) {  // the game thread may have just added or replaced an entry: look again
          const futari::renderer::ShaderEntry* again_vs = nullptr;
          const futari::renderer::ShaderEntry* again_ps = nullptr;
          LookupObject(object_vs, again_vs);
          LookupObject(object_ps, again_ps);
          if (again_vs != want_vs || again_ps != want_ps) {
            ::me::native::VerifyDifference();
            REXLOG_ERROR("[native] DIFFERENCE: lock-free object table: VS {:08X} {} / {}, PS {:08X} {} / {} (table / "
                         "map); the old path from now on", object_vs, static_cast<const void*>(again_vs),
                         static_cast<const void*>(want_vs), object_ps, static_cast<const void*>(again_ps),
                         static_cast<const void*>(want_ps));
            pairing_fast_ = false;
          }
          vs = want_vs;
          ps = want_ps;
        }
      }
      return;
    }
    vs = ps = nullptr;
    std::lock_guard<std::mutex> lock(g_by_address_mutex);
    auto a = g_entry_of_object.find(object_vs);
    auto b = g_entry_of_object.find(object_ps);
    if (a != g_entry_of_object.end()) vs = a->second;
    if (b != g_entry_of_object.end()) ps = b->second;
  }

  // By packet address: the record whose command-buffer start is the closest one below this DRAW_INDX.
  bool PairByAddress(uint32_t type) {
    DrawRecord r;
    if (RecordTable()) {
      DrainRecords();  // the table belongs to the ring thread: no lock
      DrawRecordSlot* best = nullptr;
      if (pairing_fast_) {
        best = g_records.FindDense(packet_address_);
        if (check_pairing_ > 0) {
          --check_pairing_;
          DrawRecordSlot* old = g_records.FindScan(packet_address_);
          if (old != best) {
            ::me::native::VerifyDifference();
            REXLOG_ERROR("[native] DIFFERENCE: fast record lookup for packet {:08X} chose slot {} but the scan chose {}; "
                         "the old path from now on", packet_address_, best ? int64_t(g_records.IndexOf(best)) : -1,
                         old ? int64_t(g_records.IndexOf(old)) : -1);
            pairing_fast_ = false;
            g_record_fast = false;
            best = old;
          }
        }
      } else {
        best = g_records.FindScan(packet_address_);
      }
      // The map's rule: the nearest record at or below the packet, within 0x2000 and of the same type.
      if (!best || packet_address_ - best->start > 0x2000 || best->record.type != type) return false;
      r = std::move(best->record);
      g_records.Consume(best);
    } else {
      std::lock_guard<std::mutex> lock(g_draw_mutex);
      auto it = g_draw_by_address.upper_bound(packet_address_);
      if (it == g_draw_by_address.begin()) return false;
      --it;
      if (packet_address_ - it->first > 0x2000 || it->second.type != type) return false;
      r = it->second;
      g_draw_by_address.erase(it);
    }
    const futari::renderer::ShaderEntry* vs = nullptr;
    const futari::renderer::ShaderEntry* ps = nullptr;
    LookupObjectEntries(r.vs, r.ps, vs, ps);
    if (vs && !AcceptVertexShaderIdentity(vs, "pair-address-object")) vs = nullptr;
    if (vs && vs->binding_per_register) {
      // Direct3D rewrote this vertex shader for the vertex declaration: the variant the ring loaded is
      // its own library entry, found by microcode (vs_). Without it, keep a copy to extend the library.
      if (vs_ && vs_->binding_per_register) {
        vs = vs_;
      } else {
        DumpRewrittenVertexShader(*vs);
      }
    }
    // Audit the object even when the old register-linked override would hide a
    // stale entry. The guard protects every path, not just shader loading.
    if (ps && !AcceptPixelShaderIdentity(ps, "pair-address-object")) ps = nullptr;
    if (ps && ps->binding_per_register) {
      // Direct3D reuses its small internal PS objects and overwrites the same ring allocation with
      // different microcode. The object entry therefore describes only the first variant observed;
      // the IM_LOAD at this draw is authoritative, exactly as for rewritten register-linked VS.
      ps = ps_ && ps_->binding_per_register ? ps_ : nullptr;
    }
    // The IM_LOAD is the authoritative patched variant. A Draw* record may point at the
    // unpatched object, or at a different variant queued ahead of this indirect buffer.
    // Do not combine such an entry with the current ring microcode: ComputeEntry would
    // reject it with cause 20 (or derive the wrong vertex layout).
    if (vs && AcceptVertexShaderIdentity(vs, "pair-address-selected") &&
        vs->microcode.size() == vs_microcode_.size() &&
        FetchCoherent(vs)) {
      draw_vs_ = PreferVertexShaderCandidate(draw_vs_, vs, "pair-address-preference");
    }
    if (ps && AcceptPixelShaderIdentity(ps, "pair-address-selected")) draw_ps_ = ps;
    ++draws_paired_by_address_;
    if (!vs) {
      ++paired_no_vs_;
      if (missing_objects_.size() < 32 || missing_objects_.count(r.vs)) ++missing_objects_[r.vs];
    }
    if (!ps) {
      ++paired_no_ps_;
      if (missing_objects_.size() < 32 || missing_objects_.count(r.ps)) ++missing_objects_[r.ps];
    }
    return true;
  }

  // The first of up to 8 pending records with the same primitive type and
  // a matching count gives the VS and PS objects; otherwise the ring's own identification.
  void PairDraw() {
    draw_vs_ = vs_ && AcceptVertexShaderIdentity(vs_, "pair-initial-ring") ? vs_ : nullptr;
    draw_ps_ = ps_;
    // futari_ring_pairing_skip: no record exists, so the table, the replay map (filled only by a pairing) and the
    // queue are empty and every lookup below fails.
    static const bool pairing_skip = REXCVAR_GET(futari_ring_pairing_skip);
    if (pairing_skip && !AbOff("futari_ring_lean") && pending_.empty() && paired_packets_.empty() &&
        g_draw_records.load(std::memory_order_relaxed) == 0) {
      ++draws_unpaired_;
      return;
    }
    if (PairByAddress(Register(kRegVgtDrawInitiator) & 0x3F)) return;
    // Predicated tiling replays the same command buffer once per EDRAM tile: a DRAW_INDX packet at an
    // address already paired in this frame reuses that pairing and consumes no record.
    if (auto it = paired_packets_.find(packet_address_);
        it != paired_packets_.end() && it->second.frame == swaps_) {
      draw_vs_ = PreferVertexShaderCandidate(draw_vs_, it->second.vs, "pair-replay");
      if (it->second.ps && AcceptPixelShaderIdentity(it->second.ps, "pair-replay"))
        draw_ps_ = it->second.ps;
      ++draws_replayed_;
      return;
    }
    if (!pairing_fast_) {  // nothing ever fills g_draw_queue (records go through the lock-free queue)
      std::lock_guard<std::mutex> lock(g_draw_mutex);
      while (pending_.size() < 4096 && !g_draw_queue.empty()) {
        pending_.push_back(g_draw_queue.front());
        g_draw_queue.pop_front();
      }
    }
    const uint32_t initiator = Register(kRegVgtDrawInitiator);
    const uint32_t type = initiator & 0x3F, count = initiator >> 16;
    const size_t n = std::min<size_t>(pending_.size(), 64);
    for (size_t i = 0; i < n; ++i) {
      const DrawRecord& r = pending_[i];
      if (r.type != type) continue;
      if (r.r5 != count && r.r6 != count && r.r7 != count && CountForPrimitives(type, r.r7) != count &&
          CountForPrimitives(type, r.r6) != count) {
        continue;
      }
      const futari::renderer::ShaderEntry* vs = nullptr;
      const futari::renderer::ShaderEntry* ps = nullptr;
      LookupObjectEntries(r.vs, r.ps, vs, ps);
      pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(i + 1));
      ++draws_paired_;
      draw_vs_ = PreferVertexShaderCandidate(draw_vs_, vs, "pair-queue");
      if (ps && AcceptPixelShaderIdentity(ps, "pair-queue")) draw_ps_ = ps;
      paired_packets_[packet_address_] = {swaps_, draw_vs_, ps};
      return;
    }
    ++draws_unpaired_;
    if (logged_unpaired_ < 30 && draws_ > 60000 && type != 1) {
      ++logged_unpaired_;
      std::string p;
      for (size_t i = 0; i < pending_.size() && i < 4; ++i) {
        p += fmt::format(" [t{} {} {} {}]", pending_[i].type, pending_[i].r5, pending_[i].r6, pending_[i].r7);
      }
      REXLOG_INFO("[native] unpaired ring draw: type {} count {} source {}; pending {}:{}", type, count,
                  (initiator >> 6) & 3, pending_.size(), p);
    }
  }

  // A proven rectangle (CPU-executed VS) that overwrites every covered pixel of its views:
  // no discard, full sample mask, depth written with ALWAYS and stencil off, or color fully masked
  // without blending. The EDRAM tiles fully inside need no transfer from their previous owner
  // (xenia's "spurious ownership transfer round trips"; here the shadow maps and the scene share EDRAM).
  void ProveRectangleList(futari::renderer::SubmissionDraw& request, uint32_t rects) {
    if (!request.ps) return;
    // futari-nx (futari_ring_no_per_draw_copies): the estimator needs the register file with the draw initiator and
    // the index offset changed. It was a full copy of the register file per rectangle list (a heap allocation and
    // kRegisterCount words); the two words are now changed in place and restored on every way out. Nothing else in
    // the loop reads them (ProveFullOverwrite and the proof read other registers).
    static const bool in_place = REXCVAR_GET(futari_ring_no_per_draw_copies);
    std::vector<uint32_t> copy;
    if (!in_place) copy = registers_;
    std::vector<uint32_t>& regs = in_place ? registers_ : copy;
    const uint32_t initiator = regs[kRegVgtDrawInitiator];
    const uint32_t offset = regs[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET];
    struct Restore {
      std::vector<uint32_t>& r;
      uint32_t initiator, offset;
      ~Restore() {
        r[kRegVgtDrawInitiator] = initiator;
        r[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET] = offset;
      }
    } restore{regs, initiator, offset};
    regs[kRegVgtDrawInitiator] = (initiator & 0xFFFFu) | (3u << 16);
    std::optional<std::array<int32_t, 4>> best, bounds;
    int64_t best_area = -1;
    uint32_t slots = 0;
    uint32_t max_y = 0;
    for (uint32_t k = 0; k < rects; ++k) {
      regs[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET] = offset + 3u * k;
      NativeDrawExtentEstimator::Diagnostics diagnostic;
      const auto estimate = EstimateExtent(regs, &diagnostic);
      if (!estimate) { ++multi_rect_rejects_[0]; return; }  // every rectangle must be proven
      max_y = std::max(max_y, *estimate);
      StencilClearOwnershipInput proof;
      proof.depth_control = Register(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL);
      proof.stencil_front = Register(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK);
      proof.stencil_back = Register(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK_BF);
      proof.color_control = Register(rex::graphics::XE_GPU_REG_RB_COLORCONTROL);
      proof.su_sc_mode = Register(rex::graphics::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
      proof.vte_cntl = Register(rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL);
      const uint32_t tl = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
      const uint32_t br = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
      const uint32_t window_offset = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
      const auto sign15 = [](uint32_t v) { return int32_t(v & 0x7FFF) - int32_t((v & 0x4000) << 1); };
      const int32_t wx = (tl >> 31) ? 0 : sign15(window_offset);
      const int32_t wy = (tl >> 31) ? 0 : sign15(window_offset >> 16);
      const uint32_t screen_tl = Register(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL);
      const uint32_t screen_br = Register(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR);
      proof.scissor_x0 = uint32_t(std::max({0, int32_t(tl & 0x3FFF) + wx, sign15(screen_tl)}));
      proof.scissor_y0 = uint32_t(std::max({0, int32_t((tl >> 16) & 0x3FFF) + wy, sign15(screen_tl >> 16)}));
      proof.scissor_x1 = uint32_t(std::max(0, std::min(int32_t(br & 0x3FFF) + wx, sign15(screen_br))));
      proof.scissor_y1 = uint32_t(std::max(0, std::min(int32_t((br >> 16) & 0x3FFF) + wy, sign15(screen_br >> 16))));
      proof.post_vs_position = diagnostic.position;
      proof.xy_raster = diagnostic.xy_raster;
      proof.ps_no_discard = ProveStraightLinePixelNoKill(ps_microcode_) ||
          (REXCVAR_GET(futari_native_edram4_ps_no_kill) && ProvePixelNoKillAnywhere(ps_microcode_));
      proof.ps_no_depth_export = !(request.ps->outputs & 0x10);
      proof.sample_coverage_proven_full = (Register(rex::graphics::XE_GPU_REG_PA_SC_AA_MASK) & 0xFFFFu) == 0xFFFFu;
      futari::renderer::SubmissionDraw one = request;
      one.edram_overwrite_rect.reset();
      ProveFullOverwrite(one, proof);
      if (!one.edram_overwrite_rect) { ++multi_rect_rejects_[1]; return; }
      if (k && one.edram_overwrite_slots != slots) { ++multi_rect_rejects_[2]; return; }
      request.edram_overwrite_rects[k] = *one.edram_overwrite_rect;
      slots = one.edram_overwrite_slots;
      const auto& r = *one.edram_overwrite_rect;
      const int64_t area = int64_t(r[2] - r[0]) * (r[3] - r[1]);
      if (area > best_area) { best_area = area; best = r; }
      const auto& b = one.edram_bounds_rect ? *one.edram_bounds_rect : r;
      if (!bounds) bounds = b;
      else bounds = std::array<int32_t, 4>{std::min((*bounds)[0], b[0]), std::min((*bounds)[1], b[1]),
                                           std::max((*bounds)[2], b[2]), std::max((*bounds)[3], b[3])};
    }
    // Pixels the coverage rectangles exclude (partial pixels at the edges) are not overwritten, but they are
    // covered by the bounds only if covered at all: the proven rects are exactly the covered pixel sets.
    request.edram_used_height_estimate = max_y;
    request.edram_overwrite_rect = best;
    request.edram_overwrite_slots = slots;
    request.edram_bounds_rect = bounds;
    request.edram_overwrite_rect_count = rects;
    ++multi_rect_proven_;
  }
  uint64_t multi_rect_proven_ = 0;
  uint64_t extent_diagnostics_quiet_ = 0;  // draws since the "bounded draw extent" diagnostic last found a new key

  // futari_ring_extent_memo (see the cvar): the estimator's rejections that depend on the microcode alone.
  static bool StaticExtentReason(std::string_view reason) {
    static constexpr std::string_view kStatic[] = {
        "program-size", "cf-overlaps-body", "memory-export-alloc", "exec-bounds", "fetch-profile",
        "full-fetch-index", "mini-without-full", "fetch-format", "fetch-swizzle", "alu-profile",
        "alu-undefined-temp", "unsupported-export", "partial-position-export",
        "position-undefined-fetch-components", "alu-constant-operand", "index-temp-overwrite"};
    for (const auto& r : kStatic)
      if (reason == r) return true;
    return false;
  }
  std::optional<uint32_t> EstimateExtent(std::span<const uint32_t> regs, NativeDrawExtentEstimator::Diagnostics* d) {
    static const bool memo = REXCVAR_GET(futari_ring_extent_memo);
    const bool memo_now = memo && !AbOff("futari_ring_lean");  // in-run A/B: the skip only
    if (memo_now && extent_reject_generation_ == vs_identity_generation_) {
      ++extent_memo_skips_;
      if (extent_checks_left_ > 0 || direct_verify_) {  // the estimator must reject this draw too
        if (extent_checks_left_ > 0) --extent_checks_left_;
        ++extent_checks_;
        const auto estimate = draw_extent_estimator_->Estimate(regs, vs_microcode_, d);
        if (estimate) {
          ++extent_differences_;
          extent_reject_generation_ = 0;
          REXLOG_ERROR("[native] DIFFERENCE: futari_ring_extent_memo skipped a vertex shader the estimator accepts "
                       "(remembered reason {}); not remembered any more", extent_reject_reason_);
          return estimate;
        }
        return std::nullopt;
      }
      if (d) {
        *d = {};
        d->reason = extent_reject_reason_;
      }
      return std::nullopt;
    }
    const auto estimate = draw_extent_estimator_->Estimate(regs, vs_microcode_, d);
    if (memo && !estimate && d && StaticExtentReason(d->reason)) {
      extent_reject_generation_ = vs_identity_generation_;
      extent_reject_reason_ = d->reason;
    }
    return estimate;
  }
  uint64_t extent_reject_generation_ = 0;  // vs_identity_generation_ (starts at 1) of a static rejection
  std::string_view extent_reject_reason_;
  int32_t extent_checks_left_ = 4096;
  uint64_t extent_memo_skips_ = 0, extent_checks_ = 0, extent_differences_ = 0, report_extent_checks_ = 0,
           report_extent_differences_ = 0;
  std::unordered_map<uint64_t, const futari::renderer::ShaderEntry*> identity_memo_;
  uint64_t identity_memo_hits_ = 0;
  uint64_t exact_edges_ = 0;
  uint32_t edges_traces_ = 0, alias_traces_ = 0, traces_depth_ = 0;
  std::array<uint64_t, 3> multi_rect_rejects_{};  // estimator, overwrite proof, slot mismatch

  void ProveFullOverwrite(futari::renderer::SubmissionDraw& request, const StencilClearOwnershipInput& proof) {
    if (!REXCVAR_GET(futari_native_edram4_overwrite) || !request.ps) return;
    if (!proof.ps_no_discard) { ++overwrite_rejects_[0]; return; }
    if (!proof.sample_coverage_proven_full) { ++overwrite_rejects_[1]; return; }
    const auto& pos = proof.post_vs_position;
    float x0 = pos[0][0], x1 = pos[0][0], y0 = pos[0][1], y1 = pos[0][1];
    for (const auto& v : pos) {
      x0 = std::min(x0, v[0]); x1 = std::max(x1, v[0]);
      y0 = std::min(y0, v[1]); y1 = std::max(y1, v[1]);
    }
    // OpenGL pixel centers (PA_SU_VTX_CNTL.pix_center = 1, at i + 0.5): the same coverage as D3D centers
    // with every position half a pixel lower. The rest of the proof uses the D3D convention.
    if (REXCVAR_GET(futari_native_edram4_center_ogl) &&
        (Register(rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL) & 1)) {
      x0 -= 0.5f; x1 -= 0.5f; y0 -= 0.5f; y1 -= 0.5f;
    }
    // D3D9 pixel space: pixel i spans [i - 0.5, i + 0.5), so [-0.5, W - 0.5] covers pixels 0..W-1.
    // A small epsilon rejects rectangles that stop short of a pixel by rounding.
    constexpr float kEps = 1.0f / 64.0f;
    std::array<int32_t, 4> rect{int32_t(std::ceil(x0 + 0.5f - kEps)), int32_t(std::ceil(y0 + 0.5f - kEps)),
                                int32_t(std::floor(x1 + 0.5f + kEps)), int32_t(std::floor(y1 + 0.5f + kEps))};
    rect[0] = std::max<int32_t>(rect[0], int32_t(proof.scissor_x0));
    rect[1] = std::max<int32_t>(rect[1], int32_t(proof.scissor_y0));
    rect[2] = std::min<int32_t>(rect[2], int32_t(proof.scissor_x1));
    rect[3] = std::min<int32_t>(rect[3], int32_t(proof.scissor_y1));
    if (rect[2] <= rect[0] || rect[3] <= rect[1]) { ++overwrite_rejects_[2]; return; }
    uint32_t slots = 0;
    const uint32_t dc = proof.depth_control;
    const bool z_enable = dc & 2, z_write = dc & 4, stencil = dc & 1;
    // Stencil is fully replaced when every sample passes (func ALWAYS; depth func ALWAYS so zfail never
    // happens), the pass op is REPLACE and the write mask is 0xFF, on both faces (D3D Clear rectangles).
    const auto stencil_replaced = [&](uint32_t func, uint32_t zpass, uint32_t refmask) {
      // REPLACE (2) writes the reference, ZERO (1) writes 0: either way every bit gets a known value.
      return func == 7 && (zpass == 2 || zpass == 1) && ((refmask >> 16) & 0xFFu) == 0xFFu;
    };
    // An inert stencil (test ALWAYS, every op KEEP or write mask 0, both faces) neither kills nor writes: the
    // draw is a depth-only draw for the overwrite proof (the depth restore pass, Z ALWAYS + stencil ALWAYS/KEEP).
    const auto inert = [&](uint32_t func, uint32_t ops, uint32_t refmask) {
      return func == 7 && (ops == 0 || ((refmask >> 16) & 0xFFu) == 0);
    };
    const bool stencil_inert = stencil && REXCVAR_GET(futari_native_edram4_stencil_inert) &&
        inert((dc >> 8) & 7, (dc >> 11) & 0x1FF, proof.stencil_front) &&
        (!(dc & 0x80) || inert((dc >> 20) & 7, (dc >> 23) & 0x1FF, proof.stencil_back));
    bool stencil_ok = !stencil || stencil_inert;
    if (stencil && !stencil_inert) {
      stencil_ok = stencil_replaced((dc >> 8) & 7, (dc >> 14) & 7, proof.stencil_front) &&
                   (!(dc & 0x80) || stencil_replaced((dc >> 20) & 7, (dc >> 26) & 7, proof.stencil_back));
    }
    // Alpha test or alpha-to-coverage can kill samples: then neither depth nor stencil is fully written.
    const bool killable = ((proof.color_control >> 3) & 1) || ((proof.color_control >> 4) & 1);
    if (!killable && stencil && stencil_ok && !stencil_inert && (!z_enable || ((dc >> 4) & 7) == 7))
      request.edram_stencil_replace_rect = rect;  // every sample reaches the stencil pass op
    if (!killable && z_enable && z_write && ((dc >> 4) & 7) == 7 && stencil_ok) slots |= 1;
    else if ((dc & 6) == 6) ++overwrite_rejects_[3];
    const uint32_t edram_mode = Register(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 7;
    const bool alpha_test = (proof.color_control >> 3) & 1;
    if (edram_mode == uint32_t(xenos::EdramMode::kColorDepth) && !alpha_test) {
      const uint32_t mask = Register(rex::graphics::XE_GPU_REG_RB_COLOR_MASK);
      static constexpr uint32_t kBlend[4] = {rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0,
                                             rex::graphics::XE_GPU_REG_RB_BLENDCONTROL1,
                                             rex::graphics::XE_GPU_REG_RB_BLENDCONTROL2,
                                             rex::graphics::XE_GPU_REG_RB_BLENDCONTROL3};
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(request.ps->outputs & (1u << i))) continue;
        if (((mask >> (i * 4)) & 15u) == 15u && (Register(kBlend[i]) & 0x1FFF1FFFu) == 0x00010001u)
          slots |= 2u << i;
      }
    }
    {
      // Diagnostic: a clear whose depth and color 0 share the EDRAM base (D3D Clear of both through the 4x alias).
      const uint32_t di = Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO), ci = Register(rex::graphics::XE_GPU_REG_RB_COLOR_INFO);
      if ((di & 0xFFF) == (ci & 0xFFF) && alias_traces_ < 12) {
        ++alias_traces_;
        REXLOG_INFO("[native] aliased depth/color clear proof: slots {:X} mode {} dc {:08X} colorctl {:08X} mask {:08X} "
                    "blend0 {:08X} ps outputs {:X} depthinfo {:08X} colorinfo {:08X} rect {},{}-{},{} killable {}",
                    slots, edram_mode, dc, proof.color_control, Register(rex::graphics::XE_GPU_REG_RB_COLOR_MASK),
                    Register(rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0), request.ps->outputs, di, ci,
                    rect[0], rect[1], rect[2], rect[3], killable);
      }
    }
    if (!slots) { ++overwrite_rejects_[4]; return; }
    ++overwrite_rejects_[5];
    // Area bound for mode 4: rounded outward and padded by a pixel, so it can only over-cover (the
    // overwrite rect above is rounded inward on purpose).
    // Edges exactly on pixel boundaries (k - 0.5 with D3D pixel centers) touch no sample of the neighbouring
    // pixel at 1x or 4x (samples at least 0.25 inside), so the inward rect is also the exact bound.
    const auto in_edge = [](float v) {
      const float b = v + 0.5f;
      return std::abs(b - std::round(b)) < 1.0f / 1024.0f;
    };
    const uint32_t vte_xy = proof.vte_cntl;
    const bool w_one = proof.xy_raster || (vte_xy & 0x100) ||
                       (pos[0][3] == 1.0f && pos[1][3] == 1.0f && pos[2][3] == 1.0f);
    const bool exact = REXCVAR_GET(futari_native_edram4_exact_edges) && w_one &&
        (!(Register(rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL) & 1) || REXCVAR_GET(futari_native_edram4_center_ogl)) &&
        (!(proof.su_sc_mode & (1u << 16)) || !Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET)) &&
        in_edge(x0) && in_edge(x1) && in_edge(y0) && in_edge(y1);
    if (exact) ++exact_edges_;
    else if (REXCVAR_GET(futari_native_edram4_exact_edges) && edges_traces_ < 24) {
      ++edges_traces_;
      REXLOG_INFO("[native] edges not exact: x {}..{} y {}..{} w {} {} {} vte {:08X} vtx_cntl {:08X} su_sc {:08X} "
                  "rect {},{}-{},{} scissor {},{}-{},{}", x0, x1, y0, y1, pos[0][3], pos[1][3], pos[2][3], vte_xy,
                  Register(rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL), proof.su_sc_mode, rect[0], rect[1], rect[2],
                  rect[3], proof.scissor_x0, proof.scissor_y0, proof.scissor_x1, proof.scissor_y1);
    }
    request.edram_bounds_rect = exact ? rect : std::array<int32_t, 4>{
        std::max<int32_t>(int32_t(std::floor(x0 + 0.5f)) - 1, int32_t(proof.scissor_x0)),
        std::max<int32_t>(int32_t(std::floor(y0 + 0.5f)) - 1, int32_t(proof.scissor_y0)),
        std::min<int32_t>(int32_t(std::ceil(x1 + 0.5f)) + 1, int32_t(proof.scissor_x1)),
        std::min<int32_t>(int32_t(std::ceil(y1 + 0.5f)) + 1, int32_t(proof.scissor_y1))};
    request.edram_overwrite_rect = rect;
    request.edram_overwrite_slots = slots;
    // Constant depth: the same post-viewport Z at all three corners, no polygon offset, no PS depth
    // export. Lets mode 4 apply the rectangle as a depth clear in the current owner's view.
    if ((slots & 1) && proof.ps_no_depth_export && !(proof.su_sc_mode & 0x3800u)) {
      const uint32_t vte = proof.vte_cntl;
      const float zs = (vte & 0x10) ? std::bit_cast<float>(Register(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE))
                                    : 1.0f;
      const float zo = (vte & 0x20) ? std::bit_cast<float>(Register(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET))
                                    : 0.0f;
      std::optional<float> z;
      bool constant = true;
      for (const auto& v : pos) {
        float zf = v[2];
        if (!(vte & 0x200)) zf = (vte & 0x400) ? zf / v[3] : zf * v[3];
        zf = zf * zs + zo;
        // A hair below 0 (-3.7e-9 in ME's clears) stores the all-zero depth field in both encodings (unorm24
        // clamps, float24 maps z <= 0 to 0).
        if (zf < 0.0f && zf > -1.0e-6f) zf = 0.0f;
        if (!std::isfinite(zf) || zf < 0.0f || zf > 1.0f || (z && *z != zf)) { constant = false; break; }
        z = zf;
      }
      if (constant && z) request.edram_overwrite_depth = z;
    }
    if ((slots & 1) && !request.edram_overwrite_depth && traces_depth_ < 16) {
      ++traces_depth_;
      REXLOG_INFO("[native] constant depth not proven: depthinfo {:08X} ps_no_depth_export {} su_sc {:08X} vte {:08X} "
                  "z {} {} {} w {} {} {} zscale {} zoffset {}", Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                  proof.ps_no_depth_export, proof.su_sc_mode, proof.vte_cntl, pos[0][2], pos[1][2], pos[2][2],
                  pos[0][3], pos[1][3], pos[2][3],
                  std::bit_cast<float>(Register(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE)),
                  std::bit_cast<float>(Register(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET)));
    }
  }

  std::array<uint64_t, 6> overwrite_rejects_{};  // discard, sample mask, empty rect, depth/stencil, no slot, ok
  // futari-nx: futari_pc_swap_limit_hz. On the PC the native renderer's present never blocks, so the game runs
  // uncapped (~1800 frames a second in Xbox 360 mode) and scripted test runs fast-forward through the gameplay. The
  // Switch is paced by its FIFO swapchain. This holds each guest swap to the given rate on the ring thread (test
  // runs and PC profiles that should look like the console); 0 = off.
  void LimitSwaps() {
    static const int32_t hz = REXCVAR_GET(futari_pc_swap_limit_hz);
    if (hz <= 0) return;
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1000000000 / hz);
    const auto now = Clock::now();
    if (swap_deadline_ == Clock::time_point{} || now > swap_deadline_ + period * 4) {
      swap_deadline_ = now + period;  // first swap, or far behind: restart the cadence
      return;
    }
    if (now < swap_deadline_) std::this_thread::sleep_until(swap_deadline_);
    swap_deadline_ += period;
  }
  std::chrono::steady_clock::time_point swap_deadline_{};

  void TraceSwap() {
#if !REX_PLATFORM_SWITCH && !REX_PLATFORM_MAC
    const int32_t capture_at = REXCVAR_GET(banjo_renderdoc_capture_s);
    if (renderdoc_capturing_) {
      renderdoc_capturing_ = false;
      REXLOG_INFO("[native] RenderDoc capture of swap {} ended: {}", swaps_, banjo::renderdoc::End() ? "ok" : "FAILED");
    }
    if (capture_at > 0 && !renderdoc_triggered_ && Clock::now() - start_ >= std::chrono::seconds(capture_at)) {
      renderdoc_triggered_ = true;
      const auto folder = rex::filesystem::GetExecutableFolder() / "capturas" / "rdc";
      std::error_code ec;
      std::filesystem::create_directories(folder, ec);
      if (banjo::renderdoc::Start((folder / "banjo").string().c_str())) {
        renderdoc_capturing_ = true;
        REXLOG_INFO("[native] RenderDoc capture started at swap {} ({})", swaps_, folder.string());
      } else {
        REXLOG_WARN("[native] RenderDoc capture requested but RenderDoc is not loaded");
      }
    }
#endif
    const int32_t at = REXCVAR_GET(banjo_trace_frame_s);
    if (trace_phase_ == 0 && at > 0 && Clock::now() - start_ >= std::chrono::seconds(at)) {
      trace_phase_ = 1;
      REXLOG_INFO("[trace] ---- frame after swap {} ----", swaps_);
    } else if (trace_phase_ == 1) {
      trace_phase_ = 2;
      REXLOG_INFO("[trace] ---- end at swap {}: {} draws ({} without resolve sampling not listed) ----", swaps_,
                  trace_draws_, trace_quiet_draws_);
    }
  }

  void TraceCopy() {
    namespace g = rex::graphics;
    const uint32_t surface = Register(g::XE_GPU_REG_RB_SURFACE_INFO), color = Register(g::XE_GPU_REG_RB_COLOR_INFO);
    const uint32_t depth = Register(g::XE_GPU_REG_RB_DEPTH_INFO), control = Register(g::XE_GPU_REG_RB_COPY_CONTROL);
    const uint32_t pitch = Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH), info = Register(g::XE_GPU_REG_RB_COPY_DEST_INFO);
    const uint32_t tl = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL), br = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    REXLOG_INFO("[trace] #{} RESOLVE src {} (color base {} fmt {} / depth base {}) pitch {} msaa {} -> {:08X} {}x{} "
                "fmt {} | control {:08X} (clear c{} d{}) clear {:08X} | window {},{}-{},{} offset {:08X}",
                draws_, (control & 7) == 4 ? "depth" : "color", color & 0xFFF, (color >> 16) & 0xF, depth & 0xFFF,
                surface & 0x3FFF, (surface >> 16) & 3, Register(g::XE_GPU_REG_RB_COPY_DEST_BASE), pitch & 0x3FFF,
                (pitch >> 16) & 0x3FFF, (info >> 7) & 0x3F, control, (control >> 8) & 1, (control >> 9) & 1,
                Register(g::XE_GPU_REG_RB_COLOR_CLEAR), tl & 0x3FFF, (tl >> 16) & 0x3FFF, br & 0x3FFF,
                (br >> 16) & 0x3FFF, Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET));
  }

  void TraceDraw() {
    namespace g = rex::graphics;
    ++trace_draws_;
    const uint32_t target[4] = {Register(g::XE_GPU_REG_RB_SURFACE_INFO), Register(g::XE_GPU_REG_RB_COLOR_INFO),
                                Register(g::XE_GPU_REG_RB_DEPTH_INFO), Register(kRegRbModeControl)};
    if (std::memcmp(target, trace_target_, sizeof(target))) {
      std::memcpy(trace_target_, target, sizeof(target));
      REXLOG_INFO("[trace] #{} TARGET pitch {} msaa {} color base {} fmt {} depth base {} fmt {} mode {}", draws_,
                  target[0] & 0x3FFF, (target[0] >> 16) & 3, target[1] & 0xFFF, (target[1] >> 16) & 0xF,
                  target[2] & 0xFFF, (target[2] >> 16) & 1, target[3] & 7);
    }
    std::string textures;
    uint32_t used = 0;  // texture slots the pixel shader samples (all slots when it is unknown)
    if (draw_ps_)
      for (const auto& sampler : draw_ps_->samplers) used |= 1u << (sampler.register_value & 31);
    else
      used = ~0u;
    bool any_resolved = false;
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (!(used >> slot & 1)) continue;
      const uint32_t* f = &registers_[g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6];
      if ((f[0] & 3) != 2) continue;
      const uint32_t base = f[1] & 0x1FFFF000u;
      any_resolved |= resolve_destinations_.count(base) != 0;
      textures += fmt::format(" t{}={:08X} {}x{} fmt {} pitch {} tiled {}", slot, base, (f[2] & 0x1FFF) + 1,
                              ((f[2] >> 13) & 0x1FFF) + 1, f[1] & 0x3F, ((f[0] >> 22) & 0x1FF) << 5,
                              (f[0] >> 31) & 1);
    }
    // Every draw into the final 1280-wide targets is listed; others only when they sample a resolve destination.
    // sa2-nx: FUTARI_TRACE_ALL_DRAWS=1 lists every draw of the traced frame (the clears that D3D draws with its
    // own shaders go into other targets and were never listed).
    static const bool trace_all = [] {
      const char* v = std::getenv("FUTARI_TRACE_ALL_DRAWS");
      return v && *v && *v != '0';
    }();
    if (!trace_all && !any_resolved && (target[0] & 0x3FFF) != 1280) {
      ++trace_quiet_draws_;
      return;
    }
    const uint32_t tl = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL), br = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    // futari-nx: the vertex shader's c255 (Futari's sprite shaders scale integer coordinates with it), the viewport
    // and the clip control, as floats/hex.
    {
      float c255[4];
      for (int i = 0; i < 4; ++i) {
        const uint32_t bits = Register(g::XE_GPU_REG_SHADER_CONSTANT_000_X + 255 * 4 + i);
        std::memcpy(&c255[i], &bits, 4);
      }
      REXLOG_INFO("[trace] #{}   vs c255 {} {} {} {} | vte {:08X} clip {:08X} vport scale {:08X} {:08X} offset {:08X} "
                  "{:08X}", draws_, c255[0], c255[1], c255[2], c255[3], Register(g::XE_GPU_REG_PA_CL_VTE_CNTL),
                  Register(g::XE_GPU_REG_PA_CL_CLIP_CNTL), Register(g::XE_GPU_REG_PA_CL_VPORT_XSCALE),
                  Register(g::XE_GPU_REG_PA_CL_VPORT_YSCALE), Register(g::XE_GPU_REG_PA_CL_VPORT_XOFFSET),
                  Register(g::XE_GPU_REG_PA_CL_VPORT_YOFFSET));
    }
    REXLOG_INFO("[trace] #{} DRAW {} VS n{} PS n{} prim {} count {} scissor {},{}-{},{} blend0 {:08X} mask {:08X} "
                "depth {:08X} c0 {:08X} {:08X} {:08X} {:08X}:{}", draws_,
                trace_result_ == 1 ? "ok" : trace_result_ == 0 ? fmt::format("REJ{}", g_banjo_last_reject_cause)
                                                                : std::string("SKIP"),
                draw_vs_ ? int64_t(draw_vs_->number) : -1, draw_ps_ ? int64_t(draw_ps_->number) : -1,
                Register(kRegVgtDrawInitiator) & 63, Register(kRegVgtDrawInitiator) >> 16, tl & 0x3FFF,
                (tl >> 16) & 0x3FFF, br & 0x3FFF, (br >> 16) & 0x3FFF, Register(g::XE_GPU_REG_RB_BLENDCONTROL0),
                Register(g::XE_GPU_REG_RB_COLOR_MASK), Register(g::XE_GPU_REG_RB_DEPTHCONTROL),
                Register(g::XE_GPU_REG_SHADER_CONSTANT_256_X), Register(g::XE_GPU_REG_SHADER_CONSTANT_256_Y),
                Register(g::XE_GPU_REG_SHADER_CONSTANT_256_Z), Register(g::XE_GPU_REG_SHADER_CONSTANT_256_W),
                textures);
  }

  void Draw() {
    if (!MatchesVertexShaderIdentity(draw_vs_) && MatchesVertexShaderIdentity(vs_))
      draw_vs_ = PreferVertexShaderCandidate(draw_vs_, vs_, "draw-final-ring-preference");
    if (draw_vs_ && !AcceptVertexShaderIdentity(draw_vs_, "draw-final")) {
      // No unchecked fallback: the last loaded variant is the only authority.
      draw_vs_ = vs_ && AcceptVertexShaderIdentity(vs_, "draw-final-ring") ? vs_ : nullptr;
      if (!draw_vs_) { ++draws_unidentified_; return; }
    }
    if (!EnsureTargets()) return;
    // Candidate warnings include discarded object/queue alternatives. Count
    // the actual final selection separately, without changing rollout policy.
    if (!draw_vs_) {
      ++vs_final_missing_;
    } else if (MatchesVertexShaderIdentity(draw_vs_)) {
      ++vs_final_proven_;
    } else {
      const uint64_t unproven = ++vs_final_unproven_;
      if (unproven <= 8 || (unproven & 255) == 0) {
        REXLOG_WARN("[native] FINAL VS unproven {} draw={} loaded_host_hash={:016X} "
                    "loaded_words={} selected_n={} selected_container={:016X}",
                    unproven, draws_, current_vs_hash_, vs_microcode_.size(),
                    draw_vs_->number, draw_vs_->shader ? draw_vs_->shader->fingerprint : 0);
      }
    }
    futari::renderer::SubmissionDraw request;
    request.registers = registers_.data();
    request.vs = draw_vs_;
    request.ps = draw_ps_;
    request.vs_microcode = vs_microcode_;
    request.ps_microcode = ps_microcode_;
    request.generation_vs = gen_vs_;
    request.generation_constants_vs = gen_constants_vs_;
    request.generation_constants_ps = gen_constants_ps_;
    request.generation_fetch = gen_fetch_;
    request.generation_framing = gen_viewport_;
    if (memory_ &&
        ((Register(rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL) & 0x10000u) ||
         REXCVAR_GET(futari_native_edram4_clip_inside)) &&
        MatchesVertexShaderIdentity(draw_vs_)) {
      const uint32_t initiator = Register(kRegVgtDrawInitiator);
      // The adapter performs the full admission proof. This outer filter avoids
      // copying/analyzing unrelated normal draws when the opt-in is enabled.
      const uint32_t rect_vertices = initiator >> 16;
      if ((initiator & 63u) == uint32_t(xenos::PrimitiveType::kRectangleList) &&
          ((initiator >> 6) & 3u) == uint32_t(xenos::SourceSelect::kAutoIndex) &&
          rect_vertices > 3u && rect_vertices % 3u == 0 && rect_vertices <= 24u &&
          REXCVAR_GET(futari_native_edram4_several_rect)) {
        // Rect lists of several rectangles (D3D Clear of two regions): prove each one with the same
        // single-rectangle estimator on a register copy whose index offset selects that rectangle.
        if (!draw_extent_estimator_) { draw_extent_estimator_ = std::make_unique<NativeDrawExtentEstimator>(*memory_); draw_extent_estimator_->AllowClipInside(REXCVAR_GET(futari_native_edram4_clip_inside)); draw_extent_estimator_->AllowQuadTriangles(REXCVAR_GET(futari_native_edram4_quad_triangles)); draw_extent_estimator_->AllowAlu(REXCVAR_GET(futari_native_edram4_vs_alu)); }
        ProveRectangleList(request, rect_vertices / 3u);
      } else if ((((initiator >> 6) & 3u) == uint32_t(xenos::SourceSelect::kAutoIndex) ||
                  (((initiator >> 6) & 3u) == uint32_t(xenos::SourceSelect::kDMA) &&
                   (initiator & 63u) != uint32_t(xenos::PrimitiveType::kRectangleList))) &&
          (((initiator & 63u) == uint32_t(xenos::PrimitiveType::kRectangleList) && (initiator >> 16) == 3u) ||
           (REXCVAR_GET(futari_native_edram4_quad_triangles) &&
            (((initiator & 63u) == uint32_t(xenos::PrimitiveType::kTriangleList) && (initiator >> 16) == 6u) ||
             ((initiator & 63u) == uint32_t(xenos::PrimitiveType::kTriangleStrip) && (initiator >> 16) == 4u))))) {
        if (!draw_extent_estimator_) { draw_extent_estimator_ = std::make_unique<NativeDrawExtentEstimator>(*memory_); draw_extent_estimator_->AllowClipInside(REXCVAR_GET(futari_native_edram4_clip_inside)); draw_extent_estimator_->AllowQuadTriangles(REXCVAR_GET(futari_native_edram4_quad_triangles)); draw_extent_estimator_->AllowAlu(REXCVAR_GET(futari_native_edram4_vs_alu)); }
        NativeDrawExtentEstimator::Diagnostics diagnostic;
        request.edram_used_height_estimate = EstimateExtent(registers_, &diagnostic);
        if (request.edram_used_height_estimate && request.ps) {
          StencilClearOwnershipInput proof;
          proof.depth_control = Register(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL);
          proof.stencil_front = Register(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK);
          proof.stencil_back = Register(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK_BF);
          proof.color_control = Register(rex::graphics::XE_GPU_REG_RB_COLORCONTROL);
          proof.su_sc_mode = Register(rex::graphics::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
          proof.vtx_cntl = Register(rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL);
          proof.vte_cntl = Register(rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL);
          proof.window_offset = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
          proof.clip_cntl = Register(rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL);
          const uint32_t surface = Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
          proof.msaa_enum = (surface >> 16) & 3;
          proof.physical_pitch_tiles = (((surface & 0x3FFF) << uint32_t(proof.msaa_enum == 2)) + 79) / 80;
          const uint32_t tl = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
          const uint32_t br = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
          const auto sign15 = [](uint32_t v) { return int32_t(v & 0x7FFF) - int32_t((v & 0x4000) << 1); };
          const int32_t wx = (tl >> 31) ? 0 : sign15(proof.window_offset);
          const int32_t wy = (tl >> 31) ? 0 : sign15(proof.window_offset >> 16);
          const uint32_t screen_tl = Register(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL);
          const uint32_t screen_br = Register(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR);
          proof.scissor_x0 = uint32_t(std::max({0, int32_t(tl & 0x3FFF) + wx, sign15(screen_tl)}));
          proof.scissor_y0 = uint32_t(std::max({0, int32_t((tl >> 16) & 0x3FFF) + wy, sign15(screen_tl >> 16)}));
          proof.scissor_x1 = uint32_t(std::max(0, std::min(int32_t(br & 0x3FFF) + wx, sign15(screen_br))));
          proof.scissor_y1 = uint32_t(std::max(0, std::min(int32_t((br >> 16) & 0x3FFF) + wy, sign15(screen_br >> 16))));
          proof.post_vs_position = diagnostic.position;
      proof.xy_raster = diagnostic.xy_raster;
          proof.rectangle_proven = true;  // Successful bounded adapter proof, not bbox-only.
          proof.ps_identity_proven = MatchesPixelShaderIdentity(request.ps);
          // Counting SPIR-V OpKill is not proof: an optimizer may merge guest
          // and alpha-test kills. Inspect every admitted loaded ALU instead.
          proof.ps_no_discard = ProveStraightLinePixelNoKill(ps_microcode_) ||
          (REXCVAR_GET(futari_native_edram4_ps_no_kill) && ProvePixelNoKillAnywhere(ps_microcode_));
          proof.ps_no_depth_export = !(request.ps->outputs & 0x10);
          // Admit only the all-enabled 16-bit AA mask, never an unknown mask.
          // The native pipeline has no additional programmable sample mask.
          proof.sample_coverage_proven_full = (Register(rex::graphics::XE_GPU_REG_PA_SC_AA_MASK) & 0xFFFFu) == 0xFFFFu;
          request.edram_stencil_clear = ProveStencilClearOwnership(proof);
          ProveFullOverwrite(request, proof);
          if ((proof.depth_control & 7) == 1 && stencil_clear_diagnostics_++ < 16)
            REXLOG_INFO("[native] canonical stencil clear candidate: VS={} PS={} eligible={} reason={} "
                        "aaMask={:08X} physical={}x{} tiles={} ref={}",
                        request.vs->number, request.ps->number, request.edram_stencil_clear.eligible,
                        request.edram_stencil_clear.reason, Register(rex::graphics::XE_GPU_REG_PA_SC_AA_MASK),
                        request.edram_stencil_clear.physical_width, request.edram_stencil_clear.physical_height,
                        request.edram_stencil_clear.length_tiles, request.edram_stencil_clear.stencil_reference);
        }
        // futari-nx (futari_ring_no_per_draw_copies): a diagnostic that looks for new keys for ever (Futari never reaches
        // 32) stops after 100,000 draws without a new one.
        static const bool quiet_cap = REXCVAR_GET(futari_ring_no_per_draw_copies);
        if (extent_diagnostics_.size() < 32 && (!quiet_cap || extent_diagnostics_quiet_ < 100000)) {
          ++extent_diagnostics_quiet_;
          // futari-nx (futari_ring_no_per_draw_copies): the key was a fmt::format string built on EVERY draw until 32
          // distinct ones were seen, which Futari never reaches. Same keys, as a hash of the same fields.
          static const bool by_value = REXCVAR_GET(futari_ring_no_per_draw_copies);
          uint64_t key;
          if (by_value) {
            const uint64_t fields[4] = {current_vs_hash_, Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                                        Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO),
                                        request.edram_used_height_estimate.value_or(UINT32_MAX)};
            key = XXH3_64bits_withSeed(fields, sizeof(fields),
                                       XXH3_64bits(diagnostic.reason.data(), diagnostic.reason.size()));
          } else {
            const std::string text = fmt::format("{:X}/{:X}/{}/{}/{}", current_vs_hash_,
                Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO),
                request.edram_used_height_estimate.value_or(UINT32_MAX), diagnostic.reason);
            key = XXH3_64bits(text.data(), text.size());
          }
          if (extent_diagnostics_.insert(key).second && (extent_diagnostics_quiet_ = 0, true))
            REXLOG_INFO("[native] bounded draw extent: code={:016X} VS={} depth={:08X} surface={:08X} "
                        "accepted={} maxY={} reason={} postVS=[{},{},{},{}; {},{},{},{}; {},{},{},{}]",
                        current_vs_hash_, request.vs->number,
                        Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                        Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO),
                        bool(request.edram_used_height_estimate),
                        request.edram_used_height_estimate.value_or(UINT32_MAX), diagnostic.reason,
                        diagnostic.position[0][0], diagnostic.position[0][1], diagnostic.position[0][2], diagnostic.position[0][3],
                        diagnostic.position[1][0], diagnostic.position[1][1], diagnostic.position[1][2], diagnostic.position[1][3],
                        diagnostic.position[2][0], diagnostic.position[2][1], diagnostic.position[2][2], diagnostic.position[2][3]);
        }
      }
    }
    ++native_draw_attempts_;
    g_banjo_last_reject_cause = 0;
    const bool drawn = targets_->Draw(request);
    trace_result_ = drawn ? 1 : 0;
    if (!drawn) {
      if (++native_draw_failures_ <= 8)
        REXLOG_WARN("[native] draw rejected before completion; visual conformance is not established");
    }
  }

  void Copy() {
    if (!EnsureTargets()) return;
    namespace g = rex::graphics;
    futari::renderer::RegistersCopy r;
    r.rb_surface_info = Register(g::XE_GPU_REG_RB_SURFACE_INFO);
    r.rb_color_info[0] = Register(g::XE_GPU_REG_RB_COLOR_INFO);
    r.rb_color_info[1] = Register(g::XE_GPU_REG_RB_COLOR1_INFO);
    r.rb_color_info[2] = Register(g::XE_GPU_REG_RB_COLOR2_INFO);
    r.rb_color_info[3] = Register(g::XE_GPU_REG_RB_COLOR3_INFO);
    r.rb_depth_info = Register(g::XE_GPU_REG_RB_DEPTH_INFO);
    r.rb_copy_control = Register(g::XE_GPU_REG_RB_COPY_CONTROL);
    r.rb_copy_dest_base = Register(g::XE_GPU_REG_RB_COPY_DEST_BASE);
    r.rb_copy_dest_pitch = Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH);
    r.rb_copy_dest_info = Register(g::XE_GPU_REG_RB_COPY_DEST_INFO);
    r.rb_color_clear = Register(g::XE_GPU_REG_RB_COLOR_CLEAR);
    r.rb_color_clear_lo = Register(g::XE_GPU_REG_RB_COLOR_CLEAR_LO);
    r.rb_depth_clear = Register(g::XE_GPU_REG_RB_DEPTH_CLEAR);
    r.pa_sc_window_offset = Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    r.pa_sc_window_scissor_tl = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
    r.pa_sc_window_scissor_br = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    r.pa_su_sc_mode_cntl = Register(g::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
    r.pa_su_vtx_cntl = Register(g::XE_GPU_REG_PA_SU_VTX_CNTL);
    r.fetch_vertices[0] = Register(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
    r.fetch_vertices[1] = Register(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1);
    ++native_copy_attempts_;
    if (!targets_->Copy(r)) {
      if (++native_copy_failures_ <= 8)
        REXLOG_WARN("[native] copy/clear rejected before completion; visual conformance is not established");
    }
  }

  void Present() {
    if (!presenter_ || !EnsureTargets()) return;
    if (gamma_sent_version_ != gamma_version_) {
      gamma_sent_version_ = gamma_version_;
      targets_->RampGamma(gamma_ramp_);
      if (!gamma_logged_) {
        gamma_logged_ = true;
        REXLOG_INFO("[native] game gamma ramp: {} writes, [0]={}/{}/{}, [64]={}/{}/{}, "
                    "[128]={}/{}/{}, [255]={}/{}/{}",
                    gamma_writes_, gamma_ramp_[0][0], gamma_ramp_[0][1], gamma_ramp_[0][2],
                    gamma_ramp_[64][0], gamma_ramp_[64][1], gamma_ramp_[64][2],
                    gamma_ramp_[128][0], gamma_ramp_[128][1], gamma_ramp_[128][2],
                    gamma_ramp_[255][0], gamma_ramp_[255][1], gamma_ramp_[255][2]);
      }
    }
    futari::renderer::TextureSwap texture;
    for (uint32_t i = 0; i < 6; ++i) {
      texture.dword[i] = Register(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i);
    }
    if (targets_->Present(presenter_.get(), texture, swap_width_, swap_height_)) {
      ++presented_;
    }
  }

  void Report(bool force) {
    const auto now = Clock::now();
    if (!force && now - last_report_ < std::chrono::seconds(10)) return;
    const double secs = std::chrono::duration<double>(now - last_report_).count();
    last_report_ = now;
    // GPU time per category, real milliseconds per Swap of this interval: raw NVK timestamps x 1.627 on the Switch.
    if (targets_) {
      std::array<uint64_t, futari::renderer::kGpuCategories> gpu{}, frag{}, vert{}, prim{};
      targets_->TimeGpuPerCategory(gpu);
      targets_->StatsPipeline(frag, vert, prim);
      constexpr double kScale = 1.627;
      const uint64_t swaps = swaps_ - report_gpu_swaps_;
      if (swaps) {
        static constexpr const char* kNames[futari::renderer::kGpuCategories] = {
            "other",  "shadows",    "scene",        "640",         "320",         "smaller",     "copies",
            "clears", "scene_no_z", "gap",         "edram_import", "edram_export", "edram_alias", "edram_import9"};
        std::string t;
        double total = 0;
        for (uint32_t i = 0; i < futari::renderer::kGpuCategories; ++i) {
          const double ms = double(gpu[i] - report_gpu_ns_[i]) * kScale / 1e6 / double(swaps);
          total += ms;
          t += fmt::format(" {} {:.1f} ms ({:.2f} Mfrag)", kNames[i], ms,
                           double(frag[i] - report_gpu_frag_[i]) / 1e6 / double(swaps));
        }
        REXLOG_INFO("[native] GPU per Swap ({} Swaps): total {:.1f} ms;{}", swaps, total, t);
        REXLOG_INFO("[native] overwrite proof: discard {} samplemask {} emptyrect {} depthstencil {} noslot {} ok {}",
                    overwrite_rejects_[0], overwrite_rejects_[1], overwrite_rejects_[2], overwrite_rejects_[3],
                    overwrite_rejects_[4], overwrite_rejects_[5]);
        overwrite_rejects_ = {};
      }
      report_gpu_ns_ = gpu;
      report_gpu_frag_ = frag;
      report_gpu_swaps_ = swaps_;
    }
    {  // PM4 mix of this interval (what the ring parses per frame): type 0 and the busiest type 3 opcodes
      std::array<std::pair<uint64_t, uint32_t>, 128> order;
      for (uint32_t i = 0; i < 128; ++i) order[i] = {opcodes_[i], i};
      std::partial_sort(order.begin(), order.begin() + 8, order.end(), std::greater<>());
      std::string mix;
      for (uint32_t i = 0; i < 8 && order[i].first; ++i)
        mix += fmt::format(" {:02X}:{}pk/{}w", order[i].second, order[i].first, opcode_words_[order[i].second]);
      REXLOG_INFO("[native] PM4 mix (cumulative): {} packets, type 0: {} ({} words, {} single-register); type 3 opcodes "
                  "(packets/words):{}", packets_, type0_packets_, type0_words_, type0_single_, mix);
    }
    {  // The ring thread's own time of this interval: its parse loop, less the WAIT_REG_MEM waits for the game.
      const uint64_t swaps = swaps_ - report_ring_swaps_, draws = draws_ - report_ring_draws_;
      const double busy = double(ring_busy_ns_ - report_ring_busy_ns_) / 1e6;
      const double wait = double(ring_wait_ns_ - report_ring_wait_ns_) / 1e6;
      if (swaps)
        REXLOG_INFO("[native] ring thread: {:.2f} ms of work per Swap ({:.2f} us per draw, {} draws per Swap), "
                    "{:.2f} ms per Swap waiting for the game (WAIT_REG_MEM)",
                    (busy - wait) / double(swaps), draws ? (busy - wait) * 1000.0 / double(draws) : 0.0,
                    draws / swaps, wait / double(swaps));
      report_ring_swaps_ = swaps_;
      report_ring_draws_ = draws_;
      report_ring_busy_ns_ = ring_busy_ns_;
      report_ring_wait_ns_ = ring_wait_ns_;
    }
    ReportCensus();
    ::futari::api::Report();  // futari_api_*: the native Direct3D layer's game-thread side
    if (api_applied_ || api_lost_)
      REXLOG_INFO("[native] native Direct3D records applied by the ring (cumulative): {}, not found {}", api_applied_,
                  api_lost_);
    if (extent_memo_skips_) {
      REXLOG_INFO("[native] extent memo (futari_ring_extent_memo): {} estimator runs skipped (cumulative), {} "
                  "checked", extent_memo_skips_, extent_checks_);
      ::futari::native::ReportVerify("extent memo", extent_checks_ - report_extent_checks_,
                                     extent_differences_ - report_extent_differences_);
      report_extent_checks_ = extent_checks_;
      report_extent_differences_ = extent_differences_;
    }
    if (direct_ab_s_ > 0) {
      const auto us = [](uint64_t ticks, uint64_t draws) {
        return draws ? double(ticks) / direct::kAbScale / double(draws) : 0.0;
      };
      const uint64_t wn0 = ab_ns_[0] - ab_reported_ns_[0], wn1 = ab_ns_[1] - ab_reported_ns_[1];
      const uint64_t wd0 = ab_draws_[0] - ab_reported_draws_[0], wd1 = ab_draws_[1] - ab_reported_draws_[1];
      REXLOG_INFO("[native] A/B of {} (alternating every {} s; the ring thread's own work) last 10 s: on {:.3f} {} "
                  "per draw ({} draws), off {:.3f} ({} draws), {:+.1f} % | cumulative: on {:.3f} over {}, off {:.3f} "
                  "over {}",
                  REXCVAR_GET(futari_ab_key), direct_ab_s_, us(wn0, wd0), direct::kAbUnit, wd0, us(wn1, wd1), wd1,
                  us(wn1, wd1) > 0 ? 100.0 * (us(wn0, wd0) / us(wn1, wd1) - 1.0) : 0.0, us(ab_ns_[0], ab_draws_[0]),
                  ab_draws_[0], us(ab_ns_[1], ab_draws_[1]), ab_draws_[1]);
      for (int i = 0; i < 2; ++i) ab_reported_ns_[i] = ab_ns_[i], ab_reported_draws_[i] = ab_draws_[i];
    }
    if (REXCVAR_GET(futari_cpu_direct_state)) {
      using namespace direct;
      const uint64_t converted = g_converted.load(), checked = g_checked.load(),
                     check_differences = g_check_differences.load();
      REXLOG_INFO("[native] direct state (cumulative): {} draws converted of {} hooked draw calls ({:.1f} % of the "
                  "ring's {} draws; {} words of PM4 not parsed), {} records applied, {} replayed, {} passed "
                  "unreached, {} lost{}; left as PM4: submitted {}, segment {}, unknown packet {} (last opcode {:02X}), "
                  "NOP head {}, small {}, no draw {}, ring full {}, other drawing thread {}; {} records checked at the "
                  "draw ({} differences) and {} again by the ring ({} differences); submissions that waited for a "
                  "marker {}",
                  converted, g_calls.load(), draws_ ? 100.0 * double(converted) / double(draws_) : 0.0, draws_,
                  g_words_covered.load(), direct_applied_, direct_replayed_, direct_skipped_, direct_lost_,
                  g_failed.load() ? " (STOPPED)" : "", g_skip_submitted.load(), g_skip_segment.load(),
                  g_skip_unknown.load(), g_unknown_opcode.load(), g_skip_nop_head.load(), g_skip_small.load(),
                  g_skip_no_draw.load(), g_skip_full.load(), g_skip_thread.load(), checked, check_differences,
                  direct_ring_checks_, direct_ring_differences_, g_kick_waits.load());
      // The "[verify]" line: the decoder's re-encode checks, and every marker finding its record.
      ::futari::native::ReportVerify("direct records", checked - report_direct_checked_,
                                     check_differences - report_direct_check_differences_);
      ::futari::native::ReportVerify("direct markers", direct_applied_ - report_direct_applied_,
                                     direct_lost_ - report_direct_lost_);
      ::futari::native::ReportVerify("direct runs", direct_run_checks_ - report_direct_run_checks_,
                                     direct_run_differences_ - report_direct_run_differences_);
      report_direct_run_checks_ = direct_run_checks_;
      ::futari::native::ReportVerify("direct regions", direct_ring_checks_ - report_direct_ring_checks_,
                                     direct_ring_differences_ - report_direct_ring_differences_);
      report_direct_ring_checks_ = direct_ring_checks_;
      report_direct_ring_differences_ = direct_ring_differences_;
      report_direct_run_differences_ = direct_run_differences_;
      report_direct_checked_ = checked;
      report_direct_check_differences_ = check_differences;
      report_direct_applied_ = direct_applied_;
      report_direct_lost_ = direct_lost_;
    }
    REXLOG_INFO("[native] shader loads reused (identical reload): {}; multi-rect draws proven: {} (rejected: "
                "estimator {}, overwrite proof {}, slot mismatch {}); pixel-exact bounds: {}; identity memo hits {}",
                shader_loads_reused_,
                multi_rect_proven_, multi_rect_rejects_[0], multi_rect_rejects_[1], multi_rect_rejects_[2],
                exact_edges_, identity_memo_hits_);
    identity_memo_hits_ = 0;
    multi_rect_proven_ = 0;
    exact_edges_ = 0;
    multi_rect_rejects_ = {};
    shader_loads_reused_ = 0;
    REXLOG_INFO("[native] PS identity audit: {} mismatching candidate checks "
                "(all selection paths, cumulative; not distinct draws)", ps_identity_mismatches_);
    REXLOG_INFO("[native] VS identity audit: {} mismatching candidate checks "
                "(declared fetch patches only; all ALU/CF words compared; cumulative)", vs_identity_mismatches_);
    REXLOG_INFO("[native] FINAL VS selection audit: {} proven, {} unproven, {} missing "
                "(cumulative attempted draws; not GPU/DEF/ABI equivalence proof)",
                vs_final_proven_, vs_final_unproven_, vs_final_missing_);
    if (shaders_reordered_ || diag_reorder_swaps_)
      REXLOG_INFO("[native] VS loads identified with their fetches reordered by Direct3D (futari_gpu_vs_fetch_reorder): "
                  "{} (cumulative); test swaps made (futari_diag_vs_reorder_swap): {}", shaders_reordered_,
                  diag_reorder_swaps_);
    futari::renderer::ReportReorderVerify();
    uint64_t copies = 0, clears = 0, presented = 0, rejected = 0;
    if (targets_) targets_->Stats(copies, clears, presented, rejected);
    const auto st = targets_ ? targets_->StatsOfDraws() : futari::renderer::StatsDraws{};
    REXLOG_INFO("[native] completion coverage: draw failures {}/{}; copy/clear failures {}/{} "
                "(includes target preparation; successful returns alone do not certify rendering)",
                native_draw_failures_, native_draw_attempts_, native_copy_failures_, native_copy_attempts_);
    REXLOG_INFO("[native] {:.1f} s: {} Swaps, {} draws ({} without shaders), {} copies, {} packets, "
                "{} interrupts, {} WAIT_REG_MEM timeouts; shaders {} identified ({} by address) / {} not; native: "
                "{} drawn, {} rejected, {} pipelines, {} textures, {} presented",
                secs, swaps_ - reported_swaps_, draws_ - reported_draws_, draws_unidentified_,
                copies_ - reported_copies_, packets_ - reported_packets_,
                interrupts_.load() - reported_interrupts_, waits_timed_out_, shaders_identified_,
                shaders_by_address_, shaders_unidentified_, st.drawn, st.rejected, st.pipelines, st.textures,
                presented);
    std::string causes;
    for (size_t i = 0; i < st.causes.size() && i < 6; ++i) {
      causes += fmt::format(" {}x{}", st.causes[i].first, st.causes[i].second);
    }
    if (!causes.empty()) REXLOG_INFO("[native] rejection causes (code x count):{}", causes);
    if (!missing_draw_pairs_.empty()) {
      std::vector<std::pair<std::pair<uint64_t, uint64_t>, uint64_t>> ranked(
          missing_draw_pairs_.begin(), missing_draw_pairs_.end());
      std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
      });
      std::string missing;
      for (size_t i = 0; i < ranked.size() && i < 12; ++i) {
        missing += fmt::format(" {}x(VS={:016X},PS={:016X})", ranked[i].second,
                               ranked[i].first.first, ranked[i].first.second);
      }
      REXLOG_INFO("[native] unresolved draw shader pairs (cumulative, 0 means identified):{}", missing);
    }
    {
      std::lock_guard<std::mutex> lock(g_by_address_mutex);
      REXLOG_INFO("[native] tile replays reusing a pairing {}, draws dropped by predicate {}",
                  draws_replayed_, draws_predicated_);
      std::string miss;
      for (auto& [o, n] : missing_objects_) miss += fmt::format(" {:08X}x{}", o, n);
      REXLOG_INFO("[native] draws paired by command-buffer address {} (no VS entry {}, no PS entry {});"
                  " objects without an entry:{}", draws_paired_by_address_, paired_no_vs_, paired_no_ps_, miss);
      REXLOG_INFO("[native] draws paired with a Draw* record {}, unpaired {}; "
                  "draws inside indirect buffers {} of {}; Draw* records {}",
                  draws_paired_, draws_unpaired_, draws_indirect_, draws_,
                  g_draw_records.load());
      REXLOG_INFO("[native] identified by code {}; unidentified loads: VS by address {}, PS by address "
                  "{}, immediate {}; {} objects seen, {} addresses and {} codes mapped, {} code conflicts",
                  shaders_by_code_, miss_vs_addr_, miss_ps_addr_, miss_immediate_, g_objects_seen.size(),
                  g_by_address.size(), g_by_code.size(), g_code_conflicts);
    }
    // futari-nx: the verify configuration (futari_cpu_verify: every self-check compares on every use). How many
    // comparisons each ring fast path has made against its reference path, and how many disagreed anywhere in the
    // renderer (every DIFFERENCE / MISMATCH line counts): 0 = the same result as the original paths.
    if (REXCVAR_GET(futari_cpu_verify)) {
      // The shader identity audits (every ALU/CF word of the selected library entry compared with the loaded
      // microcode) are not fast paths, but they cover the relaxed VS matching: shown alongside (peggle2-nx's idea).
      REXLOG_INFO("[verify] checks done: identity memo {}, record pairing {}, raw microcode {}, PM4 runs {}, object "
                  "table {}; shader identity audit mismatches VS {} PS {}; {} | differences: {}{}",
                  verify_initial_ - check_identity_, verify_initial_ - check_pairing_, verify_initial_ - check_raw_,
                  verify_initial_ - check_pm4_, verify_initial_ - check_objects_, vs_identity_mismatches_,
                  ps_identity_mismatches_, ::futari::native::VerifySummary(),
                  ::me::native::g_verify_differences.load(std::memory_order_relaxed),
                  ::me::native::g_verify_differences.load(std::memory_order_relaxed) == 0
                      ? " (every fast path gave the same result as the original)"
                      : " (see the DIFFERENCE / MISMATCH lines above; that path turned itself off)");
    }
    reported_swaps_ = swaps_;
    reported_draws_ = draws_;
    reported_copies_ = copies_;
    reported_packets_ = packets_;
    reported_interrupts_ = interrupts_.load();
  }

  futari::renderer::ShadersNative shaders_;
  std::unique_ptr<futari::renderer::TargetsNative> targets_;
  bool targets_failed_ = false;
  const futari::renderer::ShaderEntry* vs_ = nullptr;
  const futari::renderer::ShaderEntry* ps_ = nullptr;
  std::vector<uint32_t> microcode_, vs_microcode_, ps_microcode_;
  uint64_t gen_vs_ = 0, gen_microcode_ = 0, gen_constants_vs_ = 0, gen_constants_ps_ = 0,
           gen_fetch_ = 0, gen_viewport_ = 0;
  uint32_t swap_width_ = 1280, swap_height_ = 720;
  uint32_t logged_misses_ = 0;
  uint32_t im_load_address_ = 0;
  uint64_t shaders_by_code_ = 0, draws_paired_ = 0, draws_unpaired_ = 0;
  uint64_t native_draw_attempts_ = 0, native_draw_failures_ = 0;
  std::unique_ptr<NativeDrawExtentEstimator> draw_extent_estimator_;
  std::unordered_set<uint64_t> extent_diagnostics_;  // futari-nx: hashed keys (was strings formatted per draw)
  uint32_t stencil_clear_diagnostics_ = 0;
  uint64_t native_copy_attempts_ = 0, native_copy_failures_ = 0;
  uint32_t logged_unpaired_ = 0;
  uint64_t draws_paired_by_address_ = 0, paired_no_vs_ = 0, paired_no_ps_ = 0;
  std::unordered_map<uint32_t, uint64_t> missing_objects_;
  std::unordered_set<uint64_t> dumped_rewritten_;
  std::array<uint64_t, futari::renderer::kGpuCategories> report_gpu_ns_{}, report_gpu_frag_{};
  uint64_t report_gpu_swaps_ = 0;
  std::unordered_set<uint64_t> dumped_vertex_variants_;
  uint64_t draws_indirect_ = 0, draws_replayed_ = 0, draws_predicated_ = 0;
  uint32_t packet_address_ = 0;
  struct PairedPacket {
    uint64_t frame;
    const futari::renderer::ShaderEntry* vs;
    const futari::renderer::ShaderEntry* ps;
  };
  std::unordered_map<uint32_t, PairedPacket> paired_packets_;
  std::deque<DrawRecord> pending_;
  const futari::renderer::ShaderEntry* draw_vs_ = nullptr;
  const futari::renderer::ShaderEntry* draw_ps_ = nullptr;
  uint64_t shaders_by_address_ = 0, miss_vs_addr_ = 0, miss_ps_addr_ = 0, miss_immediate_ = 0;
  uint64_t draws_unidentified_ = 0, shaders_identified_ = 0, shaders_unidentified_ = 0, presented_ = 0;
  uint64_t current_vs_hash_ = 0, current_ps_hash_ = 0;
  uint32_t last_vs_load_address_ = UINT32_MAX, last_ps_load_address_ = UINT32_MAX;
  uint64_t shader_loads_reused_ = 0;
  uint64_t ps_identity_mismatches_ = 0;
  uint64_t vs_identity_mismatches_ = 0;
  uint64_t vs_final_proven_ = 0, vs_final_unproven_ = 0, vs_final_missing_ = 0;
  uint64_t vs_identity_generation_ = 1;
  // futari_gpu_vs_fetch_reorder: the entry the current VS load was identified as with its declared fetches reordered by
  // Direct3D (IdentifyShader; nullptr for every other load), the one candidate ComputeVertexIdentity accepts beyond the
  // ordered predicate, after proving it again on the loaded words; and the identity memo keys of such loads, so that a
  // memo hit restores it.
  const futari::renderer::ShaderEntry* vs_reorder_entry_ = nullptr;
  std::unordered_set<uint64_t> vs_reorder_memo_keys_;
  uint64_t shaders_reordered_ = 0;
  // futari_diag_vs_reorder_swap: the first pair of executed vertex fetches whose swap the proof accepts (the loaded
  // program, its fetch triples masked as the library keeps them, standing for the library entry) is swapped.
  const bool diag_reorder_swap_ = REXCVAR_GET(futari_diag_vs_reorder_swap);
  uint64_t diag_reorder_swaps_ = 0;
  bool DiagSwapFetches(std::vector<uint32_t>& mc) {
    me::native::reorder_detail::Locations locations;
    if (me::native::reorder_detail::FetchLocations(mc, locations) || locations.count < 2) return false;
    struct Slot {
      uint16_t instruction;
    };
    std::array<Slot, 32> slots{};
    for (size_t i = 0; i < locations.count; ++i) slots[i].instruction = uint16_t(locations.items[i].instruction);
    const std::span<const Slot> declared(slots.data(), locations.count);
    std::vector<uint32_t> selected = mc;
    constexpr uint32_t keeps[3]{0x0007FFFFu, 0x80000FFFu, 0x80000000u};
    for (const Slot& slot : declared)
      for (size_t lane = 0; lane < 3; ++lane) selected[size_t(slot.instruction) * 3 + lane] &= keeps[lane];
    for (size_t a = 0; a < locations.count; ++a) {
      for (size_t b = a + 1; b < locations.count; ++b) {
        std::vector<uint32_t> swapped = mc;
        const size_t p = size_t(locations.items[a].instruction) * 3, q = size_t(locations.items[b].instruction) * 3;
        for (size_t lane = 0; lane < 3; ++lane) std::swap(swapped[p + lane], swapped[q + lane]);
        if (me::native::ProveVertexFetchReorder(std::span<const uint32_t>(swapped), std::span<const uint32_t>(selected),
                                                declared, [](const Slot& s) { return s.instruction; })) {
          mc = std::move(swapped);
          ++diag_reorder_swaps_;
          return true;
        }
      }
    }
    return false;
  }
  uint64_t ps_identity_generation_ = 1;
  std::unordered_map<const futari::renderer::ShaderEntry*, std::pair<uint64_t, bool>> vs_identity_cache_;
  std::array<IdentityCell, kIdentityCells> vs_cells_{}, ps_cells_{}, fc_cells_{};
  // Ring CPU switches (futari_native_flat_identity, _fast_pair, _raw_microcode, _pm4_fast) and the
  // remaining self-check uses of each.
  bool identity_flat_ = false, pairing_fast_ = false, raw_microcode_ = false, pm4_fast_ = false;
  uint32_t check_identity_ = 0, check_pairing_ = 0, check_raw_ = 0, check_pm4_ = 0, check_objects_ = 0;
  uint64_t ring_busy_ns_ = 0, ring_wait_ns_ = 0;  // futari-nx (sa2-nx's ring timer)
  uint64_t report_ring_busy_ns_ = 0, report_ring_wait_ns_ = 0, report_ring_swaps_ = 0, report_ring_draws_ = 0;
  uint32_t verify_initial_ = 0;  // futari-nx: futari_native_verify_n at start, for the "[verify]" line
  std::vector<uint32_t> raw_vs_, raw_ps_;  // the guest words of the last IM_LOAD of each stage (raw_microcode_)
  bool raw_vs_valid_ = false, raw_ps_valid_ = false;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> missing_draw_pairs_;
  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  bool mmio_registered_ = false;
  std::atomic<bool> active_{false};
  std::vector<uint32_t> registers_;
  std::array<std::array<uint16_t, 3>, 256> gamma_ramp_ = [] {
    std::array<std::array<uint16_t, 3>, 256> ramp{};
    for (uint32_t i = 0; i < 256; ++i) ramp[i].fill(uint16_t(i * 0x3FF / 0xFF));
    return ramp;
  }();
  uint32_t gamma_component_ = 0;
  uint32_t gamma_mask_ = 0b111;
  uint64_t gamma_version_ = 0, gamma_sent_version_ = 0, gamma_writes_ = 0;
  bool gamma_logged_ = false;
  std::atomic<uint32_t> callback_{0}, callback_data_{0};
  std::atomic<uint32_t> ring_base_{0}, ring_words_{0}, ring_generation_{0};
  std::atomic<uint32_t> write_pointer_{0}, read_writeback_{0};
  std::atomic<uint32_t> counter_{0};
  std::mutex ring_mutex_;
  std::atomic<bool> ring_waiting_{false};
  std::condition_variable ring_cv_;
  rex::system::object_ref<rex::system::XHostThread> ring_thread_, vblank_thread_;
  uint64_t bin_mask_ = ~0ull, bin_select_ = ~0ull;
  // Ring-thread counters.
  uint64_t packets_ = 0, draws_ = 0, copies_ = 0, swaps_ = 0, waits_timed_out_ = 0;
  uint64_t opcodes_[128] = {};
  uint64_t opcode_words_[128] = {}, type0_packets_ = 0, type0_words_ = 0, type0_single_ = 0;  // PM4 mix report
  std::atomic<uint64_t> interrupts_{0};
  uint64_t reported_swaps_ = 0, reported_draws_ = 0, reported_copies_ = 0, reported_packets_ = 0,
           reported_interrupts_ = 0;
  Clock::time_point start_, last_report_;
  // banjo_trace_frame_s: 0 waiting, 1 tracing the current frame, 2 done.
  int trace_phase_ = 0;
  bool renderdoc_triggered_ = false;
  bool renderdoc_capturing_ = false;
  int trace_result_ = -1;  // last draw: -1 not attempted, 0 rejected, 1 drawn
  uint64_t trace_draws_ = 0, trace_quiet_draws_ = 0;
  uint32_t trace_target_[4] = {~0u, ~0u, ~0u, ~0u};
  std::unordered_set<uint32_t> resolve_destinations_;  // 4 KB pages of every resolve destination seen
};

}  // namespace

bool HasEntry(uint32_t object) {
  std::lock_guard<std::mutex> lock(g_by_address_mutex);
  return g_entry_of_object.count(object) != 0;
}

// banjo-nx: host pointer of a guest virtual address in a physical-memory view (the shader microcode, 0xA0000000 and
// up). On the Switch and on macOS the views map 1:1 onto `base`; on Windows the SDK shifts the 0xE0000000 view by
// 0x1000 (its 64 KB allocation granularity), so `base + address` reads the wrong bytes there. TranslateVirtual
// applies each heap's offset.
static const uint8_t* GuestPhysicalView(const uint8_t* base, uint32_t address) {
#if defined(_WIN32)
  return rex::system::kernel_state()->memory()->TranslateVirtual(address);
#else
  return base + address;
#endif
}

void NoteShaderObject(const uint8_t* base, uint32_t object, bool vertex) {
  const futari::renderer::ShadersNative* library = g_library.load(std::memory_order_acquire);
  auto load32 = [base](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(base + a); };
  bool trace_this = false;  // sa2-nx diagnostic: log the path taken for the first objects
  // futari-nx (futari_shader_note_fast, sa2-nx's finding): once the 32 objects are logged the diagnostic's mutex and set
  // are skipped (they were taken on EVERY call, twice per draw on the game thread).
  static const bool note_fast = REXCVAR_GET(futari_shader_note_fast);
  static std::atomic<bool> traced_full{false};
  if (!note_fast || !traced_full.load(std::memory_order_relaxed)) {  // sa2-nx diagnostic: the first distinct objects
    static std::mutex trace_mutex;
    static std::unordered_set<uint64_t> traced;
    std::lock_guard<std::mutex> lock(trace_mutex);
    if (traced.size() >= 32) traced_full.store(true, std::memory_order_relaxed);
    if (traced.size() < 32 && traced.insert(uint64_t(object) << 1 | (vertex ? 1 : 0)).second) {
      trace_this = true;
      REXLOG_INFO("[native] NoteShaderObject({}, {:08X}): library {}, word0 {:08X}", vertex ? "VS" : "PS", object,
                  library ? "loaded" : "none",
                  object >= 0x40000000 && object < 0x7F000000 ? load32(object) : 0u);
    }
  }
  if (!library || object < 0x40000000 || object >= 0x7F000000) { if (trace_this) REXLOG_INFO("[native]   {:08X} exit L15", object); return; }
  if ((load32(object) & 0xFF) != (vertex ? 6u : 7u)) {
    // sa2-nx diagnostic: what is bound where a shader object was expected (once per object, first 16).
    static std::mutex logged_mutex;
    static std::unordered_set<uint32_t> logged;
    std::lock_guard<std::mutex> lock(logged_mutex);
    if (logged.size() < 16 && logged.insert(object).second) {
      std::string words;
      for (uint32_t i = 0; i < 0x30; i += 4) words += fmt::format(" {:08X}", load32(object + i));
      REXLOG_WARN("[native] {} slot holds {:08X}, not a shader object (type byte {:02X}):{}", vertex ? "VS" : "PS",
                  object, load32(object) & 0xFF, words);
    }
    if (trace_this) REXLOG_INFO("[native]   {:08X} exit L27", object);
    return;
  }
  // Objects whose work is finished (seen and code-mapped: the locked path below only returns for them) are
  // remembered per game thread, so the per-draw call skips the shared mutex (contended with the ring thread).
  // futari-nx (futari_shader_note_fast): shared by all threads with relaxed atomics instead of thread_local (every
  // thread_local access is a call on the Switch, -mtp=soft). Exact: "finished" never ends (nothing erases
  // g_objects_seen / g_code_mapped / g_entry_of_object), so a stale cell only sends a call down the locked path.
  thread_local std::array<uint32_t, 512> finished_cache_local{};
  static std::array<std::atomic<uint32_t>, 512> finished_cache_shared{};
  struct FinishedCell {
    uint32_t* local;
    std::atomic<uint32_t>* shared;
    bool operator==(uint32_t v) const {
      return shared ? shared->load(std::memory_order_relaxed) == v : *local == v;
    }
    FinishedCell& operator=(uint32_t v) {
      if (shared) shared->store(v, std::memory_order_relaxed); else *local = v;
      return *this;
    }
  } finished{note_fast ? nullptr : &finished_cache_local[(object >> 4) & 511],
             note_fast ? &finished_cache_shared[(object >> 4) & 511] : nullptr};
  if (finished == object) { if (trace_this) REXLOG_INFO("[native]   {:08X} exit L33", object); return; }
  {
    std::lock_guard<std::mutex> lock(g_by_address_mutex);
    if (!g_objects_seen.insert(object).second) {
      // Already identified: map its current microcode once (for VS, after D3D patched it).
      auto it = g_entry_of_object.find(object);
      if (it != g_entry_of_object.end() && g_code_mapped.count(object)) finished = object;
      // Seen but never identified: only the first sighting identifies an object (below), so this stays
      // unidentified; remember it too instead of taking the mutex on every draw (~0.5 % of the render thread).
      if (it == g_entry_of_object.end()) finished = object;
      if (it == g_entry_of_object.end() || g_code_mapped.count(object)) { if (trace_this) REXLOG_INFO("[native]   {:08X} exit L43", object); return; }
      const uint32_t mc = load32(object + (vertex ? 0x20 : 0x18));
      const uint32_t header = object + (vertex ? 0x368 : 0x28);
      const uint32_t sh = header + load32(header + 24);
      const uint32_t mc_offset = load32(sh), mc_bytes = load32(sh + 4);
      if (mc < 0xA0000000u || !mc_bytes || mc_bytes > 0x40000) { if (trace_this) REXLOG_INFO("[native]   {:08X} exit L48", object); return; }
      const uint64_t h = CodeHash(GuestPhysicalView(base, mc) + mc_offset, mc_bytes / 4);
      auto [slot, inserted] = g_by_code.emplace(h, it->second);
      if (!inserted && slot->second != it->second) ++g_code_conflicts;
      g_code_mapped.insert(object);
      finished = object;
      if (trace_this) REXLOG_INFO("[native]   {:08X} exit L54", object);
      return;
    }
  }
  // Same layout as src/me_shader_dump.cpp: container copy in the object, microcode pointer.
  const uint32_t header = object + (vertex ? 0x368 : 0x28);
  const uint32_t virtual_size = load32(header + 4);
  const uint32_t physical_size = load32(header + 8);
  const uint32_t microcode = load32(object + (vertex ? 0x20 : 0x18));
  if ((load32(header) & 0xFFFFFF00u) != 0x102A1100u || virtual_size < 28 || virtual_size > 0x40000 ||
      !physical_size || physical_size > 0x40000 || microcode < 0xA0000000u) {
    // sa2-nx: the object does not have the container layout this expects (a newer XDK, or one of Direct3D's own
    // shader objects). Say what is there, once per object, so the layout can be checked.
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
      std::string words;
      for (uint32_t i = 0; i < 0x40; i += 4) words += fmt::format(" {:08X}", load32(object + i));
      REXLOG_WARN("[native] {} object {:08X} not identified: container word {:08X} sizes {}/{}, microcode {:08X}; "
                  "object:{}", vertex ? "VS" : "PS", object, load32(header), virtual_size, physical_size, microcode,
                  words);
    }
    if (trace_this) REXLOG_INFO("[native]   {:08X} exit L74", object);
    return;
  }
  std::vector<uint8_t> container(virtual_size + physical_size);
  std::memcpy(container.data(), base + header, virtual_size);
  std::memcpy(container.data() + virtual_size, GuestPhysicalView(base, microcode), physical_size);
  const futari::renderer::ShaderEntry* entry = library->IdentifyContainer(container);
  if (!entry && vertex) {
    std::lock_guard<std::mutex> lock(g_by_address_mutex);
    if (!g_vs_by_virtual_built) BuildVirtualIndex(library);
    auto it = g_vs_by_virtual.find(XXH3_64bits(container.data(), virtual_size));
    if (it != g_vs_by_virtual.end()) entry = it->second;
  }
  // Direct3D's internal containers may not be in the package byte-for-byte, because discovery
  // replaces their incomplete virtual metadata with a conservative register-linked wrapper.
  // Their physical microcode is still authoritative. The library code index deliberately prefers
  // the complete wrapper when the same code occurs in multiple containers.
  if (!entry) {
    const uint32_t shader_header = virtual_size >= 28 ? load32(header + 24) : 0;
    const uint32_t physical_offset =
        shader_header + 8 <= virtual_size ? load32(header + shader_header) : 0;
    const uint32_t shader_bytes =
        shader_header + 8 <= virtual_size ? load32(header + shader_header + 4) : 0;
    if (shader_bytes && !(shader_bytes & 3) &&
        uint64_t(physical_offset) + shader_bytes <= physical_size) {
      const uint64_t code = CodeHash(container.data() + virtual_size + physical_offset,
                                     shader_bytes / 4) ^ (vertex ? 1ull : 0ull);
      if (auto it = g_library_by_code.find(code); it != g_library_by_code.end()) {
        entry = it->second;
        REXLOG_INFO("[native] shader object {:08X} matched register-linked {} n{} by microcode",
                    object, vertex ? "VS" : "PS", entry->number);
      }
    }
  }
  if (!entry) {
    const uint32_t shader_header = virtual_size >= 28 ? load32(header + 24) : 0;
    const uint32_t physical_offset = shader_header + 8 <= virtual_size ? load32(header + shader_header) : 0;
    const uint32_t shader_bytes = shader_header + 8 <= virtual_size ? load32(header + shader_header + 4) : 0;
    REXLOG_INFO("[native] shader object {:08X} not in the library: {}_{:016x}; mc {:08X}, "
                "virtual {} physical {}, shader header +{:X}, code +{:X}/{} -> ring {:08X}",
                object, vertex ? "vs" : "ps", XXH3_64bits(container.data(), container.size()),
                microcode, virtual_size, physical_size, shader_header, physical_offset, shader_bytes,
                (microcode & 0x1FFFFFFFu) + physical_offset);
    // Not in the library (containers Direct3D or the engine build at startup): keep a copy with
    // FUTARI_SHADER_MISSING=<folder> so the library can be extended (shaders/tools/build_shader_spirv.sh).
    if (const char* folder = std::getenv("FUTARI_SHADER_MISSING"); folder && *folder) {
      std::error_code ec;
      std::filesystem::create_directories(folder, ec);
      const uint64_t h = XXH3_64bits(container.data(), container.size());
      const auto path = std::filesystem::path(folder) /
                        fmt::format("{}_{:016x}.bin", vertex ? "vs" : "ps", h);
      if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(container.data(), 1, container.size(), f);
        std::fclose(f);
      }
    }
    if (trace_this) REXLOG_INFO("[native]   {:08X} exit L129", object);
    return;
  }
  const uint32_t shader_header = rex::memory::load_and_swap<uint32_t>(container.data() + 24);
  const uint32_t physical_offset =
      shader_header + 4 <= virtual_size
          ? rex::memory::load_and_swap<uint32_t>(container.data() + shader_header)
          : 0;
  const uint32_t mc_bytes = shader_header + 8 <= virtual_size
                                ? rex::memory::load_and_swap<uint32_t>(container.data() + shader_header + 4)
                                : 0;
  std::lock_guard<std::mutex> lock(g_by_address_mutex);
  g_by_address[(microcode & 0x1FFFFFFFu) + physical_offset] = entry;
  g_entry_of_object[object] = entry;
  PublishObject(object, entry);
  if (trace_this) REXLOG_INFO("[native]   {:08X} identified as n{}", object, entry->number);
  if (!vertex) {  // pixel shaders are never patched: map their microcode now
    if (mc_bytes && virtual_size + physical_offset + mc_bytes <= container.size()) {
      const uint64_t h = CodeHash(container.data() + virtual_size + physical_offset, mc_bytes / 4);
      auto [slot, inserted] = g_by_code.emplace(h, entry);
      if (!inserted && slot->second != entry) ++g_code_conflicts;
      g_code_mapped.insert(object);
    }
  }
}

namespace {
// The bound shader objects in the game's D3D device (SetDeviceShaderOffsets); Mass Effect's by default.
std::atomic<uint32_t> g_device_vs_offset{12416};
std::atomic<uint32_t> g_device_ps_offset{12412};
}  // namespace

void SetDeviceShaderOffsets(uint32_t vertex_shader_offset, uint32_t pixel_shader_offset) {
  g_device_vs_offset.store(vertex_shader_offset, std::memory_order_relaxed);
  g_device_ps_offset.store(pixel_shader_offset, std::memory_order_relaxed);
}

void NoteDrawCall(const uint8_t* base, uint32_t device, uint32_t type, uint32_t r5, uint32_t r6, uint32_t r7) {
  if (!g_native_active.load(std::memory_order_acquire)) return;
  // The device the call was made on (r3). This used to be Mass Effect's device address (0x4004B000) and offsets,
  // which in another game reads unrelated memory: every record then carried a wrong command-buffer position and
  // no ring draw could be paired by address.
  if (device < 0x40000000 || device >= 0x7F000000) return;
  const uint32_t vs =
      rex::memory::load_and_swap<uint32_t>(base + device + g_device_vs_offset.load(std::memory_order_relaxed));
  const uint32_t ps =
      rex::memory::load_and_swap<uint32_t>(base + device + g_device_ps_offset.load(std::memory_order_relaxed));
  NoteShaderObject(base, vs, true);
  NoteShaderObject(base, ps, false);
  g_draw_records.fetch_add(1, std::memory_order_relaxed);
  const uint32_t start = rex::memory::load_and_swap<uint32_t>(base + device + 48) & 0x1FFFFFFFu;
  DrawRecord record{type, r5, r6, r7, vs, ps, start};
  if (RecordTable()) {
    PushRecord(std::move(record), start);  // lock-free; the ring inserts it into its table
    return;
  }
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  g_draw_by_address[start] = std::move(record);
  if (g_draw_by_address.size() > 65536) g_draw_by_address.erase(g_draw_by_address.begin());
}

bool NativeActive() { return g_native_active.load(std::memory_order_acquire); }

// futari_ab_s / futari_ab_key: true while the named enhancement is in the "off" half of an in-run A/B.
bool AbOff(const char* key) {
  static const int seconds = REXCVAR_GET(futari_ab_s);
  if (seconds <= 0) return false;
  static const std::string target = REXCVAR_GET(futari_ab_key);
  // "futari_ring_all": every ring enhancement of 2026-10-07 that supports the A/B, together.
  static const bool all = target == "futari_ring_all";
  const bool mine = all ? (std::string_view(key) == "futari_cpu_direct_state" ||
                           std::string_view(key) == "futari_ring_edram_memo" ||
                           std::string_view(key) == "futari_ring_rectangle_fast" ||
                           std::string_view(key) == "futari_ring_lean" ||
                           std::string_view(key) == "futari_ring_target_memo")
                        : target == key;
  return mine && direct::AbPhase(seconds);
}

// futari_cpu_direct_state, game thread: what the draw hook needs from before the original draw function runs.
DrawDirect BeginDrawDirect(const uint8_t* base, uint32_t device, uint32_t end_offset) {
  static const bool on = REXCVAR_GET(futari_cpu_direct_state);
  DrawDirect d;
  if (!on || direct::g_failed.load(std::memory_order_relaxed) || !g_native_active.load(std::memory_order_acquire) ||
      device < 0x40000000 || device >= 0x7F000000)
    return d;
  if (AbOff("futari_cpu_direct_state")) [[unlikely]] return d;  // the "off" half of an A/B measurement
  // One drawing thread converts (the record ring has one producer; sasrt-nx's game draws from two threads into
  // separate command buffers). The first thread that draws owns it; draws from any other thread keep their PM4.
  static std::atomic<uint64_t> owner{0};
  thread_local int8_t mine = -1;
  if (mine < 0) [[unlikely]] {
    const uint64_t id = uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id())) | 1;
    uint64_t expected = 0;
    mine = owner.compare_exchange_strong(expected, id) || expected == id;
    if (!mine) REXLOG_WARN("[native] direct state: a second thread draws; its draws stay PM4");
  }
  if (!mine) {
    direct::g_skip_thread.fetch_add(1, std::memory_order_relaxed);
    return d;
  }
  direct::Bump(direct::g_calls, 1);
  d.device = device;
  d.end_offset = end_offset;
  d.start = rex::memory::load_and_swap<uint32_t>(base + device + 48);  // the last word written so far
  d.limit = rex::memory::load_and_swap<uint32_t>(base + device + 52);
  d.soft_limit = rex::memory::load_and_swap<uint32_t>(base + device + 56);
  d.submissions = direct::g_wptr_writes.load(std::memory_order_relaxed);
  return d;
}

namespace {
// n big-endian words to host order (vectorised by the compiler: one rev32 per four words).
inline void SwapWords(uint32_t* dst, const uint8_t* src, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t w;
    std::memcpy(&w, src + size_t(i) * 4, 4);
    dst[i] = __builtin_bswap32(w);
  }
}

}  // namespace

// futari_cpu_direct_state, game thread, right after the original draw function: its state packets become a host record
// and one NOP packet (namespace direct above).
void EndDrawDirect(uint8_t* base, const DrawDirect& d) {
  using namespace direct;
  if (!d.device) return;
  if (g_wptr_writes.load(std::memory_order_relaxed) != d.submissions) {  // the ring may be reading that segment
    Bump(g_skip_submitted, 1);
    return;
  }
  // The last word written: device+48 for a draw function that ends its packets there, the draw packet's last word
  // (device+13652) for BeginVertices / BeginIndexedVertices (EndVertices copies it to +48 later).
  const uint32_t end = rex::memory::load_and_swap<uint32_t>(base + d.device + d.end_offset);
  if (rex::memory::load_and_swap<uint32_t>(base + d.device + 52) != d.limit ||
      rex::memory::load_and_swap<uint32_t>(base + d.device + 56) != d.soft_limit) {  // another segment
    Bump(g_skip_segment, 1);
    return;
  }
  if (end <= d.start || end - d.start > 0x10000u) {
    Bump(g_skip_no_draw, 1);
    return;
  }
  const uint32_t first = d.start + 4;  // the first word the draw function wrote
  // The record is decoded in place, in the ring of records: contiguous, never waiting for the ring thread (its records
  // may be in segments not submitted yet); when there is no room, this draw keeps its packets. A packet's record is at
  // most twice its words (a one-word type-3 packet becomes four), plus the record's two header words.
  const uint32_t written = (end - first) / 4 + 1;
  const uint32_t room = kHeader + 2 * written;
  if (!g_ring) [[unlikely]] g_ring = std::make_unique<uint32_t[]>(kRingWords);
  constexpr uint64_t mask = kRingWords - 1;
  const uint64_t pos = g_head.load(std::memory_order_relaxed);
  const uint64_t tail = g_tail.load(std::memory_order_acquire);
  uint64_t start_record = pos;
  if ((pos & mask) + room > kRingWords) start_record = (pos | mask) + 1;  // wrap
  if (start_record + room - tail > kRingWords || room > kRingWords / 4) {
    Bump(g_skip_full, 1);
    return;
  }
  uint32_t* const rec = &g_ring[start_record & mask];
  uint32_t* out = rec + kHeader;
  uint32_t draw_at = 0;
  for (uint32_t p = first; p <= end;) {
    const uint8_t* const hp = Guest(base, p);
    const uint32_t h = rex::memory::load_and_swap<uint32_t>(hp);
    const uint32_t type = h >> 30;
    if (type == 0) {
      if (!h) { p += 4; continue; }  // Packet() skips a zero word
      const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
      if (p + 4 * count > end) break;
      *out++ = ((h >> 15) & 1 ? kRunSingle : kRun) | (count << 8);
      *out++ = h & 0x7FFF;
      SwapWords(out, hp + 4, count);
      out += count;
      p += 4 * (1 + count);
    } else if (type == 1) {
      if (p + 8 > end) break;
      *out++ = kType1;
      *out++ = h;
      SwapWords(out, hp + 4, 2);
      out += 2;
      p += 12;
    } else if (type == 2) {
      p += 4;  // filler
    } else {
      const uint32_t opcode = (h >> 8) & 0x7F, count = ((h >> 16) & 0x3FFF) + 1;
      if (IsDraw(opcode)) {
        draw_at = p;
        break;
      }
      if ((h & 1) || !Carried(opcode)) {
        g_unknown_opcode.store(opcode, std::memory_order_relaxed);
        Bump(g_skip_unknown, 1);
        return;
      }
      if (p + 4 * count > end) break;
      if (opcode == xenos::PM4_NOP) {
        // The marker covers the region's first three words: a NOP there may hold data another packet points at.
        if (p - first < 12) {
          Bump(g_skip_nop_head, 1);
          return;
        }
        p += 4 * (1 + count);
        continue;
      }
      *out++ = kType3 | (count << 8);
      *out++ = h;
      *out++ = p & 0x1FFFFFFFu;  // the packet's physical address (Type3 reads packet_address_)
      std::memcpy(out, hp + 4, size_t(count) * 4);  // guest byte order: Type3 reads them
      out += count;
      p += 4 * (1 + count);
    }
  }
  if (!draw_at) {
    Bump(g_skip_no_draw, 1);
    return;
  }
  const uint32_t region = (draw_at - first) / 4;
  if (region < 3 || region - 1 > 0x4000) {
    Bump(g_skip_small, 1);
    return;
  }
  const uint32_t need = uint32_t(out - rec);
  // The decoder's own check (first records, every record in verify mode).
  static const bool verify = REXCVAR_GET(futari_cpu_verify);
  if (const int32_t left = g_checks_left.load(std::memory_order_relaxed); left > 0 || verify) {
    if (left > 0) g_checks_left.store(left - 1, std::memory_order_relaxed);
    Bump(g_checked, 1);
    const auto word = [base](uint32_t p) { return rex::memory::load_and_swap<uint32_t>(Guest(base, p)); };
    if (!SameAsWritten(word, first, draw_at, rec + kHeader, need - kHeader)) {
      Bump(g_check_differences, 1);
      g_failed.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("[native] DIFFERENCE: a direct-state record does not re-encode to the packets Direct3D wrote "
                   "(region {:08X}, {} words); futari_cpu_direct_state off from now on (records already made are "
                   "still applied)", first, region);
      return;
    }
  }
  // From here to the marker's last word no submission may be published (g_marking, WriteMmio): checked once more now.
  g_marking.store(true, std::memory_order_seq_cst);
  if (g_wptr_writes.load(std::memory_order_seq_cst) != d.submissions) {
    g_marking.store(false, std::memory_order_release);
    Bump(g_skip_submitted, 1);
    return;  // the words decoded into the ring are not published: the next record overwrites them
  }
  if (start_record != pos) g_ring[pos & mask] = kPad;
  uint32_t seq = uint32_t(++g_seq);
  if (seq == kPad) seq = uint32_t(++g_seq);
  g_api_reserved = ~0ull;  // futari_api_*: a reservation does not survive another record
  rec[0] = seq;
  rec[1] = need;
  for (uint32_t i = 0; i < 3; ++i) rec[2 + i] = rex::memory::load_and_swap<uint32_t>(Guest(base, first + 4 * i));
  g_head.store(start_record + need, std::memory_order_release);
  // The marker: one NOP covering the region; the old packets stay inside it as payload, the draw packet after it.
  uint8_t* marker = Guest(base, first);
  rex::memory::store_and_swap<uint32_t>(marker, 0xC0000000u | ((region - 2) << 16) | (xenos::PM4_NOP << 8));
  rex::memory::store_and_swap<uint32_t>(marker + 4, kMagic);
  rex::memory::store_and_swap<uint32_t>(marker + 8, seq);
  g_marking.store(false, std::memory_order_release);
  Bump(g_converted, 1);
  Bump(g_words_covered, region);
}

// ---------------------------------------------------------------------------------------------------------------------
// futari_api_*: the native Direct3D layer's records (src/me/futari_api_internal.h), sa2-nx's ReserveApiRecord /
// CommitApiRecord / PublishApiRecord with Futari's record header ([seq][size][three unused words]). The drawing thread is
// their only producer (the direct-state conversion above runs on the same thread). The consumer's g_tail is read again
// only when the cached older value says there is no room: an older value only understates the room, and reading the
// line the ring thread writes cost a cache miss per record (sa2-nx round 10).
namespace {
uint64_t g_api_tail_seen = 0;
uint64_t g_api_ready = 0;  // ring words (monotonic) readied (zeroed) ahead of the head

// futari_api_ring_zero_ahead (sa2-nx round 11): the ring of records (8 MB) is far larger than the A57's 2 MB L2, so every
// record line was a write miss (the line read from DRAM before the stores complete) and the publish's store-release
// waited for them. DC ZVA allocates a block of zeros in the cache without reading memory (allowed in user mode when
// DCZID_EL0.DZP is clear, as newlib's memset checks); free ring space is readied this way a little ahead of the head.
// Elsewhere a memset of the same blocks runs the same logic, so the PC's tests cover which words are zeroed.
constexpr uint32_t kReadyAheadWords = 1024;
inline uint32_t ZeroBlockWords() {
#if defined(__aarch64__)
  static const uint32_t words = [] {
    uint64_t id;
    asm volatile("mrs %0, dczid_el0" : "=r"(id));
    return (id & 16) ? 0u : (1u << (id & 15));
  }();
  return words;
#else
  return 16;
#endif
}
inline void ZeroBlock(uint32_t* p, uint32_t words) {
#if defined(__aarch64__)
  (void)words;
  asm volatile("dc zva, %0" : : "r"(p) : "memory");
#else
  std::memset(p, 0, size_t(words) * 4);
#endif
}

// The first word of a record of `need` words at the head (the rest of the lap skipped when it does not fit), or ~0 when
// the ring is behind (no room).
uint64_t ApiRecordStart(uint32_t need) {
  using namespace direct;
  constexpr uint64_t mask = kRingWords - 1;
  const uint64_t pos = g_head.load(std::memory_order_relaxed);
  uint64_t start = pos;
  if ((pos & mask) + need > kRingWords) start = (pos | mask) + 1;  // wrap
  if (start + need - g_api_tail_seen > kRingWords) {
    g_api_tail_seen = g_tail.load(std::memory_order_acquire);
    if (start + need - g_api_tail_seen > kRingWords) return ~0ull;
  }
  return start;
}

// Publishes the record of `need` words at `start` (its ops written), the head having been at `pos`: its header, the
// skip mark at `pos` when it wrapped, the head (store-release), and the next records' space readied ahead.
uint32_t ApiRecordPublish(uint64_t pos, uint64_t start, uint32_t need) {
  using namespace direct;
  constexpr uint64_t mask = kRingWords - 1;
  uint32_t* const rec = &g_ring[start & mask];
  if (start != pos) g_ring[pos & mask] = kPad;
  uint32_t seq = uint32_t(++g_seq);
  if (seq == kPad) seq = uint32_t(++g_seq);
  rec[0] = seq;
  rec[1] = need;
  rec[2] = rec[3] = rec[4] = 0;
  const uint64_t head = start + need;
  g_head.store(head, std::memory_order_release);
  Bump(g_api_records, 1);
  Bump(g_api_record_words, need);
  // Whole blocks only, each once, from the first block boundary at or past the new head (the block holding the head may
  // hold this record's last words), and only free space: below the consumer's tail plus the ring's size, judged by the
  // cached older tail (which only understates the room).
  if (g_api_tail_seen + kRingWords < head + kReadyAheadWords) [[unlikely]]
    g_api_tail_seen = g_tail.load(std::memory_order_acquire);
  const uint64_t limit = std::min(head + kReadyAheadWords, g_api_tail_seen + kRingWords);
  static const bool zero_ahead = REXCVAR_GET(futari_api_ring_zero_ahead);
  static const bool verify = REXCVAR_GET(futari_cpu_verify);
  if (const uint32_t block = zero_ahead ? ZeroBlockWords() : 0) [[likely]] {
    uint64_t from = std::max(g_api_ready, (head + block - 1) & ~uint64_t(block - 1));
    if (verify) [[unlikely]] {  // every block about to be zeroed lies past the published head, inside the free space
      Bump(g_api_zero_checks, 1);
      const uint64_t tail = g_tail.load(std::memory_order_acquire);
      if (from < head || (from < limit && limit > tail + kRingWords)) {
        Bump(g_api_zero_differences, 1);
        REXLOG_ERROR("[native] DIFFERENCE: futari_api_ring_zero_ahead would zero words {}..{} (head {}, tail {})", from,
                     limit, head, tail);
      }
    }
    for (; from + block <= limit; from += block) ZeroBlock(&g_ring[from & mask], block);
    g_api_ready = std::max(g_api_ready, from);
  } else {
    uint64_t from = std::max(g_api_ready, head);
    for (; from < limit; from += 16) {
      if ((from & mask) + 16 > kRingWords) continue;  // the lap's last partial line: the next lap starts at word 0
      __builtin_prefetch(&g_ring[from & mask], 1, 3);
    }
    g_api_ready = std::max(g_api_ready, limit);
  }
  return seq;
}
}  // namespace

uint32_t PublishApiRecord(const uint32_t* ops, uint32_t n) {
  using namespace direct;
  constexpr uint64_t mask = kRingWords - 1;
  if (!g_ring) [[unlikely]] g_ring = std::make_unique<uint32_t[]>(kRingWords);
  const uint32_t need = kHeader + n;
  if (need > kRingWords / 4) return 0;
  const uint64_t pos = g_head.load(std::memory_order_relaxed);
  const uint64_t start = ApiRecordStart(need);
  if (start == ~0ull) return 0;  // the ring is behind: no room
  std::memcpy(&g_ring[(start & mask) + kHeader], ops, size_t(n) * 4);
  g_api_reserved = ~0ull;  // a reservation does not survive another record
  return ApiRecordPublish(pos, start, need);
}

uint32_t* ReserveApiRecord(uint32_t max_words) {
  using namespace direct;
  constexpr uint64_t mask = kRingWords - 1;
  if (!g_ring) [[unlikely]] g_ring = std::make_unique<uint32_t[]>(kRingWords);
  const uint32_t need = kHeader + max_words;
  if (need > kRingWords / 4) return nullptr;
  const uint64_t start = ApiRecordStart(need);
  if (start == ~0ull) return nullptr;
  g_api_reserved = start;
  g_api_reserved_head = g_head.load(std::memory_order_relaxed);
  return &g_ring[(start & mask) + kHeader];
}

uint32_t CommitApiRecord(const uint32_t* ops, uint32_t n) {
  using namespace direct;
  constexpr uint64_t mask = kRingWords - 1;
  const uint64_t pos = g_head.load(std::memory_order_relaxed);
  // One producer: nothing else publishes between the reservation and its commit. If something did (the direct-state
  // conversion, say), the reserved words may have been written over: refused, never published half.
  if (g_api_reserved == ~0ull || pos != g_api_reserved_head || ops != &g_ring[(g_api_reserved & mask) + kHeader])
    [[unlikely]] return 0;
  const uint64_t start = g_api_reserved;
  g_api_reserved = ~0ull;
  return ApiRecordPublish(pos, start, kHeader + n);
}

bool Enabled() {
  // Horizon does not launch an NRO with a useful process environment: the toml decides. Native by default;
  // futari_renderer_native = false + gpu_plugin = "xenos" runs ReXGlue's Xenos emulation instead.
  return REXCVAR_GET(futari_renderer_native);
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem() {
  return std::make_unique<NativeGraphicsSystem>();
}

}  // namespace me::native

// futari_cpu_direct_state for the game cores (src/native/futari_native_core.cpp): each core links its own Direct3D, and
// its draw hooks reach the renderer through these (GetProcAddress on the PC, the final link on the Switch).
#if defined(_WIN32)
#define FUTARI_DIRECT_EXPORT extern "C" __declspec(dllexport)
#else
#define FUTARI_DIRECT_EXPORT extern "C" __attribute__((visibility("default")))
#endif
FUTARI_DIRECT_EXPORT void FutariBeginDrawDirect(const uint8_t* base, uint32_t device, uint32_t end_offset,
                                                me::native::DrawDirect* out) {
  *out = me::native::BeginDrawDirect(base, device, end_offset);
}
FUTARI_DIRECT_EXPORT void FutariEndDrawDirect(uint8_t* base, const me::native::DrawDirect* d) {
  me::native::EndDrawDirect(base, *d);
}
