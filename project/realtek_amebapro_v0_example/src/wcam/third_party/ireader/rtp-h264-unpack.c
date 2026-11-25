// RFC6184 RTP Payload Format for H.264 Video

#include "rtp-packet.h"
#include "rtp-payload-internal.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#define H264_NAL(v)	((v) & 0x1F)
#define FU_START(v) ((v) & 0x80)
#define FU_END(v)	((v) & 0x40)
#define FU_NAL(v)	((v) & 0x1F)

struct rtp_decode_h264_t
{
	struct rtp_payload_t handler;
	void* cbparam;

	uint16_t seq; // rtp seq
	uint32_t timestamp;

	uint8_t* ptr;
	int size, capacity;

	int flags;
};

static void* rtp_h264_unpack_create(struct rtp_payload_t *handler, void* param)
{
	struct rtp_decode_h264_t *unpacker;
	unpacker = (struct rtp_decode_h264_t *)calloc(1, sizeof(*unpacker));
	if(!unpacker)
		return NULL;

	memcpy(&unpacker->handler, handler, sizeof(unpacker->handler));
	unpacker->cbparam = param;
	unpacker->flags = -1;
	return unpacker;
}

static void rtp_h264_unpack_destroy(void* p)
{
	struct rtp_decode_h264_t *unpacker;
	unpacker = (struct rtp_decode_h264_t *)p;

	/* AU storage belongs to caller. */
#if defined(_DEBUG) || defined(DEBUG)
	memset(unpacker, 0xCC, sizeof(*unpacker));
#endif
	free(unpacker);
}

// 5.7.1. Single-Time Aggregation Packet (STAP) (p23)
/*
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           RTP Header                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|STAP-B NAL HDR |            DON                |  NALU 1 Size  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| NALU 1 Size   | NALU 1 HDR    |         NALU 1 Data           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               +
:                                                               :
+               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               | NALU 2 Size                   |   NALU 2 HDR  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                            NALU 2 Data                        |
:                                                               :
|                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                               :    ...OPTIONAL RTP padding    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
static int rtp_h264_unpack_stap(struct rtp_decode_h264_t *unpacker, const uint8_t* ptr, int bytes, uint32_t timestamp, int stap_b)
{
	int r, n;
	uint16_t len;
	uint16_t don;

	r = 0;
	n = stap_b ? 3 : 1;
    if (bytes < n)
    {
        assert(0);
        return -EINVAL; // error
    }
	don = stap_b ? nbo_r16(ptr + 1) : 0;
	ptr += n; // STAP-A / STAP-B HDR + DON

	for(bytes -= n; 0 == r && bytes >= 2; bytes -= len + 2)
	{
		len = nbo_r16(ptr);
		if(len + 2 > bytes || len < 1 || (ptr[2] & 0x80) ||
           !H264_NAL(ptr[2]) || H264_NAL(ptr[2]) >= 24)
		{
			assert(0);
			unpacker->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
			unpacker->size = 0;
			return -EINVAL; // error
		}

		assert(H264_NAL(ptr[2]) > 0 && H264_NAL(ptr[2]) < 24);
		r = unpacker->handler.packet(unpacker->cbparam, ptr + 2, len, timestamp, unpacker->flags);
		unpacker->flags = 0;
		unpacker->size = 0;

		ptr += len + 2; // next NALU
		don = (don + 1) % 65536;
	}

	return r ? r : (bytes == 0 ? 1 : -EINVAL); // complete aggregate
}

// 5.8. Fragmentation Units (FUs) (p29)
/*
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  FU indicator |   FU header   |              DON              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-|
|                                                               |
|                          FU payload                           |
|                                                               |
|                               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                               :   ...OPTIONAL RTP padding     |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
static int rtp_h264_unpack_fu(struct rtp_decode_h264_t *unpacker, const uint8_t* ptr, int bytes, uint32_t timestamp, int fu_b)
{
	int r, n;
	uint8_t fuheader;
	//uint16_t don;

	r = 0;
	n = fu_b ? 4 : 2;
	if (bytes <= n || (ptr[1] & 0x20) || (ptr[1] & 0xc0) == 0xc0 ||
        !(ptr[1] & 31) || (ptr[1] & 31) >= 24 ||
        unpacker->size + bytes - n > RTP_PAYLOAD_MAX_SIZE)
	{
		assert(0);
		return -EINVAL; // error
	}

	if (unpacker->size + bytes - n + 1 > unpacker->capacity) {
        unpacker->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
        unpacker->size = 0;
        return -ENOMEM;
    }

	fuheader = ptr[1];
	//don = nbo_r16(ptr + 2);
	if (FU_START(fuheader))
	{
#if 0
		if (unpacker->size > 0)
		{
			unpacker->flags |= RTP_PAYLOAD_FLAG_PACKET_CORRUPT;
			unpacker->handler.packet(unpacker->cbparam, unpacker->ptr, unpacker->size, unpacker->timestamp, unpacker->flags);
			unpacker->flags = 0;
			unpacker->size = 0; // reset
		}
#endif

		unpacker->size = 1; // NAL unit type byte
		unpacker->ptr[0] = (ptr[0]/*indicator*/ & 0xE0) | (fuheader & 0x1F);
		assert(H264_NAL(unpacker->ptr[0]) > 0 && H264_NAL(unpacker->ptr[0]) < 24);
	}
	else
	{
		if (0 == unpacker->size)
		{
			unpacker->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
			return 0; // packet discard
		}
		assert(unpacker->size > 0);
	}

	unpacker->timestamp = timestamp;
	if (bytes > n)
	{
		assert(unpacker->capacity >= unpacker->size + bytes - n);
		memmove(unpacker->ptr + unpacker->size, ptr + n, bytes - n);
		unpacker->size += bytes - n;
	}

	if(FU_END(fuheader))
	{
        if(unpacker->size > 0)
            r = unpacker->handler.packet(unpacker->cbparam, unpacker->ptr, unpacker->size, timestamp, unpacker->flags);
		unpacker->flags = 0;
		unpacker->size = 0; // reset
	}

	return 0 == r ? 1 : r; // packet handled
}

