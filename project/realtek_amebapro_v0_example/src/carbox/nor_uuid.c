#include "nor_uuid.h"
#include "nor_otp.h"
#include <stdio.h>
#include <string.h>

#include "hal_flash.h"
#include "hal_spic.h"
#include "rtl8195bhp_spic_type.h"

extern hal_spic_adaptor_t *pglob_spic_adaptor;
extern void flash_resource_lock(void);
extern void flash_resource_unlock(void);

/* Bounded polling, independent of RTOS ticks (interrupts are locked). */
#define NOR_UUID_POLL_LIMIT 1000000UL

typedef struct {
#if CARBOX_NOR_UUID_DIAG
    unsigned count, sequence;
    unsigned mode, cmd_ch, flash_id;
    struct {
        unsigned sequence, phase, opcode, address, length, received;
        uint32_t sr, ssienr, txflr, rxflr;
        uint32_t ctrlr0, ctrlr1, ctrlr2, addr_length;
        uint32_t baudr, fbaudr, auto_length, valid_cmd;
    } failure[2]; /* Original timeout AND a possible QPI-restore timeout. */
#else
    unsigned unused;
#endif
} nor_uuid_diag_t;

/* RAM-only capture, BEFORE disabling/flushing destroys the failure state.
 * Do not read DR or clear-on-read interrupt registers here. */
static void nor_uuid_capture(nor_uuid_diag_t *diag, SPIC_Type *dev,
                             unsigned phase, unsigned opcode, unsigned address,
                             unsigned length, unsigned received)
{
#if CARBOX_NOR_UUID_DIAG
    unsigned n;
    if (!diag || diag->count >= 2U)
        return;
    n = diag->count++;
    diag->failure[n].sequence = diag->sequence;
    diag->failure[n].phase = phase;
    diag->failure[n].opcode = opcode;
    diag->failure[n].address = address;
    diag->failure[n].length = length;
    diag->failure[n].received = received;
    diag->failure[n].sr = dev->sr;
    diag->failure[n].ssienr = dev->ssienr;
    diag->failure[n].txflr = dev->txflr;
    diag->failure[n].rxflr = dev->rxflr;
    diag->failure[n].ctrlr0 = dev->ctrlr0;
    diag->failure[n].ctrlr1 = dev->ctrlr1;
    diag->failure[n].ctrlr2 = dev->ctrlr2;
    diag->failure[n].addr_length = dev->addr_length;
    diag->failure[n].baudr = dev->baudr;
    diag->failure[n].fbaudr = dev->fbaudr;
    diag->failure[n].auto_length = dev->auto_length;
    diag->failure[n].valid_cmd = dev->valid_cmd;
#else
    (void)diag; (void)dev; (void)phase; (void)opcode; (void)address;
    (void)length; (void)received;
#endif
}

#ifndef NOR_UUID_LOG
#define NOR_UUID_LOG printf
#endif

/* Called only after restoration and flash_resource_unlock(). The strings and
 * logger may live in flash. Restoration after a hardware fault is best-effort. */
