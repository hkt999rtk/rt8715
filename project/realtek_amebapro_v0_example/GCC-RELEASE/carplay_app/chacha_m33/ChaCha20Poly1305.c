/* ChaCha20 and ChaCha20-Poly1305 implementation. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "ChaCha20Poly1305.h"
#include "ChaCha20Poly1305_rtl8195b.h"
#if defined(__has_include)
#  if __has_include(<string.h>)
#    include <string.h>
#  else
void *memset(void *s, int c, size_t n);
#  endif
#else
#  include <string.h>
#endif

/* ---------------- Target select ---------------- */
#if !defined(CHACHA_TARGET_M0) && !defined(CHACHA_TARGET_M4) && !defined(CHACHA_TARGET_M33)
  #if defined(__ARM_ARCH_8M_MAIN__)
    #define CHACHA_TARGET_M33 1
  #elif defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_7M__)
    #define CHACHA_TARGET_M4 1
  #else
    #define CHACHA_TARGET_M0 1
  #endif
#endif

#if (defined(CHACHA_TARGET_M4) && CHACHA_TARGET_M4) || \
    (defined(CHACHA_TARGET_M33) && CHACHA_TARGET_M33)
#define CHACHA_TARGET_FAST_ARM 1
#else
#define CHACHA_TARGET_FAST_ARM 0
#endif

#define FORCE_INLINE static inline __attribute__((always_inline))
#define CHACHA_UNUSED __attribute__((unused))

#ifndef CHACHA_ENABLE_CLEAR
#define CHACHA_ENABLE_CLEAR 0
#endif

#if CHACHA_ENABLE_CLEAR
#define CHACHA_CLEAR(p, n) memset((p), 0, (n))
#else
#define CHACHA_CLEAR(p, n) do { (void)(p); (void)(n); } while (0)
#endif

#if CARBOX_CHACHA_MODE != CARBOX_CHACHA_MODE_SOFTWARE_ONLY
/* Testable allocation seam; products normally use the weak default. */
__attribute__((weak))
void *carbox_chacha_aad_realloc(void *ptr, size_t len) {
  return realloc(ptr, len);
}
#endif

static void chacha_secure_clear(void *ptr, size_t len) {
  volatile uint8_t *p = (volatile uint8_t *)ptr;
  while (len-- != 0u) *p++ = 0u;
}

static void chacha_clear_state_key_material(
  chacha20_poly1305_state *state
) {
  chacha_secure_clear(state->chacha_key, sizeof(state->chacha_key));
  chacha_secure_clear(state->chacha_nonce, sizeof(state->chacha_nonce));
  chacha_secure_clear(state->poly_r, sizeof(state->poly_r));
  chacha_secure_clear(state->poly_s, sizeof(state->poly_s));
  chacha_secure_clear(state->poly_pad, sizeof(state->poly_pad));
}

/* ---------------- Portable helpers ---------------- */
FORCE_INLINE uint32_t rotl32(uint32_t x, unsigned n) {
#if defined(__has_builtin)
#  if __has_builtin(__builtin_rotateleft32)
    return __builtin_rotateleft32(x, n);
#  endif
#endif
  return (x << n) | (x >> (32u - n));
}

FORCE_INLINE uint32_t load32_le_u(const uint8_t *p) { /* unaligned ok */
  return ((uint32_t)p[0]) |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

FORCE_INLINE void store32_le_u(uint8_t *p, uint32_t v) { /* unaligned ok */
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

#define QR(a,b,c,d) do { \
  (a) += (b); (d) ^= (a); (d) = rotl32((d),16); \
  (c) += (d); (b) ^= (c); (b) = rotl32((b),12); \
  (a) += (b); (d) ^= (a); (d) = rotl32((d), 8); \
  (c) += (d); (b) ^= (c); (b) = rotl32((b), 7); \
} while (0)

/* ---------------- ChaCha20 block core ---------------- */
/* RFC7539: 32-bit counter + 96-bit nonce */
FORCE_INLINE void chacha20_block_10dr(
  uint32_t out[16],
  const uint32_t key[8],
  uint32_t counter,
  const uint32_t nonce[3],
  int unroll_10 /* 0: loop 10 times, 1: fully unrolled 10x */
){
  const uint32_t x0 = 0x61707865u, x1 = 0x3320646eu, x2 = 0x79622d32u, x3 = 0x6b206574u;
  const uint32_t x4 = key[0], x5 = key[1], x6 = key[2], x7 = key[3];
  const uint32_t x8 = key[4], x9 = key[5], x10 = key[6], x11 = key[7];
  const uint32_t x12 = counter, x13 = nonce[0], x14 = nonce[1], x15 = nonce[2];

  uint32_t a=x0,b=x1,c=x2,d=x3;
  uint32_t e=x4,f=x5,g=x6,h=x7;
  uint32_t i=x8,j=x9,k=x10,l=x11;
  uint32_t m=x12,n=x13,o=x14,p=x15;

  #define DR() do { \
    QR(a,e,i,m); QR(b,f,j,n); QR(c,g,k,o); QR(d,h,l,p); \
    QR(a,f,k,p); QR(b,g,l,m); QR(c,h,i,n); QR(d,e,j,o); \
  } while(0)

  if (!unroll_10) {
    for (int r=0; r<10; r++) DR();
  } else {
    DR(); DR(); DR(); DR(); DR();
    DR(); DR(); DR(); DR(); DR();
  }

  #undef DR

  out[0]=a+x0;  out[1]=b+x1;  out[2]=c+x2;  out[3]=d+x3;
  out[4]=e+x4;  out[5]=f+x5;  out[6]=g+x6;  out[7]=h+x7;
  out[8]=i+x8;  out[9]=j+x9;  out[10]=k+x10; out[11]=l+x11;
  out[12]=m+x12;out[13]=n+x13;out[14]=o+x14; out[15]=p+x15;
}

#undef QR

/* ---------------- API: XOR stream (encrypt/decrypt) ---------------- */
FORCE_INLINE void chacha20_load_key_nonce(
  uint32_t key[8], uint32_t nonce[3],
  const uint8_t key_bytes[32], const uint8_t nonce_bytes[12]
){
  /* always safe for M0/M4 (unaligned ok) */
  for (int i=0;i<8;i++) key[i] = load32_le_u(key_bytes + (size_t)i*4u);
  nonce[0] = load32_le_u(nonce_bytes + 0);
  nonce[1] = load32_le_u(nonce_bytes + 4);
  nonce[2] = load32_le_u(nonce_bytes + 8);
}

/* -------- M0 path: smaller code, safer alignment, still fast -------- */
#if CHACHA_TARGET_FAST_ARM
#define CHACHA_M0_MAYBE_UNUSED __attribute__((unused))
#else
#define CHACHA_M0_MAYBE_UNUSED
#endif

static CHACHA_M0_MAYBE_UNUSED void chacha20_xor_m0(
  uint8_t *out, const uint8_t *in, size_t len,
  const uint8_t key_bytes[32], const uint8_t nonce_bytes[12],
  uint32_t counter
){
  uint32_t key[8], nonce[3];
  chacha20_load_key_nonce(key, nonce, key_bytes, nonce_bytes);

  uint32_t ks[16];
  uint8_t  ks8[64];

  while (len >= 64) {
    chacha20_block_10dr(ks, key, counter++, nonce, 0 /* loop */);

    /* serialize (unaligned safe), M0 對 byte 操作較平衡，也避免 unaligned word */
    for (int i=0;i<16;i++) store32_le_u(ks8 + (size_t)i*4u, ks[i]);
    for (int i=0;i<64;i++) out[i] = (uint8_t)(in[i] ^ ks8[i]);

    in += 64; out += 64; len -= 64;
  }

  if (len) {
    chacha20_block_10dr(ks, key, counter, nonce, 0);
    for (int i=0;i<16;i++) store32_le_u(ks8 + (size_t)i*4u, ks[i]);
    for (size_t i=0;i<len;i++) out[i] = (uint8_t)(in[i] ^ ks8[i]);
  }

  /* optional clear (compile-time configurable) */
  CHACHA_CLEAR(ks, sizeof(ks));
  CHACHA_CLEAR(ks8, sizeof(ks8));
  CHACHA_CLEAR(key, sizeof(key));
  CHACHA_CLEAR(nonce, sizeof(nonce));
}

/* -------- M4 path: fastest (unaligned fallback is small) -------- */
#if CHACHA_TARGET_FAST_ARM
FORCE_INLINE int is_aligned4(const void *p) {
  return (((uintptr_t)p) & 3u) == 0u;
}

/*
 * M4/M33-like targets: prefer Thumb-2 asm core.
 * Keep a compile-time override so integrators can force C core if needed.
 */
#ifndef CHACHA_M4_ASM_CORE
#if defined(CHACHA_TARGET_M4) && CHACHA_TARGET_M4 && \
    (defined(__thumb2__) || defined(__ARM_ARCH_7EM__)) && \
    !defined(CHACHA_FORCE_C_M4_CORE)
#define CHACHA_M4_ASM_CORE 1
#else
#define CHACHA_M4_ASM_CORE 0
#endif
#endif

