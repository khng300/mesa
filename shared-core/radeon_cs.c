/*
 * Copyright 2008 Jerome Glisse.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Jerome Glisse <glisse@freedesktop.org>
 */
#include "drmP.h"
#include "radeon_drm.h"
#include "radeon_drv.h"
#include "r300_reg.h"

int radeon_cs2_ioctl(struct drm_device *dev, void *data, struct drm_file *fpriv)
{
	struct drm_radeon_cs_parser parser;
	struct drm_radeon_private *dev_priv = dev->dev_private;
	struct drm_radeon_cs2 *cs = data;
	uint32_t cs_id;
	struct drm_radeon_cs_chunk __user **chunk_ptr = NULL;
	uint64_t *chunk_array;
	uint64_t *chunk_array_ptr;
	long size;
	int r, i;

	/* set command stream id to 0 which is fake id */
	cs_id = 0;
	cs->cs_id = cs_id;

	if (dev_priv == NULL) {
		DRM_ERROR("called with no initialization\n");
		return -EINVAL;
	}
	if (!cs->num_chunks) {
		return 0;
	}


	chunk_array = drm_calloc(cs->num_chunks, sizeof(uint64_t), DRM_MEM_DRIVER);
	if (!chunk_array) {
		return -ENOMEM;
	}

	chunk_array_ptr = (uint64_t *)(unsigned long)(cs->chunks);

	if (DRM_COPY_FROM_USER(chunk_array, chunk_array_ptr, sizeof(uint64_t)*cs->num_chunks)) {
		r = -EFAULT;
		goto out;
	}

	parser.dev = dev;
	parser.file_priv = fpriv;
	parser.reloc_index = -1;
	parser.ib_index = -1;
	parser.num_chunks = cs->num_chunks;
	/* copy out the chunk headers */
	parser.chunks = drm_calloc(parser.num_chunks, sizeof(struct drm_radeon_kernel_chunk), DRM_MEM_DRIVER);
	if (!parser.chunks) {
		return -ENOMEM;
	}

	for (i = 0; i < parser.num_chunks; i++) {
		struct drm_radeon_cs_chunk user_chunk;

		chunk_ptr = (void __user *)(unsigned long)chunk_array[i];

		if (DRM_COPY_FROM_USER(&user_chunk, chunk_ptr, sizeof(struct drm_radeon_cs_chunk))){
			r = -EFAULT;
			goto out;
		}
		parser.chunks[i].chunk_id = user_chunk.chunk_id;

		if (parser.chunks[i].chunk_id == RADEON_CHUNK_ID_RELOCS)
			parser.reloc_index = i;

		if (parser.chunks[i].chunk_id == RADEON_CHUNK_ID_IB)
			parser.ib_index = i;

		if (parser.chunks[i].chunk_id == RADEON_CHUNK_ID_OLD) {
			parser.ib_index = i;
			parser.reloc_index = -1;
		}

		parser.chunks[i].length_dw = user_chunk.length_dw;
		parser.chunks[i].chunk_data = (uint32_t *)(unsigned long)user_chunk.chunk_data;

		parser.chunks[i].kdata = NULL;
		size = parser.chunks[i].length_dw * sizeof(uint32_t);

		switch(parser.chunks[i].chunk_id) {
		case RADEON_CHUNK_ID_IB:
		case RADEON_CHUNK_ID_OLD:
			if (size == 0) {
				r = -EINVAL;
				goto out;
			}
		case RADEON_CHUNK_ID_RELOCS:
			if (size) {
				parser.chunks[i].kdata = drm_alloc(size, DRM_MEM_DRIVER);
				if (!parser.chunks[i].kdata) { 
					r = -ENOMEM;
					goto out;
				}
				
				if (DRM_COPY_FROM_USER(parser.chunks[i].kdata, parser.chunks[i].chunk_data, size)) {
					r = -EFAULT;
					goto out;
				}
			} else
				parser.chunks[i].kdata = NULL;
			break;
		default:
			break;
		}
		DRM_DEBUG("chunk %d %d %d %p\n", i, parser.chunks[i].chunk_id, parser.chunks[i].length_dw,
			  parser.chunks[i].chunk_data);
	}


	if (parser.chunks[parser.ib_index].length_dw > (16 * 1024)) {
		DRM_ERROR("cs->dwords too big: %d\n", parser.chunks[parser.ib_index].length_dw);
		r = -EINVAL;
		goto out;
	}

	/* get ib */
	r = dev_priv->cs.ib_get(&parser);
	if (r) {
		DRM_ERROR("ib_get failed\n");
		goto out;
	}

	/* now parse command stream */
	r = dev_priv->cs.parse(&parser);
	if (r) {
		goto out;
	}

	/* emit cs id sequence */
	dev_priv->cs.id_emit(&parser, &cs_id);

	cs->cs_id = cs_id;
		
out:
	dev_priv->cs.ib_free(&parser);

	for (i = 0; i < parser.num_chunks; i++) {
		if (parser.chunks[i].kdata)
			drm_free(parser.chunks[i].kdata, parser.chunks[i].length_dw * sizeof(uint32_t), DRM_MEM_DRIVER);
	}

	drm_free(parser.chunks, sizeof(struct drm_radeon_kernel_chunk)*parser.num_chunks, DRM_MEM_DRIVER);
	drm_free(chunk_array, sizeof(uint64_t)*parser.num_chunks, DRM_MEM_DRIVER);

	return r;
}

