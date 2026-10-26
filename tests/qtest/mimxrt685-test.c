/*
 * QTests for the local NXP MIMXRT685-EVK model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest-single.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define CLKCTL0 0x40001000
#define RSTCTL0 0x40000000
#define IOPCTL  0x40004000
#define RSTCTL1 0x40020000
#define SYSCTL0 0x40002000
#define CLKCTL1 0x40021000
#define SYSCTL1 0x40022000
#define GPIO    0x40100000
#define OSTIMER 0x40113000
#define FC0     0x40106000
#define FC14    0x40126000
#define FC15    0x40127000

#define FC_PSELID_PRESENT_ALL 0x001010f0
#define FC_PSELID_PRESENT_SPI 0x00101020
#define FC_PSELID_PRESENT_I2C 0x00101040
#define FC14_PATH "/machine/soc/flexcomm14"

static uint64_t gray_to_binary(uint64_t gray)
{
    uint64_t binary = gray;

    for (unsigned shift = 1; shift < 64; shift <<= 1) {
        binary ^= binary >> shift;
    }
    return binary;
}

static uint64_t binary_to_gray(uint64_t binary)
{
    return binary ^ (binary >> 1);
}

static uint64_t read_ostimer(QTestState *s)
{
    uint64_t raw = qtest_readl(s, OSTIMER);

    raw |= (uint64_t)qtest_readl(s, OSTIMER + 4) << 32;
    return gray_to_binary(raw);
}

static void enable_flexcomm0(QTestState *s)
{
    qtest_writel(s, CLKCTL1 + 0x040, BIT(8));
    qtest_writel(s, RSTCTL1 + 0x070, BIT(8));
    qtest_writel(s, FC0 + 0xff8, 1);
    qtest_writel(s, FC0 + 0x000, 1);
}

static void enable_flexcomm15_i2c(QTestState *s)
{
    qtest_writel(s, CLKCTL1 + 0x040, BIT(23));
    qtest_writel(s, RSTCTL1 + 0x070, BIT(23));
    qtest_writel(s, FC15 + 0xff8, 3);
    qtest_writel(s, FC15 + 0x800, BIT(0));
}

static void enable_flexcomm14_spi(QTestState *s)
{
    qtest_writel(s, CLKCTL1 + 0x040, BIT(22));
    qtest_writel(s, RSTCTL1 + 0x070, BIT(22));
    qtest_writel(s, FC14 + 0xff8, 2);
}

static void enable_gpio0(QTestState *s)
{
    qtest_writel(s, CLKCTL1 + 0x044, BIT(0));
    qtest_writel(s, RSTCTL1 + 0x074, BIT(0));
}

static void test_cpu_topology(void)
{
    QTestState *s = qtest_init("-machine mimxrt685-evk");
    QDict *response = qtest_qmp(s, "{'execute': 'query-cpus-fast'}");
    QList *cpus = qdict_get_qlist(response, "return");
    QDict *cpu;

    g_assert_cmpuint(qlist_size(cpus), ==, 1);
    cpu = qobject_to(QDict, qlist_peek(cpus));
    g_assert_cmpstr(qdict_get_str(cpu, "qom-type"), ==,
                    "cortex-m33-arm-cpu");

    qobject_unref(response);
    qtest_quit(s);
}

static void test_memory_alias(void)
{
    QTestState *s = qtest_init("-machine mimxrt685-evk");

    qtest_writel(s, 0x20000100, 0x12345678);
    g_assert_cmphex(qtest_readl(s, 0x00000100), ==, 0x12345678);

    qtest_writel(s, 0x0047fffc, 0xa5a55a5a);
    g_assert_cmphex(qtest_readl(s, 0x2047fffc), ==, 0xa5a55a5a);

    qtest_writel(s, 0x00020000, 0xfeedface);
    g_assert_cmphex(qtest_readl(s, 0x20020000), ==, 0xfeedface);

    /* The first words beyond both 4.5 MiB views are intentionally unmapped. */
    qtest_writel(s, 0x00480000, 0xffffffff);
    qtest_writel(s, 0x20480000, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, 0x0047fffc), ==, 0xa5a55a5a);
    g_assert_cmphex(qtest_readl(s, 0x2047fffc), ==, 0xa5a55a5a);
    qtest_quit(s);
}

static void assert_machine_rejects(const char *option, const char *value,
                                   const char *message)
{
    const char *qemu = qtest_qemu_binary(NULL);
    const char *args[] = {
        qemu, "-machine", "mimxrt685-evk", option, value,
        "-display", "none", NULL,
    };
    g_autofree char *standard_error = NULL;
    int wait_status;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &standard_error,
                               &wait_status, NULL));
    g_assert_true(WIFEXITED(wait_status));
    g_assert_cmpint(WEXITSTATUS(wait_status), !=, 0);
    g_assert_nonnull(strstr(standard_error, message));
}

