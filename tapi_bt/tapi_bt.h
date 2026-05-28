/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Bluetooth from a test
 *
 * @defgroup tapi_bt Bluetooth (tapi_bt)
 * @{
 *
 * The adapter of a Test Agent, the devices around it, and what the
 * pairing between them is worth - on Linux, macOS and Windows.
 *
 * @section tapi_bt_backends Four backends, and no pretence
 *
 * There is no portable way to drive Bluetooth. Every system exposes a
 * different thing, and the differences are not spellings of one idea:
 *
 * - **@ref TAPI_BT_BLUEZ** - Linux with @c bluetoothd running.
 *   @c bluetoothctl over D-Bus for devices and pairing, @c btmgmt for
 *   the adapter. The complete one.
 * - **@ref TAPI_BT_HCI** - Linux without the daemon: @c hciconfig,
 *   @c hcitool, @c l2ping, @c sdptool talking to the kernel over an
 *   HCI socket. No pairing, no GATT - those live in the daemon - but
 *   it works on an embedded image that has no D-Bus.
 * - **@ref TAPI_BT_MACOS** - @c blueutil where it is installed, and
 *   @c system_profiler, which is always there, for everything that
 *   can be read rather than changed.
 * - **@ref TAPI_BT_WINDOWS** - PowerShell: the PnP device tree for
 *   what is there, and the WinRT radio API for power. Windows does not
 *   give a program the HCI link at all, so the HCI half of this
 *   library is not available there; what it has instead is an ETW
 *   trace, which is a different thing and is treated as one.
 *
 * So the common core is only what all four genuinely do, and the rest
 * is behind tapi_bt_supports(). A test asks, and skips or takes
 * another route; it is never told that something worked where the
 * system cannot do it.
 *
 * | | BlueZ | HCI | macOS | Windows |
 * |---|---|---|---|---|
 * | adapter address, name, power | yes | yes | yes | yes |
 * | power on and off | yes | yes | yes | yes |
 * | discoverable | yes | yes | yes | no |
 * | scan for devices | yes | yes | yes | yes |
 * | known and connected devices | yes | no | yes | yes |
 * | pair, remove | yes | no | yes | no |
 * | connect, disconnect | yes | no | yes | no |
 * | RSSI | yes | yes | yes | no |
 * | HCI trace | yes | yes | no | no (ETW) |
 * | l2ping, SDP browse | yes | yes | no | no |
 *
 * @section tapi_bt_detect What the backend is
 *
 * @ref TAPI_BT_AUTO asks the agent. It is worth knowing how, because
 * the obvious way does not work:
 *
 * - **@c hcitool @c dev exits 0 with no adapter at all.** It prints
 *   @c "Devices:" and an empty list, so its exit status says nothing
 *   and the list has to be read.
 * - **@c bluetoothctl aborts when @c bluetoothd is not running** - not
 *   an error message and a status, a @c SIGABRT out of libdbus, which
 *   arrives as exit code 134 or as a signal. So it is never the thing
 *   asked first.
 *
 * Both measured on BlueZ 5.72. Detection therefore goes by
 * @c btmgmt and @c hciconfig, which fail honestly, and only reaches
 * for @c bluetoothctl once the daemon is known to answer.
 *
 * @code
 * tapi_bt_adapter adapter;
 *
 * if (!tapi_bt_available(factory, TAPI_BT_AUTO, 10000))
 *     TEST_SKIP("There is no Bluetooth adapter on the agent");
 *
 * CHECK_RC(tapi_bt_adapter_get(factory, TAPI_BT_AUTO, 10000, &adapter));
 * RING("%s is %s", adapter.address, adapter.powered ? "on" : "off");
 * tapi_bt_adapter_free(&adapter);
 * @endcode
 */

#ifndef __TSF_TAPI_BT_H__
#define __TSF_TAPI_BT_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "tapi_job.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default timeout for one Bluetooth command, ms. */
#define TAPI_BT_TIMEOUT_MS 30000

/** Length of an address in the form @c "00:11:22:33:44:55", with NUL. */
#define TAPI_BT_ADDR_LEN 18

/** How the agent's Bluetooth is reached. */
typedef enum tapi_bt_backend {
    /** Ask the agent which of the others it has. */
    TAPI_BT_AUTO = 0,
    /** Linux, @c bluetoothd over D-Bus. */
    TAPI_BT_BLUEZ,
    /** Linux, the kernel's HCI socket with no daemon. */
    TAPI_BT_HCI,
    /** macOS: @c blueutil and @c system_profiler. */
    TAPI_BT_MACOS,
    /** Windows: PowerShell over the PnP tree and the radio API. */
    TAPI_BT_WINDOWS,
} tapi_bt_backend;

/** Read the adapter's address, name and power state. */
#define TAPI_BT_FEAT_ADAPTER      (1u << 0)
/** Turn the adapter on and off. */
#define TAPI_BT_FEAT_POWER        (1u << 1)
/** Make the adapter discoverable, or stop it being so. */
#define TAPI_BT_FEAT_DISCOVERABLE (1u << 2)
/** Look for devices that are advertising. */
#define TAPI_BT_FEAT_SCAN         (1u << 3)
/** List the devices the system already knows. */
#define TAPI_BT_FEAT_KNOWN        (1u << 4)
/** Pair with a device and remove a pairing. */
#define TAPI_BT_FEAT_PAIR         (1u << 5)
/** Connect to a paired device and disconnect. */
#define TAPI_BT_FEAT_CONNECT      (1u << 6)
/** Report a signal strength. */
#define TAPI_BT_FEAT_RSSI         (1u << 7)
/** Capture the HCI link. */
#define TAPI_BT_FEAT_HCI_TRACE    (1u << 8)
/** @c l2ping and @c sdptool. */
#define TAPI_BT_FEAT_L2_SDP       (1u << 9)

/** What the adapter is. */
typedef struct tapi_bt_adapter {
    /** Its address, or an empty string when it would not say. */
    char address[TAPI_BT_ADDR_LEN];
    /** Its name, or @c NULL. */
    char *name;
    /** The backend that answered. */
    tapi_bt_backend backend;
    /** @c true when the radio is on. */
    bool powered;
    /** @c true when it answers an inquiry from strangers. */
    bool discoverable;
    /** @c true when it accepts a pairing attempt from strangers. */
    bool pairable;
    /** What the system calls it: @c "hci0", the chipset, the PnP id. */
    char *identifier;
    /** Free text the backend gave about it; for the log, not a verdict. */
    char *detail;
} tapi_bt_adapter;

/**
 * Is there a Bluetooth adapter the agent can use?
 *
 * Reads what the tools print rather than trusting their exit status,
 * because on Linux neither is reliable on its own - see
 * @ref tapi_bt_detect.
 *
 * @param factory       Job factory.
 * @param backend       Backend to check, or @ref TAPI_BT_AUTO.
 * @param timeout_ms    Timeout, ms.
 *
 * @return @c true when an adapter answered.
 */
extern bool tapi_bt_available(tapi_job_factory_t *factory,
                              tapi_bt_backend backend, int timeout_ms);

/**
 * Work out which backend the agent has.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] backend      The backend found.
 *
 * @return Status code.
 * @retval TE_ENOENT        Nothing on the agent answers.
 */
extern te_errno tapi_bt_detect(tapi_job_factory_t *factory, int timeout_ms,
                               tapi_bt_backend *backend);

/**
 * What a backend can do.
 *
 * @param backend       Backend.
 *
 * @return A mask of @c TAPI_BT_FEAT_*.
 */
extern unsigned int tapi_bt_features(tapi_bt_backend backend);

/**
 * Can this backend do this?
 *
 * @param backend       Backend.
 * @param feature       One or more @c TAPI_BT_FEAT_*.
 *
 * @return @c true when all of @p feature are supported.
 */
extern bool tapi_bt_supports(tapi_bt_backend backend, unsigned int feature);

/**
 * Read the adapter.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  backend      Backend, or @ref TAPI_BT_AUTO.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] adapter      What it said; release with
 *                          tapi_bt_adapter_free().
 *
 * @return Status code.
 * @retval TE_ENOENT        There is no adapter.
 */
extern te_errno tapi_bt_adapter_get(tapi_job_factory_t *factory,
                                    tapi_bt_backend backend, int timeout_ms,
                                    tapi_bt_adapter *adapter);

/**
 * Turn the adapter on or off.
 *
 * @note This changes the agent. Through Configurator it would roll
 *       back by itself; here it does not, so a test that turns a radio
 *       off is responsible for turning it back on, in a cleanup
 *       section that runs even when the test failed.
 *
 * @param factory       Job factory.
 * @param backend       Backend, or @ref TAPI_BT_AUTO.
 * @param on            What to set it to.
 * @param timeout_ms    Timeout, ms.
 *
 * @return Status code.
 * @retval TE_EOPNOTSUPP    The backend cannot.
 */
extern te_errno tapi_bt_power_set(tapi_job_factory_t *factory,
                                  tapi_bt_backend backend, bool on,
                                  int timeout_ms);

/**
 * Make the adapter discoverable, or stop it.
 *
 * @param factory       Job factory.
 * @param backend       Backend, or @ref TAPI_BT_AUTO.
 * @param on            What to set it to.
 * @param timeout_ms    Timeout, ms.
 *
 * @return Status code.
 * @retval TE_EOPNOTSUPP    The backend cannot; Windows cannot.
 */
extern te_errno tapi_bt_discoverable_set(tapi_job_factory_t *factory,
                                         tapi_bt_backend backend, bool on,
                                         int timeout_ms);

/**
 * Write an adapter into the log.
 *
 * @param adapter       Adapter.
 */
extern void tapi_bt_adapter_log(const tapi_bt_adapter *adapter);

/**
 * Release an adapter.
 *
 * @param adapter       Adapter.
 */
extern void tapi_bt_adapter_free(tapi_bt_adapter *adapter);

/**
 * Put an address into one form.
 *
 * Every system writes an address differently: BlueZ uses
 * @c "20:15:82:EF:D2:5D", @c blueutil uses @c "20-15-82-ef-d2-5d", and
 * Windows hides one inside a PnP identifier. Comparing them as they
 * come is a bug waiting for the first test that pairs on one system
 * and checks on another, so everything here is normalised to upper
 * case with colons on the way in.
 *
 * @param[in]  address  An address in any of those forms.
 * @param[out] dest     Buffer of at least @ref TAPI_BT_ADDR_LEN.
 *
 * @return @c true when @p address held six hex octets.
 */
extern bool tapi_bt_addr_normalize(const char *address, char *dest);

/**
 * Spell out a backend.
 *
 * @param backend       Backend.
 *
 * @return A static string, never @c NULL.
 */
extern const char *tapi_bt_backend2str(tapi_bt_backend backend);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_BT_H__ */

/**@} <!-- END tapi_bt --> */