int radeon_cs_ioctl(struct drm_device *dev, void *data, struct drm_file *fpriv)
{
	struct drm_radeon_cs_parser parser;
	struct drm_radeon_private *dev_priv = dev->dev_private;
	struct drm_radeon_cs *cs = data;
	uint32_t *packets = NULL;
	uint32_t cs_id;
	long size;
	int r;
	struct drm_radeon_kernel_chunk chunk_fake[1];

	/* set command stream id to 0 which is fake id */
	cs_id = 0;
	cs->cs_id = cs_id;

	if (dev_priv == NULL) {
		DRM_ERROR("called with no initialization\n");
		return -EINVAL;
	}
	if (!cs->dwords) {
		return 0;
	}
	/* limit cs to 64K ib */
	if (cs->dwords > (16 * 1024)) {
		return -EINVAL;
	}
	/* copy cs from userspace maybe we should copy into ib to save
	 * one copy but ib will be mapped wc so not good for cmd checking
	 * somethings worth testing i guess (Jerome)
	 */
	size = cs->dwords * sizeof(uint32_t);
	packets = drm_alloc(size, DRM_MEM_DRIVER);
	if (packets == NULL) {
		return -ENOMEM;
	}
	if (DRM_COPY_FROM_USER(packets, (void __user *)(unsigned long)cs->packets, size)) {
		r = -EFAULT;
		goto out;
	}

	chunk_fake[0].chunk_id = RADEON_CHUNK_ID_OLD;
	chunk_fake[0].length_dw = cs->dwords;
	chunk_fake[0].kdata = packets;

	parser.dev = dev;
	parser.file_priv = fpriv;
	parser.num_chunks = 1;
	parser.chunks = chunk_fake;
	parser.ib_index = 0;
	parser.reloc_index = -1;

	/* get ib */
	r = dev_priv->cs.ib_get(&parser);
	if (r) {
		goto out;
	}

	/* now parse command stream */
	r = dev_priv->cs.parse(&parser);
	if (r) {
		goto out;
	}

	/* emit cs id sequence */
	dev_priv->cs.id_emit(&parser, &cs_id);
	COMMIT_RING();

	cs->cs_id = cs_id;
out:
	dev_priv->cs.ib_free(&parser);
	drm_free(packets, size, DRM_MEM_DRIVER);
	return r;
}

/* for non-mm */
static int radeon_nomm_relocate(struct drm_radeon_cs_parser *parser, uint32_t *reloc, uint32_t *offset)
{
	*offset = reloc[1];
	return 0;
}
#define RELOC_SIZE 2
#define RELOC_SIZE_NEW 0
#define RADEON_2D_OFFSET_MASK 0x3fffff