static void test_machine_constraints(void)
{
    assert_machine_rejects("-smp", "2", "max CPUs supported");
    assert_machine_rejects("-cpu", "cortex-m4",
                           "only valid type is: cortex-m33");
}

static void test_reset_vector_execution(void)
{
    /*
     * Thumb program: store "PASS" in SRAM, print it via semihosting, then
     * request a clean semihosting exit.  This exercises vector fetch, SRAM
     * execution and the data alias without depending on an SDK toolchain.
     */
    static const uint8_t image[] = {
        0x00, 0x10, 0x00, 0x20, 0x09, 0x00, 0x00, 0x00,
        0x04, 0x4a, 0x05, 0x49, 0x11, 0x60, 0x04, 0x20,
        0x11, 0x46, 0xab, 0xbe, 0x18, 0x20, 0x03, 0x49,
        0xab, 0xbe, 0xfe, 0xe7, 0x00, 0x01, 0x00, 0x20,
        0x50, 0x41, 0x53, 0x53, 0x26, 0x00, 0x02, 0x00,
    };
    const char *qemu = qtest_qemu_binary(NULL);
    g_autofree char *kernel = NULL;
    g_autofree char *standard_output = NULL;
    g_autofree char *standard_error = NULL;
    const char *args[] = {
        qemu, "-machine", "mimxrt685-evk", "-kernel", "",
        "-display", "none",
        "-semihosting-config", "enable=on,target=native", NULL,
    };
    int fd;
    int wait_status;

    fd = g_file_open_tmp("mimxrt685-smoke-XXXXXX.bin", &kernel, NULL);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    g_assert_cmpint(close(fd), ==, 0);
    args[4] = kernel;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                               &standard_output, &standard_error,
                               &wait_status, NULL));
    g_assert_true(WIFEXITED(wait_status));
    g_assert_cmpint(WEXITSTATUS(wait_status), ==, 0);
    g_assert_cmpstr(standard_output, ==, "");
    g_assert_cmpstr(standard_error, ==, "PASS");
    unlink(kernel);
}

static void test_syscon_reset_values(void)
{
    QTestState *s = qtest_init("-machine mimxrt685-evk");

    g_assert_cmphex(qtest_readl(s, CLKCTL0 + 0x010), ==, 0x00000005);
    g_assert_cmphex(qtest_readl(s, CLKCTL0 + 0x100), ==, 0x00020410);
    g_assert_cmphex(qtest_readl(s, CLKCTL0 + 0x190), ==, 0x807bc4d4);
    g_assert_cmphex(qtest_readl(s, CLKCTL1 + 0x400), ==, 0x40000000);
    g_assert_cmphex(qtest_readl(s, SYSCTL0 + 0x00c), ==, 1);
    g_assert_cmphex(qtest_readl(s, SYSCTL0 + 0x064), ==, 0x000b0000);
    g_assert_cmphex(qtest_readl(s, SYSCTL0 + 0x610), ==, 0x03febc80);
    g_assert_cmphex(qtest_readl(s, SYSCTL1 + 0x030), ==, 5);
    qtest_quit(s);
}

static void test_set_clear_and_masks(void)
{
    QTestState *s = qtest_init("-machine mimxrt685-evk");

    qtest_writel(s, CLKCTL1 + 0x040, 0x00000100);
    g_assert_cmphex(qtest_readl(s, CLKCTL1 + 0x010), ==, 0x00000100);
    qtest_writel(s, CLKCTL1 + 0x070, 0x00000100);
    g_assert_cmphex(qtest_readl(s, CLKCTL1 + 0x010), ==, 0);

    qtest_writel(s, SYSCTL0 + 0x620, 1U << 16);
    g_assert_cmphex(qtest_readl(s, SYSCTL0 + 0x610), ==, 0x03ffbc80);
    qtest_writel(s, SYSCTL0 + 0x630, 1U << 16);
    g_assert_cmphex(qtest_readl(s, SYSCTL0 + 0x610), ==, 0x03febc80);

    qtest_writel(s, SYSCTL1 + 0x040, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, SYSCTL1 + 0x040), ==, 0x03030303);
    qtest_writel(s, SYSCTL0 + 0x064, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, SYSCTL0 + 0x064), ==, 0x000b0000);
    qtest_quit(s);
}

