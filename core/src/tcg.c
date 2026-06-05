#include "jemu/tcg.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static uint32_t tb_hash(uint32_t pc) {
    /* Knuth multiplicative hash, shift down to index width */
    return (pc * 2654435761u) >> (32 - JEMU_TB_HASH_BITS);
}

void jemu_tb_cache_init(JemuTbCache *cache, void (*free_insns)(void *)) {
    memset(cache, 0, sizeof(*cache));
    cache->free_insns = free_insns;
}

void jemu_tb_cache_flush(JemuTbCache *cache) {
    for (uint32_t i = 0; i < JEMU_TB_HASH_SIZE; i++) {
        JemuTb *tb = cache->buckets[i];
        while (tb) {
            JemuTb *next = tb->hash_next;
            if (cache->free_insns && tb->insns)
                cache->free_insns(tb->insns);
            free(tb);
            tb = next;
        }
        cache->buckets[i] = NULL;
    }
    cache->n_tbs = 0;
    cache->n_insns_total = 0;
}

JemuTb *jemu_tb_lookup(const JemuTbCache *cache, uint32_t pc) {
    JemuTb *tb = cache->buckets[tb_hash(pc)];
    while (tb) {
        if (tb->guest_pc == pc) return tb;
        tb = tb->hash_next;
    }
    return NULL;
}

JemuTb *jemu_tb_insert(JemuTbCache *cache, uint32_t pc,
                       uint32_t guest_size, uint32_t n_insns, void *insns) {
    JemuTb *tb = malloc(sizeof(*tb));
    if (!tb) return NULL;

    tb->guest_pc   = pc;
    tb->guest_size = guest_size;
    tb->n_insns    = n_insns;
    tb->insns      = insns;
    tb->exec_count = 0;

    uint32_t bucket  = tb_hash(pc);
    tb->hash_next    = cache->buckets[bucket];
    cache->buckets[bucket] = tb;

    cache->n_tbs++;
    cache->n_insns_total += n_insns;
    return tb;
}

void jemu_tb_invalidate_range(JemuTbCache *cache, uint32_t pc, uint32_t size) {
    for (uint32_t i = 0; i < JEMU_TB_HASH_SIZE; i++) {
        JemuTb **prev = &cache->buckets[i];
        JemuTb  *tb   = *prev;
        while (tb) {
            uint32_t tb_end  = tb->guest_pc + tb->guest_size;
            uint32_t inv_end = pc + size;
            bool overlap = !(tb_end <= pc || tb->guest_pc >= inv_end);
            if (overlap) {
                *prev = tb->hash_next;
                if (cache->free_insns && tb->insns)
                    cache->free_insns(tb->insns);
                cache->n_insns_total -= tb->n_insns;
                cache->n_tbs--;
                free(tb);
                tb = *prev;
            } else {
                prev = &tb->hash_next;
                tb   = tb->hash_next;
            }
        }
    }
}
