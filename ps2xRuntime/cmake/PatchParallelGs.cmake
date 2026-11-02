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
