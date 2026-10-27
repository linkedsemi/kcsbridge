#include "cmd.hpp"
#include "kcs_channel.hpp"
#include "server.hpp"

#ifdef __ZEPHYR__

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/ipmi.h>
#include <zephyr/kernel.h>

#else

#include "args.hpp"

#include <systemd/sd-daemon.h>

#include <sys/epoll.h>

#include <sdbusplus/bus.hpp>
#include <sdbusplus/slot.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/io.hpp>
#include <sdeventplus/source/signal.hpp>
#include <stdplus/exception.hpp>
#include <stdplus/fd/create.hpp>
#include <stdplus/print.hpp>
#include <stdplus/signal.hpp>

#endif

#include <algorithm>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef __ZEPHYR__
/* Provided by openbmc/openbmc_zephyr_app (app/src/dbus_broker.c and
 * app/src/task_def.cpp). */
extern struct k_sem kcs_bridge_ready_sem;
extern "C" int connect_to_dbroker(sd_bus** bus);
#endif

namespace kcsbridge
{

#ifdef __ZEPHYR__

namespace
{

/* Backing store for the D-Bus interface vtable data pointer; must outlive the
 * interface object, so it cannot live on the stack. */
ChannelData g_channelStorage = nullptr;

/* Context handed to the driver callback.  The callback runs in the eSPI ISR,
 * so it can only defer work; everything else happens on the io_context
 * thread. */
boost::asio::io_context* g_io = nullptr;
Channel g_channel = nullptr;
sdbusplus::bus_t* g_bus = nullptr;
sdbusplus::slot_t* g_slot = nullptr;

/* The KCS driver raises IPMI_RX_EVENT (from the eSPI ISR) as soon as a whole
 * request has been assembled -- this is the Zephyr equivalent of upstream's
 * "the /dev node became readable".
 *
 * asio must not be touched from an ISR (io_context::post() takes a mutex and a
 * mutex cannot be taken in ISR context on Zephyr), so the event goes through a
 * workqueue first; the handler then posts into the io_context, which puts
 * read() on the same thread that runs the sd-bus event loop: identical to the
 * upstream select()/EPOLLIN handler, and it keeps every sd-bus call on one
 * thread.
 *
 * The work queue is deliberately a *dedicated* one, at a lower urgency than
 * the bridge pthread (K_PRIO_PREEMPT(6) vs K_PRIO_PREEMPT(4), see
 * create_task_with_pthread's SCHED_OTHER priority mapping).  The system
 * workqueue runs at K_PRIO_COOP(-1), i.e. above every preemptible thread:
 * boost::asio::post() takes the io_context's internal mutex, so a COOP(-1)
 * worker preempting the io thread while it holds that mutex would block
 * forever with nobody left to release it -- a silent, total halt with no
 * output.  With the work queue below the io thread, the io thread can always
 * run and release the lock. */
K_THREAD_STACK_DEFINE(channelEventStack, 4096);
struct k_work_q channelEventQueue;

void channelEventWork(struct k_work*)
{
    boost::asio::post(*g_io, [] {
        try
        {
            kcsbridge::read(g_channel, *g_bus, *g_slot);
        }
        catch (const std::exception& e)
        {
            /* Upstream wraps this in stdplus::exception::ignore(); an
             * exception escaping io.run() would kill this pthread. */
            log_line(std::format("KCS event handling failed: {}", e.what()));
        }
    });
}
K_WORK_DEFINE(channelEvent, channelEventWork);

int channelEventCallback(const struct device*, void*, ipmi_event_type_t type)
{
    if (type == IPMI_RX_EVENT)
    {
        /* Drop the driver's write-phase dummy byte *here*, i.e. inside the
         * eSPI ISR that completed the host's last I/O write, before that
         * transaction is even acknowledged back to the host.  Doing it later
         * (in read(), on the io thread) leaves a window in which the host
         * polls OBF, sees the stale 0x00 and takes it for the netfn byte. */
        channel_clear_obf();
        k_work_submit_to_queue(&channelEventQueue, &channelEvent);
    }
    return 0;
}

/* Zephyr has no /dev/<channel>: the IPMI-over-KCS device *is* the channel.
 * It is reached through the ipmi_kcs node so that the raw KCS channel stays
 * a devicetree detail (see app/boards/alicmu_cmu610_cpu1.overlay). */
Channel channelOpen(const char* channel)
{
    ARG_UNUSED(channel);

    const struct device* dev = DEVICE_DT_GET(DT_NODELABEL(ipmi_kcs));
    if (!device_is_ready(dev))
    {
        printk_thread("KCS bridge: %s not ready\n", dev->name);
        return nullptr;
    }
    return dev;
}

} // namespace

#endif // __ZEPHYR__

int execute(const char* channel)
{
    std::string dbusChannel = channel;
    std::replace(dbusChannel.begin(), dbusChannel.end(), '-', '_');
    auto obj = "/xyz/openbmc_project/Ipmi/Channel/" + dbusChannel;
    auto srv = "xyz.openbmc_project.Ipmi.Channel." + dbusChannel;

#ifdef __ZEPHYR__

    printk_thread("KCS bridge: starting, channel=%s\n", channel);

    sd_bus* dbus = nullptr;
    int ret = connect_to_dbroker(&dbus);
    if (ret < 0)
    {
        printk_thread("KCS bridge: failed to connect to dbus-broker: %d\n", ret);
        return ret;
    }

    /* The asio connection owns the bus and, through its read watch, keeps
     * io.run() alive below. */
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io, dbus);

