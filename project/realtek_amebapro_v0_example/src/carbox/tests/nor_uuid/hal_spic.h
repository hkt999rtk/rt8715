#ifndef NOR_UUID_MOCK_SPIC_H
#define NOR_UUID_MOCK_SPIC_H
#include <stdint.h>
#define __IOM
#define __IM
#define __OM
#include "rtl8195bhp_spic_type.h"

/* C++ MMIO proxy: record FIFO writes and supply scripted FIFO reads. */
struct MockFifo {
    void operator=(uint8_t value);
    operator uint8_t();
};
struct SPIC_Type {
    union { uint32_t ctrlr0; decltype(spic_ctrlr0_t::b) ctrlr0_b; };
    uint32_t ctrlr1;
    union { uint32_t ctrlr2; decltype(spic_ctrlr2_t::b) ctrlr2_b; };
    union { uint32_t ssienr; decltype(spic_ssienr_t::b) ssienr_b; };
    union { uint32_t sr; decltype(spic_sr_t::b) sr_b; };
    uint32_t addr_length;
    union { uint32_t baudr; decltype(spic_baudr_t::b) baudr_b; };
    union { uint32_t valid_cmd; decltype(spic_valid_cmd_t::b) valid_cmd_b; };
    uint32_t flush_fifo;
    uint32_t txflr, rxflr, fbaudr, auto_length;
    MockFifo dr_byte;
};
enum { SingleChnl = 0, QuadChnl = 2, TxMode = 0, RxMode = 3,
       FourBytesLength = 0, ThreeBytesLength = 3, MAX_BAUD_RATE = 10,
       SpicQuadIOMode = 4, SpicQpiMode = 6,
       FLASH_TYPE_EON = 5 };
struct hal_spic_adaptor_t {
    SPIC_Type *spic_dev;
    uint8_t flash_type, flash_id[3], dtr_en, spic_bit_mode, spic_send_cmd_mode;
};
void spic_disable_rtl8195bhp(SPIC_Type *);
void spic_enable_rtl8195bhp(SPIC_Type *);
#define __DSB() ((void)0)
#define __ISB() ((void)0)
#endif
