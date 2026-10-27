#pragma once

/*
 * Thin channel abstraction so that cmd.cpp / server.cpp keep the upstream
 * phosphor-ipmi-kcs logic verbatim.
 *
 * Upstream talks to /dev/<channel>, a misc char device that exports the KCS
 * payload through read()/write() and the IPMI_BMC_IOCTL_* ioctls.
 *
 * The Zephyr CPU1 build has no such node: openbmc/zephyr-osal's kcsfs
 * (/dev/kcs/<label>) is only compiled for SOC_LSQSH_CPU2.  Instead the
 * Zephyr side drives the IPMI-over-KCS driver directly
 * (zephyr/drivers/ipmi/ipmi_kcs_ls.c):
 *
 *   ipmi_read()/ipmi_write()      <-> read()/write() on the char device
 *   kcs_update_status()           <-> IPMI_BMC_IOCTL_(CLEAR_)SET_SMS_ATN
 *   ipmi_force_abort()            <-> IPMI_BMC_IOCTL_FORCE_ABORT
 *   IPMI_RX_EVENT callback        <-> select()/EPOLLIN readiness
 *
 * Only this header knows about the difference; the bridge logic does not.
 */

#include <cstdint>
#include <span>
#include <string>

#ifdef __ZEPHYR__

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/ipmi.h>
#include <zephyr/drivers/kcs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <printk_thread.h>

#else

#include <stdplus/fd/ops.hpp>
#include <stdplus/print.hpp>

#include "ipmi_bmc.h"

#endif

