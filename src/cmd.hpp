#pragma once

#include "kcs_channel.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/slot.hpp>

namespace kcsbridge
{

/* Same two entry points as upstream phosphor-ipmi-kcs, only the channel type
 * differs (fd on Linux, driver device on Zephyr).  See kcs_channel.hpp. */
void write(Channel kcs, sdbusplus::message_t&& m);
void read(Channel kcs, sdbusplus::bus_t& bus, sdbusplus::slot_t& outstanding);

} // namespace kcsbridge
