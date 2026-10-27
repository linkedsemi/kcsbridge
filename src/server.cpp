#include "server.hpp"

#include <sdbusplus/exception.hpp>
#include <sdbusplus/server/interface.hpp>
#include <sdbusplus/vtable.hpp>

#include <stdexcept>

namespace kcsbridge
{

/* The three methods below exist so other daemons can poke the KCS status
 * bits; upstream issues an ioctl on the channel fd, the channel abstraction
 * maps that onto the driver (see kcs_channel.hpp). */
void setAttention(sdbusplus::message_t& m, Channel kcs)
{
    channel_set_attention(kcs, true);
    m.new_method_return().method_return();
}

void clearAttention(sdbusplus::message_t& m, Channel kcs)
{
    channel_set_attention(kcs, false);
    m.new_method_return().method_return();
}

void forceAbort(sdbusplus::message_t& m, Channel kcs)
{
    channel_force_abort(kcs);
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
        log_line(std::string("Method response failed: ") + e.what());
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

sdbusplus::server::interface::interface createSMSHandler(sdbusplus::bus_t& bus,
                                                        const char* obj,
                                                        ChannelData* data)
{
    return sdbusplus::server::interface::interface(
        bus, obj, "xyz.openbmc_project.Ipmi.Channel.SMS",
        dbusMethods<ChannelData>, data);
}

} // namespace kcsbridge
