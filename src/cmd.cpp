#include "cmd.hpp"

#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/slot.hpp>

#include <array>
#include <cstring>
#include <format>
#include <iterator>
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

/* Send the ipmid reply back into the KCS output buffer.  Body is upstream
 * (based on IPMI KCS spec figure 9-2), only the channel access at the end
 * goes through kcs_channel.hpp. */
void write(Channel kcs, message_t&& m)
{
    std::array<uint8_t, 1024> buffer;
    std::span<uint8_t> out(buffer.begin(), 3);
    try
    {
        if (m.is_method_error())
        {
            // Extra copy to workaround lack of `const sd_bus_error` constructor
            auto error = *m.get_error();
            throw sdbusplus::exception::SdBusError(&error, "ipmid response");
        }
        std::tuple<uint8_t, uint8_t, uint8_t, uint8_t, std::vector<uint8_t>>
            ret;
        m.read(ret);
        const auto& [netfn, lun, cmd, cc, data] = ret;
        // Based on the IPMI KCS spec Figure 9-2
        // netfn needs to be changed to odd in KCS responses
        if (data.size() + 3 > buffer.size())
        {
            throw std::runtime_error(std::format(
                "too large {} > {}", data.size() + 3, buffer.size()));
        }
        buffer[0] = (netfn | 1) << 2;
        buffer[0] |= lun;
        buffer[1] = cmd;
        buffer[2] = cc;
        memcpy(&buffer[3], data.data(), data.size());
        out = std::span<uint8_t>(buffer.begin(), data.size() + 3);
    }
    catch (const std::exception& e)
    {
        log_line(std::format("IPMI response failure: {}", e.what()));
        buffer[0] |= 1 << 2;
        buffer[2] = 0xff;
    }

    /* The driver leaves the write-phase dummy byte in the data register with
     * OBF set, and refuses to overwrite it, so clear the flag or the first
     * byte of this response is dropped. See kcs_channel.hpp. */
    channel_clear_obf();
    channel_write(kcs, out);

    /* Bring-up aid: the whole KCS payload as the host will read it.  Cheap
     * enough at IPMI rates and invaluable when nothing else answers. */
#ifdef __ZEPHYR__
    std::string hex;
    for (size_t i = 0; i < out.size(); i++)
    {
        std::format_to(std::back_inserter(hex), "{:02x} ",
                       static_cast<unsigned>(out[i]));
    }
    log_line(std::format("KCS response ({} bytes): {}", out.size(), hex));
#endif
}

/* Turn one complete KCS request into an xyz.openbmc_project.Ipmi.Host.execute
 * call.  Upstream body, the "readiness" source is the channel itself. */
void read(Channel kcs, bus_t& bus, slot_t& outstanding)
{
    std::array<uint8_t, 1024> buffer;
    auto in = channel_read(kcs, buffer);
    if (in.empty())
    {
        /* We were woken up (a KCS request completed) but the driver had no
         * message for us: either the phase already moved on, or we raced with
         * a host abort.  Worth one line during bring-up. */
        log_line("KCS: woken but no complete message (ipmi_read returned "
                 "nothing)");
        return;
    }
    if (outstanding)
    {
        log_line("Canceling outstanding request");
        outstanding = slot_t(nullptr);
    }
    if (in.size() < 2)
    {
        log_line("Read too small, ignoring");
        return;
    }
    // Based on the IPMI KCS spec Figure 9-1
    uint8_t netfn = in[0] >> 2, lun = in[0] & 3, cmd = in[1];

    /* The host has just finished writing, so the only thing OBF can hold is the
     * driver's write-phase dummy byte.  Drop it now, before the D-Bus round
     * trip: the earlier it is gone, the smaller the window in which the host
     * can mistake it for the first response byte. */
    channel_clear_obf();

    log_line(std::format("KCS req: netfn={} lun={} cmd=0x{:02x} len={}",
                         static_cast<unsigned>(netfn),
                         static_cast<unsigned>(lun),
                         static_cast<unsigned>(cmd), in.size() - 2));

    auto m = bus.new_method_call("xyz.openbmc_project.Ipmi.Host",
                                 "/xyz/openbmc_project/Ipmi",
                                 "xyz.openbmc_project.Ipmi.Server", "execute");
    std::map<std::string, std::variant<int>> options;
    m.append(netfn, lun, cmd, in.subspan(2), options);
    outstanding = m.call_async([&outstanding, kcs](message_t&& m) {
        outstanding = slot_t(nullptr);
        write(kcs, std::move(m));
    });
}

} // namespace kcsbridge
