/* Copyright (c) 2010-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "settings.h"
#include "mail-storage.h"
#include "mail-storage-service.h"
#include "mail-namespace.h"
#include "doveadm-mailbox-list-iter.h"
#include "doveadm-mail-iter.h"
#include "doveadm-mail.h"

struct import_cmd_context {
	struct doveadm_mail_cmd_context ctx;

	const char *src_mail_driver, *src_mail_path;
	const char *const *src_options;
	const char *src_username;
	struct mail_user *src_user;
	const char *dest_parent;
	bool subscribe;
};

static const char *
convert_vname_separators(const char *vname, char src_sep, char dest_sep)
{
	string_t *str = t_str_new(128);

	for (; *vname != '\0'; vname++) {
		if (*vname == src_sep)
			str_append_c(str, dest_sep);
		else if (*vname == dest_sep)
			str_append_c(str, '_');
		else
			str_append_c(str, *vname);
	}
	return str_c(str);
}

static int
dest_mailbox_open_or_create(struct import_cmd_context *ctx,
			    struct mail_user *user,
			    const struct mailbox_info *info,
			    struct mailbox **box_r)
{
	struct mail_namespace *ns;
	struct mailbox *box;
	enum mail_error error;
	const char *name, *errstr;

	if (*ctx->dest_parent != '\0') {
		/* prefix destination mailbox name with given parent mailbox */
		ns = mail_namespace_find(user->namespaces, ctx->dest_parent);
	} else {
		ns = mail_namespace_find(user->namespaces, info->vname);
	}
	name = convert_vname_separators(info->vname,
					mail_namespace_get_sep(info->ns),
					mail_namespace_get_sep(ns));

	if (*ctx->dest_parent != '\0') {
		name = t_strdup_printf("%s%c%s", ctx->dest_parent,
				       mail_namespace_get_sep(ns), name);
	}

	box = mailbox_alloc(ns->list, name, MAILBOX_FLAG_SAVEONLY);
	if (mailbox_create(box, NULL, FALSE) < 0) {
		errstr = mailbox_get_last_internal_error(box, &error);
		if (error != MAIL_ERROR_EXISTS) {
			e_error(user->event,
				"Couldn't create mailbox %s: %s", name, errstr);
			doveadm_mail_failed_mailbox(&ctx->ctx, box);
			mailbox_free(&box);
			return -1;
		}
	}
	if (ctx->subscribe) {
		if (mailbox_set_subscribed(box, TRUE) < 0) {
			e_error(user->event,
				"Couldn't subscribe to mailbox %s: %s",
				name, mailbox_get_last_internal_error(box, NULL));
		}
	}
	if (mailbox_sync(box, MAILBOX_SYNC_FLAG_FULL_READ) < 0) {
		e_error(user->event,
			"Syncing mailbox %s failed: %s", name,
			mailbox_get_last_internal_error(box, NULL));
		doveadm_mail_failed_mailbox(&ctx->ctx, box);
		mailbox_free(&box);
		return -1;
	}
	*box_r = box;
	return 0;
}

static int
cmd_import_box_contents(struct doveadm_mail_cmd_context *ctx,
			struct doveadm_mail_iter *iter, struct mail *src_mail,
			struct mailbox *dest_box)
{
	struct mail_save_context *save_ctx;
	struct mailbox_transaction_context *dest_trans;
	int ret = 0;

	dest_trans = mailbox_transaction_begin(dest_box,
					MAILBOX_TRANSACTION_FLAG_EXTERNAL |
					ctx->transaction_flags,	__func__);
	do {
		e_debug(mail_event(src_mail), "import");
		save_ctx = mailbox_save_alloc(dest_trans);
		mailbox_save_copy_flags(save_ctx, src_mail);
		if (mailbox_copy(&save_ctx, src_mail) < 0) {
			e_error(mail_event(src_mail),
				"Copying failed: %s",
				mailbox_get_last_internal_error(dest_box, NULL));
			ret = -1;
		}
	} while (doveadm_mail_iter_next(iter, &src_mail));

