/*
    Copyright (c) 2007-2017 Contributors as noted in the AUTHORS file

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "testutil.hpp"
#if defined(ZMQ_HAVE_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdexcept>
#define close closesocket
typedef SOCKET raw_socket;
#else
#include <arpa/inet.h>
typedef int raw_socket;
#endif

#include <unity.h>

void setUp ()
{
    //    setup_test_context ();
}

void tearDown ()
{
    //    teardown_test_context ();
}


//  Read one event off the monitor socket; return value and address
//  by reference, if not null, and event number by value. Returns -1
//  in case of error.

static int get_monitor_event (void *monitor)
{
    for (int i = 0; i < 2; i++) {
        //  First frame in message contains event number and value
        zmq_msg_t msg;
        int rc = zmq_msg_init (&msg);
        assert (rc == 0);
        if (zmq_msg_recv (&msg, monitor, ZMQ_DONTWAIT) == -1) {
            msleep (SETTLE_TIME);
            continue; //  Interruped, presumably
        }
        assert (zmq_msg_more (&msg));

        uint8_t *data = (uint8_t *) zmq_msg_data (&msg);
        uint16_t event = *(uint16_t *) (data);

        //  Second frame in message contains event address
        rc = zmq_msg_init (&msg);
        assert (rc == 0);
        if (zmq_msg_recv (&msg, monitor, 0) == -1) {
            return -1; //  Interruped, presumably
        }
        assert (!zmq_msg_more (&msg));

        return event;
    }
    return -1;
}

static void recv_with_retry (raw_socket fd, char *buffer, int bytes)
{
    int received = 0;
    while (true) {
        int rc = recv (fd, buffer + received, bytes - received, 0);
        assert (rc > 0);
        received += rc;
        assert (received <= bytes);
        if (received == bytes)
            break;
        // ZMQ_REP READY message is shorter, check the actual socket type
        if (received >= 3 && buffer[received - 1] == 'P'
            && buffer[received - 2] == 'E' && buffer[received - 3] == 'R')
            break;
    }
}

static void mock_handshake (raw_socket fd)
{
    const uint8_t zmtp_greeting[33] = {0xff, 0, 0, 0,   0,   0,   0,   0, 0,
                                       0x7f, 3, 0, 'N', 'U', 'L', 'L', 0};
    char buffer[128];
    memset (buffer, 0, sizeof (buffer));
    memcpy (buffer, zmtp_greeting, sizeof (zmtp_greeting));

    int rc = send (fd, buffer, 64, 0);
    assert (rc == 64);

    recv_with_retry (fd, buffer, 64);

    const uint8_t zmtp_ready[43] = {
      4,   41,  5,   'R', 'E', 'A', 'D', 'Y', 11,  'S', 'o', 'c', 'k', 'e', 't',
      '-', 'T', 'y', 'p', 'e', 0,   0,   0,   6,   'D', 'E', 'A', 'L', 'E', 'R',
      8,   'I', 'd', 'e', 'n', 't', 'i', 't', 'y', 0,   0,   0,   0};

    memset (buffer, 0, sizeof (buffer));
    memcpy (buffer, zmtp_ready, 43);
    rc = send (fd, buffer, 43, 0);
    assert (rc == 43);

    recv_with_retry (fd, buffer, 43);
}

static void setup_curve (void *socket, int is_server)
{
    const char *secret_key;
    const char *public_key;
    const char *server_key;

    if (is_server) {
        secret_key = "JTKVSB%%)wK0E.X)V>+}o?pNmC{O&4W4b!Ni{Lh6";
        public_key = "rq:rM>}U?@Lns47E1%kR.o@n%FcmmsL/@{H8]yf7";
        server_key = NULL;
    } else {
        secret_key = "D:)Q[IlAW!ahhC2ac:9*A}h:p?([4%wOTJ%JR%cs";
        public_key = "Yne@$w-vo<fVvi]a<NY6T1ed:M$fCG*[IaLV{hID";
        server_key = "rq:rM>}U?@Lns47E1%kR.o@n%FcmmsL/@{H8]yf7";
    }

    zmq_setsockopt (socket, ZMQ_CURVE_SECRETKEY, secret_key,
                    strlen (secret_key));
    zmq_setsockopt (socket, ZMQ_CURVE_PUBLICKEY, public_key,
                    strlen (public_key));
    if (is_server)
        zmq_setsockopt (socket, ZMQ_CURVE_SERVER, &is_server,
                        sizeof (is_server));
    else
        zmq_setsockopt (socket, ZMQ_CURVE_SERVERKEY, server_key,
                        strlen (server_key));
}

static void prep_server_socket (void *ctx,
                                int set_heartbeats,
                                int is_curve,
                                void **server_out,
                                void **mon_out,
                                char *endpoint,
                                size_t ep_length,
                                int socket_type)
{
    int rc;
    //  We'll be using this socket in raw mode
    void *server = zmq_socket (ctx, socket_type);
    assert (server);

    int value = 0;
    rc = zmq_setsockopt (server, ZMQ_LINGER, &value, sizeof (value));
    assert (rc == 0);

    if (set_heartbeats) {
        value = 50;
        rc = zmq_setsockopt (server, ZMQ_HEARTBEAT_IVL, &value, sizeof (value));
        assert (rc == 0);
    }

    if (is_curve)
        setup_curve (server, 1);

    rc = zmq_bind (server, "tcp://127.0.0.1:*");
    assert (rc == 0);
    rc = zmq_getsockopt (server, ZMQ_LAST_ENDPOINT, endpoint, &ep_length);
    assert (rc == 0);

    //  Create and connect a socket for collecting monitor events on dealer
    void *server_mon = zmq_socket (ctx, ZMQ_PAIR);
    assert (server_mon);

    rc = zmq_socket_monitor (server, "inproc://monitor-dealer",
                             ZMQ_EVENT_CONNECTED | ZMQ_EVENT_DISCONNECTED
                               | ZMQ_EVENT_ACCEPTED);
    assert (rc == 0);

    //  Connect to the inproc endpoint so we'll get events
    rc = zmq_connect (server_mon, "inproc://monitor-dealer");
    assert (rc == 0);

    *server_out = server;
    *mon_out = server_mon;
}

// This checks for a broken TCP connection (or, in this case a stuck one
// where the peer never responds to PINGS). There should be an accepted event
// then a disconnect event.
static void test_heartbeat_timeout (int server_type)
{
    int rc;
    char my_endpoint[MAX_SOCKET_STRING];

    //  Set up our context and sockets
    void *ctx = zmq_ctx_new ();
    assert (ctx);

    void *server, *server_mon;
    prep_server_socket (ctx, 1, 0, &server, &server_mon, my_endpoint,
                        MAX_SOCKET_STRING, server_type);

    struct sockaddr_in ip4addr;
    raw_socket s;

    ip4addr.sin_family = AF_INET;
    ip4addr.sin_port = htons (atoi (strrchr (my_endpoint, ':') + 1));
#if defined(ZMQ_HAVE_WINDOWS) && (_WIN32_WINNT < 0x0600)
    ip4addr.sin_addr.s_addr = inet_addr ("127.0.0.1");
#else
    inet_pton (AF_INET, "127.0.0.1", &ip4addr.sin_addr);
#endif

    s = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    rc = connect (s, (struct sockaddr *) &ip4addr, sizeof ip4addr);
    assert (rc > -1);

    // Mock a ZMTP 3 client so we can forcibly time out a connection
    mock_handshake (s);

    // By now everything should report as connected
    rc = get_monitor_event (server_mon);
    assert (rc == ZMQ_EVENT_ACCEPTED);

    // We should have been disconnected
    rc = get_monitor_event (server_mon);
    assert (rc == ZMQ_EVENT_DISCONNECTED);

    close (s);

    rc = zmq_close (server);
    assert (rc == 0);

    rc = zmq_close (server_mon);
    assert (rc == 0);

    rc = zmq_ctx_term (ctx);
    assert (rc == 0);
}

// This checks that peers respect the TTL value in ping messages
// We set up a mock ZMTP 3 client and send a ping message with a TLL
// to a server that is not doing any heartbeating. Then we sleep,
// if the server disconnects the client, then we know the TTL did
// its thing correctly.
static void test_heartbeat_ttl (int client_type, int server_type)
{
    int rc, value;
    char my_endpoint[MAX_SOCKET_STRING];

    //  Set up our context and sockets
    void *ctx = zmq_ctx_new ();
    assert (ctx);

    void *server, *server_mon, *client;
    prep_server_socket (ctx, 0, 0, &server, &server_mon, my_endpoint,
                        MAX_SOCKET_STRING, server_type);

    client = zmq_socket (ctx, client_type);
    assert (client != NULL);

    // Set the heartbeat TTL to 0.1 seconds
    value = 100;
    rc = zmq_setsockopt (client, ZMQ_HEARTBEAT_TTL, &value, sizeof (value));
    assert (rc == 0);

    // Set the heartbeat interval to much longer than the TTL so that
    // the socket times out oon the remote side.
    value = 250;
    rc = zmq_setsockopt (client, ZMQ_HEARTBEAT_IVL, &value, sizeof (value));
    assert (rc == 0);

    rc = zmq_connect (client, my_endpoint);
    assert (rc == 0);

    // By now everything should report as connected
    rc = get_monitor_event (server_mon);
    assert (rc == ZMQ_EVENT_ACCEPTED);

    msleep (SETTLE_TIME);

    // We should have been disconnected
    rc = get_monitor_event (server_mon);
    assert (rc == ZMQ_EVENT_DISCONNECTED);

    rc = zmq_close (server);
    assert (rc == 0);

    rc = zmq_close (server_mon);
    assert (rc == 0);

    rc = zmq_close (client);
    assert (rc == 0);

    rc = zmq_ctx_term (ctx);
    assert (rc == 0);
}

// This checks for normal operation - that is pings and pongs being
// exchanged normally. There should be an accepted event on the server,
// and then no event afterwards.
static void
test_heartbeat_notimeout (int is_curve, int client_type, int server_type)
{
    int rc;
    char my_endpoint[MAX_SOCKET_STRING];

    //  Set up our context and sockets
    void *ctx = zmq_ctx_new ();
    assert (ctx);

    void *server, *server_mon;
    prep_server_socket (ctx, 1, is_curve, &server, &server_mon, my_endpoint,
                        MAX_SOCKET_STRING, server_type);

    void *client = zmq_socket (ctx, client_type);
    if (is_curve)
        setup_curve (client, 0);
    rc = zmq_connect (client, my_endpoint);

    // Give it a sec to connect and handshake
    msleep (SETTLE_TIME);

    // By now everything should report as connected
    rc = get_monitor_event (server_mon);
    assert (rc == ZMQ_EVENT_ACCEPTED);

    // We should still be connected because pings and pongs are happenin'
    rc = get_monitor_event (server_mon);
    assert (rc == -1);

    rc = zmq_close (client);
    assert (rc == 0);

    rc = zmq_close (server);
    assert (rc == 0);

    rc = zmq_close (server_mon);
    assert (rc == 0);

    rc = zmq_ctx_term (ctx);
    assert (rc == 0);
}

void test_heartbeat_timeout_router ()
{
    test_heartbeat_timeout (ZMQ_ROUTER);
}

void test_heartbeat_timeout_rep ()
{
    test_heartbeat_timeout (ZMQ_REP);
}

#define DEFINE_TESTS(first, second, first_define, second_define)               \
    void test_heartbeat_ttl_##first##_##second ()                              \
    {                                                                          \
        test_heartbeat_ttl (first_define, second_define);                      \
    }                                                                          \
    void test_heartbeat_notimeout_##first##_##second ()                        \
    {                                                                          \
        test_heartbeat_notimeout (0, first_define, second_define);             \
    }                                                                          \
    void test_heartbeat_notimeout_##first##_##second##_with_curve ()           \
    {                                                                          \
        test_heartbeat_notimeout (1, first_define, second_define);             \
    }

DEFINE_TESTS (dealer, router, ZMQ_DEALER, ZMQ_ROUTER)
DEFINE_TESTS (req, rep, ZMQ_REQ, ZMQ_REP)
DEFINE_TESTS (pull, push, ZMQ_PULL, ZMQ_PUSH)
DEFINE_TESTS (sub, pub, ZMQ_SUB, ZMQ_PUB)
DEFINE_TESTS (pair, pair, ZMQ_PAIR, ZMQ_PAIR)

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();

    RUN_TEST (test_heartbeat_timeout_router);
    RUN_TEST (test_heartbeat_timeout_rep);

    RUN_TEST (test_heartbeat_ttl_dealer_router);
    RUN_TEST (test_heartbeat_ttl_req_rep);
    RUN_TEST (test_heartbeat_ttl_pull_push);
    RUN_TEST (test_heartbeat_ttl_sub_pub);
    RUN_TEST (test_heartbeat_ttl_pair_pair);

    RUN_TEST (test_heartbeat_notimeout_dealer_router);
    RUN_TEST (test_heartbeat_notimeout_req_rep);
    RUN_TEST (test_heartbeat_notimeout_pull_push);
    RUN_TEST (test_heartbeat_notimeout_sub_pub);
    RUN_TEST (test_heartbeat_notimeout_pair_pair);

    RUN_TEST (test_heartbeat_notimeout_dealer_router_with_curve);
    RUN_TEST (test_heartbeat_notimeout_req_rep_with_curve);
    RUN_TEST (test_heartbeat_notimeout_pull_push_with_curve);
    RUN_TEST (test_heartbeat_notimeout_sub_pub_with_curve);
    RUN_TEST (test_heartbeat_notimeout_pair_pair_with_curve);

    return UNITY_END ();
}