static void test_system_reset(void)
{
    QTestState *s = qtest_init("-machine mimxrt685-evk");

    qtest_writel(s, 0x20000400, 0xc001c0de);
    qtest_writel(s, CLKCTL0 + 0x430, 3);
    qtest_writel(s, SYSCTL0 + 0x00c, 0);
    qtest_writel(s, IOPCTL + 0x164, 0xabc);
    enable_flexcomm0(s);
    qtest_writel(s, FC0 + 0xe00, 3);

    qtest_system_reset(s);

    /* SRAM is retained across a system reset; peripheral state is not. */
    g_assert_cmphex(qtest_readl(s, 0x00000400), ==, 0xc001c0de);
    g_assert_cmphex(qtest_readl(s, CLKCTL0 + 0x430), ==, 0);
    g_assert_cmphex(qtest_readl(s, SYSCTL0 + 0x00c), ==, 1);
    g_assert_cmphex(qtest_readl(s, IOPCTL + 0x164), ==, 0x41);
    g_assert_cmphex(qtest_readl(s, FC0 + 0xff8), ==, FC_PSELID_PRESENT_ALL);
    qtest_quit(s);
}

static void test_reset_and_iopctl(void)
{
    QTestState *s = qtest_init("-machine mimxrt685-evk");

    g_assert_cmphex(qtest_readl(s, RSTCTL0 + 0x010), ==, 0x01f11f02);
    g_assert_cmphex(qtest_readl(s, RSTCTL1 + 0x010), ==, 0x01c0ff00);
    g_assert_cmphex(qtest_readl(s, RSTCTL1 + 0x014), ==, 0xb18100ff);
    qtest_writel(s, RSTCTL1 + 0x070, BIT(8));
    g_assert_cmphex(qtest_readl(s, RSTCTL1 + 0x010) & BIT(8), ==, 0);
    qtest_writel(s, RSTCTL1 + 0x040, BIT(8));
    g_assert_cmphex(qtest_readl(s, RSTCTL1 + 0x010) & BIT(8), ==, BIT(8));

    g_assert_cmphex(qtest_readl(s, IOPCTL + 0x164), ==, 0x41);
    qtest_writel(s, IOPCTL + 0x164, 0xffffffff);
    g_assert_cmphex(qtest_readl(s, IOPCTL + 0x164), ==, 0xfff);
    qtest_quit(s);
}

static void test_gpio_registers_and_irq(void)
{
    QTestState *s = qtest_init("-machine mimxrt685-evk");

    qtest_irq_intercept_in(s, "/machine/soc/armv7m");

    /* Clock-gated/reset ports ignore writes. */
    qtest_writel(s, GPIO + 0x2000, BIT(4));
    g_assert_cmphex(qtest_readl(s, GPIO + 0x2000), ==, 0);

    enable_gpio0(s);
    qtest_writel(s, GPIO + 0x2000, BIT(4));
    qtest_writel(s, GPIO + 0x2200, BIT(4));
    g_assert_cmphex(qtest_readl(s, GPIO + 0x2100), ==, BIT(4));
    g_assert_cmphex(qtest_readb(s, GPIO + 4), ==, 1);
    qtest_writeb(s, GPIO + 4, 0);
    g_assert_cmphex(qtest_readl(s, GPIO + 0x2100), ==, 0);

    /* Rising edge on GPIO0_3 sets status A and asserts GPIO interrupt A. */
    qtest_writel(s, GPIO + 0x2500, BIT(3));
    qtest_writel(s, GPIO + 0x2600, BIT(3));
    qtest_writel(s, GPIO + 0x2680, BIT(3));
    qtest_set_irq_in(s, "/machine/soc/gpio", NULL, 3, 1);
    g_assert_cmphex(qtest_readl(s, GPIO + 0x2700), ==, BIT(3));
    g_assert_true(qtest_get_irq(s, 2));
    qtest_writel(s, GPIO + 0x2700, BIT(3));
    g_assert_cmphex(qtest_readl(s, GPIO + 0x2700), ==, 0);
    g_assert_false(qtest_get_irq(s, 2));
    qtest_writel(s, RSTCTL1 + 0x044, BIT(0));
    g_assert_cmphex(qtest_readl(s, GPIO + 0x2000), ==, 0);
    qtest_writel(s, RSTCTL1 + 0x074, BIT(0));
    g_assert_cmphex(qtest_readl(s, GPIO + 0x2000), ==, 0);
    qtest_quit(s);
}