static void nor_uuid_report(const nor_uuid_diag_t *diag, int ret)
{
#if CARBOX_NOR_UUID_DIAG
    unsigned n;
    if (ret >= 0)
        return;
    NOR_UUID_LOG("[nor-uuid] ret=%d id=%06x mode=%u cmd_ch=%u seq=%u timeouts=%u poll_limit=%u\n",
                 ret, diag->flash_id, diag->mode, diag->cmd_ch,
                 diag->sequence, diag->count, (unsigned)NOR_UUID_POLL_LIMIT);
    for (n = 0; n < diag->count; ++n) {
        NOR_UUID_LOG("[nor-uuid] seq=%u phase=%s op=%02x addr=%06x rx=%u/%u SR=%08x SSIENR=%08x TXFLR=%u RXFLR=%u\n",
            diag->failure[n].sequence,
            diag->failure[n].phase == 1 ? "pre-busy" :
            (diag->failure[n].phase == 2 ? "rx-wait" : "complete-wait"),
            diag->failure[n].opcode, diag->failure[n].address,
            diag->failure[n].received, diag->failure[n].length,
            (unsigned)diag->failure[n].sr, (unsigned)diag->failure[n].ssienr,
            (unsigned)diag->failure[n].txflr, (unsigned)diag->failure[n].rxflr);
        NOR_UUID_LOG("[nor-uuid] seq=%u CTRLR0=%08x CTRLR1=%08x CTRLR2=%08x ADDR_LENGTH=%08x BAUDR=%08x FBAUDR=%08x AUTO_LENGTH=%08x VALID_CMD=%08x\n",
            diag->failure[n].sequence,
            (unsigned)diag->failure[n].ctrlr0, (unsigned)diag->failure[n].ctrlr1,
            (unsigned)diag->failure[n].ctrlr2, (unsigned)diag->failure[n].addr_length,
            (unsigned)diag->failure[n].baudr, (unsigned)diag->failure[n].fbaudr,
            (unsigned)diag->failure[n].auto_length, (unsigned)diag->failure[n].valid_cmd);
    }
#else
    (void)diag; (void)ret;
#endif
}

/* The whole file is in SRAM_C. Never log or access flash-resident data while
 * the controller/flash is in the temporary single-I/O configuration.
 * EN25S64A Rev.H, pp. 64/70: 5A + A23..A0 + 8 dummy clocks + data.
 * The dummy byte is deliberately sent as a fourth controller address byte;
 * it is NOT a fourth address byte as interpreted by the flash. The OTP reader
 * below uses the same SRAM transport with three address bytes and NO dummy.
 */
static int nor_uuid_transfer_diag(SPIC_Type *dev, uint8_t channels, uint8_t opcode,
                             unsigned address_bytes, uint32_t address,
                             uint8_t *data, unsigned length, nor_uuid_diag_t *diag)
{
    spic_ctrlr0_t ctrl;
    unsigned i;
    uint32_t remaining = NOR_UUID_POLL_LIMIT;
    unsigned phase = 2;

#if CARBOX_NOR_UUID_DIAG
    if (diag)
        ++diag->sequence;
#endif

    spic_disable_rtl8195bhp(dev);
    dev->flush_fifo = 1;
    ctrl.w = dev->ctrlr0;
    ctrl.b.tmod = length ? RxMode : TxMode;
    ctrl.b.cmd_ch = channels;
    ctrl.b.addr_ch = channels;
    ctrl.b.data_ch = channels;
    ctrl.b.cmd_ddr_en = 0;
    ctrl.b.addr_ddr_en = 0;
    ctrl.b.data_ddr_en = 0;
    ctrl.b.fast_rd = 0; /* BAUDR command clock, not the fast/XIP clock. */
    dev->ctrlr0 = ctrl.w;
    dev->ctrlr1 = length;
    dev->addr_length = address_bytes == 3U ? ThreeBytesLength : FourBytesLength;
    dev->dr_byte = opcode;
    if (address_bytes) {
        dev->dr_byte = (uint8_t)(address >> 16);
        dev->dr_byte = (uint8_t)(address >> 8);
        dev->dr_byte = (uint8_t)address;
        if (address_bytes == 4U)
            dev->dr_byte = 0; /* 8 dummy clocks under the SAME chip select. */
    }
    spic_enable_rtl8195bhp(dev);

    for (i = 0; i < length; ++i) {
        while (!dev->sr_b.rfne) {
            if (--remaining == 0)
                goto timeout;
        }
        data[i] = dev->dr_byte;
    }
    /* SPIC clears its enable bit when the user transaction completes. */
    phase = 3;
    while (dev->ssienr_b.spic_en || dev->sr_b.busy) {
        if (--remaining == 0)
            goto timeout;
    }
    return 0;

timeout:
    nor_uuid_capture(diag, dev, phase, opcode, address, length, i);
    spic_disable_rtl8195bhp(dev);
    dev->flush_fifo = 1;
    return CARBOX_NOR_UUID_TIMEOUT;
}

