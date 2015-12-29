/*
 * Argon2 source code package
 *
 * Written by Daniel Dinu and Dmitry Khovratovich, 2015
 *
 * This work is licensed under a Creative Commons CC0 1.0 License/Waiver.
 *
 * You should have received a copy of the CC0 Public Domain Dedication along
 * with
 * this software. If not, see
 * <http://creativecommons.org/publicdomain/zero/1.0/>.
 */

#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h>
#endif
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto_generichash_blake2b.h"
#include "runtime.h"
#include "utils.h"

#include "argon2-core.h"
#include "argon2-impl.h"
#include "blake2b-long.h"

#if !defined(MAP_ANON) && defined(MAP_ANONYMOUS)
# define MAP_ANON MAP_ANONYMOUS
#endif

static fill_segment_fn fill_segment = fill_segment_ref;

/***************Instance and Position constructors**********/
void init_block_value(block *b, uint8_t in) {
    memset(b->v, in, sizeof(b->v));
}

void copy_block(block *dst, const block *src) {
    memcpy(dst->v, src->v, sizeof(uint64_t) * ARGON2_QWORDS_IN_BLOCK);
}

void xor_block(block *dst, const block *src) {
    int i;
    for (i = 0; i < ARGON2_QWORDS_IN_BLOCK; ++i) {
        dst->v[i] ^= src->v[i];
    }
}

static void load_block(block *dst, const void *input) {
    unsigned i;
    for (i = 0; i < ARGON2_QWORDS_IN_BLOCK; ++i) {
        dst->v[i] = load64((const uint8_t *)input + i * sizeof(dst->v[i]));
    }
}

static void store_block(void *output, const block *src) {
    unsigned i;
    for (i = 0; i < ARGON2_QWORDS_IN_BLOCK; ++i) {
        store64((uint8_t *)output + i * sizeof(src->v[i]), src->v[i]);
    }
}

/***************Memory allocators*****************/
int allocate_memory(block_region **region, uint32_t m_cost) {
    block *memory;
    size_t memory_size;

    if (region == NULL) {
        return ARGON2_MEMORY_ALLOCATION_ERROR;
    }
    memory_size = sizeof(block) * m_cost;
    if (m_cost == 0 ||
        memory_size / m_cost != sizeof(block)) { /*1. Check for multiplication overflow*/
        return ARGON2_MEMORY_ALLOCATION_ERROR;
    }
    *region = (block_region *)malloc(sizeof(block_region));  /*2. Try to allocate region*/
    if (!*region) {
        return ARGON2_MEMORY_ALLOCATION_ERROR;
    }

    memory = (block *)malloc(memory_size); /*3. Try to allocate block*/
    if (!memory) {
        return ARGON2_MEMORY_ALLOCATION_ERROR;
    }
    (*region)->memory = memory;

    return ARGON2_OK;
}

/*********Memory functions*/

void clear_memory(argon2_instance_t *instance, int clear) {
    if (instance->region != NULL && clear) {
        sodium_memzero(instance->region->memory,
                       sizeof(block) * instance->memory_blocks);
    }
}

void free_memory(block_region *region) {
    free(region->memory);
    free(region);
}

void finalize(const argon2_context *context, argon2_instance_t *instance) {
    if (context != NULL && instance != NULL) {
        block blockhash;
        uint32_t l;

        copy_block(&blockhash, instance->region->memory + instance->lane_length - 1);

        /* XOR the last blocks */
        for (l = 1; l < instance->lanes; ++l) {
            uint32_t last_block_in_lane =
                l * instance->lane_length + (instance->lane_length - 1);
            xor_block(&blockhash, instance->region->memory + last_block_in_lane);
        }

        /* Hash the result */
        {
            uint8_t blockhash_bytes[ARGON2_BLOCK_SIZE];
            store_block(blockhash_bytes, &blockhash);
            blake2b_long(context->out, context->outlen, blockhash_bytes,
                         ARGON2_BLOCK_SIZE);
            sodium_memzero(blockhash.v,
                           ARGON2_BLOCK_SIZE); /* clear blockhash */
            sodium_memzero(blockhash_bytes,
                           ARGON2_BLOCK_SIZE); /* clear blockhash_bytes */
        }

        /* Clear memory */
        clear_memory(instance, context->flags & ARGON2_FLAG_CLEAR_PASSWORD);

        /* Deallocate the memory */
        if (NULL != context->free_cbk) {
            context->free_cbk((uint8_t *)instance->region->memory,
                              instance->memory_blocks * sizeof(block));
        } else {
            free_memory(instance->region);
        }
    }
}

