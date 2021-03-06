/*
 * Copyright (c) 2016 Dmitry Osipenko <digetx@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "decoder.h"
#include "syntax_parse.h"
#include "transforms/common.h"

#define ARRAY_SIZE(x)	(sizeof(x) / sizeof(*(x)))

void decoder_init(decoder_context *decoder, void *data, uint32_t size)
{
	int i;

	bzero(decoder, sizeof(*decoder));

	bitstream_reader_selftest();
	bitstream_init(&decoder->reader, data, size);

	for (i = 0; i < ARRAY_SIZE(decoder->frames); i++) {
		decoder->frames[i] = malloc(sizeof(frame_data));
	}
}

void decoder_set_notify(decoder_context *decoder,
			void (*frame_decoded_notify)(decoder_context*),
			void *opaque)
{
	decoder->frame_decoded_notify = frame_decoded_notify;
	decoder->opaque = opaque;
}

void decoder_reset_SPS(decoder_context_sps *sps)
{
	free(sps->offset_for_ref_frame);
	bzero(sps, sizeof(*sps));
}

void decoder_reset_PPS(decoder_context_pps *pps)
{
	free(pps->run_length_minus1);
	free(pps->top_left);
	free(pps->bottom_right);
	free(pps->slice_group_id);
	bzero(pps, sizeof(*pps));
}

void decoder_reset_SH(decoder_context *decoder)
{
	free(decoder->sh.pred_weight_l0);
	free(decoder->sh.pred_weight_l1);
	bzero(&decoder->sh, sizeof(decoder->sh));
}

void decoder_reset_SD(decoder_context *decoder)
{
	bzero(&decoder->sd, sizeof(decoder->sd));
}

static void decoder_reset_context(decoder_context *decoder)
{
	decoder_reset_SH(decoder);
	decoder_reset_SD(decoder);

	bzero(&decoder->nal, sizeof(decoder->nal));
}

size_t decoder_image_frame_size(decoder_context *decoder)
{
	unsigned pic_width_in_mbs = decoder->active_sps->pic_width_in_mbs_minus1 + 1;
	unsigned pic_height_in_mbs = decoder->active_sps->pic_height_in_map_units_minus1 + 1;
	unsigned img_width = pic_width_in_mbs * 16;
	unsigned img_height = pic_height_in_mbs * 16;
	unsigned img_size = img_width * img_height;

	return img_size + img_size / 2;
}

void decoder_render_macroblock(decoder_context *decoder, unsigned mb_id)
{
	macroblock *mb = &decoder->frames[0]->macroblocks[mb_id];
	uint8_t *decoded_image = decoder->decoded_image;
	unsigned pic_width_in_mbs = decoder->active_sps->pic_width_in_mbs_minus1 + 1;
	unsigned pic_height_in_mbs = decoder->active_sps->pic_height_in_map_units_minus1 + 1;
	unsigned img_width = pic_width_in_mbs * 16;
	unsigned img_height = pic_height_in_mbs * 16;
	unsigned img_size = img_width * img_height;
	unsigned sub_mb_id;
	unsigned plane_id;

	for (sub_mb_id = 0; sub_mb_id < 16; sub_mb_id++) {
		uint8_t *luma_decoded = mb->luma_decoded[sub_mb_id];
		int x_in_mbs = mb_id % pic_width_in_mbs;
		int y_in_mbs = mb_id / pic_width_in_mbs;
		int offset_mb = x_in_mbs * 16 + y_in_mbs * img_width * 16;
		int x_in_mb = mb_scan_map(sub_mb_id) % 4;
		int y_in_mb = mb_scan_map(sub_mb_id) / 4;
		int offset_sub_mb = x_in_mb * 4 + y_in_mb * img_width * 4;
		int x, y;

		for (y = 0; y < 4; y++) {
			for (x = 0; x < 4; x++) {
				unsigned offt = offset_mb + offset_sub_mb;
				decoded_image[offt + y * img_width + x] = luma_decoded[y * 4 + x];
			}
		}
	}

	img_width /= 2;

	for (plane_id = 0; plane_id < 2; plane_id++) {
		for (sub_mb_id = 0; sub_mb_id < 4; sub_mb_id++) {
			uint8_t *chroma_decoded;
			int x_in_mbs = mb_id % pic_width_in_mbs;
			int y_in_mbs = mb_id / pic_width_in_mbs;
			int offset_mb = x_in_mbs * 8 + y_in_mbs * img_width * 8;
			int x_in_mb = sub_mb_id % 2;
			int y_in_mb = sub_mb_id / 2;
			int offset_sub_mb;
			int x, y;

			offset_sub_mb  = img_size + plane_id * img_size / 4;
			offset_sub_mb += x_in_mb * 4 + y_in_mb * img_width * 4;

			SETUP_PLANE_PTR(chroma_decoded, plane_id, mb, sub_mb_id);

			for (y = 0; y < 4; y++) {
				for (x = 0; x < 4; x++) {
					unsigned offt = offset_mb + offset_sub_mb;
					decoded_image[offt + y * img_width + x] = chroma_decoded[y * 4 + x];
				}
			}
		}
	}
}

static int decode_macroblock(decoder_context *decoder, unsigned mb_id, int QPY,
			     int QpBdOffsetC, int QpBdOffsetY,
			     int qpprime_y_zero_transform_bypass_flag)
{
	macroblock *mb = &decoder->frames[0]->macroblocks[mb_id];
	int partPredMode = MbPartPredMode(mb, decoder->sh.slice_type);
	unsigned chroma_pred_mode = mb->intra_chroma_pred_mode;
	int QP_Y, QPcb, QPcr, qPOffsetCb, qPOffsetCr, qPi;
	int TransformBypassModeFlag = 0;
	int16_t residual[16][16];
	int16_t residual_DC[16];
	int sub_mb_id;
	int plane_id;

	DECODE_DPRINT("---- decoding macroblock id = %d %s ----\n",
		      mb_id, (partPredMode != Intra_16x16) ? "4x4" : "16x16");

	QPY = ((QPY + mb->mb_qp_delta + 52 + 2 * QpBdOffsetY) %
					(52 + QpBdOffsetY)) - QpBdOffsetY;
	QP_Y = QPY - QpBdOffsetY;

	if (qpprime_y_zero_transform_bypass_flag == 1 && QP_Y == 0) {
		DECODE_ERR("TransformBypassModeFlag unimplemented\n");
		TransformBypassModeFlag = 1;
	}

	qPOffsetCb = decoder->active_pps->chroma_qp_index_offset;
	qPOffsetCr = decoder->active_pps->second_chroma_qp_index_offset;

	qPi = Clip3(-QpBdOffsetC, 51, QPY + qPOffsetCb);
	QPcb = qPc(qPi) + qPOffsetCb;
	qPi = Clip3(-QpBdOffsetC, 51, QPY + qPOffsetCr);
	QPcr = qPc(qPi) + qPOffsetCr;

	mb->QP_Y = QP_Y;
	mb->QPcb = QPcb;
	mb->QPcr = QPcr;

	if (partPredMode == Intra_16x16) {
		if (mb->luma_DC.totalcoeff) {
			TRANSFORM_DPRINT("decoding luma DC:\n");

			unzigzag_4x4(mb->luma_DC.coeffs);

			if (!TransformBypassModeFlag) {
				int16_t transformed[16];

				inv_transform_luma_DC(mb->luma_DC.coeffs,
						      transformed);

				inv_scale_luma_DC(decoder, transformed,
						  residual_DC, QP_Y);
			} else {
				inv_scale_luma_DC(decoder, mb->luma_DC.coeffs,
						  residual_DC, QP_Y);
			}
		} else {
			bzero(residual_DC, sizeof(residual_DC));
		}


	}

	for (sub_mb_id = 0; sub_mb_id < 16; sub_mb_id++) {
		if (mb->luma_AC[sub_mb_id].totalcoeff == 0) {
			bzero(residual[sub_mb_id], sizeof(residual[sub_mb_id]));

			if (partPredMode != Intra_16x16) {
				mb->luma_has_transform_coeffs[sub_mb_id] = 0;
				continue;
			}

			if (residual_DC[mb_scan_map(sub_mb_id)] == 0) {
				mb->luma_has_transform_coeffs[sub_mb_id] = 0;
				continue;
			}
		}

		mb->luma_has_transform_coeffs[sub_mb_id] = 1;

		DECODE_DPRINT("decoding luma AC[%d]:\n", sub_mb_id);

		if (mb->luma_AC[sub_mb_id].totalcoeff != 0) {
			if (partPredMode == Intra_16x16) {
				unzigzag_4x4_15(mb->luma_AC[sub_mb_id].coeffs);
			} else {
				unzigzag_4x4(mb->luma_AC[sub_mb_id].coeffs);
			}

			if (!TransformBypassModeFlag) {
				inv_scale_4x4(decoder, mb->luma_AC[sub_mb_id].coeffs,
					      residual[sub_mb_id], QP_Y);
			}
		}

		if (partPredMode == Intra_16x16) {
			residual[sub_mb_id][0] = residual_DC[mb_scan_map(sub_mb_id)];
		}

		if (!TransformBypassModeFlag) {
			inv_transform_4x4(residual[sub_mb_id]);
		}
	}

	if (partPredMode == Intra_16x16) {
		mb_apply_luma_prediction_16x16(decoder, mb_id,
					       (mb->mb_type - 1) % 4, residual);
	}

	if (partPredMode == Intra_4x4) {
		mb_apply_luma_prediction_4x4(decoder, mb_id, residual);
	}

	for (plane_id = 0; plane_id < 2; plane_id++) {
		macro_sub_block *chroma_DC =
			plane_id ? &mb->chroma_V_DC : &mb->chroma_U_DC;
		int QPC = plane_id ? QPcr : QPcb;

		if (chroma_DC->totalcoeff) {
			DECODE_DPRINT("decoding chroma DC %s:\n",
				      plane_id ? "red" : "blue");

			if (!TransformBypassModeFlag) {
				int16_t transformed[4];

				inv_transform_chroma_DC(chroma_DC->coeffs,
							transformed);

				inv_scale_chroma_DC(decoder, transformed,
						    residual_DC, QPC);
			} else {
				inv_scale_chroma_DC(decoder, chroma_DC->coeffs,
						    residual_DC, QPC);
			}
		} else {
			bzero(residual_DC, sizeof(residual_DC));
		}

		for (sub_mb_id = 0; sub_mb_id < 4; sub_mb_id++) {
			macro_sub_block *chroma_AC =
				plane_id ? mb->chroma_V_AC : mb->chroma_U_AC;

			if (chroma_AC[sub_mb_id].totalcoeff == 0) {
				bzero(residual[sub_mb_id], sizeof(residual[sub_mb_id]));

				if (residual_DC[sub_mb_id] == 0) {
					continue;
				}
			} else {
				unzigzag_4x4_15(chroma_AC[sub_mb_id].coeffs);

				if (!TransformBypassModeFlag) {
					inv_scale_4x4(decoder, chroma_AC[sub_mb_id].coeffs,
						      residual[sub_mb_id], QPC);
				}
			}

			DECODE_DPRINT("decoding chroma AC[%d] %s:\n",
				      sub_mb_id, plane_id ? "red" : "blue");

			residual[sub_mb_id][0] = residual_DC[sub_mb_id];

			if (!TransformBypassModeFlag) {
				inv_transform_4x4(residual[sub_mb_id]);
			}
		}

		mb_apply_chroma_prediction(decoder, plane_id, mb_id, residual,
					   chroma_pred_mode);
	}

	return QPY;
}

void decode_current_slice(decoder_context *decoder, unsigned last_mb_id)
{
	int qpprime_y_zero_transform_bypass_flag = decoder->active_sps->qpprime_y_zero_transform_bypass_flag;
	int QPY = decoder->active_pps->pic_init_qp_minus26 + 26 + decoder->sh.slice_qp_delta;
	int QpBdOffsetC = 6 * decoder->active_sps->bit_depth_chroma_minus8;
	int QpBdOffsetY = 6 * decoder->active_sps->bit_depth_luma_minus8;
	unsigned CurrMbAddr = decoder->sh.first_mb_in_slice;
	unsigned last_mb_in_frame;

	last_mb_in_frame  = decoder->active_sps->pic_width_in_mbs_minus1 + 1;
	last_mb_in_frame *= decoder->active_sps->pic_height_in_map_units_minus1 + 1;

	DECODE_DPRINT("last_mb_in_frame %d\n", last_mb_in_frame);

	for (; CurrMbAddr < last_mb_id; CurrMbAddr++) {
		QPY = decode_macroblock(decoder, CurrMbAddr, QPY,
					QpBdOffsetC, QpBdOffsetY,
					qpprime_y_zero_transform_bypass_flag);
	}

	if (decoder->sh.disable_deblocking_filter_idc != 1) {
		CurrMbAddr = decoder->sh.first_mb_in_slice;

		for (; CurrMbAddr < last_mb_id; CurrMbAddr++) {
			mb_apply_deblocking(decoder, CurrMbAddr);
		}
	}

	if (CurrMbAddr == last_mb_in_frame) {
		size_t img_sz = decoder_image_frame_size(decoder);

		decoder->decoded_image = realloc(decoder->decoded_image, img_sz);
		assert(decoder->decoded_image != NULL);

		for (CurrMbAddr = 0; CurrMbAddr < last_mb_in_frame; CurrMbAddr++) {
			decoder_render_macroblock(decoder, CurrMbAddr);
		}

		if (decoder->frame_decoded_notify != NULL) {
			decoder->frame_decoded_notify(decoder);
		}
	}
}