FORCE_INLINE void chacha20_block_10dr_m4(
  uint32_t out[16],
  const uint32_t key[8],
  uint32_t counter,
  const uint32_t nonce[3]
){
  const uint32_t x0 = 0x61707865u, x1 = 0x3320646eu, x2 = 0x79622d32u, x3 = 0x6b206574u;
  const uint32_t x4 = key[0], x5 = key[1], x6 = key[2], x7 = key[3];
  const uint32_t x8 = key[4], x9 = key[5], x10 = key[6], x11 = key[7];
  const uint32_t x12 = counter, x13 = nonce[0], x14 = nonce[1], x15 = nonce[2];

  uint32_t a=x0,b=x1,c=x2,d=x3;
  uint32_t e=x4,f=x5,g=x6,h=x7;
  uint32_t i=x8,j=x9,k=x10,l=x11;
  uint32_t m=x12,n=x13,o=x14,p=x15;

#if CHACHA_M4_ASM_CORE
  #define QR_M4(a,b,c,d) do { \
    __asm__ volatile ( \
      "adds %0, %0, %1\n\t" \
      "eors %3, %3, %0\n\t" \
      "ror %3, %3, #16\n\t" \
      "adds %2, %2, %3\n\t" \
      "eors %1, %1, %2\n\t" \
      "ror %1, %1, #20\n\t" \
      "adds %0, %0, %1\n\t" \
      "eors %3, %3, %0\n\t" \
      "ror %3, %3, #24\n\t" \
      "adds %2, %2, %3\n\t" \
      "eors %1, %1, %2\n\t" \
      "ror %1, %1, #25\n\t" \
      : "+r"(a), "+r"(b), "+r"(c), "+r"(d) \
      : \
      : "cc" \
    ); \
  } while (0)
#else
  #define QR_M4(a,b,c,d) do { \
    (a) += (b); (d) ^= (a); (d) = rotl32((d),16); \
    (c) += (d); (b) ^= (c); (b) = rotl32((b),12); \
    (a) += (b); (d) ^= (a); (d) = rotl32((d), 8); \
    (c) += (d); (b) ^= (c); (b) = rotl32((b), 7); \
  } while (0)
#endif

  #define DR_M4() do { \
    QR_M4(a,e,i,m); QR_M4(b,f,j,n); QR_M4(c,g,k,o); QR_M4(d,h,l,p); \
    QR_M4(a,f,k,p); QR_M4(b,g,l,m); QR_M4(c,h,i,n); QR_M4(d,e,j,o); \
  } while (0)

  DR_M4(); DR_M4(); DR_M4(); DR_M4(); DR_M4();
  DR_M4(); DR_M4(); DR_M4(); DR_M4(); DR_M4();

  #undef DR_M4
  #undef QR_M4

  out[0]=a+x0;  out[1]=b+x1;  out[2]=c+x2;  out[3]=d+x3;
  out[4]=e+x4;  out[5]=f+x5;  out[6]=g+x6;  out[7]=h+x7;
  out[8]=i+x8;  out[9]=j+x9;  out[10]=k+x10; out[11]=l+x11;
  out[12]=m+x12;out[13]=n+x13;out[14]=o+x14; out[15]=p+x15;
}

FORCE_INLINE void chacha20_xor64_aligned_u32(
  uint8_t *out,
  const uint8_t *in,
  const uint32_t ks[16]
){
  uint32_t *o32 = (uint32_t*)out;
  const uint32_t *i32 = (const uint32_t*)in;
  const uint32_t *k32 = ks;
  for (int w=0; w<16; w+=4) {
    o32[0] = i32[0] ^ k32[0];
    o32[1] = i32[1] ^ k32[1];
    o32[2] = i32[2] ^ k32[2];
    o32[3] = i32[3] ^ k32[3];
    o32 += 4;
    i32 += 4;
    k32 += 4;
  }
}

FORCE_INLINE void chacha20_xor64_unaligned_u32(
  uint8_t *out,
  const uint8_t *in,
  const uint32_t ks[16]
){
  const uint8_t *inb = in;
  uint8_t *outb = out;
  const uint32_t *k = ks;
  for (int w=0; w<16; w+=4) {
    uint32_t v0 = load32_le_u(inb + 0u) ^ k[0];
    uint32_t v1 = load32_le_u(inb + 4u) ^ k[1];
    uint32_t v2 = load32_le_u(inb + 8u) ^ k[2];
    uint32_t v3 = load32_le_u(inb + 12u) ^ k[3];
    store32_le_u(outb + 0u, v0);
    store32_le_u(outb + 4u, v1);
    store32_le_u(outb + 8u, v2);
    store32_le_u(outb + 12u, v3);
    inb += 16u;
    outb += 16u;
    k += 4;
  }
}

FORCE_INLINE void chacha20_xor_partial_u32(
  uint8_t *out,
  const uint8_t *in,
  const uint32_t ks[16],
  size_t len
){
  const uint32_t *k = ks;
  while (len >= 16u) {
    uint32_t v0 = load32_le_u(in + 0u) ^ k[0];
    uint32_t v1 = load32_le_u(in + 4u) ^ k[1];
    uint32_t v2 = load32_le_u(in + 8u) ^ k[2];
    uint32_t v3 = load32_le_u(in + 12u) ^ k[3];
    store32_le_u(out + 0u, v0);
    store32_le_u(out + 4u, v1);
    store32_le_u(out + 8u, v2);
    store32_le_u(out + 12u, v3);
    in += 16u;
    out += 16u;
    k += 4;
    len -= 16u;
  }

  while (len >= 4u) {
    uint32_t v = load32_le_u(in) ^ *k++;
    store32_le_u(out, v);
    in += 4u;
    out += 4u;
    len -= 4u;
  }

  if (len) {
    const uint8_t *ks8 = (const uint8_t*)k;
    for (size_t i=0; i<len; i++) {
      out[i] = (uint8_t)(in[i] ^ ks8[i]);
    }
  }
}

#if defined(CHACHA_TARGET_M33) && CHACHA_TARGET_M33
#define CHACHA_M4_FAST_MAYBE_UNUSED __attribute__((unused))
#else
#define CHACHA_M4_FAST_MAYBE_UNUSED
#endif

static CHACHA_M4_FAST_MAYBE_UNUSED void chacha20_xor_m4_fast(
  uint8_t *out, const uint8_t *in, size_t len,
  const uint8_t key_bytes[32], const uint8_t nonce_bytes[12],
  uint32_t counter
){
  uint32_t key[8], nonce[3];
  chacha20_load_key_nonce(key, nonce, key_bytes, nonce_bytes);

  /* 若你能保證 in/out 4-byte aligned，這段就是極速 */
  if (is_aligned4(in) && is_aligned4(out)) {
    uint32_t ks[16];

    while (len >= 128) {
      chacha20_block_10dr_m4(ks, key, counter++, nonce);
      chacha20_xor64_aligned_u32(out, in, ks);
      in  += 64; out += 64; len -= 64;

      chacha20_block_10dr_m4(ks, key, counter++, nonce);
      chacha20_xor64_aligned_u32(out, in, ks);
      in  += 64; out += 64; len -= 64;
    }

    while (len >= 64) {
      chacha20_block_10dr_m4(ks, key, counter++, nonce);
      chacha20_xor64_aligned_u32(out, in, ks);
      in += 64; out += 64; len -= 64;
    }

    if (len) {
      chacha20_block_10dr_m4(ks, key, counter, nonce);
      chacha20_xor_partial_u32(out, in, ks, len);
    }

    CHACHA_CLEAR(ks, sizeof(ks));
  } else {
    /* unaligned: keep M4 block core, xor by unaligned-safe 32-bit helpers */
    uint32_t ks[16];

    while (len >= 64) {
      chacha20_block_10dr_m4(ks, key, counter++, nonce);
      chacha20_xor64_unaligned_u32(out, in, ks);
      in += 64; out += 64; len -= 64;
    }

    if (len) {
      chacha20_block_10dr_m4(ks, key, counter, nonce);
      chacha20_xor_partial_u32(out, in, ks, len);
    }

    CHACHA_CLEAR(ks, sizeof(ks));
  }

  CHACHA_CLEAR(key, sizeof(key));
  CHACHA_CLEAR(nonce, sizeof(nonce));
}
#endif

#if defined(CHACHA_TARGET_M33) && CHACHA_TARGET_M33
static void chacha20_xor_m33_fast(
  uint8_t *out, const uint8_t *in, size_t len,
  const uint8_t key_bytes[32], const uint8_t nonce_bytes[12],
  uint32_t counter
){
  chacha20_xor_m4_fast(out, in, len, key_bytes, nonce_bytes, counter);
}
#endif

/* -------- Public entry -------- */
void chacha20_xor(
  uint8_t *out, const uint8_t *in, size_t len,
  const uint8_t key_bytes[32], const uint8_t nonce_bytes[12],
  uint32_t counter
){
#if defined(CHACHA_TARGET_M33) && CHACHA_TARGET_M33
  chacha20_xor_m33_fast(out, in, len, key_bytes, nonce_bytes, counter);
#elif defined(CHACHA_TARGET_M4) && CHACHA_TARGET_M4
  chacha20_xor_m4_fast(out, in, len, key_bytes, nonce_bytes, counter);
#else
  chacha20_xor_m0(out, in, len, key_bytes, nonce_bytes, counter);
#endif
}

/* encode/decode are identical */
#define chacha20_encode chacha20_xor
#define chacha20_decode chacha20_xor

/* ---------------- ChaCha20-Poly1305 (64-bit nonce/counter) ---------------- */
#define POLY1305_MASK26 0x3ffffffu

FORCE_INLINE void store64_le_u(uint8_t *p, uint64_t v) {
  store32_le_u(p, (uint32_t)v);
  store32_le_u(p + 4, (uint32_t)(v >> 32));
}