static __inline__ int radeon_cs_relocate_packet0(struct drm_radeon_cs_parser *parser, uint32_t offset_dw)
{
	struct drm_device *dev = parser->dev;
	drm_radeon_private_t *dev_priv = dev->dev_private;
	uint32_t hdr, reg, val, packet3_hdr;
	uint32_t tmp, offset;
	struct drm_radeon_kernel_chunk *ib_chunk;
	int ret;

	ib_chunk = &parser->chunks[parser->ib_index];
//	if (parser->reloc_index == -1)
//		is_old = 1;

	hdr = ib_chunk->kdata[offset_dw];
	reg = (hdr & R300_CP_PACKET0_REG_MASK) << 2;
	val = ib_chunk->kdata[offset_dw + 1];
	packet3_hdr = ib_chunk->kdata[offset_dw + 2];

	/* this is too strict we may want to expand the length in the future and have
	 old kernels ignore it. */ 
	if (parser->reloc_index == -1) {
		if (packet3_hdr != (RADEON_CP_PACKET3 | RADEON_CP_NOP | (RELOC_SIZE << 16))) {
			DRM_ERROR("Packet 3 was %x should have been %x: reg is %x at %d\n", packet3_hdr, RADEON_CP_PACKET3 | RADEON_CP_NOP | (RELOC_SIZE << 16), reg, offset_dw);
			return -EINVAL;
		}
		if (packet3_hdr != (RADEON_CP_PACKET3 | RADEON_CP_NOP | (RELOC_SIZE_NEW << 16))) {
			DRM_ERROR("Packet 3 was %x should have been %x: reg is %x at %d\n", packet3_hdr, RADEON_CP_PACKET3 | RADEON_CP_NOP | (RELOC_SIZE_NEW << 16), reg, offset_dw);
			return -EINVAL;

		}
	}
	
	switch(reg) {
	case RADEON_DST_PITCH_OFFSET:
	case RADEON_SRC_PITCH_OFFSET:
		/* pass in the start of the reloc */
		ret = dev_priv->cs.relocate(parser,
				ib_chunk->kdata + offset_dw + 2, &offset);
		if (ret) {
			return ret;
		}
		tmp = (val & RADEON_2D_OFFSET_MASK) << 10;
		val &= ~RADEON_2D_OFFSET_MASK;
		offset += tmp;
		offset >>= 10;
		val |= offset;
		break;
	case RADEON_RB3D_COLOROFFSET:
	case R300_RB3D_COLOROFFSET0:
	case R300_ZB_DEPTHOFFSET:
	case R300_TX_OFFSET_0:
	case R300_TX_OFFSET_0+4:
	case R200_PP_TXOFFSET_0:
	case R200_PP_TXOFFSET_1:
	case RADEON_PP_TXOFFSET_0:
	case RADEON_PP_TXOFFSET_1:
	        ret = dev_priv->cs.relocate(parser,
				ib_chunk->kdata + offset_dw + 2, &offset);
		if (ret) {
			DRM_ERROR("Failed to relocate %d\n", offset_dw);
			return ret;
		}

		offset &= 0xffffffe0;
		val += offset;
		break;
	default:
		break;
	}

	ib_chunk->kdata[offset_dw + 1] = val;
	return 0;
}

