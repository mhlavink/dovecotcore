/* Copyright (c) 2017-2018 Dovecot authors, see the included COPYING file */

#include "stats-common.h"
#include "restrict-access.h"
#include "ioloop.h"
#include "settings.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "master-service-ssl-settings.h"
#include "stats-settings.h"
#include "stats-event-category.h"
#include "stats-metrics.h"
#include "stats-service.h"
#include "client-writer.h"
#include "client-reader.h"
#include "client-http.h"

const struct master_service_ssl_settings *master_ssl_set;
struct stats_metrics *stats_metrics;
time_t stats_startup_time;

static const struct stats_settings *stats_settings;

static void client_connected(struct master_service_connection *conn)
{
	const char *type = master_service_connection_get_type(conn);

	if (strcmp(type, "http") == 0)
		client_http_create(conn);
	else if (strcmp(type, "writer") == 0)
		client_writer_create(conn->fd);
	else
		client_reader_create(conn->fd);
	master_service_client_connection_accept(conn);
}

static void stats_die(void)
{
	/* just wait for existing stats clients to disconnect from us */
}

static void main_preinit(void)
{
	restrict_access_by_env(RESTRICT_ACCESS_FLAG_ALLOW_ROOT, NULL);
	restrict_access_allow_coredumps(TRUE);
}

static void main_init(void)
{
	const char *error;

	stats_settings =
		settings_get_or_fatal(master_service_get_event(master_service),
				      &stats_setting_parser_info);
	master_ssl_set =
		settings_get_or_fatal(master_service_get_event(master_service),
				      &master_service_ssl_setting_parser_info);

	stats_startup_time = ioloop_time;
	if (stats_metrics_init(master_service_get_event(master_service),
			       stats_settings, &stats_metrics, &error) < 0)
		i_fatal("%s", error);
	stats_event_categories_init();
	client_readers_init();
	client_writers_init();
	client_http_init(stats_settings);
	stats_services_init();
}

static void main_deinit(void)
{
	stats_services_deinit();
	client_readers_deinit();
	client_writers_deinit();
	client_http_deinit();
	stats_event_categories_deinit();
	stats_metrics_deinit(&stats_metrics);
	settings_free(stats_settings);
	settings_free(master_ssl_set);
}

int main(int argc, char *argv[])
{
	const enum master_service_flags service_flags =
		MASTER_SERVICE_FLAG_NO_SSL_INIT |
		MASTER_SERVICE_FLAG_DONT_SEND_STATS |
		MASTER_SERVICE_FLAG_NO_IDLE_DIE |
		MASTER_SERVICE_FLAG_UPDATE_PROCTITLE;
	const char *error;

	master_service = master_service_init("stats", service_flags,
					     &argc, &argv, "");
	if (master_getopt(master_service) > 0)
		return FATAL_DEFAULT;

	if (master_service_settings_read_simple(master_service, &error) < 0)
		i_fatal("%s", error);
	master_service_init_log(master_service);
	master_service_set_die_callback(master_service, stats_die);

	main_preinit();

	main_init();
	master_service_init_finish(master_service);
	master_service_run(master_service, client_connected);
	main_deinit();
	master_service_deinit(&master_service);
        return 0;
}
