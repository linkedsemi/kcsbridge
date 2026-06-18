/*
 * KCS bridge - Zephyr port
 *
 * Bridges between the KCS hardware interface and the IPMI host service
 * via D-Bus. Runs as a persistent thread, similar to ipmid.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/ipmi.h>
#include <zephyr/drivers/kcs.h>
#include <zephyr/logging/log.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/vtable.hpp>

#include <boost/asio/io_context.hpp>

#include <array>
#include <map>
#include <variant>
#include <vector>
#include <string>
#include <cstring>

LOG_MODULE_REGISTER(kcsbridge, CONFIG_KCSBRIDGE_LOG_LEVEL);

extern "C" int connect_to_dbroker(sd_bus **bus);
extern "C" void bmc_kcs_rx_callback(const struct device *dev, void *msg);
extern struct k_sem kcs_bridge_ready_sem;

/* KCS status register bits (from IPMI 2.0 spec Table 9-1) */
#define KCS_STATUS_SMS_ATN BIT(2)

/* Per-instance context */
struct KcsContext {
    const struct device *ipmi_dev;   /* IPMI KCS device */
    sd_bus *bus;                     /* D-Bus connection (shared with asio) */
    // sd_bus *work_bus;                /* D-Bus connection (workqueue only, own fd) */
    boost::asio::io_context *io;     /* asio event loop */
    uint8_t buffer[1024];            /* IPMI request/response buffer */
};

static KcsContext kcs_ctx;

/* ------------------------------------------------------------------ */
/* D-Bus method handlers (SMS interface)                              */
/* ------------------------------------------------------------------ */

static int method_setAttention(sd_bus_message *m, void *userdata,
                               sd_bus_error *ret_error)
{
    auto *ctx = static_cast<KcsContext *>(userdata);
    ipmi_update_status(ctx->ipmi_dev, KCS_STATUS_SMS_ATN, KCS_STATUS_SMS_ATN);
    return sd_bus_reply_method_return(m, "");
}

static int method_clearAttention(sd_bus_message *m, void *userdata,
                                 sd_bus_error *ret_error)
{
    auto *ctx = static_cast<KcsContext *>(userdata);
    ipmi_update_status(ctx->ipmi_dev, KCS_STATUS_SMS_ATN, 0);
    return sd_bus_reply_method_return(m, "");
}

static int method_forceAbort(sd_bus_message *m, void *userdata,
                             sd_bus_error *ret_error)
{
    auto *ctx = static_cast<KcsContext *>(userdata);
    ipmi_force_abort(ctx->ipmi_dev);
    return sd_bus_reply_method_return(m, "");
}

/* Vtable for the SMS interface (basu format) */
static const sd_bus_vtable sms_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("setAttention", "", "",
                  method_setAttention, 0),
    SD_BUS_METHOD("clearAttention", "", "",
                  method_clearAttention, 0),
    SD_BUS_METHOD("forceAbort", "", "",
                  method_forceAbort, 0),
    SD_BUS_VTABLE_END
};

/* ------------------------------------------------------------------ */
/* IPMI request processing                                            */
/* ------------------------------------------------------------------ */