	if (mailbox_transaction_commit(&dest_trans) < 0) {
		e_error(ctx->cctx->event,
			"Committing copied mails failed: %s",
			mailbox_get_last_internal_error(dest_box, NULL));
		ret = -1;
	}
	return ret;
}

static int
cmd_import_box(struct import_cmd_context *ctx, struct mail_user *dest_user,
	       const struct mailbox_info *info,
	       struct mail_search_args *search_args)
{
	struct doveadm_mail_iter *iter;
	struct mailbox *box;
	struct mail *mail;

	int ret = doveadm_mail_iter_init(&ctx->ctx, info, search_args, 0, NULL,
					 DOVEADM_MAIL_ITER_FLAG_READONLY,
					 &iter);
	if (ret <= 0)
		return ret;

	ret = 0;
	if (doveadm_mail_iter_next(iter, &mail)) {
		/* at least one mail matches in this mailbox */
		if (dest_mailbox_open_or_create(ctx, dest_user, info, &box) < 0)
			ret = -1;
		else {
			if (cmd_import_box_contents(&ctx->ctx, iter, mail, box) < 0) {
				doveadm_mail_failed_mailbox(&ctx->ctx, mail->box);
				ret = -1;
			}
			mailbox_free(&box);
		}
	}
	if (doveadm_mail_iter_deinit_sync(&iter) < 0)
		ret = -1;
	return ret;
}

static void cmd_import_init_source_user(struct import_cmd_context *ctx, struct mail_user *dest_user)
{
	struct mail_storage_service_input input;
	struct mail_storage_service_user *service_user;
	struct mail_user *user;
	const char *error;

	/* create a user for accessing the source storage */
	i_zero(&input);
	input.username = ctx->src_username != NULL ?
			 ctx->src_username :
			 dest_user->username;

	mail_storage_service_io_deactivate_user(ctx->ctx.cur_service_user);
	input.flags_override_add = MAIL_STORAGE_SERVICE_FLAG_NO_NAMESPACES |
		MAIL_STORAGE_SERVICE_FLAG_NO_RESTRICT_ACCESS;
	if (mail_storage_service_lookup(ctx->ctx.storage_service, &input,
					&service_user, &error) < 0)
		i_fatal("Import service user initialization failed: %s", error);

	struct settings_instance *set_instance =
		mail_storage_service_user_get_settings_instance(service_user);
	mail_storage_2nd_settings_reset(set_instance, "*/");
	for (unsigned int i = 0; ctx->src_options[i] != NULL; i++) {
		const char *key, *value;
		t_split_key_value_eq(ctx->src_options[i], &key, &value);
		settings_override(set_instance, t_strconcat("*/", key, NULL),
				  value, SETTINGS_OVERRIDE_TYPE_2ND_CLI_PARAM);
	}
	settings_override(set_instance, "*/mail_driver", ctx->src_mail_driver,
			  SETTINGS_OVERRIDE_TYPE_2ND_CLI_PARAM);
	settings_override(set_instance, "*/mail_path", ctx->src_mail_path,
			  SETTINGS_OVERRIDE_TYPE_2ND_CLI_PARAM);

	if (mail_storage_service_next(ctx->ctx.storage_service, service_user,
				      &user, &error) < 0)
		i_fatal("Import user initialization failed: %s", error);

	if (mail_namespaces_init_location(user, user->event, &error) < 0)
		i_fatal("Import namespace initialization failed: %s", error);
	mail_storage_service_user_unref(&service_user);

	ctx->src_user = user;
	mail_storage_service_io_deactivate_user(user->service_user);
	mail_storage_service_io_activate_user(ctx->ctx.cur_service_user);
}

