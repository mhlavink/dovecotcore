#ifndef DOVEADM_SETTINGS_H
#define DOVEADM_SETTINGS_H

#include "net.h"

struct ssl_iostream_settings;

/* <settings checks> */
enum dsync_features {
	DSYNC_FEATURE_EMPTY_HDR_WORKAROUND = 0x1,
	DSYNC_FEATURE_NO_HEADER_HASHES = 0x2,
};
/* </settings checks> */

struct doveadm_settings {
	pool_t pool;
	const char *base_dir;
	const char *libexec_dir;
	const char *mail_plugins;
	const char *mail_plugin_dir;
	const char *mail_temp_dir;
	bool auth_debug;
	const char *auth_socket_path;
	const char *doveadm_socket_path;
	unsigned int doveadm_worker_count;
	in_port_t doveadm_port;
	const char *doveadm_ssl;
	const char *doveadm_username;
	const char *doveadm_password;
	const char *doveadm_allowed_commands;
	const char *dsync_alt_char;
	const char *dsync_remote_cmd;
	const char *doveadm_api_key;
	const char *dsync_features;
	const char *dsync_hashed_headers;
	unsigned int dsync_commit_msgs_interval;
	const char *doveadm_http_rawlog_dir;
	enum dsync_features parsed_features;
	ARRAY(const char *) plugin_envs;
};


extern const struct setting_parser_info doveadm_setting_parser_info;
extern const struct doveadm_settings *doveadm_settings;
extern const struct master_service_ssl_settings *doveadm_ssl_set;
extern bool doveadm_verbose_proctitle;

void doveadm_get_ssl_settings(struct ssl_iostream_settings *set_r, pool_t pool);

void doveadm_read_settings(void);
/* Returns the global binary config fd. Note that it may be -1 if doveadm was
   called with -O parameter. */
int doveadm_settings_get_config_fd(void);

void doveadm_settings_deinit(void);

#endif