/** Forward an IPMI request to host_ipmid and write back the response. */
static int process_kcs_request(KcsContext *ctx)
{
    int ret;

    /* Read the IPMI request from KCS */
    ret = ipmi_read(ctx->ipmi_dev, ctx->buffer, sizeof(ctx->buffer));
    if (ret <= 0) {
        return ret;
    }

    size_t req_len = ret;
    if (req_len < 2) {
        LOG_WRN("KCS request too short: %zu bytes", req_len);
        return -EINVAL;
    }

    uint8_t netfn = ctx->buffer[0] >> 2;
    uint8_t lun   = ctx->buffer[0] & 3;
    uint8_t cmd   = ctx->buffer[1];

    printk("KCS req: netfn=0x%02x lun=%d cmd=0x%02x len=%zu\n",
            netfn, lun, cmd, req_len);

    /* Call host_ipmid.execute via D-Bus */
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *call = nullptr;
    sd_bus_message *reply = nullptr;
    int ret2;

    ret = sd_bus_message_new_method_call(
        ctx->bus, &call,
        "xyz.openbmc_project.Ipmi.Host",
        "/xyz/openbmc_project/Ipmi",
        "xyz.openbmc_project.Ipmi.Server",
        "execute");
    if (ret < 0)
        goto cleanup;

    /* Append (netfn, lun, cmd) as 3 bytes */
    ret = sd_bus_message_append(call, "yyy", netfn, lun, cmd);
    if (ret < 0)
        goto cleanup;

    /* Append data array (can be empty) */
    ret = sd_bus_message_append_array(call, 'y', ctx->buffer + 2, req_len - 2);
    if (ret < 0)
        goto cleanup;

    /* Empty options dict a{sv} */
    ret = sd_bus_message_open_container(call, 'a', "{sv}");
    if (ret < 0)
        goto cleanup;
    ret = sd_bus_message_close_container(call);
    if (ret < 0)
        goto cleanup;

    /* Synchronous call with 5s timeout */
    ret = sd_bus_call(ctx->bus, call, 5000000, &error, &reply);
    if (ret < 0) {
        LOG_ERR("execute call failed: %s", error.message);
        ipmi_write(ctx->ipmi_dev, ctx->buffer, 3);
        goto cleanup;
    }

    /* Parse the response: struct{yyyyay} = (r_netfn, r_lun, r_cmd, cc, data[]) */
    {
        uint8_t r_netfn, r_lun, r_cmd, r_cc;
        uint8_t *r_data = nullptr;
        size_t r_data_len = 0;
        size_t resp_len;

        ret2 = sd_bus_message_enter_container(reply, SD_BUS_TYPE_STRUCT,
                                               "yyyyay");
        if (ret2 < 0) {
            LOG_ERR("Failed to enter response struct");
            goto resp_error;
        }
        ret2 = sd_bus_message_read(reply, "yyyy",
                                   &r_netfn, &r_lun, &r_cmd, &r_cc);
        if (ret2 < 0) {
            LOG_ERR("Failed to read response header");
            goto resp_error;
        }
        ret2 = sd_bus_message_read_array(reply, 'y',
                                         (const void **)&r_data, &r_data_len);
        if (ret2 < 0) {
            LOG_ERR("Failed to read response data");
            goto resp_error;
        }
        sd_bus_message_exit_container(reply);

        /* Build KCS response: (netfn|1)<<2 | lun, cmd, cc, data... */
        resp_len = 3 + r_data_len;
        ctx->buffer[0] = (r_netfn | 1) << 2 | r_lun;
        ctx->buffer[1] = r_cmd;
        ctx->buffer[2] = r_cc;
        if (r_data_len > 0)
            memcpy(ctx->buffer + 3, r_data, r_data_len);

        ipmi_write(ctx->ipmi_dev, ctx->buffer, resp_len);
        printk("KCS resp: cc=0x%02x len=%zu data=", r_cc, resp_len);
        for (size_t i = 0; i < resp_len; i++)
            printk("%02x ", ctx->buffer[i]);
        printk("\n");
    }
    goto cleanup;

resp_error:
    ctx->buffer[0] = (netfn | 1) << 2 | lun;
    ctx->buffer[1] = cmd;
    ctx->buffer[2] = 0xff;
    ipmi_write(ctx->ipmi_dev, ctx->buffer, 3);

cleanup:
    sd_bus_error_free(&error);
    sd_bus_message_unref(call);
    sd_bus_message_unref(reply);
    return ret;
}

/* ------------------------------------------------------------------ */
/* KCS event handler (called from interrupt/workqueue context)        */
/* ------------------------------------------------------------------ */

static void kcs_data_work_handler(struct k_work *work);

static K_KERNEL_STACK_DEFINE(kcs_work_stack, 4096);
static struct k_work_q kcs_work_q;
static struct k_work kcs_data_work;

static void kcs_data_work_handler(struct k_work *work)
{
    process_kcs_request(&kcs_ctx);
}

/** Callback from the IPMI KCS driver when a full request is received. */
static int kcs_rx_callback(const struct device *dev, void *param,
                           ipmi_event_type_t type)
{
    if (type != IPMI_RX_EVENT)
        return -EINVAL;

    /* Submit to workqueue for deferred processing (not in ISR) */
    k_work_submit_to_queue(&kcs_work_q, &kcs_data_work);
    return 0;
}

