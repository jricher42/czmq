/*  =========================================================================
    zgossip - gossip protocol service

    Copyright (c) the Contributors as noted in the AUTHORS file.
    This file is part of CZMQ, the high-level C binding for 0MQ:
    http://czmq.zeromq.org.

    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at http://mozilla.org/MPL/2.0/.
    =========================================================================
*/

/*
@header
    Implements a gossip protocol (RFC TBD).
@discuss
    The gossip protocol distributes information around a loosely-connected
    network of gossip services. The information consists of name/value pairs
    published by applications at any point in the network. The goal of the
    gossip protocol is to create eventual consistency between all the using
    applications.

    The name/value pairs (tuples) can be used for configuration data, for
    status updates, for presence, or for discovery. When used for discovery,
    the gossip protocol works as an alternative to e.g. UDP beaconing.

    The gossip network consists of a set of loosely-coupled nodes that
    exchange tuples. Nodes can be connected across arbitrary transports,
    so the gossip network can have nodes that communicate over inproc,
    over IPC, and/or over TCP, at the same time.

    Each node runs the same stack, which is a server-client hybrid using
    a modified Harmony pattern (from Chapter 8 of the Guide):
    http://zguide.zeromq.org/page:all#True-Peer-Connectivity-Harmony-Pattern

    Each node provides a ROUTER socket that accepts client connections on an
    key defined by the application via a BIND command. The state machine
    for these connections is in zgossip.xml, and the generated code is in
    zgossip_engine.h.

    Each node additionally creates outbound connections via DEALER sockets
    to a set of servers ("remotes"), and under control of the calling app,
    which sends CONNECT commands for each configured remote.

    The messages between client and server are defined in zgossip_msg.xml.
    This stack is built using the zeromq/zproto toolkit.

    To join the gossip network, a node has to connect to one or more peers.
    Every peer acts as a forwarder. This loosely-coupled network can scale
    to thousands of nodes. However the gossip protocol is NOT designed to
    be efficient, and should not be used for application data, as the same
    tuples may be sent many times across the network.

    The basic logic of the gossip service is to accept PUBLISH messages
    from its owning application, and to forward these to every remote, and
    every client it talks to. When a node gets a duplicate tuple, it throws
    it away. When a node gets a new tuple, it stores it, and fowards it as
    just described. At any point the application can access the node's set
    of tuples.
    
    The protocol uses ping-pong heartbeating to monitor presence. This code
    doesn't do anything with expired nodes yet.

    The assumptions in this design are:

    * The data set is slow-changing. Thus, the cost of the gossip protocol
      is irrelevant with respect to other traffic.
@end
*/

#include "../include/czmq.h"
#include "zgossip_msg.h"


//  ---------------------------------------------------------------------
//  Forward declarations for the two main classes we use here

typedef struct _server_t server_t;
typedef struct _client_t client_t;
typedef struct _tuple_t tuple_t;

//  ---------------------------------------------------------------------
//  This structure defines the context for each running server. Store
//  whatever properties and structures you need for the server.

struct _server_t {
    //  These properties must always be present in the server_t
    //  and are set by the generated engine; do not modify them!
    zconfig_t *config;          //  Current loaded configuration
    
    //  Add any properties you need here
    zlist_t *remotes;           //  Parents, as zsock_t instances
    zhash_t *tuples;            //  Tuples, indexed by key

    tuple_t *cur_tuple;         //  Holds current tuple to publish
};

//  ---------------------------------------------------------------------
//  This structure defines the state for each client connection. It will
//  be passed to each action in the 'self' argument.

struct _client_t {
    //  These properties must always be present in the client_t
    //  and are set by the generated engine; do not modify them!
    server_t *server;           //  Reference to parent server
    zgossip_msg_t *request;     //  Last received request
    zgossip_msg_t *reply;       //  Reply to send out, if any
};


//  ---------------------------------------------------------------------
//  This structure defines one tuple that we track

struct _tuple_t {
    zhash_t *container;         //  Hash table that holds this item
    char *key;                  //  Tuple key
    char *value;                //  Tuple value
};

//  Callback when we remove a tuple from its container

static void
tuple_free (void *argument)
{
    tuple_t *self = (tuple_t *) argument;
    free (self->key);
    free (self->value);
    free (self);
}

//  Handle traffic from remotes
static int
remote_handler (zloop_t *loop, zsock_t *remote, void *argument);

//  ---------------------------------------------------------------------
//  Include the generated server engine

#include "zgossip_engine.h"

//  Allocate properties and structures for a new server instance.
//  Return 0 if OK, or -1 if there was an error.

