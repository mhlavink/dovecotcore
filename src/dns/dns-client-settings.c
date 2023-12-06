/* Copyright (c) 2010-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "settings-parser.h"
#include "service-settings.h"

struct service_settings dns_client_service_settings = {
	.name = "dns-client",
	.protocol = "",
	.type = "",
	.executable = "dns-client",
	.user = "$default_internal_user",
	.group = "",
	.privileged_group = "",
	.extra_groups = "",
	.chroot = "",

	.drop_priv_before_exec = FALSE,

	.process_min_avail = 0,
	.process_limit = 0,
	.client_limit = 1,
	.service_count = 0,
	.idle_kill = 0,
	.vsz_limit = UOFF_T_MAX,

	.unix_listeners = ARRAY_INIT,
	.fifo_listeners = ARRAY_INIT,
	.inet_listeners = ARRAY_INIT
};

const struct setting_keyvalue dns_client_service_settings_defaults[] = {
	{ "unix_listener", "dns-client login\\sdns-client" },

	{ "unix_listener/dns-client/path", "dns-client" },
	{ "unix_listener/dns-client/mode", "0666" },

	{ "unix_listener/login\\sdns-client/path", "login/dns-client" },
	{ "unix_listener/login\\sdns-client/mode", "0666" },

	{ NULL, NULL }
};
