/*  =========================================================================
    mailbox - mailbox deliver

    Copyright (C) 2014 - 2017 Eaton

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    =========================================================================
*/

/*
@header
    mailbox - mailbox deliver
@discuss
@end
*/

#include "fty_metric_cache_classes.h"

#define ENDPOINT "ipc://@/malamute"
//  --------------------------------------------------------------------------
//  Perform mailbox deliver protocol
void
mailbox_perform (mlm_client_t *client, zmsg_t **msg_p, rt_t *data)
{
    assert (client);
    assert (msg_p);
    assert (data);

    if (!*msg_p)
        return;
    zmsg_t *msg = *msg_p;

    // check subject
    if (!streq (mlm_client_subject (client), RFC_RT_DATA_SUBJECT)) {
        zmsg_destroy (msg_p);
        log_warning (
                "Message with bad subject received. Sender: '%s', Subject: '%s'.",
                mlm_client_sender (client), mlm_client_subject (client));
        return;
    }
    // check uuid
    char *uuid = zmsg_popstr (msg);
    if (!uuid) {
        zmsg_destroy (msg_p);
        log_warning (
                "Bad message. Expected multipart string message `uuid/...`"
                " - 'uuid' field is missing. Sender: '%s', Subject: '%s'.",
                mlm_client_sender (client), mlm_client_subject (client));
        return;
    }
    // check command
    char *command = zmsg_popstr (msg);
    if (!command) {
        zmsg_destroy (msg_p);
        zstr_free (&uuid);
        log_warning (
                "Bad message. Expected multipart string message `uuid/(GET|LIST)...`"
                " - 'GET/LIST' string is missing. Sender: '%s', Subject: '%s'.",
                mlm_client_sender (client), mlm_client_subject (client));
        return;
    }

    if (streq (command, "LIST")) {

        zmsg_t *send = zmsg_new ();
        zmsg_addstr (send, uuid);
        zmsg_addstr (send, "OK");
        zmsg_addstr (send, command);
        zmsg_addstr (send, rt_get_list_devices(data));

        int rv = mlm_client_sendto (client, mlm_client_sender(client), RFC_RT_DATA_SUBJECT, NULL, 5000, &send);
        if ( rv != 0 ) {
            log_error (
                    "mlm_client_sendto (sender = '%s', subject = '%s', timeout = '5000') failed.",
                    mlm_client_sender (client), RFC_RT_DATA_SUBJECT);
        }
    } else if(streq (command, "GET")) {
        // check element
        char *element = zmsg_popstr (msg);
        if (!element) {
            zstr_free (&command);
            zstr_free (&uuid);
            zmsg_destroy (msg_p);
            log_warning (
                    "Bad message. Expected multipart string message `uuid/GET/element`"
                    " - 'element' is missing. Sender: '%s', Subject: '%s'.",
                    mlm_client_sender (client), mlm_client_subject (client));
            return;
        }
        zmsg_t *reply = zmsg_new ();
        zmsg_addstr (reply, uuid);
        zmsg_addstr (reply, "OK");
        zmsg_addstr (reply, element);

        zhashx_t *hash = rt_get_element (data, element);
        uint64_t now_s = time(NULL);
        if (hash) {
            fty_proto_t *metric = (fty_proto_t *) zhashx_first (hash);
            while (metric) {
                if ( fty_proto_time(metric) + fty_proto_ttl(metric) > now_s ) {
                    fty_proto_t *copy = fty_proto_dup (metric);
                    zmsg_t *encoded = fty_proto_encode (&copy);
                    zmsg_addmsg (reply, &encoded);
                }
                metric = (fty_proto_t *) zhashx_next (hash);
            }
        }

        zstr_free (&element);

        int rv = mlm_client_sendto (client, mlm_client_sender(client), RFC_RT_DATA_SUBJECT, NULL, 5000, &reply);

        if (rv != 0) {
            log_error (
                    "mlm_client_sendto (sender = '%s', subject = '%s', timeout = '5000') failed.",
                    mlm_client_sender (client), RFC_RT_DATA_SUBJECT);
        }
    } else {
        log_warning (
                "Unrecognized command %s. Sender: '%s', Subject: '%s'.",
                command, mlm_client_sender (client), mlm_client_subject (client));
    }
    zstr_free (&uuid);
    zstr_free (&command);
    zmsg_destroy (msg_p);

}
//  --------------------------------------------------------------------------
//  Self test of this class

static fty_proto_t *
test_metric_new (
        const char *type,
        const char *element,
        const char *value,
        const char *unit,
        uint32_t ttl
        )
{
    fty_proto_t *metric = fty_proto_new (FTY_PROTO_METRIC);
    fty_proto_set_type (metric, "%s", type);
    fty_proto_set_name (metric, "%s", element);
    fty_proto_set_unit (metric, "%s", unit);
    fty_proto_set_value (metric, "%s", value);
    fty_proto_set_ttl (metric, ttl);
    return metric;
}

static void
test_assert_proto (
        fty_proto_t *p,
        const char *type,
        const char *element,
        const char *value,
        const char *unit,
        uint32_t ttl)
{
    assert (p);
    assert (streq (fty_proto_type (p), type));
    assert (streq (fty_proto_name (p), element));
    assert (streq (fty_proto_unit (p), unit));
    assert (streq (fty_proto_value (p), value));
    assert (fty_proto_ttl (p) == ttl);
}