static int radeon_cs_relocate_packet3(struct drm_radeon_cs_parser *parser,
				      uint32_t offset_dw)
{
	drm_radeon_private_t *dev_priv = parser->dev->dev_private;
	uint32_t hdr, num_dw, reg, i;
	uint32_t offset, val, tmp, nptr, cptr;
	uint32_t *reloc;
	int ret;
	struct drm_radeon_kernel_chunk *ib_chunk;

	ib_chunk = &parser->chunks[parser->ib_index];
//	if (parser->reloc_index == -1)
//		is_old = 1;

	hdr = ib_chunk->kdata[offset_dw];
	num_dw = (hdr & RADEON_CP_PACKET_COUNT_MASK) >> 16;
	reg = hdr & 0xff00;

	switch(reg) {
	case RADEON_CNTL_HOSTDATA_BLT:
		val = ib_chunk->kdata[offset_dw + 2];
		ret = dev_priv->cs.relocate(parser,
				ib_chunk->kdata + offset_dw + num_dw + 2,
				&offset);
		if (ret) {
			return ret;
		}

		tmp = (val & RADEON_2D_OFFSET_MASK) << 10;
		val &= ~RADEON_2D_OFFSET_MASK;
		offset += tmp;
		offset >>= 10;
		val |= offset;

		ib_chunk->kdata[offset_dw + 2] = val;
		break;
	case RADEON_3D_LOAD_VBPNTR:
		nptr = ib_chunk->kdata[offset_dw + 1];
		cptr = offset_dw + 3;
		for (i = 0; i < (nptr & ~1); i+= 2) {
			reloc = ib_chunk->kdata + offset_dw + num_dw + 2;
			reloc += ((i + 0) * 2);
			ret = dev_priv->cs.relocate(parser, reloc, &offset);
			if (ret) {
				return ret;
			}
			ib_chunk->kdata[cptr] += offset;
			cptr += 1;
			reloc = ib_chunk->kdata + offset_dw + num_dw + 2;
			reloc += ((i + 1) * 2);
			ret = dev_priv->cs.relocate(parser, reloc, &offset);
			if (ret) {
				return ret;
			}
			ib_chunk->kdata[cptr] += offset;
			cptr += 2;
		}
		if (nptr & 1) {
			reloc = ib_chunk->kdata + offset_dw + num_dw + 2;
			reloc += ((nptr - 1) * 2);
			ret = dev_priv->cs.relocate(parser, reloc, &offset);
			if (ret) {
				return ret;
			}
			ib_chunk->kdata[cptr] += offset;
		}
		break;
	case RADEON_CP_INDX_BUFFER:
		reloc = ib_chunk->kdata + offset_dw + num_dw + 2;
		ret = dev_priv->cs.relocate(parser, reloc, &offset);
		if (ret) {
			return ret;
		}
		ib_chunk->kdata[offset_dw + 2] += offset;
		break;
	default:
		DRM_ERROR("Unknown packet3 0x%08X\n", hdr);
		return -EINVAL;
	}
	return 0;
}

int radeon_cs_packet0(struct drm_radeon_cs_parser *parser, uint32_t offset_dw)
{
	uint32_t hdr, num_dw, reg;
	int count_dw = 1;
	int ret;

	hdr = parser->chunks[parser->ib_index].kdata[offset_dw];
	num_dw = ((hdr & RADEON_CP_PACKET_COUNT_MASK) >> 16) + 2;
	reg = (hdr & R300_CP_PACKET0_REG_MASK) << 2;

	if (hdr & (1 << 15)) {
		if (reg == 0x2208) {
			return 0;
		}
	}

	while (count_dw < num_dw) {
		/* need to have something like the r300 validation here - 
		   list of allowed registers */
		int flags;

		ret = r300_check_range(reg, 1);
		switch(ret) {
		case -1:
			DRM_ERROR("Illegal register %x\n", reg);
			break;
		case 0:
			break;
		case 1:
			flags = r300_get_reg_flags(reg);
			if (flags == MARK_CHECK_OFFSET) {
				if (num_dw > 2) {
					DRM_ERROR("Cannot relocate inside type stream of reg0 packets\n");
					return -EINVAL;
				}

				ret = radeon_cs_relocate_packet0(parser, offset_dw);
				if (ret)
					return ret;
				DRM_DEBUG("need to relocate %x %d\n", reg, flags);
				/* okay it should be followed by a NOP */
			} else if (flags == MARK_CHECK_SCISSOR) {
				DRM_DEBUG("need to validate scissor %x %d\n", reg, flags);
			} else {
				
				DRM_ERROR("illegal register 0x%x %d at %d\n", reg, flags, offset_dw);
				return -EINVAL;
			}
			break;
		}
		count_dw++;
		reg += 4;
	}
	return 0;
}

