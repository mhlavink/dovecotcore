/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "settings-parser.h"
#include "master-service-private.h"
#include "master-service-ssl-settings.h"
#include "iostream-ssl.h"

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct master_service_ssl_settings)

static bool
master_service_ssl_settings_check(void *_set, pool_t pool, const char **error_r);

static const struct setting_define master_service_ssl_setting_defines[] = {
	DEF(ENUM, ssl),
	DEF(STR, ssl_ca),
	DEF(STR, ssl_client_ca_file),
	DEF(STR, ssl_client_ca_dir),
	DEF(STR, ssl_client_cert),
	DEF(STR, ssl_client_key),
	DEF(STR, ssl_cipher_list),
	DEF(STR, ssl_cipher_suites),
	DEF(STR, ssl_curve_list),
	DEF(STR, ssl_min_protocol),
	DEF(STR, ssl_cert_username_field),
	DEF(STR, ssl_crypto_device),
	DEF(BOOL, ssl_verify_client_cert),
	DEF(BOOL, ssl_client_require_valid_cert),
	DEF(BOOL, ssl_require_crl),
	DEF(BOOL, verbose_ssl),
	DEF(BOOL, ssl_prefer_server_ciphers),
	DEF(STR, ssl_options), /* parsed as a string to set bools */

	SETTING_DEFINE_LIST_END
};

static const struct master_service_ssl_settings master_service_ssl_default_settings = {
	.ssl = "yes:no:required",
	.ssl_ca = "",
	.ssl_client_ca_file = "",
	.ssl_client_ca_dir = "",
	.ssl_client_cert = "",
	.ssl_client_key = "",
	.ssl_cipher_list = "ALL:!kRSA:!SRP:!kDHd:!DSS:!aNULL:!eNULL:!EXPORT:!DES:!3DES:!MD5:!PSK:!RC4:!ADH:!LOW@STRENGTH",
	.ssl_cipher_suites = "", /* Use TLS library provided value */
	.ssl_curve_list = "",
	.ssl_min_protocol = "TLSv1.2",
	.ssl_cert_username_field = "commonName",
	.ssl_crypto_device = "",
	.ssl_verify_client_cert = FALSE,
	.ssl_client_require_valid_cert = TRUE,
	.ssl_require_crl = TRUE,
	.verbose_ssl = FALSE,
	.ssl_prefer_server_ciphers = FALSE,
	.ssl_options = "",
};

const struct setting_parser_info master_service_ssl_setting_parser_info = {
	.name = "master_service_ssl",
	.defines = master_service_ssl_setting_defines,
	.defaults = &master_service_ssl_default_settings,

	.pool_offset1 = 1 + offsetof(struct master_service_ssl_settings, pool),
	.struct_size = sizeof(struct master_service_ssl_settings),
	.check_func = master_service_ssl_settings_check
};

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct master_service_ssl_server_settings)

static const struct setting_define master_service_ssl_server_setting_defines[] = {
	DEF(STR, ssl_cert),
	DEF(STR, ssl_key),
	DEF(STR, ssl_alt_cert),
	DEF(STR, ssl_alt_key),
	DEF(STR, ssl_key_password),
	DEF(STR, ssl_dh),

	SETTING_DEFINE_LIST_END
};

static const struct master_service_ssl_server_settings master_service_ssl_server_default_settings = {
	.ssl_cert = "",
	.ssl_key = "",
	.ssl_alt_cert = "",
	.ssl_alt_key = "",
	.ssl_key_password = "",
	.ssl_dh = "",
};

const struct setting_parser_info master_service_ssl_server_setting_parser_info = {
	.name = "master_service_ssl_server",

	.defines = master_service_ssl_server_setting_defines,
	.defaults = &master_service_ssl_server_default_settings,

	.pool_offset1 = 1 + offsetof(struct master_service_ssl_server_settings, pool),
	.struct_size = sizeof(struct master_service_ssl_server_settings),
};

