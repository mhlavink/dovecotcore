/* Copyright (c) 2005-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "istream.h"
#include "istream-seekable.h"
#include "ostream.h"
#include "str.h"
#include "settings.h"
#include "mail-user.h"
#include "index-storage.h"
#include "index-mail.h"
#include "compression.h"
#include "mail-compress-plugin.h"

#include <fcntl.h>

#define MAIL_COMPRESS_CONTEXT(obj) \
	MODULE_CONTEXT_REQUIRE(obj, mail_compress_storage_module)
#define MAIL_COMPRESS_MAIL_CONTEXT(obj) \
	MODULE_CONTEXT_REQUIRE(obj, mail_compress_mail_module)
#define MAIL_COMPRESS_USER_CONTEXT(obj) \
	MODULE_CONTEXT_REQUIRE(obj, mail_compress_user_module)

#define MAX_INBUF_SIZE (1024*1024)
#define MAIL_COMPRESS_MAIL_CACHE_EXPIRE_MSECS (60*1000)

struct mail_compress_mail {
	union mail_module_context module_ctx;
	bool verifying_save;
};

struct mail_compress_mail_cache {
	struct timeout *to;
	struct mailbox *box;
	uint32_t uid;

	struct istream *input;
};

struct mail_compress_user {
	union mail_user_module_context module_ctx;

	struct mail_compress_mail_cache cache;

	const struct compression_handler *save_handler;
};

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct mail_compress_settings)

static struct setting_define mail_compress_setting_defines[] = {
	DEF(STR, mail_compress_write_method),

	SETTING_DEFINE_LIST_END
};

static struct mail_compress_settings mail_compress_default_settings = {
	.mail_compress_write_method = "",
};

const struct setting_parser_info mail_compress_setting_parser_info = {
	.name = "mail_compress",
	.plugin_dependency = "lib20_mail_compress_plugin",

	.defines = mail_compress_setting_defines,
	.defaults = &mail_compress_default_settings,

	.struct_size = sizeof(struct mail_compress_settings),
	.pool_offset1 = 1 + offsetof(struct mail_compress_settings, pool),
};

const char *mail_compress_plugin_version = DOVECOT_ABI_VERSION;

static MODULE_CONTEXT_DEFINE_INIT(mail_compress_user_module,
				  &mail_user_module_register);
static MODULE_CONTEXT_DEFINE_INIT(mail_compress_storage_module,
				  &mail_storage_module_register);
static MODULE_CONTEXT_DEFINE_INIT(mail_compress_mail_module, &mail_module_register);

static bool mail_compress_mailbox_is_permail(struct mailbox *box)
{
	enum mail_storage_class_flags class_flags = box->storage->class_flags;

	return (class_flags & MAIL_STORAGE_CLASS_FLAG_OPEN_STREAMS) == 0 &&
		(class_flags & MAIL_STORAGE_CLASS_FLAG_BINARY_DATA) != 0;
}

static void mail_compress_mail_cache_close(struct mail_compress_user *zuser)
{
	struct mail_compress_mail_cache *cache = &zuser->cache;

	timeout_remove(&cache->to);
	i_stream_unref(&cache->input);
	i_zero(cache);
}

static struct istream *
mail_compress_mail_cache_open(struct mail_compress_user *zuser,
			      struct mail *mail, struct istream *input,
			      bool do_cache)
{
	struct mail_compress_mail_cache *cache = &zuser->cache;
	struct istream *inputs[2];
	string_t *temp_prefix = t_str_new(128);

	if (do_cache)
		mail_compress_mail_cache_close(zuser);

	/* compress istream is seekable, but very slow. create a seekable
	   istream which we can use to quickly seek around in the stream that's
	   been read so far. usually the partial IMAP FETCHes continue from
	   where the previous left off, so this isn't strictly necessary, but
	   with the way lib-imap-storage's CRLF-cache works it has to seek
	   backwards somewhat, which causes a compress stream reset. And the
	   CRLF-cache isn't easy to fix.. */
	input->seekable = FALSE;
	inputs[0] = input;
	inputs[1] = NULL;
	mail_user_set_get_temp_prefix(temp_prefix, mail->box->storage->user->set);
	input = i_stream_create_seekable_path(inputs,
				i_stream_get_max_buffer_size(inputs[0]),
				str_c(temp_prefix));
	i_stream_set_name(input, t_strdup_printf("compress(%s)",
						 i_stream_get_name(inputs[0])));
	i_stream_unref(&inputs[0]);

	if (do_cache) {
		cache->to = timeout_add(MAIL_COMPRESS_MAIL_CACHE_EXPIRE_MSECS,
					mail_compress_mail_cache_close, zuser);
		cache->box = mail->box;
		cache->uid = mail->uid;
		cache->input = input;
		/* index-mail wants the stream to be destroyed at close, so create
		   a new stream instead of just increasing reference. */
		return i_stream_create_limit(cache->input, UOFF_T_MAX);
	} else {
		return input;
	}
}