uint32_t index_alpha(const argon2_instance_t *instance,
                     const argon2_position_t *position, uint32_t pseudo_rand,
                     int same_lane) {
    /*
     * Pass 0:
     *      This lane : all already finished segments plus already constructed
     * blocks in this segment
     *      Other lanes : all already finished segments
     * Pass 1+:
     *      This lane : (SYNC_POINTS - 1) last segments plus already constructed
     * blocks in this segment
     *      Other lanes : (SYNC_POINTS - 1) last segments
     */
    uint32_t reference_area_size;
    uint64_t relative_position;
    uint32_t start_position, absolute_position;

    if (0 == position->pass) {
        /* First pass */
        if (0 == position->slice) {
            /* First slice */
            reference_area_size =
                position->index - 1; /* all but the previous */
        } else {
            if (same_lane) {
                /* The same lane => add current segment */
                reference_area_size =
                    position->slice * instance->segment_length +
                    position->index - 1;
            } else {
                reference_area_size =
                    position->slice * instance->segment_length +
                    ((position->index == 0) ? (-1) : 0);
            }
        }
    } else {
        /* Second pass */
        if (same_lane) {
            reference_area_size = instance->lane_length -
                                  instance->segment_length + position->index -
                                  1;
        } else {
            reference_area_size = instance->lane_length -
                                  instance->segment_length +
                                  ((position->index == 0) ? (-1) : 0);
        }
    }

    /* 1.2.4. Mapping pseudo_rand to 0..<reference_area_size-1> and produce
     * relative position */
    relative_position = pseudo_rand;
    relative_position = relative_position * relative_position >> 32;
    relative_position = reference_area_size - 1 -
                        (reference_area_size * relative_position >> 32);

    /* 1.2.5 Computing starting position */
    start_position = 0;

    if (0 != position->pass) {
        start_position = (position->slice == ARGON2_SYNC_POINTS - 1)
                             ? 0
                             : (position->slice + 1) * instance->segment_length;
    }

    /* 1.2.6. Computing absolute position */
    absolute_position = (start_position + relative_position) %
                        instance->lane_length; /* absolute position */
    return absolute_position;
}

void fill_memory_blocks(argon2_instance_t *instance) {
    uint32_t r, s;

    if (instance == NULL || instance->lanes == 0) {
        return;
    }

    for (r = 0; r < instance->passes; ++r) {
        for (s = 0; s < ARGON2_SYNC_POINTS; ++s) {
            uint32_t l;

            for (l = 0; l < instance->lanes; ++l) {
                argon2_position_t position;

                position.pass = r;
                position.lane = l;
                position.slice = (uint8_t)s;
                position.index = 0;
                fill_segment(instance, position);
            }
        }
    }
}