static void kcs_test(void)
{
    printk("KCS self-test: injecting Get Device ID via shared memory...\n");

    const struct device *kcs_dev = DEVICE_DT_GET(DT_NODELABEL(espi1_kcs0));
    
    if (!device_is_ready(kcs_dev)) {
        LOG_ERR("  kcs_dev not ready, skip self-test");
    }
    else 
    {
        /* Shared memory layout at 0x40045d00:
            *   offset 0: lock (4 bytes, atomic_t)
            *   offset 4: status (uint8_t)
            *   offset 5: data_out (uint8_t)
            *   offset 6: data_in (uint8_t)
            */
        volatile uint8_t *kcs_base = (volatile uint8_t *)0x40045d00;
        volatile uint8_t *kcs_status  = kcs_base + 4;
        volatile uint8_t *kcs_data_in = kcs_base + 6;

        /* Clear shared memory lock + status (offset 0..7) before injection.
         * The lock field (atomic_t, offset 0) must be 0, otherwise
         * kcs_env_lock() in the KCS driver will spin forever.
         */
        *(volatile uint32_t *)(kcs_base + 0) = 0;
        *(volatile uint32_t *)(kcs_base + 4) = 0;

        /* Step 1: WRITE_START (0x61) - CMD_DAT=1 */
        *kcs_data_in = 0x61;
        *kcs_status  = KCS_IBF | KCS_CMD_DAT;
        bmc_kcs_rx_callback(kcs_dev, nullptr);
        k_sleep(K_MSEC(10));

        /* Step 2: netfn<<2|lun = 0x18 - CMD_DAT=0 (data) */
        *kcs_data_in = (0x06 << 2) | 0;
        *kcs_status  = KCS_IBF;
        bmc_kcs_rx_callback(kcs_dev, nullptr);
        k_sleep(K_MSEC(10));

        /* Step 3: WRITE_END (0x62) - CMD_DAT=1 */
        *kcs_data_in = 0x62;
        *kcs_status  = KCS_IBF | KCS_CMD_DAT;
        bmc_kcs_rx_callback(kcs_dev, nullptr);
        k_sleep(K_MSEC(10));

        /* Step 4: cmd = 0x04 (last data byte) - CMD_DAT=0 */
        *kcs_data_in = 0x04;
        *kcs_status  = KCS_IBF;
        bmc_kcs_rx_callback(kcs_dev, nullptr);
        k_sleep(K_MSEC(10));

        /* 等自注入处理完成 */
        k_sleep(K_MSEC(500));

        /* 读 KCS status - 看 OBF 位 */
        volatile uint8_t *kcs_status_r = kcs_base + 4;
        printk("KCS self-test: status = 0x%02x\n", *kcs_status_r);
    }
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                   */
/* ------------------------------------------------------------------ */

extern "C" int kcsbridge_main(void)
{
    int ret;

    LOG_INF("KCS bridge starting");

    /* Find the KCS device */
    const struct device *ipmi_dev = DEVICE_DT_GET(DT_NODELABEL(ipmi_kcs));
    if (!device_is_ready(ipmi_dev)) {
        LOG_ERR("KCS device not ready");
        return -ENODEV;
    }
    kcs_ctx.ipmi_dev = ipmi_dev;

    /* Connect to dbus-broker */
    ret = connect_to_dbroker(&kcs_ctx.bus);
    if (ret < 0) {
        LOG_ERR("Failed to connect to dbus-broker: %d", ret);
        return ret;
    }

    /* Setup asio event loop */
    boost::asio::io_context io;
    kcs_ctx.io = &io;

    auto conn = std::make_shared<sdbusplus::asio::connection>(io, kcs_ctx.bus);

    /* Request D-Bus name (set short timeout to avoid 25s default wait) */
    // sd_bus_set_method_call_timeout(kcs_ctx.bus, 5000000);  /* 5s timeout */
    conn->request_name("xyz.openbmc_project.Ipmi.Channel.ipmi_kcs3");
    // sd_bus_set_method_call_timeout(kcs_ctx.bus, 0);        /* restore default */

    /* Register SMS handler vtable on /xyz/openbmc_project/Ipmi/Channel/ipmi_kcs3 */
    sd_bus_add_object_vtable(kcs_ctx.bus, nullptr,
                             "/xyz/openbmc_project/Ipmi/Channel/ipmi_kcs3",
                             "xyz.openbmc_project.Ipmi.Channel.SMS",
                             sms_vtable, &kcs_ctx);

    /* Initialise a dedicated work queue for KCS processing */
    k_work_queue_init(&kcs_work_q);
    k_work_queue_start(&kcs_work_q, kcs_work_stack,
                       K_KERNEL_STACK_SIZEOF(kcs_work_stack),
                       K_PRIO_COOP(8), nullptr);

    /* Initialise the work item */
    k_work_init(&kcs_data_work, kcs_data_work_handler);

    /* Create a separate bus connection for the workqueue thread.
     * Must NOT share kcs_ctx.bus fd with asio event loop, otherwise
     * sd_bus_call() in the workqueue and asio poll the same fd -> race.
     */
    // ret = connect_to_dbroker(&kcs_ctx.work_bus);
    // if (ret < 0) {
    //     LOG_ERR("Failed to connect workqueue bus: %d", ret);
    //     kcs_ctx.work_bus = kcs_ctx.bus;  /* fallback to shared */
    // } else {
    //     printk("  workqueue D-Bus connected\n");
    // }

    /* Register KCS receive callback */
    ret = ipmi_set_callback(ipmi_dev, kcs_rx_callback, &kcs_ctx);
    if (ret < 0) {
        LOG_ERR("Failed to set KCS callback: %d", ret);
        return ret;
    }

    printk("KCS bridge ready on %s\n", ipmi_dev->name);
    k_sem_give(&kcs_bridge_ready_sem);

    /* --- KCS self-test: inject a Get Device ID request after 2s --- */
    k_sleep(K_SECONDS(2));
    kcs_test();
    /* --- End self-test --- */

    /* Run event loop - serves D-Bus method calls */
    io.run();

    /* Should not reach here */
    LOG_ERR("KCS bridge event loop exited");
    return 0;
}