static void test_ostimer_counter_match_and_gating(void)
{
    QTestState *s = qtest_init("-machine mimxrt685-evk");
    uint64_t before, after, match, raw_match;

    qtest_irq_intercept_in(s, "/machine/soc/armv7m");

    /* CLKCTL1.PSCCTL0 bit 27 gates the 1 MHz OS/Event timer. */
    before = read_ostimer(s);
    qtest_clock_step(s, 10 * 1000);
    g_assert_cmpuint(read_ostimer(s), ==, before);
    qtest_writel(s, CLKCTL1 + 0x040, BIT(27));
    qtest_clock_step(s, 10 * 1000);
    after = read_ostimer(s);
    g_assert_cmpuint(after - before, ==, 10);

    /* MATCH is Gray encoded and raises OS_EVENT_IRQn (41). */
    match = after + 100;
    raw_match = binary_to_gray(match);
    qtest_writel(s, OSTIMER + 0x10, raw_match);
    qtest_writel(s, OSTIMER + 0x14, raw_match >> 32);
    qtest_writel(s, OSTIMER + 0x1c, BIT(1));
    qtest_clock_step(s, 99 * 1000);
    g_assert_false(qtest_get_irq(s, 41));
    qtest_clock_step(s, 1000);
    g_assert_cmphex(qtest_readl(s, OSTIMER + 0x1c), ==, BIT(0) | BIT(1));
    g_assert_true(qtest_get_irq(s, 41));
    qtest_writel(s, OSTIMER + 0x1c, BIT(0) | BIT(1));
    g_assert_false(qtest_get_irq(s, 41));

    /* Clock gating freezes the counter; reset clears local and count state. */
    qtest_writel(s, CLKCTL1 + 0x070, BIT(27));
    before = read_ostimer(s);
    qtest_clock_step(s, 20 * 1000);
    g_assert_cmpuint(read_ostimer(s), ==, before);
    qtest_writel(s, RSTCTL1 + 0x040, BIT(27));
    g_assert_cmpuint(read_ostimer(s), ==, 0);
    g_assert_cmphex(qtest_readl(s, OSTIMER + 0x10), ==, 0);
    g_assert_cmphex(qtest_readl(s, OSTIMER + 0x1c), ==, 0);

    qtest_quit(s);
}

static void test_flexcomm_topology(void)
{
    static const uint32_t fc_base[] = {
        0x40106000, 0x40107000, 0x40108000, 0x40109000,
        0x40122000, 0x40123000, 0x40124000, 0x40125000,
    };
    QTestState *s = qtest_init("-machine mimxrt685-evk");

    /* Flexcomm 0..7 integrate USART, SPI, I2C and I2S. */
    for (unsigned i = 0; i < ARRAY_SIZE(fc_base); i++) {
        g_assert_cmphex(qtest_readl(s, fc_base[i] + 0xff8), ==,
                        FC_PSELID_PRESENT_ALL);
    }
    /* Flexcomm 14 is SPI-only and Flexcomm 15 is I2C-only. */
    g_assert_cmphex(qtest_readl(s, FC14 + 0xff8), ==, FC_PSELID_PRESENT_SPI);
    g_assert_cmphex(qtest_readl(s, FC15 + 0xff8), ==, FC_PSELID_PRESENT_I2C);

    /* Selecting an absent function is rejected; 0 (none) is accepted. */
    qtest_writel(s, FC14 + 0xff8, 1);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xff8) & 0xf, ==, 0);
    qtest_writel(s, FC14 + 0xff8, 3);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xff8) & 0xf, ==, 0);
    qtest_writel(s, FC14 + 0xff8, 2);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xff8) & 0xf, ==, 2);
    qtest_writel(s, FC14 + 0xff8, 0);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xff8) & 0xf, ==, 0);

    qtest_writel(s, FC15 + 0xff8, 1);
    g_assert_cmphex(qtest_readl(s, FC15 + 0xff8) & 0xf, ==, 0);
    qtest_writel(s, FC15 + 0xff8, 2);
    g_assert_cmphex(qtest_readl(s, FC15 + 0xff8) & 0xf, ==, 0);
    qtest_writel(s, FC15 + 0xff8, 3);
    g_assert_cmphex(qtest_readl(s, FC15 + 0xff8) & 0xf, ==, 3);

    /* LOCK freezes PERSEL until a peripheral reset. */
    qtest_writel(s, FC15 + 0xff8, 3 | BIT(3));
    g_assert_cmphex(qtest_readl(s, FC15 + 0xff8) & 0xf, ==, 3 | BIT(3));
    qtest_writel(s, FC15 + 0xff8, 0);
    g_assert_cmphex(qtest_readl(s, FC15 + 0xff8) & 0xf, ==, 3 | BIT(3));

    /* CLKCTL1 bit 22/23 and RSTCTL1 bit 22/23 gate FC14/FC15. */
    qtest_writel(s, CLKCTL1 + 0x040, BIT(22));
    g_assert_cmphex(qtest_readl(s, CLKCTL1 + 0x010) & BIT(22), ==, BIT(22));
    qtest_writel(s, CLKCTL1 + 0x070, BIT(22));
    g_assert_cmphex(qtest_readl(s, CLKCTL1 + 0x010) & BIT(22), ==, 0);
    qtest_writel(s, RSTCTL1 + 0x070, BIT(23));
    g_assert_cmphex(qtest_readl(s, RSTCTL1 + 0x010) & BIT(23), ==, 0);
    qtest_writel(s, RSTCTL1 + 0x040, BIT(23));
    g_assert_cmphex(qtest_readl(s, RSTCTL1 + 0x010) & BIT(23), ==, BIT(23));

    /* Peripheral reset clears LOCK and restores the capability bits. */
    qtest_writel(s, RSTCTL1 + 0x040, BIT(23));
    g_assert_cmphex(qtest_readl(s, FC15 + 0xff8), ==, FC_PSELID_PRESENT_I2C);
    qtest_writel(s, RSTCTL1 + 0x070, BIT(23));
    qtest_quit(s);
}

