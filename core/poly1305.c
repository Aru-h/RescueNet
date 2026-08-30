#include "poly1305.h"

static uint32_t poly1305_load_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void poly1305_store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/*
 * Processes `blocks` complete 16-byte chunks of `m` into the
 * accumulator h[5], under multiplier r[5] (and r*5 precomputed as
 * s[5], the standard donna optimization for the mod-(2^130-5)
 * reduction). `hibit` is 1<<24 for ordinary full blocks (the
 * "implicit 2^128 term" RFC 8439 section 2.5.1 adds per full block)
 * or 0 for the final, already-padded-with-0x01-then-zeros block
 * poly1305_mac() below constructs when the message length isn't a
 * multiple of 16 - see that function's own comment.
 */
static void poly1305_blocks(uint32_t h[5], const uint32_t r[5], const uint32_t s[5],
                             const uint8_t *m, uint32_t blocks, uint32_t hibit)
{
    uint32_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4];
    const uint32_t r0 = r[0], r1 = r[1], r2 = r[2], r3 = r[3], r4 = r[4];
    const uint32_t s1 = s[1], s2 = s[2], s3 = s[3], s4 = s[4];

    while (blocks > 0u)
    {
        uint64_t d0, d1, d2, d3, d4;
        uint32_t c;
        uint32_t t0 = poly1305_load_le32(&m[0]);
        uint32_t t1 = poly1305_load_le32(&m[4]);
        uint32_t t2 = poly1305_load_le32(&m[8]);
        uint32_t t3 = poly1305_load_le32(&m[12]);

        /* h += next 16-byte chunk, split into five 26-bit limbs. */
        h0 += t0 & 0x3ffffffu;
        h1 += (uint32_t)(((((uint64_t)t1 << 32) | t0) >> 26) & 0x3ffffffu);
        h2 += (uint32_t)(((((uint64_t)t2 << 32) | t1) >> 20) & 0x3ffffffu);
        h3 += (uint32_t)(((((uint64_t)t3 << 32) | t2) >> 14) & 0x3ffffffu);
        h4 += (t3 >> 8) | hibit;

        /* h *= r, mod 2^130 - 5 (s1..s4 = r1..r4 * 5, the standard
           trick that turns the reduction into extra additions
           instead of a second modulus pass). */
        d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3
           + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4
           + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0
           + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1
           + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2
           + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        /* Partial carry propagation, radix 2^26. */
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffffu;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffffu;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffffu;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffffu;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffffu;
        h0 += c * 5u; c = h0 >> 26; h0 &= 0x3ffffffu;
        h1 += c;

        m += 16;
        blocks--;
    }

    h[0] = h0; h[1] = h1; h[2] = h2; h[3] = h3; h[4] = h4;
}

