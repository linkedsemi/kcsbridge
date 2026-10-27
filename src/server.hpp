#pragma once

#include "kcs_channel.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/interface.hpp>

namespace kcsbridge
{

/* Register xyz.openbmc_project.Ipmi.Channel.SMS on `obj`.
 *
 * `data` must stay alive for as long as the returned interface object: it is
 * the vtable data pointer handed back to the method callbacks (same contract
 * as upstream, where it points at the channel fd). */
sdbusplus::server::interface::interface createSMSHandler(sdbusplus::bus_t& bus,
                                                        const char* obj,
                                                        ChannelData* data);

} // namespace kcsbridge
