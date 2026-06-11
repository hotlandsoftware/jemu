# gemu — Jaguar Emulator

General-purpose machine emulator written in C. Modeled after QEMU in architecture,
targeting speed over strict accuracy (balanced). Not associated with the Atari Jaguar.

---

## Architecture

```
gemu/
├── core/               # Platform-agnostic emulator infrastructure
│   ├── include/gemu/
│   │   ├── gemu.h      # Version, common macros
│   │   ├── memory.h    # Memory bus abstraction
│   │   ├── cpu.h       # Abstract CPU interface
│   │   ├── device.h    # Device I/O interface
│   │   └── tcg.h       # Translation Block Cache (DCT infrastructure)
│   └── src/
│       ├── memory.c
│       └── tcg.c
└── gemu-chip8/         # CHIP-8 platform (first target)
    ├── include/chip8.h
    └── src/
        ├── main.c      # CLI entry + arg parsing
        ├── cpu.c       # Decoder, TB translator, executor
        ├── display.c   # SDL2 display backend
        ├── input.c     # SDL2 keyboard input
        └── machine.c   # Machine init, run loop, timers
```

---

## Design Principles

### Dynamic Code Translation (DCT)
Guest instructions are translated into **Translation Blocks (TBs)** — sequences of
decoded instructions from a starting PC to the next branch. TBs are cached in a hash
table keyed on guest PC. On cache hit the decoded block is executed directly without
re-decoding, eliminating repeated fetch-decode overhead on hot paths.

Future roadmap: TBs will hold native host machine code (JIT), then optionally backed
by KVM for hardware-accelerated execution.

### Separation of Concerns
| Layer            | Responsibility                                      |
|------------------|-----------------------------------------------------|
| core/tcg         | TB cache management, invalidation                   |
| core/memory      | Flat and banked memory, ROM/RAM regions             |
| core/cpu         | Abstract CPU vtable (reset, step, destroy)          |
| core/device      | Device I/O vtable (read, write, update)             |
| gemu-\<platform\> | Platform-specific decoder, machine init, run loop   |

---

## Platforms

### gemu-chip8 (current)
CHIP-8 interpreter + translation block cache.

**Specs:**
- 4 KB memory (0x000–0xFFF); ROM loads at 0x200
- 16 × 8-bit general registers (V0–VF); VF = flag register
- 16-bit index register I, program counter PC
- 16-level call stack
- Delay timer + sound timer (60 Hz)
- 64×32 monochrome display (XOR sprite drawing)
- 16-key hex keypad (mapped to 1234/QWER/ASDF/ZXCV)
- 34 opcodes, all 2 bytes wide

**Usage:**
```
gemu-chip8 [options] <rom.ch8>

Options:
  -m SIZE     Memory size (default: 4K)
  -cpu TYPE   CPU variant: chip8 (default)
  -vga TYPE   Display backend: std (SDL2, default) | none (headless)
  -scale N    Pixel scale factor (default: 10 → 640×320 window)
  -hz N       CPU speed in instructions/sec (default: 700)
```

**Example:**
```
gemu-chip8 -m 4K -cpu chip8 -vga std roms/pong.ch8
```

---

## Future Platforms (planned, not yet implemented)

| Binary        | Target                     |
|---------------|----------------------------|
| gemu-x86      | x86/x86-64 PC (i440FX)     |
| gemu-arm      | ARMv7/AArch64              |

## Future Features (not yet implemented)
- KVM acceleration backend
- User-mode emulation
- Virtual PC 2007-style GUI frontend
- Snapshot / save states
- GDB remote stub for guest debugging
