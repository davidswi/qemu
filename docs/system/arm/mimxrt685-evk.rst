.. SPDX-License-Identifier: GPL-2.0-or-later

NXP MIMXRT685-EVK (``mimxrt685-evk``)
=========================================

This is a local experimental model of the Cortex-M33 side of the NXP
MIMXRT685S.  It is not intended for submission to upstream QEMU.

The model uses QEMU's Armv7M container with a Cortex-M33, 60 external IRQ
inputs and three implemented priority bits.  The CPU retains the Armv8-M
Security Extension and therefore starts in Secure state.  The SAU is disabled
at reset, as required by the architecture.  The model does not yet implement
the RT685 IDAU, secure bus attribution, or the boot-ROM secure-to-non-secure
handoff.  Both ``init-svtor`` and ``init-nsvtor`` are zero.
The Armv7M Secure and Non-secure address spaces use the same machine-memory
container; no RT685-specific IDAU or downstream security checker is modeled.

Memory map
----------

The shared SRAM consists of 30 physical partitions totaling 4.5 MiB.  M0
models these as one backing store with its two Cortex-M33 views:

.. list-table:: M0 memory map
   :header-rows: 1

   * - Region (address, size)
     - Physical backing and alias/security relationship
     - Access
     - Reset domain and kind
   * - Shared SRAM code view (``0x00000000``, ``0x00480000``)
     - One 4.5 MiB SRAM allocation; aliases the data view and is visible in
       the common Secure/Non-secure memory container
     - Read/write/execute
     - Contents retained on system reset; real storage
   * - Shared SRAM data view (``0x20000000``, ``0x00480000``)
     - Alias of the same SRAM allocation, not duplicate RAM
     - Read/write/execute
     - Contents retained on system reset; alias
   * - CLKCTL0 (``0x40001000``, ``0x00001000``)
     - CLKCTL0 register device in the common memory container
     - 32-bit register-dependent read/write access
     - Registers reset; device
   * - SYSCTL0 (``0x40002000``, ``0x00001000``)
     - SYSCTL0 register device in the common memory container
     - 32-bit register-dependent read/write access
     - Registers reset; device
   * - CLKCTL1 (``0x40021000``, ``0x00001000``)
     - CLKCTL1 register device in the common memory container
     - 32-bit register-dependent read/write access
     - Registers reset; device
   * - SYSCTL1 (``0x40022000``, ``0x00001000``)
     - SYSCTL1 register device in the common memory container
     - 32-bit register-dependent read/write access
     - Registers reset; device
   * - RSTCTL0/1 (``0x40000000``/``0x40020000``, ``0x80`` each)
     - Peripheral reset controllers with PRSTCTL SET/CLR aliases
     - 32-bit register access
     - Registers reset; device reset outputs are wired to GPIO and Flexcomm
   * - IOPCTL (``0x40004000``, ``0x408``)
     - Pin configuration storage using the SVD reset values and writable mask
     - Aligned 32-bit access
     - Registers reset; device
   * - GPIO (``0x40100000``, ``0x2800``)
     - Eight 32-pin ports, byte/word pin aliases, direction and interrupt state
     - Register-dependent 8- or 32-bit access
     - Per-port clock and reset domains; IRQs 2 and 3
   * - Flexcomm 0--7 (``0x40106000``--``0x40109000`` and
       ``0x40122000``--``0x40125000``, ``0x1000`` each)
     - Flexcomm selector plus the USART and FIFO register view
     - Aligned 32-bit access
     - Per-instance clock/reset domains; IRQs 14--19, 43 and 44
   * - Cortex-M system space (``0xe0000000`` architectural window)
     - QEMU Armv7M NVIC, SysTick and system-control implementation
     - Architectural access rules
     - Architectural reset; device

Addresses outside these regions are intentionally unmapped.  The selected
M33 configuration has no modeled M33 TCM, and no boot-ROM image is supplied.

SRAM contents are retained over an emulated system reset.  The clock and
system-control registers return to the reset values in the supplied
``MIMXRT685S_cm33`` CMSIS-SVD.

Clock and system control
------------------------

The four CLKCTL/SYSCTL instances provide reset values, writable masks,
read-only registers, and the documented SET/CLR aliases used by SDK startup.
CLKCTL0 drives the Armv7M CPU clock.  Its reset path is FFRO/4 (12 MHz), and
the modeled MAINCLK mux, FFRO 48/60 MHz selection, and CPU divider update the
QEMU clock immediately.  Oscillator and PLL lock delays are not modeled.
CLKCTL1 gates each GPIO port and Flexcomm instance; RSTCTL1 independently
asserts and deasserts their reset inputs.

M1 peripheral scope
-------------------

M1 implements the board-startup path expected for the SDK debug console:
IOPCTL pin setup, GPIO state, the Flexcomm function selector, and LPC USART
FIFO transmit/receive through QEMU character backends.  Flexcomm 0 is attached
to QEMU serial port 0, with subsequent instances attached in numerical order.
The USART model supports configuration, interrupt enables, baud and oversample
registers, FIFO enable/empty controls, FIFO levels, polling transmit/receive,
and receive-level interrupts.  Transmit is synchronous, so its modeled FIFO is
always empty and not full.

The register layout, reset values and writable masks come from the
project-local ``MIMXRT685S_cm33`` CMSIS-SVD.  The SDK source census identifies
Flexcomm 0 as the EVK debug console.  A pinned SDK build, linker map and EVK
execution trace are not present in this checkout, so ANACTRL, PMC, cache-control
and ROM-call requirements cannot yet be admitted to or excluded from the final
firmware-derived census.

Reference matrix
----------------

.. list-table:: M1 device references and boundaries
   :header-rows: 1

   * - Device
     - Sources
     - Implemented subset
     - Deferred behavior
   * - RSTCTL0/1
     - CMSIS-SVD ``RSTCTL0``/``RSTCTL1``
     - Reset values, writable masks, W1C status, SET/CLR aliases and reset lines
     - Complete SoC reset-tree and warm-reset causes
   * - IOPCTL
     - CMSIS-SVD ``IOPCTL``; SDK board pin-mux initialization
     - Per-pin reset values and masked configuration storage
     - Electrical pulls, slew timing and pin-level mux routing
   * - GPIO
     - CMSIS-SVD ``GPIO``
     - Byte/word pin aliases, direction, mask, SET/CLR/NOT, and A/B interrupts
     - Pattern matching and physical board connections
   * - Flexcomm USART
     - CMSIS-SVD ``FLEXCOMM0`` and ``USART0``; SDK debug-console initialization
     - Function selection/lock, core USART configuration, FIFO polling, RX IRQ,
       and character backend
     - SPI/I2C/I2S functions, line timing, modem signals and error injection

The models are native QEMU devices.  No register compatibility was assumed
from the SOF i.MX8 devices, and no third-party model behavior was copied.

Boot
----

Use ``-kernel firmware.elf`` for an ELF linked into either SRAM view.  Raw
images are loaded at address zero.  The first word must be the initial stack
pointer and the second word the Thumb reset-handler address.

Not modeled
-----------

The boot ROM, HiFi4 core and its TCM, external flash/PSRAM, non-USART Flexcomm
functions, timers other than architectural SysTick, DMA, cache controller,
ANACTRL/PMC power and analog behavior, and all board-level devices remain
unmodeled.
