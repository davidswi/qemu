.. _Xtensa-System-emulator:

Xtensa System emulator
----------------------

Two executables cover simulation of both Xtensa endian options,
``qemu-system-xtensa`` and ``qemu-system-xtensaeb``. The following
machine types are emulated:

-  Xtensa emulator pseudo board \"sim\"

-  Avnet LX60/LX110/LX200 board

-  NXP MIMXRT685-EVK HiFi4 DSP (minimal local model)

The sim pseudo board emulation provides an environment similar to one
provided by the proprietary Tensilica ISS. It supports:

-  A range of Xtensa CPUs, default is the DC232B

-  Console and filesystem access via semihosting calls

The Avnet LX60/LX110/LX200 emulation supports:

-  A range of Xtensa CPUs, default is the DC232B

-  16550 UART

-  OpenCores 10/100 Mbps Ethernet MAC

NXP MIMXRT685-EVK HiFi4 DSP
~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``mimxrt685-evk-hifi4`` machine is a deliberately minimal model for
directly loading bare-metal DSP ELF files.  It provides the exact RT600
Xtensa ISA configuration, the DSP instruction and data TCM, a 2.5 MiB DSP
SRAM window at ``0x00200000``, and the clock, reset, IOPCTL, and FLEXCOMM0
registers used by the MCUXpresso ``hello_world_usart`` example.

It does not model the Cortex-M33, DSP power/stall sequencing, shared-memory
aliases, MU/SEMA42, the DSP interrupt mux, caches, DMA, or general RT685
peripheral behavior.  It is therefore an early firmware bring-up fixture,
not a complete RT685 SoC model.  The RT600 overlay describes the complete
configured ISA, but QEMU's translator still does not implement arbitrary
HiFi4 audio-engine instructions; the current fixture is validated only for
the scalar instruction path used by ``hello_world_usart``.

An Xtensa ELF is required.  For example, from this project's QEMU directory::

  build/qemu-system-xtensa \
    -M mimxrt685-evk-hifi4 \
    -nographic -monitor none \
    -kernel ../firmware/dsp/hello_world_usart/build-gcc/hello_world_usart.elf
