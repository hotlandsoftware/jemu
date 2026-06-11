#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct GemuCpu GemuCpu;

typedef struct GemuCpuOps {
    void (*reset)(GemuCpu *cpu);
    bool (*step)(GemuCpu *cpu);     /* false = halt or unrecoverable error */
    void (*destroy)(GemuCpu *cpu);
} GemuCpuOps;

struct GemuCpu {
    const GemuCpuOps *ops;
    uint64_t          cycle_count;
    bool              halted;
};

static inline void gemu_cpu_reset(GemuCpu *cpu) {
    cpu->cycle_count = 0;
    cpu->halted = false;
    cpu->ops->reset(cpu);
}

static inline bool gemu_cpu_step(GemuCpu *cpu) {
    if (cpu->halted) return false;
    return cpu->ops->step(cpu);
}

static inline void gemu_cpu_destroy(GemuCpu *cpu) {
    cpu->ops->destroy(cpu);
}
