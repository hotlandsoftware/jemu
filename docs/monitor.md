The JEMU monitor is made to give commands to the JEMU emulator. It is designed to be as backwards compatible as possible with the QEMU emulator, with a couple of additional commands per emulator.

# Commands
The following commands are available:

> ``help``
Shows the list of commands.

> ``quit`` / ``q``
Quits the emulator.

> ``reset`` / ``system_Reset``
Hard resets the emulator (equivalent to pressing the reset button on a PC)

> ``stop`` / ``halt``
Halts the emulator (does not quit)

> ``cont``
Resumes the emulator if it was stopped or halted.

> ``step`` / ``s``
Executes one instruction