static void test_flexcomm_uart(void)
{
    int sock_fd;
    int tries;
    char ch;
    QTestState *s = qtest_init_with_serial("-machine mimxrt685-evk",
                                           &sock_fd);

    qtest_irq_intercept_in(s, "/machine/soc/armv7m");

    g_assert_cmphex(qtest_readl(s, FC0 + 0xff8), ==, FC_PSELID_PRESENT_ALL);
    enable_flexcomm0(s);
    qtest_writel(s, FC0 + 0xe00, BIT(0) | BIT(1));
    qtest_writel(s, FC0 + 0xe20, 'T');
    g_assert_cmpint(recv(sock_fd, &ch, 1, 0), ==, 1);
    g_assert_cmpint(ch, ==, 'T');

    qtest_writel(s, FC0 + 0xe08, BIT(1));
    qtest_writel(s, FC0 + 0xe10, BIT(3));
    g_assert_cmpint(send(sock_fd, "R", 1, 0), ==, 1);
    for (tries = 0; tries < 1000 &&
         !(qtest_readl(s, FC0 + 0xe04) & BIT(6)); tries++) {
        g_usleep(1000);
    }
    g_assert_cmpint(tries, <, 1000);
    g_assert_cmphex(qtest_readl(s, FC0 + 0xe18) & BIT(3), ==, BIT(3));
    g_assert_true(qtest_get_irq(s, 14));
    g_assert_cmphex(qtest_readl(s, FC0 + 0xe30), ==, 'R');
    g_assert_false(qtest_get_irq(s, 14));

    qtest_writel(s, FC0 + 0xff8, 1 | BIT(3));
    qtest_writel(s, FC0 + 0xff8, 2);
    g_assert_cmphex(qtest_readl(s, FC0 + 0xff8) & 0xf, ==, 9);
    qtest_writel(s, RSTCTL1 + 0x040, BIT(8));
    g_assert_cmphex(qtest_readl(s, FC0 + 0xff8), ==, FC_PSELID_PRESENT_ALL);
    qtest_writel(s, RSTCTL1 + 0x070, BIT(8));
    qtest_writel(s, FC0 + 0xff8, 1);
    g_assert_cmphex(qtest_readl(s, FC0 + 0xe00), ==, 0);

    close(sock_fd);
    qtest_quit(s);
}

static void test_flexcomm_i2c(void)
{
    QTestState *s = qtest_init("-machine mimxrt685-evk "
                               "-device tmp105,bus=flexcomm15.i2c,"
                               "address=0x48");

    qtest_irq_intercept_in(s, "/machine/soc/armv7m");
    enable_flexcomm15_i2c(s);

    g_assert_cmphex(qtest_readl(s, FC15 + 0x804), ==, 0x00000801);
    g_assert_cmphex(qtest_readl(s, FC15 + 0x810), ==, 0x0000ffff);
    g_assert_cmphex(qtest_readl(s, FC15 + 0x824), ==, 0x00000077);
    g_assert_cmphex(qtest_readl(s, FC15 + 0x848), ==, 0x00000001);

    /* Pending is a level interrupt while the master waits for software. */
    qtest_writel(s, FC15 + 0x808, BIT(0));
    g_assert_cmphex(qtest_readl(s, FC15 + 0x818), ==, BIT(0));
    g_assert_true(qtest_get_irq(s, 21));
    qtest_writel(s, FC15 + 0x80c, BIT(0));
    g_assert_false(qtest_get_irq(s, 21));

    /* Select the TMP105 configuration register and write 0x60. */
    qtest_writel(s, FC15 + 0x828, 0x48 << 1);
    qtest_writel(s, FC15 + 0x820, BIT(1));
    g_assert_cmphex(qtest_readl(s, FC15 + 0x804) & 0xf, ==, 0x5);
    qtest_writel(s, FC15 + 0x828, 0x01);
    qtest_writel(s, FC15 + 0x820, BIT(0));
    qtest_writel(s, FC15 + 0x828, 0x60);
    qtest_writel(s, FC15 + 0x820, BIT(0));
    qtest_writel(s, FC15 + 0x820, BIT(2));
    g_assert_cmphex(qtest_readl(s, FC15 + 0x804) & 0xf, ==, 0x1);

    /* Read the value back using the SDK-style repeated START sequence. */
    qtest_writel(s, FC15 + 0x828, 0x48 << 1);
    qtest_writel(s, FC15 + 0x820, BIT(1));
    qtest_writel(s, FC15 + 0x828, 0x01);
    qtest_writel(s, FC15 + 0x820, BIT(0));
    qtest_writel(s, FC15 + 0x828, (0x48 << 1) | 1);
    qtest_writel(s, FC15 + 0x820, BIT(1));
    g_assert_cmphex(qtest_readl(s, FC15 + 0x804) & 0xf, ==, 0x3);
    g_assert_cmphex(qtest_readl(s, FC15 + 0x828), ==, 0x60);
    qtest_writel(s, FC15 + 0x820, BIT(2));

    /* Address NACK is reported as pending master state 3. */
    qtest_writel(s, FC15 + 0x828, 0x7f << 1);
    qtest_writel(s, FC15 + 0x820, BIT(1));
    g_assert_cmphex(qtest_readl(s, FC15 + 0x804) & 0xf, ==, 0x7);
    qtest_writel(s, FC15 + 0x820, BIT(2));

    qtest_writel(s, RSTCTL1 + 0x040, BIT(23));
    g_assert_cmphex(qtest_readl(s, FC15 + 0xff8), ==,
                    FC_PSELID_PRESENT_I2C);
    qtest_quit(s);
}

