/* Copyright (c) 2005-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "hash-format.h"
#include "var-expand.h"
#include "unichar.h"
#include "hostpid.h"
#include "settings.h"
#include "settings-parser.h"
#include "message-address.h"
#include "message-header-parser.h"
#include "smtp-address.h"
#include "mail-index.h"
#include "mail-user.h"
#include "mail-namespace.h"
#include "mail-storage-private.h"
#include "mail-storage-settings.h"
#include "iostream-ssl.h"

static bool mail_storage_settings_ext_check(struct event *event, void *_set, pool_t pool, const char **error_r);
static bool namespace_settings_ext_check(struct event *event, void *_set, pool_t pool, const char **error_r);
static bool mailbox_settings_check(void *_set, pool_t pool, const char **error_r);
static bool mail_user_settings_check(void *_set, pool_t pool, const char **error_r);
static bool mail_user_settings_expand_check(void *_set, pool_t pool ATTR_UNUSED, const char **error_r);

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct mail_storage_settings)

static const struct setting_define mail_storage_setting_defines[] = {
	DEF(STR_VARS, mail_location),
	{ .type = SET_ALIAS, .key = "mail" },
	DEF(STR_VARS, mail_attachment_fs),
	DEF(STR_VARS, mail_attachment_dir),
	DEF(STR, mail_attachment_hash),
	DEF(SIZE, mail_attachment_min_size),
	DEF(STR, mail_attachment_detection_options),
	DEF(STR_VARS, mail_attribute_dict),
	DEF(UINT, mail_prefetch_count),
	DEF(STR, mail_cache_fields),
	DEF(STR, mail_always_cache_fields),
	DEF(STR, mail_never_cache_fields),
	DEF(STR, mail_server_comment),
	DEF(STR, mail_server_admin),
	DEF(TIME_HIDDEN, mail_cache_unaccessed_field_drop),
	DEF(SIZE_HIDDEN, mail_cache_record_max_size),
	DEF(UINT_HIDDEN, mail_cache_max_header_name_length),
	DEF(UINT_HIDDEN, mail_cache_max_headers_count),
	DEF(SIZE_HIDDEN, mail_cache_max_size),
	DEF(UINT_HIDDEN, mail_cache_min_mail_count),
	DEF(SIZE_HIDDEN, mail_cache_purge_min_size),
	DEF(UINT_HIDDEN, mail_cache_purge_delete_percentage),
	DEF(UINT_HIDDEN, mail_cache_purge_continued_percentage),
	DEF(UINT_HIDDEN, mail_cache_purge_header_continue_count),
	DEF(SIZE_HIDDEN, mail_index_rewrite_min_log_bytes),
	DEF(SIZE_HIDDEN, mail_index_rewrite_max_log_bytes),
	DEF(SIZE_HIDDEN, mail_index_log_rotate_min_size),
	DEF(SIZE_HIDDEN, mail_index_log_rotate_max_size),
	DEF(TIME_HIDDEN, mail_index_log_rotate_min_age),
	DEF(TIME_HIDDEN, mail_index_log2_max_age),
	DEF(TIME, mailbox_idle_check_interval),
	DEF(UINT, mail_max_keyword_length),
	DEF(TIME, mail_max_lock_timeout),
	DEF(TIME, mail_temp_scan_interval),
	DEF(UINT, mail_vsize_bg_after_count),
	DEF(UINT, mail_sort_max_read_count),
	DEF(BOOL, mail_save_crlf),
	DEF(ENUM, mail_fsync),
	DEF(BOOL, mmap_disable),
	DEF(BOOL, dotlock_use_excl),
	DEF(BOOL, mail_nfs_storage),
	DEF(BOOL, mail_nfs_index),
	DEF(BOOL, mailbox_list_index),
	DEF(BOOL, mailbox_list_index_very_dirty_syncs),
	DEF(BOOL, mailbox_list_index_include_inbox),
	DEF(BOOL, mail_full_filesystem_access),
	DEF(BOOL, maildir_stat_dirs),
	DEF(BOOL, mail_shared_explicit_inbox),
	DEF(ENUM, lock_method),
	DEF(STR, pop3_uidl_format),

	DEF(STR, recipient_delimiter),

	{ .type = SET_FILTER_ARRAY, .key = "namespace",
	   .offset = offsetof(struct mail_storage_settings, namespaces),
	   .filter_array_field_name = "namespace_name" },
	{ .type = SET_STRLIST, .key = "plugin",
	  .offset = offsetof(struct mail_storage_settings, plugin_envs) },

	SETTING_DEFINE_LIST_END
};

const struct mail_storage_settings mail_storage_default_settings = {
	.mail_location = "",
	.mail_attachment_fs = "sis posix",
	.mail_attachment_dir = "",
	.mail_attachment_hash = "%{sha1}",
	.mail_attachment_min_size = 1024*128,
	.mail_attachment_detection_options = "",
	.mail_attribute_dict = "",
	.mail_prefetch_count = 0,
	.mail_cache_fields = "flags",
	.mail_always_cache_fields = "",
	.mail_never_cache_fields = "imap.envelope",
	.mail_server_comment = "",
	.mail_server_admin = "",
	.mail_cache_min_mail_count = 0,
	.mail_cache_unaccessed_field_drop = 60*60*24*30,
	.mail_cache_record_max_size = 64 * 1024,
	.mail_cache_max_header_name_length = 100,
	.mail_cache_max_headers_count = 100,
	.mail_cache_max_size = 1024 * 1024 * 1024,
	.mail_cache_purge_min_size = 32 * 1024,
	.mail_cache_purge_delete_percentage = 20,
	.mail_cache_purge_continued_percentage = 200,
	.mail_cache_purge_header_continue_count = 4,
	.mail_index_rewrite_min_log_bytes = 8 * 1024,
	.mail_index_rewrite_max_log_bytes = 128 * 1024,
	.mail_index_log_rotate_min_size = 32 * 1024,
	.mail_index_log_rotate_max_size = 1024 * 1024,
	.mail_index_log_rotate_min_age = 5 * 60,
	.mail_index_log2_max_age = 3600 * 24 * 2,
	.mailbox_idle_check_interval = 30,
	.mail_max_keyword_length = 50,
	.mail_max_lock_timeout = 0,
	.mail_temp_scan_interval = 7*24*60*60,
	.mail_vsize_bg_after_count = 0,
	.mail_sort_max_read_count = 0,
	.mail_save_crlf = FALSE,
	.mail_fsync = "optimized:never:always",
	.mmap_disable = FALSE,
	.dotlock_use_excl = TRUE,
	.mail_nfs_storage = FALSE,
	.mail_nfs_index = FALSE,
	.mailbox_list_index = TRUE,
	.mailbox_list_index_very_dirty_syncs = FALSE,
	.mailbox_list_index_include_inbox = FALSE,
	.mail_full_filesystem_access = FALSE,
	.maildir_stat_dirs = FALSE,
	.mail_shared_explicit_inbox = FALSE,
	.lock_method = "fcntl:flock:dotlock",
	.pop3_uidl_format = "%08Xu%08Xv",

	.recipient_delimiter = "+",

	.namespaces = ARRAY_INIT,
	.plugin_envs = ARRAY_INIT,
};

const struct setting_parser_info mail_storage_setting_parser_info = {
	.name = "mail_storage",

	.defines = mail_storage_setting_defines,
	.defaults = &mail_storage_default_settings,

	.struct_size = sizeof(struct mail_storage_settings),
	.pool_offset1 = 1 + offsetof(struct mail_storage_settings, pool),
	.ext_check_func = mail_storage_settings_ext_check,
};

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type("mailbox_"#name, name, struct mailbox_settings)

static const struct setting_define mailbox_setting_defines[] = {
	DEF(STR, name),
	{ .type = SET_ENUM, .key = "mailbox_auto",
	  .offset = offsetof(struct mailbox_settings, autocreate) } ,
	DEF(STR, special_use),
	DEF(STR, driver),
	DEF(STR, comment),
	DEF(TIME, autoexpunge),
	DEF(UINT, autoexpunge_max_mails),

	SETTING_DEFINE_LIST_END
};

const struct mailbox_settings mailbox_default_settings = {
	.name = "",
	.autocreate = MAILBOX_SET_AUTO_NO":"
		MAILBOX_SET_AUTO_CREATE":"
		MAILBOX_SET_AUTO_SUBSCRIBE,
	.special_use = "",
	.driver = "",
	.comment = "",
	.autoexpunge = 0,
	.autoexpunge_max_mails = 0
};

const struct setting_parser_info mailbox_setting_parser_info = {
	.name = "mailbox",

	.defines = mailbox_setting_defines,
	.defaults = &mailbox_default_settings,

	.struct_size = sizeof(struct mailbox_settings),
	.pool_offset1 = 1 + offsetof(struct mailbox_settings, pool),

	.check_func = mailbox_settings_check
};

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type("namespace_"#name, name, struct mail_namespace_settings)

static const struct setting_define mail_namespace_setting_defines[] = {
	DEF(STR, name),
	DEF(ENUM, type),
	DEF(STR, separator),
	DEF(STR_VARS, prefix),
	DEF(STR_VARS, location),
	DEF(STR_VARS, alias_for),

	DEF(BOOL, inbox),
	DEF(BOOL, hidden),
	DEF(ENUM, list),
	DEF(BOOL, subscriptions),
	DEF(BOOL, ignore_on_failure),
	DEF(BOOL, disabled),
	DEF(UINT, order),

	{ .type = SET_FILTER_ARRAY, .key = "mailbox",
	   .offset = offsetof(struct mail_namespace_settings, mailboxes),
	   .filter_array_field_name = "mailbox_name" },

	SETTING_DEFINE_LIST_END
};

const struct mail_namespace_settings mail_namespace_default_settings = {
	.name = "",
	.type = "private:shared:public",
	.separator = "",
	.prefix = "",
	.location = "",
	.alias_for = NULL,

	.inbox = FALSE,
	.hidden = FALSE,
	.list = "yes:no:children",
	.subscriptions = TRUE,
	.ignore_on_failure = FALSE,
	.disabled = FALSE,
	.order = 0,

	.mailboxes = ARRAY_INIT
};

const struct setting_parser_info mail_namespace_setting_parser_info = {
	.name = "mail_namespace",

	.defines = mail_namespace_setting_defines,
	.defaults = &mail_namespace_default_settings,

	.struct_size = sizeof(struct mail_namespace_settings),
	.pool_offset1 = 1 + offsetof(struct mail_namespace_settings, pool),

	.ext_check_func = namespace_settings_ext_check
};

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct mail_user_settings)

static const struct setting_define mail_user_setting_defines[] = {
	DEF(STR, base_dir),
	DEF(STR, auth_socket_path),
	DEF(STR_VARS, mail_temp_dir),
	DEF(BOOL, mail_debug),

	DEF(STR, mail_uid),
	DEF(STR, mail_gid),
	DEF(STR_VARS, mail_home),
	DEF(STR_VARS, mail_chroot),
	DEF(STR, mail_access_groups),
	DEF(STR, mail_privileged_group),
	DEF(STR, valid_chroot_dirs),

	DEF(UINT, first_valid_uid),
	DEF(UINT, last_valid_uid),
	DEF(UINT, first_valid_gid),
	DEF(UINT, last_valid_gid),

	DEF(STR, mail_plugins),
	DEF(STR, mail_plugin_dir),

	DEF(STR_VARS, mail_log_prefix),

	DEF(STR, hostname),
	DEF(STR_VARS, postmaster_address),

	SETTING_DEFINE_LIST_END
};

static const struct mail_user_settings mail_user_default_settings = {
	.base_dir = PKG_RUNDIR,
	.auth_socket_path = "auth-userdb",
	.mail_temp_dir = "/tmp",
	.mail_debug = FALSE,

	.mail_uid = "",
	.mail_gid = "",
	.mail_home = "",
	.mail_chroot = "",
	.mail_access_groups = "",
	.mail_privileged_group = "",
	.valid_chroot_dirs = "",

	.first_valid_uid = 500,
	.last_valid_uid = 0,
	.first_valid_gid = 1,
	.last_valid_gid = 0,

	.mail_plugins = "",
	.mail_plugin_dir = MODULEDIR,

	.mail_log_prefix = "%s(%u)<%{pid}><%{session}>: ",

	.hostname = "",
	.postmaster_address = "postmaster@%{if;%d;ne;;%d;%{hostname}}",
};

const struct setting_parser_info mail_user_setting_parser_info = {
	.name = "mail_user",

	.defines = mail_user_setting_defines,
	.defaults = &mail_user_default_settings,

	.struct_size = sizeof(struct mail_user_settings),
	.pool_offset1 = 1 + offsetof(struct mail_user_settings, pool),
	.check_func = mail_user_settings_check,
#ifndef CONFIG_BINARY
	.expand_check_func = mail_user_settings_expand_check,
#endif
};

const struct mail_storage_settings *
mail_user_set_get_storage_set(struct mail_user *user)
{
	i_assert(user->_mail_set != NULL);
	return user->_mail_set;
}

static void
fix_base_path(struct mail_user_settings *set, pool_t pool, const char **str)
{
	if (*str != NULL && **str != '\0' && **str != '/')
		*str = p_strconcat(pool, set->base_dir, "/", *str, NULL);
}

/* <settings checks> */
static bool mail_cache_fields_parse(const char *key, const char *value,
				    const char **error_r)
{
	const char *const *arr;

	for (arr = t_strsplit_spaces(value, " ,"); *arr != NULL; arr++) {
		const char *name = *arr;

		if (str_begins_icase(name, "hdr.", &name) &&
		    !message_header_name_is_valid(name)) {
			*error_r = t_strdup_printf(
				"Invalid %s: %s is not a valid header name",
				key, name);
			return FALSE;
		}
	}
	return TRUE;
}