static void poly1305_blocks_c(
  chacha20_poly1305_state *state, const uint8_t *in, size_t len,
  uint32_t hibit
){
  uint32_t h0 = state->poly_h[0];
  uint32_t h1 = state->poly_h[1];
  uint32_t h2 = state->poly_h[2];
  uint32_t h3 = state->poly_h[3];
  uint32_t h4 = state->poly_h[4];
  const uint32_t r0 = state->poly_r[0];
  const uint32_t r1 = state->poly_r[1];
  const uint32_t r2 = state->poly_r[2];
  const uint32_t r3 = state->poly_r[3];
  const uint32_t r4 = state->poly_r[4];
  const uint32_t s1 = state->poly_s[0];
  const uint32_t s2 = state->poly_s[1];
  const uint32_t s3 = state->poly_s[2];
  const uint32_t s4 = state->poly_s[3];

  while (len >= 16u) {
    const uint32_t t0 = load32_le_u(in + 0);
    const uint32_t t1 = load32_le_u(in + 4);
    const uint32_t t2 = load32_le_u(in + 8);
    const uint32_t t3 = load32_le_u(in + 12);
    uint64_t d0, d1, d2, d3, d4;
    uint32_t c;

    h0 += t0 & POLY1305_MASK26;
    h1 += (uint32_t)((((uint64_t)t1 << 32) | t0) >> 26) & POLY1305_MASK26;
    h2 += (uint32_t)((((uint64_t)t2 << 32) | t1) >> 20) & POLY1305_MASK26;
    h3 += (uint32_t)((((uint64_t)t3 << 32) | t2) >> 14) & POLY1305_MASK26;
    h4 += (t3 >> 8) | hibit;

    d0 = ((uint64_t)h0 * r0) + ((uint64_t)h1 * s4) +
         ((uint64_t)h2 * s3) + ((uint64_t)h3 * s2) +
         ((uint64_t)h4 * s1);
    d1 = ((uint64_t)h0 * r1) + ((uint64_t)h1 * r0) +
         ((uint64_t)h2 * s4) + ((uint64_t)h3 * s3) +
         ((uint64_t)h4 * s2);
    d2 = ((uint64_t)h0 * r2) + ((uint64_t)h1 * r1) +
         ((uint64_t)h2 * r0) + ((uint64_t)h3 * s4) +
         ((uint64_t)h4 * s3);
    d3 = ((uint64_t)h0 * r3) + ((uint64_t)h1 * r2) +
         ((uint64_t)h2 * r1) + ((uint64_t)h3 * r0) +
         ((uint64_t)h4 * s4);
    d4 = ((uint64_t)h0 * r4) + ((uint64_t)h1 * r3) +
         ((uint64_t)h2 * r2) + ((uint64_t)h3 * r1) +
         ((uint64_t)h4 * r0);

    h0 = (uint32_t)d0 & POLY1305_MASK26;
    c = (uint32_t)(d0 >> 26);
    d1 += c;
    h1 = (uint32_t)d1 & POLY1305_MASK26;
    c = (uint32_t)(d1 >> 26);
    d2 += c;
    h2 = (uint32_t)d2 & POLY1305_MASK26;
    c = (uint32_t)(d2 >> 26);
    d3 += c;
    h3 = (uint32_t)d3 & POLY1305_MASK26;
    c = (uint32_t)(d3 >> 26);
    d4 += c;
    h4 = (uint32_t)d4 & POLY1305_MASK26;
    c = (uint32_t)(d4 >> 26);
    h0 += c * 5u;

    in += 16;
    len -= 16u;
  }

  state->poly_h[0] = h0;
  state->poly_h[1] = h1;
  state->poly_h[2] = h2;
  state->poly_h[3] = h3;
  state->poly_h[4] = h4;
}

static void poly1305_blocks(
  chacha20_poly1305_state *state, const uint8_t *in, size_t len,
  uint32_t hibit
){
  poly1305_blocks_c(state, in, len, hibit);
}

static void poly1305_init_state(
  chacha20_poly1305_state *state, const uint8_t key[32]
){
  const uint32_t t0 = load32_le_u(key + 0);
  const uint32_t t1 = load32_le_u(key + 4);
  const uint32_t t2 = load32_le_u(key + 8);
  const uint32_t t3 = load32_le_u(key + 12);

  state->poly_r[0] = t0 & 0x3ffffffu;
  state->poly_r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03u;
  state->poly_r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ffu;
  state->poly_r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fffu;
  state->poly_r[4] = (t3 >> 8) & 0x00fffffu;
  state->poly_s[0] = state->poly_r[1] * 5u;
  state->poly_s[1] = state->poly_r[2] * 5u;
  state->poly_s[2] = state->poly_r[3] * 5u;
  state->poly_s[3] = state->poly_r[4] * 5u;
  memset(state->poly_h, 0, sizeof(state->poly_h));
  state->poly_leftover = 0;
  for (size_t i = 0; i < 16u; ++i) state->poly_pad[i] = key[i + 16u];
}

static void poly1305_update_state(
  chacha20_poly1305_state *state, const uint8_t *in, size_t len
){
  if (state->poly_leftover != 0u) {
    size_t want = 16u - state->poly_leftover;
    if (want > len) want = len;
    for (size_t i = 0; i < want; ++i) {
      state->poly_buffer[state->poly_leftover + i] = in[i];
    }
    in += want;
    len -= want;
    state->poly_leftover += want;
    if (state->poly_leftover < 16u) return;
    poly1305_blocks(state, state->poly_buffer, 16u, 1u << 24);
    state->poly_leftover = 0;
  }

  if (len >= 16u) {
    const size_t blocks_len = len & ~(size_t)15u;
    poly1305_blocks(state, in, blocks_len, 1u << 24);
    in += blocks_len;
    len -= blocks_len;
  }

  for (size_t i = 0; i < len; ++i) state->poly_buffer[i] = in[i];
  state->poly_leftover = len;
}

static void poly1305_final_state(
  chacha20_poly1305_state *state, uint8_t tag[16]
){
  uint32_t h0, h1, h2, h3, h4;
  uint32_t g0, g1, g2, g3, g4;
  uint32_t c, mask, not_mask;
  uint64_t f0, f1, f2, f3;

  if (state->poly_leftover != 0u) {
    size_t i = state->poly_leftover;
    state->poly_buffer[i++] = 1u;
    while (i < 16u) state->poly_buffer[i++] = 0u;
    poly1305_blocks(state, state->poly_buffer, 16u, 0u);
  }

  h0 = state->poly_h[0];
  h1 = state->poly_h[1];
  h2 = state->poly_h[2];
  h3 = state->poly_h[3];
  h4 = state->poly_h[4];

  c = h1 >> 26; h1 &= POLY1305_MASK26; h2 += c;
  c = h2 >> 26; h2 &= POLY1305_MASK26; h3 += c;
  c = h3 >> 26; h3 &= POLY1305_MASK26; h4 += c;
  c = h4 >> 26; h4 &= POLY1305_MASK26; h0 += c * 5u;
  c = h0 >> 26; h0 &= POLY1305_MASK26; h1 += c;

  g0 = h0 + 5u; c = g0 >> 26; g0 &= POLY1305_MASK26;
  g1 = h1 + c; c = g1 >> 26; g1 &= POLY1305_MASK26;
  g2 = h2 + c; c = g2 >> 26; g2 &= POLY1305_MASK26;
  g3 = h3 + c; c = g3 >> 26; g3 &= POLY1305_MASK26;
  g4 = h4 + c - (1u << 26);

  mask = (g4 >> 31) - 1u;
  not_mask = ~mask;
  h0 = (h0 & not_mask) | (g0 & mask);
  h1 = (h1 & not_mask) | (g1 & mask);
  h2 = (h2 & not_mask) | (g2 & mask);
  h3 = (h3 & not_mask) | (g3 & mask);
  h4 = (h4 & not_mask) | (g4 & mask);

  f0 = (uint32_t)(h0 | (h1 << 26));
  f1 = (uint32_t)((h1 >> 6) | (h2 << 20));
  f2 = (uint32_t)((h2 >> 12) | (h3 << 14));
  f3 = (uint32_t)((h3 >> 18) | (h4 << 8));

  f0 += load32_le_u(state->poly_pad + 0);
  f1 += load32_le_u(state->poly_pad + 4) + (f0 >> 32);
  f2 += load32_le_u(state->poly_pad + 8) + (f1 >> 32);
  f3 += load32_le_u(state->poly_pad + 12) + (f2 >> 32);
  store32_le_u(tag + 0, (uint32_t)f0);
  store32_le_u(tag + 4, (uint32_t)f1);
  store32_le_u(tag + 8, (uint32_t)f2);
  store32_le_u(tag + 12, (uint32_t)f3);
}

