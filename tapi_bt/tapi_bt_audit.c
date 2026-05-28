/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief What the Bluetooth of an agent is worth
 */

#define TE_LGR_USER "TAPI BT"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_str.h"
#include "te_string.h"
#include "te_vector.h"

#include "tapi_bt_audit.h"
#include "tapi_bt_device.h"
#include "tapi_bt_internal.h"

const tapi_bt_policy tapi_bt_default_policy = {
    .allow_discoverable = false,
    .allow_pairable = false,
    .check_name = true,
};

/**
 * Does the settings line hold this word?
 *
 * The line is space separated - "powered bondable ssp br/edr le
 * secure-conn" - so a plain substring search would find "conn" inside
 * "secure-conn" and "le" inside every other word. Boundaries matter.
 */
static bool
bt_setting(const char *settings, const char *word)
{
    size_t len = strlen(word);
    const char *pos = settings;

    if (settings == NULL)
        return false;

    while ((pos = strstr(pos, word)) != NULL)
    {
        char before = pos == settings ? ' ' : pos[-1];
        char after = pos[len];

        if ((before == ' ' || before == '\t') &&
            (after == '\0' || after == ' ' || after == '\t' ||
             after == '\n'))
        {
            return true;
        }

        pos += len;
    }

    return false;
}

/** Findings that come from the adapter's own settings. */
static void
bt_audit_adapter(const tapi_bt_adapter *adapter,
                 const tapi_bt_policy *policy, tapi_cybersec_report *report)
{
    const char *subject = adapter->address[0] != '\0' ?
                          adapter->address : "adapter";
    const char *settings = adapter->detail;

    if (!adapter->powered)
    {
        /*
         * Reported, and reported as information: a radio that is off
         * is the safest a radio gets, and everything below was read
         * from a system that had nothing to say.
         */
        tapi_cybersec_report_add(report, TAPI_CYBERSEC_SEV_INFO,
            "bt.powered-off", subject,
            "The adapter is off, so nothing else here was measured "
            "against a live radio.");
        return;
    }

    if (adapter->discoverable && !policy->allow_discoverable)
    {
        tapi_cybersec_report_add(report,
            adapter->pairable && !policy->allow_pairable ?
                TAPI_CYBERSEC_SEV_HIGH : TAPI_CYBERSEC_SEV_MEDIUM,
            "bt.discoverable", subject,
            "The adapter answers an inquiry from anyone in range%s. "
            "Discoverability is meant to be turned on to pair and turned "
            "off again.",
            adapter->pairable ? ", and accepts a bond from them" : "");
    }
    else if (adapter->pairable && !policy->allow_pairable &&
             bt_setting(settings, "connectable"))
    {
        tapi_cybersec_report_add(report, TAPI_CYBERSEC_SEV_LOW,
            "bt.pairable", subject,
            "The adapter accepts a bond it did not ask for. It is not "
            "discoverable, so a stranger needs the address first - which "
            "is not a secret.");
    }

    /*
     * The rest is read out of the settings line, which only BlueZ
     * gives. Everything below this point is therefore Linux-only, and
     * its absence elsewhere is reported rather than passed over.
     */
    if (settings == NULL)
    {
        tapi_cybersec_report_add(report, TAPI_CYBERSEC_SEV_INFO,
            "bt.not-assessed", subject,
            "This system does not report how the adapter pairs or "
            "encrypts, so the pairing checks were not run. Only what is "
            "listed above was measured.");
        return;
    }

    if (bt_setting(settings, "debug-keys"))
    {
        /*
         * The debug key is in the specification. A link encrypted with
         * it is a link anyone listening can decrypt, and it exists so
         * that a sniffer can be pointed at a device on a bench.
         */
        tapi_cybersec_report_add(report, TAPI_CYBERSEC_SEV_CRITICAL,
            "bt.debug-keys", subject,
            "The adapter is using debug keys. The key is published in the "
            "specification, so every link it makes can be decrypted by "
            "anyone within earshot. This belongs on a bench and nowhere "
            "else.");
    }

    if (bt_setting(settings, "br/edr") && !bt_setting(settings, "secure-conn"))
    {
        tapi_cybersec_report_add(report, TAPI_CYBERSEC_SEV_MEDIUM,
            "bt.no-secure-connections", subject,
            "Secure Connections is off, so classic pairing falls back to "
            "the legacy P-192 exchange. Current settings: %s", settings);
    }

    if (bt_setting(settings, "br/edr") && !bt_setting(settings, "ssp"))
    {
        tapi_cybersec_report_add(report, TAPI_CYBERSEC_SEV_HIGH,
            "bt.no-ssp", subject,
            "Secure Simple Pairing is off, so pairing uses a legacy PIN. "
            "Current settings: %s", settings);
    }

    if (bt_setting(settings, "le") && !bt_setting(settings, "privacy"))
    {
        tapi_cybersec_report_add(report, TAPI_CYBERSEC_SEV_LOW,
            "bt.no-le-privacy", subject,
            "LE privacy is off, so the adapter advertises one address "
            "for as long as it is up and can be followed by it.");
    }
}

