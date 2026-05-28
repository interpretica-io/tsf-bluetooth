/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Bluetooth TAPI: internal helpers
 *
 * Internal to tsf-bluetooth; not installed.
 */

#ifndef __TSF_TAPI_BT_INTERNAL_H__
#define __TSF_TAPI_BT_INTERNAL_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "te_vector.h"
#include "tapi_job.h"

#include "tapi_devtool_run.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Append one argument to a vector, taking ownership of it. */
extern void tapi_bt_arg(te_vec *args, const char *fmt, ...)
    TE_LIKE_PRINTF(2, 3);

/**
 * Run @p program with @p args and wait for it.
 *
 * Waited for, not checked. Every tool this library drives has its own
 * idea of failure - see @ref tapi_bt_available() for how little the
 * exit status is worth here - so the caller gets the status and the
 * output and decides.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  name         Tool name for log messages.
 * @param[in]  program      Program name or path.
 * @param[in]  args         Arguments after @c argv[0].
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] out          Standard output, or @c NULL.
 * @param[out] err          Standard error, or @c NULL.
 * @param[out] exit_code    Exit status, or @c NULL. @c -1 when the
 *                          tool died on a signal, which
 *                          @c bluetoothctl does.
 *
 * @return Status code of running the tool, not of the tool.
 */
extern te_errno tapi_bt_cmd(tapi_job_factory_t *factory, const char *name,
                            const char *program, const te_vec *args,
                            int timeout_ms, te_string *out, te_string *err,
                            int *exit_code);

/**
 * Start @p program with @p args and leave it running.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  name         Tool name for log messages.
 * @param[in]  program      Program name or path.
 * @param[in]  args         Arguments after @c argv[0].
 * @param[out] run          Run handle; release it with
 *                          tapi_devtool_run_fini().
 *
 * @return Status code.
 */
extern te_errno tapi_bt_spawn(tapi_job_factory_t *factory, const char *name,
                              const char *program, const te_vec *args,
                              tapi_devtool_run *run);

/**
 * Run a PowerShell command on a Windows agent.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  script       The command, as a single PowerShell line.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] out          Standard output.
 * @param[out] exit_code    Exit status, or @c NULL.
 *
 * @return Status code.
 */
extern te_errno tapi_bt_powershell(tapi_job_factory_t *factory,
                                   const char *script, int timeout_ms,
                                   te_string *out, int *exit_code);

/**
 * Pull one JSON string field out of a flat object.
 *
 * Not a JSON parser: the objects this reads are produced by
 * @c blueutil and by @c ConvertTo-Json, they are flat, and their
 * values are strings, numbers or booleans. A parser would be the right
 * thing if TE had one to use - te_json is a writer only.
 *
 * @param[in]  object   Text of one JSON object.
 * @param[in]  key      Field name, without quotes.
 * @param[out] dest     String to append the value to.
 *
 * @return @c true when the field was there.
 */
extern bool tapi_bt_json_str(const char *object, const char *key,
                             te_string *dest);

/**
 * Pull one JSON boolean field out of a flat object.
 *
 * @param[in]  object   Text of one JSON object.
 * @param[in]  key      Field name.
 * @param[out] value    Where to put it.
 *
 * @return @c true when the field was there.
 */
extern bool tapi_bt_json_bool(const char *object, const char *key,
                              bool *value);

/**
 * Pull one JSON number field out of a flat object.
 *
 * @param[in]  object   Text of one JSON object.
 * @param[in]  key      Field name.
 * @param[out] value    Where to put it.
 *
 * @return @c true when the field was there.
 */
extern bool tapi_bt_json_int(const char *object, const char *key,
                             int *value);

/**
 * Walk the objects of a flat JSON array.
 *
 * @param[in]  array    Text of a JSON array.
 * @param[in]  prev     Where the previous object ended, or @c NULL to
 *                      start.
 * @param[out] object   String to append the object's text to; reset
 *                      first.
 *
 * @return Where this object ended, or @c NULL when there are no more.
 */
extern const char *tapi_bt_json_next(const char *array, const char *prev,
                                     te_string *object);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_BT_INTERNAL_H__ */