static void chacha20_poly1305_xor(
  chacha20_poly1305_state *state,
  uint8_t *out, const uint8_t *in, size_t len
){
  uint32_t nonce[3];
  uint32_t block[16];
#if !CHACHA_TARGET_FAST_ARM
  uint8_t stream[64];
#endif

  nonce[0] = (uint32_t)(state->chacha_counter >> 32);
  nonce[1] = state->chacha_nonce[0];
  nonce[2] = state->chacha_nonce[1];

  while (len != 0u) {
    size_t n = len < 64u ? len : 64u;
#if CHACHA_TARGET_FAST_ARM
    chacha20_block_10dr_m4(
      block, state->chacha_key, (uint32_t)state->chacha_counter, nonce
    );
    if (n == 64u) {
      if (is_aligned4(in) && is_aligned4(out)) {
        chacha20_xor64_aligned_u32(out, in, block);
      } else {
        chacha20_xor64_unaligned_u32(out, in, block);
      }
    } else {
      chacha20_xor_partial_u32(out, in, block, n);
    }
#else
    chacha20_block_10dr(
      block, state->chacha_key, (uint32_t)state->chacha_counter, nonce, 0
    );
    for (size_t i = 0; i < 16u; ++i) {
      store32_le_u(stream + i * 4u, block[i]);
    }
    for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(in[i] ^ stream[i]);
#endif
    ++state->chacha_counter;
    nonce[0] = (uint32_t)(state->chacha_counter >> 32);
    out += n;
    in += n;
    len -= n;
  }

  CHACHA_CLEAR(block, sizeof(block));
#if !CHACHA_TARGET_FAST_ARM
  CHACHA_CLEAR(stream, sizeof(stream));
#endif
}

static size_t chacha20_poly1305_update_cipher(
  chacha20_poly1305_state *state,
  const uint8_t *src, size_t len, uint8_t *dst
){
  uint8_t *dst_start = dst;

  if (state->chacha_leftover != 0u) {
    size_t want = 64u - state->chacha_leftover;
    if (want > len) want = len;
    for (size_t i = 0; i < want; ++i) {
      state->chacha_buffer[state->chacha_leftover + i] = src[i];
    }
    src += want;
    len -= want;
    state->chacha_leftover += want;
    if (state->chacha_leftover < 64u) return 0;
    chacha20_poly1305_xor(state, dst, state->chacha_buffer, 64u);
    dst += 64u;
    state->chacha_leftover = 0;
  }

  if (len >= 64u) {
    const size_t blocks_len = len & ~(size_t)63u;
    chacha20_poly1305_xor(state, dst, src, blocks_len);
    src += blocks_len;
    dst += blocks_len;
    len -= blocks_len;
  }

  for (size_t i = 0; i < len; ++i) state->chacha_buffer[i] = src[i];
  state->chacha_leftover = len;
  return (size_t)(dst - dst_start);
}

static void chacha20_poly1305_init_64x64_software(
  chacha20_poly1305_state *state,
  const uint8_t key[32], const uint8_t nonce[8]
){
  uint8_t zeros[64] = {0};
  uint8_t first_block[64];

  memset(state, 0, sizeof(*state));
  for (size_t i = 0; i < 8u; ++i) {
    state->chacha_key[i] = load32_le_u(key + i * 4u);
  }
  state->chacha_nonce[0] = load32_le_u(nonce + 0);
  state->chacha_nonce[1] = load32_le_u(nonce + 4);
  state->chacha_counter = 0;
  chacha20_poly1305_xor(state, first_block, zeros, sizeof(first_block));
  poly1305_init_state(state, first_block);
  CHACHA_CLEAR(first_block, sizeof(first_block));
}

static void chacha20_poly1305_add_aad_software(
  chacha20_poly1305_state *state, const void *src, size_t len
){
  poly1305_update_state(state, (const uint8_t *)src, len);
  state->aad_len += (uint64_t)len;
}

static void chacha20_poly1305_pad_aad(chacha20_poly1305_state *state) {
  static const uint8_t zeros[16] = {0};
  if (!state->aad_padded) {
    const size_t rem = (size_t)(state->aad_len & 15u);
    if (rem != 0u) poly1305_update_state(state, zeros, 16u - rem);
    state->aad_padded = 1u;
  }
}

static size_t chacha20_poly1305_encrypt_software(
  chacha20_poly1305_state *state,
  const void *src, size_t len, void *dst
){
  size_t written;
  chacha20_poly1305_pad_aad(state);
  written = chacha20_poly1305_update_cipher(
    state, (const uint8_t *)src, len, (uint8_t *)dst
  );
  if (written != 0u) {
    poly1305_update_state(state, (const uint8_t *)dst, written);
    state->data_len += (uint64_t)written;
  }
  return written;
}

static CHACHA_UNUSED size_t chacha20_poly1305_decrypt_software(
  chacha20_poly1305_state *state,
  const void *src, size_t len, void *dst
){
  size_t written;

  chacha20_poly1305_pad_aad(state);
  poly1305_update_state(state, (const uint8_t *)src, len);
  written = chacha20_poly1305_update_cipher(
    state, (const uint8_t *)src, len, (uint8_t *)dst
  );
  state->data_len += (uint64_t)written;
  return written;
}

static void chacha20_poly1305_finish_tag(
  chacha20_poly1305_state *state, uint8_t tag[16]
){
  static const uint8_t zeros[16] = {0};
  uint8_t lengths[16];

  chacha20_poly1305_pad_aad(state);
  {
    const size_t rem = (size_t)(state->data_len & 15u);
    if (rem != 0u) poly1305_update_state(state, zeros, 16u - rem);
  }
  store64_le_u(lengths + 0, state->aad_len);
  store64_le_u(lengths + 8, state->data_len);
  poly1305_update_state(state, lengths, sizeof(lengths));
  poly1305_final_state(state, tag);

  CHACHA_CLEAR(lengths, sizeof(lengths));
}

static int chacha20_poly1305_tag_equal(
  const uint8_t a[16], const uint8_t b[16]
){
  uint32_t diff = 0;
  for (size_t i = 0; i < 16u; ++i) diff |= (uint32_t)(a[i] ^ b[i]);
  return diff == 0u;
}

static size_t chacha20_poly1305_final_software(
  chacha20_poly1305_state *state, void *dst, uint8_t tag[16]
){
  const size_t written = state->chacha_leftover;

  chacha20_poly1305_pad_aad(state);
  if (written != 0u) {
    chacha20_poly1305_xor(
      state, (uint8_t *)dst, state->chacha_buffer, written
    );
    poly1305_update_state(state, (const uint8_t *)dst, written);
    state->data_len += (uint64_t)written;
    state->chacha_leftover = 0;
  }
  chacha20_poly1305_finish_tag(state, tag);

  CHACHA_CLEAR(state, sizeof(*state));
  return written;
}

static CHACHA_UNUSED size_t chacha20_poly1305_verify_software(
  chacha20_poly1305_state *state, void *dst,
  const uint8_t tag[16], int32_t *out_error
){
  uint8_t calculated[16];
  const size_t written = state->chacha_leftover;

  chacha20_poly1305_pad_aad(state);
  if (written != 0u) {
    chacha20_poly1305_xor(
      state, (uint8_t *)dst, state->chacha_buffer, written
    );
    state->data_len += (uint64_t)written;
    state->chacha_leftover = 0;
  }
  chacha20_poly1305_finish_tag(state, calculated);
  *out_error = chacha20_poly1305_tag_equal(calculated, tag) ? 0 : -1;

  CHACHA_CLEAR(calculated, sizeof(calculated));
  CHACHA_CLEAR(state, sizeof(*state));
  return written;
}

static CHACHA_UNUSED void chacha20_poly1305_encrypt_all_64x64_software(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *plaintext, size_t plaintext_len,
  void *ciphertext, uint8_t tag[16]
){
  chacha20_poly1305_state state;
  size_t written;

  chacha20_poly1305_init_64x64_software(&state, key, nonce);
  chacha20_poly1305_add_aad_software(&state, aad, aad_len);
  written = chacha20_poly1305_encrypt_software(
    &state, plaintext, plaintext_len, ciphertext
  );
  (void)chacha20_poly1305_final_software(
    &state, (uint8_t *)ciphertext + written, tag
  );
}

static CHACHA_UNUSED int32_t chacha20_poly1305_decrypt_all_64x64_software(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *ciphertext, size_t ciphertext_len,
  void *plaintext, const uint8_t tag[16]
){
  chacha20_poly1305_state state;
  uint8_t calculated[16];
  int32_t result;

  chacha20_poly1305_init_64x64_software(&state, key, nonce);
  chacha20_poly1305_add_aad_software(&state, aad, aad_len);
  chacha20_poly1305_pad_aad(&state);
  poly1305_update_state(&state, (const uint8_t *)ciphertext, ciphertext_len);
  state.data_len = (uint64_t)ciphertext_len;
  chacha20_poly1305_finish_tag(&state, calculated);
  result = chacha20_poly1305_tag_equal(calculated, tag) ? 0 : -1;
  if (result == 0) {
    chacha20_poly1305_xor(
      &state, (uint8_t *)plaintext,
      (const uint8_t *)ciphertext, ciphertext_len
    );
  }

  CHACHA_CLEAR(calculated, sizeof(calculated));
  CHACHA_CLEAR(&state, sizeof(state));
  return result;
}

/* ---------------- CarPlay backend selection ---------------- */

#if UINTPTR_MAX <= UINT32_MAX
typedef char chacha20_poly1305_state_must_fit_carplay_abi[
  (sizeof(chacha20_poly1305_state) <= 0x118u) ? 1 : -1
];
#endif

enum {
  CHACHA_RTL_DIRECTION_NONE = 0,
  CHACHA_RTL_DIRECTION_ENCRYPT = 1,
  CHACHA_RTL_DIRECTION_DECRYPT = 2
};

enum {
  CHACHA_HW_BACKEND_NONE = 0,
  CHACHA_HW_BACKEND_COMBINED = 1,
  CHACHA_HW_BACKEND_STANDALONE = 2,
  CHACHA_HW_BACKEND_CHUNKED = 3
};