int radeon_cs_parse(struct drm_radeon_cs_parser *parser)
{
	volatile int rb;
	struct drm_radeon_kernel_chunk *ib_chunk;
	/* scan the packet for various things */
	int count_dw = 0, size_dw;
	int ret = 0;

	ib_chunk = &parser->chunks[parser->ib_index];
	size_dw = ib_chunk->length_dw;

	while (count_dw < size_dw && ret == 0) {
		int hdr = ib_chunk->kdata[count_dw];
		int num_dw = (hdr & RADEON_CP_PACKET_COUNT_MASK) >> 16;
		int reg;

		switch (hdr & RADEON_CP_PACKET_MASK) {
		case RADEON_CP_PACKET0:
			ret = radeon_cs_packet0(parser, count_dw);
			if (ret)
				return ret;
			break;
		case RADEON_CP_PACKET1:
		case RADEON_CP_PACKET2:
			reg = hdr & RADEON_CP_PACKET0_REG_MASK;
			DRM_DEBUG("Packet 1/2: %d  %x\n", num_dw, reg);
			break;

		case RADEON_CP_PACKET3:
			reg = hdr & 0xff00;
			
			switch(reg) {
			case RADEON_3D_LOAD_VBPNTR:
			case RADEON_CP_INDX_BUFFER:
			case RADEON_CNTL_HOSTDATA_BLT:
				ret =radeon_cs_relocate_packet3(parser,
						count_dw);
				if (ret)
					return ret;
				break;

			case RADEON_CNTL_BITBLT_MULTI:
				DRM_ERROR("need relocate packet 3 for %x %d\n", reg, count_dw);
				break;

			case RADEON_3D_DRAW_IMMD:	/* triggers drawing using in-packet vertex data */
			case RADEON_CP_3D_DRAW_IMMD_2:	/* triggers drawing using in-packet vertex data */
			case RADEON_CP_3D_DRAW_VBUF_2:	/* triggers drawing of vertex buffers setup elsewhere */
			case RADEON_CP_3D_DRAW_INDX_2:	/* triggers drawing using indices to vertex buffer */
			case RADEON_WAIT_FOR_IDLE:
			case RADEON_CP_NOP:
				break;
			default:
				DRM_ERROR("unknown packet 3 %x at %d\n", reg, count_dw);
				ret = -EINVAL;
			}
			break;
		}
		count_dw += num_dw+2;
	}

	if (ret)
		return ret;
	     

	/* copy the packet into the IB */
	memcpy(parser->ib, ib_chunk->kdata, ib_chunk->length_dw * sizeof(uint32_t));

	/* read back last byte to flush WC buffers */
	rb = readl((parser->ib + (ib_chunk->length_dw-1) * sizeof(uint32_t)));

	return 0;
}

uint32_t radeon_cs_id_get(struct drm_radeon_private *radeon)
{
	/* FIXME: protect with a spinlock */
	/* FIXME: check if wrap affect last reported wrap & sequence */
	radeon->cs.id_scnt = (radeon->cs.id_scnt + 1) & 0x00FFFFFF;
	if (!radeon->cs.id_scnt) {
		/* increment wrap counter */
		radeon->cs.id_wcnt += 0x01000000;
		/* valid sequence counter start at 1 */
		radeon->cs.id_scnt = 1;
	}
	return (radeon->cs.id_scnt | radeon->cs.id_wcnt);
}

void r100_cs_id_emit(struct drm_radeon_cs_parser *parser, uint32_t *id)
{
	drm_radeon_private_t *dev_priv = parser->dev->dev_private;
	RING_LOCALS;

	dev_priv->irq_emitted = radeon_update_breadcrumb(parser->dev);
	/* ISYNC_CNTL should have CPSCRACTH bit set */
	*id = radeon_cs_id_get(dev_priv);
	/* emit id in SCRATCH4 (not used yet in old drm) */
	BEGIN_RING(10);
	OUT_RING(CP_PACKET0(RADEON_CP_IB_BASE, 1));
	OUT_RING(parser->card_offset);
	OUT_RING(parser->chunks[parser->ib_index].length_dw);
	OUT_RING(CP_PACKET2());
	OUT_RING(CP_PACKET0(RADEON_SCRATCH_REG4, 0));
	OUT_RING(*id);
	OUT_RING_REG(RADEON_LAST_SWI_REG, dev_priv->irq_emitted);
	OUT_RING_REG(RADEON_GEN_INT_STATUS, RADEON_SW_INT_FIRE);
	ADVANCE_RING();	
	COMMIT_RING();

}