static int
cmd_import_run(struct doveadm_mail_cmd_context *_ctx, struct mail_user *user)
{
	struct import_cmd_context *ctx =
		container_of(_ctx, struct import_cmd_context, ctx);
	const enum mailbox_list_iter_flags iter_flags =
		MAILBOX_LIST_ITER_NO_AUTO_BOXES |
		MAILBOX_LIST_ITER_RETURN_NO_FLAGS;
	struct doveadm_mailbox_list_iter *iter;
	const struct mailbox_info *info;
	int ret = 0;

	if (ctx->src_user == NULL)
		cmd_import_init_source_user(ctx, user);

	iter = doveadm_mailbox_list_iter_init(_ctx, ctx->src_user,
					      _ctx->search_args, iter_flags);
	while ((info = doveadm_mailbox_list_iter_next(iter)) != NULL) T_BEGIN {
		if (cmd_import_box(ctx, user, info, _ctx->search_args) < 0)
			ret = -1;
	} T_END;
	if (doveadm_mailbox_list_iter_deinit(&iter) < 0)
		ret = -1;
	return ret;
}

static void cmd_import_init(struct doveadm_mail_cmd_context *_ctx)
{
	struct import_cmd_context *ctx =
		container_of(_ctx, struct import_cmd_context, ctx);
	struct doveadm_cmd_context *cctx = _ctx->cctx;
	const char *src_location;

	(void)doveadm_cmd_param_str(cctx, "source-user", &ctx->src_username);
	ctx->subscribe = doveadm_cmd_param_flag(cctx, "subscribe");

	const char *const *query;
	if (!doveadm_cmd_param_str(cctx, "source-location", &src_location) ||
	    !doveadm_cmd_param_str(cctx, "dest-parent-mailbox", &ctx->dest_parent) ||
	    !doveadm_cmd_param_array(cctx, "query", &query))
		doveadm_mail_help_name("import");

	ctx->src_mail_path = strchr(src_location, ':');
	if (ctx->src_mail_path == NULL ||
	    strchr(ctx->src_mail_path + 1, ':') != NULL) {
		e_error(_ctx->cctx->event,
			"Source (%s) should be in mail_driver:mail_path syntax",
			src_location);
		ctx->ctx.exit_code = EX_USAGE;
		return;
	}
	ctx->src_mail_driver =
		p_strdup_until(_ctx->pool, src_location, ctx->src_mail_path++);

	if (!doveadm_cmd_param_array(cctx, "source-option",
				     &ctx->src_options))
		ctx->src_options = empty_str_array;

	_ctx->search_args = doveadm_mail_build_search_args(query);
}

static void cmd_import_deinit(struct doveadm_mail_cmd_context *_ctx)
{
	struct import_cmd_context *ctx =
		container_of(_ctx, struct import_cmd_context, ctx);

	if (ctx->src_user != NULL)
		mail_user_deinit(&ctx->src_user);
}

static struct doveadm_mail_cmd_context *cmd_import_alloc(void)
{
	struct import_cmd_context *ctx;

	ctx = doveadm_mail_cmd_alloc(struct import_cmd_context);
	ctx->ctx.v.init = cmd_import_init;
	ctx->ctx.v.deinit = cmd_import_deinit;
	ctx->ctx.v.run = cmd_import_run;
	return &ctx->ctx;
}

struct doveadm_cmd_ver2 doveadm_cmd_import_ver2 = {
	.name = "import",
	.mail_cmd = cmd_import_alloc,
	.usage = DOVEADM_CMD_MAIL_USAGE_PREFIX
		"[-U source-user] [-s] [-p <source option> [...]] "
		"<source mail driver>:<source mail path> "
		"<dest parent mailbox> <search query>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_MAIL_COMMON
DOVEADM_CMD_PARAM('U', "source-user", CMD_PARAM_STR, 0)
DOVEADM_CMD_PARAM('s', "subscribe", CMD_PARAM_BOOL, 0)
DOVEADM_CMD_PARAM('p', "source-option", CMD_PARAM_ARRAY, 0)
DOVEADM_CMD_PARAM('\0', "source-location", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "dest-parent-mailbox", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "query", CMD_PARAM_ARRAY, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
};