static volatile int g_chacha_mode_printed;
static volatile uint32_t g_chacha_backend_printed;
static CHACHA_UNUSED uint32_t g_chacha_verify_operations;
static CHACHA_UNUSED uint32_t g_chacha_verify_mismatches;
static CHACHA_UNUSED uint32_t g_chacha_verify_skipped;
static CHACHA_UNUSED uint32_t g_chacha_hw_operations;
static CHACHA_UNUSED uint32_t g_chacha_hw_fallbacks;

static void chacha_copy_bytes(void *dst_arg, const void *src_arg, size_t len) {
  uint8_t *dst = (uint8_t *)dst_arg;
  const uint8_t *src = (const uint8_t *)src_arg;
  size_t i;
  for (i = 0; i < len; ++i) dst[i] = src[i];
}

static CHACHA_UNUSED size_t chacha_first_difference(
  const uint8_t *a, const uint8_t *b, size_t len
) {
  size_t i;
  for (i = 0; i < len; ++i) {
    if (a[i] != b[i]) return i;
  }
  return len;
}

static int chacha_should_log(uint32_t count) {
  return (count <= 8u) || ((count & (count - 1u)) == 0u);
}

static void chacha_announce_mode(void) {
  if (__sync_bool_compare_and_swap(&g_chacha_mode_printed, 0, 1)) {
#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_SOFTWARE_ONLY
    printf("[CHACHA] mode=SOFTWARE_ONLY\n");
#elif CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_SOFTWARE_HW_VERIFY
    printf(
      "[CHACHA] mode=SOFTWARE_HW_VERIFY "
      "(software authoritative, hardware not board-validated)\n"
    );
#elif CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_HARDWARE_ONLY
    printf(
      "[CHACHA] mode=HARDWARE_ONLY "
      "(software fallback enabled, hardware not board-validated)\n"
    );
#else
#error "Unsupported CARBOX_CHACHA_MODE"
#endif
  }
}

static void chacha_export_key_nonce(
  const chacha20_poly1305_state *state,
  uint8_t key[32], uint8_t nonce[8]
) {
  size_t i;
  for (i = 0; i < 8u; ++i) {
    store32_le_u(key + (i * 4u), state->chacha_key[i]);
  }
  store32_le_u(nonce + 0, state->chacha_nonce[0]);
  store32_le_u(nonce + 4, state->chacha_nonce[1]);
}

static void chacha_record_aad(
  chacha20_poly1305_state *state, const void *src_arg, size_t len
) {
  const uint8_t *src = (const uint8_t *)src_arg;

  if (len == 0u) return;
#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_SOFTWARE_ONLY
  (void)state;
  (void)src;
#else
  if (!state->rtl_eligible) return;
  if (chacha_rtl8195b_precheck_context() != CHACHA_RTL_OK) {
    state->rtl_eligible = 0u;
    state->rtl_reserved = (uint8_t)CHACHA_RTL_SKIP_INTERRUPT;
    return;
  }
  if (state->rtl_aad_len > SIZE_MAX - len) {
    state->rtl_eligible = 0u;
    state->rtl_reserved = (uint8_t)CHACHA_RTL_SKIP_LENGTH;
    return;
  }
  {
    const size_t old_len = state->rtl_aad_len;
    const size_t new_len = old_len + len;
    uint8_t *snapshot =
      (uint8_t *)carbox_chacha_aad_realloc(state->rtl_aad, new_len);

    if (!snapshot) {
      state->rtl_eligible = 0u;
      state->rtl_reserved = (uint8_t)CHACHA_RTL_SKIP_MEMORY;
      return;
    }
    chacha_copy_bytes(snapshot + old_len, src, len);
    state->rtl_aad = snapshot;
    state->rtl_aad_len = new_len;
    state->rtl_aad_seen = 1u;
  }
#endif
}

static void chacha_record_io_start(
  chacha20_poly1305_state *state, int direction,
  const void *src_arg, size_t len, void *dst_arg
) {
  const uint8_t *src = (const uint8_t *)src_arg;
  uint8_t *dst = (uint8_t *)dst_arg;

#if CARBOX_CHACHA_MODE != CARBOX_CHACHA_MODE_SOFTWARE_ONLY
  /*
   * A zero-length AAD call does not pass through chacha_record_aad(). Check
   * every data update before Mode 1 can allocate a decrypt snapshot or Mode 2
   * can retain caller buffers for a later hardware transaction.
   */
  if (state->rtl_eligible &&
      (chacha_rtl8195b_precheck_context() != CHACHA_RTL_OK)) {
    state->rtl_eligible = 0u;
    state->rtl_reserved = (uint8_t)CHACHA_RTL_SKIP_INTERRUPT;
  }
#endif

  if (state->rtl_direction == CHACHA_RTL_DIRECTION_NONE) {
    state->rtl_direction = (uint8_t)direction;
    state->rtl_output_base = dst;
    state->rtl_output_next = dst;
    state->rtl_input_base = src;
    state->rtl_input_next = src;
  } else if (state->rtl_direction != (uint8_t)direction) {
    state->rtl_eligible = 0u;
  }

  if (dst != state->rtl_output_next) state->rtl_eligible = 0u;
  if (direction == CHACHA_RTL_DIRECTION_DECRYPT) {
    if (src != state->rtl_input_next) state->rtl_eligible = 0u;
    state->rtl_input_next = src + len;
#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_SOFTWARE_HW_VERIFY
    if (state->rtl_eligible && (len != 0u)) {
      const size_t old_len = state->rtl_input_snapshot_len;
      const size_t new_len = old_len + len;
      uint8_t *snapshot = NULL;

      if (new_len >= old_len) {
        snapshot = (uint8_t *)realloc(state->rtl_input_snapshot, new_len);
      }
      if (snapshot != NULL) {
        chacha_copy_bytes(snapshot + old_len, src, len);
        state->rtl_input_snapshot = snapshot;
        state->rtl_input_snapshot_len = new_len;
      } else {
        state->rtl_eligible = 0u;
        state->rtl_reserved = (uint8_t)CHACHA_RTL_SKIP_MEMORY;
      }
    }
#endif
  }
}

static CHACHA_UNUSED size_t chacha_deferred_copy(
  chacha20_poly1305_state *state,
  const uint8_t *src, size_t len, uint8_t *dst
) {
  uint8_t *dst_start = dst;

  if (state->chacha_leftover != 0u) {
    size_t want = 64u - state->chacha_leftover;
    size_t i;
    if (want > len) want = len;
    for (i = 0; i < want; ++i) {
      state->chacha_buffer[state->chacha_leftover + i] = src[i];
    }
    src += want;
    len -= want;
    state->chacha_leftover += want;
    if (state->chacha_leftover < 64u) return 0u;
    chacha_copy_bytes(dst, state->chacha_buffer, 64u);
    dst += 64u;
    state->chacha_leftover = 0u;
  }

  if (len >= 64u) {
    const size_t blocks_len = len & ~(size_t)63u;
    chacha_copy_bytes(dst, src, blocks_len);
    src += blocks_len;
    dst += blocks_len;
    len -= blocks_len;
  }

  if (len != 0u) {
    chacha_copy_bytes(state->chacha_buffer, src, len);
  }
  state->chacha_leftover = len;
  return (size_t)(dst - dst_start);
}

static CHACHA_UNUSED void chacha_log_skip(
                            const char *kind, int status, size_t len,
                            size_t aad_len, uint32_t count) {
  if (chacha_should_log(count)) {
    printf(
      "[CHACHA][%s] %s: reason=%s len=%lu aad_len=%lu count=%lu\n",
      kind, (kind[0] == 'V') ? "skipped" : "software fallback",
      chacha_rtl8195b_status_string(status),
      (unsigned long)len, (unsigned long)aad_len, (unsigned long)count
    );
  }
}

static CHACHA_UNUSED int chacha_hardware_precheck(
  size_t len, size_t aad_len
) {
  if (len < (size_t)CARBOX_CHACHA_HW_MIN_LEN) {
    return CHACHA_RTL_SKIP_THRESHOLD;
  }
  if (len == 0u) {
    return CHACHA_RTL_SKIP_LENGTH;
  }
  {
    int context_status = chacha_rtl8195b_precheck_context();
    if (context_status != CHACHA_RTL_OK) return context_status;
  }
  (void)aad_len;
  return CHACHA_RTL_OK;
}

static CHACHA_UNUSED int chacha_round_up_16(
  size_t value, size_t *rounded
) {
  if (value > SIZE_MAX - 15u) return CHACHA_RTL_SKIP_LENGTH;
  *rounded = (value + 15u) & ~(size_t)15u;
  return CHACHA_RTL_OK;
}

static CHACHA_UNUSED int chacha_poly_input_size(
  size_t aad_len, size_t data_len, size_t *poly_input_len
) {
  size_t aad_padded;
  size_t data_padded;
  int status;

  status = chacha_round_up_16(aad_len, &aad_padded);
  if (status != CHACHA_RTL_OK) return status;
  status = chacha_round_up_16(data_len, &data_padded);
  if (status != CHACHA_RTL_OK) return status;
  if (aad_padded > SIZE_MAX - data_padded ||
      aad_padded + data_padded > SIZE_MAX - 16u) {
    return CHACHA_RTL_SKIP_POLY_LENGTH;
  }
  *poly_input_len = aad_padded + data_padded + 16u;
  if (*poly_input_len > 65536u) {
    return CHACHA_RTL_SKIP_POLY_LENGTH;
  }
  return CHACHA_RTL_OK;
}