static int
server_initialize (server_t *self)
{
    //  Default timeout for clients is one second; the caller can
    //  override this with a SET message.
    engine_configure (self, "server/timeout", "1000");
    self->remotes = zlist_new ();
    self->tuples = zhash_new ();
    return 0;
}

//  Free properties and structures for a server instance

static void
server_terminate (server_t *self)
{
    while (zlist_size (self->remotes) > 0) {
        zsock_t *remote = (zsock_t *) zlist_pop (self->remotes);
        zsock_destroy (&remote);
    }
    zlist_destroy (&self->remotes);
    zhash_destroy (&self->tuples);
}

//  Connect to a remote server

static void
server_connect (server_t *self, const char *endpoint)
{
    zsock_t *remote = zsock_new (ZMQ_DEALER);
    assert (remote);          //  No recovery if exhausted
    if (zsock_connect (remote, "%s", endpoint)) {
        zsys_error ("bad zgossip endpoint '%s'", endpoint);
        zsock_destroy (&remote);
        return;
    }
    //  Send HELLO and then PUBLISH for each tuple we have
    zgossip_msg_send_hello (remote);
    tuple_t *tuple = (tuple_t *) zhash_first (self->tuples);
    while (tuple) {
        zgossip_msg_send_publish (remote, tuple->key, tuple->value);
        tuple = (tuple_t *) zhash_next (self->tuples);
    }
    //  Now monitor this remote for incoming messages
    engine_handle_socket (self, remote, remote_handler);
    zlist_append (self->remotes, remote);
}

    
//  Process an incoming tuple on this server.

static void
server_accept (server_t *self, const char *key, const char *value)
{
    tuple_t *tuple = (tuple_t *) zhash_lookup (self->tuples, key);
    if (tuple && streq (tuple->value, value))
        return;                 //  Duplicate tuple, do nothing

    //  Create new tuple
    tuple = (tuple_t *) zmalloc (sizeof (tuple_t));
    assert (tuple);
    tuple->container = self->tuples;
    tuple->key = strdup (key);
    tuple->value = strdup (value);

    //  Store new tuple
    zhash_update (tuple->container, key, tuple);
    zhash_freefn (tuple->container, key, tuple_free);

    //  Hold in server context so we can broadcast to all clients
    self->cur_tuple = tuple;
    engine_broadcast_event (self, NULL, forward_event);

    //  Copy new tuple announcement to all remotes
    zsock_t *remote = (zsock_t *) zlist_first (self->remotes);
    while (remote) {
        zgossip_msg_send_publish (remote, key, value);
        remote = (zsock_t *) zlist_next (self->remotes);
    }
}

//  Process server API method, return reply message if any

static zmsg_t *
server_method (server_t *self, const char *method, zmsg_t *msg)
{
    //  Connect to a remote
    if (streq (method, "CONNECT")) {
        char *endpoint = zmsg_popstr (msg);
        assert (endpoint);
        server_connect (self, endpoint);
        zstr_free (&endpoint);
    }
    else
    if (streq (method, "PUBLISH")) {
        char *key = zmsg_popstr (msg);
        char *value = zmsg_popstr (msg);
        server_accept (self, key, value);
        zstr_free (&key);
        zstr_free (&value);
    }
    else
    if (streq (method, "DUMP")) {
        char *name = zmsg_popstr (msg);
        printf ("%s - %d\n", name, (int) zhash_size (self->tuples));
        zstr_free (&name);
    }
    else
        zsys_error ("unknown zgossip method '%s'", method);
    
    return NULL;
}


//  Allocate properties and structures for a new client connection and
//  optionally engine_set_next_event (). Return 0 if OK, or -1 on error.

static int
client_initialize (client_t *self)
{
    //  Construct properties here
    return 0;
}

//  Free properties and structures for a client connection

static void
client_terminate (client_t *self)
{
    //  Destroy properties here
}


//  --------------------------------------------------------------------------
//  get_first_tuple
//

static void
get_first_tuple (client_t *self)
{
    tuple_t *tuple = (tuple_t *) zhash_first (self->server->tuples);
    if (tuple) {
        zgossip_msg_set_key (self->reply, tuple->key);
        zgossip_msg_set_value (self->reply, tuple->value);
        engine_set_next_event (self, ok_event);
    }
    else
        engine_set_next_event (self, finished_event);
}


//  --------------------------------------------------------------------------
//  get_next_tuple
//

static void
get_next_tuple (client_t *self)
{
    tuple_t *tuple = (tuple_t *) zhash_next (self->server->tuples);
    if (tuple) {
        zgossip_msg_set_key (self->reply, tuple->key);
        zgossip_msg_set_value (self->reply, tuple->value);
        engine_set_next_event (self, ok_event);
    }
    else
        engine_set_next_event (self, finished_event);
}