/* <settings checks> */
static bool
master_service_ssl_settings_check(void *_set, pool_t pool ATTR_UNUSED,
				  const char **error_r)
{
	struct master_service_ssl_settings *set = _set;

	if (strcmp(set->ssl, "no") == 0) {
		/* disabled */
		return TRUE;
	}
	/* we get called from many different tools, possibly with -O parameter,
	   and few of those tools care about SSL settings. so don't check
	   ssl_cert/ssl_key/etc validity here except in doveconf, because it
	   usually is just an extra annoyance. */
	if (is_config_binary()) T_BEGIN {
		const char *proto = t_str_ucase(set->ssl_min_protocol);
		if (strstr(proto, "ANY") != NULL)
			i_warning("ssl_min_protocol=ANY is used - This is "
				  "insecure and intended only for testing");
	} T_END;

	if (set->ssl_verify_client_cert && *set->ssl_ca == '\0') {
		*error_r = "ssl_verify_client_cert set, but ssl_ca not";
		return FALSE;
	}

	/* Now explode the ssl_options string into individual flags */
	/* First set them all to defaults */
	set->parsed_opts.compression = FALSE;
	set->parsed_opts.tickets = TRUE;

	/* Then modify anything specified in the string */
	const char **opts = t_strsplit_spaces(set->ssl_options, ", ");
	const char *opt;
	while ((opt = *opts++) != NULL) {
		if (strcasecmp(opt, "compression") == 0) {
			set->parsed_opts.compression = TRUE;
		} else if (strcasecmp(opt, "no_ticket") == 0) {
			set->parsed_opts.tickets = FALSE;
		} else {
			*error_r = t_strdup_printf("ssl_options: unknown flag: '%s'",
						   opt);
			return FALSE;
		}
	}

	return TRUE;
}
/* </settings checks> */

static void master_service_ssl_common_settings_to_iostream_set(
	const struct master_service_ssl_settings *ssl_set, pool_t pool,
	struct ssl_iostream_settings *set_r)
{
	i_zero(set_r);
	set_r->min_protocol = p_strdup(pool, ssl_set->ssl_min_protocol);
	set_r->cipher_list = p_strdup(pool, ssl_set->ssl_cipher_list);
	/* leave NULL if empty - let library decide */
	set_r->ciphersuites = p_strdup_empty(pool, ssl_set->ssl_cipher_suites);
	/* NOTE: It's a bit questionable whether ssl_ca should be used for
	   clients. But at least for now it's needed for login-proxy. */
	set_r->ca = p_strdup_empty(pool, ssl_set->ssl_ca);

	set_r->crypto_device = p_strdup(pool, ssl_set->ssl_crypto_device);
	set_r->cert_username_field = p_strdup(pool, ssl_set->ssl_cert_username_field);

	set_r->verbose = ssl_set->verbose_ssl;
	set_r->verbose_invalid_cert = ssl_set->verbose_ssl;
	set_r->prefer_server_ciphers = ssl_set->ssl_prefer_server_ciphers;
	set_r->compression = ssl_set->parsed_opts.compression;
	set_r->tickets = ssl_set->parsed_opts.tickets;
	set_r->curve_list = p_strdup(pool, ssl_set->ssl_curve_list);
}

void master_service_ssl_client_settings_to_iostream_set(
	const struct master_service_ssl_settings *ssl_set, pool_t pool,
	struct ssl_iostream_settings *set_r)
{
	master_service_ssl_common_settings_to_iostream_set(ssl_set, pool, set_r);

	set_r->ca_file = p_strdup_empty(pool, ssl_set->ssl_client_ca_file);
	set_r->ca_dir = p_strdup_empty(pool, ssl_set->ssl_client_ca_dir);
	set_r->cert.cert = p_strdup_empty(pool, ssl_set->ssl_client_cert);
	set_r->cert.key = p_strdup_empty(pool, ssl_set->ssl_client_key);
	set_r->verify_remote_cert = ssl_set->ssl_client_require_valid_cert;
	set_r->allow_invalid_cert = !set_r->verify_remote_cert;
	/* client-side CRL checking not supported currently */
	set_r->skip_crl_check = TRUE;
}

void master_service_ssl_server_settings_to_iostream_set(
	const struct master_service_ssl_settings *ssl_set,
	const struct master_service_ssl_server_settings *ssl_server_set,
	pool_t pool, struct ssl_iostream_settings *set_r)
{
	master_service_ssl_common_settings_to_iostream_set(ssl_set, pool, set_r);

	set_r->cert.cert = p_strdup(pool, ssl_server_set->ssl_cert);
	set_r->cert.key = p_strdup(pool, ssl_server_set->ssl_key);
	set_r->cert.key_password = p_strdup(pool, ssl_server_set->ssl_key_password);
	if (ssl_server_set->ssl_alt_cert != NULL &&
	    *ssl_server_set->ssl_alt_cert != '\0') {
		set_r->alt_cert.cert = p_strdup(pool, ssl_server_set->ssl_alt_cert);
		set_r->alt_cert.key = p_strdup(pool, ssl_server_set->ssl_alt_key);
		set_r->alt_cert.key_password = p_strdup(pool, ssl_server_set->ssl_key_password);
	}
	set_r->dh = p_strdup(pool, ssl_server_set->ssl_dh);
	set_r->verify_remote_cert = ssl_set->ssl_verify_client_cert;
	set_r->allow_invalid_cert = !set_r->verify_remote_cert;
	/* ssl_require_crl is used only for checking client-provided SSL
	   certificate's CRL. */
	set_r->skip_crl_check = !ssl_set->ssl_require_crl;
}
