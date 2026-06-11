#include "cmd.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/slot.hpp>
#include <zephyr/drivers/ipmi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <array>
#include <cstdio>
#include <format>
#include <map>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace kcsbridge
{

using sdbusplus::bus_t;
using sdbusplus::message_t;
using sdbusplus::slot_t;

void kcs_write(const struct device *kcs_dev, message_t&& m)
{
    uint8_t buffer[1024];
    size_t out_size = 3;
    try
    {
        if (m.is_method_error())
        {
            auto error = *m.get_error();
            throw sdbusplus::exception::SdBusError(&error, "ipmid response");
        }
        std::tuple<uint8_t, uint8_t, uint8_t, uint8_t, std::vector<uint8_t>>
            ret;
        m.read(ret);
        const auto& [netfn, lun, cmd, cc, data] = ret;
        if (data.size() + 3 > sizeof(buffer))
        {
            throw std::runtime_error(std::format(
                "too large {} > {}", data.size() + 3, sizeof(buffer)));
        }
        buffer[0] = (netfn | 1) << 2;
        buffer[0] |= lun;
        buffer[1] = cmd;
        buffer[2] = cc;
        memcpy(&buffer[3], data.data(), data.size());
        out_size = data.size() + 3;
    }
    catch (const std::exception& e)
    {
        printk("IPMI response failure: %s\n", e.what());
        buffer[0] |= 1 << 2;
        buffer[2] = 0xff;
    }
    int ret = ipmi_write(kcs_dev, buffer, out_size);
    if (ret < 0) {
        printk("ipmi_write failed ret:%d\n", ret);
    } else {
        printk("kcs_write: response len=%zu data=", out_size);
        for (size_t i = 0; i < out_size; i++)
            printk("%02x ", buffer[i]);
        printk("\n");
    }
}

void kcs_read(const struct device *kcs_dev, bus_t& bus, slot_t& outstanding)
{
    uint8_t buffer[1024];
    int ret = ipmi_read(kcs_dev, buffer, sizeof(buffer));
    if (ret <= 0) {
        printk("kcs_read: ipmi_read failed ret=%d\n", ret);
        return;
    }
    size_t in_len = ret;

    if (outstanding)
    {
        printk("Canceling outstanding request\n");
        outstanding = slot_t(nullptr);
    }
    auto m = bus.new_method_call("xyz.openbmc_project.Ipmi.Host",
                                 "/xyz/openbmc_project/Ipmi",
                                 "xyz.openbmc_project.Ipmi.Server", "execute");
    std::map<std::string, std::variant<int>> options;
    uint8_t netfn = buffer[0] >> 2, lun = buffer[0] & 3, cmd = buffer[1];
    m.append(netfn, lun, cmd,
             std::span<uint8_t>(buffer + 2, in_len - 2), options);
    outstanding = m.call_async(
        [&outstanding, kcs_dev](message_t&& m) {
            outstanding = slot_t(nullptr);
            kcs_write(kcs_dev, std::move(m));
        });
}

} // namespace kcsbridge
