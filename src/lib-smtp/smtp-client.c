/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "net.h"
#include "str.h"
#include "hash.h"
#include "array.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "connection.h"
#include "settings.h"
#include "dns-lookup.h"
#include "iostream-rawlog.h"
#include "iostream-ssl.h"

#include "smtp-client-private.h"

struct event_category event_category_smtp_client = {
	.name = "smtp-client"
};

/*
 * Client
 */

struct smtp_client *smtp_client_init(const struct smtp_client_settings *set)
{
	struct smtp_client *client;
	pool_t pool;

	pool = pool_alloconly_create("smtp client", 1024);
	client = p_new(pool, struct smtp_client, 1);
	client->pool = pool;

	client->set.my_ip = set->my_ip;
	client->set.my_hostname = p_strdup(pool, set->my_hostname);

	client->set.forced_capabilities = set->forced_capabilities;
	if (set->extra_capabilities != NULL) {
		client->set.extra_capabilities =
			p_strarray_dup(pool, set->extra_capabilities);
	}

	client->set.dns_client = set->dns_client;
	client->set.rawlog_dir = p_strdup_empty(pool, set->rawlog_dir);

	if (set->ssl != NULL) {
		client->set.ssl = set->ssl;
		pool_ref(client->set.ssl->pool);
	}
	client->set.ssl_allow_invalid_cert = set->ssl_allow_invalid_cert;

	client->set.master_user = p_strdup_empty(pool, set->master_user);
	client->set.username = p_strdup_empty(pool, set->username);
	client->set.sasl_mech = set->sasl_mech;
	if (set->sasl_mech == NULL) {
		client->set.sasl_mechanisms =
			p_strdup(pool, set->sasl_mechanisms);
	}

	client->set.connect_timeout_msecs = set->connect_timeout_msecs != 0 ?
		set->connect_timeout_msecs :
		SMTP_DEFAULT_CONNECT_TIMEOUT_MSECS;
	client->set.command_timeout_msecs = set->command_timeout_msecs != 0 ?
		set->command_timeout_msecs :
		SMTP_DEFAULT_COMMAND_TIMEOUT_MSECS;
	client->set.max_reply_size = set->max_reply_size != 0 ?
		set->max_reply_size : SMTP_DEFAULT_MAX_REPLY_SIZE;
	client->set.max_data_chunk_size = set->max_data_chunk_size != 0 ?
		set->max_data_chunk_size : SMTP_DEFAULT_MAX_DATA_CHUNK_SIZE;
	client->set.max_data_chunk_pipeline = set->max_data_chunk_pipeline != 0 ?
		set->max_data_chunk_pipeline : SMTP_DEFAULT_MAX_DATA_CHUNK_PIPELINE;

	client->set.socket_send_buffer_size = set->socket_send_buffer_size;
	client->set.socket_recv_buffer_size = set->socket_recv_buffer_size;
	client->set.debug = set->debug;
	client->set.verbose_user_errors = set->verbose_user_errors;

	smtp_proxy_data_merge(pool, &client->set.proxy_data, &set->proxy_data);

	client->conn_list = smtp_client_connection_list_init();

	/* There is no event log prefix added here, since the client itself does
	   not log anything and the prefix is protocol-dependent. */
	client->event = event_create(set->event_parent);
	event_add_category(client->event, &event_category_smtp_client);
	event_set_forced_debug(client->event, set->debug);

	return client;
}

void smtp_client_deinit(struct smtp_client **_client)
{
	struct smtp_client *client = *_client;

	connection_list_deinit(&client->conn_list);

	settings_free(client->set.ssl);
	if (client->ssl_ctx != NULL)
		ssl_iostream_context_unref(&client->ssl_ctx);
	event_unref(&client->event);
	pool_unref(&client->pool);
	*_client = NULL;
}

void smtp_client_switch_ioloop(struct smtp_client *client)
{
	struct connection *_conn = client->conn_list->connections;

	/* move connections */
	for (; _conn != NULL; _conn = _conn->next) {
		struct smtp_client_connection *conn =
			(struct smtp_client_connection *)_conn;

		smtp_client_connection_switch_ioloop(conn);
	}
}

int smtp_client_init_ssl_ctx(struct smtp_client *client, const char **error_r)
{
	const struct ssl_settings *ssl_set;
	const struct ssl_iostream_settings *set = NULL;

	if (client->ssl_ctx != NULL)
		return 0;

	if (client->set.ssl != NULL) {
		return ssl_iostream_client_context_cache_get(client->set.ssl,
			&client->ssl_ctx, error_r);
	}
	/* no ssl settings given via smtp_client_settings -
	   look them up automatically */
	if (ssl_client_settings_get(client->event, &ssl_set, error_r) < 0)
		return -1;
	ssl_client_settings_to_iostream_set(ssl_set, &set);

	int ret = ssl_iostream_client_context_cache_get(set, &client->ssl_ctx,
							error_r);
	settings_free(set);
	settings_free(ssl_set);
	return ret;
}

// FIXME: Implement smtp_client_run()
