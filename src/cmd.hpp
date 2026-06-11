#pragma once
#include <sdbusplus/bus.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/slot.hpp>
#include <zephyr/device.h>
#include <zephyr/drivers/ipmi.h>

namespace kcsbridge
{

void kcs_write(const struct device *kcs_dev, sdbusplus::message_t&& m);
void kcs_read(const struct device *kcs_dev, sdbusplus::bus_t& bus,
              sdbusplus::slot_t& outstanding);

} // namespace kcsbridge
