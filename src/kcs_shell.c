/*
 * Bring-up diagnostics for the IPMI-over-KCS path.
 *
 * There is no way to see the eSPI/LPC KCS traffic from the BMC shell -- the
 * host drives I/O 0xCA2/0xCA3 over the eSPI link.  These commands poke the
 * same software register file that the host-facing half
 * (soc/linkedsemi/common/espi_lpc/espi_lpc_host_kcs.c) writes, so the whole
 * chain "register file -> IPMI KCS state machine -> kcsbridge -> ipmid ->
 * D-Bus reply -> register file" can be exercised without the x86 host.
 *
 *   kcsbridge env                     dump the register file
 *   kcsbridge hostwr <data|cmd> <hex> simulate one host I/O write
 *   kcsbridge hostrd                  simulate one host data read
 *   kcsbridge selftest                inject Get Self Test Results, print reply
 *
 * Note: the commands are not atomic against a live host.  Run them with the
 * host powered off (or before it starts polling KCS).
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>

#include <string.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(ipmi_kcs), okay)

#define IPMI_NODE DT_NODELABEL(ipmi_kcs)
#define KCS_NODE DT_PHANDLE(IPMI_NODE, kcs_chan)
#define KCS_ENV_ADDR DT_PROP(KCS_NODE, kcs_env_addr)

/* Layout of struct host_kcs_env, see
 * zephyr/soc/linkedsemi/common/espi_lpc/espi_lpc_common.h.  Kept as a local
 * mirror so this file does not depend on the soc private include path. */
struct kcs_env_shadow
{
    uint32_t lock;
    uint8_t status;
    uint8_t data_out;
    uint8_t data_in;
};

#define ENV ((volatile struct kcs_env_shadow *)KCS_ENV_ADDR)

/* Catch the mirror drifting away from the real struct. */
BUILD_ASSERT(sizeof(struct kcs_env_shadow) == 8,
             "host_kcs_env layout changed, update the mirror above");

/* IPMI 2.0 table 9-1 */
#define KCS_STR_OBF BIT(0)
#define KCS_STR_IBF BIT(1)
#define KCS_STR_SMS_ATN BIT(2)
#define KCS_STR_CMD_DAT BIT(3)

/* IPMI 2.0 table 9-2, KCS interface state bits (status register 7:6) */
#define KCS_STR_STATE_MASK (BIT(7) | BIT(6))

/* IPMI 2.0 table 9-3 */
#define KCS_CMD_WRITE_START 0x61
#define KCS_CMD_WRITE_END 0x62
#define KCS_CMD_READ_BYTE 0x68

/* Peer callback of the BMC KCS register shim (drivers/kcs/kcs_ls.c).  This is
 * exactly what the host-facing half calls after a host I/O write; it runs the
 * IPMI KCS state machine and raises IPMI_RX_EVENT when a request is
 * complete. */
extern void bmc_kcs_rx_callback(const struct device *dev, void *msg);

static const struct device *kcs_dev(void)
{
    return DEVICE_DT_GET(KCS_NODE);
}

static void print_status(const struct shell *sh, uint8_t status)
{
    shell_print(sh, "  status 0x%02x:%s%s%s%s", status,
                (status & KCS_STR_IBF) ? " IBF" : "",
                (status & KCS_STR_OBF) ? " OBF" : "",
                (status & KCS_STR_CMD_DAT) ? " CMD_DAT" : "",
                (status & KCS_STR_SMS_ATN) ? " SMS_ATN" : "");
}

/* One host write: either the data port (0xCA2) or the command/status port
 * (0xCA3).  Mirrors data_cmd_stt_iowr() in espi_lpc_host_kcs.c. */
static void host_write(uint8_t byte, bool is_data)
{
    ENV->data_in = byte;
    ENV->status |= KCS_STR_IBF;
    if (is_data)
    {
        ENV->status &= (uint8_t)~KCS_STR_CMD_DAT;
    }
    else
    {
        ENV->status |= KCS_STR_CMD_DAT;
    }
    bmc_kcs_rx_callback(kcs_dev(), NULL);
}

/* One host read of the data port: returns the BMC byte and clears OBF,
 * mirroring data_io_read(). */
static int host_read(uint8_t *out)
{
    if ((ENV->status & KCS_STR_OBF) == 0)
    {
        return -1;
    }
    *out = ENV->data_out;
    ENV->status &= (uint8_t)~KCS_STR_OBF;
    return 0;
}

static int cmd_env(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "KCS register file @ 0x%08x (%s)", KCS_ENV_ADDR,
                kcs_dev()->name);
    print_status(sh, ENV->status);
    shell_print(sh, "  data_out 0x%02x, data_in 0x%02x", ENV->data_out,
                ENV->data_in);
    return 0;
}