static int
mail_storage_settings_find_ns_by_prefix(struct event *event,
					struct mail_storage_settings *set,
					const char *ns_prefix,
					const struct mail_namespace_settings **ns_r,
					const char **error_r)
{
	const struct mail_namespace_settings *ns;
	const char *ns_name, *error;

	array_foreach_elem(&set->namespaces, ns_name) {
		if (settings_get_filter(event, "namespace", ns_name,
					&mail_namespace_setting_parser_info,
					SETTINGS_GET_FLAG_NO_CHECK |
					SETTINGS_GET_FLAG_FAKE_EXPAND,
					&ns, &error) < 0) {
			*error_r = t_strdup_printf(
				"Failed to get namespace %s: %s",
				ns_name, error);
			return -1;
		}
		if (strcmp(ns->prefix, ns_prefix) == 0) {
			*ns_r = ns;
			return 0;
		}
		settings_free(ns);
	}
	*ns_r = NULL;
	return 0;
}

static bool
mail_storage_settings_check_namespaces(struct event *event,
				       struct mail_storage_settings *set,
				       const char **error_r)
{
	const struct mail_namespace_settings *ns, *alias_ns;
	const char *ns_name, *error;

	if (!array_is_created(&set->namespaces))
		return TRUE;

	array_foreach_elem(&set->namespaces, ns_name) {
		if (settings_get_filter(event, "namespace", ns_name,
					&mail_namespace_setting_parser_info,
					SETTINGS_GET_FLAG_FAKE_EXPAND,
					&ns, &error) < 0) {
			*error_r = t_strdup_printf(
				"Failed to get namespace %s: %s",
				ns_name, error);
			return FALSE;
		}

		if (ns->disabled) {
			settings_free(ns);
			continue;
		}

		if (ns->parsed_have_special_use_mailboxes)
			set->parsed_have_special_use_mailboxes = TRUE;

		if (ns->alias_for == NULL) {
			settings_free(ns);
			continue;
		}

		if (mail_storage_settings_find_ns_by_prefix(event, set,
				ns->alias_for, &alias_ns, error_r) < 0) {
			settings_free(ns);
			return FALSE;
		}
		if (alias_ns == NULL) {
			*error_r = t_strdup_printf(
				"Namespace '%s': alias_for points to "
				"unknown namespace: %s",
				ns->prefix != NULL ? ns->prefix : "",
				ns->alias_for);
			settings_free(ns);
			return FALSE;
		}
		if (alias_ns->alias_for != NULL) {
			*error_r = t_strdup_printf(
				"Namespace '%s': alias_for chaining isn't "
				"allowed: %s -> %s",
				ns->prefix != NULL ? ns->prefix : "",
				ns->alias_for, alias_ns->alias_for);
			settings_free(alias_ns);
			settings_free(ns);
			return FALSE;
		}
		settings_free(alias_ns);
		settings_free(ns);
	}
	return TRUE;
}

