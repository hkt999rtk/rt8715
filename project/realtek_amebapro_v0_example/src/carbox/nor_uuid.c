#include "nor_uuid.h"
#include "nor_otp.h"
#include <stdio.h>
#include <string.h>

#include "hal_flash.h"
#include "hal_cache.h"
#include "hal_spic.h"
#include "rtl8195bhp_spic_type.h"

extern hal_spic_adaptor_t *pglob_spic_adaptor;
/* Used only before tasks/peripherals start. Preserve the incoming IRQ/cache
 * state instead of using the SDK RTOS tick-compensating flash resource lock. */
typedef struct {
    uint32_t primask, cache;
} nor_boot_guard;

static void nor_boot_lock(nor_boot_guard *guard)
{
    guard->primask = __get_PRIMASK();
    __disable_irq();
    guard->cache = SCB->CCR;
    __DSB();
    if (guard->cache & SCB_CCR_DC_Msk) {
        dcache_clean();
        dcache_disable();
    }
    if (guard->cache & SCB_CCR_IC_Msk)
        icache_disable();
    __DSB();
    __ISB();
}

static void nor_boot_unlock(const nor_boot_guard *guard)
{
    __DSB();
    if (guard->cache & SCB_CCR_DC_Msk)
        dcache_enable();
    if (guard->cache & SCB_CCR_IC_Msk) {
        icache_invalidate();
        icache_enable();
    }
    __DSB();
    __ISB();
    __set_PRIMASK(guard->primask);
}

/* Bounded polling, independent of RTOS ticks (interrupts are locked). */
#define NOR_UUID_POLL_LIMIT 1000000UL

