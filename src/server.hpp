#pragma once
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/interface.hpp>
#include <zephyr/device.h>

namespace kcsbridge
{

sdbusplus::server::interface::interface createSMSHandler(
    sdbusplus::bus_t& bus, const char* obj, const struct device *kcs_dev);

}
