/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief The HCI link itself
 *
 * @defgroup tapi_bt_hci HCI capture (tapi_bt_hci)
 * @ingroup tapi_bt
 * @{
 *
 * Everything else in this library asks the system what it thinks. This
 * records what actually went over the link, which is the only thing
 * that settles an argument about whether a pairing was encrypted or
 * which version of it ran.
 *
 * @section tapi_bt_hci_where Where this works
 *
 * **Linux only, and that is not an omission.** The Linux kernel offers
 * a monitor socket that hands a copy of every HCI packet to anything
 * allowed to listen, which is what @c btmon reads. Nothing equivalent
 * is offered to a program on the other two:
 *
 * - **macOS** keeps the link inside the system. Apple's PacketLogger
 *   can see it, and PacketLogger is a separate download that a test
 *   cannot assume and cannot drive.
 * - **Windows** does have a trace, through ETW, but it is an ETW trace
 *   and not an HCI socket: a different capture, in a different format
 *   (@c .etl), started through @c netsh or @c logman, and converted
 *   before anything can read it. tapi_bt_hci_start() says so rather
 *   than producing something that looks like a capture and is not.
 *
 * @code
 * tapi_bt_hci_trace trace;
 *
 * CHECK_RC(tapi_bt_hci_start(factory, TAPI_BT_AUTO, NULL, &trace));
 * ... do the thing under test ...
 * CHECK_RC(tapi_bt_hci_stop(&trace));
 * RING("the capture is on the agent at %s", trace.path);
 * tapi_bt_hci_free(&trace);
 * @endcode
 */

#ifndef __TSF_TAPI_BT_HCI_H__
#define __TSF_TAPI_BT_HCI_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "tapi_job.h"

#include "tapi_bt.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A running capture. */
typedef struct tapi_bt_hci_trace tapi_bt_hci_trace;

/**
 * Start recording the HCI link.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  backend      Backend, or @ref TAPI_BT_AUTO.
 * @param[in]  path         Where to write on the agent, or @c NULL for
 *                          a file in its temporary directory.
 * @param[out] trace        Handle; release with tapi_bt_hci_free().
 *
 * @return Status code.
 * @retval TE_EOPNOTSUPP    The system does not give a program the HCI
 *                          link.
 */
extern te_errno tapi_bt_hci_start(tapi_job_factory_t *factory,
                                  tapi_bt_backend backend,
                                  const char *path,
                                  tapi_bt_hci_trace **trace);

/**
 * Stop recording.
 *
 * @param trace         Handle.
 *
 * @return Status code.
 */
extern te_errno tapi_bt_hci_stop(tapi_bt_hci_trace *trace);

/**
 * Where the capture is, on the agent.
 *
 * @param trace         Handle.
 *
 * @return The path, never @c NULL.
 */
extern const char *tapi_bt_hci_path(const tapi_bt_hci_trace *trace);

/**
 * Summarise a finished capture into the log.
 *
 * Runs @c btmon over the file and reports what it found, so that a
 * test that failed leaves an account of the link in its log rather
 * than only a file nobody will open.
 *
 * @param[in]  trace        A stopped capture.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] text         String to append the summary to, or
 *                          @c NULL to only log it.
 *
 * @return Status code.
 */
extern te_errno tapi_bt_hci_summary(tapi_bt_hci_trace *trace,
                                    int timeout_ms, te_string *text);

/**
 * Release a capture handle. The file on the agent stays.
 *
 * @param trace         Handle.
 */
extern void tapi_bt_hci_free(tapi_bt_hci_trace *trace);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_BT_HCI_H__ */

/**@} <!-- END tapi_bt_hci --> */
