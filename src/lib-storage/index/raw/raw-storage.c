/* Copyright (c) 2007-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "istream.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "index-mail.h"
#include "mail-copy.h"
#include "mail-storage-service.h"
#include "mailbox-list-private.h"
#include "raw-sync.h"
#include "raw-storage.h"

extern struct mail_storage raw_storage;
extern struct mailbox raw_mailbox;

struct mail_user *
raw_storage_create_from_set(struct mail_storage_service_ctx *ctx,
			    struct settings_instance *set_instance)
{
	struct mail_user *user;
	struct mail_namespace *ns;
	struct mail_namespace_settings *ns_set;
	struct event *event;
	const char *error;

	struct ioloop_context *old_ioloop_ctx =
		io_loop_get_current_context(current_ioloop);

	event = event_create(NULL);
	/* Don't include raw user's events in statistics or anything else.
	   They would just cause confusion. */
	event_disable_callbacks(event);

	const struct master_service_settings *service_set =
		master_service_get_service_settings(master_service);
	const char *const userdb_fields[] = {
		/* use unwritable home directory */
		t_strdup_printf("home=%s/empty", service_set->base_dir),
		/* absolute paths are ok with raw storage */
		"mail_full_filesystem_access=yes",
		NULL,
	};
	struct mail_storage_service_input input = {
		.event_parent = event,
		.username = "raw-mail-user",
		.set_instance = set_instance,
		.autocreated = TRUE,
		.no_userdb_lookup = TRUE,
		.userdb_fields = userdb_fields,
		.flags_override_add =
			MAIL_STORAGE_SERVICE_FLAG_NO_RESTRICT_ACCESS |
			MAIL_STORAGE_SERVICE_FLAG_NO_CHDIR |
			MAIL_STORAGE_SERVICE_FLAG_NO_LOG_INIT |
			MAIL_STORAGE_SERVICE_FLAG_NO_PLUGINS |
			MAIL_STORAGE_SERVICE_FLAG_NO_NAMESPACES,
	};
	if (mail_storage_service_lookup_next(ctx, &input,
					     &user, &error) < 0)
		i_fatal("Raw user initialization failed: %s", error);
	event_unref(&event);

	ns_set = p_new(user->pool, struct mail_namespace_settings, 1);
	ns_set->name = "raw-storage";
	ns_set->location = ":LAYOUT=none";
	ns_set->separator = "/";

	ns = mail_namespaces_init_empty(user);
	/* raw storage doesn't have INBOX. We especially don't want LIST to
	   return INBOX. */
	ns->flags &= ENUM_NEGATE(NAMESPACE_FLAG_INBOX_USER);
	ns->flags |= NAMESPACE_FLAG_NOQUOTA | NAMESPACE_FLAG_NOACL;
	ns->set = ns_set;

	if (mail_storage_create(ns, "raw", 0, &error) < 0)
		i_fatal("Couldn't create internal raw storage: %s", error);
	if (mail_namespaces_init_finish(ns, &error) < 0)
		i_fatal("Couldn't create internal raw namespace: %s", error);

	if (old_ioloop_ctx != NULL)
		io_loop_context_switch(old_ioloop_ctx);
	else
		mail_storage_service_io_deactivate_user(user->service_user);
	return user;
}

static int ATTR_NULL(2, 3)
raw_mailbox_alloc_common(struct mail_user *user, struct istream *input,
			 const char *path, time_t received_time,
			 const char *envelope_sender, struct mailbox **box_r)
{
	struct mail_namespace *ns = user->namespaces;
	struct mailbox *box;
	struct raw_mailbox *raw_box;
	const char *name;

	name = path != NULL ? path : i_stream_get_name(input);
	box = *box_r = mailbox_alloc(ns->list, name,
				     MAILBOX_FLAG_NO_INDEX_FILES);
	if (input != NULL) {
		if (mailbox_open_stream(box, input) < 0)
			return -1;
	} else {
		if (mailbox_open(box) < 0)
			return -1;
	}
	if (mailbox_sync(box, 0) < 0)
		return -1;

	i_assert(strcmp(box->storage->name, RAW_STORAGE_NAME) == 0);
	raw_box = RAW_MAILBOX(box);
	raw_box->envelope_sender = p_strdup(box->pool, envelope_sender);
	raw_box->mtime = received_time;
	return 0;
}

int raw_mailbox_alloc_stream(struct mail_user *user, struct istream *input,
			     time_t received_time, const char *envelope_sender,
			     struct mailbox **box_r)
{
	return raw_mailbox_alloc_common(user, input, NULL, received_time,
					envelope_sender, box_r);
}

int raw_mailbox_alloc_path(struct mail_user *user, const char *path,
			   time_t received_time, const char *envelope_sender,
			   struct mailbox **box_r)
{
	return raw_mailbox_alloc_common(user, NULL, path, received_time,
					envelope_sender, box_r);
}

static struct mail_storage *raw_storage_alloc(void)
{
	struct raw_storage *storage;
	pool_t pool;

	pool = pool_alloconly_create("raw storage", 512+256);
	storage = p_new(pool, struct raw_storage, 1);
	storage->storage = raw_storage;
	storage->storage.pool = pool;
	return &storage->storage;
}