static bool
mail_storage_settings_ext_check(struct event *event, void *_set, pool_t pool,
				const char **error_r)
{
	struct mail_storage_settings *set = _set;
	struct hash_format *format;
	const char *p, *value, *error;
	bool uidl_format_ok;
	char c;

#ifndef CONFIG_BINARY
	i_assert(set->mail_location[0] == SETTING_STRVAR_UNEXPANDED[0] ||
		 set->mail_location[0] == SETTING_STRVAR_EXPANDED[0]);
	set->unexpanded_mail_location = set->mail_location;
#endif

	if (set->mailbox_idle_check_interval == 0) {
		*error_r = "mailbox_idle_check_interval must not be 0";
		return FALSE;
	}

	if (strcmp(set->mail_fsync, "optimized") == 0)
		set->parsed_fsync_mode = FSYNC_MODE_OPTIMIZED;
	else if (strcmp(set->mail_fsync, "never") == 0)
		set->parsed_fsync_mode = FSYNC_MODE_NEVER;
	else if (strcmp(set->mail_fsync, "always") == 0)
		set->parsed_fsync_mode = FSYNC_MODE_ALWAYS;
	else {
		*error_r = t_strdup_printf("Unknown mail_fsync: %s",
					   set->mail_fsync);
		return FALSE;
	}

	if (set->mail_nfs_index && !set->mmap_disable) {
		*error_r = "mail_nfs_index=yes requires mmap_disable=yes";
		return FALSE;
	}
	if (set->mail_nfs_index &&
	    set->parsed_fsync_mode != FSYNC_MODE_ALWAYS) {
		*error_r = "mail_nfs_index=yes requires mail_fsync=always";
		return FALSE;
	}

	if (!file_lock_method_parse(set->lock_method,
				    &set->parsed_lock_method)) {
		*error_r = t_strdup_printf("Unknown lock_method: %s",
					   set->lock_method);
		return FALSE;
	}

	if (set->mail_cache_max_size > 1024 * 1024 * 1024) {
		*error_r = "mail_cache_max_size can't be over 1 GB";
		return FALSE;
	}
	if (set->mail_cache_purge_delete_percentage > 100) {
		*error_r = "mail_cache_purge_delete_percentage can't be over 100";
		return FALSE;
	}

	uidl_format_ok = FALSE;
	for (p = set->pop3_uidl_format; *p != '\0'; p++) {
		if (p[0] != '%' || p[1] == '\0')
			continue;

		c = var_get_key(++p);
		switch (c) {
		case 'v':
		case 'u':
		case 'm':
		case 'f':
		case 'g':
			uidl_format_ok = TRUE;
			break;
		case '%':
			break;
		default:
			*error_r = t_strdup_printf(
				"Unknown pop3_uidl_format variable: %%%c", c);
			return FALSE;
		}
	}
	if (!uidl_format_ok) {
		*error_r = "pop3_uidl_format setting doesn't contain any "
			"%% variables.";
		return FALSE;
	}

	if (strchr(set->mail_attachment_hash, '/') != NULL) {
		*error_r = "mail_attachment_hash setting "
			"must not contain '/' characters";
		return FALSE;
	}
	if (hash_format_init(set->mail_attachment_hash, &format, &error) < 0) {
		*error_r = t_strconcat("Invalid mail_attachment_hash setting: ",
				       error, NULL);
		return FALSE;
	}
	if (strchr(set->mail_attachment_hash, '-') != NULL) {
		*error_r = "mail_attachment_hash setting "
			"must not contain '-' characters";
		return FALSE;
	}
	hash_format_deinit_free(&format);

	// FIXME: check set->mail_server_admin syntax (RFC 5464, Section 6.2.2)

	/* parse mail_attachment_indicator_options */
	if (*set->mail_attachment_detection_options != '\0') {
		ARRAY_TYPE(const_string) content_types;
		p_array_init(&content_types, pool, 2);

		const char *const *options =
			t_strsplit_spaces(set->mail_attachment_detection_options, " ");

		while(*options != NULL) {
			const char *opt = *options;

			if (strcmp(opt, "add-flags") == 0 ||
			    strcmp(opt, "add-flags-on-save") == 0) {
				set->parsed_mail_attachment_detection_add_flags = TRUE;
			} else if (strcmp(opt, "no-flags-on-fetch") == 0) {
				set->parsed_mail_attachment_detection_no_flags_on_fetch = TRUE;
			} else if (strcmp(opt, "exclude-inlined") == 0) {
				set->parsed_mail_attachment_exclude_inlined = TRUE;
			} else if (str_begins(opt, "content-type=", &value)) {
				value = p_strdup(pool, value);
				array_push_back(&content_types, &value);
			} else {
				*error_r = t_strdup_printf("mail_attachment_detection_options: "
					"Unknown option: %s", opt);
				return FALSE;
			}
			options++;
		}

		array_append_zero(&content_types);
		set->parsed_mail_attachment_content_type_filter = array_front(&content_types);
	}

	if (!mail_cache_fields_parse("mail_cache_fields",
				     set->mail_cache_fields, error_r))
		return FALSE;
	if (!mail_cache_fields_parse("mail_always_cache_fields",
				     set->mail_always_cache_fields, error_r))
		return FALSE;
	if (!mail_cache_fields_parse("mail_never_cache_fields",
				     set->mail_never_cache_fields, error_r))
		return FALSE;

	if (!mail_storage_settings_check_namespaces(event, set, error_r))
		return FALSE;
	return TRUE;
}