namespace kcsbridge
{

/* KCS interface status register: SMS attention bit (IPMI 2.0 table 9-1) */
#define KCS_STATUS_SMS_ATN BIT(2)

/* Log one already formatted line.  Keeps the call sites free of the
 * printf/fmt difference between printk() and stdplus::print(). */
inline void log_line(const std::string& line)
{
#ifdef __ZEPHYR__
    printk_thread("%s\n", line.c_str());
#else
    stdplus::print(stderr, "{}\n", line);
#endif
}

#ifdef __ZEPHYR__

/* The driver device is the channel. */
using Channel = const struct device*;

/* Type stored behind the D-Bus interface vtable data pointer.  On Linux this
 * is the fd handle itself (see below). */
using ChannelData = const struct device*;

/* The stock KCS driver serialises the shared host_kcs_env through
 * kcs_env_lock() -- a hand-rolled atomic busy-wait that does NOT mask
 * interrupts.  The host-side LPC I/O handlers take that same lock from
 * interrupt context on this very core, so whenever the host touches a KCS
 * port while this thread holds the lock, the ISR spins forever waiting for a
 * thread that can never be scheduled again and the whole SoC freezes.
 * Observed symptom: ipmid logged the completion code and the reply reached
 * the bridge, then the console went silent forever -- the stall was inside
 * channel_clear_obf(), i.e. inside that busy-wait, and serial plus heartbeat
 * died with it.
 *
 * The real fix belongs in linkedsemi's
 * soc/linkedsemi/common/espi_lpc/espi_lpc_common.c (use k_spin_lock()), but
 * that is a shared library used by other projects, so it is not ours to
 * change.  Until it is fixed upstream, mask interrupts across every driver
 * call issued from thread context here -- that is precisely the window in
 * which the lock is held, and the critical sections are a few register
 * accesses (lpc_send_edge_irq() even takes its own nested k_spin_lock), so
 * this costs microseconds.  Calls made from the KCS ISR itself are already
 * uninterruptible and need no wrapper. */
inline std::span<uint8_t> channel_read(Channel kcs, std::span<uint8_t> buf)
{
    unsigned int key = arch_irq_lock();
    int ret = ipmi_read(kcs, buf.data(), static_cast<uint32_t>(buf.size()));
    arch_irq_unlock(key);
    if (ret <= 0)
    {
        return {};
    }
    return buf.first(static_cast<size_t>(ret));
}

inline void channel_write(Channel kcs, std::span<const uint8_t> buf)
{
    /* Note: ipmi_kcs_ls_write() reports the requested size even when
     * kcs_write_data() refused the first byte because the host had not read
     * the previous OBF yet (host abort / refused response).  The host then
     * sees a desynced response and recovers with a KCS abort (0x60).  Worth
     * knowing when debugging "first byte wrong" symptoms. */
    unsigned int key = arch_irq_lock();
    int ret = ipmi_write(kcs, const_cast<uint8_t*>(buf.data()),
                         static_cast<uint32_t>(buf.size()));
    arch_irq_unlock(key);
    if (ret < 0)
    {
        log_line(std::string("KCS write failed: ") + std::to_string(ret));
    }
}

/* Raw KCS register file of this channel (the ls-kcs device behind the
 * ipmi_kcs device).  Needed because ipmi_kcs_ls_update_status() in this tree
 * calls kcs_update_status() with the *ipmi* device instead of the kcs device,
 * so it lands on the wrong driver api slot (force_abort) and aborts an
 * in-flight transfer.  The kcs api itself is correct, so go straight to it.
 * (Driver left untouched on purpose.) */
#define KCSBG_IPMI_NODE DT_NODELABEL(ipmi_kcs)
#define KCSBG_KCS_NODE DT_PHANDLE(KCSBG_IPMI_NODE, kcs_chan)

inline const struct device* channel_regs()
{
    return DEVICE_DT_GET(KCSBG_KCS_NODE);
}

inline void channel_set_attention(Channel kcs, bool on)
{
    ARG_UNUSED(kcs);
    unsigned int key = arch_irq_lock();
    kcs_update_status(channel_regs(), KCS_STATUS_SMS_ATN,
                      on ? KCS_STATUS_SMS_ATN : 0);
    arch_irq_unlock(key);
}

inline void channel_force_abort(Channel kcs)
{
    unsigned int key = arch_irq_lock();
    ipmi_force_abort(kcs);
    arch_irq_unlock(key);
}

/* Drop a stale output-buffer-full flag.
 *
 * ipmi_kcs_ls.c acknowledges every *command* write by putting a dummy 0x00 in
 * the data register and setting OBF, and it never clears it.  kcs_ls_write_data()
 * refuses to write while OBF is set (-EIO) and ipmi_kcs_ls_write() ignores
 * that error, so without this the first byte of every response (the netfn
 * byte) would be silently dropped and the host would read the leftover dummy
 * instead -- the response shifts by one and the host never parses it.
 *
 * Safe to call whenever we are about to produce the first response byte: at
 * that point the host has just finished writing and is waiting for OBF. */
inline void channel_clear_obf()
{
    /* Also called from the KCS event ISR, where this is a no-op wrapper:
     * arch_irq_lock()/unlock() nest correctly. */
    unsigned int key = arch_irq_lock();
    kcs_update_status(channel_regs(), KCS_OBF, 0);
    arch_irq_unlock(key);
}


#else

using Channel = stdplus::Fd&;
using ChannelData = stdplus::Fd;

inline std::span<uint8_t> channel_read(Channel kcs, std::span<uint8_t> buf)
{
    return stdplus::fd::read(kcs, buf);
}

inline void channel_write(Channel kcs, std::span<const uint8_t> buf)
{
    stdplus::fd::writeExact(kcs, buf);
}

inline void channel_set_attention(Channel kcs, bool on)
{
    stdplus::fd::ioctl(kcs, on ? IPMI_BMC_IOCTL_SET_SMS_ATN
                               : IPMI_BMC_IOCTL_CLEAR_SMS_ATN,
                       nullptr);
}

inline void channel_force_abort(Channel kcs)
{
    stdplus::fd::ioctl(kcs, IPMI_BMC_IOCTL_FORCE_ABORT, nullptr);
}

/* The Linux char device models a real KCS BMC and does not leave a stale OBF
 * behind, so there is nothing to clear (kept so cmd.cpp stays branch-free). */
inline void channel_clear_obf()
{
}

#endif

} // namespace kcsbridge