static CHACHA_UNUSED int chacha_select_hardware_backend(
  size_t len, size_t aad_len, int *backend
) {
  size_t poly_input_len;
  int status = chacha_hardware_precheck(len, aad_len);

  *backend = CHACHA_HW_BACKEND_NONE;
  if (status != CHACHA_RTL_OK) return status;

  if ((len <= 65536u) && ((len & 15u) == 0u) && (aad_len <= 496u)) {
    *backend = CHACHA_HW_BACKEND_COMBINED;
    return CHACHA_RTL_OK;
  }
  if (len <= 65536u &&
      chacha_poly_input_size(aad_len, len, &poly_input_len) ==
        CHACHA_RTL_OK) {
    *backend = CHACHA_HW_BACKEND_STANDALONE;
    return CHACHA_RTL_OK;
  }
  *backend = CHACHA_HW_BACKEND_CHUNKED;
  return CHACHA_RTL_OK;
}

static CHACHA_UNUSED void chacha_announce_backend(int backend) {
  uint32_t bit;
  const char *name;

  if (backend <= CHACHA_HW_BACKEND_NONE ||
      backend > CHACHA_HW_BACKEND_CHUNKED) return;
  bit = 1u << (unsigned int)backend;
  if ((__sync_fetch_and_or(&g_chacha_backend_printed, bit) & bit) != 0u) {
    return;
  }
  if (backend == CHACHA_HW_BACKEND_COMBINED) {
    name = "combined-chacha-poly1305";
  } else if (backend == CHACHA_HW_BACKEND_STANDALONE) {
    name = "standalone-chacha+poly1305";
  } else {
    name = "chunked-chacha+software-poly1305";
  }
  printf(
    "[CHACHA][HW] backend=%s min_len=%lu\n",
    name, (unsigned long)CARBOX_CHACHA_HW_MIN_LEN
  );
}

static CHACHA_UNUSED void chacha_auth_software(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const uint8_t *ciphertext, size_t ciphertext_len,
  uint8_t tag[16]
) {
  chacha20_poly1305_state state;

  chacha20_poly1305_init_64x64_software(&state, key, nonce);
  chacha20_poly1305_add_aad_software(&state, aad, aad_len);
  chacha20_poly1305_pad_aad(&state);
  poly1305_update_state(&state, ciphertext, ciphertext_len);
  state.data_len = (uint64_t)ciphertext_len;
  chacha20_poly1305_finish_tag(&state, tag);
  CHACHA_CLEAR(&state, sizeof(state));
}

#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_HARDWARE_ONLY
/*
 * Mode 2 defers payload processing so the final record length can select a HW
 * backend. If HW is ineligible or safely recovered from an error, finish from
 * the already-initialized software state. This preserves every AAD update and
 * does not depend on the caller keeping the original AAD buffer alive.
 */
static size_t chacha_mode2_finish_encrypt_software(
  chacha20_poly1305_state *state, uint8_t *buffer, size_t len,
  uint8_t tag[16]
) {
  size_t written;

  state->data_len = 0u;
  state->chacha_leftover = 0u;
  written = chacha20_poly1305_encrypt_software(
    state, buffer, len, buffer
  );
  written += chacha20_poly1305_final_software(
    state, buffer + written, tag
  );
  return written;
}

static size_t chacha_mode2_finish_decrypt_software(
  chacha20_poly1305_state *state, uint8_t *buffer, size_t len,
  const uint8_t tag[16], int32_t *out_error
) {
  size_t written;

  state->data_len = 0u;
  state->chacha_leftover = 0u;
  written = chacha20_poly1305_decrypt_software(
    state, buffer, len, buffer
  );
  written += chacha20_poly1305_verify_software(
    state, buffer + written, tag, out_error
  );
  return written;
}
#endif

static CHACHA_UNUSED int chacha_hardware_poly_key(
  const uint8_t key[32], const uint8_t nonce[8],
  uint8_t poly_key[32]
) {
  uint8_t zeros[32] = {0};
  int status = chacha_rtl8195b_chacha_xor(
    key, nonce, 0u, zeros, sizeof(zeros), poly_key
  );
  CHACHA_CLEAR(zeros, sizeof(zeros));
  return status;
}

static CHACHA_UNUSED int chacha_hardware_xor_padded(
  const uint8_t key[32], const uint8_t nonce[8], uint32_t counter,
  const uint8_t *input, size_t len, uint8_t *output
) {
  uint8_t *padded_input;
  uint8_t *padded_output;
  size_t padded_len;
  int status;

  status = chacha_round_up_16(len, &padded_len);
  if (status != CHACHA_RTL_OK || padded_len == 0u ||
      padded_len > 65536u) {
    return CHACHA_RTL_SKIP_LENGTH;
  }
  if (padded_len == len) {
    return chacha_rtl8195b_chacha_xor(
      key, nonce, counter, input, len, output
    );
  }

  padded_input = (uint8_t *)malloc(padded_len);
  padded_output = (uint8_t *)malloc(padded_len);
  if (!padded_input || !padded_output) {
    if (padded_input) free(padded_input);
    if (padded_output) free(padded_output);
    return CHACHA_RTL_SKIP_MEMORY;
  }
  chacha_copy_bytes(padded_input, input, len);
  memset(padded_input + len, 0, padded_len - len);
  status = chacha_rtl8195b_chacha_xor(
    key, nonce, counter, padded_input, padded_len, padded_output
  );
  if (status == CHACHA_RTL_OK) {
    chacha_copy_bytes(output, padded_output, len);
  }
  CHACHA_CLEAR(padded_input, padded_len);
  CHACHA_CLEAR(padded_output, padded_len);
  free(padded_output);
  free(padded_input);
  return status;
}

static CHACHA_UNUSED int chacha_hardware_xor_chunks(
  const uint8_t key[32], const uint8_t nonce[8],
  const uint8_t *input, size_t len, uint8_t *output
) {
  size_t offset = 0u;
  uint32_t counter = 1u;

  while (offset < len) {
    size_t chunk_len = len - offset;
    uint32_t blocks;
    int status;

    if (chunk_len > 65536u) chunk_len = 65536u;
    status = chacha_hardware_xor_padded(
      key, nonce, counter, input + offset, chunk_len, output + offset
    );
    if (status != CHACHA_RTL_OK) return status;

    blocks = (uint32_t)((chunk_len + 63u) / 64u);
    if (counter > UINT32_MAX - blocks && offset + chunk_len < len) {
      return CHACHA_RTL_SKIP_LENGTH;
    }
    counter += blocks;
    offset += chunk_len;
  }
  return CHACHA_RTL_OK;
}

static CHACHA_UNUSED int chacha_build_poly_input(
  const void *aad, size_t aad_len,
  const uint8_t *ciphertext, size_t ciphertext_len,
  uint8_t **poly_input, size_t *poly_input_len
) {
  uint8_t *buffer;
  size_t aad_padded;
  size_t data_padded;
  int status = chacha_poly_input_size(
    aad_len, ciphertext_len, poly_input_len
  );

  if (status != CHACHA_RTL_OK) return status;
  (void)chacha_round_up_16(aad_len, &aad_padded);
  (void)chacha_round_up_16(ciphertext_len, &data_padded);

  buffer = (uint8_t *)malloc(*poly_input_len);
  if (!buffer) return CHACHA_RTL_SKIP_MEMORY;
  memset(buffer, 0, *poly_input_len);
  if (aad_len != 0u) chacha_copy_bytes(buffer, aad, aad_len);
  chacha_copy_bytes(buffer + aad_padded, ciphertext, ciphertext_len);
  store64_le_u(buffer + aad_padded + data_padded, (uint64_t)aad_len);
  store64_le_u(
    buffer + aad_padded + data_padded + 8u, (uint64_t)ciphertext_len
  );
  *poly_input = buffer;
  return CHACHA_RTL_OK;
}

static CHACHA_UNUSED int chacha_hardware_auth_standalone(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const uint8_t *ciphertext, size_t ciphertext_len,
  uint8_t tag[16]
) {
  uint8_t poly_key[32];
  uint8_t *poly_input = NULL;
  size_t poly_input_len = 0u;
  int status = chacha_hardware_poly_key(key, nonce, poly_key);

  if (status == CHACHA_RTL_OK) {
    status = chacha_build_poly_input(
      aad, aad_len, ciphertext, ciphertext_len,
      &poly_input, &poly_input_len
    );
  }
  if (status == CHACHA_RTL_OK) {
    status = chacha_rtl8195b_poly1305(
      poly_key, poly_input, poly_input_len, tag
    );
  }
  if (poly_input) {
    CHACHA_CLEAR(poly_input, poly_input_len);
    free(poly_input);
  }
  chacha_secure_clear(poly_key, sizeof(poly_key));
  return status;
}

static CHACHA_UNUSED int chacha_hardware_encrypt_auto(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const uint8_t *plaintext, size_t len,
  uint8_t *ciphertext, uint8_t tag[16], int *backend
) {
  int status = chacha_select_hardware_backend(len, aad_len, backend);

  if (status != CHACHA_RTL_OK) return status;
  chacha_announce_backend(*backend);
  if (*backend == CHACHA_HW_BACKEND_COMBINED) {
    return chacha_rtl8195b_encrypt(
      key, nonce, aad, aad_len, plaintext, len, ciphertext, tag
    );
  }

  status = chacha_hardware_xor_chunks(
    key, nonce, plaintext, len, ciphertext
  );
  if (status != CHACHA_RTL_OK) return status;
  if (*backend == CHACHA_HW_BACKEND_STANDALONE) {
    return chacha_hardware_auth_standalone(
      key, nonce, aad, aad_len, ciphertext, len, tag
    );
  }
  chacha_auth_software(
    key, nonce, aad, aad_len, ciphertext, len, tag
  );
  return CHACHA_RTL_OK;
}