    sdbusplus::slot_t slot(nullptr);

    Channel kcs = channelOpen(channel);
    if (kcs == nullptr)
    {
        return -ENODEV;
    }

    /* Publish the context before arming the callback: an event may fire as
     * soon as ipmi_set_callback() returns. */
    g_channelStorage = kcs;
    g_io = &io;
    g_channel = kcs;
    g_bus = conn.get();
    g_slot = &slot;

    /* Below the io thread's K_PRIO_PREEMPT(4), see channelEventWork(). */
    k_work_queue_start(&channelEventQueue, channelEventStack,
                       K_THREAD_STACK_SIZEOF(channelEventStack),
                       K_PRIO_PREEMPT(6), NULL);
    k_thread_name_set(&channelEventQueue.thread, "kcsbridge_ev");

    ret = ipmi_set_callback(kcs, channelEventCallback, nullptr);
    if (ret < 0)
    {
        printk_thread("KCS bridge: ipmi_set_callback failed: %d\n", ret);
        return ret;
    }

    auto intf = createSMSHandler(*conn, obj.c_str(), &g_channelStorage);

    /* Readiness must not be gated on a broker round-trip.
     *
     * request_name() is a synchronous sd_bus_request_name() that blocks until
     * the broker answers, and kcs_bridge_ready_sem is what
     * start_all_auto_threads() waits on.  One slow broker reply would
     * otherwise stop the whole boot.  Signal readiness first, then acquire
     * the name -- nothing depends on the bridge. */
    printk_thread("KCS bridge ready\n");
    k_sem_give(&kcs_bridge_ready_sem);

    /* Bound the wait: the basu port defaults to a 25 s method-call timeout.
     * This deliberately stays on this thread rather than being posted to the
     * io_context: request_name() throws SdBusError on failure and an uncaught
     * exception out of a Zephyr pthread kills the whole process. */
    sd_bus_set_method_call_timeout(dbus, 5000000);
    try
    {
        conn->request_name(srv.c_str());
        printk_thread("KCS bridge: acquired %s\n", srv.c_str());
    }
    catch (const std::exception& e)
    {
        printk_thread("KCS bridge: request_name failed: %s\n", e.what());
    }
    sd_bus_set_method_call_timeout(dbus, 0);

    /* Runs until the sd-bus socket dies; the KCS side is driven by
     * channelEventCallback() -> channelEventWork() -> read(). */
    io.run();
    printk_thread("KCS bridge: event loop exited\n");
    return 0;

#else

    using sdeventplus::source::IO;
    using sdeventplus::source::Signal;
    using stdplus::fd::OpenAccess;
    using stdplus::fd::OpenFlag;
    using stdplus::fd::OpenFlags;

    // Set up our DBus and event loop
    auto event = sdeventplus::Event::get_default();
    auto bus = sdbusplus::bus::new_default();
    bus.attach_event(event.get(), SD_EVENT_PRIORITY_NORMAL);

    // Configure basic signal handling
    auto exit_handler = [&event](Signal&, const struct signalfd_siginfo*) {
        stdplus::print(stderr, "Interrupted, Exiting\n");
        event.exit(0);
    };
    stdplus::signal::block(SIGINT);
    Signal sig_int(event, SIGINT, exit_handler);
    stdplus::signal::block(SIGTERM);
    Signal sig_term(event, SIGTERM, exit_handler);

    // Open the KCS channel
    stdplus::ManagedFd kcs = stdplus::fd::open(
        std::format("/dev/{}", channel),
        OpenFlags(OpenAccess::ReadWrite).set(OpenFlag::NonBlock));
    sdbusplus::slot_t slot(nullptr);

    // Add a reader to the bus for handling inbound IPMI
    IO ioSource(
        event, kcs.get(), EPOLLIN | EPOLLET,
        stdplus::exception::ignore([&kcs, &bus, &slot](IO&, int, uint32_t) {
            kcsbridge::read(kcs, bus, slot);
        }));

    // Allow processes to affect the state machine
    auto intf = createSMSHandler(bus, obj.c_str(),
                                 reinterpret_cast<ChannelData*>(&kcs));
    bus.request_name(srv.c_str());

    sd_notify(0, "READY=1");
    return event.loop();

#endif
}

} // namespace kcsbridge

#ifdef __ZEPHYR__

/* Zephyr entry point.  openbmc_zephyr_app starts it as a pthread from
 * app/src/task_def.cpp::kcs_bridge_init(), see THREAD_DEFINE(kcs_bridge,...)
 * in app/src/main.cpp. */
extern "C" int kcsbridge_main(void)
{
    return kcsbridge::execute(CONFIG_OPENBMC_KCSBRIDGE_CHANNEL);
}

#else

int main(int argc, char* argv[])
{
    try
    {
        kcsbridge::Args args(argc, argv);
        return kcsbridge::execute(args.channel);
    }
    catch (const std::exception& e)
    {
        stdplus::print(stderr, "FAILED: {}\n", e.what());
        return 1;
    }
}

#endif
