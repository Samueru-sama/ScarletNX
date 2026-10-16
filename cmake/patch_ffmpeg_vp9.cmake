# SPDX-License-Identifier: GPL-3.0-or-later
# NVDEC supplies the probabilities for each VP9 picture but reports show_frame only in the next picture. Reconstructed
# packets can finish at their own fence when the decoder starts each packet from the supplied table's default baseline
# and takes previous visibility from the current picture setup.
set(snx_vp9_file "${FFMPEG_SOURCE}/libavcodec/vp9.c")
file(READ "${snx_vp9_file}" snx_vp9_source)
set(snx_vp9_before "    last_invisible   = s->s.h.invisible;")
set(snx_vp9_after "    last_invisible   = s->s.h.invisible;\n    if (avctx->opaque && ((const unsigned int *)avctx->opaque)[0] == 0x534e5856)\n        last_invisible = ((const unsigned int *)avctx->opaque)[1];")
string(FIND "${snx_vp9_source}" "${snx_vp9_before}" snx_vp9_position)
if(snx_vp9_position EQUAL -1)
    message(FATAL_ERROR "The pinned FFmpeg VP9 visibility code changed")
endif()
string(REPLACE "${snx_vp9_before}" "${snx_vp9_after}" snx_vp9_source "${snx_vp9_source}")
set(snx_vp9_before "    // next 16 bits is size of the rest of the header (arith-coded)")
set(snx_vp9_after "    if (avctx->opaque && ((const unsigned int *)avctx->opaque)[0] == 0x534e5856) {\n        for (int snx_i = 0; snx_i < 4; snx_i++) {\n            s->prob_ctx[snx_i].p = ff_vp9_default_probs;\n            memcpy(s->prob_ctx[snx_i].coef, ff_vp9_default_coef_probs,\n                   sizeof(ff_vp9_default_coef_probs));\n        }\n    }\n\n    // next 16 bits is size of the rest of the header (arith-coded)")
string(FIND "${snx_vp9_source}" "${snx_vp9_before}" snx_vp9_position)
if(snx_vp9_position EQUAL -1)
    message(FATAL_ERROR "The pinned FFmpeg VP9 probability code changed")
endif()
string(REPLACE "${snx_vp9_before}" "${snx_vp9_after}" snx_vp9_source "${snx_vp9_source}")
file(WRITE "${snx_vp9_file}" "${snx_vp9_source}")
