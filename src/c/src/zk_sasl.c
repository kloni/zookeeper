/*
 * zookeeper_sasl.c
 *
 *  Created on: 30.09.2011
 *      Author: tom
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>

#include "zk_sasl.h"
#include "zookeeper_log.h"

#define SAMPLE_SEC_BUF_SIZE (2048)

int sasl_step(int rc, zhandle_t *zh, sasl_conn_t *conn, const char *serverin,
        int serverinlen);

int sasl_complete(int rc, zhandle_t *zh, sasl_conn_t *conn,
        const char *serverin, int serverinlen);

int zoo_sasl_init(sasl_callback_t *callbacks) {
    /* initialize the sasl library */
    int rc = sasl_client_init(callbacks);
    if (rc != SASL_OK) {
        LOG_ERROR(
                ("initializing libsasl: %s", sasl_errstring(rc, NULL, NULL)));
    }
    return rc;
}

int zoo_sasl_connect(zhandle_t *zh, char *servicename, char *host, sasl_conn_t **sasl_conn,
        const char **mechs, int *mechlen) {
    char localaddr[NI_MAXHOST + NI_MAXSERV], remoteaddr[NI_MAXHOST + NI_MAXSERV];
    char hbuf[NI_MAXHOST], pbuf[NI_MAXSERV];
    int r;
    int salen;
    int niflags, error;
    struct sockaddr_storage local_ip, remote_ip;
    sasl_conn_t *conn;
    //sasl_security_properties_t secprops;
    //sasl_ssf_t extssf = 128;

    /* set ip addresses */
    salen = sizeof(local_ip);
    if (getsockname(zh->fd, (struct sockaddr *) &local_ip,
                    (unsigned *) &salen) < 0) {
        LOG_ERROR(("getsockname"));
    }

    niflags = (NI_NUMERICHOST | NI_NUMERICSERV);
#ifdef NI_WITHSCOPEID
    if (((struct sockaddr *)&local_ip)->sa_family ==AF_INET6)
    niflags |= NI_WITHSCOPEID;
#endif
    error = getnameinfo((struct sockaddr *) &local_ip, salen, hbuf,
            sizeof(hbuf), pbuf, sizeof(pbuf), niflags);
    if (error != 0) {
        LOG_ERROR(("getnameinfo: %s\n", gai_strerror(error)));
        strcpy(hbuf, "unknown");
        strcpy(pbuf, "unknown");
    }
    snprintf(localaddr, sizeof(localaddr), "%s;%s", hbuf, pbuf);

    salen = sizeof(remote_ip);
    if (getpeername(zh->fd, (struct sockaddr *) &remote_ip,
                    (unsigned *) &salen) < 0) {
        LOG_ERROR(("getpeername"));
    }

    niflags = (NI_NUMERICHOST | NI_NUMERICSERV);
#ifdef NI_WITHSCOPEID
    if (((struct sockaddr *)&remote_ip)->sa_family == AF_INET6)
    niflags |= NI_WITHSCOPEID;
#endif
    error = getnameinfo((struct sockaddr *) &remote_ip, salen, hbuf,
            sizeof(hbuf), pbuf, sizeof(pbuf), niflags);
    if (error != 0) {
        LOG_ERROR(("getnameinfo: %s\n", gai_strerror(error)));
        strcpy(hbuf, "unknown");
        strcpy(pbuf, "unknown");
    }
    snprintf(remoteaddr, sizeof(remoteaddr), "%s;%s", hbuf, pbuf);

    LOG_DEBUG(
            ("Zookeeper Host: %s %s %s", "ubook", localaddr, remoteaddr));

    /*
     memset(&secprops, 0L, sizeof(secprops));
     secprops.maxbufsize = SAMPLE_SEC_BUF_SIZE;
     secprops.max_ssf = 2048;
     secprops.min_ssf = 128;
     */

    /* client new connection */
    r = sasl_client_new(servicename, host ? host : hbuf, localaddr, remoteaddr,
            NULL, 0, &conn);
    if (r != SASL_OK) {
        LOG_ERROR(
                ("allocating connection state: %s %s", sasl_errstring(r, NULL, NULL), sasl_errdetail(conn)));
    }

    //sasl_setprop(conn, SASL_SSF_EXTERNAL, &extssf);

    //sasl_setprop(conn, SASL_SEC_PROPS, &secprops);

    sasl_listmech(conn, NULL, NULL, " ", NULL, mechs, NULL, mechlen);

    *sasl_conn = conn;

    return r;
}

static int sasl_proceed(int sr, zhandle_t *zh, sasl_conn_t *conn,
        const char *clientout, int clientoutlen) {
    int r = ZOK;

    if (sr != SASL_OK && sr != SASL_CONTINUE) {
        LOG_ERROR(
                ("starting SASL negotiation: %s %s", sasl_errstring(sr, NULL, NULL), sasl_errdetail(conn)));
        return -1;
    }

    if (sr == SASL_CONTINUE || clientoutlen > 0) {
        r = queue_sasl_request(zh, conn, clientout, clientoutlen,
                (sr == SASL_CONTINUE) ? sasl_step : sasl_complete);
    }
    if (r != ZOK) {
        LOG_ERROR(("Sending sasl request failed: %d", r));
        return r;
    }
    return r;
}

int zoo_sasl_negotiate(zhandle_t *zh, sasl_conn_t *conn, const char *mech,
        const char *supportedmechs) {
    const char *clientout;
    const char *chosenmech;
    unsigned clientoutlen;
    int sr = 0;

    /*
     if (supportedmechs) {
     serverin = (char *) malloc(strlen(supportedmechs));
     strncpy(serverin, supportedmechs, strlen(supportedmechs));
     }
     */

    if (mech) {
        if (!strstr(supportedmechs, mech)) {
            LOG_DEBUG(("server doesn't offer mandatory mech '%s'\n", mech));
            return -1;
        }
    }

    sr = sasl_client_start(conn, mech, NULL, &clientout, &clientoutlen,
            &chosenmech);

    LOG_DEBUG(("SASL Authentication mechanism: %s", chosenmech));

    return sasl_proceed(sr, zh, conn, clientout, clientoutlen);
}

int sasl_step(int rc, zhandle_t *zh, sasl_conn_t *conn, const char *serverin,
        int serverinlen) {
    const char *clientout;
    unsigned clientoutlen;
    int sr;
    int r = rc;

    if (r != ZOK) {
        LOG_ERROR(("Reading sasl response failed: %d", r));
        return r;
    }

    sr = sasl_client_step(conn, serverin, serverinlen, NULL, &clientout,
            &clientoutlen);

    return sasl_proceed(sr, zh, conn, clientout, clientoutlen);
}

int sasl_complete(int rc, zhandle_t *zh, sasl_conn_t *conn,
        const char *serverin, int serverinlen) {
    if (rc != ZOK) {
        LOG_ERROR(("Reading sasl response failed: %d", rc));
        return rc;
    }

    LOG_DEBUG(("SASL Authentication complete [%d]", rc));
    return rc;
}
