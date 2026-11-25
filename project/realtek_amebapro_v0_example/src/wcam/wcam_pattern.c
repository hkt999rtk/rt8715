#include "wcam_pattern.h"
#include <string.h>

/* BT.601 limited-range 100% bars: white, yellow, cyan, green,
 * magenta, red, blue, black. NV12 stores interleaved Cb then Cr. */
static const uint8_t bars[8][3] = {
    {235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
    {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}
};

/* 5x7 digits, FRAME, TX and the RX waiting message. */
static const uint8_t glyphs[24][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
    {31,16,16,30,16,16,16}, {30,17,17,30,20,18,17},
    {14,17,17,31,17,17,17}, {17,27,21,21,17,17,17},
    {31,16,16,30,16,16,31}, {0,0,0,0,0,0,0},
    {17,17,10,4,10,17,17}, {31,4,4,4,4,4,4},
    {17,17,17,21,21,21,10}, {31,4,4,4,4,4,31},
    {17,25,21,19,17,17,17}, {14,17,16,23,17,17,14},
    {14,17,17,17,17,17,14}, {14,17,16,16,16,17,14}
};

static void draw_glyph(uint8_t *nv12, unsigned glyph, unsigned x,
                       unsigned y, unsigned scale)
{
    unsigned row, col, line;
    for (row = 0; row < 7; ++row)
        for (col = 0; col < 5; ++col)
            if (glyphs[glyph][row] & (1u << (4 - col)))
                for (line = 0; line < scale; ++line)
                    memset(nv12 + (y + row * scale + line) * WCAM_WIDTH +
                           x + col * scale, 235, scale);
}

static unsigned waiting_glyph(char c)
{
    switch (c) {
    case 'W': return 18;
    case 'A': return 12;
    case 'I': return 19;
    case 'T': return 17;
    case 'N': return 20;
    case 'G': return 21;
    case 'F': return 10;
    case 'O': return 22;
    case 'R': return 11;
    case 'C': return 23;
    case 'E': return 14;
    default: return 15;
    }
}

static void draw_waiting(uint8_t *nv12, uint32_t counter)
{
    static const char message[] = "WAITING FOR CONNECTION";
    enum { MESSAGE_SCALE = 6, COUNTER_SCALE = 8,
           MESSAGE_X = (WCAM_WIDTH - (sizeof(message) - 1) * 6 * MESSAGE_SCALE) / 2,
           MESSAGE_Y = 295, COUNTER_X = (WCAM_WIDTH - 16 * 6 * COUNTER_SCALE) / 2,
           COUNTER_Y = 369 };
    uint8_t text[16] = {10, 11, 12, 13, 14, 15};
    unsigned i, y;
    for (i = 16; i > 6; --i) {
        text[i - 1] = counter % 10;
        counter /= 10;
    }
    for (y = MESSAGE_Y; y < MESSAGE_Y + 7 * MESSAGE_SCALE; ++y)
        memset(nv12 + y * WCAM_WIDTH + MESSAGE_X, 16,
               (sizeof(message) - 1) * 6 * MESSAGE_SCALE);
    for (y = COUNTER_Y; y < COUNTER_Y + 7 * COUNTER_SCALE; ++y)
        memset(nv12 + y * WCAM_WIDTH + COUNTER_X, 16, 16 * 6 * COUNTER_SCALE);
    for (i = 0; i < sizeof(message) - 1; ++i)
        draw_glyph(nv12, waiting_glyph(message[i]),
                   MESSAGE_X + i * 6 * MESSAGE_SCALE, MESSAGE_Y, MESSAGE_SCALE);
    for (i = 0; i < 16; ++i)
        draw_glyph(nv12, text[i], COUNTER_X + i * 6 * COUNTER_SCALE,
                   COUNTER_Y, COUNTER_SCALE);
}

void wcam_pattern_init_role(uint8_t *nv12, int role)
{
    unsigned x, y;
    if (role == WCAM_PATTERN_RX) {
        memset(nv12, 16, WCAM_Y_SIZE);
        memset(nv12 + WCAM_Y_SIZE, 128, WCAM_NV12_SIZE - WCAM_Y_SIZE);
        return;
    }
    for (x = 0; x < WCAM_WIDTH; ++x)
        nv12[x] = bars[role == WCAM_PATTERN_TX ? 7 - x / (WCAM_WIDTH / 8) : x / (WCAM_WIDTH / 8)][0];
    for (y = 1; y < WCAM_HEIGHT; ++y)
        memcpy(nv12 + y * WCAM_WIDTH, nv12, WCAM_WIDTH);
    for (x = 0; x < WCAM_WIDTH; x += 2) {
        unsigned bar = x / (WCAM_WIDTH / 8);
        if (role == WCAM_PATTERN_TX) bar = 7 - bar;
        nv12[WCAM_Y_SIZE + x] = bars[bar][1];
        nv12[WCAM_Y_SIZE + x + 1] = bars[bar][2];
    }
    for (y = 1; y < WCAM_HEIGHT / 2; ++y)
        memcpy(nv12 + WCAM_Y_SIZE + y * WCAM_WIDTH,
               nv12 + WCAM_Y_SIZE, WCAM_WIDTH);
}

void wcam_pattern_init(uint8_t *nv12) { wcam_pattern_init_role(nv12, WCAM_PATTERN_LOCAL); }

void wcam_pattern_counter_role(uint8_t *nv12, uint32_t counter, int role)
{
    enum { SCALE = 8, PANEL_H = 96 };
    const unsigned label = role == WCAM_PATTERN_LOCAL ? 0 : 3;
    const unsigned panel_w = label ? 1000 : 800;
    const unsigned text_w = label ? 912 : 760;
    const unsigned px = (WCAM_WIDTH - panel_w) / 2;
    const unsigned py = (WCAM_HEIGHT - PANEL_H) / 2;
    const unsigned tx = (WCAM_WIDTH - text_w) / 2;
    const unsigned ty = (WCAM_HEIGHT - 7 * SCALE) / 2;
    uint8_t text[19] = {0};
    unsigned i, y, x, row, col;
    if (role == WCAM_PATTERN_RX) {
        draw_waiting(nv12, counter);
        return;
    }
    if (label) {
        text[0] = role == WCAM_PATTERN_TX ? 17 : 11;
        text[1] = 16; text[2] = 15;
    }
    text[label]=10; text[label+1]=11; text[label+2]=12;
    text[label+3]=13; text[label+4]=14; text[label+5]=15;
    for (i = label + 16; i > label + 6; --i) {
        text[i - 1] = counter % 10;
        counter /= 10;
    }
    for (y = py; y < py + PANEL_H; ++y)
        memset(nv12 + y * WCAM_WIDTH + px, 16, panel_w);
    for (y = py / 2; y < (py + PANEL_H) / 2; ++y)
        memset(nv12 + WCAM_Y_SIZE + y * WCAM_WIDTH + px, 128, panel_w);
    for (i = 0; i < label + 16; ++i)
        for (row = 0; row < 7; ++row)
            for (col = 0; col < 5; ++col)
                if (glyphs[text[i]][row] & (1u << (4 - col))) {
                    x = tx + i * 6 * SCALE + col * SCALE;
                    for (y = 0; y < SCALE; ++y)
                        memset(nv12 + (ty + row * SCALE + y) * WCAM_WIDTH + x,
                               235, SCALE);
                }
}

void wcam_pattern_counter(uint8_t *nv12, uint32_t counter)
{
    wcam_pattern_counter_role(nv12,counter,WCAM_PATTERN_LOCAL);
}