int validate_inputs(const argon2_context *context) {
    if (NULL == context) {
        return ARGON2_INCORRECT_PARAMETER;
    }

    if (NULL == context->out) {
        return ARGON2_OUTPUT_PTR_NULL;
    }

    /* Validate output length */
    if (ARGON2_MIN_OUTLEN > context->outlen) {
        return ARGON2_OUTPUT_TOO_SHORT;
    }

    if (ARGON2_MAX_OUTLEN < context->outlen) {
        return ARGON2_OUTPUT_TOO_LONG;
    }

    /* Validate password length */
    if (NULL == context->pwd) {
        if (0 != context->pwdlen) {
            return ARGON2_PWD_PTR_MISMATCH;
        }
    } else {
        if (ARGON2_MIN_PWD_LENGTH > context->pwdlen) {
            return ARGON2_PWD_TOO_SHORT;
        }

        if (ARGON2_MAX_PWD_LENGTH < context->pwdlen) {
            return ARGON2_PWD_TOO_LONG;
        }
    }

    /* Validate salt length */
    if (NULL == context->salt) {
        if (0 != context->saltlen) {
            return ARGON2_SALT_PTR_MISMATCH;
        }
    } else {
        if (ARGON2_MIN_SALT_LENGTH > context->saltlen) {
            return ARGON2_SALT_TOO_SHORT;
        }

        if (ARGON2_MAX_SALT_LENGTH < context->saltlen) {
            return ARGON2_SALT_TOO_LONG;
        }
    }

    /* Validate secret length */
    if (NULL == context->secret) {
        if (0 != context->secretlen) {
            return ARGON2_SECRET_PTR_MISMATCH;
        }
    } else {
        if (ARGON2_MIN_SECRET > context->secretlen) {
            return ARGON2_SECRET_TOO_SHORT;
        }

        if (ARGON2_MAX_SECRET < context->secretlen) {
            return ARGON2_SECRET_TOO_LONG;
        }
    }

    /* Validate associated data */
    if (NULL == context->ad) {
        if (0 != context->adlen) {
            return ARGON2_AD_PTR_MISMATCH;
        }
    } else {
        if (ARGON2_MIN_AD_LENGTH > context->adlen) {
            return ARGON2_AD_TOO_SHORT;
        }

        if (ARGON2_MAX_AD_LENGTH < context->adlen) {
            return ARGON2_AD_TOO_LONG;
        }
    }

    /* Validate memory cost */
    if (ARGON2_MIN_MEMORY > context->m_cost) {
        return ARGON2_MEMORY_TOO_LITTLE;
    }

    if (ARGON2_MAX_MEMORY < context->m_cost) {
        return ARGON2_MEMORY_TOO_MUCH;
    }

    if (context->m_cost < 8*context->lanes) {
        return ARGON2_MEMORY_TOO_LITTLE;
    }

    /* Validate time cost */
    if (ARGON2_MIN_TIME > context->t_cost) {
        return ARGON2_TIME_TOO_SMALL;
    }

    if (ARGON2_MAX_TIME < context->t_cost) {
        return ARGON2_TIME_TOO_LARGE;
    }

    /* Validate lanes */
    if (ARGON2_MIN_LANES > context->lanes) {
        return ARGON2_LANES_TOO_FEW;
    }

    if (ARGON2_MAX_LANES < context->lanes) {
        return ARGON2_LANES_TOO_MANY;
    }

    /* Validate threads */
    if (ARGON2_MIN_THREADS > context->threads) {
        return ARGON2_THREADS_TOO_FEW;
    }

    if (ARGON2_MAX_THREADS < context->threads) {
        return ARGON2_THREADS_TOO_MANY;
    }

    if (NULL != context->allocate_cbk && NULL == context->free_cbk) {
        return ARGON2_FREE_MEMORY_CBK_NULL;
    }

    if (NULL == context->allocate_cbk && NULL != context->free_cbk) {
        return ARGON2_ALLOCATE_MEMORY_CBK_NULL;
    }

    return ARGON2_OK;
}

void fill_first_blocks(uint8_t *blockhash, const argon2_instance_t *instance) {
    uint32_t l;
    /* Make the first and second block in each lane as G(H0||i||0) or
       G(H0||i||1) */
    uint8_t blockhash_bytes[ARGON2_BLOCK_SIZE];
    for (l = 0; l < instance->lanes; ++l) {

        store32(blockhash + ARGON2_PREHASH_DIGEST_LENGTH, 0);
        store32(blockhash + ARGON2_PREHASH_DIGEST_LENGTH + 4, l);
        blake2b_long(blockhash_bytes, ARGON2_BLOCK_SIZE, blockhash,
                     ARGON2_PREHASH_SEED_LENGTH);
        load_block(&instance->region->memory[l * instance->lane_length + 0],
                   blockhash_bytes);

        store32(blockhash + ARGON2_PREHASH_DIGEST_LENGTH, 1);
        blake2b_long(blockhash_bytes, ARGON2_BLOCK_SIZE, blockhash,
                     ARGON2_PREHASH_SEED_LENGTH);
        load_block(&instance->region->memory[l * instance->lane_length + 1],
                   blockhash_bytes);
    }
    sodium_memzero(blockhash_bytes, ARGON2_BLOCK_SIZE);
}