static CHACHA_UNUSED int chacha_hardware_decrypt_auto(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const uint8_t *ciphertext, size_t len,
  uint8_t *plaintext, uint8_t calculated_tag[16], int *backend
) {
  int status = chacha_select_hardware_backend(len, aad_len, backend);

  if (status != CHACHA_RTL_OK) return status;
  chacha_announce_backend(*backend);
  if (*backend == CHACHA_HW_BACKEND_COMBINED) {
    return chacha_rtl8195b_decrypt(
      key, nonce, aad, aad_len,
      ciphertext, len, plaintext, calculated_tag
    );
  }

  if (*backend == CHACHA_HW_BACKEND_STANDALONE) {
    status = chacha_hardware_auth_standalone(
      key, nonce, aad, aad_len, ciphertext, len, calculated_tag
    );
  } else {
    chacha_auth_software(
      key, nonce, aad, aad_len, ciphertext, len, calculated_tag
    );
    status = CHACHA_RTL_OK;
  }
  if (status != CHACHA_RTL_OK) return status;
  return chacha_hardware_xor_chunks(
    key, nonce, ciphertext, len, plaintext
  );
}

#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_SOFTWARE_HW_VERIFY
static void chacha_verify_encrypt_result(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const uint8_t *software_ciphertext, size_t len,
  const uint8_t software_tag[16], int eligible
) {
  uint8_t nonce12[12];
  uint8_t hardware_tag[16];
  uint8_t *plaintext;
  uint8_t *hardware_ciphertext;
  size_t first_diff;
  size_t tag_diff;
  int backend = CHACHA_HW_BACKEND_NONE;
  int status;

  ++g_chacha_verify_operations;
  if (len < (size_t)CARBOX_CHACHA_HW_MIN_LEN) {
    ++g_chacha_verify_skipped;
    return;
  }
  if (!eligible) {
    ++g_chacha_verify_skipped;
    chacha_log_skip(
      "VERIFY", CHACHA_RTL_SKIP_LAYOUT, len, aad_len,
      g_chacha_verify_skipped
    );
    return;
  }
  status = chacha_hardware_precheck(len, aad_len);
  if (status != CHACHA_RTL_OK) {
    ++g_chacha_verify_skipped;
    if (status != CHACHA_RTL_SKIP_THRESHOLD) {
      chacha_log_skip(
        "VERIFY", status, len, aad_len, g_chacha_verify_skipped
      );
    }
    return;
  }

  plaintext = (uint8_t *)malloc(len);
  hardware_ciphertext = (uint8_t *)malloc(len);
  if (!plaintext || !hardware_ciphertext) {
    if (plaintext) free(plaintext);
    if (hardware_ciphertext) free(hardware_ciphertext);
    ++g_chacha_verify_skipped;
    chacha_log_skip(
      "VERIFY", CHACHA_RTL_SKIP_MEMORY, len, aad_len,
      g_chacha_verify_skipped
    );
    return;
  }

  memset(nonce12, 0, sizeof(nonce12));
  chacha_copy_bytes(nonce12 + 4, nonce, 8);
  chacha20_xor(
    plaintext, software_ciphertext, len, key, nonce12, 1u
  );
  status = chacha_hardware_encrypt_auto(
    key, nonce, aad, aad_len, plaintext, len,
    hardware_ciphertext, hardware_tag, &backend
  );
  if (status == CHACHA_RTL_OK) {
    first_diff = chacha_first_difference(
      software_ciphertext, hardware_ciphertext, len
    );
    tag_diff = chacha_first_difference(software_tag, hardware_tag, 16u);
    if ((first_diff != len) || (tag_diff != 16u)) {
      ++g_chacha_verify_mismatches;
      printf(
        "[CHACHA][VERIFY][MISMATCH] encrypt len=%lu aad_len=%lu "
        "data_diff=%ld tag_diff=%ld count=%lu\n",
        (unsigned long)len, (unsigned long)aad_len,
        (long)((first_diff == len) ? -1 : (long)first_diff),
        (long)((tag_diff == 16u) ? -1 : (long)tag_diff),
        (unsigned long)g_chacha_verify_mismatches
      );
    }
  } else {
    ++g_chacha_verify_skipped;
    chacha_log_skip(
      "VERIFY", status, len, aad_len, g_chacha_verify_skipped
    );
  }
  free(hardware_ciphertext);
  free(plaintext);
}

static void chacha_verify_decrypt_result(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const uint8_t *ciphertext, const uint8_t *software_plaintext,
  size_t len, const uint8_t supplied_tag[16],
  int32_t software_error, int eligible, int ineligible_status
) {
  uint8_t hardware_tag[16];
  uint8_t *hardware_plaintext;
  size_t first_diff;
  int hardware_ok;
  int software_ok;
  int backend = CHACHA_HW_BACKEND_NONE;
  int status;

  ++g_chacha_verify_operations;
  if (len < (size_t)CARBOX_CHACHA_HW_MIN_LEN) {
    ++g_chacha_verify_skipped;
    return;
  }
  if (!eligible) {
    ++g_chacha_verify_skipped;
    chacha_log_skip(
      "VERIFY", ineligible_status, len, aad_len,
      g_chacha_verify_skipped
    );
    return;
  }
  status = chacha_hardware_precheck(len, aad_len);
  if (status != CHACHA_RTL_OK) {
    ++g_chacha_verify_skipped;
    if (status != CHACHA_RTL_SKIP_THRESHOLD) {
      chacha_log_skip(
        "VERIFY", status, len, aad_len, g_chacha_verify_skipped
      );
    }
    return;
  }

  hardware_plaintext = (uint8_t *)malloc(len);
  if (!hardware_plaintext) {
    ++g_chacha_verify_skipped;
    chacha_log_skip(
      "VERIFY", CHACHA_RTL_SKIP_MEMORY, len, aad_len,
      g_chacha_verify_skipped
    );
    return;
  }

  status = chacha_hardware_decrypt_auto(
    key, nonce, aad, aad_len, ciphertext, len,
    hardware_plaintext, hardware_tag, &backend
  );
  if (status == CHACHA_RTL_OK) {
    first_diff = chacha_first_difference(
      software_plaintext, hardware_plaintext, len
    );
    hardware_ok = chacha20_poly1305_tag_equal(hardware_tag, supplied_tag);
    software_ok = (software_error == 0);
    if ((first_diff != len) || (hardware_ok != software_ok)) {
      ++g_chacha_verify_mismatches;
      printf(
        "[CHACHA][VERIFY][MISMATCH] decrypt len=%lu aad_len=%lu "
        "data_diff=%ld sw_auth=%d hw_auth=%d count=%lu\n",
        (unsigned long)len, (unsigned long)aad_len,
        (long)((first_diff == len) ? -1 : (long)first_diff),
        software_ok, hardware_ok,
        (unsigned long)g_chacha_verify_mismatches
      );
    }
  } else {
    ++g_chacha_verify_skipped;
    chacha_log_skip(
      "VERIFY", status, len, aad_len, g_chacha_verify_skipped
    );
  }
  free(hardware_plaintext);
}
#endif

void chacha20_poly1305_init_64x64(
  chacha20_poly1305_state *state,
  const uint8_t key[32], const uint8_t nonce[8]
) {
  chacha_announce_mode();
  chacha20_poly1305_init_64x64_software(state, key, nonce);
  state->rtl_eligible = 1u;
}

void chacha20_poly1305_add_aad(
  chacha20_poly1305_state *state, const void *src, size_t len
) {
  chacha_record_aad(state, src, len);
  chacha20_poly1305_add_aad_software(state, src, len);
}

size_t chacha20_poly1305_encrypt(
  chacha20_poly1305_state *state,
  const void *src, size_t len, void *dst
) {
  size_t written;

  chacha_record_io_start(
    state, CHACHA_RTL_DIRECTION_ENCRYPT, src, len, dst
  );
#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_HARDWARE_ONLY
  written = chacha_deferred_copy(
    state, (const uint8_t *)src, len, (uint8_t *)dst
  );
  state->data_len += (uint64_t)len;
#else
  written = chacha20_poly1305_encrypt_software(state, src, len, dst);
#endif
  state->rtl_output_next += written;
  return written;
}

size_t chacha20_poly1305_decrypt(
  chacha20_poly1305_state *state,
  const void *src, size_t len, void *dst
) {
  size_t written;

  chacha_record_io_start(
    state, CHACHA_RTL_DIRECTION_DECRYPT, src, len, dst
  );
#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_HARDWARE_ONLY
  written = chacha_deferred_copy(
    state, (const uint8_t *)src, len, (uint8_t *)dst
  );
  state->data_len += (uint64_t)len;
#else
  written = chacha20_poly1305_decrypt_software(state, src, len, dst);
#endif
  state->rtl_output_next += written;
  return written;
}