static int
namespace_have_special_use_mailboxes(struct event *event,
				     struct mail_namespace_settings *ns,
				     const char **error_r)
{
	struct mailbox_settings *box_set;
	const char *box_name, *error;
	int ret = 0;

	if (!array_is_created(&ns->mailboxes))
		return 0;

	event = event_create(event);
	event_add_str(event, "namespace", ns->name);
	array_foreach_elem(&ns->mailboxes, box_name) {
		if (settings_get_filter(event,
					SETTINGS_EVENT_MAILBOX_NAME_WITHOUT_PREFIX,
					box_name,
					&mailbox_setting_parser_info,
					SETTINGS_GET_FLAG_NO_CHECK |
					SETTINGS_GET_FLAG_FAKE_EXPAND,
					&box_set, &error) < 0) {
			*error_r = t_strdup_printf(
				"Failed to get mailbox %s: %s",
				box_name, error);
			break;
		}
		bool have_special_use = box_set->special_use[0] != '\0';
		settings_free(box_set);
		if (have_special_use) {
			ret = 1;
			break;
		}
	}
	event_unref(&event);
	return ret;
}

static bool namespace_settings_ext_check(struct event *event,
					 void *_set, pool_t pool ATTR_UNUSED,
					 const char **error_r)
{
	struct mail_namespace_settings *ns = _set;
	const char *name;
	int ret;

	name = ns->prefix != NULL ? ns->prefix : "";

#ifndef CONFIG_BINARY
	i_assert(ns->location[0] == SETTING_STRVAR_UNEXPANDED[0] ||
		 ns->location[0] == SETTING_STRVAR_EXPANDED[0]);
	ns->unexpanded_location = ns->location;
#endif

	if (ns->separator[0] != '\0' && ns->separator[1] != '\0') {
		*error_r = t_strdup_printf("Namespace '%s': "
			"Hierarchy separator must be only one character long",
			name);
		return FALSE;
	}
	if (!uni_utf8_str_is_valid(name)) {
		*error_r = t_strdup_printf("Namespace prefix not valid UTF8: %s",
					   name);
		return FALSE;
	}

	ret = namespace_have_special_use_mailboxes(event, ns, error_r);
	if (ret < 0)
		return FALSE;
	if (ret > 0)
		ns->parsed_have_special_use_mailboxes = TRUE;
	return TRUE;
}

