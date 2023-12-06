/* Copyright (c) 2005-2018 Dovecot authors, see the included COPYING file */

#include "login-common.h"
#include "hostpid.h"
#include "var-expand.h"
#include "settings-parser.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "master-service-ssl-settings.h"
#include "login-settings.h"

#include <unistd.h>

static bool login_settings_check(void *_set, pool_t pool, const char **error_r);

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct login_settings)

static const struct setting_define login_setting_defines[] = {
	DEF(STR, login_trusted_networks),
	DEF(STR, login_source_ips),
	DEF(STR_VARS, login_greeting),
	DEF(STR, login_log_format_elements),
	DEF(STR, login_log_format),
	DEF(STR_VARS, login_proxy_notify_path),
	DEF(STR, login_plugin_dir),
	DEF(STR, login_plugins),
	DEF(TIME_MSECS, login_proxy_timeout),
	DEF(UINT, login_proxy_max_reconnects),
	DEF(TIME, login_proxy_max_disconnect_delay),
	DEF(STR, login_proxy_rawlog_dir),
	DEF(STR, login_socket_path),

	DEF(BOOL, auth_ssl_require_client_cert),
	DEF(BOOL, auth_ssl_username_from_cert),

	DEF(BOOL, auth_allow_cleartext),
	DEF(BOOL, auth_verbose),
	DEF(BOOL, auth_debug),
	DEF(BOOL, verbose_proctitle),

	DEF(ENUM, ssl),

	DEF(UINT, mail_max_userip_connections),

	SETTING_DEFINE_LIST_END
};

static const struct login_settings login_default_settings = {
	.login_trusted_networks = "",
	.login_source_ips = "",
	.login_greeting = PACKAGE_NAME" ready.",
	.login_log_format_elements = "user=<%u> method=%m rip=%r lip=%l mpid=%e %c session=<%{session}>",
	.login_log_format = "%$: %s",
	.login_proxy_notify_path = "proxy-notify",
	.login_plugin_dir = MODULEDIR"/login",
	.login_plugins = "",
	.login_proxy_timeout = 30*1000,
	.login_proxy_max_reconnects = 3,
	.login_proxy_max_disconnect_delay = 0,
	.login_proxy_rawlog_dir = "",
	.login_socket_path = "",

	.auth_ssl_require_client_cert = FALSE,
	.auth_ssl_username_from_cert = FALSE,

	.auth_allow_cleartext = FALSE,
	.auth_verbose = FALSE,
	.auth_debug = FALSE,
	.verbose_proctitle = FALSE,

	.ssl = "yes:no:required",

	.mail_max_userip_connections = 10
};

const struct setting_parser_info login_setting_parser_info = {
	.name = "login",

	.defines = login_setting_defines,
	.defaults = &login_default_settings,

	.struct_size = sizeof(struct login_settings),
	.pool_offset1 = 1 + offsetof(struct login_settings, pool),
	.check_func = login_settings_check
};

/* <settings checks> */
static bool login_settings_check(void *_set, pool_t pool,
				 const char **error_r)
{
	struct login_settings *set = _set;

	set->log_format_elements_split =
		p_strsplit(pool, set->login_log_format_elements, " ");

	if (strcmp(set->ssl, "required") == 0 && set->auth_allow_cleartext) {
		*error_r = "auth_allow_cleartext=yes has no effect with ssl=required";
		return FALSE;
	}

	return TRUE;
}
/* </settings checks> */