static int mail_compress_istream_opened(struct mail *_mail, struct istream **stream)
{
	struct mail_compress_user *zuser = MAIL_COMPRESS_USER_CONTEXT(_mail->box->storage->user);
	struct mail_compress_mail_cache *cache = &zuser->cache;
	struct mail_private *mail = (struct mail_private *)_mail;
	struct mail_compress_mail *zmail = MAIL_COMPRESS_MAIL_CONTEXT(mail);
	struct istream *input;
	const struct compression_handler *handler;

	if (zmail->verifying_save) {
		/* mail_compress_mail_save_finish() is verifying that the user-given
		   input doesn't look compressed. */
		return zmail->module_ctx.super.istream_opened(_mail, stream);
	}

	if (_mail->uid > 0 && cache->uid == _mail->uid && cache->box == _mail->box) {
		/* use the cached stream. when doing partial reads it should
		   already be seeked into the wanted offset. */
		i_stream_unref(stream);
		i_stream_seek(cache->input, 0);
		*stream = i_stream_create_limit(cache->input, UOFF_T_MAX);
		return zmail->module_ctx.super.istream_opened(_mail, stream);
	}

	handler = compression_detect_handler(*stream);
	if (handler != NULL) {
		if (handler->create_istream == NULL) {
			mail_set_critical(_mail,
				"mail_compress plugin: Detected %s compression "
				"but support not compiled in", handler->ext);
			return -1;
		}

		input = *stream;
		*stream = handler->create_istream(input);
		i_stream_unref(&input);
		/* dont cache the stream if _mail->uid is 0 */
		*stream = mail_compress_mail_cache_open(zuser, _mail, *stream,
							(_mail->uid > 0));
	}
	return zmail->module_ctx.super.istream_opened(_mail, stream);
}

static void mail_compress_mail_close(struct mail *_mail)
{
	struct mail_private *mail = (struct mail_private *)_mail;
	struct mail_compress_mail *zmail = MAIL_COMPRESS_MAIL_CONTEXT(mail);
	struct mail_compress_user *zuser = MAIL_COMPRESS_USER_CONTEXT(_mail->box->storage->user);
	struct mail_compress_mail_cache *cache = &zuser->cache;
	uoff_t size;

	if (_mail->uid > 0 && cache->uid == _mail->uid && cache->box == _mail->box) {
		/* make sure we have read the entire email into the seekable
		   stream (which causes the original input stream to be
		   unrefed). we can't safely keep the original input stream
		   open after the mail is closed. */
		if (i_stream_get_size(cache->input, TRUE, &size) < 0)
			mail_compress_mail_cache_close(zuser);
	}
	zmail->module_ctx.super.close(_mail);
}

static void mail_compress_mail_allocated(struct mail *_mail)
{
	struct mail_private *mail = (struct mail_private *)_mail;
	struct mail_vfuncs *v = mail->vlast;
	struct mail_compress_mail *zmail;

	if (!mail_compress_mailbox_is_permail(_mail->box))
		return;

	zmail = p_new(mail->pool, struct mail_compress_mail, 1);
	zmail->module_ctx.super = *v;
	mail->vlast = &zmail->module_ctx.super;

	v->istream_opened = mail_compress_istream_opened;
	v->close = mail_compress_mail_close;
	MODULE_CONTEXT_SET(mail, mail_compress_mail_module, zmail);
}

static int mail_compress_mail_save_finish(struct mail_save_context *ctx)
{
	struct mailbox *box = ctx->transaction->box;
	union mailbox_module_context *zbox = MAIL_COMPRESS_CONTEXT(box);
	struct mail_private *mail = (struct mail_private *)ctx->dest_mail;
	struct mail_compress_mail *zmail = MAIL_COMPRESS_MAIL_CONTEXT(mail);
	struct istream *input;
	int ret;

	if (zbox->super.save_finish(ctx) < 0)
		return -1;

	zmail->verifying_save = TRUE;
	ret = mail_get_stream(ctx->dest_mail, NULL, NULL, &input);
	zmail->verifying_save = FALSE;
	if (ret < 0)
		return -1;

	if (compression_detect_handler(input) != NULL) {
		mail_storage_set_error(box->storage, MAIL_ERROR_NOTPOSSIBLE,
			"Saving mails compressed by client isn't supported");
		return -1;
	}
	return 0;
}

static int
mail_compress_mail_save_compress_begin(struct mail_save_context *ctx,
				       struct istream *input)
{
	struct mailbox *box = ctx->transaction->box;
	struct mail_compress_user *zuser = MAIL_COMPRESS_USER_CONTEXT(box->storage->user);
	union mailbox_module_context *zbox = MAIL_COMPRESS_CONTEXT(box);
	struct ostream *output;

	if (zbox->super.save_begin(ctx, input) < 0)
		return -1;

