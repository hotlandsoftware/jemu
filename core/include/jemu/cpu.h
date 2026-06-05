#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct JemuCpu JemuCpu;

typedef struct JemuCpuOps {
    void (*reset)(JemuCpu *cpu);
    bool (*step)(JemuCpu *cpu);     /* false = halt or unrecoverable error */
    void (*destroy)(JemuCpu *cpu);
} JemuCpuOps;

struct JemuCpu {
    const JemuCpuOps *ops;
    uint64_t          cycle_count;
    bool              halted;
};

static inline void jemu_cpu_reset(JemuCpu *cpu) {
    cpu->cycle_count = 0;
    cpu->halted = false;
    cpu->ops->reset(cpu);
}

static inline bool jemu_cpu_step(JemuCpu *cpu) {
    if (cpu->halted) return false;
    return cpu->ops->step(cpu);
}

static inline void jemu_cpu_destroy(JemuCpu *cpu) {
    cpu->ops->destroy(cpu);
}
