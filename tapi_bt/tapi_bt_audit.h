/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief What the Bluetooth of an agent is worth
 *
 * @defgroup tapi_bt_audit Bluetooth security posture
 * @ingroup tapi_bt
 * @{
 *
 * Bluetooth is a radio that answers strangers, and most of what goes
 * wrong with it is a setting nobody looked at. An adapter that stays
 * discoverable, a pairing that anyone in range can start, a link
 * encrypted with a key that is published in the specification.
 *
 * This reads the settings the system reports and turns them into
 * @ref tapi_cybersec findings.
 *
 * @code
 * tapi_cybersec_report report;
 *
 * tapi_cybersec_report_init(&report);
 * CHECK_RC(tapi_bt_audit(factory, TAPI_BT_AUTO, NULL, 30000, &report));
 * tapi_cybersec_report_log(&report);
 * @endcode
 *
 * @note Most of it is read from one line of @c btmgmt output, so it is
 *       fullest on Linux. macOS reports whether the radio is on and
 *       discoverable and nothing about how it pairs; Windows reports
 *       less again. Each finding says which systems can raise it, and
 *       @c bt.not-assessed is raised where the rest could not be
 *       asked - a silent report from a system that cannot answer is
 *       the thing to avoid.
 */

#ifndef __TSF_TAPI_BT_AUDIT_H__
#define __TSF_TAPI_BT_AUDIT_H__

#include "te_defs.h"
#include "te_errno.h"

#include "tapi_cybersec.h"
#include "tapi_bt.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What the agent's Bluetooth is expected to be. */
typedef struct tapi_bt_policy {
    /** The adapter may answer an inquiry from a stranger. */
    bool allow_discoverable;
    /** The adapter may accept a bond it did not ask for. */
    bool allow_pairable;
    /**
     * Addresses the agent is expected to be paired with, in any of the
     * usual forms. A bond with anything else is reported.
     */
    const char **expected_devices;
    /** Number of @a expected_devices; @c 0 turns the check off. */
    size_t n_expected_devices;
    /** Report a device name that gives away the host's name. */
    bool check_name;
} tapi_bt_policy;

/** The default: nothing unsolicited is allowed. */
extern const tapi_bt_policy tapi_bt_default_policy;

/**
 * Read the agent's Bluetooth posture and add what is wrong to
 * @p report.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  backend      Backend, or @ref TAPI_BT_AUTO.
 * @param[in]  policy       What is expected, or @c NULL for the
 *                          default.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] report       Report to append findings to.
 *
 * @return Status code of reading the posture, not its verdict.
 * @retval TE_ENOENT        There is no adapter to look at.
 */
extern te_errno tapi_bt_audit(tapi_job_factory_t *factory,
                              tapi_bt_backend backend,
                              const tapi_bt_policy *policy, int timeout_ms,
                              tapi_cybersec_report *report);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_BT_AUDIT_H__ */

/**@} <!-- END tapi_bt_audit --> */
