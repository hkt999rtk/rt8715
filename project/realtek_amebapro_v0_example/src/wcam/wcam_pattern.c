#include "wcam_pattern.h"
#include <string.h>

/* BT.601 limited-range 100% bars: white, yellow, cyan, green,
 * magenta, red, blue, black. NV12 stores interleaved Cb then Cr. */
static const uint8_t bars[8][3] = {
    {235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
    {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}
};

/* 5x7 digits, followed by F R A M E and space. */
static const uint8_t glyphs[16][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
    {31,16,16,30,16,16,16}, {30,17,17,30,20,18,17},
    {14,17,17,31,17,17,17}, {17,27,21,21,17,17,17},
    {31,16,16,30,16,16,31}, {0,0,0,0,0,0,0}
};

void wcam_pattern_init(uint8_t *nv12)
{
    unsigned x, y;
    for (x = 0; x < WCAM_WIDTH; ++x)
        nv12[x] = bars[x / (WCAM_WIDTH / 8)][0];
    for (y = 1; y < WCAM_HEIGHT; ++y)
        memcpy(nv12 + y * WCAM_WIDTH, nv12, WCAM_WIDTH);
    for (x = 0; x < WCAM_WIDTH; x += 2) {
        unsigned bar = x / (WCAM_WIDTH / 8);
        nv12[WCAM_Y_SIZE + x] = bars[bar][1];
        nv12[WCAM_Y_SIZE + x + 1] = bars[bar][2];
    }
    for (y = 1; y < WCAM_HEIGHT / 2; ++y)
        memcpy(nv12 + WCAM_Y_SIZE + y * WCAM_WIDTH,
               nv12 + WCAM_Y_SIZE, WCAM_WIDTH);
}

void wcam_pattern_counter(uint8_t *nv12, uint32_t counter)
{
    enum { SCALE = 8, PANEL_W = 800, PANEL_H = 96, TEXT_W = 760 };
    const unsigned px = (WCAM_WIDTH - PANEL_W) / 2;
    const unsigned py = (WCAM_HEIGHT - PANEL_H) / 2;
    const unsigned tx = (WCAM_WIDTH - TEXT_W) / 2;
    const unsigned ty = (WCAM_HEIGHT - 7 * SCALE) / 2;
    uint8_t text[16] = {10,11,12,13,14,15};
    unsigned i, y, x, row, col;
    for (i = 16; i > 6; --i) {
        text[i - 1] = counter % 10;
        counter /= 10;
    }
    for (y = py; y < py + PANEL_H; ++y)
        memset(nv12 + y * WCAM_WIDTH + px, 16, PANEL_W);
    for (y = py / 2; y < (py + PANEL_H) / 2; ++y)
        memset(nv12 + WCAM_Y_SIZE + y * WCAM_WIDTH + px, 128, PANEL_W);
    for (i = 0; i < 16; ++i)
        for (row = 0; row < 7; ++row)
            for (col = 0; col < 5; ++col)
                if (glyphs[text[i]][row] & (1u << (4 - col))) {
                    x = tx + i * 6 * SCALE + col * SCALE;
                    for (y = 0; y < SCALE; ++y)
                        memset(nv12 + (ty + row * SCALE + y) * WCAM_WIDTH + x,
                               235, SCALE);
                }
}