void initial_hash(uint8_t *blockhash, argon2_context *context,
                  argon2_type type) {
    crypto_generichash_blake2b_state BlakeHash;
    uint8_t value[4U /* sizeof(uint32_t) */];

    if (NULL == context || NULL == blockhash) {
        return;
    }

    crypto_generichash_blake2b_init(&BlakeHash, NULL, 0U,
                                    ARGON2_PREHASH_DIGEST_LENGTH);

    store32(&value, context->lanes);
    crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    store32(&value, context->outlen);
    crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    store32(&value, context->m_cost);
    crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    store32(&value, context->t_cost);
    crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    store32(&value, ARGON2_VERSION_NUMBER);
    crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    store32(&value, (uint32_t)type);
    crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    store32(&value, context->pwdlen);
    crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    if (context->pwd != NULL) {
        crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)context->pwd,
                                          context->pwdlen);

        if (context->flags & ARGON2_FLAG_CLEAR_PASSWORD) {
            sodium_memzero(context->pwd, context->pwdlen);
            context->pwdlen = 0;
        }
    }

    store32(&value, context->saltlen);
    crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    if (context->salt != NULL) {
        crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)context->salt,
                       context->saltlen);
    }

    store32(&value, context->secretlen);
    crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    if (context->secret != NULL) {
        crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)context->secret,
                       context->secretlen);

        if (context->flags & ARGON2_FLAG_CLEAR_SECRET) {
            sodium_memzero(context->secret, context->secretlen);
            context->secretlen = 0;
        }
    }

    store32(&value, context->adlen);
    crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)&value, sizeof(value));

    if (context->ad != NULL) {
        crypto_generichash_blake2b_update(&BlakeHash, (const uint8_t *)context->ad,
                                          context->adlen);
    }

    crypto_generichash_blake2b_final(&BlakeHash, blockhash, ARGON2_PREHASH_DIGEST_LENGTH);
}

int initialize(argon2_instance_t *instance, argon2_context *context) {
    uint8_t blockhash[ARGON2_PREHASH_SEED_LENGTH];
    int result = ARGON2_OK;

    if (instance == NULL || context == NULL)
        return ARGON2_INCORRECT_PARAMETER;

    /* 1. Memory allocation */

    if (NULL != context->allocate_cbk) {
        uint8_t *p;
        result = context->allocate_cbk(&p, instance->memory_blocks *
                                       ARGON2_BLOCK_SIZE);
        if (ARGON2_OK != result) {
            return result;
        }
        memcpy(&(instance->region->memory), p, sizeof(instance->region->memory));
    } else {
        result = allocate_memory(&(instance->region), instance->memory_blocks);
        if (ARGON2_OK != result) {
            return result;
        }
    }

    /* 2. Initial hashing */
    /* H_0 + 8 extra bytes to produce the first blocks */
    /* uint8_t blockhash[ARGON2_PREHASH_SEED_LENGTH]; */
    /* Hashing all inputs */
    initial_hash(blockhash, context, instance->type);
    /* Zeroing 8 extra bytes */
    sodium_memzero(blockhash + ARGON2_PREHASH_DIGEST_LENGTH,
                   ARGON2_PREHASH_SEED_LENGTH - ARGON2_PREHASH_DIGEST_LENGTH);

    /* 3. Creating first blocks, we always have at least two blocks in a slice
     */
    fill_first_blocks(blockhash, instance);
    /* Clearing the hash */
    sodium_memzero(blockhash, ARGON2_PREHASH_SEED_LENGTH);

    return ARGON2_OK;
}

int argon2_pick_best_implementation(void)
{
#if (defined(HAVE_EMMINTRIN_H) && defined(HAVE_TMMINTRIN_H)) || \
    (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64)))
    if (sodium_runtime_has_ssse3()) {
        fill_segment = fill_segment_ssse3;
        return 0;
    }
#endif
    fill_segment = fill_segment_ref;

    return 0;
}