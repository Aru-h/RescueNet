#include "chacha20.h"
#include <string.h> /* memcpy - already an established freestanding-safe
                        dependency in this tree, see e.g. core/team.c,
                        core/path.c */

/* RFC 8439 section 2.3: "expand 32-byte k" as four little-endian
   32-bit words. Fixed for every ChaCha20 key/nonce - this is a
   constant of the algorithm, not something callers configure. */
static const uint32_t CHACHA20_CONSTANTS[4] = {
    0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u
};

static uint32_t chacha20_rotl32(uint32_t x, int n)
{
    return (uint32_t)((x << n) | (x >> (32 - n)));
}

/* RFC 8439 section 2.1: the ChaCha quarter round, applied to four
   of the sixteen state words in place. */
static void chacha20_quarter_round(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a += *b; *d ^= *a; *d = chacha20_rotl32(*d, 16);
    *c += *d; *b ^= *c; *b = chacha20_rotl32(*b, 12);
    *a += *b; *d ^= *a; *d = chacha20_rotl32(*d, 8);
    *c += *d; *b ^= *c; *b = chacha20_rotl32(*b, 7);
}

static uint32_t chacha20_load_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void chacha20_store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

void chacha20_block(const uint8_t key[CHACHA20_KEY_BYTES],
                     const uint8_t nonce[CHACHA20_NONCE_BYTES],
                     uint32_t counter,
                     uint8_t out[CHACHA20_BLOCK_BYTES])
{
    uint32_t state[16];
    uint32_t working[16];
    int i;

    /* RFC 8439 section 2.3 state layout:
         words  0.. 3  constants
         words  4..11  256-bit key
         word   12     32-bit block counter
         words 13..15  96-bit nonce */
    state[0] = CHACHA20_CONSTANTS[0];
    state[1] = CHACHA20_CONSTANTS[1];
    state[2] = CHACHA20_CONSTANTS[2];
    state[3] = CHACHA20_CONSTANTS[3];

    for (i = 0; i < 8; i++)
    {
        state[4 + i] = chacha20_load_le32(&key[i * 4]);
    }

    state[12] = counter;
    state[13] = chacha20_load_le32(&nonce[0]);
    state[14] = chacha20_load_le32(&nonce[4]);
    state[15] = chacha20_load_le32(&nonce[8]);

    memcpy(working, state, sizeof(state));

    /* 20 rounds = 10 iterations of (4 column rounds + 4 diagonal
       rounds), per RFC 8439 section 2.3's reference chacha20_block(). */
    for (i = 0; i < 10; i++)
    {
        chacha20_quarter_round(&working[0], &working[4], &working[8], &working[12]);
        chacha20_quarter_round(&working[1], &working[5], &working[9], &working[13]);
        chacha20_quarter_round(&working[2], &working[6], &working[10], &working[14]);
        chacha20_quarter_round(&working[3], &working[7], &working[11], &working[15]);

        chacha20_quarter_round(&working[0], &working[5], &working[10], &working[15]);
        chacha20_quarter_round(&working[1], &working[6], &working[11], &working[12]);
        chacha20_quarter_round(&working[2], &working[7], &working[8], &working[13]);
        chacha20_quarter_round(&working[3], &working[4], &working[9], &working[14]);
    }

    for (i = 0; i < 16; i++)
    {
        uint32_t sum = working[i] + state[i];
        chacha20_store_le32(&out[i * 4], sum);
    }
}

void chacha20_xor(const uint8_t key[CHACHA20_KEY_BYTES],
                   const uint8_t nonce[CHACHA20_NONCE_BYTES],
                   uint32_t initial_counter,
                   const uint8_t *in, uint8_t *out, uint32_t len)
{
    uint8_t block[CHACHA20_BLOCK_BYTES];
    uint32_t counter = initial_counter;
    uint32_t offset = 0u;

    while (offset < len)
    {
        uint32_t chunk = len - offset;
        uint32_t i;

        if (chunk > CHACHA20_BLOCK_BYTES)
        {
            chunk = CHACHA20_BLOCK_BYTES;
        }

        chacha20_block(key, nonce, counter, block);

        for (i = 0u; i < chunk; i++)
        {
            out[offset + i] = (uint8_t)(in[offset + i] ^ block[i]);
        }

        offset += chunk;
        counter++; /* RFC 8439 section 2.4: counter increments once
                       per 64-byte block, wraparound within a single
                       message is the caller's problem (not reachable
                       here - LORA_FRAME_MAX_PAYLOAD is 144 bytes,
                       3 blocks, nowhere near 2^32 blocks). */
    }
}