static void
raw_storage_get_list_settings(const struct mail_namespace *ns ATTR_UNUSED,
			      struct mailbox_list_settings *set)
{
	if (set->layout == NULL)
		set->layout = MAILBOX_LIST_NAME_FS;
	if (set->subscription_fname == NULL)
		set->subscription_fname = RAW_SUBSCRIPTION_FILE_NAME;
}

static struct mailbox *
raw_mailbox_alloc(struct mail_storage *storage, struct mailbox_list *list,
		  const char *vname, enum mailbox_flags flags)
{
	struct raw_mailbox *mbox;
	pool_t pool;

	flags |= MAILBOX_FLAG_READONLY | MAILBOX_FLAG_NO_INDEX_FILES;

	pool = pool_alloconly_create("raw mailbox", 1024*3);
	mbox = p_new(pool, struct raw_mailbox, 1);
	mbox->box = raw_mailbox;
	mbox->box.pool = pool;
	mbox->box.storage = storage;
	mbox->box.list = list;
	mbox->box.mail_vfuncs = &raw_mail_vfuncs;

	index_storage_mailbox_alloc(&mbox->box, vname, flags, "dovecot.index");

	mbox->mtime = mbox->ctime = (time_t)-1;
	mbox->storage = RAW_STORAGE(storage);
	mbox->size = UOFF_T_MAX;
	return &mbox->box;
}

static int raw_mailbox_open(struct mailbox *box)
{
	struct raw_mailbox *mbox = RAW_MAILBOX(box);
	const char *path;
	int fd;

	if (box->input != NULL) {
		mbox->mtime = mbox->ctime = ioloop_time;
		return index_storage_mailbox_open(box, FALSE);
	}

	path = box->_path = box->name;
	mbox->have_filename = TRUE;
	fd = open(path, O_RDONLY);
	if (fd == -1) {
		if (ENOTFOUND(errno)) {
			mail_storage_set_error(box->storage,
				MAIL_ERROR_NOTFOUND,
				T_MAIL_ERR_MAILBOX_NOT_FOUND(box->vname));
		} else if (!mail_storage_set_error_from_errno(box->storage)) {
			mailbox_set_critical(box, "open(%s) failed: %m", path);
		}
		return -1;
	}
	box->input = i_stream_create_fd_autoclose(&fd, MAIL_READ_FULL_BLOCK_SIZE);
	i_stream_set_name(box->input, path);
	i_stream_set_init_buffer_size(box->input, MAIL_READ_FULL_BLOCK_SIZE);
	return index_storage_mailbox_open(box, FALSE);
}

static int
raw_mailbox_create(struct mailbox *box,
		   const struct mailbox_update *update ATTR_UNUSED,
		   bool directory ATTR_UNUSED)
{
	mail_storage_set_error(box->storage, MAIL_ERROR_NOTPOSSIBLE,
			       "Raw mailbox creation isn't supported");
	return -1;
}

static int
raw_mailbox_update(struct mailbox *box,
		   const struct mailbox_update *update ATTR_UNUSED)
{
	mail_storage_set_error(box->storage, MAIL_ERROR_NOTPOSSIBLE,
			       "Raw mailbox update isn't supported");
	return -1;
}

static void raw_notify_changes(struct mailbox *box ATTR_UNUSED)
{
}

struct mail_storage raw_storage = {
	.name = RAW_STORAGE_NAME,
	.class_flags = MAIL_STORAGE_CLASS_FLAG_MAILBOX_IS_FILE |
		MAIL_STORAGE_CLASS_FLAG_OPEN_STREAMS |
		MAIL_STORAGE_CLASS_FLAG_BINARY_DATA,

	.v = {
		raw_storage_alloc,
		NULL,
		index_storage_destroy,
		NULL,
		raw_storage_get_list_settings,
		NULL,
		raw_mailbox_alloc,
		NULL,
		NULL,
	}
};

struct mailbox raw_mailbox = {
	.v = {
		index_storage_is_readonly,
		index_storage_mailbox_enable,
		index_storage_mailbox_exists,
		raw_mailbox_open,
		index_storage_mailbox_close,
		index_storage_mailbox_free,
		raw_mailbox_create,
		raw_mailbox_update,
		index_storage_mailbox_delete,
		index_storage_mailbox_rename,
		index_storage_get_status,
		index_mailbox_get_metadata,
		index_storage_set_subscribed,
		index_storage_attribute_set,
		index_storage_attribute_get,
		index_storage_attribute_iter_init,
		index_storage_attribute_iter_next,
		index_storage_attribute_iter_deinit,
		index_storage_list_index_has_changed,
		index_storage_list_index_update_sync,
		raw_storage_sync_init,
		index_mailbox_sync_next,
		index_mailbox_sync_deinit,
		NULL,
		raw_notify_changes,
		index_transaction_begin,
		index_transaction_commit,
		index_transaction_rollback,
		NULL,
		index_mail_alloc,
		index_storage_search_init,
		index_storage_search_deinit,
		index_storage_search_next_nonblock,
		index_storage_search_next_update_seq,
		index_storage_search_next_match_mail,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		mail_storage_copy,
		NULL,
		NULL,
		NULL,
		index_storage_is_inconsistent
	}
};