static void test_flexcomm_spi(void)
{
    uint32_t stat;
    QTestState *s = qtest_init("-machine mimxrt685-evk "
                               "-device m25p80,bus=flexcomm14.spi,cs=0");

    qtest_irq_intercept_in(s, "/machine/soc/armv7m");
    enable_flexcomm14_spi(s);

    g_assert_cmphex(qtest_readl(s, FC14 + 0x400), ==, 0);
    g_assert_cmphex(qtest_readl(s, FC14 + 0x408), ==, BIT(8));
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe00), ==, 0x20);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe04), ==, 0x30);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe48), ==, 16);

    /* An eight-bit loopback frame returns data and SSEL/SOT metadata. */
    qtest_writel(s, FC14 + 0x400, BIT(0) | BIT(2) | BIT(7));
    qtest_writel(s, FC14 + 0xe00, BIT(0) | BIT(1));
    qtest_writel(s, FC14 + 0xe20, (7U << 24) | (0xeU << 16) |
                                      BIT(20) | 0x5a);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe04), ==, 0x00010070);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe40), ==, 0x001e005a);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe30), ==, 0x001e005a);
    g_assert_cmphex(qtest_readl(s, FC14 + 0x408), ==,
                    BIT(8) | BIT(5) | BIT(4));
    qtest_writel(s, FC14 + 0x408, BIT(5) | BIT(4));

    /* Peripheral and FIFO level/error conditions drive IRQ 20. */
    qtest_writel(s, FC14 + 0x40c, BIT(8));
    g_assert_cmphex(qtest_readl(s, FC14 + 0x428), ==, BIT(8));
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe18) & BIT(4), ==, BIT(4));
    g_assert_true(qtest_get_irq(s, 20));
    qtest_writel(s, FC14 + 0x410, BIT(8));
    g_assert_false(qtest_get_irq(s, 20));

    qtest_writel(s, FC14 + 0xe08, BIT(1));
    qtest_writel(s, FC14 + 0xe10, BIT(3));
    qtest_writel(s, FC14 + 0xe20, (7U << 24) | (0xeU << 16) |
                                      BIT(20) | 0xa5);
    g_assert_true(qtest_get_irq(s, 20));
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe30) & 0xff, ==, 0xa5);
    g_assert_false(qtest_get_irq(s, 20));
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe30), ==, 0);
    qtest_writel(s, FC14 + 0xe10, BIT(1));
    g_assert_true(qtest_get_irq(s, 20));
    qtest_writel(s, FC14 + 0xe04, BIT(1));
    g_assert_false(qtest_get_irq(s, 20));

    /* A seventeenth received frame is dropped and marks RX overflow. */
    for (unsigned i = 0; i < 17; i++) {
        qtest_writel(s, FC14 + 0xe20, (7U << 24) | (0xeU << 16) |
                                          BIT(20) | i);
    }
    stat = qtest_readl(s, FC14 + 0xe04);
    g_assert_cmphex(stat & BIT(1), ==, BIT(1));
    g_assert_cmphex(stat & BIT(7), ==, BIT(7));
    g_assert_cmpuint(extract32(stat, 16, 5), ==, 16);
    qtest_writel(s, FC14 + 0xe00, BIT(17) | BIT(1) | BIT(0));
    qtest_writel(s, FC14 + 0xe04, BIT(1));

    /* Turn off loopback and read the attached M25P80 JEDEC ID. */
    qtest_writel(s, FC14 + 0xe08, 0);
    qtest_writel(s, FC14 + 0xe14, 0xf);
    qtest_writel(s, FC14 + 0x400, BIT(0) | BIT(2));
    qtest_writel(s, FC14 + 0xe20, (7U << 24) | (0xeU << 16) | 0x9f);
    qtest_readl(s, FC14 + 0xe30);
    qtest_writel(s, FC14 + 0xe20, (7U << 24) | (0xeU << 16));
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe30) & 0xff, ==, 0x20);
    qtest_writel(s, FC14 + 0xe20, (7U << 24) | (0xeU << 16));
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe30) & 0xff, ==, 0x20);
    qtest_writel(s, FC14 + 0xe20, (7U << 24) | (0xeU << 16) | BIT(20));
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe30) & 0xff, ==, 0x14);

    /* USART and SPI retain separate state behind the shared FIFO window. */
    enable_flexcomm0(s);
    qtest_writel(s, FC0 + 0xe00, BIT(0));
    qtest_writel(s, FC0 + 0xff8, 2);
    qtest_writel(s, FC0 + 0xe00, BIT(1));
    g_assert_cmphex(qtest_readl(s, FC0 + 0xe00), ==, 0x22);
    qtest_writel(s, FC0 + 0xff8, 1);
    g_assert_cmphex(qtest_readl(s, FC0 + 0xe00), ==, BIT(0));

    qtest_writel(s, RSTCTL1 + 0x040, BIT(22));
    g_assert_cmphex(qtest_readl(s, FC14 + 0xff8), ==,
                    FC_PSELID_PRESENT_SPI);
    qtest_quit(s);
}

