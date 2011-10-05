/*
 * zookeeper_sasl.h
 *
 *  Created on: 29.09.2011
 *      Author: tom
 */

#ifndef ZOOKEEPER_SASL_H_
#define ZOOKEEPER_SASL_H_

#include <sasl/sasl.h>
#include "zk_adaptor.h"

ZOOAPI int zoo_sasl_init(sasl_callback_t *callbacks);

ZOOAPI int zoo_sasl_connect(zhandle_t *zh, char *servicename,
        char *host, sasl_conn_t **sasl_conn, const char **mechs, int *mechlen);

ZOOAPI int zoo_sasl_negotiate(zhandle_t *th, sasl_conn_t *conn, const char *mech,
        const char *supportedmechs);

#endif /* ZOOKEEPER_SASL_H_ */