typedef struct {
#if CARBOX_NOR_UUID_DIAG
    unsigned count, sequence;
    unsigned mode, cmd_ch, flash_id, flash_type, dtr;
    unsigned otp, stage, offset, requested;
    unsigned observed_id, sfdp, status, valid_id, valid_sfdp, valid_status;
    unsigned mismatch, original_valid, restore_attempted;
    int operation_ret, cleanup_ret;
    uint32_t original_ctrlr0, original_baudr, original_fbaudr, original_auto;
    uint32_t original_valid_cmd, original_ctrlr2;
    unsigned timing_valid, xip_dummy_clocks, wire_dummy_bus, rx_delay_bus;
    unsigned launch_count;
    struct {
        unsigned sequence, opcode;
        uint32_t ser, loaded_txflr, loaded_ssienr;
        uint32_t enabled_sr, enabled_txflr, enabled_ssienr;
    } launch[8]; /* First commands only; enough for mode entry/ID/first read. */
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

enum {
    NOR_STAGE_ARGUMENT, NOR_STAGE_ADAPTOR, NOR_STAGE_SUPPORT,
    NOR_STAGE_PRE_BUSY, NOR_STAGE_EXIT_QPI, NOR_STAGE_STATUS,
    NOR_STAGE_JEDEC, NOR_STAGE_SFDP, NOR_STAGE_UID_FIRST,
    NOR_STAGE_UID_SECOND, NOR_STAGE_UID_COMPARE, NOR_STAGE_UID_CONTENT,
    NOR_STAGE_OTP_ENTER, NOR_STAGE_OTP_READ, NOR_STAGE_DONE, NOR_STAGE_RX_TIMING
};
#if CARBOX_NOR_UUID_DIAG
#define NOR_DIAG(d, field, value) ((d).field = (value))
#else
#define NOR_DIAG(d, field, value) ((void)0)
#endif

/* Metadata only: no flash traffic, heap or logging while the resource is locked. */
static void nor_diag_adaptor(nor_uuid_diag_t *diag, hal_spic_adaptor_t *adaptor)
{
#if CARBOX_NOR_UUID_DIAG
    SPIC_Type *dev = adaptor->spic_dev;
    diag->mode = adaptor->spic_bit_mode;
    diag->cmd_ch = adaptor->spic_send_cmd_mode;
    diag->flash_type = adaptor->flash_type;
    diag->dtr = adaptor->dtr_en;
    diag->flash_id = (unsigned)adaptor->flash_id[0] << 16 |
                     (unsigned)adaptor->flash_id[1] << 8 | adaptor->flash_id[2];
    diag->original_ctrlr0 = dev->ctrlr0;
    diag->original_ctrlr2 = dev->ctrlr2;
    diag->original_baudr = dev->baudr;
    diag->original_fbaudr = dev->fbaudr;
    diag->original_auto = dev->auto_length;
    diag->original_valid_cmd = dev->valid_cmd;
    diag->original_valid = 1;
#else
    (void)diag; (void)adaptor;
#endif
}

/* SDK spic_calibration() uses protocol_dummy * baud * 2 + path delay.
 * Our 9F/05/03 reads have no protocol dummy. 5A's eight dummy clocks are
 * already emitted as the fourth address byte, so it also needs only the
 * calibrated path delay here. Read metadata before changing flash mode.
 */
static int nor_uuid_rx_delay(hal_spic_adaptor_t *adaptor, unsigned *delay,
                            nor_uuid_diag_t *diag)
{
    SPIC_Type *dev = adaptor->spic_dev;
    unsigned total = dev->auto_length_b.rd_dummy_length;
    unsigned baud = dev->fbaudr_b.fsckdv;
    unsigned dummy, wire;
    if (!adaptor->dummy_cycle || !baud)
        return CARBOX_NOR_UUID_INVALID_DATA;
    dummy = ((volatile const uint8_t *)adaptor->dummy_cycle)[adaptor->spic_bit_mode];
    wire = dummy * baud * 2U;
    /* STR SDK calibration searches residual delays in [2, MAX_AUTO_LENGTH).
     * Reject inconsistent metadata instead of guessing a sample point. */
    if (total < wire + 2U || total - wire >= MAX_AUTO_LENGTH)
        return CARBOX_NOR_UUID_INVALID_DATA;
    *delay = total - wire;
#if CARBOX_NOR_UUID_DIAG
    diag->timing_valid = 1;
    diag->xip_dummy_clocks = dummy;
    diag->wire_dummy_bus = wire;
    diag->rx_delay_bus = *delay;
#else
    (void)diag;
#endif
    return 0;
}

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

/* Called only after restoration and nor_boot_unlock(). The strings and
 * logger may live in flash. Restoration after a hardware fault is best-effort. */
static void nor_uuid_report(const nor_uuid_diag_t *diag, int ret)
{
#if CARBOX_NOR_UUID_DIAG
    unsigned n;
    const char *tag = diag->otp ? "nor-otp" : "nor-uuid";
    const char *stages[] = {"argument", "adaptor", "support", "pre-busy",
        "exit-qpi", "status", "jedec", "sfdp", "uid-first", "uid-second",
        "uid-compare", "uid-content", "otp-enter", "otp-read", "done", "rx-timing"};
    if (ret >= 0)
        return;
    if (diag->timing_valid)
        NOR_UUID_LOG("[%s] timing xip_dummy_clocks=%u subtract_bus=%u rx_delay_bus=%u command_baud=%u ret=%d\n",
            tag, diag->xip_dummy_clocks, diag->wire_dummy_bus,
            diag->rx_delay_bus, (unsigned)MAX_BAUD_RATE, ret);
    NOR_UUID_LOG("[%s] ret=%d id=%06x mode=%u cmd_ch=%u seq=%u timeouts=%u poll_limit=%u\n",
                 tag, ret, diag->flash_id, diag->mode, diag->cmd_ch,
                 diag->sequence, diag->count, (unsigned)NOR_UUID_POLL_LIMIT);
    NOR_UUID_LOG("[%s] stage=%s type=%u dtr=%u offset=%u requested=%u operation_ret=%d cleanup_ret=%d restore_attempted=%u mismatch_index=%u\n",
        tag, stages[diag->stage], diag->flash_type, diag->dtr, diag->offset,
        diag->requested, diag->restore_attempted ? diag->operation_ret : ret,
        diag->cleanup_ret, diag->restore_attempted, diag->mismatch);
    NOR_UUID_LOG("[%s] observed_jedec=%06x sfdp=%08x status=%02x valid_id/sfdp/status=%u/%u/%u\n",
        tag, diag->observed_id, diag->sfdp, diag->status,
        diag->valid_id, diag->valid_sfdp, diag->valid_status);
    if (diag->original_valid)
        NOR_UUID_LOG("[%s] entry CTRLR0=%08x CTRLR2=%08x BAUDR=%08x FBAUDR=%08x AUTO_LENGTH=%08x VALID_CMD=%08x\n",
            tag, (unsigned)diag->original_ctrlr0, (unsigned)diag->original_ctrlr2,
            (unsigned)diag->original_baudr, (unsigned)diag->original_fbaudr,
            (unsigned)diag->original_auto, (unsigned)diag->original_valid_cmd);
    for (n = 0; n < diag->launch_count; ++n) {
        NOR_UUID_LOG("[%s] launch seq=%u op=%02x SER=%08x loaded_tx=%u loaded_en=%08x enabled_SR=%08x enabled_tx=%u enabled_en=%08x\n",
            tag, diag->launch[n].sequence, diag->launch[n].opcode,
            (unsigned)diag->launch[n].ser, (unsigned)diag->launch[n].loaded_txflr,
            (unsigned)diag->launch[n].loaded_ssienr,
            (unsigned)diag->launch[n].enabled_sr,
            (unsigned)diag->launch[n].enabled_txflr,
            (unsigned)diag->launch[n].enabled_ssienr);
    }
    for (n = 0; n < diag->count; ++n) {
        NOR_UUID_LOG("[%s] seq=%u phase=%s op=%02x addr=%06x rx=%u/%u SR=%08x SSIENR=%08x TXFLR=%u RXFLR=%u\n",
            tag, diag->failure[n].sequence,
            diag->failure[n].phase == 1 ? "pre-busy" :
            (diag->failure[n].phase == 2 ? "rx-wait" : "complete-wait"),
            diag->failure[n].opcode, diag->failure[n].address,
            diag->failure[n].received, diag->failure[n].length,
            (unsigned)diag->failure[n].sr, (unsigned)diag->failure[n].ssienr,
            (unsigned)diag->failure[n].txflr, (unsigned)diag->failure[n].rxflr);
        NOR_UUID_LOG("[%s] seq=%u CTRLR0=%08x CTRLR1=%08x CTRLR2=%08x ADDR_LENGTH=%08x BAUDR=%08x FBAUDR=%08x AUTO_LENGTH=%08x VALID_CMD=%08x\n",
            tag, diag->failure[n].sequence,
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
    unsigned launch_index = 8U;
#endif

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
    __DSB();
#if CARBOX_NOR_UUID_DIAG
    if (diag && diag->launch_count < 8U) {
        launch_index = diag->launch_count++;
        diag->launch[launch_index].sequence = diag->sequence;
        diag->launch[launch_index].opcode = opcode;
        diag->launch[launch_index].ser = dev->ser;
        diag->launch[launch_index].loaded_txflr = dev->txflr;
        diag->launch[launch_index].loaded_ssienr = dev->ssienr;
    }
#endif
    spic_enable_rtl8195bhp(dev);
    __DSB(); /* Complete FIFO/configuration/enable writes before polling. */
#if CARBOX_NOR_UUID_DIAG
    if (launch_index < 8U) {
        diag->launch[launch_index].enabled_sr = dev->sr;
        diag->launch[launch_index].enabled_txflr = dev->txflr;
        diag->launch[launch_index].enabled_ssienr = dev->ssienr;
    }
#endif

    for (i = 0; i < length; ++i) {
        while (!dev->sr_b.rfne) {
            if (--remaining == 0)
                goto timeout;
        }
        data[i] = dev->dr_byte;
    }
    phase = 3;
    /* This controller leaves SSIENR set after both TX and complete RX:
     * board traces show 9F rx=3/3 and 05 rx=1/1 with SR=0x06, SSIENR=1.
     * RX must receive every requested byte above; then drain TX and wait
     * for the serial shifter before explicitly disabling the controller.
     * A cleared enable bit is not required, nor is idle alone enough for RX.
     */
    while (!dev->sr_b.tfe || dev->sr_b.busy) {
        if (--remaining == 0)
            goto timeout;
    }
    spic_disable_rtl8195bhp(dev);
    __DSB();
    return 0;

timeout:
    nor_uuid_capture(diag, dev, phase, opcode, address, length, i);
    spic_disable_rtl8195bhp(dev);
    dev->flush_fifo = 1;
    return CARBOX_NOR_UUID_TIMEOUT;
}

static int nor_read_uuid_hardware(uint8_t *uuid, size_t capacity)
{
    hal_spic_adaptor_t *adaptor;
    SPIC_Type *dev;
    uint32_t ctrlr0, ctrlr1, ctrlr2, addr_length, valid_cmd, ssienr;
    uint32_t baudr, auto_length;
    uint32_t remaining;
    uint8_t first[CARBOX_NOR_UUID_SIZE], second[CARBOX_NOR_UUID_SIZE];
    uint8_t check[4];
    unsigned i, rx_delay;
    int ret, qpi, restore_qpi = 0;
    uint8_t any_nonzero = 0, any_nonff = 0;
    nor_uuid_diag_t diag;
    nor_boot_guard guard;

    memset(&diag, 0, sizeof(diag));
    NOR_DIAG(diag, requested, capacity);
    NOR_DIAG(diag, mismatch, ~0U);

    if (!uuid || capacity < CARBOX_NOR_UUID_SIZE) {
        nor_uuid_report(&diag, CARBOX_NOR_UUID_INVALID_ARGUMENT);
        return CARBOX_NOR_UUID_INVALID_ARGUMENT;
    }

    /* Do not initialize/reconfigure the boot flash pinmux here. */
    nor_boot_lock(&guard);
    NOR_DIAG(diag, stage, NOR_STAGE_ADAPTOR);
    adaptor = pglob_spic_adaptor;
    if (!adaptor || !adaptor->spic_dev) {
        ret = CARBOX_NOR_UUID_NOT_READY;
        goto unlock;
    }
    qpi = adaptor->spic_bit_mode == SpicQpiMode;
    nor_diag_adaptor(&diag, adaptor);
    NOR_DIAG(diag, stage, NOR_STAGE_SUPPORT);
    if (adaptor->flash_type != FLASH_TYPE_EON ||
        adaptor->flash_id[0] != 0x1c || adaptor->flash_id[1] != 0x38 ||
        adaptor->flash_id[2] != 0x17 || adaptor->dtr_en ||
        (!qpi && adaptor->spic_bit_mode > SpicQuadIOMode) ||
        adaptor->spic_send_cmd_mode != (qpi ? QuadChnl : SingleChnl)) {
        ret = CARBOX_NOR_UUID_UNSUPPORTED;
        goto unlock;
    }
    dev = adaptor->spic_dev;
    NOR_DIAG(diag, stage, NOR_STAGE_PRE_BUSY);
    remaining = NOR_UUID_POLL_LIMIT;
    while (dev->sr_b.busy) {
        if (--remaining == 0) {
            nor_uuid_capture(&diag, dev, 1, 0, 0, 0, 0);
            ret = CARBOX_NOR_UUID_TIMEOUT;
            goto unlock;
        }
    }

    NOR_DIAG(diag, stage, NOR_STAGE_RX_TIMING);
    ret = nor_uuid_rx_delay(adaptor, &rx_delay, &diag);
    if (ret < 0)
        goto unlock;
    ctrlr0 = dev->ctrlr0;
    ctrlr1 = dev->ctrlr1;
    ctrlr2 = dev->ctrlr2;
    addr_length = dev->addr_length;
    valid_cmd = dev->valid_cmd;
    ssienr = dev->ssienr;
    baudr = dev->baudr;
    auto_length = dev->auto_length;
    spic_disable_rtl8195bhp(dev);
    dev->baudr_b.sckdv = MAX_BAUD_RATE;
    dev->auto_length_b.rd_dummy_length = rx_delay;
    dev->valid_cmd_b.prm_en = 0;
    dev->ctrlr2_b.seq_en = 0;
    __DSB();

    /* FF exits enhanced-read mode, or QPI when sent on four channels.
     * No reset, write-enable, OTP entry or status-register write is needed.
     */
    restore_qpi = qpi;
    NOR_DIAG(diag, stage, NOR_STAGE_EXIT_QPI);
    ret = nor_uuid_transfer_diag(dev, qpi ? QuadChnl : SingleChnl,
                            0xff, 0, 0, NULL, 0, &diag);
    if (ret < 0)
        goto restore;
    NOR_DIAG(diag, stage, NOR_STAGE_JEDEC);
    ret = nor_uuid_transfer_diag(dev, SingleChnl, 0x9f, 0, 0, check, 3, &diag);
    if (ret < 0)
        goto restore;
    NOR_DIAG(diag, observed_id, (unsigned)check[0] << 16 | (unsigned)check[1] << 8 | check[2]);
    NOR_DIAG(diag, valid_id, 1);
    if (check[0] != 0x1c || check[1] != 0x38 || check[2] != 0x17) {
        ret = CARBOX_NOR_UUID_INVALID_DATA;
        goto restore;
    }
    /* Verify framing against the known SFDP signature before trusting UID. */
    NOR_DIAG(diag, stage, NOR_STAGE_SFDP);
    ret = nor_uuid_transfer_diag(dev, SingleChnl, 0x5a, 4, 0, check, 4, &diag);
    if (ret < 0)
        goto restore;
    NOR_DIAG(diag, sfdp, (unsigned)check[0] << 24 | (unsigned)check[1] << 16 |
                         (unsigned)check[2] << 8 | check[3]);
    NOR_DIAG(diag, valid_sfdp, 1);
    if (check[0] != 'S' || check[1] != 'F' ||
        check[2] != 'D' || check[3] != 'P') {
        ret = CARBOX_NOR_UUID_INVALID_DATA;
        goto restore;
    }
    NOR_DIAG(diag, stage, NOR_STAGE_UID_FIRST);
    ret = nor_uuid_transfer_diag(dev, SingleChnl, 0x5a, 4, 0x80,
                            first, CARBOX_NOR_UUID_SIZE, &diag);
    if (ret < 0)
        goto restore;
    NOR_DIAG(diag, stage, NOR_STAGE_UID_SECOND);
    ret = nor_uuid_transfer_diag(dev, SingleChnl, 0x5a, 4, 0x80,
                            second, CARBOX_NOR_UUID_SIZE, &diag);
    if (ret < 0)
        goto restore;
    NOR_DIAG(diag, stage, NOR_STAGE_UID_COMPARE);
    for (i = 0; i < CARBOX_NOR_UUID_SIZE; ++i) {
        if (first[i] != second[i]) {
            NOR_DIAG(diag, mismatch, i);
            ret = CARBOX_NOR_UUID_INVALID_DATA;
            goto restore;
        }
        any_nonzero |= first[i];
        any_nonff |= (uint8_t)~first[i];
    }
    NOR_DIAG(diag, stage, NOR_STAGE_UID_CONTENT);
    ret = (any_nonzero && any_nonff) ? (int)CARBOX_NOR_UUID_SIZE :
          CARBOX_NOR_UUID_INVALID_DATA;

restore:
    NOR_DIAG(diag, operation_ret, ret);
    NOR_DIAG(diag, restore_attempted, 1);
    if (restore_qpi) {
        /* 38 enters QPI without changing the original dummy-cycle setting. */
        if (nor_uuid_transfer_diag(dev, SingleChnl, 0x38, 0, 0, NULL, 0, &diag) < 0) {
            NOR_DIAG(diag, cleanup_ret, CARBOX_NOR_UUID_TIMEOUT);
            ret = CARBOX_NOR_UUID_TIMEOUT;
        }
    }
    spic_disable_rtl8195bhp(dev);
    dev->flush_fifo = 1;
    dev->ctrlr0 = ctrlr0;
    dev->ctrlr1 = ctrlr1;
    dev->ctrlr2 = ctrlr2;
    dev->addr_length = addr_length;
    dev->valid_cmd = valid_cmd;
    dev->auto_length = auto_length;
    dev->baudr = baudr;
    dev->ssienr = ssienr;
    __DSB();
    __ISB();
    if (ret == (int)CARBOX_NOR_UUID_SIZE) {
        for (i = 0; i < CARBOX_NOR_UUID_SIZE; ++i)
            uuid[i] = first[i];
    }
unlock:
    nor_boot_unlock(&guard);
    nor_uuid_report(&diag, ret);
    return ret;
}

static int nor_read_otp_hardware(uint32_t offset, void *buffer, size_t length)
{
    hal_spic_adaptor_t *adaptor;
    SPIC_Type *dev;
    uint32_t ctrlr0, ctrlr1, ctrlr2, addr_length, valid_cmd, ssienr;
    uint32_t baudr, auto_length;
    unsigned rx_delay;
    uint32_t remaining;
    uint8_t staging[CARPLAY_NOR_OTP_SIZE];
    uint8_t check[3];
    uint8_t *out = (uint8_t *)buffer;
    size_t done, chunk;
    int ret, qpi, otp_attempted = 0;
    nor_uuid_diag_t diag;
    nor_boot_guard guard;
    memset(&diag, 0, sizeof(diag));
    NOR_DIAG(diag, otp, 1);
    NOR_DIAG(diag, offset, offset);
    NOR_DIAG(diag, requested, length);
    NOR_DIAG(diag, mismatch, ~0U);

    /* Subtraction after validating offset avoids offset+length overflow. */
    if (offset > CARPLAY_NOR_OTP_SIZE ||
        length > CARPLAY_NOR_OTP_SIZE - offset || (!buffer && length)) {
        nor_uuid_report(&diag, CARPLAY_NOR_OTP_INVALID_ARGUMENT);
        return CARPLAY_NOR_OTP_INVALID_ARGUMENT;
    }
    if (!length)
        return 0;

    nor_boot_lock(&guard);
    NOR_DIAG(diag, stage, NOR_STAGE_ADAPTOR);
    adaptor = pglob_spic_adaptor;
    if (!adaptor || !adaptor->spic_dev) {
        ret = CARPLAY_NOR_OTP_NOT_READY;
        goto unlock;
    }
    qpi = adaptor->spic_bit_mode == SpicQpiMode;
    nor_diag_adaptor(&diag, adaptor);
    NOR_DIAG(diag, stage, NOR_STAGE_SUPPORT);
    if (adaptor->flash_type != FLASH_TYPE_EON ||
        adaptor->flash_id[0] != 0x1c || adaptor->flash_id[1] != 0x38 ||
        adaptor->flash_id[2] != 0x17 || adaptor->dtr_en ||
        (!qpi && adaptor->spic_bit_mode > SpicQuadIOMode) ||
        adaptor->spic_send_cmd_mode != (qpi ? QuadChnl : SingleChnl)) {
        ret = CARPLAY_NOR_OTP_UNSUPPORTED;
        goto unlock;
    }
    dev = adaptor->spic_dev;
    NOR_DIAG(diag, stage, NOR_STAGE_PRE_BUSY);
    remaining = NOR_UUID_POLL_LIMIT;
    while (dev->sr_b.busy) {
        if (--remaining == 0) {
            nor_uuid_capture(&diag, dev, 1, 0, 0, 0, 0);
            ret = CARPLAY_NOR_OTP_TIMEOUT;
            goto unlock;
        }
    }
    NOR_DIAG(diag, stage, NOR_STAGE_RX_TIMING);
    ret = nor_uuid_rx_delay(adaptor, &rx_delay, &diag);
    if (ret < 0)
        goto unlock;
    ctrlr0 = dev->ctrlr0;
    ctrlr1 = dev->ctrlr1;
    ctrlr2 = dev->ctrlr2;
    addr_length = dev->addr_length;
    valid_cmd = dev->valid_cmd;
    ssienr = dev->ssienr;
    baudr = dev->baudr;
    auto_length = dev->auto_length;
    spic_disable_rtl8195bhp(dev);
    /* Use the SDK's conservative divider for READ/03, independently of any
     * fast-read overclock. Preserve the original divider for normal traffic. */
    dev->baudr_b.sckdv = MAX_BAUD_RATE;
    dev->auto_length_b.rd_dummy_length = rx_delay;
    dev->valid_cmd_b.prm_en = 0;
    dev->ctrlr2_b.seq_en = 0;
    __DSB();

    NOR_DIAG(diag, stage, NOR_STAGE_EXIT_QPI);
    ret = nor_uuid_transfer_diag(dev, qpi ? QuadChnl : SingleChnl,
                            0xff, 0, 0, NULL, 0, &diag);
    if (ret < 0)
        goto restore;
    NOR_DIAG(diag, stage, NOR_STAGE_STATUS);
    ret = nor_uuid_transfer_diag(dev, SingleChnl, 0x05, 0, 0, check, 1, &diag);
    if (ret < 0)
        goto restore;
    /* Do not enter OTP during program/erase, or consume another operation's
     * write-enable latch with the WRDI used to leave OTP. Caller can retry. */
    NOR_DIAG(diag, status, check[0]);
    NOR_DIAG(diag, valid_status, 1);
    if (check[0] & 0x03U) {
        ret = CARPLAY_NOR_OTP_BUSY;
        goto restore;
    }
    NOR_DIAG(diag, stage, NOR_STAGE_JEDEC);
    ret = nor_uuid_transfer_diag(dev, SingleChnl, 0x9f, 0, 0, check, 3, &diag);
    if (ret < 0)
        goto restore;
    NOR_DIAG(diag, observed_id, (unsigned)check[0] << 16 | (unsigned)check[1] << 8 | check[2]);
    NOR_DIAG(diag, valid_id, 1);
    if (check[0] != 0x1c || check[1] != 0x38 || check[2] != 0x17) {
        ret = CARPLAY_NOR_OTP_INVALID_DATA;
        goto restore;
    }

    /* EN25S64A Rev.H p.62: 3A maps user OTP at 7FF000..7FF1FF;
     * 04 (WRDI) exits it. Unlike UID/5A, normal READ/03 has no dummy byte.
     * Mark the attempt BEFORE submission so a timeout still tries WRDI. */
    NOR_DIAG(diag, stage, NOR_STAGE_OTP_ENTER);
    otp_attempted = 1;
    ret = nor_uuid_transfer_diag(dev, SingleChnl, 0x3a, 0, 0, NULL, 0, &diag);
    if (ret < 0)
        goto restore;
    NOR_DIAG(diag, stage, NOR_STAGE_OTP_READ);
    for (done = 0; done < length; done += chunk) {
        chunk = length - done;
        if (chunk > 16U)
            chunk = 16U; /* Fit a complete response in the hardware FIFO. */
        ret = nor_uuid_transfer_diag(dev, SingleChnl, 0x03, 3,
                                0x7ff000U + offset + (uint32_t)done,
                                staging + done, (unsigned)chunk, &diag);
        if (ret < 0)
            goto restore;
    }
    NOR_DIAG(diag, stage, NOR_STAGE_DONE);
    ret = (int)length;

restore:
    NOR_DIAG(diag, operation_ret, ret);
    NOR_DIAG(diag, restore_attempted, 1);
    /* Always leave OTP before restoring QPI/XIP, even after a partial read. */
    if (otp_attempted &&
        nor_uuid_transfer_diag(dev, SingleChnl, 0x04, 0, 0, NULL, 0, &diag) < 0) {
        NOR_DIAG(diag, cleanup_ret, CARPLAY_NOR_OTP_TIMEOUT);
        ret = CARPLAY_NOR_OTP_TIMEOUT;
    }
    if (qpi && nor_uuid_transfer_diag(dev, SingleChnl, 0x38, 0, 0, NULL, 0, &diag) < 0) {
        NOR_DIAG(diag, cleanup_ret, CARPLAY_NOR_OTP_TIMEOUT);
        ret = CARPLAY_NOR_OTP_TIMEOUT;
    }
    spic_disable_rtl8195bhp(dev);
    dev->flush_fifo = 1;
    dev->ctrlr0 = ctrlr0;
    dev->ctrlr1 = ctrlr1;
    dev->ctrlr2 = ctrlr2;
    dev->addr_length = addr_length;
    dev->valid_cmd = valid_cmd;
    dev->baudr = baudr;
    dev->auto_length = auto_length;
    dev->ssienr = ssienr;
    __DSB();
    __ISB();
    if (ret == (int)length) {
        for (done = 0; done < length; ++done)
            out[done] = staging[done];
    }
unlock:
    nor_boot_unlock(&guard);
    nor_uuid_report(&diag, ret);
    return ret;
}

/* Written once before scheduler start, then immutable. Publish only after both
 * attempts finish. Failed reads retain their error and are never retried by a
 * customer API call; partially filled hardware staging never becomes valid. */
static uint8_t nor_cached_uuid[CARBOX_NOR_UUID_SIZE];
static uint8_t nor_cached_otp[CARPLAY_NOR_OTP_SIZE];
static int nor_cached_uuid_ret = CARBOX_NOR_UUID_NOT_READY;
static int nor_cached_otp_ret = CARPLAY_NOR_OTP_NOT_READY;
static unsigned nor_cache_initialized;

void carbox_nor_identity_cache_init(void)
{
    if (__atomic_load_n(&nor_cache_initialized, __ATOMIC_ACQUIRE))
        return;
    nor_cached_uuid_ret = nor_read_uuid_hardware(nor_cached_uuid,
                                                sizeof(nor_cached_uuid));
    nor_cached_otp_ret = nor_read_otp_hardware(0, nor_cached_otp,
                                              sizeof(nor_cached_otp));
    __atomic_store_n(&nor_cache_initialized, 1U, __ATOMIC_RELEASE);
}

void carbox_nor_identity_cache_profile_report(uint32_t sequence)
{
    unsigned initialized = __atomic_load_n(&nor_cache_initialized,
                                           __ATOMIC_ACQUIRE);
    int uuid_ret = initialized ? nor_cached_uuid_ret :
                                 CARBOX_NOR_UUID_NOT_READY;
    int otp_ret = initialized ? nor_cached_otp_ret :
                                CARPLAY_NOR_OTP_NOT_READY;

    /* This observes the immutable early-boot cache only. Do not replace these
     * values with the public copy-out APIs: the profile must remain allocation-
     * free and must never reissue a flash transaction. */
    NOR_UUID_LOG("[PCPROF][%lu][NORCACHE] initialized=%u "
                 "uuid_ret=%d uuid_ok=%u otp_ret=%d otp_ok=%u "
                 "source=boot-cache flash_access=0\r\n",
                 (unsigned long)sequence, initialized, uuid_ret,
                 uuid_ret == (int)CARBOX_NOR_UUID_SIZE, otp_ret,
                 otp_ret == (int)CARPLAY_NOR_OTP_SIZE);
}

int carbox_nor_read_uuid(uint8_t *uuid, size_t capacity)
{
    if (!uuid || capacity < CARBOX_NOR_UUID_SIZE)
        return CARBOX_NOR_UUID_INVALID_ARGUMENT;
    if (!__atomic_load_n(&nor_cache_initialized, __ATOMIC_ACQUIRE))
        return CARBOX_NOR_UUID_NOT_READY;
    if (nor_cached_uuid_ret != (int)CARBOX_NOR_UUID_SIZE)
        return nor_cached_uuid_ret;
    memcpy(uuid, nor_cached_uuid, CARBOX_NOR_UUID_SIZE);
    return CARBOX_NOR_UUID_SIZE;
}

int carplay_nor_read_otp(uint32_t offset, void *buffer, size_t length)
{
    if (offset > CARPLAY_NOR_OTP_SIZE ||
        length > CARPLAY_NOR_OTP_SIZE - offset || (!buffer && length))
        return CARPLAY_NOR_OTP_INVALID_ARGUMENT;
    if (!length)
        return 0;
    if (!__atomic_load_n(&nor_cache_initialized, __ATOMIC_ACQUIRE))
        return CARPLAY_NOR_OTP_NOT_READY;
    if (nor_cached_otp_ret != (int)CARPLAY_NOR_OTP_SIZE)
        return nor_cached_otp_ret;
    memcpy(buffer, nor_cached_otp + offset, length);
    return (int)length;
}

/* The customer archive's two legacy entry points discard the read error.
 * Linker wrapping replaces both layers without changing the vendor archive.
 * Preserve their fixed 16-byte read and success == 1 ABI, but propagate errors.
 * Do not invoke __real_*: those functions would turn a failed read into success.
 */
int __wrap_spinor_read_otp(uint32_t offset, void *buffer)
{
    int ret = carplay_nor_read_otp(offset, buffer, 16);
    return ret < 0 ? ret : 1;
}

int __wrap_CarApi_GetFlashOTP(uint32_t offset, void *buffer)
{
    return __wrap_spinor_read_otp(offset, buffer);
}