static bool mailbox_special_use_exists(const char *name)
{
	if (name[0] != '\\')
		return FALSE;
	name++;

	if (strcasecmp(name, "All") == 0)
		return TRUE;
	if (strcasecmp(name, "Archive") == 0)
		return TRUE;
	if (strcasecmp(name, "Drafts") == 0)
		return TRUE;
	if (strcasecmp(name, "Flagged") == 0)
		return TRUE;
	if (strcasecmp(name, "Important") == 0)
		return TRUE;
	if (strcasecmp(name, "Junk") == 0)
		return TRUE;
	if (strcasecmp(name, "Sent") == 0)
		return TRUE;
	if (strcasecmp(name, "Trash") == 0)
		return TRUE;
	return FALSE;
}

static void
mailbox_special_use_check(struct mailbox_settings *set, pool_t pool)
{
	const char *const *uses, *str;
	unsigned int i;

	uses = t_strsplit_spaces(set->special_use, " ");
	for (i = 0; uses[i] != NULL; i++) {
		if (!mailbox_special_use_exists(uses[i])) {
			i_warning("mailbox %s: special_use label %s is not an "
				  "RFC-defined label - allowing anyway",
				  set->name, uses[i]);
		}
	}
	/* make sure there are no extra spaces */
	str = t_strarray_join(uses, " ");
	if (strcmp(str, set->special_use) != 0)
		set->special_use = p_strdup(pool, str);
}

