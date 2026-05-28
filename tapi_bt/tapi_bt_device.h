/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief The devices around the agent
 *
 * @defgroup tapi_bt_device Devices (tapi_bt_device)
 * @ingroup tapi_bt
 * @{
 *
 * Two different questions, kept apart because the answers mean
 * different things:
 *
 * - **what the system knows** - tapi_bt_known(): the devices it has
 *   paired with or seen before. Reading it changes nothing and takes
 *   no time.
 * - **what is in range** - tapi_bt_scan(): an inquiry, which takes
 *   seconds, puts the radio on the air, and finds things that have
 *   never been paired.
 *
 * A device that is paired but switched off appears in the first and
 * not the second. A stranger's headset appears in the second and not
 * the first. Asking one and reporting the other is the mistake this
 * separation exists to prevent.
 *
 * @code
 * te_vec devices;
 *
 * CHECK_RC(tapi_bt_scan(factory, TAPI_BT_AUTO, 10, 30000, &devices));
 * TE_VEC_FOREACH(&devices, tapi_bt_device, dev)
 *     RING("%s %s %d dBm", dev->address, dev->name, dev->rssi);
 * tapi_bt_devices_free(&devices);
 * @endcode
 */

#ifndef __TSF_TAPI_BT_DEVICE_H__
#define __TSF_TAPI_BT_DEVICE_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "te_vector.h"
#include "tapi_job.h"

#include "tapi_bt.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The value of #tapi_bt_device::rssi when nobody measured one. */
#define TAPI_BT_RSSI_UNKNOWN 127

/** One device. */
typedef struct tapi_bt_device {
    /** Its address, normalised to upper case with colons. */
    char address[TAPI_BT_ADDR_LEN];
    /** Its name, or @c NULL when it did not give one. */
    char *name;
    /** Signal strength in dBm, or @ref TAPI_BT_RSSI_UNKNOWN. */
    int rssi;
    /** @c true when the system has a bond with it. */
    bool paired;
    /** @c true when it is connected now. */
    bool connected;
    /** @c true when the system has been told to trust it. */
    bool trusted;
    /** Class of device, or @c 0 when unknown. */
    unsigned int class_of_device;
    /** Free text for the log: the services, the type, the transport. */
    char *detail;
} tapi_bt_device;

/**
 * The devices the system already knows.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  backend      Backend, or @ref TAPI_BT_AUTO.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] devices      Vector of #tapi_bt_device; release with
 *                          tapi_bt_devices_free().
 *
 * @return Status code.
 * @retval TE_EOPNOTSUPP    The backend keeps no such list; a Linux
 *                          without @c bluetoothd does not.
 */
extern te_errno tapi_bt_known(tapi_job_factory_t *factory,
                              tapi_bt_backend backend, int timeout_ms,
                              te_vec *devices);

/**
 * Look for devices that are in range.
 *
 * This puts the radio on the air for @p seconds. It finds only what is
 * willing to be found: a device that is not discoverable is not a
 * device that is absent.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  backend      Backend, or @ref TAPI_BT_AUTO.
 * @param[in]  seconds      How long to look.
 * @param[in]  timeout_ms   Timeout for the job, ms. Give it more than
 *                          @p seconds.
 * @param[out] devices      Vector of #tapi_bt_device.
 *
 * @return Status code.
 */
extern te_errno tapi_bt_scan(tapi_job_factory_t *factory,
                             tapi_bt_backend backend, unsigned int seconds,
                             int timeout_ms, te_vec *devices);

/**
 * Find one device in a vector by address.
 *
 * @param devices       Vector of #tapi_bt_device.
 * @param address       Address in any of the usual forms.
 *
 * @return The device, or @c NULL. It belongs to @p devices.
 */
extern const tapi_bt_device *tapi_bt_device_find(const te_vec *devices,
                                                 const char *address);

/**
 * Pair with a device.
 *
 * @note Pairing changes the agent, and nothing here undoes it. A test
 *       that pairs is responsible for tapi_bt_remove() in a cleanup
 *       section that runs even when it failed.
 *
 * @param factory       Job factory.
 * @param backend       Backend, or @ref TAPI_BT_AUTO.
 * @param address       Address of the device.
 * @param timeout_ms    Timeout, ms. Pairing waits for the other end.
 *
 * @return Status code.
 * @retval TE_EOPNOTSUPP    The backend cannot pair unattended.
 */
extern te_errno tapi_bt_pair(tapi_job_factory_t *factory,
                             tapi_bt_backend backend, const char *address,
                             int timeout_ms);

/**
 * Remove a pairing.
 *
 * @param factory       Job factory.
 * @param backend       Backend, or @ref TAPI_BT_AUTO.
 * @param address       Address of the device.
 * @param timeout_ms    Timeout, ms.
 *
 * @return Status code.
 */
extern te_errno tapi_bt_remove(tapi_job_factory_t *factory,
                               tapi_bt_backend backend, const char *address,
                               int timeout_ms);

/**
 * Connect to a paired device, or disconnect from it.
 *
 * @param factory       Job factory.
 * @param backend       Backend, or @ref TAPI_BT_AUTO.
 * @param address       Address of the device.
 * @param connect       @c true to connect, @c false to disconnect.
 * @param timeout_ms    Timeout, ms.
 *
 * @return Status code.
 */
extern te_errno tapi_bt_connect(tapi_job_factory_t *factory,
                                tapi_bt_backend backend,
                                const char *address, bool connect,
                                int timeout_ms);

/**
 * Write a device list into the log.
 *
 * @param what          A word for the log, e.g. @c "in range".
 * @param devices       Vector of #tapi_bt_device.
 */
extern void tapi_bt_devices_log(const char *what, const te_vec *devices);

/**
 * Release a device list.
 *
 * @param devices       Vector of #tapi_bt_device.
 */
extern void tapi_bt_devices_free(te_vec *devices);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_BT_DEVICE_H__ */

/**@} <!-- END tapi_bt_device --> */
