#include "cmd.hpp"
#include "server.hpp"
#include "ipmi_bmc.h"

#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/slot.hpp>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/ipmi.h>
#include <zephyr/drivers/kcs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <cstdio>
#include <stdexcept>
#include <string>

extern struct k_sem kcs_bridge_ready_sem;
extern "C" int connect_to_dbroker(sd_bus **bus);
extern "C" void bmc_kcs_rx_callback(const struct device *dev, void *msg);

/* Global context for workqueue handler */
static const struct device *g_kcs_dev = nullptr;
static const struct device *g_raw_kcs = nullptr;
static sdbusplus::bus_t *g_bus = nullptr;
static sdbusplus::slot_t *g_slot = nullptr;

static K_KERNEL_STACK_DEFINE(kcs_work_stack, 4096);
static struct k_work_q kcs_work_q;
static struct k_work kcs_data_work;
static struct k_work_delayable kcs_test_work;

static void kcs_work_handler(struct k_work *)
{
    kcsbridge::kcs_read(g_kcs_dev, *g_bus, *g_slot);
}

static int kcs_ipmi_callback(const struct device *dev, void *cb_data, ipmi_event_type_t type)
{
    k_work_submit_to_queue(&kcs_work_q, &kcs_data_work);
    return 0;
}

static void kcs_test(const struct device *ipmi_dev, const struct device *raw_kcs_dev)
{
    printk("KCS test: injecting Self Test\n");
    volatile uint8_t *base = (volatile uint8_t *)0x40045d00;
    *(volatile uint32_t *)(base + 0) = 0;
    *(volatile uint32_t *)(base + 4) = 0;

    *(base + 6) = 0x61; *(base + 4) = KCS_IBF | KCS_CMD_DAT;
    bmc_kcs_rx_callback(raw_kcs_dev, nullptr); k_sleep(K_MSEC(2));

    *(base + 6) = (0x06 << 2) | 0; *(base + 4) = KCS_IBF;
    bmc_kcs_rx_callback(raw_kcs_dev, nullptr); k_sleep(K_MSEC(2));

    *(base + 6) = 0x62; *(base + 4) = KCS_IBF | KCS_CMD_DAT;
    bmc_kcs_rx_callback(raw_kcs_dev, nullptr); k_sleep(K_MSEC(2));

    *(base + 6) = 0x04; *(base + 4) = KCS_IBF;
    bmc_kcs_rx_callback(raw_kcs_dev, nullptr); k_sleep(K_MSEC(2));

    printk("KCS test: injection done\n");
}

static void kcs_test_work_handler(struct k_work *)
{
    kcs_test(g_kcs_dev, g_raw_kcs);
}

namespace kcsbridge
{

int execute()
{
    const struct device *kcs_dev = DEVICE_DT_GET(DT_NODELABEL(ipmi_kcs));
    if (!device_is_ready(kcs_dev)) {
        printk("KCS device not ready\n");
        return -ENODEV;
    }

    /* Connect to dbus-broker */
    sd_bus *dbus = nullptr;
    int ret = connect_to_dbroker(&dbus);
    if (ret < 0) {
        printk("Failed to connect: %d\n", ret);
        return ret;
    }

    /* Asio event loop — properly polls the bus fd on Zephyr */
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io, dbus);

    sdbusplus::slot_t slot(nullptr);

    /* Workqueue for KCS data processing (IPMI callback may be in ISR) */
    k_work_queue_init(&kcs_work_q);
    k_work_queue_start(&kcs_work_q, kcs_work_stack,
                       K_KERNEL_STACK_SIZEOF(kcs_work_stack),
                       K_PRIO_COOP(8), nullptr);
    k_work_init(&kcs_data_work, kcs_work_handler);
    k_work_init_delayable(&kcs_test_work, kcs_test_work_handler);

    g_kcs_dev = kcs_dev;
    g_raw_kcs = DEVICE_DT_GET(DT_NODELABEL(espi1_kcs0));
    g_bus = conn.get();  // sdbusplus::asio::connection is-a bus_t
    g_slot = &slot;

    ret = ipmi_set_callback(kcs_dev, kcs_ipmi_callback, nullptr);
    if (ret < 0) {
        printk("Failed to set KCS callback: %d\n", ret);
        return ret;
    }

    /* Register vtable using the bus_t's process_loop discarding.
     * Keep intf alive (execute() never returns, so auto intf stays in scope).
     */
    auto intf = createSMSHandler(
        *conn, "/xyz/openbmc_project/Ipmi/Channel/ipmi_kcs3", kcs_dev);
    conn->request_name("xyz.openbmc_project.Ipmi.Channel.ipmi_kcs3");

    printk("KCS bridge ready\n");
    k_sem_give(&kcs_bridge_ready_sem);

    /* Schedule self-test on workqueue after 5s */
    k_work_schedule_for_queue(&kcs_work_q, &kcs_test_work, K_SECONDS(2));

    io.run();
    return 0;
}

} // namespace kcsbridge

extern "C" int kcsbridge_main(void)
{
    return kcsbridge::execute();
}