	output = zuser->save_handler->create_ostream_auto(ctx->data.output,
							  box->event);
	o_stream_unref(&ctx->data.output);
	ctx->data.output = output;
	o_stream_cork(ctx->data.output);
	return 0;
}

static void
mail_compress_permail_alloc_init(struct mailbox *box, struct mailbox_vfuncs *v)
{
	struct mail_compress_user *zuser = MAIL_COMPRESS_USER_CONTEXT(box->storage->user);

	if (zuser->save_handler == NULL) {
		v->save_finish = mail_compress_mail_save_finish;
	} else {
		v->save_begin = mail_compress_mail_save_compress_begin;
	}
}

static void mail_compress_mailbox_open_input(struct mailbox *box)
{
	const struct compression_handler *handler;
	struct istream *input;
	struct stat st;
	int fd;

	if (compression_lookup_handler_from_ext(box->name, &handler) <= 0)
		return;

	if (mail_storage_is_mailbox_file(box->storage)) {
		/* looks like a compressed single file mailbox. we should be
		   able to handle this. */
		const char *box_path = mailbox_get_path(box);

		fd = open(box_path, O_RDONLY);
		if (fd == -1) {
			/* let the standard handler figure out what to do
			   with the failure */
			return;
		}
		if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
			i_close_fd(&fd);
			return;
		}
		input = i_stream_create_fd_autoclose(&fd, MAX_INBUF_SIZE);
		i_stream_set_name(input, box_path);
		box->input = handler->create_istream(input);
		i_stream_unref(&input);
		box->flags |= MAILBOX_FLAG_READONLY;
	}
}

static int mail_compress_mailbox_open(struct mailbox *box)
{
	union mailbox_module_context *zbox = MAIL_COMPRESS_CONTEXT(box);

	if (box->input == NULL &&
	    (box->storage->class_flags &
	     MAIL_STORAGE_CLASS_FLAG_OPEN_STREAMS) != 0)
		mail_compress_mailbox_open_input(box);

	return zbox->super.open(box);
}

static void mail_compress_mailbox_close(struct mailbox *box)
{
	union mailbox_module_context *zbox = MAIL_COMPRESS_CONTEXT(box);
	struct mail_compress_user *zuser = MAIL_COMPRESS_USER_CONTEXT(box->storage->user);

	if (zuser->cache.box == box)
		mail_compress_mail_cache_close(zuser);
	zbox->super.close(box);
}

static void mail_compress_mailbox_allocated(struct mailbox *box)
{
	struct mailbox_vfuncs *v = box->vlast;
	union mailbox_module_context *zbox;

	zbox = p_new(box->pool, union mailbox_module_context, 1);
	zbox->super = *v;
	box->vlast = &zbox->super;
	v->open = mail_compress_mailbox_open;
	v->close = mail_compress_mailbox_close;

	MODULE_CONTEXT_SET_SELF(box, mail_compress_storage_module, zbox);

	if (mail_compress_mailbox_is_permail(box))
		mail_compress_permail_alloc_init(box, v);
}

static void mail_compress_mail_user_deinit(struct mail_user *user)
{
	struct mail_compress_user *zuser = MAIL_COMPRESS_USER_CONTEXT(user);

	mail_compress_mail_cache_close(zuser);
	zuser->module_ctx.super.deinit(user);
}

static void mail_compress_mail_user_created(struct mail_user *user)
{
	struct mail_user_vfuncs *v = user->vlast;
	struct mail_compress_user *zuser;
	const struct mail_compress_settings *set;
	const char *error;
	int ret;

	zuser = p_new(user->pool, struct mail_compress_user, 1);
	zuser->module_ctx.super = *v;
	user->vlast = &zuser->module_ctx.super;
	v->deinit = mail_compress_mail_user_deinit;

	if (settings_get(user->event, &mail_compress_setting_parser_info, 0,
			 &set, &error) < 0) {
		user->error = p_strdup(user->pool, error);
		return;
	}
	if (set->mail_compress_write_method[0] != '\0') {
		ret = compression_lookup_handler(set->mail_compress_write_method,
						 &zuser->save_handler);
		if (ret <= 0) {
			user->error = p_strdup_printf(user->pool,
				"mail_compress_save_method: %s: %s", ret == 0 ?
				"Support not compiled in for handler" :
				"Unknown handler",
				set->mail_compress_write_method);
			settings_free(set);
			return;
		}
	}
	settings_free(set);

	MODULE_CONTEXT_SET(user, mail_compress_user_module, zuser);
}

static struct mail_storage_hooks mail_compress_mail_storage_hooks = {
	.mail_user_created = mail_compress_mail_user_created,
	.mailbox_allocated = mail_compress_mailbox_allocated,
	.mail_allocated = mail_compress_mail_allocated
};

void mail_compress_plugin_init(struct module *module)
{
	mail_storage_hooks_add(module, &mail_compress_mail_storage_hooks);
}

void mail_compress_plugin_deinit(void)
{
	mail_storage_hooks_remove(&mail_compress_mail_storage_hooks);
}