//  --------------------------------------------------------------------------
//  store_tuple_if_new
//

static void
store_tuple_if_new (client_t *self)
{
    server_accept (self->server,
                   zgossip_msg_key (self->request),
                   zgossip_msg_value (self->request));
}


//  --------------------------------------------------------------------------
//  get_tuple_to_forward
//

static void
get_tuple_to_forward (client_t *self)
{
    //  Hold this in server->cur_tuple so it's available to all
    //  clients; the whole broadcast operation happens in one thread
    //  so there's no risk of confusion here.
    tuple_t *tuple = self->server->cur_tuple;
    zgossip_msg_set_key (self->reply, tuple->key);
    zgossip_msg_set_value (self->reply, tuple->value);
}


//  --------------------------------------------------------------------------
//  Handle messages coming from remotes

static int
remote_handler (zloop_t *loop, zsock_t *remote, void *argument)
{
    zgossip_msg_t *msg = zgossip_msg_recv (remote);
    if (!msg)
        return -1;              //  Interrupted

    if (zgossip_msg_id (msg) == ZGOSSIP_MSG_PUBLISH)
        server_accept ((server_t *) argument,
                        zgossip_msg_key (msg),
                        zgossip_msg_value (msg));
    else
    if (zgossip_msg_id (msg) == ZGOSSIP_MSG_INVALID)
        //  Connection was reset, so send HELLO again
        zgossip_msg_send_hello (remote);
    else
    if (zgossip_msg_id (msg) == ZGOSSIP_MSG_PONG)
        assert (true);   //  Do nothing with PONGs
        
    zgossip_msg_destroy (&msg);
    return 0;
}


//  --------------------------------------------------------------------------
//  Selftest

void
zgossip_test (bool verbose)
{
    printf (" * zgossip: ");
    if (verbose)
        printf ("\n");
    
    //  @selftest
    //  Test basic client-to-server operation of the protocol
    zactor_t *server = zactor_new (zgossip, "server");
    zstr_sendx (server, "SET", "server/animate", verbose? "1": "0", NULL);
    zstr_sendx (server, "BIND", "ipc:///tmp/zgossip", NULL);
    char *port_str = zstr_recv (server);
    assert (streq (port_str, "0"));
    zstr_free (&port_str);

    zsock_t *client = zsock_new (ZMQ_DEALER);
    assert (client);
    zsock_set_rcvtimeo (client, 2000);
    zsock_connect (client, "ipc:///tmp/zgossip");

    //  Send HELLO, which gets no reply
    zgossip_msg_t *request, *reply;
    request = zgossip_msg_new (ZGOSSIP_MSG_HELLO);
    zgossip_msg_send (&request, client);

    //  Send PING, expect PONG back
    request = zgossip_msg_new (ZGOSSIP_MSG_PING);
    zgossip_msg_send (&request, client);
    reply = zgossip_msg_recv (client);
    assert (reply);
    assert (zgossip_msg_id (reply) == ZGOSSIP_MSG_PONG);
    zgossip_msg_destroy (&reply);
    
    zactor_destroy (&server);

    zsock_destroy (&client);
    zactor_destroy (&server);

    //  Test peer-to-peer operations
    zactor_t *base = zactor_new (zgossip, "base");
    assert (base);
    zstr_sendx (base, "SET", "server/animate", verbose? "1": "0", NULL);
    //  Set a 100msec timeout on clients so we can test expiry
    zstr_sendx (base, "SET", "server/timeout", "100", NULL);
    zstr_sendx (base, "BIND", "inproc://base", NULL);
    port_str = zstr_recv (base);
    assert (streq (port_str, "0"));
    zstr_free (&port_str);

    zactor_t *alpha = zactor_new (zgossip, "alpha");
    assert (alpha);
    zstr_sendx (alpha, "CONNECT", "inproc://base", NULL);
    zstr_sendx (alpha, "PUBLISH", "inproc://alpha-1", "service1", NULL);
    zstr_sendx (alpha, "PUBLISH", "inproc://alpha-2", "service2", NULL);

    zactor_t *beta = zactor_new (zgossip, "beta");
    assert (beta);
    zstr_sendx (beta, "CONNECT", "inproc://base", NULL);
    zstr_sendx (beta, "PUBLISH", "inproc://beta-1", "service1", NULL);
    zstr_sendx (beta, "PUBLISH", "inproc://beta-2", "service2", NULL);

    //  got nothing
    zclock_sleep (200);
    
    zactor_destroy (&base);
    zactor_destroy (&alpha);
    zactor_destroy (&beta);
    
    //  @end
    printf ("OK\n");
}