void
mailbox_test (bool verbose)
{
    static const char* endpoint = "inproc://fty-metric-cache-mailbox-test";

    printf (" * mailbox: ");
    //  @selftest

    // Malamute
    zactor_t *server = zactor_new (mlm_server, "Malamute");
    zstr_sendx (server, "BIND", endpoint, NULL);
    if (verbose)
        zstr_send (server, "VERBOSE");

    // User Interface
    mlm_client_t *ui = mlm_client_new ();
    mlm_client_connect (ui, endpoint, 1000, "UI");

    // Mailbox peer
    mlm_client_t *mailbox = mlm_client_new ();
    mlm_client_connect (mailbox, endpoint, 1000, "MAILBOX");

    // data, fill
    rt_t *data = rt_new ();
    fty_proto_t *metric = test_metric_new ("temp", "ups", "15", "C", 100);
    rt_put (data, &metric);
    metric = test_metric_new ("humidity", "ups", "40", "%", 200);
    rt_put (data, &metric);
    metric = test_metric_new ("battery.remaining", "ups", "20", "%", 200);
    rt_put (data, &metric);
    metric = test_metric_new ("humidity", "epdu", "21", "%", 100);
    rt_put (data, &metric);
    metric = test_metric_new ("load.input", "switch", "134", "V", 200);
    rt_put (data, &metric);
    metric = test_metric_new ("amperes", "switch", "50", "A", 300);
    rt_put (data, &metric);

    // ===============================================
    // Test case #1:
    //      GET ups
    // Expected:
    //      3 measurements
    // ===============================================
    zmsg_t *send = zmsg_new ();
    zmsg_addstr (send, "12345");
    zmsg_addstr (send, "GET");
    zmsg_addstr (send, "ups");
    int rv = mlm_client_sendto (ui, "MAILBOX", RFC_RT_DATA_SUBJECT, NULL, 5000, &send);
    assert (rv == 0);

    zmsg_t *reply = mlm_client_recv (mailbox);
    assert (reply);
    mailbox_perform (mailbox, &reply, data);
    reply = mlm_client_recv (ui);
    assert (reply);
    assert (streq (mlm_client_subject (ui), RFC_RT_DATA_SUBJECT));

    char *uuid = zmsg_popstr (reply);
    assert (uuid);
    assert (streq (uuid, "12345"));
    zstr_free (&uuid);

    char *command = zmsg_popstr (reply);
    assert (command);
    assert (streq (command, "OK"));
    zstr_free (&command);

    char *element = zmsg_popstr (reply);
    assert (element);
    assert (streq (element, "ups"));
    zstr_free (&element);

    zmsg_t *encoded = zmsg_popmsg (reply);
    assert (encoded);
    fty_proto_t *proto = fty_proto_decode (&encoded);
    test_assert_proto (proto, "battery.remaining", "ups", "20", "%", 200);
    fty_proto_destroy (&proto);

    encoded = zmsg_popmsg (reply);
    assert (encoded);
    proto = fty_proto_decode (&encoded);
    test_assert_proto (proto, "temp", "ups", "15", "C", 100);
    fty_proto_destroy (&proto);

    encoded = zmsg_popmsg (reply);
    assert (encoded);
    proto = fty_proto_decode (&encoded);
    test_assert_proto (proto, "humidity", "ups", "40", "%", 200);

    fty_proto_destroy (&proto);

    encoded = zmsg_popmsg (reply);
    assert (encoded == NULL);
    zmsg_destroy (&reply);

    // End Test case #1

    // ===============================================
    // Test case #2:
    //      Send bad message
    // Expected:
    //      no reply
    // ===============================================
    send = zmsg_new ();
    zmsg_addstr (send, "GET");
    zmsg_addstr (send, "ups");
    rv = mlm_client_sendto (ui, "MAILBOX", RFC_RT_DATA_SUBJECT, NULL, 5000, &send);
    assert (rv == 0);

    reply = mlm_client_recv (mailbox);
    assert (reply);
    mailbox_perform (mailbox, &reply, data);

    zsys_debug ("Waiting in zpoller for 5000ms");
    zpoller_t *poller = zpoller_new (mlm_client_msgpipe (ui), NULL);
    void *which = zpoller_wait (poller, 5000);
    assert (which == NULL);
    assert (zpoller_expired (poller));

    zpoller_destroy (&poller);

    // End Test case #2

    // ===============================================
    // Test case #3:
    //      GET non-existant-element
    // Expected:
    //      0 measurements
    // ===============================================
    send = zmsg_new ();
    zmsg_addstr (send, "8cb3e9a9-649b-4bef-8de2-25e9c2cebb38");
    zmsg_addstr (send, "GET");
    zmsg_addstr (send, "non-existant-element");
    rv = mlm_client_sendto (ui, "MAILBOX", RFC_RT_DATA_SUBJECT, NULL, 5000, &send);
    assert (rv == 0);

    reply = mlm_client_recv (mailbox);
    assert (reply);
    mailbox_perform (mailbox, &reply, data);
    reply = mlm_client_recv (ui);
    assert (reply);
    assert (streq (mlm_client_subject (ui), RFC_RT_DATA_SUBJECT));

    uuid = zmsg_popstr (reply);
    assert (uuid);
    assert (streq (uuid, "8cb3e9a9-649b-4bef-8de2-25e9c2cebb38"));
    zstr_free (&uuid);

    command = zmsg_popstr (reply);
    assert (command);
    assert (streq (command, "OK"));
    zstr_free (&command);

    element = zmsg_popstr (reply);
    assert (element);
    assert (streq (element, "non-existant-element"));
    zstr_free (&element);

    encoded = zmsg_popmsg (reply);
    assert (encoded == NULL);
    zmsg_destroy (&reply);

    // End Test case #3

    rt_destroy (&data);
    mlm_client_destroy (&ui);
    mlm_client_destroy (&mailbox);
    zactor_destroy (&server);

    //  @end
    printf ("OK\n");
}
