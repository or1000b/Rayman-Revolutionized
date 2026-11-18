# Local fixes to the fetched paraLLEl-GS sources. Applied in place at configure time; each one is
# idempotent (skipped when its marker is already in the file) and fails loudly if its anchor is gone,
# so a paraLLEl-GS update cannot drop a fix silently.

function(ps2x_patch_source file anchor replacement marker)
    file(READ "${file}" content)
    string(FIND "${content}" "${marker}" already)
    if(NOT already EQUAL -1)
        return()
    endif()
    string(FIND "${content}" "${anchor}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "paraLLEl-GS patch '${marker}': anchor not found in ${file}")
    endif()
    string(REPLACE "${anchor}" "${replacement}" content "${content}")
    file(WRITE "${file}" "${content}")
    message(STATUS "paraLLEl-GS: applied patch '${marker}'")
endfunction()

# compute_page_rect: for a texture rect that is not page aligned and spans several pages, the block
# range inside a page wraps (max_block < min_block). "max - min + 1" is unsigned, becomes ~4 billion,
# and swizzle_block_iterate_mask never returns: the EE thread hangs inside gif_transfer with the GS
# lock held (Rayman Revolution, right after PRESS START). Treat that case as a straddle, which is the
# conservative answer the function already has.
ps2x_patch_source("${ps2x_parallel_gs_SOURCE_DIR}/gs/gs_util.cpp"
    "uint64_t block_mask = swizzle_block_mask_PS2(block_offset,"
    "uint64_t block_mask = (max_block_x < min_block_x || max_block_y < min_block_y) ? ~0ull : /* PS2RECOMP-PATCH page-rect-wrap */ swizzle_block_mask_PS2(block_offset,"
    "PS2RECOMP-PATCH page-rect-wrap")

# check_flush_stats submits the pending work every 4096 primitives ("normal 3D games seem to be between 4k and 40k
# primitives per frame"); Rayman draws ~90k in busy places, so ~20 submissions a frame, each with a frame context
# advance and a driver submit on the GS thread. PS2X_PGS_FLUSH_PRIMS sets that count; default 16384 (2026-11-16, the
# bridge in the Minisaurus Plain, regular 120: 4096 -> 115-117 FPS, 16384 / 32768 -> ~118, 65536 -> 104-117: the GPU
# starts too late). Pictures identical.
ps2x_patch_source("${ps2x_parallel_gs_SOURCE_DIR}/gs/gs_renderer.cpp"
    [==[static constexpr uint32_t MinimumPrimitivesForFlush = 4096;]==]
    [==[static uint32_t ps2x_flush_prims() { const char *v = getenv("PS2X_PGS_FLUSH_PRIMS"); const unsigned long n = v ? strtoul(v, nullptr, 0) : 0ul; return n >= 256ul && n <= (1ul << 20) ? uint32_t(n) : 16384u; } /* PS2RECOMP-PATCH flush-prims */
static const uint32_t MinimumPrimitivesForFlush = ps2x_flush_prims();]==]
    "PS2RECOMP-PATCH flush-prims")

# Palette memoization kept across uploads (memo-keep). paraLLEl-GS skips a CLUT load that repeats one of the last 16 of
# the render pass, but forgets them all whenever a write hits VRAM that was read as a texture. Rayman draws the world in
# passes and, for every object, uploads three small lightmaps into the same three texture slots and then reloads the
# CLUT with every texture change (~5000 loads a frame at the bridge in the Minisaurus Plain): each lightmap upload made
# paraLLEl-GS forget, so ~500 loads a frame reached the GPU, each a step of a serial compute job (~2.2 ms of GPU time a
# frame; with PS2X_GS_SSAA=16 the GPU was the limit there, 2026-11-17). Now, when the forgetting comes from a host->local
# transfer or a local->local copy, only the remembered CLUTs whose source pages the transfer writes are marked stale
# (kept in the list, as the bank-history check walks it, but never matched), and every transfer marks them so even when
# the page tracker does not flush. Everything else forgets as before. A remembered CLUT is also not matched once 512
# CLUT uploads came after it (the GPU keeps 1024 CLUT instances in a ring). PS2X_PGS_MEMO_KEEP=0: upstream behaviour.
set(_ps2x_gsi "${ps2x_parallel_gs_SOURCE_DIR}/gs/gs_interface.cpp")
ps2x_patch_source("${ps2x_parallel_gs_SOURCE_DIR}/gs/gs_interface.hpp"
    [==[PaletteUploadDescriptor upload;]==]
    [==[PaletteUploadDescriptor upload;
			PageRect ps2x_page; /* PS2RECOMP-PATCH memo-keep-fields: the CLUT's source pages */
			uint32_t ps2x_serial; // ps2x_palette_serial when its CLUT instance was made
			bool ps2x_stale;      // its source pages were written since: not matched]==]
    "PS2RECOMP-PATCH memo-keep-fields")
ps2x_patch_source("${ps2x_parallel_gs_SOURCE_DIR}/gs/gs_interface.hpp"
    [==[void forget_in_render_pass_memoization();]==]
    [==[void forget_in_render_pass_memoization();
	// PS2Recomp memo-keep (PatchParallelGs.cmake). /* PS2RECOMP-PATCH memo-keep-members */
	void ps2x_reset_memo();
	void ps2x_mark_memo_stale(const PageRect &rect);
	void ps2x_transfer_begin(const PageRect &rect);
	void ps2x_transfer_end(const PageRect &rect);
	bool ps2x_in_transfer = false;
	PageRect ps2x_transfer_rect = {};
	uint32_t ps2x_palette_serial = 0;]==]
    "PS2RECOMP-PATCH memo-keep-members")
ps2x_patch_source("${_ps2x_gsi}"
    [==[void GSInterface::forget_in_render_pass_memoization()]==]
    [==[// PS2Recomp memo-keep (PatchParallelGs.cmake): page-level overlap (blocks ignored: conservative). /* PS2RECOMP-PATCH memo-keep-functions */
static bool ps2x_memo_keep_on()
{
	static const bool on = [] { const char *e = getenv("PS2X_PGS_MEMO_KEEP"); return !(e && e[0] == '0'); }();
	return on;
}

static bool ps2x_pages_overlap(const PageRect &a, const PageRect &b)
{
	for (uint32_t ay = 0; ay < a.page_height; ay++)
		for (uint32_t ax = 0; ax < a.page_width; ax++)
		{
			const uint32_t pa = (a.base_page + ay * a.page_stride + ax) & 511u;
			for (uint32_t by = 0; by < b.page_height; by++)
				for (uint32_t bx = 0; bx < b.page_width; bx++)
					if (((b.base_page + by * b.page_stride + bx) & 511u) == pa)
						return true;
		}
	return false;
}

void GSInterface::ps2x_mark_memo_stale(const PageRect &rect)
{
	for (uint32_t i = 0; i < render_pass.num_memoized_palettes; i++)
		if (ps2x_pages_overlap(render_pass.memoized_palettes[i].ps2x_page, rect))
			render_pass.memoized_palettes[i].ps2x_stale = true;
}

void GSInterface::ps2x_reset_memo()
{
	if (ps2x_in_transfer)
		ps2x_mark_memo_stale(ps2x_transfer_rect);
	else
		render_pass.num_memoized_palettes = 0u; // "0u": the memo-keep-reset patch replaces the "= 0;" form
}

void GSInterface::ps2x_transfer_begin(const PageRect &rect)
{
	ps2x_in_transfer = ps2x_memo_keep_on();
	ps2x_transfer_rect = rect;
}

void GSInterface::ps2x_transfer_end(const PageRect &rect)
{
	ps2x_in_transfer = false;
	ps2x_mark_memo_stale(rect);
}

void GSInterface::forget_in_render_pass_memoization()]==]
    "PS2RECOMP-PATCH memo-keep-functions")
foreach(_pair
        "render_pass.num_memoized_palettes = 0;|ps2x_reset_memo(); /* PS2RECOMP-PATCH memo-keep-reset (was: render_pass.num_memoized_palettes = 0) */"
        "tracker.mark_transfer_write(dst_rect);|ps2x_transfer_begin(dst_rect); tracker.mark_transfer_write(dst_rect); ps2x_transfer_end(dst_rect); /* PS2RECOMP-PATCH memo-keep-upload */"
        "transfer_state.copy.needs_shadow_vram = tracker.mark_transfer_copy(dst_rect, src_rect);|ps2x_transfer_begin(dst_rect); transfer_state.copy.needs_shadow_vram = tracker.mark_transfer_copy(dst_rect, src_rect); ps2x_transfer_end(dst_rect); /* PS2RECOMP-PATCH memo-keep-copy */"
        "if (memoized.csa_mask == page.csa_mask &&|if (!memoized.ps2x_stale && ps2x_palette_serial - memoized.ps2x_serial < 512u && /* PS2RECOMP-PATCH memo-keep-match */ memoized.csa_mask == page.csa_mask &&"
        "if (memoized.clut_instance != render_pass.clut_instance)|const uint32_t ps2x_found_serial = memoized.ps2x_serial; /* PS2RECOMP-PATCH memo-keep-found */ if (memoized.clut_instance != render_pass.clut_instance)"
        "last_memoized.clut_instance = render_pass.clut_instance;|last_memoized.clut_instance = render_pass.clut_instance; last_memoized.ps2x_page = page; last_memoized.ps2x_serial = ps2x_found_serial; last_memoized.ps2x_stale = false; /* PS2RECOMP-PATCH memo-keep-move */"
        "render_pass.clut_instance = renderer.update_palette_cache(palette_desc);|ps2x_palette_serial++; /* PS2RECOMP-PATCH memo-keep-serial */ render_pass.clut_instance = renderer.update_palette_cache(palette_desc);"
        "auto &memoized = render_pass.memoized_palettes[render_pass.num_memoized_palettes++];|auto &memoized = render_pass.memoized_palettes[render_pass.num_memoized_palettes++]; memoized.ps2x_page = page; memoized.ps2x_serial = ps2x_palette_serial; memoized.ps2x_stale = false; /* PS2RECOMP-PATCH memo-keep-new */")
    string(FIND "${_pair}" "|" _bar)
    string(SUBSTRING "${_pair}" 0 ${_bar} _anchor)
    math(EXPR _rest "${_bar} + 1")
    string(SUBSTRING "${_pair}" ${_rest} -1 _replacement)
    string(REGEX MATCH "PS2RECOMP-PATCH memo-keep-[a-z-]+" _marker "${_replacement}")
    ps2x_patch_source("${_ps2x_gsi}" "${_anchor}" "${_replacement}" "${_marker}")
endforeach()

# PS2X_GS_SSAA=16: a 4x scanout (2560 x 2048 for PAL, real 1440p: the user's request, 2026-11-17). With 16 samples on
# the ordered grid each PS2 pixel holds a 4 x 4 grid of samples, but paraLLEl-GS scans out at most 2x (each output pixel
# averages the 4 samples of a quarter). VSyncInfo::ps2x_scanout_4x makes it 4x: the circuits and the merged picture 4x
# the mode, the blits scaled to match, and the circuits sampled by sample_circuit_4x.frag (one sample per output pixel;
# ps2xRuntime/src/lib/gs/pgs/shaders, SPIR-V from tools/pgs_shaders/compile_scanout4x.py), which reuses the original
# shader's reflection layout (same bindings, push constants and specialization constants). Everything else stays 2x.
function(ps2x_require_near file before text)
    file(READ "${file}" content)
    string(FIND "${content}" "${text}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "paraLLEl-GS patch scanout-4x: '${text}' not found in ${file}")
    endif()
    math(EXPR from "${at} - 120")
    if(from LESS 0)
        set(from 0)
    endif()
    string(SUBSTRING "${content}" ${from} 120 window)
    string(FIND "${window}" "${before}" ok)
    if(ok EQUAL -1)
        message(FATAL_ERROR "paraLLEl-GS patch scanout-4x: '${before}' is not right before '${text}' in ${file} (shader layout table changed)")
    endif()
endfunction()
ps2x_require_near("${ps2x_parallel_gs_SOURCE_DIR}/gs/shaders/slangmosh.hpp"
    "layout.unserialize(reflection_bank + 4176, 348);" "this->sample_circuit[0] = device.request_shader(")
ps2x_require_near("${ps2x_parallel_gs_SOURCE_DIR}/gs/shaders/slangmosh.hpp"
    "layout.unserialize(reflection_bank + 4524, 348);" "this->sample_circuit[1] = device.request_shader(")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../src/lib/gs/pgs/shaders/sample_circuit_4x_spirv.h"
    "${ps2x_parallel_gs_SOURCE_DIR}/gs/shaders/ps2x_sample_circuit_4x_spirv.h" COPYONLY)
ps2x_patch_source("${ps2x_parallel_gs_SOURCE_DIR}/gs/gs_interface.hpp"
    [==[bool skip_deinterlace;]==]
    [==[bool skip_deinterlace;
	// PS2Recomp: with 16x ordered-grid SSAA and high_resolution_scanout, scan out at 4x instead of 2x. /* PS2RECOMP-PATCH scanout-4x-info */
	bool ps2x_scanout_4x;]==]
    "PS2RECOMP-PATCH scanout-4x-info")
ps2x_patch_source("${ps2x_parallel_gs_SOURCE_DIR}/gs/gs_renderer.hpp"
    [==[Vulkan::Program *sample_quad[2] = {};]==]
    [==[Vulkan::Program *sample_quad[2] = {};
	Vulkan::Program *ps2x_sample_quad_4x[2] = {}; /* PS2RECOMP-PATCH scanout-4x-members */
	uint32_t ps2x_scanout_scale = 2;]==]
    "PS2RECOMP-PATCH scanout-4x-members")
set(_ps2x_gsr "${ps2x_parallel_gs_SOURCE_DIR}/gs/gs_renderer.cpp")
ps2x_patch_source("${_ps2x_gsr}"
    [==[#include "shaders/slangmosh.hpp"]==]
    [==[#include "shaders/slangmosh.hpp"
#include "shaders/ps2x_sample_circuit_4x_spirv.h" /* PS2RECOMP-PATCH scanout-4x-include */]==]
    "PS2RECOMP-PATCH scanout-4x-include")
ps2x_patch_source("${_ps2x_gsr}"
    [==[sample_quad[1] = device_->request_program(shaders.quad, shaders.sample_circuit[1]);]==]
    [==[sample_quad[1] = device_->request_program(shaders.quad, shaders.sample_circuit[1]);
	{ /* PS2RECOMP-PATCH scanout-4x-programs: the 4x scanout shader, with sample_circuit's layouts */
		Vulkan::ResourceLayout ps2x_layout;
		ps2x_layout.unserialize(reflection_bank + 4176, 348);
		auto *ps2x_s0 = device_->request_shader(ps2x_sample_circuit_4x_spirv0, sizeof(ps2x_sample_circuit_4x_spirv0), &ps2x_layout);
		ps2x_layout.unserialize(reflection_bank + 4524, 348);
		auto *ps2x_s1 = device_->request_shader(ps2x_sample_circuit_4x_spirv1, sizeof(ps2x_sample_circuit_4x_spirv1), &ps2x_layout);
		ps2x_sample_quad_4x[0] = ps2x_s0 ? device_->request_program(shaders.quad, ps2x_s0) : nullptr;
		ps2x_sample_quad_4x[1] = ps2x_s1 ? device_->request_program(shaders.quad, ps2x_s1) : nullptr;
	}]==]
    "PS2RECOMP-PATCH scanout-4x-programs")
ps2x_patch_source("${_ps2x_gsr}"
    [==[bool EN1 = priv.pmode.EN1;]==]
    [==[// PS2Recomp: the scanout's scale over the mode (1, 2, or 4 with ps2x_scanout_4x and a 4 x 4 sample grid). /* PS2RECOMP-PATCH scanout-4x-scale */
	const uint32_t ps2x_hires_scale = !high_resolution_scanout ? 1u :
		(info.ps2x_scanout_4x && sampling_rate_x_log2 == 2 && sampling_rate_y_log2 == 2 &&
		 ps2x_sample_quad_4x[0] && ps2x_sample_quad_4x[1] ? 4u : 2u);
	ps2x_scanout_scale = ps2x_hires_scale == 4u ? 4u : 2u;
	bool EN1 = priv.pmode.EN1;]==]
    "PS2RECOMP-PATCH scanout-4x-scale")
foreach(_pair
        "image_info.width *= 2;|image_info.width *= ps2x_hires_scale; /* PS2RECOMP-PATCH scanout-4x-circuit-w */"
        "image_info.height *= 2;|image_info.height *= ps2x_hires_scale; /* PS2RECOMP-PATCH scanout-4x-circuit-h */"
        "valid_extent.width *= 2;|valid_extent.width *= ps2x_scanout_scale; /* PS2RECOMP-PATCH scanout-4x-valid-w */"
        "valid_extent.height *= 2;|valid_extent.height *= ps2x_scanout_scale; /* PS2RECOMP-PATCH scanout-4x-valid-h */"
        "cmd.set_program(sample_quad[promoted ? 1 : 0]);|cmd.set_program(super_samples > 1 && ps2x_scanout_scale == 4u ? ps2x_sample_quad_4x[promoted ? 1 : 0] : sample_quad[promoted ? 1 : 0]); /* PS2RECOMP-PATCH scanout-4x-select */"
        "vp.x *= 2.0f;|vp.x *= float(ps2x_hires_scale); /* PS2RECOMP-PATCH scanout-4x-vp-x */"
        "vp.y *= 2.0f;|vp.y *= float(ps2x_hires_scale); /* PS2RECOMP-PATCH scanout-4x-vp-y */"
        "vp.width *= 2.0f;|vp.width *= float(ps2x_hires_scale); /* PS2RECOMP-PATCH scanout-4x-vp-w */"
        "vp.height *= 2.0f;|vp.height *= float(ps2x_hires_scale); /* PS2RECOMP-PATCH scanout-4x-vp-h */"
        "vp.y -= 1.0f;|vp.y -= float(ps2x_hires_scale / 2u); /* PS2RECOMP-PATCH scanout-4x-vp-field */"
        "image_info.width = mode_width << int(high_resolution_scanout);|image_info.width = mode_width * ps2x_hires_scale; /* PS2RECOMP-PATCH scanout-4x-merged-w */"
        "image_info.height = mode_height << int(high_resolution_scanout);|image_info.height = mode_height * ps2x_hires_scale; /* PS2RECOMP-PATCH scanout-4x-merged-h */"
        "effective_mode_width *= 2;|effective_mode_width *= ps2x_hires_scale; /* PS2RECOMP-PATCH scanout-4x-raw-w */"
        "effective_mode_height *= 2;|effective_mode_height *= ps2x_hires_scale; /* PS2RECOMP-PATCH scanout-4x-raw-h */"
        "result.internal_width = result.image->get_width() >> int(high_resolution_scanout);|result.internal_width = result.image->get_width() / ps2x_hires_scale; /* PS2RECOMP-PATCH scanout-4x-internal-w */"
        "result.internal_height = result.image->get_height() >> int(high_resolution_scanout);|result.internal_height = result.image->get_height() / ps2x_hires_scale; /* PS2RECOMP-PATCH scanout-4x-internal-h */"
        "horiz_resolution0 >>= 1;|horiz_resolution0 /= ps2x_hires_scale; /* PS2RECOMP-PATCH scanout-4x-adapt-0 */"
        "horiz_resolution1 >>= 1;|horiz_resolution1 /= ps2x_hires_scale; /* PS2RECOMP-PATCH scanout-4x-adapt-1 */")
    string(FIND "${_pair}" "|" _bar)
    string(SUBSTRING "${_pair}" 0 ${_bar} _anchor)
    math(EXPR _rest "${_bar} + 1")
    string(SUBSTRING "${_pair}" ${_rest} -1 _replacement)
    string(REGEX MATCH "PS2RECOMP-PATCH scanout-4x-[a-z0-9-]+" _marker "${_replacement}")
    ps2x_patch_source("${_ps2x_gsr}" "${_anchor}" "${_replacement}" "${_marker}")
endforeach()

# Billboards at full resolution (2026-11-18, the user: glows, sparkles and Rayman's energy balls looked pixelated at
# quality 16). With super-sampling, triangle_setup.comp takes a primitive with the same Z at all three vertices and no
# change of Q for flat UI and snaps its attribute interpolation to the PS2 pixel; camera-facing billboards are just as
# flat, so they came out in blocks of one PS2 pixel. Rayman draws all its 2D at Z = 0xFFFF: only that counts as flat UI.
# paraLLEl-GS ships its shaders precompiled, so tools/pgs_shaders/compile_triangle_setup.py applies the change to its
# triangle_setup.comp and compiles it (ps2xRuntime/src/lib/gs/pgs/shaders/triangle_setup_spirv.h); the program is
# swapped in with the original's reflection layout (same bindings). PS2X_PGS_FLAT_UI_Z=0: paraLLEl-GS's own shader.
ps2x_require_near("${ps2x_parallel_gs_SOURCE_DIR}/gs/shaders/slangmosh.hpp"
    "layout.unserialize(reflection_bank + 1740, 348);" "this->triangle_setup = device.request_program(")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../src/lib/gs/pgs/shaders/triangle_setup_spirv.h"
    "${ps2x_parallel_gs_SOURCE_DIR}/gs/shaders/ps2x_triangle_setup_spirv.h" COPYONLY)
ps2x_patch_source("${_ps2x_gsr}"
    [==[#include "shaders/ps2x_sample_circuit_4x_spirv.h" /* PS2RECOMP-PATCH scanout-4x-include */]==]
    [==[#include "shaders/ps2x_sample_circuit_4x_spirv.h" /* PS2RECOMP-PATCH scanout-4x-include */
#include "shaders/ps2x_triangle_setup_spirv.h" /* PS2RECOMP-PATCH flat-ui-z-include */]==]
    "PS2RECOMP-PATCH flat-ui-z-include")
ps2x_patch_source("${_ps2x_gsr}"
    [==[weave_quad = device_->request_program(shaders.quad, shaders.weave);]==]
    [==[{ /* PS2RECOMP-PATCH flat-ui-z-program: triangle setup where only Z = 0xFFFF is flat UI (billboards super-sampled) */
		const char *ps2x_flat = getenv("PS2X_PGS_FLAT_UI_Z");
		if (!(ps2x_flat && ps2x_flat[0] == '0'))
		{
			Vulkan::ResourceLayout ps2x_ts_layout;
			ps2x_ts_layout.unserialize(reflection_bank + 1740, 348);
			if (auto *ps2x_ts = device_->request_program(ps2x_triangle_setup_spirv, sizeof(ps2x_triangle_setup_spirv), &ps2x_ts_layout))
				shaders.triangle_setup = ps2x_ts;
		}
	}
	weave_quad = device_->request_program(shaders.quad, shaders.weave);]==]
    "PS2RECOMP-PATCH flat-ui-z-program")