static bool mailbox_settings_check(void *_set, pool_t pool,
				   const char **error_r)
{
	struct mailbox_settings *set = _set;

	if (!uni_utf8_str_is_valid(set->name)) {
		*error_r = t_strdup_printf("mailbox %s: name isn't valid UTF-8",
					   set->name);
		return FALSE;
	}
	if (*set->special_use != '\0') {
		mailbox_special_use_check(set, pool);
	}
	return TRUE;
}

static bool mail_user_settings_check(void *_set, pool_t pool ATTR_UNUSED,
				     const char **error_r ATTR_UNUSED)
{
	struct mail_user_settings *set = _set;

#ifndef CONFIG_BINARY
	fix_base_path(set, pool, &set->auth_socket_path);

	if (*set->hostname == '\0')
		set->hostname = p_strdup(pool, my_hostdomain());
	if (set->postmaster_address[0] == SETTING_STRVAR_UNEXPANDED[0] &&
	    set->postmaster_address[1] == '\0') {
		/* check for valid looking fqdn in hostname */
		if (strchr(set->hostname, '.') == NULL) {
			*error_r = "postmaster_address setting not given";
			return FALSE;
		}
		set->postmaster_address =
			p_strconcat(pool, SETTING_STRVAR_UNEXPANDED,
				    "postmaster@", set->hostname, NULL);
	}
#else
	if (*set->mail_plugins != '\0' &&
	    faccessat(AT_FDCWD, set->mail_plugin_dir, R_OK | X_OK, AT_EACCESS) < 0) {
		*error_r = t_strdup_printf(
			"mail_plugin_dir: access(%s) failed: %m",
			set->mail_plugin_dir);
		return FALSE;
	}
#endif
	return TRUE;
}