void r300_cs_id_emit(struct drm_radeon_cs_parser *parser, uint32_t *id)
{
	drm_radeon_private_t *dev_priv = parser->dev->dev_private;
	int i;
	RING_LOCALS;

	dev_priv->irq_emitted = radeon_update_breadcrumb(parser->dev);

	/* ISYNC_CNTL should not have CPSCRACTH bit set */
	*id = radeon_cs_id_get(dev_priv);

	/* emit id in SCRATCH6 */
	BEGIN_RING(16);
	OUT_RING(CP_PACKET0(RADEON_CP_IB_BASE, 1));
	OUT_RING(parser->card_offset);
	OUT_RING(parser->chunks[parser->ib_index].length_dw);
	OUT_RING(CP_PACKET0(R300_RB3D_DSTCACHE_CTLSTAT, 0));
	OUT_RING(0);
	for (i = 0; i < 11; i++) /* emit fillers like fglrx */
		OUT_RING(CP_PACKET2());
	ADVANCE_RING();
	COMMIT_RING();

	BEGIN_RING(16);
	OUT_RING_REG(R300_RB3D_DSTCACHE_CTLSTAT, R300_RB3D_DC_FLUSH);
	OUT_RING(CP_PACKET0(R300_CP_RESYNC_ADDR, 1));
	OUT_RING(6);
	OUT_RING(*id);
	OUT_RING_REG(R300_RB3D_DSTCACHE_CTLSTAT, R300_RB3D_DC_FINISH|R300_RB3D_DC_FLUSH);
	/* emit inline breadcrumb for TTM fencing */
#if 1
	RADEON_WAIT_UNTIL_3D_IDLE();
	OUT_RING_REG(RADEON_LAST_SWI_REG, dev_priv->irq_emitted);
#else
	OUT_RING(CP_PACKET0(R300_CP_RESYNC_ADDR, 1));
	OUT_RING(3); /* breadcrumb register */
	OUT_RING(dev_priv->irq_emitted);
	OUT_RING(CP_PACKET2());
#endif
	OUT_RING_REG(RADEON_GEN_INT_STATUS, RADEON_SW_INT_FIRE);
	OUT_RING(CP_PACKET2());
	OUT_RING(CP_PACKET2());
	OUT_RING(CP_PACKET2());
	ADVANCE_RING();	
	COMMIT_RING();

}

uint32_t r100_cs_id_last_get(struct drm_device *dev)
{
	drm_radeon_private_t *dev_priv = dev->dev_private;

	return GET_SCRATCH(4);
}

uint32_t r300_cs_id_last_get(struct drm_device *dev)
{
	drm_radeon_private_t *dev_priv = dev->dev_private;

	return GET_SCRATCH(6);
}

int radeon_cs_init(struct drm_device *dev)
{
	drm_radeon_private_t *dev_priv = dev->dev_private;

	if (dev_priv->chip_family < CHIP_RV280) {
		dev_priv->cs.id_emit = r100_cs_id_emit;
		dev_priv->cs.id_last_get = r100_cs_id_last_get;
	} else if (dev_priv->chip_family < CHIP_R600) {
		dev_priv->cs.id_emit = r300_cs_id_emit;
		dev_priv->cs.id_last_get = r300_cs_id_last_get;
	}

	dev_priv->cs.parse = radeon_cs_parse;
	/* ib get depends on memory manager or not so memory manager */
	dev_priv->cs.relocate = radeon_nomm_relocate;
	return 0;
}