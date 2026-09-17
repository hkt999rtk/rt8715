/* Host protocol/control-flow tests, NOT an SPIC timing or silicon emulator. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cstdarg>
#include "hal_spic.h"
#include "../../nor_uuid.h"
#if CARBOX_NOR_UUID_DIAG
static int test_log(const char *, ...);
#endif
#define NOR_UUID_LOG test_log
#include "../../nor_uuid.c"

static SPIC_Type device;
static hal_spic_adaptor_t adapter;
hal_spic_adaptor_t *pglob_spic_adaptor = &adapter;
static std::vector<uint8_t> tx, rx;
static unsigned read_pos, transactions;
static int lock_depth, timeout_at, bad_at;
static bool in_qpi, original_qpi;
static bool in_otp;
static uint8_t status_register;
static int otp_fill, fail_read_byte;
static unsigned otp_reads, otp_exits, otp_enters;
static std::string logs;
static int fifo_pending_at;
static bool restore_timeout;
static int busy_at;
static bool enable_stays_set;
static uint8_t dummy_cycles[10];
static uint32_t expected_auto_length;
static unsigned expected_rx_delay;

#if CARBOX_NOR_UUID_DIAG
static int test_log(const char *format, ...) {
    assert(lock_depth == 0); // Never call logging while flash is locked.
    assert(device.ctrlr0 == 0x00400300 && device.ctrlr1 == 17);
    assert(device.ctrlr2 == 9 && device.addr_length == 3);
    assert(device.valid_cmd == 0x1234 && device.ssienr == 0);
    assert(device.auto_length == expected_auto_length && device.baudr == 3);
    char line[1024];
    va_list args;
    va_start(args, format);
    int size = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    assert(size >= 0 && (unsigned)size < sizeof(line));
    logs += line;
    return size;
}
#endif

NorMockSCB nor_mock_scb;
static uint32_t primask;
static unsigned guard_entries;
uint32_t __get_PRIMASK() { return primask; }
void __disable_irq() { assert(lock_depth++ == 0); primask = 1; ++guard_entries; }
void __set_PRIMASK(uint32_t value) { assert(--lock_depth == 0); primask = value; }
void dcache_clean() { assert(lock_depth == 1 && primask == 1); }
void dcache_disable() { nor_mock_scb.CCR &= ~SCB_CCR_DC_Msk; }
void icache_disable() { nor_mock_scb.CCR &= ~SCB_CCR_IC_Msk; }
void dcache_enable() { nor_mock_scb.CCR |= SCB_CCR_DC_Msk; }
void icache_invalidate() { assert(lock_depth == 1); }
void icache_enable() { nor_mock_scb.CCR |= SCB_CCR_IC_Msk; }
void MockFifo::operator=(uint8_t value) {
    tx.push_back(value);
    device.txflr = tx.size();
}
MockFifo::operator uint8_t() {
    assert(read_pos < rx.size());
    uint8_t value = rx[read_pos++];
    device.sr_b.rfne = read_pos < rx.size() &&
        !((int)read_pos == fail_read_byte && !tx.empty() &&
          (tx[0] == 0x03 || (tx[0] == 0x5a && tx[3] == 0x80)));
    return value;
}
void spic_disable_rtl8195bhp(SPIC_Type *dev) {
    dev->ssienr = 0;
    dev->sr = 0;
    dev->txflr = dev->rxflr = 0;
    tx.clear();
}
void spic_enable_rtl8195bhp(SPIC_Type *dev) {
    assert(lock_depth == 1 && primask == 1);
    assert(!(nor_mock_scb.CCR & (SCB_CCR_IC_Msk | SCB_CCR_DC_Msk)));
    assert(!tx.empty());
    ++transactions;
    assert(dev->ctrlr0_b.cmd_ch == (in_qpi ? QuadChnl : SingleChnl));
    assert(dev->ctrlr0_b.addr_ch == dev->ctrlr0_b.cmd_ch);
    assert(dev->ctrlr0_b.data_ch == dev->ctrlr0_b.cmd_ch);
    assert(!dev->ctrlr0_b.fast_rd && !dev->valid_cmd_b.prm_en);
    assert(!dev->ctrlr2_b.seq_en);
    assert(dev->auto_length_b.rd_dummy_length == expected_rx_delay);
    assert(dev->baudr_b.sckdv == MAX_BAUD_RATE);
    rx.clear();
    read_pos = 0;
    if (tx[0] == 0xff || tx[0] == 0x38) {
        assert(tx.size() == 1 && dev->ctrlr1 == 0);
        assert(dev->ctrlr0_b.tmod == TxMode);
        if (tx[0] == 0x38) assert(!in_otp);
        in_qpi = tx[0] == 0x38;
    } else if (tx[0] == 0x3a || tx[0] == 0x04) {
        assert(tx.size() == 1 && dev->ctrlr1 == 0);
        assert(dev->ctrlr0_b.tmod == TxMode);
        assert(!in_qpi);
        in_otp = tx[0] == 0x3a;
        if (in_otp) ++otp_enters; else ++otp_exits;
    } else {
        assert(dev->ctrlr0_b.tmod == RxMode);
        if (tx[0] == 0x05) {
            assert(tx.size() == 1 && dev->ctrlr1 == 1);
            rx = {status_register};
        } else if (tx[0] == 0x03) {
            assert(in_otp && !in_qpi);
            assert(tx.size() == 4 && dev->addr_length == ThreeBytesLength);
            assert(dev->baudr_b.sckdv == MAX_BAUD_RATE);
            uint32_t address = (uint32_t)tx[1] << 16 |
                               (uint32_t)tx[2] << 8 | tx[3];
            assert(address >= 0x7ff000 && address < 0x7ff200);
            assert(dev->ctrlr1 > 0 && dev->ctrlr1 <= 16);
            assert(address + dev->ctrlr1 <= 0x7ff200);
            for (unsigned i = 0; i < dev->ctrlr1; ++i)
                rx.push_back(otp_fill < 0 ? (uint8_t)(address + i) : otp_fill);
            ++otp_reads;
        } else if (tx[0] == 0x9f) {
            assert(tx.size() == 1 && dev->ctrlr1 == 3);
            rx = {0x1c, 0x38, 0x17};
        } else {
            assert(tx[0] == 0x5a && tx.size() == 5);
            assert(tx[1] == 0 && tx[2] == 0 && tx[4] == 0);
            assert(dev->addr_length == FourBytesLength);
            if (tx[3] == 0) {
                assert(dev->ctrlr1 == 4);
                rx = {'S', 'F', 'D', 'P'};
            } else {
                assert(tx[3] == 0x80 && dev->ctrlr1 == 12);
                for (unsigned i = 0; i < 12; ++i)
                    rx.push_back(0x10 + i);
            }
        }
    }
    if ((int)transactions == bad_at && !rx.empty())
        rx[0] ^= 0x80;
    bool timeout = (int)transactions == timeout_at ||
                   (restore_timeout && tx[0] == 0x38);
    dev->ssienr = timeout || enable_stays_set;
    dev->sr = 0;
    dev->sr_b.tfnf = 1;
    dev->sr_b.tfe = !timeout && (int)transactions != fifo_pending_at;
    dev->sr_b.busy = (int)transactions == busy_at;
    dev->sr_b.rfne = !rx.empty() && !timeout;
    dev->txflr = timeout || (int)transactions == fifo_pending_at ? (unsigned)tx.size() : 0;
    dev->rxflr = timeout ? 0 : (unsigned)rx.size();
}

static void setup(bool qpi) {
    device = {};
    device.ctrlr0 = 0x00400300;
    device.ctrlr1 = 17;
    device.ctrlr2 = 9;
    device.addr_length = 3;
    device.valid_cmd = 0x1234;
    device.ssienr = 0;
    device.ser = 1;
    device.baudr = 3;
    device.fbaudr = 2;
    expected_auto_length = device.auto_length = 0x58030017;
    expected_rx_delay = 7;
    memset(dummy_cycles, 4, sizeof(dummy_cycles));
    adapter = {&device, FLASH_TYPE_EON, {0x1c, 0x38, 0x17}, 0,
               (uint8_t)(qpi ? SpicQpiMode : SpicQuadIOMode),
               (uint8_t)(qpi ? QuadChnl : SingleChnl), dummy_cycles};
    pglob_spic_adaptor = &adapter;
    in_qpi = original_qpi = qpi;
    transactions = 0;
    timeout_at = bad_at = -1;
    lock_depth = 0;
    guard_entries = 0;
    primask = 0;
    nor_mock_scb.CCR = SCB_CCR_IC_Msk | SCB_CCR_DC_Msk;
    in_otp = false;
    status_register = 0;
    otp_fill = fail_read_byte = -1;
    otp_reads = otp_enters = otp_exits = 0;
    fifo_pending_at = -1;
    restore_timeout = false;
    busy_at = -1;
    enable_stays_set = true;
    logs.clear();
}

static void run_otp(uint32_t offset, size_t length, int expected) {
    uint8_t output[514];
    memset(output, 0xa5, sizeof(output));
    SPIC_Type before = device;
    assert(nor_read_otp_hardware(offset, output + 1, length) == expected);
    assert(lock_depth == 0 && in_qpi == original_qpi && !in_otp);
    assert(device.ctrlr0 == before.ctrlr0 && device.ctrlr1 == before.ctrlr1);
    assert(device.ctrlr2 == before.ctrlr2 && device.addr_length == before.addr_length);
    assert(device.valid_cmd == before.valid_cmd && device.ssienr == before.ssienr);
    assert(device.baudr == before.baudr);
    assert(device.auto_length == before.auto_length && device.fbaudr == before.fbaudr);
    assert(output[0] == 0xa5);
    for (unsigned i = 0; i < 513; ++i) {
        uint8_t wanted = expected > 0 && i < length ?
            (otp_fill < 0 ? (uint8_t)(offset + i) : otp_fill) : 0xa5;
        assert(output[i + 1] == wanted);
    }
}

static void run(int expected) {
    uint8_t output[14];
    memset(output, 0xa5, sizeof(output));
    SPIC_Type before = device;
    assert(nor_read_uuid_hardware(output + 1, 12) == expected);
    assert(lock_depth == 0 && in_qpi == original_qpi);
    assert(device.ctrlr0 == before.ctrlr0 && device.ctrlr1 == before.ctrlr1);
    assert(device.ctrlr2 == before.ctrlr2 && device.addr_length == before.addr_length);
    assert(device.valid_cmd == before.valid_cmd && device.ssienr == before.ssienr);
    assert(device.baudr == before.baudr && device.auto_length == before.auto_length);
    assert(device.fbaudr == before.fbaudr);
    assert(output[0] == 0xa5 && output[13] == 0xa5);
    for (unsigned i = 0; i < 12; ++i)
        assert(output[i + 1] == (expected == 12 ? 0x10 + i : 0xa5));
}

static void cache_tests() {
    uint8_t uid[14], otp[514];
    // Model independent power cycles for each failure and incoming CPU state.
    for (unsigned caches : {0U, SCB_CCR_IC_Msk, SCB_CCR_DC_Msk,
                            SCB_CCR_IC_Msk | SCB_CCR_DC_Msk}) {
      for (unsigned irq : {0U, 1U}) {
        for (unsigned fault : {0U, 1U, 2U, 3U}) {
          setup(true);
          nor_cache_initialized = 0;
          nor_mock_scb.CCR = caches;
          primask = irq;
          memset(uid, 0xa5, sizeof(uid));
          memset(otp, 0xa5, sizeof(otp));
          assert(carbox_nor_read_uuid(uid + 1, 12) == CARBOX_NOR_UUID_NOT_READY);
          assert(carplay_nor_read_otp(0, otp + 1, 512) == CARPLAY_NOR_OTP_NOT_READY);
          assert(transactions == 0 && guard_entries == 0);
          // UUID bad ID, OTP partial read failure, or both unavailable.
          if (fault == 1) bad_at = 2;
          if (fault == 2) timeout_at = 12; // UUID 6 commands + OTP second data read
          if (fault == 3) pglob_spic_adaptor = nullptr;
          carbox_nor_identity_cache_init();
          assert(primask == irq && nor_mock_scb.CCR == caches && lock_depth == 0);
          const unsigned commands = transactions, locks = guard_entries;
          const int uid_ret = fault == 1 ? CARBOX_NOR_UUID_INVALID_DATA :
                              fault == 3 ? CARBOX_NOR_UUID_NOT_READY : 12;
          const int otp_ret = fault == 2 ? CARPLAY_NOR_OTP_TIMEOUT :
                              fault == 3 ? CARPLAY_NOR_OTP_NOT_READY : 512;
          // Hardware becomes inaccessible. Cached reads and repeated init must
          // neither touch its registers nor acquire a hardware guard.
          pglob_spic_adaptor = nullptr;
          for (unsigned repeat = 0; repeat < 3; ++repeat) {
            carbox_nor_identity_cache_init();
            assert(carbox_nor_read_uuid(uid + 1, 12) == uid_ret);
            for (unsigned i = 0; i < 12; ++i)
              assert(uid[i + 1] == (uid_ret == 12 ? 0x10 + i : 0xa5));
            assert(uid[0] == 0xa5 && uid[13] == 0xa5);
            for (unsigned offset : {0U, 1U, 255U, 511U}) {
              size_t len = 512 - offset;
              memset(otp, 0xa5, sizeof(otp));
              assert(carplay_nor_read_otp(offset, otp + 1, len) ==
                     (otp_ret == 512 ? (int)len : otp_ret));
              for (unsigned i = 0; i < 513; ++i)
                assert(otp[i + 1] == (otp_ret == 512 && i < len ?
                                     (uint8_t)(offset + i) : 0xa5));
              assert(otp[0] == 0xa5);
            }
          }
          assert(carbox_nor_read_uuid(nullptr, 12) == -1);
          assert(carbox_nor_read_uuid(uid, 11) == -1);
          assert(carplay_nor_read_otp(512, nullptr, 0) == 0);
          assert(carplay_nor_read_otp(511, otp, 2) == -1);
          assert(carplay_nor_read_otp(UINT32_MAX, otp, 1) == -1);
          assert(carplay_nor_read_otp(0, otp, SIZE_MAX) == -1);
          assert(carplay_nor_read_otp(0, nullptr, 1) == -1);
          assert(transactions == commands && guard_entries == locks);
        }
      }
    }
    puts("NOR boot cache: full OTP, API bounds, independent failures, no rereads, CPU-state restoration PASS");
}

static void legacy_otp_tests() {
    const unsigned commands = transactions, guards = guard_entries;
    uint8_t output[18];
    for (auto read : {__wrap_spinor_read_otp, __wrap_CarApi_GetFlashOTP}) {
        nor_cache_initialized = 0;
        memset(output, 0xa5, sizeof(output));
        assert(read(0, output + 1) == CARPLAY_NOR_OTP_NOT_READY);
        for (uint8_t byte : output) assert(byte == 0xa5);
        nor_cache_initialized = 1;
        // Every cached hardware error must survive both legacy API layers.
        for (int error = -1; error >= -6; --error) {
            nor_cached_otp_ret = error;
            assert(read(0, output + 1) == error);
            for (uint8_t byte : output) assert(byte == 0xa5);
        }
        nor_cached_otp_ret = 512;
        for (unsigned i = 0; i < 512; ++i) nor_cached_otp[i] = (uint8_t)i;
        for (unsigned offset : {0U, 1U, 496U}) {
            memset(output, 0xa5, sizeof(output));
            assert(read(offset, output + 1) == 1);
            assert(output[0] == 0xa5 && output[17] == 0xa5);
            for (unsigned i = 0; i < 16; ++i)
                assert(output[i + 1] == (uint8_t)(offset + i));
        }
        memset(output, 0xa5, sizeof(output));
        for (unsigned offset : {497U, 512U, UINT32_MAX})
            assert(read(offset, output + 1) == CARPLAY_NOR_OTP_INVALID_ARGUMENT);
        assert(read(0, nullptr) == CARPLAY_NOR_OTP_INVALID_ARGUMENT);
        for (uint8_t byte : output) assert(byte == 0xa5);
    }
    assert(transactions == commands && guard_entries == guards);
    puts("NOR legacy OTP wrappers: success=1, errors preserved, output bounds, cache-only PASS");
}

int main() {
    // Preserve the calibrated sampling residual, not XIP protocol clocks.
    for (unsigned baud : {2U, 4U}) {
        for (unsigned path_delay : {2U, 7U, 19U}) {
            setup(true);
            device.fbaudr = baud;
            expected_auto_length = device.auto_length =
                0x58030000U | (4U * baud * 2U + path_delay);
            expected_rx_delay = path_delay;
            run(12);
            run_otp(0, 16, 16);
        }
    }
    for (unsigned bad_delay : {1U, 20U}) {
        setup(true);
        expected_auto_length = device.auto_length = 0x58030000U | (16U + bad_delay);
        run(CARBOX_NOR_UUID_INVALID_DATA);
        run_otp(0, 16, CARPLAY_NOR_OTP_INVALID_DATA);
        assert(transactions == 0);
    }
    setup(true); adapter.dummy_cycle = nullptr;
    run(CARBOX_NOR_UUID_INVALID_DATA);
    run_otp(0, 16, CARPLAY_NOR_OTP_INVALID_DATA);
    assert(transactions == 0);
    setup(true); device.fbaudr = 0;
    run(CARBOX_NOR_UUID_INVALID_DATA);
    run_otp(0, 16, CARPLAY_NOR_OTP_INVALID_DATA);
    assert(transactions == 0);
    for (bool qpi : {false, true}) {
        setup(qpi); run(12);
        assert(logs.empty());
        assert(transactions == (qpi ? 6U : 5U));
        // Both enable-bit behaviours are valid after completed TX/RX.
        setup(qpi); enable_stays_set = false; run(12);
        // Empty FIFO alone is insufficient: the serial shifter may be busy.
        setup(qpi); busy_at = 1; run(CARBOX_NOR_UUID_TIMEOUT);
        for (int step = 1; step <= (qpi ? 6 : 5); ++step) {
            setup(qpi); timeout_at = step; run(CARBOX_NOR_UUID_TIMEOUT);
#if CARBOX_NOR_UUID_DIAG
            assert(logs.find("ret=-3") != std::string::npos);
            assert(logs.find("id=1c3817") != std::string::npos);
            assert(logs.find("timeouts=1") != std::string::npos);
            assert(logs.find("SSIENR=00000001") != std::string::npos);
            assert(logs.find("BAUDR=0000000a") != std::string::npos);
            assert(logs.find("subtract_bus=16 rx_delay_bus=7 command_baud=10") != std::string::npos);
            if (step == 1 || step == 2 || step == 6)
                assert(logs.find("TXFLR=1 RXFLR=0") != std::string::npos);
            assert(logs.find("phase=" + std::string(step == 1 || step == 6 ?
                "complete-wait" : "rx-wait")) != std::string::npos);
#else
            assert(logs.empty());
#endif
        }
        for (int step = 2; step <= 5; ++step) {
            setup(qpi); bad_at = step; run(CARBOX_NOR_UUID_INVALID_DATA);
#if CARBOX_NOR_UUID_DIAG
            assert(logs.find("operation_ret=-4 cleanup_ret=0") != std::string::npos);
            const char *stage = step == 2 ? "stage=jedec" :
                                step == 3 ? "stage=sfdp" : "stage=uid-compare";
            assert(logs.find(stage) != std::string::npos);
            if (step == 2) assert(logs.find("observed_jedec=9c3817") != std::string::npos);
            if (step == 3) assert(logs.find("sfdp=d3464450") != std::string::npos);
            if (step >= 4) assert(logs.find("mismatch_index=0") != std::string::npos);
#endif
        }
    }
    setup(false); device.sr_b.busy = 1; run(CARBOX_NOR_UUID_TIMEOUT);
#if CARBOX_NOR_UUID_DIAG
    assert(logs.find("seq=0 phase=pre-busy") != std::string::npos);
#endif
    setup(false); busy_at = 2; run(CARBOX_NOR_UUID_TIMEOUT);
#if CARBOX_NOR_UUID_DIAG
    assert(logs.find("seq=2 phase=complete-wait op=9f") != std::string::npos);
    assert(logs.find("rx=3/3") != std::string::npos);
    assert(logs.find("launch seq=2 op=9f SER=00000001 loaded_tx=1 loaded_en=00000000") != std::string::npos);
#endif
    // Full RX must still fail if the transmit FIFO has not drained.
    setup(false); fifo_pending_at = 2; run(CARBOX_NOR_UUID_TIMEOUT);
    setup(false); fail_read_byte = 7; run(CARBOX_NOR_UUID_TIMEOUT);
#if CARBOX_NOR_UUID_DIAG
    assert(logs.find("seq=4 phase=rx-wait op=5a addr=000080 rx=7/12") != std::string::npos);
#endif
    setup(true); timeout_at = 2; restore_timeout = true; run(CARBOX_NOR_UUID_TIMEOUT);
#if CARBOX_NOR_UUID_DIAG
    assert(logs.find("timeouts=2") != std::string::npos);
    assert(logs.find("seq=2 phase=rx-wait op=9f") != std::string::npos);
    assert(logs.find("seq=3 phase=complete-wait op=38") != std::string::npos);
#endif
    setup(false);
    assert(nor_read_uuid_hardware(nullptr, 12) == CARBOX_NOR_UUID_INVALID_ARGUMENT);
    uint8_t short_buffer[11];
    assert(nor_read_uuid_hardware(short_buffer, 11) == CARBOX_NOR_UUID_INVALID_ARGUMENT);
    assert(transactions == 0 && lock_depth == 0);
    setup(false); adapter.flash_id[0] = 0xef; run(CARBOX_NOR_UUID_UNSUPPORTED);
    setup(false); adapter.dtr_en = 1; run(CARBOX_NOR_UUID_UNSUPPORTED);
    setup(false); pglob_spic_adaptor = nullptr; run(CARBOX_NOR_UUID_NOT_READY);
    for (bool qpi : {false, true}) {
        for (unsigned offset : {0U, 1U, 15U, 16U, 255U, 511U}) {
            setup(qpi); run_otp(offset, 1, 1);
            assert(otp_enters == 1 && otp_exits == 1 && otp_reads == 1);
        }
        setup(qpi); run_otp(0, 512, 512);
        assert(otp_reads == 32 && otp_exits == 1);
        assert(logs.empty());
        setup(qpi); enable_stays_set = false; run_otp(0, 512, 512);
        setup(qpi); busy_at = 2; run_otp(0, 16, CARPLAY_NOR_OTP_TIMEOUT);
        setup(qpi); fifo_pending_at = 2; run_otp(0, 16, CARPLAY_NOR_OTP_TIMEOUT);
        setup(qpi); run_otp(7, 33, 33);
        assert(otp_reads == 3);
        for (int fill : {0, 255}) {
            setup(qpi); otp_fill = fill; run_otp(0, 512, 512);
        }
        // FF, RDSR, RDID, enter, 32 reads, exit, optional QPI restore.
        for (int step = 1; step <= (qpi ? 38 : 37); ++step) {
            setup(qpi); timeout_at = step;
            run_otp(0, 512, CARPLAY_NOR_OTP_TIMEOUT);
            assert(otp_exits == (step >= 4 ? 1U : 0U));
#if CARBOX_NOR_UUID_DIAG
            assert(logs.find("[nor-otp] ret=-3") != std::string::npos);
            assert(logs.find("offset=0 requested=512") != std::string::npos);
            assert(logs.find("timeouts=1") != std::string::npos);
            if (step >= 37) {
                assert(logs.find("stage=done") != std::string::npos);
                assert(logs.find("operation_ret=512 cleanup_ret=-3") != std::string::npos);
            } else {
                assert(logs.find("operation_ret=-3 cleanup_ret=0") != std::string::npos);
            }
#else
            assert(logs.empty());
#endif
        }
        setup(qpi); fail_read_byte = 7;
        run_otp(0, 16, CARPLAY_NOR_OTP_TIMEOUT);
        assert(otp_exits == 1);
#if CARBOX_NOR_UUID_DIAG
        assert(logs.find("stage=otp-read") != std::string::npos);
        assert(logs.find("op=03 addr=7ff000 rx=7/16") != std::string::npos);
        assert(logs.find("BAUDR=0000000a") != std::string::npos);
#endif
        setup(qpi); bad_at = 3; run_otp(0, 16, CARPLAY_NOR_OTP_INVALID_DATA);
        assert(otp_enters == 0);
        for (uint8_t status : {1, 2, 3}) {
            setup(qpi); status_register = status;
            run_otp(0, 16, CARPLAY_NOR_OTP_BUSY);
            assert(otp_enters == 0 && otp_exits == 0);
#if CARBOX_NOR_UUID_DIAG
            assert(logs.find("stage=status") != std::string::npos);
            assert(logs.find("valid_id/sfdp/status=0/0/1") != std::string::npos);
#endif
        }
    }
    setup(false);
    run_otp(511, 2, CARPLAY_NOR_OTP_INVALID_ARGUMENT);
    run_otp(UINT32_MAX, 1, CARPLAY_NOR_OTP_INVALID_ARGUMENT);
    run_otp(0, SIZE_MAX, CARPLAY_NOR_OTP_INVALID_ARGUMENT);
    run_otp(513, 0, CARPLAY_NOR_OTP_INVALID_ARGUMENT);
    assert(nor_read_otp_hardware(0, nullptr, 1) == CARPLAY_NOR_OTP_INVALID_ARGUMENT);
    assert(nor_read_otp_hardware(512, nullptr, 0) == 0);
    assert(transactions == 0 && lock_depth == 0);
    setup(false); adapter.flash_id[0] = 0xef;
    run_otp(0, 16, CARPLAY_NOR_OTP_UNSUPPORTED);
    setup(false); adapter.dtr_en = 1; run_otp(0, 16, CARPLAY_NOR_OTP_UNSUPPORTED);
    setup(false); pglob_spic_adaptor = nullptr;
    run_otp(0, 16, CARPLAY_NOR_OTP_NOT_READY);
#if CARBOX_NOR_UUID_DIAG
    assert(logs.find("stage=adaptor") != std::string::npos);
#endif
    setup(false); device.sr_b.busy = 1;
    run_otp(0, 16, CARPLAY_NOR_OTP_TIMEOUT);
#if CARBOX_NOR_UUID_DIAG
    assert(logs.find("seq=0 phase=pre-busy") != std::string::npos);
#endif
    setup(true); timeout_at = 5; restore_timeout = true;
    run_otp(0, 16, CARPLAY_NOR_OTP_TIMEOUT);
#if CARBOX_NOR_UUID_DIAG
    assert(logs.find("operation_ret=-3 cleanup_ret=-3") != std::string::npos);
    assert(logs.find("timeouts=2") != std::string::npos);
    assert(logs.find("op=03 addr=7ff000") != std::string::npos);
    assert(logs.find("op=38") != std::string::npos);
#endif
    cache_tests();
    legacy_otp_tests();
    puts("NOR UID + OTP protocol/control-flow tests passed (not hardware validation)");
}
