#include "server.hpp"
#include "ipmi_bmc.h"

#include <sdbusplus/exception.hpp>
#include <sdbusplus/server/interface.hpp>
#include <sdbusplus/vtable.hpp>
#include <zephyr/drivers/ipmi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <cstdio>
#include <stdexcept>

#define KCS_STATUS_SMS_ATN BIT(2)

namespace kcsbridge
{

void setAttention(sdbusplus::message_t& m, const struct device *kcs_dev)
{
    ipmi_update_status(kcs_dev, KCS_STATUS_SMS_ATN, KCS_STATUS_SMS_ATN);
    m.new_method_return().method_return();
}

void clearAttention(sdbusplus::message_t& m, const struct device *kcs_dev)
{
    ipmi_update_status(kcs_dev, KCS_STATUS_SMS_ATN, 0);
    m.new_method_return().method_return();
}

void forceAbort(sdbusplus::message_t& m, const struct device *kcs_dev)
{
    ipmi_force_abort(kcs_dev);
    m.new_method_return().method_return();
}

template <auto func, typename Data>
int methodRsp(sd_bus_message* mptr, void* dataptr, sd_bus_error* error) noexcept
{
    sdbusplus::message_t m(mptr);
    try
    {
        func(m, *reinterpret_cast<Data*>(dataptr));
    }
    catch (const std::exception& e)
    {
        printk("Method response failed: %s\n", e.what());
        sd_bus_error_set(error,
                         "xyz.openbmc_project.Common.Error.InternalFailure",
                         "The operation failed internally.");
    }
    return 1;
}

template <typename Data>
constexpr sdbusplus::vtable::vtable_t dbusMethods[] = {
    sdbusplus::vtable::start(),
    sdbusplus::vtable::method("setAttention", "", "",
                              methodRsp<setAttention, Data>),
    sdbusplus::vtable::method("clearAttention", "", "",
                              methodRsp<clearAttention, Data>),
    sdbusplus::vtable::method("forceAbort", "", "",
                              methodRsp<forceAbort, Data>),
    sdbusplus::vtable::end(),
};

/* kcs_dev_storage must outlive the returned interface object.
 * Caller ensures this by keeping the interface alive in execute(). */
static const struct device *kcs_dev_storage = nullptr;

sdbusplus::server::interface::interface createSMSHandler(
    sdbusplus::bus_t& bus, const char* obj, const struct device *kcs_dev)
{
    kcs_dev_storage = kcs_dev;
    return sdbusplus::server::interface::interface(
        bus, obj, "xyz.openbmc_project.Ipmi.Channel.SMS",
        dbusMethods<const struct device *>,
        const_cast<const struct device **>(&kcs_dev_storage));
}

} // namespace kcsbridge
