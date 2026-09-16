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
static int complete_timeout_at;
static bool restore_timeout;

#if CARBOX_NOR_UUID_DIAG
static int test_log(const char *format, ...) {
    assert(lock_depth == 0); // Never call logging while flash is locked.
    assert(device.ctrlr0 == 0x00400300 && device.ctrlr1 == 17);
    assert(device.ctrlr2 == 9 && device.addr_length == 3);
    assert(device.valid_cmd == 0x1234 && device.ssienr == 0);
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

void flash_resource_lock() { assert(lock_depth++ == 0); }
void flash_resource_unlock() { assert(--lock_depth == 0); }
void MockFifo::operator=(uint8_t value) { tx.push_back(value); }
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
    assert(lock_depth == 1);
    assert(!tx.empty());
    ++transactions;
    assert(dev->ctrlr0_b.cmd_ch == (in_qpi ? QuadChnl : SingleChnl));
    assert(dev->ctrlr0_b.addr_ch == dev->ctrlr0_b.cmd_ch);
    assert(dev->ctrlr0_b.data_ch == dev->ctrlr0_b.cmd_ch);
    assert(!dev->ctrlr0_b.fast_rd && !dev->valid_cmd_b.prm_en);
    assert(!dev->ctrlr2_b.seq_en);
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
    dev->ssienr = timeout || (int)transactions == complete_timeout_at;
    dev->sr = 0;
    dev->sr_b.rfne = !rx.empty() && !timeout;
    dev->txflr = timeout ? (unsigned)tx.size() : 0;
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
    device.baudr = 3;
    adapter = {&device, FLASH_TYPE_EON, {0x1c, 0x38, 0x17}, 0,
               (uint8_t)(qpi ? SpicQpiMode : SpicQuadIOMode),
               (uint8_t)(qpi ? QuadChnl : SingleChnl)};
    pglob_spic_adaptor = &adapter;
    in_qpi = original_qpi = qpi;
    transactions = 0;
    timeout_at = bad_at = -1;
    lock_depth = 0;
    in_otp = false;
    status_register = 0;
    otp_fill = fail_read_byte = -1;
    otp_reads = otp_enters = otp_exits = 0;
    complete_timeout_at = -1;
    restore_timeout = false;
    logs.clear();
}

static void run_otp(uint32_t offset, size_t length, int expected) {
    uint8_t output[514];
    memset(output, 0xa5, sizeof(output));
    SPIC_Type before = device;
    assert(carplay_nor_read_otp(offset, output + 1, length) == expected);
    assert(lock_depth == 0 && in_qpi == original_qpi && !in_otp);
    assert(device.ctrlr0 == before.ctrlr0 && device.ctrlr1 == before.ctrlr1);
    assert(device.ctrlr2 == before.ctrlr2 && device.addr_length == before.addr_length);
    assert(device.valid_cmd == before.valid_cmd && device.ssienr == before.ssienr);
    assert(device.baudr == before.baudr);
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
    assert(carbox_nor_read_uuid(output + 1, 12) == expected);
    assert(lock_depth == 0 && in_qpi == original_qpi);
    assert(device.ctrlr0 == before.ctrlr0 && device.ctrlr1 == before.ctrlr1);
    assert(device.ctrlr2 == before.ctrlr2 && device.addr_length == before.addr_length);
    assert(device.valid_cmd == before.valid_cmd && device.ssienr == before.ssienr);
    assert(output[0] == 0xa5 && output[13] == 0xa5);
    for (unsigned i = 0; i < 12; ++i)
        assert(output[i + 1] == (expected == 12 ? 0x10 + i : 0xa5));
}

int main() {
    for (bool qpi : {false, true}) {
        setup(qpi); run(12);
        assert(logs.empty());
        assert(transactions == (qpi ? 6U : 5U));
        for (int step = 1; step <= (qpi ? 6 : 5); ++step) {
            setup(qpi); timeout_at = step; run(CARBOX_NOR_UUID_TIMEOUT);
#if CARBOX_NOR_UUID_DIAG
            assert(logs.find("ret=-3") != std::string::npos);
            assert(logs.find("id=1c3817") != std::string::npos);
            assert(logs.find("timeouts=1") != std::string::npos);
            assert(logs.find("SSIENR=00000001") != std::string::npos);
            assert(logs.find("BAUDR=00000003") != std::string::npos);
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
        }
    }
    setup(false); device.sr_b.busy = 1; run(CARBOX_NOR_UUID_TIMEOUT);
#if CARBOX_NOR_UUID_DIAG
    assert(logs.find("seq=0 phase=pre-busy") != std::string::npos);
#endif
    setup(false); complete_timeout_at = 2; run(CARBOX_NOR_UUID_TIMEOUT);
#if CARBOX_NOR_UUID_DIAG
    assert(logs.find("seq=2 phase=complete-wait op=9f") != std::string::npos);
    assert(logs.find("rx=3/3") != std::string::npos);
#endif
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
    assert(carbox_nor_read_uuid(nullptr, 12) == CARBOX_NOR_UUID_INVALID_ARGUMENT);
    uint8_t short_buffer[11];
    assert(carbox_nor_read_uuid(short_buffer, 11) == CARBOX_NOR_UUID_INVALID_ARGUMENT);
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
        }
        setup(qpi); fail_read_byte = 7;
        run_otp(0, 16, CARPLAY_NOR_OTP_TIMEOUT);
        assert(otp_exits == 1);
        setup(qpi); bad_at = 3; run_otp(0, 16, CARPLAY_NOR_OTP_INVALID_DATA);
        assert(otp_enters == 0);
        for (uint8_t status : {1, 2, 3}) {
            setup(qpi); status_register = status;
            run_otp(0, 16, CARPLAY_NOR_OTP_BUSY);
            assert(otp_enters == 0 && otp_exits == 0);
        }
    }
    setup(false);
    run_otp(511, 2, CARPLAY_NOR_OTP_INVALID_ARGUMENT);
    run_otp(UINT32_MAX, 1, CARPLAY_NOR_OTP_INVALID_ARGUMENT);
    run_otp(0, SIZE_MAX, CARPLAY_NOR_OTP_INVALID_ARGUMENT);
    run_otp(513, 0, CARPLAY_NOR_OTP_INVALID_ARGUMENT);
    assert(carplay_nor_read_otp(0, nullptr, 1) == CARPLAY_NOR_OTP_INVALID_ARGUMENT);
    assert(carplay_nor_read_otp(512, nullptr, 0) == 0);
    assert(transactions == 0 && lock_depth == 0);
    setup(false); adapter.flash_id[0] = 0xef;
    run_otp(0, 16, CARPLAY_NOR_OTP_UNSUPPORTED);
    setup(false); adapter.dtr_en = 1; run_otp(0, 16, CARPLAY_NOR_OTP_UNSUPPORTED);
    setup(false); pglob_spic_adaptor = nullptr;
    run_otp(0, 16, CARPLAY_NOR_OTP_NOT_READY);
    puts("NOR UID + OTP protocol/control-flow tests passed (not hardware validation)");
}