#ifndef CONFIG_BINARY
static bool parse_postmaster_address(const char *address, pool_t pool,
				     struct mail_user_settings *set,
				     const char **error_r) ATTR_NULL(3)
{
	struct message_address *addr;
	struct smtp_address *smtp_addr;

	addr = message_address_parse(pool,
		(const unsigned char *)address,
		strlen(address), 2, 0);
	if (addr == NULL || addr->domain == NULL || addr->invalid_syntax ||
	    smtp_address_create_from_msg(pool, addr, &smtp_addr) < 0) {
		*error_r = t_strdup_printf(
			"invalid address `%s' specified for the "
			"postmaster_address setting", address);
		return FALSE;
	}
	if (addr->next != NULL) {
		*error_r = "more than one address specified for the "
			"postmaster_address setting";
		return FALSE;
	}
	if (addr->name == NULL || *addr->name == '\0')
		addr->name = "Postmaster";
	if (set != NULL) {
		set->_parsed_postmaster_address = addr;
		set->_parsed_postmaster_address_smtp = smtp_addr;
	}
	return TRUE;
}

static bool
mail_user_settings_expand_check(void *_set, pool_t pool,
				const char **error_r ATTR_UNUSED)
{
	struct mail_user_settings *set = _set;
	const char *error;

	/* Parse if possible. Perform error handling later. */
	(void)parse_postmaster_address(set->postmaster_address, pool,
				       set, &error);
	return TRUE;
}
#endif

/* </settings checks> */

static void
get_postmaster_address_error(const struct mail_user_settings *set,
			     const char **error_r)
{
	if (parse_postmaster_address(set->postmaster_address,
				     pool_datastack_create(), NULL, error_r))
		i_panic("postmaster_address='%s' parsing succeeded unexpectedly after it had already failed",
			set->postmaster_address);
}

bool mail_user_set_get_postmaster_address(const struct mail_user_settings *set,
					  const struct message_address **address_r,
					  const char **error_r)
{
	*address_r = set->_parsed_postmaster_address;
	if (*address_r != NULL)
		return TRUE;
	/* parsing failed - do it again to get the error */
	get_postmaster_address_error(set, error_r);
	return FALSE;
}

bool mail_user_set_get_postmaster_smtp(const struct mail_user_settings *set,
				       const struct smtp_address **address_r,
				       const char **error_r)
{
	*address_r = set->_parsed_postmaster_address_smtp;
	if (*address_r != NULL)
		return TRUE;
	/* parsing failed - do it again to get the error */
	get_postmaster_address_error(set, error_r);
	return FALSE;
}
