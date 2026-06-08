# JEMU (Jaguar EMUlator)

> [!WARNING]
> JEMU is currently unstable and should not be used in production environments.

# What the hell is it?
JEMU is a generic emulator designed to emulate as many machines, hardware, and peripherals as possible - while maintaining the simple UI of Virtual PC 2007 and flexibility, advanced featureset, speed, and ease of use of QEMU. 

JEMU is still in an alpha state. It will likely never be "complete" as there will always be more machines and hardware for it to emulate.

# Build command
```
make
make GTK=1 # for GTK support
```
# Targets
See MACHINES.md

# Why?
To put it bluntly, I'm very curious to see how far AI agents have come, and I'm curious to see *how* many different machines we can make AI emulate with little reference. (And also because I wanted something like MAME with the power of QEMU that could run off a VNC server.)