void poly1305_mac(const uint8_t key[POLY1305_KEY_BYTES],
                   const uint8_t *msg, uint32_t len,
                   uint8_t out_tag[POLY1305_TAG_BYTES])
{
    uint32_t r[5];
    uint32_t s[5]; /* s[0] unused; s[1..4] = r[1..4] * 5 */
    uint32_t h[5] = { 0u, 0u, 0u, 0u, 0u };
    uint32_t pad[4];
    uint32_t full_blocks = len / 16u;
    uint32_t remainder = len % 16u;
    uint32_t t0, t1, t2, t3;

    /* RFC 8439 section 2.5.1: clamp r before use. */
    t0 = poly1305_load_le32(&key[0]);
    t1 = poly1305_load_le32(&key[4]);
    t2 = poly1305_load_le32(&key[8]);
    t3 = poly1305_load_le32(&key[12]);

    r[0] = t0 & 0x3ffffffu;
    r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03u;
    r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ffu;
    r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fffu;
    r[4] = (t3 >> 8) & 0x00fffffu;

    s[1] = r[1] * 5u;
    s[2] = r[2] * 5u;
    s[3] = r[3] * 5u;
    s[4] = r[4] * 5u;

    pad[0] = poly1305_load_le32(&key[16]);
    pad[1] = poly1305_load_le32(&key[20]);
    pad[2] = poly1305_load_le32(&key[24]);
    pad[3] = poly1305_load_le32(&key[28]);

    if (full_blocks > 0u)
    {
        poly1305_blocks(h, r, s, msg, full_blocks, 1u << 24);
    }

    if (remainder > 0u)
    {
        /* RFC 8439 section 2.5.1: pad the trailing partial block
           with a single 0x01 byte then zeros up to 16 bytes, and
           process it WITHOUT the implicit 2^128 term (hibit = 0) -
           the 0x01 byte itself is what stands in for that term on a
           short final block. */
        uint8_t final_block[16] = { 0u };
        uint32_t i;

        for (i = 0u; i < remainder; i++)
        {
            final_block[i] = msg[full_blocks * 16u + i];
        }
        final_block[remainder] = 0x01u;

        poly1305_blocks(h, r, s, final_block, 1u, 0u);
    }

    /* Fully carry h, radix 2^26. */
    {
        uint32_t c;
        uint32_t g0, g1, g2, g3, g4;
        uint32_t mask;
        uint64_t f;

        c = h[1] >> 26; h[1] &= 0x3ffffffu;
        h[2] += c; c = h[2] >> 26; h[2] &= 0x3ffffffu;
        h[3] += c; c = h[3] >> 26; h[3] &= 0x3ffffffu;
        h[4] += c; c = h[4] >> 26; h[4] &= 0x3ffffffu;
        h[0] += c * 5u; c = h[0] >> 26; h[0] &= 0x3ffffffu;
        h[1] += c;

        /* g = h - p (p = 2^130 - 5), computed as h + (-p) mod 2^32
           per limb, to test h >= p without a general bignum
           comparison. */
        g0 = h[0] + 5u; c = g0 >> 26; g0 &= 0x3ffffffu;
        g1 = h[1] + c; c = g1 >> 26; g1 &= 0x3ffffffu;
        g2 = h[2] + c; c = g2 >> 26; g2 &= 0x3ffffffu;
        g3 = h[3] + c; c = g3 >> 26; g3 &= 0x3ffffffu;
        g4 = h[4] + c - (1u << 26);

        /* g4's top bit (as a 32-bit value) is 1 iff h < p (the
           subtraction "went negative"/wrapped) - select h in that
           case, g otherwise. mask = bit - 1 (unsigned wraparound):
           bit=1 (h<p) -> mask=0 (keep h below); bit=0 (h>=p) ->
           mask=0xffffffff (select g below). Explicit 32-bit-wide
           test (not sizeof(unsigned long)), matching this file's
           stdint-only policy - see this file's own top comment. */
        mask = (g4 >> 31) - 1u;
        g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
        mask = ~mask;
        h[0] = (h[0] & mask) | g0;
        h[1] = (h[1] & mask) | g1;
        h[2] = (h[2] & mask) | g2;
        h[3] = (h[3] & mask) | g3;
        h[4] = (h[4] & mask) | g4;

        /* Repack the five 26-bit limbs into four 32-bit words
           (h mod 2^128). */
        h[0] = (h[0]) | (h[1] << 26);
        h[1] = (h[1] >> 6) | (h[2] << 20);
        h[2] = (h[2] >> 12) | (h[3] << 14);
        h[3] = (h[3] >> 18) | (h[4] << 8);

        /* tag = (h + pad) mod 2^128 */
        f = (uint64_t)h[0] + pad[0];                h[0] = (uint32_t)f;
        f = (uint64_t)h[1] + pad[1] + (f >> 32);     h[1] = (uint32_t)f;
        f = (uint64_t)h[2] + pad[2] + (f >> 32);     h[2] = (uint32_t)f;
        f = (uint64_t)h[3] + pad[3] + (f >> 32);     h[3] = (uint32_t)f;
    }

    poly1305_store_le32(&out_tag[0], h[0]);
    poly1305_store_le32(&out_tag[4], h[1]);
    poly1305_store_le32(&out_tag[8], h[2]);
    poly1305_store_le32(&out_tag[12], h[3]);
}