size_t chacha20_poly1305_final(
  chacha20_poly1305_state *state, void *dst, uint8_t tag[16]
) {
  uint8_t key[32];
  uint8_t nonce[8];
  const uint8_t *aad = state->rtl_aad;
  size_t aad_len = state->rtl_aad_len;
  uint8_t *aad_snapshot = state->rtl_aad;
  uint8_t *input_snapshot = state->rtl_input_snapshot;
  uint8_t *output_base = state->rtl_output_base;
  size_t total_len;
  size_t written;
  int eligible = state->rtl_eligible &&
                 (state->rtl_direction == CHACHA_RTL_DIRECTION_ENCRYPT);

  /*
   * If the private AAD snapshot failed, hardware is already ineligible. Use
   * the software accumulator for diagnostics so the log still reports the
   * caller's complete AAD length.
   */
  if (!eligible) aad_len = (size_t)state->aad_len;

#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_SOFTWARE_ONLY
  (void)aad;
  (void)aad_len;
  (void)output_base;
  (void)input_snapshot;
  (void)total_len;
  (void)eligible;
#endif
  chacha_export_key_nonce(state, key, nonce);
#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_HARDWARE_ONLY
  total_len = (size_t)state->data_len;
  written = state->chacha_leftover;
  if ((uint8_t *)dst != state->rtl_output_next) eligible = 0;
  if (written != 0u) {
    chacha_copy_bytes(dst, state->chacha_buffer, written);
    state->rtl_output_next += written;
    state->chacha_leftover = 0u;
  }
  if (eligible) {
    int backend = CHACHA_HW_BACKEND_NONE;
    int status = chacha_hardware_precheck(total_len, aad_len);
    uint8_t *hardware_output =
      (status == CHACHA_RTL_OK) ? (uint8_t *)malloc(total_len) : NULL;
    if ((status == CHACHA_RTL_OK) && !hardware_output) {
      status = CHACHA_RTL_SKIP_MEMORY;
    }
    if (status == CHACHA_RTL_OK) {
      status = chacha_hardware_encrypt_auto(
        key, nonce, aad, aad_len,
        output_base, total_len, hardware_output, tag, &backend
      );
    }
    if (status == CHACHA_RTL_OK) {
      chacha_copy_bytes(output_base, hardware_output, total_len);
      ++g_chacha_hw_operations;
    } else {
      if (status != CHACHA_RTL_SKIP_THRESHOLD) {
        ++g_chacha_hw_fallbacks;
        chacha_log_skip(
          "HW", status, total_len, aad_len,
          g_chacha_hw_fallbacks
        );
      }
      (void)chacha_mode2_finish_encrypt_software(
        state, output_base, total_len, tag
      );
    }
    if (hardware_output) free(hardware_output);
  } else {
    if (total_len >= (size_t)CARBOX_CHACHA_HW_MIN_LEN) {
      int fallback_reason = state->rtl_reserved ?
                            state->rtl_reserved : CHACHA_RTL_SKIP_LAYOUT;
      ++g_chacha_hw_fallbacks;
      chacha_log_skip(
        "HW", fallback_reason, total_len, aad_len,
        g_chacha_hw_fallbacks
      );
    }
    (void)chacha_mode2_finish_encrypt_software(
      state, output_base, total_len, tag
    );
  }
  memset(state, 0, sizeof(*state));
#else
  total_len = (size_t)(state->data_len + state->chacha_leftover);
  if ((uint8_t *)dst != state->rtl_output_next) eligible = 0;
  written = chacha20_poly1305_final_software(state, dst, tag);
#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_SOFTWARE_HW_VERIFY
  chacha_verify_encrypt_result(
    key, nonce, aad, aad_len,
    output_base, total_len, tag, eligible
  );
#endif
#endif
  chacha_clear_state_key_material(state);
  chacha_secure_clear(key, sizeof(key));
  chacha_secure_clear(nonce, sizeof(nonce));
  if (input_snapshot) free(input_snapshot);
  if (aad_snapshot) free(aad_snapshot);
  return written;
}

size_t chacha20_poly1305_verify(
  chacha20_poly1305_state *state, void *dst,
  const uint8_t tag[16], int32_t *out_error
) {
  uint8_t key[32];
  uint8_t nonce[8];
  const uint8_t *aad = state->rtl_aad;
  size_t aad_len = state->rtl_aad_len;
  uint8_t *aad_snapshot = state->rtl_aad;
  uint8_t *input_snapshot = state->rtl_input_snapshot;
#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_SOFTWARE_HW_VERIFY
  size_t input_snapshot_len = state->rtl_input_snapshot_len;
  int ineligible_status = state->rtl_reserved ?
                          state->rtl_reserved : CHACHA_RTL_SKIP_LAYOUT;
#endif
  uint8_t *output_base = state->rtl_output_base;
  size_t total_len;
  size_t written;
  int eligible = state->rtl_eligible &&
                 (state->rtl_direction == CHACHA_RTL_DIRECTION_DECRYPT);

  if (!eligible) aad_len = (size_t)state->aad_len;

#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_SOFTWARE_ONLY
  (void)aad;
  (void)aad_len;
  (void)input_snapshot;
  (void)output_base;
  (void)total_len;
  (void)eligible;
#endif
  chacha_export_key_nonce(state, key, nonce);
#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_HARDWARE_ONLY
  total_len = (size_t)state->data_len;
  written = state->chacha_leftover;
  if ((uint8_t *)dst != state->rtl_output_next) eligible = 0;
  if (written != 0u) {
    chacha_copy_bytes(dst, state->chacha_buffer, written);
    state->rtl_output_next += written;
    state->chacha_leftover = 0u;
  }
  if (eligible) {
    uint8_t calculated[16];
    int backend = CHACHA_HW_BACKEND_NONE;
    int status = chacha_hardware_precheck(total_len, aad_len);
    uint8_t *hardware_output =
      (status == CHACHA_RTL_OK) ? (uint8_t *)malloc(total_len) : NULL;
    if ((status == CHACHA_RTL_OK) && !hardware_output) {
      status = CHACHA_RTL_SKIP_MEMORY;
    }
    if (status == CHACHA_RTL_OK) {
      status = chacha_hardware_decrypt_auto(
        key, nonce, aad, aad_len,
        output_base, total_len, hardware_output, calculated, &backend
      );
    }
    if (status == CHACHA_RTL_OK) {
      *out_error = chacha20_poly1305_tag_equal(calculated, tag) ? 0 : -1;
      if (*out_error == 0) {
        chacha_copy_bytes(output_base, hardware_output, total_len);
      }
      ++g_chacha_hw_operations;
    } else {
      if (status != CHACHA_RTL_SKIP_THRESHOLD) {
        ++g_chacha_hw_fallbacks;
        chacha_log_skip(
          "HW", status, total_len, aad_len,
          g_chacha_hw_fallbacks
        );
      }
      (void)chacha_mode2_finish_decrypt_software(
        state, output_base, total_len, tag, out_error
      );
    }
    if (hardware_output) free(hardware_output);
  } else {
    if (total_len >= (size_t)CARBOX_CHACHA_HW_MIN_LEN) {
      int fallback_reason = state->rtl_reserved ?
                            state->rtl_reserved : CHACHA_RTL_SKIP_LAYOUT;
      ++g_chacha_hw_fallbacks;
      chacha_log_skip(
        "HW", fallback_reason, total_len, aad_len,
        g_chacha_hw_fallbacks
      );
    }
    (void)chacha_mode2_finish_decrypt_software(
      state, output_base, total_len, tag, out_error
    );
  }
  memset(state, 0, sizeof(*state));
#else
  total_len = (size_t)(state->data_len + state->chacha_leftover);
  if ((uint8_t *)dst != state->rtl_output_next) eligible = 0;
#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_SOFTWARE_HW_VERIFY
  if (input_snapshot_len != total_len) eligible = 0;
#endif
  written = chacha20_poly1305_verify_software(
    state, dst, tag, out_error
  );
#if CARBOX_CHACHA_MODE == CARBOX_CHACHA_MODE_SOFTWARE_HW_VERIFY
  chacha_verify_decrypt_result(
    key, nonce, aad, aad_len,
    input_snapshot, output_base, total_len, tag, *out_error, eligible,
    ineligible_status
  );
#endif
#endif
  chacha_clear_state_key_material(state);
  chacha_secure_clear(key, sizeof(key));
  chacha_secure_clear(nonce, sizeof(nonce));
  if (input_snapshot) free(input_snapshot);
  if (aad_snapshot) free(aad_snapshot);
  return written;
}

void chacha20_poly1305_encrypt_all_64x64(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *plaintext, size_t plaintext_len,
  void *ciphertext, uint8_t tag[16]
) {
  chacha20_poly1305_state state;
  size_t written;

  chacha20_poly1305_init_64x64(&state, key, nonce);
  chacha20_poly1305_add_aad(&state, aad, aad_len);
  written = chacha20_poly1305_encrypt(
    &state, plaintext, plaintext_len, ciphertext
  );
  (void)chacha20_poly1305_final(
    &state, (uint8_t *)ciphertext + written, tag
  );
}

int32_t chacha20_poly1305_decrypt_all_64x64(
  const uint8_t key[32], const uint8_t nonce[8],
  const void *aad, size_t aad_len,
  const void *ciphertext, size_t ciphertext_len,
  void *plaintext, const uint8_t tag[16]
) {
  chacha20_poly1305_state state;
  size_t written;
  int32_t result;

  chacha20_poly1305_init_64x64(&state, key, nonce);
  chacha20_poly1305_add_aad(&state, aad, aad_len);
  written = chacha20_poly1305_decrypt(
    &state, ciphertext, ciphertext_len, plaintext
  );
  (void)chacha20_poly1305_verify(
    &state, (uint8_t *)plaintext + written, tag, &result
  );
  return result;
}

#undef POLY1305_MASK26