static void test_flexcomm_spi_slave(void)
{
    uint32_t stat;
    QTestState *s = qtest_init("-machine mimxrt685-evk");

    qtest_irq_intercept_in(s, "/machine/soc/armv7m");
    enable_flexcomm14_spi(s);

    /* Start with all active-low SSEL inputs inactive, then enable slave. */
    for (unsigned i = 0; i < 4; i++) {
        qtest_set_irq_in(s, FC14_PATH, "spi-slave-ssel", i, 1);
    }
    qtest_writel(s, FC14 + 0xe00, BIT(0) | BIT(1));
    qtest_writel(s, FC14 + 0x400, BIT(0));
    g_assert_cmphex(qtest_readl(s, FC14 + 0x408), ==, BIT(8));

    /* SSEL assertion is sticky and can interrupt in slave mode. */
    qtest_writel(s, FC14 + 0x40c, BIT(4) | BIT(5));
    qtest_set_irq_in(s, FC14_PATH, "spi-slave-ssel", 0, 0);
    g_assert_cmphex(qtest_readl(s, FC14 + 0x408), ==, BIT(8) | BIT(4));
    g_assert_true(qtest_get_irq(s, 20));
    qtest_writel(s, FC14 + 0x408, BIT(4));
    g_assert_false(qtest_get_irq(s, 20));

    /* A master frame consumes one TX word and produces SSEL/SOT RX metadata. */
    qtest_writel(s, FC14 + 0xe20, (7U << 24) | 0xa5);
    stat = qtest_readl(s, FC14 + 0xe04);
    g_assert_cmpuint(extract32(stat, 8, 5), ==, 1);
    qtest_writel(s, FC14 + 0xe08, BIT(1));
    qtest_writel(s, FC14 + 0xe10, BIT(3));
    qtest_set_irq_in(s, FC14_PATH, "spi-slave-frame", 0, 0x5a);
    stat = qtest_readl(s, FC14 + 0xe04);
    g_assert_cmpuint(extract32(stat, 8, 5), ==, 0);
    g_assert_cmpuint(extract32(stat, 16, 5), ==, 1);
    g_assert_true(qtest_get_irq(s, 20));
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe40), ==, 0x001e005a);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe30), ==, 0x001e005a);
    g_assert_false(qtest_get_irq(s, 20));

    /* An unprepared slave transmission reports TX underrun. */
    qtest_writel(s, FC14 + 0xe10, BIT(0));
    qtest_set_irq_in(s, FC14_PATH, "spi-slave-frame", 0, 0x33);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe04) & BIT(0), ==, BIT(0));
    g_assert_true(qtest_get_irq(s, 20));
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe30), ==, 0x000e0033);
    g_assert_true(qtest_get_irq(s, 20));
    qtest_writel(s, FC14 + 0xe04, BIT(0));
    g_assert_false(qtest_get_irq(s, 20));

    /* TXIGNORE establishes persistent receive-only operation. */
    qtest_writel(s, FC14 + 0xe20, (7U << 24) | BIT(23));
    qtest_set_irq_in(s, FC14_PATH, "spi-slave-frame", 0, 0x44);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe04) & BIT(0), ==, 0);
    g_assert_cmphex(qtest_readl(s, FC14 + 0xe30), ==, 0x000e0044);

    /* RXIGNORE suppresses the corresponding received frame. */
    qtest_writel(s, FC14 + 0xe20, (7U << 24) | BIT(22) | 0x66);
    qtest_set_irq_in(s, FC14_PATH, "spi-slave-frame", 0, 0x55);
    g_assert_cmpuint(extract32(qtest_readl(s, FC14 + 0xe04), 16, 5),
                     ==, 0);

    /* A seventeenth queued slave response is rejected as TX overflow. */
    for (unsigned i = 0; i < 17; i++) {
        qtest_writel(s, FC14 + 0xe20, (7U << 24) | i);
    }
    stat = qtest_readl(s, FC14 + 0xe04);
    g_assert_cmphex(stat & BIT(0), ==, BIT(0));
    g_assert_cmpuint(extract32(stat, 8, 5), ==, 16);
    qtest_writel(s, FC14 + 0xe04, BIT(0));

    qtest_set_irq_in(s, FC14_PATH, "spi-slave-ssel", 0, 1);
    g_assert_cmphex(qtest_readl(s, FC14 + 0x408), ==, BIT(8) | BIT(5));
    g_assert_true(qtest_get_irq(s, 20));
    qtest_writel(s, FC14 + 0x408, BIT(5));
    g_assert_false(qtest_get_irq(s, 20));

    /* FIFO reset commands clear both independently maintained queues. */
    qtest_writel(s, FC14 + 0xe00,
                 BIT(16) | BIT(17) | BIT(0) | BIT(1));
    stat = qtest_readl(s, FC14 + 0xe04);
    g_assert_cmpuint(extract32(stat, 8, 5), ==, 0);
    g_assert_cmpuint(extract32(stat, 16, 5), ==, 0);

    qtest_quit(s);
}

