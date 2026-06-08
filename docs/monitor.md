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

> ``media``

Lists the media devices registered by the active machine, such as ``tape`` or
``cartridge``.

> ``dipswitch list``

Lists machine-specific DIP switches (when supported by the active machine)

> ``dipswitch (name) (value)``

Sets a machine-specific DIP switch (when supported by the active machine)

> ``change (device) (file)``

Inserts or changes media attached to a registered device. Device names depend
on the active machine, for example ``change cartridge game.bin`` on Studio II
or ``change tape program.bin`` on COSMAC VIP.

> ``change (device) (ADDR):(file)``

Passes an address-qualified file argument to devices that support it. For example, the COSMAC VIP tape accepts this form, for example ``change tape 0x0200:program.bin``.

> ``eject (device)``

Ejects media from a registered device. 