/** Findings that come from the devices the system is bonded to. */
static void
bt_audit_devices(tapi_job_factory_t *factory, tapi_bt_backend backend,
                 const tapi_bt_policy *policy, int timeout_ms,
                 tapi_cybersec_report *report)
{
    te_vec devices;
    size_t i;

    if (policy->n_expected_devices == 0)
        return;

    if (!tapi_bt_supports(backend, TAPI_BT_FEAT_KNOWN))
    {
        tapi_cybersec_report_add(report, TAPI_CYBERSEC_SEV_INFO,
            "bt.bonds-not-assessed", tapi_bt_backend2str(backend),
            "This system keeps no list of the devices it has bonded "
            "with, so the expected-device check was not run.");
        return;
    }

    if (tapi_bt_known(factory, backend, timeout_ms, &devices) != 0)
        return;

    tapi_bt_devices_log("the system knows", &devices);

    for (i = 0; i < te_vec_size(&devices); i++)
    {
        const tapi_bt_device *device = te_vec_get(&devices, i);
        bool expected = false;
        size_t j;

        if (!device->paired)
            continue;

        for (j = 0; j < policy->n_expected_devices; j++)
        {
            char normalized[TAPI_BT_ADDR_LEN];

            if (tapi_bt_addr_normalize(policy->expected_devices[j],
                                       normalized) &&
                strcmp(normalized, device->address) == 0)
            {
                expected = true;
                break;
            }
        }

        if (!expected)
        {
            /*
             * The subject is the device, not the adapter, so two
             * unexpected bonds are two findings and a verdict names
             * which one. The name is in the detail and not the
             * subject: a device can be renamed, and TRC has to match
             * the same finding tomorrow.
             */
            tapi_cybersec_report_add(report, TAPI_CYBERSEC_SEV_MEDIUM,
                "bt.unexpected-bond", device->address,
                "The agent is bonded to %s (%s), which the policy does "
                "not list. A bond is a stored key and survives a reboot.",
                device->address,
                device->name != NULL ? device->name : "no name");
        }
    }

    tapi_bt_devices_free(&devices);
}

/* See description in tapi_bt_audit.h */
te_errno
tapi_bt_audit(tapi_job_factory_t *factory, tapi_bt_backend backend,
              const tapi_bt_policy *policy, int timeout_ms,
              tapi_cybersec_report *report)
{
    tapi_bt_adapter adapter;
    te_errno rc;

    if (policy == NULL)
        policy = &tapi_bt_default_policy;

    rc = tapi_bt_adapter_get(factory, backend, timeout_ms, &adapter);
    if (rc != 0)
        return rc;

    tapi_bt_adapter_log(&adapter);

    bt_audit_adapter(&adapter, policy, report);

    if (policy->check_name && adapter.name != NULL &&
        adapter.name[0] != '\0' && adapter.powered && adapter.discoverable)
    {
        /*
         * Only worth saying when the adapter is discoverable: a name
         * that nobody can ask for gives nothing away. When it can be
         * asked for, it is usually the machine's hostname, and a
         * hostname tells a stranger whose laptop this is.
         */
        tapi_cybersec_report_add(report, TAPI_CYBERSEC_SEV_LOW,
            "bt.name-broadcast", adapter.address,
            "The adapter is discoverable under the name '%s', which "
            "anyone in range can read.", adapter.name);
    }

    bt_audit_devices(factory, adapter.backend, policy, timeout_ms, report);

    tapi_bt_adapter_free(&adapter);

    return 0;
}
