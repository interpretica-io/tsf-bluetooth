/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief The HCI link itself
 */

#define TE_LGR_USER "TAPI BT"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "tapi_cfg_base.h"
#include "tapi_file.h"
#include "te_alloc.h"
#include "te_string.h"
#include "te_vector.h"

#include "tapi_bt_hci.h"
#include "tapi_bt_internal.h"

/** How long btmon is given to notice it should stop, ms. */
#define BT_HCI_TERM_MS 5000

/** What btmon prints once the monitor socket is open. */
#define BT_HCI_READY "Bluetooth monitor"

/** How long to wait for that line, ms. */
#define BT_HCI_READY_MS 10000

/**
 * How long to give btmon to fall over after printing its banner, ms.
 *
 * It exits immediately when it cannot open the monitor socket, so this
 * only has to outlast the gap between the banner and the exit.
 */
#define BT_HCI_SETTLE_MS 500

struct tapi_bt_hci_trace {
    /** The btmon job. */
    tapi_devtool_run run;
    /** Job factory, for the summary pass. */
    tapi_job_factory_t *factory;
    /** Where the capture is written on the agent. */
    char *path;
    /** @c true once it has been stopped. */
    bool stopped;
};

/* See description in tapi_bt_hci.h */
te_errno
tapi_bt_hci_start(tapi_job_factory_t *factory, tapi_bt_backend backend,
                  const char *path, tapi_bt_hci_trace **trace)
{
    tapi_bt_backend actual = backend;
    tapi_bt_hci_trace *result;
    te_vec args = TE_VEC_INIT(char *);
    te_errno rc;

    if (actual == TAPI_BT_AUTO)
    {
        rc = tapi_bt_detect(factory, TAPI_BT_TIMEOUT_MS, &actual);
        if (rc != 0)
            return rc;
    }

    if (!tapi_bt_supports(actual, TAPI_BT_FEAT_HCI_TRACE))
    {
        ERROR("%s does not give a program the HCI link. On macOS it is "
              "inside the system and only PacketLogger sees it; on "
              "Windows there is an ETW trace instead, which is a "
              "different capture in a different format - start it with "
              "netsh or logman and read the .etl with a tool that "
              "understands one.", tapi_bt_backend2str(actual));
        return TE_RC(TE_TAPI, TE_EOPNOTSUPP);
    }

    result = TE_ALLOC(sizeof(*result));
    result->run = (tapi_devtool_run)TAPI_DEVTOOL_RUN_INIT;
    result->factory = factory;

    if (path != NULL)
    {
        result->path = TE_STRDUP(path);
    }
    else
    {
        const char *ta = tapi_job_factory_ta(factory);
        te_string generated = TE_STRING_INIT;
        char *tmp_dir;

        if (ta == NULL)
        {
            ERROR("Cannot determine the agent behind the job factory");
            free(result);
            return TE_RC(TE_TAPI, TE_EINVAL);
        }

        tmp_dir = tapi_cfg_base_get_ta_dir(ta, TAPI_CFG_BASE_TA_DIR_TMP);
        if (tmp_dir == NULL)
        {
            ERROR("Failed to get the temporary directory of TA %s", ta);
            free(result);
            return TE_RC(TE_TAPI, TE_EFAIL);
        }

        tapi_file_make_custom_pathname(&generated, tmp_dir, ".btsnoop");
        free(tmp_dir);
        result->path = generated.ptr;
    }

    tapi_bt_arg(&args, "-w");
    tapi_bt_arg(&args, "%s", result->path);

    rc = tapi_bt_spawn(factory, "btmon", "btmon", &args, &result->run);
    te_vec_deep_free(&args);

    if (rc != 0)
    {
        free(result->path);
        free(result);
        return rc;
    }

    /*
     * Two checks, and the second is the one that matters.
     *
     * Waiting for the banner is not enough: measured on BlueZ 5.72,
     * btmon prints "Bluetooth monitor ver 5.72" on standard output
     * whether or not it managed to open anything, writes the real
     * complaint to standard error, and exits 1. So a readiness check
     * that only looked for the banner would report a running capture
     * every time, and the test would go on to record nothing and
     * notice at the end.
     *
     * The banner says it started; still being alive a moment later
     * says it worked.
     */
    rc = tapi_devtool_run_expect(&result->run, BT_HCI_READY,
                                 BT_HCI_READY_MS);
    if (rc == 0)
    {
        rc = tapi_devtool_run_wait(&result->run, BT_HCI_SETTLE_MS);

        if (TE_RC_GET_ERROR(rc) == TE_EINPROGRESS)
        {
            /* Still running, which is what a capture should be. */
            rc = 0;
        }
        else
        {
            tapi_devtool_output output;

            tapi_devtool_run_get_output(&result->run, &output);
            ERROR("btmon stopped instead of capturing: %s",
                  output.err != NULL && output.err[0] != '\0' ?
                      output.err : "no reason given");
            rc = TE_RC(TE_TAPI, TE_EFAIL);
        }
    }

    if (rc != 0)
    {
        ERROR("The HCI link could not be recorded. btmon needs the "
              "monitor socket, which wants CAP_NET_ADMIN: an agent "
              "running as an ordinary user does not have it.");
        (void)tapi_devtool_run_stop(&result->run);
        tapi_devtool_run_fini(&result->run);
        free(result->path);
        free(result);
        return rc;
    }

    RING("Recording the HCI link to %s", result->path);

    *trace = result;

    return 0;
}

/* See description in tapi_bt_hci.h */
const char *
tapi_bt_hci_path(const tapi_bt_hci_trace *trace)
{
    return trace->path;
}

/* See description in tapi_bt_hci.h */
te_errno
tapi_bt_hci_stop(tapi_bt_hci_trace *trace)
{
    te_errno rc;

    if (trace == NULL || trace->stopped)
        return 0;

    rc = tapi_devtool_run_stop(&trace->run);
    if (rc == 0)
        (void)tapi_devtool_run_wait(&trace->run, BT_HCI_TERM_MS);

    trace->stopped = true;

    return rc;
}

/* See description in tapi_bt_hci.h */
te_errno
tapi_bt_hci_summary(tapi_bt_hci_trace *trace, int timeout_ms,
                    te_string *text)
{
    te_vec args = TE_VEC_INIT(char *);
    te_string out = TE_STRING_INIT;
    int code = 0;
    te_errno rc;

    (void)tapi_bt_hci_stop(trace);

    tapi_bt_arg(&args, "-r");
    tapi_bt_arg(&args, "%s", trace->path);

    rc = tapi_bt_cmd(trace->factory, "btmon", "btmon", &args, timeout_ms,
                     &out, NULL, &code);

    if (rc == 0)
    {
        RING("HCI capture %s:\n%s", trace->path, te_string_value(&out));
        if (text != NULL)
            te_string_append(text, "%s", te_string_value(&out));
    }

    te_vec_deep_free(&args);
    te_string_free(&out);

    return rc;
}

/* See description in tapi_bt_hci.h */
void
tapi_bt_hci_free(tapi_bt_hci_trace *trace)
{
    if (trace == NULL)
        return;

    (void)tapi_bt_hci_stop(trace);
    tapi_devtool_run_fini(&trace->run);

    free(trace->path);
    free(trace);
}
