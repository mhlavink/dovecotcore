/* Copyright (c) 2006-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "http-client.h"
#include "mail-user.h"
#include "mail-storage-hooks.h"
#include "solr-connection.h"
#include "fts-user.h"
#include "fts-solr-plugin.h"

#define DEFAULT_SOLR_BATCH_SIZE 1000

const char *fts_solr_plugin_version = DOVECOT_ABI_VERSION;
struct http_client *solr_http_client = NULL;

struct fts_solr_user_module fts_solr_user_module =
	MODULE_CONTEXT_INIT(&mail_user_module_register);

static int
fts_solr_plugin_init_settings(struct mail_user *user,
			      struct fts_solr_settings *set, const char *str)
{
	const char *value, *const *tmp;

	if (str == NULL)
		str = "";

	set->batch_size = DEFAULT_SOLR_BATCH_SIZE;
	set->soft_commit = TRUE;

	for (tmp = t_strsplit_spaces(str, " "); *tmp != NULL; tmp++) {
		if (str_begins(*tmp, "url=", &value)) {
			set->url = p_strdup(user->pool, value);
		} else if (strcmp(*tmp, "debug") == 0) {
			set->debug = TRUE;
		} else if (strcmp(*tmp, "use_libfts") == 0) {
			set->use_libfts = TRUE;
		} else if (str_begins(*tmp, "rawlog_dir=", &value)) {
			set->rawlog_dir = p_strdup(user->pool, value);
		} else if (str_begins(*tmp, "batch_size=", &value)) {
			if (str_to_uint(value, &set->batch_size) < 0 ||
			    set->batch_size == 0) {
				e_error(user->event,
					"fts-solr: batch_size must be a positive integer");
					return -1;
			}
		} else if (str_begins(*tmp, "soft_commit=", &value)) {
			if (strcmp(value, "yes") == 0) {
				set->soft_commit = TRUE;
			} else if (strcmp(value, "no") == 0) {
				set->soft_commit = FALSE;
			} else {
				e_error(user->event,
					"fts-solr: Invalid setting for soft_commit: %s",
					*tmp+12);
				return -1;
			}
		} else {
			e_error(user->event,
				"fts-solr: Invalid setting: %s", *tmp);
			return -1;
		}
	}
	if (set->url == NULL) {
		e_error(user->event, "fts-solr: url setting missing");
		return -1;
	}
	return 0;
}

static void fts_solr_mail_user_deinit(struct mail_user *user)
{
	struct fts_solr_user *fuser = FTS_SOLR_USER_CONTEXT_REQUIRE(user);

	fts_mail_user_deinit(user);
	fuser->module_ctx.super.deinit(user);
}

static void fts_solr_mail_user_create(struct mail_user *user, const char *env)
{
	struct mail_user_vfuncs *v = user->vlast;
	struct fts_solr_user *fuser;
	const char *error;

	fuser = p_new(user->pool, struct fts_solr_user, 1);
	if (fts_solr_plugin_init_settings(user, &fuser->set, env) < 0) {
		/* invalid settings, disabling */
		return;
	}
	if (fts_mail_user_init(user, fuser->set.use_libfts, &error) < 0) {
		e_error(user->event, "fts-solr: %s", error);
		return;
	}

	fuser->module_ctx.super = *v;
	user->vlast = &fuser->module_ctx.super;
	v->deinit = fts_solr_mail_user_deinit;
	MODULE_CONTEXT_SET(user, fts_solr_user_module, fuser);
}

static void fts_solr_mail_user_created(struct mail_user *user)
{
	const char *env;

	env = mail_user_plugin_getenv(user, "fts_solr");
	if (env != NULL)
		fts_solr_mail_user_create(user, env);
}

static struct mail_storage_hooks fts_solr_mail_storage_hooks = {
	.mail_user_created = fts_solr_mail_user_created
};

void fts_solr_plugin_init(struct module *module)
{
	fts_backend_register(&fts_backend_solr);
	mail_storage_hooks_add(module, &fts_solr_mail_storage_hooks);
}

void fts_solr_plugin_deinit(void)
{
	fts_backend_unregister(fts_backend_solr.name);
	mail_storage_hooks_remove(&fts_solr_mail_storage_hooks);
	if (solr_http_client != NULL)
		http_client_deinit(&solr_http_client);

}

const char *fts_solr_plugin_dependencies[] = { "fts", NULL };