/* OTP shares the transport, but UUID diagnostics do not change its logging. */
static int nor_uuid_transfer(SPIC_Type *dev, uint8_t channels, uint8_t opcode,
                             unsigned address_bytes, uint32_t address,
                             uint8_t *data, unsigned length)
{
    return nor_uuid_transfer_diag(dev, channels, opcode, address_bytes, address,
                                  data, length, NULL);
}

int carbox_nor_read_uuid(uint8_t *uuid, size_t capacity)
{
    hal_spic_adaptor_t *adaptor;
    SPIC_Type *dev;
    uint32_t ctrlr0, ctrlr1, ctrlr2, addr_length, valid_cmd, ssienr;
    uint32_t remaining;
    uint8_t first[CARBOX_NOR_UUID_SIZE], second[CARBOX_NOR_UUID_SIZE];
    uint8_t check[4];
    unsigned i;
    int ret, qpi, restore_qpi = 0;
    uint8_t any_nonzero = 0, any_nonff = 0;
    nor_uuid_diag_t diag;

    memset(&diag, 0, sizeof(diag));

    if (!uuid || capacity < CARBOX_NOR_UUID_SIZE) {
        nor_uuid_report(&diag, CARBOX_NOR_UUID_INVALID_ARGUMENT);
        return CARBOX_NOR_UUID_INVALID_ARGUMENT;
    }

    /* Do not initialize/reconfigure the boot flash pinmux here. */
    flash_resource_lock();
    adaptor = pglob_spic_adaptor;
    if (!adaptor || !adaptor->spic_dev) {
        ret = CARBOX_NOR_UUID_NOT_READY;
        goto unlock;
    }
    qpi = adaptor->spic_bit_mode == SpicQpiMode;
#if CARBOX_NOR_UUID_DIAG
    diag.mode = adaptor->spic_bit_mode;
    diag.cmd_ch = adaptor->spic_send_cmd_mode;
    diag.flash_id = (unsigned)adaptor->flash_id[0] << 16 |
                   (unsigned)adaptor->flash_id[1] << 8 | adaptor->flash_id[2];
#endif
    if (adaptor->flash_type != FLASH_TYPE_EON ||
        adaptor->flash_id[0] != 0x1c || adaptor->flash_id[1] != 0x38 ||
        adaptor->flash_id[2] != 0x17 || adaptor->dtr_en ||
        (!qpi && adaptor->spic_bit_mode > SpicQuadIOMode) ||
        adaptor->spic_send_cmd_mode != (qpi ? QuadChnl : SingleChnl)) {
        ret = CARBOX_NOR_UUID_UNSUPPORTED;
        goto unlock;
    }
    dev = adaptor->spic_dev;
    remaining = NOR_UUID_POLL_LIMIT;
    while (dev->sr_b.busy) {
        if (--remaining == 0) {
            nor_uuid_capture(&diag, dev, 1, 0, 0, 0, 0);
            ret = CARBOX_NOR_UUID_TIMEOUT;
            goto unlock;
        }
    }

    ctrlr0 = dev->ctrlr0;
    ctrlr1 = dev->ctrlr1;
    ctrlr2 = dev->ctrlr2;
    addr_length = dev->addr_length;
    valid_cmd = dev->valid_cmd;
    ssienr = dev->ssienr;
    spic_disable_rtl8195bhp(dev);
    dev->valid_cmd_b.prm_en = 0;
    dev->ctrlr2_b.seq_en = 0;
    __DSB();

    /* FF exits enhanced-read mode, or QPI when sent on four channels.
     * No reset, write-enable, OTP entry or status-register write is needed.
     */
    restore_qpi = qpi;
    ret = nor_uuid_transfer_diag(dev, qpi ? QuadChnl : SingleChnl,
                            0xff, 0, 0, NULL, 0, &diag);
    if (ret < 0)
        goto restore;
    ret = nor_uuid_transfer_diag(dev, SingleChnl, 0x9f, 0, 0, check, 3, &diag);
    if (ret < 0)
        goto restore;
    if (check[0] != 0x1c || check[1] != 0x38 || check[2] != 0x17) {
        ret = CARBOX_NOR_UUID_INVALID_DATA;
        goto restore;
    }
    /* Verify framing against the known SFDP signature before trusting UID. */
    ret = nor_uuid_transfer_diag(dev, SingleChnl, 0x5a, 4, 0, check, 4, &diag);
    if (ret < 0)
        goto restore;
    if (check[0] != 'S' || check[1] != 'F' ||
        check[2] != 'D' || check[3] != 'P') {
        ret = CARBOX_NOR_UUID_INVALID_DATA;
        goto restore;
    }
    ret = nor_uuid_transfer_diag(dev, SingleChnl, 0x5a, 4, 0x80,
                            first, CARBOX_NOR_UUID_SIZE, &diag);
    if (ret < 0)
        goto restore;
    ret = nor_uuid_transfer_diag(dev, SingleChnl, 0x5a, 4, 0x80,
                            second, CARBOX_NOR_UUID_SIZE, &diag);
    if (ret < 0)
        goto restore;
    for (i = 0; i < CARBOX_NOR_UUID_SIZE; ++i) {
        if (first[i] != second[i]) {
            ret = CARBOX_NOR_UUID_INVALID_DATA;
            goto restore;
        }
        any_nonzero |= first[i];
        any_nonff |= (uint8_t)~first[i];
    }
    ret = (any_nonzero && any_nonff) ? (int)CARBOX_NOR_UUID_SIZE :
          CARBOX_NOR_UUID_INVALID_DATA;

restore:
    if (restore_qpi) {
        /* 38 enters QPI without changing the original dummy-cycle setting. */
        if (nor_uuid_transfer_diag(dev, SingleChnl, 0x38, 0, 0, NULL, 0, &diag) < 0)
            ret = CARBOX_NOR_UUID_TIMEOUT;
    }
    spic_disable_rtl8195bhp(dev);
    dev->flush_fifo = 1;
    dev->ctrlr0 = ctrlr0;
    dev->ctrlr1 = ctrlr1;
    dev->ctrlr2 = ctrlr2;
    dev->addr_length = addr_length;
    dev->valid_cmd = valid_cmd;
    dev->ssienr = ssienr;
    __DSB();
    __ISB();
    if (ret == (int)CARBOX_NOR_UUID_SIZE) {
        for (i = 0; i < CARBOX_NOR_UUID_SIZE; ++i)
            uuid[i] = first[i];
    }
unlock:
    flash_resource_unlock();
    nor_uuid_report(&diag, ret);
    return ret;
}

