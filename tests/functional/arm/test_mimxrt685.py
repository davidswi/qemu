#!/usr/bin/env python3
#
# Local functional test for the MIMXRT685-EVK FreeRTOS firmware.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
from pathlib import Path

from qemu_test import QemuSystemTest, skipIfMissingEnv
from qemu_test import wait_for_console_pattern


class MIMXRT685Machine(QemuSystemTest):

    @skipIfMissingEnv("MIMXRT685_FREERTOS_ELF")
    def test_freertos_hello(self):
        firmware = Path(os.environ["MIMXRT685_FREERTOS_ELF"])
        self.assertTrue(firmware.is_file(),
                        f"firmware ELF does not exist: {firmware}")

        trace = Path(self.log_file("mimxrt685-int.log"))

        self.set_machine("mimxrt685-evk")
        self.vm.set_console()
        self.vm.add_args("-kernel", str(firmware),
                         "-d", "int,guest_errors,unimp",
                         "-D", str(trace))
        self.vm.launch()

        wait_for_console_pattern(self, "Hello world.")
        wait_for_console_pattern(self, "Hello world.")

        self.vm.cmd("system_reset")
        wait_for_console_pattern(self, "Hello world.")
        wait_for_console_pattern(self, "Hello world.")

        trace_text = trace.read_text(encoding="utf-8")
        self.assertGreaterEqual(trace_text.count("pending secure exception 11"),
                                2, "SVC did not start the scheduler twice")
        self.assertGreaterEqual(trace_text.count("pending secure exception 14"),
                                2, "PendSV context switching was not observed")
        self.assertGreaterEqual(trace_text.count("pending secure exception 15"),
                                2, "periodic SysTick was not observed")
        self.assertIn("Exception return: magic PC fffffffd", trace_text)

        trace_lower = trace_text.lower()
        for failure in ("failed exception return", "hardfault", "usagefault",
                        "lockup", "unimplemented", "unassigned"):
            self.assertNotIn(failure, trace_lower)


if __name__ == "__main__":
    QemuSystemTest.main()
