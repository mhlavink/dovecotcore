/* Copyright (c) the Dovecot authors, based on code by Michael Slusarz.
 * See the included COPYING file */

#ifndef FTS_FLATCURVE_PLUGIN_H
#define FTS_FLATCURVE_PLUGIN_H

#include "module-context.h"
#include "mail-user.h"
#include "lib.h"
#include "fts-api-private.h"
#include "fts-flatcurve-settings.h"

#define FTS_FLATCURVE_USER_CONTEXT(obj) \
	MODULE_CONTEXT(obj, fts_flatcurve_user_module)
#define FTS_FLATCURVE_USER_CONTEXT_REQUIRE(obj) \
	MODULE_CONTEXT_REQUIRE(obj, fts_flatcurve_user_module)

struct fts_flatcurve_user {
	union mail_user_module_context module_ctx;
	struct flatcurve_fts_backend *backend;
	struct fts_flatcurve_settings *set;
};

extern struct fts_backend fts_backend_flatcurve;
extern MODULE_CONTEXT_DEFINE(fts_flatcurve_user_module, &mail_user_module_register);

int fts_flatcurve_mail_user_get(struct mail_user *user, struct event *event,
				struct fts_flatcurve_user **fuser_r,
				const char **error_r);

void fts_flatcurve_plugin_init(struct module *module);
void fts_flatcurve_plugin_deinit(void);

#endif

