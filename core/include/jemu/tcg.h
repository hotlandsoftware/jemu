#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Translation Block Cache — core of jemu's dynamic code translation.
 *
 * Guest code is divided into basic blocks (TBs): a linear sequence of
 * instructions from a starting PC up to and including the first branch.
 * TBs are decoded once and cached in a hash table keyed on guest PC.
 *
 * Roadmap: today TBs hold pre-decoded structs (fast interpreter dispatch).
 * In a future JIT tier, TBs will hold native host code in mmap'd executable
 * memory. The cache management layer stays the same either way.
 */

#define JEMU_TB_HASH_BITS 10
#define JEMU_TB_HASH_SIZE (1u << JEMU_TB_HASH_BITS)
#define JEMU_TB_HASH_MASK (JEMU_TB_HASH_SIZE - 1u)

#define JEMU_TB_MAX_INSNS 64

typedef struct JemuTb {
    uint32_t        guest_pc;   /* first guest instruction address */
    uint32_t        guest_size; /* covered bytes (n_insns * insn_width) */
    uint32_t        n_insns;    /* number of decoded instructions */
    void           *insns;      /* platform-allocated decoded insn array */
    uint32_t        exec_count; /* hot-path hit counter */
    struct JemuTb  *hash_next;  /* chaining within hash bucket */
} JemuTb;

typedef struct JemuTbCache {
    JemuTb  *buckets[JEMU_TB_HASH_SIZE];
    uint32_t n_tbs;
    uint32_t n_insns_total;
    void    (*free_insns)(void *insns); /* platform-provided destructor */
} JemuTbCache;

void    jemu_tb_cache_init(JemuTbCache *cache, void (*free_insns)(void *));
void    jemu_tb_cache_flush(JemuTbCache *cache);

JemuTb *jemu_tb_lookup(const JemuTbCache *cache, uint32_t pc);
JemuTb *jemu_tb_insert(JemuTbCache *cache, uint32_t pc,
                       uint32_t guest_size, uint32_t n_insns, void *insns);
void    jemu_tb_invalidate_range(JemuTbCache *cache, uint32_t pc, uint32_t size);