static int rtp_h264_unpack_input(void* p, const void* packet, int bytes)
{
	int r;
	uint8_t nalt;
	struct rtp_packet_t pkt;
	struct rtp_decode_h264_t *unpacker;

	unpacker = (struct rtp_decode_h264_t *)p;
	if(!unpacker || 0 != rtp_packet_deserialize(&pkt, packet, bytes) || pkt.payloadlen < 1 ||
        (((const uint8_t*)pkt.payload)[0] & 0x80))
		return -EINVAL;
	
	if (-1 == unpacker->flags)
	{
		unpacker->flags = 0;
		unpacker->seq = (uint16_t)(pkt.rtp.seq - 1); // disable packet lost
	}

	if ((uint16_t)pkt.rtp.seq != (uint16_t)(unpacker->seq + 1))
	{
		unpacker->flags = RTP_PAYLOAD_FLAG_PACKET_LOST;
		unpacker->size = 0; // discard previous packets
	}
	unpacker->seq = (uint16_t)pkt.rtp.seq;

	nalt = ((unsigned char *)pkt.payload)[0];
	switch(nalt & 0x1F)
	{
	case 0: // reserved
	case 31: // reserved
		assert(0);
		return 0; // packet discard

	case 24: // STAP-A
		return rtp_h264_unpack_stap(unpacker, (const uint8_t*)pkt.payload, pkt.payloadlen, pkt.rtp.timestamp, 0);
	case 28: // FU-A
		return rtp_h264_unpack_fu(unpacker, (const uint8_t*)pkt.payload, pkt.payloadlen, pkt.rtp.timestamp, 0);
	case 25: case 26: case 27: case 29: case 30:
        return -EINVAL;

	default: // 1-23 NAL unit
		r = unpacker->handler.packet(unpacker->cbparam, (const uint8_t*)pkt.payload, pkt.payloadlen, pkt.rtp.timestamp, unpacker->flags);
		unpacker->flags = 0;
		unpacker->size = 0;
		return 0 == r ? 1 : r; // packet handled
	}
}

struct rtp_payload_decode_t *rtp_h264_decode()
{
	static struct rtp_payload_decode_t unpacker = {
		rtp_h264_unpack_create,
		rtp_h264_unpack_destroy,
		rtp_h264_unpack_input,
	};

	return &unpacker;
}

void wcam_ir_h264_rx_buffer(void *p, uint8_t *buffer, int capacity, int reset)
{
    struct rtp_decode_h264_t *u = p;
    u->ptr = buffer; u->capacity = capacity;
    if (reset) { u->size = 0; u->flags = -1; }
}

int wcam_ir_h264_rx_pending(void *p)
{
    return ((struct rtp_decode_h264_t*)p)->size;
}