int carplay_nor_read_otp(uint32_t offset, void *buffer, size_t length)
{
    hal_spic_adaptor_t *adaptor;
    SPIC_Type *dev;
    uint32_t ctrlr0, ctrlr1, ctrlr2, addr_length, valid_cmd, ssienr;
    uint32_t baudr;
    uint32_t remaining;
    uint8_t staging[CARPLAY_NOR_OTP_SIZE];
    uint8_t check[3];
    uint8_t *out = (uint8_t *)buffer;
    size_t done, chunk;
    int ret, qpi, otp_attempted = 0;

    /* Subtraction after validating offset avoids offset+length overflow. */
    if (offset > CARPLAY_NOR_OTP_SIZE ||
        length > CARPLAY_NOR_OTP_SIZE - offset || (!buffer && length))
        return CARPLAY_NOR_OTP_INVALID_ARGUMENT;
    if (!length)
        return 0;

    flash_resource_lock();
    adaptor = pglob_spic_adaptor;
    if (!adaptor || !adaptor->spic_dev) {
        ret = CARPLAY_NOR_OTP_NOT_READY;
        goto unlock;
    }
    qpi = adaptor->spic_bit_mode == SpicQpiMode;
    if (adaptor->flash_type != FLASH_TYPE_EON ||
        adaptor->flash_id[0] != 0x1c || adaptor->flash_id[1] != 0x38 ||
        adaptor->flash_id[2] != 0x17 || adaptor->dtr_en ||
        (!qpi && adaptor->spic_bit_mode > SpicQuadIOMode) ||
        adaptor->spic_send_cmd_mode != (qpi ? QuadChnl : SingleChnl)) {
        ret = CARPLAY_NOR_OTP_UNSUPPORTED;
        goto unlock;
    }
    dev = adaptor->spic_dev;
    remaining = NOR_UUID_POLL_LIMIT;
    while (dev->sr_b.busy) {
        if (--remaining == 0) {
            ret = CARPLAY_NOR_OTP_TIMEOUT;
            goto unlock;
        }
    }
    ctrlr0 = dev->ctrlr0;
    ctrlr1 = dev->ctrlr1;
    ctrlr2 = dev->ctrlr2;
    addr_length = dev->addr_length;
    valid_cmd = dev->valid_cmd;
    ssienr = dev->ssienr;
    baudr = dev->baudr;
    spic_disable_rtl8195bhp(dev);
    /* Use the SDK's conservative divider for READ/03, independently of any
     * fast-read overclock. Preserve the original divider for normal traffic. */
    dev->baudr_b.sckdv = MAX_BAUD_RATE;
    dev->valid_cmd_b.prm_en = 0;
    dev->ctrlr2_b.seq_en = 0;
    __DSB();

    ret = nor_uuid_transfer(dev, qpi ? QuadChnl : SingleChnl,
                            0xff, 0, 0, NULL, 0);
    if (ret < 0)
        goto restore;
    ret = nor_uuid_transfer(dev, SingleChnl, 0x05, 0, 0, check, 1);
    if (ret < 0)
        goto restore;
    /* Do not enter OTP during program/erase, or consume another operation's
     * write-enable latch with the WRDI used to leave OTP. Caller can retry. */
    if (check[0] & 0x03U) {
        ret = CARPLAY_NOR_OTP_BUSY;
        goto restore;
    }
    ret = nor_uuid_transfer(dev, SingleChnl, 0x9f, 0, 0, check, 3);
    if (ret < 0)
        goto restore;
    if (check[0] != 0x1c || check[1] != 0x38 || check[2] != 0x17) {
        ret = CARPLAY_NOR_OTP_INVALID_DATA;
        goto restore;
    }

    /* EN25S64A Rev.H p.62: 3A maps user OTP at 7FF000..7FF1FF;
     * 04 (WRDI) exits it. Unlike UID/5A, normal READ/03 has no dummy byte.
     * Mark the attempt BEFORE submission so a timeout still tries WRDI. */
    otp_attempted = 1;
    ret = nor_uuid_transfer(dev, SingleChnl, 0x3a, 0, 0, NULL, 0);
    if (ret < 0)
        goto restore;
    for (done = 0; done < length; done += chunk) {
        chunk = length - done;
        if (chunk > 16U)
            chunk = 16U; /* Fit a complete response in the hardware FIFO. */
        ret = nor_uuid_transfer(dev, SingleChnl, 0x03, 3,
                                0x7ff000U + offset + (uint32_t)done,
                                staging + done, (unsigned)chunk);
        if (ret < 0)
            goto restore;
    }
    ret = (int)length;

restore:
    /* Always leave OTP before restoring QPI/XIP, even after a partial read. */
    if (otp_attempted &&
        nor_uuid_transfer(dev, SingleChnl, 0x04, 0, 0, NULL, 0) < 0)
        ret = CARPLAY_NOR_OTP_TIMEOUT;
    if (qpi && nor_uuid_transfer(dev, SingleChnl, 0x38, 0, 0, NULL, 0) < 0)
        ret = CARPLAY_NOR_OTP_TIMEOUT;
    spic_disable_rtl8195bhp(dev);
    dev->flush_fifo = 1;
    dev->ctrlr0 = ctrlr0;
    dev->ctrlr1 = ctrlr1;
    dev->ctrlr2 = ctrlr2;
    dev->addr_length = addr_length;
    dev->valid_cmd = valid_cmd;
    dev->baudr = baudr;
    dev->ssienr = ssienr;
    __DSB();
    __ISB();
    if (ret == (int)length) {
        for (done = 0; done < length; ++done)
            out[done] = staging[done];
    }
unlock:
    flash_resource_unlock();
    return ret;
}
