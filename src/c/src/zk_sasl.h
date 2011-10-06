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

/**
 * \brief initialize sasl library
 *
 * \param callbacks sasl callbacks
 * \return ZSYSTEMERROR if initialization failed
 */
ZOOAPI int zoo_sasl_init(sasl_callback_t *callbacks);

/**
 * \brief creates a sasl connection for the zookeeper socket
 *
 * \param zh the zookeeper handle obtained by a call to \ref zookeeper_init
 * \param servicename name of the zookeeper service
 * \param host host of the zookeeper service
 * \param sasl_conn out parameter for the created sasl connection
 * \param mech out parameter for the sasl mechanisms supported by the client
 * \param mechlen out parameter for the count of supported mechs
 * \return ZSYSTEMERRROR if connection failed
 */
ZOOAPI int zoo_sasl_connect(zhandle_t *zh, char *servicename,
        char *host, zoo_sasl_conn_t **sasl_conn, const char **mechs, int *mechlen);

/**
 * \brief authenticates synchronously
 *
 * \param zh the zookeeper handle obtained by a call to \ref zookeeper_init
 * \param zh the connection handle obtained by a call to \ref zoo_sasl_connect
 * \param mech the selected mechanism
 * \param supportedmechs mechanisms supported by client (obtained by a call
 * to \ref zoo_sasl_connect)
 * \return
 */
ZOOAPI int zoo_sasl_authenticate(zhandle_t *th, zoo_sasl_conn_t *conn, const char *mech,
        const char *supportedmechs);

/**
 * \brief authenticates ssynchronously
 *
 * \param zh the zookeeper handle obtained by a call to \ref zookeeper_init
 * \param zh the connection handle obtained by a call to \ref zoo_sasl_connect
 * \param mech the selected mechanism
 * \param supportedmechs mechanisms supported by client (obtained by a call
 * to \ref zoo_sasl_connect)
 * \return
 */
ZOOAPI int zoo_asasl_authenticate(zhandle_t *th, zoo_sasl_conn_t *conn, const char *mech,
        const char *supportedmechs);

#endif /* ZOOKEEPER_SASL_H_ */