static int cmd_hostw(const struct shell *sh, size_t argc, char **argv)
{
    unsigned long value = 0;
    int err = 0;
    bool is_data;

    ARG_UNUSED(argc);

    value = shell_strtoul(argv[2], 16, &err);
    if (err != 0 || value > 0xff)
    {
        shell_error(sh, "byte must be a hex value 00..ff");
        return -EINVAL;
    }

    if (strcmp(argv[1], "data") == 0)
    {
        is_data = true;
    }
    else if (strcmp(argv[1], "cmd") == 0)
    {
        is_data = false;
    }
    else
    {
        shell_error(sh, "port must be 'data' (0xCA2) or 'cmd' (0xCA3)");
        return -EINVAL;
    }

    host_write((uint8_t)value, is_data);
    shell_print(sh, "host write %s <- 0x%02lx", argv[1], value);
    print_status(sh, ENV->status);
    return 0;
}

static int cmd_hostr(const struct shell *sh, size_t argc, char **argv)
{
    uint8_t byte = 0;

    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    if (host_read(&byte) != 0)
    {
        shell_error(sh, "no OBF: nothing to read");
        print_status(sh, ENV->status);
        return -ENODATA;
    }
    shell_print(sh, "host read 0x%02x", byte);
    print_status(sh, ENV->status);
    return 0;
}

static int cmd_selftest(const struct shell *sh, size_t argc, char **argv)
{
    /* IPMI: netfn App (0x06), lun 0, cmd Get Self Test Results (0x04).
     * Expected reply: [ (0x06|1)<<2 | 0, 0x04, cc, data... ]. */
    const uint8_t netfn = 0x06;
    const uint8_t lun = 0;
    const uint8_t ipmi_cmd = 0x04;
    uint8_t byte = 0;
    int n = 0;

    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    /* Whatever an earlier run left behind must go first: while OBF is set,
     * kcs_ls_write_data() rejects the next byte and the driver drops it
     * silently. */
    if (host_read(&byte) == 0)
    {
        shell_print(sh, "drained stale byte 0x%02x", byte);
    }
    print_status(sh, ENV->status);

    /* KCS write phase (IPMI 2.0 table 9-5): WRITE_START, first byte,
     * WRITE_END, last byte.  The last data byte completes the request and
     * wakes the bridge. */
    host_write(KCS_CMD_WRITE_START, false);
    host_write((uint8_t)((netfn << 2) | lun), true);
    host_write(KCS_CMD_WRITE_END, false);
    host_write(ipmi_cmd, true);
    print_status(sh, ENV->status);

    /* Each command write is acknowledged by the driver with a dummy 0x00 in
     * the data register (OBF set).  The real response cannot be there yet, so
     * drop it. */
    if ((ENV->status & KCS_STR_OBF) != 0)
    {
        host_read(&byte);
        shell_print(sh, "dropped write-phase dummy 0x%02x", byte);
    }

    /* Wait for the bridge + ipmid to produce the first response byte. */
    int64_t deadline = k_uptime_get() + 2000;

    while (((ENV->status & KCS_STR_OBF) == 0) && (k_uptime_get() < deadline))
    {
        k_sleep(K_MSEC(2));
    }
    if ((ENV->status & KCS_STR_OBF) == 0)
    {
        shell_error(sh,
                    "no response within 2s: check the 'KCS req' / "
                    "'KCS response' lines on the console");
        print_status(sh, ENV->status);
        return -EIO;
    }

    /* KCS read phase (IPMI 2.0 table 9-6).  Note this driver handles READ_BYTE
     * (0x68) in its *data* path (ipmi_kcs_handle_data), not in the command
     * path: writing 0x68 to 0xCA3 instead of 0xCA2 gets you force_abort()
     * and a 0xC0 (ERROR) state. */
    for (int i = 0; i < 64; i++)
    {
        host_read(&byte);
        shell_print(sh, "  [%02d] 0x%02x", n, byte);
        n++;

        /* The driver flips the state bits to IDLE right before handing out the
         * trailing dummy byte, which is the only reliable end-of-response
         * signal (the dummy itself is 0x00 and a real payload may end in 0x00
         * too). */
        if ((ENV->status & (KCS_STR_STATE_MASK)) == 0)
        {
            shell_print(sh, "end of response (KCS dummy byte)");
            break;
        }

        host_write(KCS_CMD_READ_BYTE, true);

        deadline = k_uptime_get() + 500;
        while (((ENV->status & KCS_STR_OBF) == 0) &&
               (k_uptime_get() < deadline))
        {
            k_sleep(K_MSEC(2));
        }
        if ((ENV->status & KCS_STR_OBF) == 0)
        {
            shell_print(sh, "end of response after %d bytes", n);
            break;
        }
    }

    print_status(sh, ENV->status);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    kcsbridge_cmds,
    SHELL_CMD_ARG(env, NULL, "Dump the KCS register file", cmd_env, 1, 0),
    SHELL_CMD_ARG(hostwr, NULL,
                  "Simulate a host I/O write: hostwr <data|cmd> <hexbyte>",
                  cmd_hostw, 3, 0),
    SHELL_CMD_ARG(hostrd, NULL, "Simulate a host read of the data port",
                  cmd_hostr, 1, 0),
    SHELL_CMD_ARG(selftest, NULL,
                  "Inject Get Self Test Results and print the reply",
                  cmd_selftest, 1, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(kcsbridge, &kcsbridge_cmds, "IPMI over KCS diagnostics",
                   NULL);

#endif /* DT_NODE_HAS_STATUS(DT_NODELABEL(ipmi_kcs), okay) */