static void test_flexcomm_spi_slave_miso(void)
{
    QTestState *s = qtest_init("-machine mimxrt685-evk");

    qtest_irq_intercept_out_named(s, FC14_PATH, "spi-slave-miso");
    enable_flexcomm14_spi(s);
    for (unsigned i = 0; i < 4; i++) {
        qtest_set_irq_in(s, FC14_PATH, "spi-slave-ssel", i, 1);
    }
    qtest_writel(s, FC14 + 0xe00, BIT(0) | BIT(1));
    qtest_writel(s, FC14 + 0x400, BIT(0));
    qtest_set_irq_in(s, FC14_PATH, "spi-slave-ssel", 0, 0);

    qtest_writel(s, FC14 + 0xe20, (7U << 24) | 0xa5);
    qtest_set_irq_in(s, FC14_PATH, "spi-slave-frame", 0, 0x5a);
    g_assert_true(qtest_get_irq(s, 0));

    qtest_writel(s, FC14 + 0xe20, (7U << 24));
    qtest_set_irq_in(s, FC14_PATH, "spi-slave-frame", 0, 0x5a);
    g_assert_false(qtest_get_irq(s, 0));

    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/mimxrt685/cpu-topology", test_cpu_topology);
    qtest_add_func("/mimxrt685/memory-alias", test_memory_alias);
    qtest_add_func("/mimxrt685/machine-constraints",
                   test_machine_constraints);
    qtest_add_func("/mimxrt685/reset-vector-execution",
                   test_reset_vector_execution);
    qtest_add_func("/mimxrt685/syscon-reset-values",
                   test_syscon_reset_values);
    qtest_add_func("/mimxrt685/set-clear-and-masks",
                   test_set_clear_and_masks);
    qtest_add_func("/mimxrt685/system-reset", test_system_reset);
    qtest_add_func("/mimxrt685/reset-and-iopctl", test_reset_and_iopctl);
    qtest_add_func("/mimxrt685/gpio-registers-and-irq",
                   test_gpio_registers_and_irq);
    qtest_add_func("/mimxrt685/ostimer-counter-match-and-gating",
                   test_ostimer_counter_match_and_gating);
    qtest_add_func("/mimxrt685/flexcomm-topology", test_flexcomm_topology);
    qtest_add_func("/mimxrt685/flexcomm-uart", test_flexcomm_uart);
    qtest_add_func("/mimxrt685/flexcomm-i2c", test_flexcomm_i2c);
    qtest_add_func("/mimxrt685/flexcomm-spi", test_flexcomm_spi);
    qtest_add_func("/mimxrt685/flexcomm-spi-slave",
                   test_flexcomm_spi_slave);
    qtest_add_func("/mimxrt685/flexcomm-spi-slave-miso",
                   test_flexcomm_spi_slave_miso);
    return g_test_run();
}
