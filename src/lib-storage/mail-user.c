/* Copyright (c) 2008-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "hostpid.h"
#include "ioloop.h"
#include "net.h"
#include "module-dir.h"
#include "home-expand.h"
#include "file-create-locked.h"
#include "mkdir-parents.h"
#include "safe-mkstemp.h"
#include "str.h"
#include "strescape.h"
#include "strfuncs.h"
#include "var-expand.h"
#include "settings.h"
#include "settings-parser.h"
#include "iostream-ssl.h"
#include "fs-api.h"
#include "auth-master.h"
#include "master-service.h"
#include "master-service-ssl-settings.h"
#include "dict.h"
#include "mail-storage-settings.h"
#include "mail-storage-private.h"
#include "mail-storage-service.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mailbox-list-private.h"
#include "mail-autoexpunge.h"
#include "mail-user.h"


struct mail_user_module_register mail_user_module_register = { 0 };
struct auth_master_connection *mail_user_auth_master_conn;

static void mail_user_deinit_base(struct mail_user *user)
{
	if (user->_attr_dict != NULL) {
		dict_wait(user->_attr_dict);
		dict_deinit(&user->_attr_dict);
	}
	mail_namespaces_deinit(&user->namespaces);
	mail_storage_service_user_unref(&user->service_user);
}

static void mail_user_deinit_pre_base(struct mail_user *user ATTR_UNUSED)
{
}

void mail_user_add_event_fields(struct mail_user *user)
{
	if (user->userdb_fields == NULL)
		return;
	for (unsigned int i = 0; user->userdb_fields[i] != NULL; i++) {
		const char *field = user->userdb_fields[i];
		const char *key, *value;
		t_split_key_value_eq(field, &key, &value);
		if (str_begins(key, "event_", &key))
			event_add_str(user->event, key, value);
	}
}

static void
mail_user_var_expand_callback(struct event *event,
			      const struct var_expand_table **tab_r,
			      const struct var_expand_func_table **func_tab_r)
{
	struct mail_user *user =
		event_get_ptr(event, SETTINGS_EVENT_VAR_EXPAND_FUNC_CONTEXT);
	i_assert(user != NULL);

	*tab_r = mail_user_var_expand_table(user);
	*func_tab_r = mail_user_var_expand_func_table;
}

struct mail_user *
mail_user_alloc(struct mail_storage_service_user *service_user)
{
	struct mail_user *user;
	struct event *parent_event =
		mail_storage_service_user_get_event(service_user);
	const char *username =
		mail_storage_service_user_get_username(service_user);
	i_assert(*username != '\0');

	pool_t pool = pool_alloconly_create(MEMPOOL_GROWING"mail user", 16*1024);
	user = p_new(pool, struct mail_user, 1);
	user->pool = pool;
	user->refcount = 1;
	user->service_user = service_user;
	mail_storage_service_user_ref(service_user);
	user->username = p_strdup(pool, username);
	user->set = mail_storage_service_user_get_set(service_user);
	user->service = master_service_get_name(master_service);
	user->default_normalizer = uni_utf8_to_decomposed_titlecase;
	user->session_create_time = ioloop_time;
	user->event = event_create(parent_event);
	event_add_category(user->event, &event_category_storage);
	event_add_str(user->event, "user", username);

	/* Register %variable expansion callback function for settings
	   lookups. */
	event_set_ptr(user->event, SETTINGS_EVENT_VAR_EXPAND_CALLBACK,
		      mail_user_var_expand_callback);
	event_set_ptr(user->event, SETTINGS_EVENT_VAR_EXPAND_FUNC_CONTEXT, user);

	user->v.deinit = mail_user_deinit_base;
	user->v.deinit_pre = mail_user_deinit_pre_base;
	p_array_init(&user->module_contexts, user->pool, 5);
	return user;
}

static void
mail_user_expand_plugins_envs(struct mail_user *user,
			      struct mail_storage_settings *set)
{
	const char **envs, *error;
	string_t *str;
	unsigned int i, count;

	if (!array_is_created(&set->plugin_envs))
		return;

	str = t_str_new(256);
	envs = array_get_modifiable(&set->plugin_envs, &count);
	i_assert((count % 2) == 0);
	for (i = 0; i < count; i += 2) {
		str_truncate(str, 0);
		if (var_expand_with_funcs(str, envs[i+1],
					  mail_user_var_expand_table(user),
					  mail_user_var_expand_func_table, user,
					  &error) <= 0) {
			user->error = p_strdup_printf(user->pool,
				"Failed to expand plugin setting %s = '%s': %s",
				envs[i], envs[i+1], error);
			return;
		}
		envs[i+1] = p_strdup(user->pool, str_c(str));
	}
}

int mail_user_init(struct mail_user *user, const char **error_r)
{
	const char *error;

	i_assert(!user->initialized);

	if (settings_get(user->event, &mail_storage_setting_parser_info, 0,
			 &user->_mail_set, &error) < 0)
		user->error = p_strdup(user->pool, error);
	else
		mail_user_expand_plugins_envs(user, user->_mail_set);

	user->ssl_set = p_new(user->pool, struct ssl_iostream_settings, 1);
	if (user->error == NULL &&
	    mail_storage_service_user_init_ssl_client_settings(
			user->service_user, user->pool,
			user->ssl_set, &error) < 0)
		user->error = p_strdup(user->pool, error);

	/* autocreated users for shared mailboxes need to be fully initialized
	   if they don't exist, since they're going to be used anyway */
	if (user->error == NULL || user->nonexistent) {
		user->initialized = TRUE;
		hook_mail_user_created(user);
	}

	if (user->error != NULL) {
		*error_r = t_strdup(user->error);
		return -1;
	}
	process_stat_read_start(&user->proc_stat, user->event);
	return 0;
}

void mail_user_ref(struct mail_user *user)
{
	i_assert(user->refcount > 0);

	user->refcount++;
}

static void mail_user_session_finished(struct mail_user *user)
{
	struct event *ev = user->event;
	struct process_stat *stat = &user->proc_stat;

	process_stat_read_finish(stat, ev);

	struct event_passthrough *e = event_create_passthrough(ev)->
		set_name("mail_user_session_finished")->
		add_int_nonzero("utime", stat->utime)->
		add_int_nonzero("stime", stat->stime)->
		add_int_nonzero("minor_faults", stat->minor_faults)->
		add_int_nonzero("major_faults", stat->major_faults)->
		add_int_nonzero("vol_cs", stat->vol_cs)->
		add_int_nonzero("invol_cs", stat->invol_cs)->
		add_int_nonzero("rss", stat->rss)->
		add_int_nonzero("vsz", stat->vsz)->
		add_int_nonzero("rchar", stat->rchar)->
		add_int_nonzero("wchar", stat->wchar)->
		add_int_nonzero("syscr", stat->syscr)->
		add_int_nonzero("syscw", stat->syscw);
	e_debug(e->event(), "User session is finished");
}

void mail_user_unref(struct mail_user **_user)
{
	struct mail_user *user = *_user;

	i_assert(user->refcount > 0);

	*_user = NULL;
	if (user->refcount > 1) {
		user->refcount--;
		return;
	}

	user->deinitializing = TRUE;
	if (user->creator == NULL)
		mail_user_session_finished(user);

	/* call deinit() and deinit_pre() with refcount=1, otherwise we may
	   assert-crash in mail_user_ref() that is called by some handlers. */
	T_BEGIN {
		user->v.deinit_pre(user);
		user->v.deinit(user);
	} T_END;
	settings_free(user->_mail_set);
	event_unref(&user->event);
	i_assert(user->refcount == 1);
	pool_unref(&user->pool);
}

void mail_user_deinit(struct mail_user **user)
{
	i_assert((*user)->refcount == 1);
	mail_user_unref(user);
}

struct mail_user *mail_user_find(struct mail_user *user, const char *name)
{
	struct mail_namespace *ns;

	for (ns = user->namespaces; ns != NULL; ns = ns->next) {
		if (ns->owner != NULL && strcmp(ns->owner->username, name) == 0)
			return ns->owner;
	}
	return NULL;
}

static void
mail_user_connection_init_from(struct mail_user_connection_data *conn,
	pool_t pool, const struct mail_user_connection_data *src)
{
	*conn = *src;

	if (src->local_ip != NULL && src->local_ip->family != 0) {
		conn->local_ip = p_new(pool, struct ip_addr, 1);
		*conn->local_ip = *src->local_ip;
	}
	if (src->remote_ip != NULL && src->remote_ip->family != 0) {
		conn->remote_ip = p_new(pool, struct ip_addr, 1);
		*conn->remote_ip = *src->remote_ip;
	}
	conn->local_name = p_strdup(pool, conn->local_name);
}

void mail_user_set_vars(struct mail_user *user, const char *service,
			const struct mail_user_connection_data *conn)
{
	i_assert(service != NULL);

	user->service = p_strdup(user->pool, service);
	mail_user_connection_init_from(&user->conn, user->pool, conn);
}

const struct var_expand_table *
mail_user_var_expand_table(struct mail_user *user)
{
	const char *username =
		p_strdup(user->pool, t_strcut(user->username, '@'));
	const char *domain = i_strchr_to_next(user->username, '@');
	const char *local_ip = user->conn.local_ip == NULL ? NULL :
		p_strdup(user->pool, net_ip2addr(user->conn.local_ip));
	const char *remote_ip = user->conn.remote_ip == NULL ? NULL :
		p_strdup(user->pool, net_ip2addr(user->conn.remote_ip));

	const char *auth_user, *auth_username, *auth_domain;
	if (user->auth_user == NULL) {
		auth_user = user->username;
		auth_username = username;
		auth_domain = domain;
	} else {
		auth_user = user->auth_user;
		auth_username =
			p_strdup(user->pool, t_strcut(user->auth_user, '@'));
		auth_domain = i_strchr_to_next(user->auth_user, '@');
	}

	const struct var_expand_table stack_tab[] = {
		{ 'u', user->username, "user" },
		{ 'n', username, "username" },
		{ 'd', domain, "domain" },
		{ 's', user->service, "service" },
		{ 'l', local_ip, "lip" },
		{ 'r', remote_ip, "rip" },
		{ '\0', user->session_id, "session" },
		{ '\0', auth_user, "auth_user" },
		{ '\0', auth_username, "auth_username" },
		{ '\0', auth_domain, "auth_domain" },
		{ '\0', user->set->hostname, "hostname" },
		{ '\0', user->conn.local_name, "local_name" },
		/* aliases: */
		{ '\0', local_ip, "local_ip" },
		{ '\0', remote_ip, "remote_ip" },
		/* NOTE: keep this synced with imap-hibernate's
		   imap_client_var_expand_table() */
		{ '\0', NULL, NULL }
	};
	struct var_expand_table *tab;

	tab = p_malloc(user->pool, sizeof(stack_tab));
	memcpy(tab, stack_tab, sizeof(stack_tab));

	user->var_expand_table = tab;
	return user->var_expand_table;
}

static int
mail_user_var_expand_func_home(const char *data ATTR_UNUSED, void *context,
			       const char **value_r, const char **error_r)
{
	struct mail_user *user = context;

	if (mail_user_get_home(user, value_r) <= 0) {
		*error_r = "Setting used home directory (%h) but there is no "
			"mail_home and userdb didn't return it";
		return -1;
	}
	return 1;
}

static int
mail_user_var_expand_func_userdb(const char *data, void *context,
				 const char **value_r,
				 const char **error_r ATTR_UNUSED)
{
	struct mail_user *user = context;

	*value_r = mail_storage_service_fields_var_expand(data, user->userdb_fields);
	return 1;
}

void mail_user_set_home(struct mail_user *user, const char *home)
{
	user->_home = p_strdup(user->pool, home);
	user->home_looked_up = TRUE;
}

void mail_user_add_namespace(struct mail_user *user,
			     struct mail_namespace **namespaces)
{
	struct mail_namespace **tmp, *next, *ns = *namespaces;

	for (; ns != NULL; ns = next) {
		next = ns->next;

		tmp = &user->namespaces;
		for (; *tmp != NULL; tmp = &(*tmp)->next) {
			i_assert(*tmp != ns);
			if (strlen(ns->prefix) < strlen((*tmp)->prefix))
				break;
		}
		ns->next = *tmp;
		*tmp = ns;
	}
	*namespaces = user->namespaces;

	T_BEGIN {
		hook_mail_namespaces_added(user->namespaces);
	} T_END;
}

void mail_user_drop_useless_namespaces(struct mail_user *user)
{
	struct mail_namespace *ns, *next;

	/* drop all autocreated unusable (typically shared) namespaces.
	   don't drop the autocreated prefix="" namespace that we explicitly
	   created for being the fallback namespace. */
	for (ns = user->namespaces; ns != NULL; ns = next) {
		next = ns->next;

		if (mail_namespace_is_removable(ns) && ns->prefix_len > 0)
			mail_namespace_destroy(ns);
	}
}

const char *mail_user_home_expand(struct mail_user *user, const char *path)
{
	(void)mail_user_try_home_expand(user, &path);
	return path;
}

static int mail_user_userdb_lookup_home(struct mail_user *user)
{
	struct auth_user_info info;
	struct auth_user_reply reply;
	pool_t userdb_pool;
	const char *username, *const *fields;
	int ret;

	i_assert(!user->home_looked_up);

	i_zero(&info);
	info.service = user->service;
	if (user->conn.local_ip != NULL)
		info.local_ip = *user->conn.local_ip;
	if (user->conn.remote_ip != NULL)
		info.remote_ip = *user->conn.remote_ip;
	info.local_name = user->conn.local_name;

	userdb_pool = pool_alloconly_create("userdb lookup", 2048);
	ret = auth_master_user_lookup(mail_user_auth_master_conn,
				      user->username, &info, userdb_pool,
				      &username, &fields);
	if (ret > 0) {
		const char *error;
		if (auth_user_fields_parse(fields, userdb_pool,
					   &reply, &error) < 0) {
			e_error(user->event,
				"Failed to parse credentials due to %s", error);
			ret = -1;
		} else
			user->_home = p_strdup(user->pool, reply.home);
	}
	pool_unref(&userdb_pool);
	return ret;
}

int mail_user_get_home(struct mail_user *user, const char **home_r)
{
	int ret;

	if (user->home_looked_up) {
		*home_r = user->_home;
		return user->_home != NULL ? 1 : 0;
	}

	if (mail_user_auth_master_conn == NULL) {
		/* no userdb connection. we can only use mail_home setting. */
		if (user->set->mail_home[0] != '\0')
			user->_home = user->set->mail_home;
	} else if ((ret = mail_user_userdb_lookup_home(user)) < 0) {
		/* userdb lookup failed */
		return -1;
	} else if (ret == 0) {
		/* user doesn't exist */
		user->nonexistent = TRUE;
	} else if (user->_home == NULL) {
		/* no home returned by userdb lookup, fallback to
		   mail_home setting. */
		if (user->set->mail_home[0] != '\0')
			user->_home = user->set->mail_home;
	}
	user->home_looked_up = TRUE;

	*home_r = user->_home;
	return user->_home != NULL ? 1 : 0;
}

bool mail_user_is_plugin_loaded(struct mail_user *user, struct module *module)
{
	const char *const *plugins;
	bool ret;

	T_BEGIN {
		plugins = t_strsplit_spaces(user->set->mail_plugins, ", ");
		ret = str_array_find(plugins, module_get_plugin_name(module));
	} T_END;
	return ret;
}

bool mail_user_plugin_getenv_bool(struct mail_user *user, const char *name)
{
	const struct mail_storage_settings *mail_set =
		mail_user_set_get_storage_set(user);
	return mail_user_set_plugin_getenv_bool(mail_set, name);
}

bool mail_user_set_plugin_getenv_bool(const struct mail_storage_settings *set,
				      const char *name)
{
	const char *env = mail_user_set_plugin_getenv(set, name);

	if (env == NULL)
		return FALSE;
	switch (env[0]) {
		case 'n':
		case 'N':
		case '0':
		case 'f':
		case 'F':
		return FALSE;
	}

	//any other value including empty string will be treated as TRUE.
	return TRUE;
}

const char *mail_user_plugin_getenv(struct mail_user *user, const char *name)
{
	const struct mail_storage_settings *mail_set =
		mail_user_set_get_storage_set(user);
	return mail_user_set_plugin_getenv(mail_set, name);
}

const char *mail_user_set_plugin_getenv(const struct mail_storage_settings *set,
					const char *name)
{
	const char *const *envs;
	unsigned int i, count;

	if (!array_is_created(&set->plugin_envs))
		return NULL;

	envs = array_get(&set->plugin_envs, &count);
	for (i = 0; i < count; i += 2) {
		if (strcmp(envs[i], name) == 0)
			return envs[i+1];
	}
	return NULL;
}

int mail_user_try_home_expand(struct mail_user *user, const char **pathp)
{
	const char *home, *path = *pathp;

	if (*path != '~') {
		/* no need to expand home */
		return 0;
	}

	if (mail_user_get_home(user, &home) <= 0)
		return -1;

	path = home_expand_tilde(path, home);
	if (path == NULL)
		return -1;

	*pathp = path;
	return 0;
}

void mail_user_set_get_temp_prefix(string_t *dest,
				   const struct mail_user_settings *set)
{
	str_append(dest, set->mail_temp_dir);
	str_append(dest, "/dovecot.");
	str_append(dest, master_service_get_name(master_service));
	str_append_c(dest, '.');
}

const char *mail_user_get_volatile_dir(struct mail_user *user)
{
	struct mailbox_list *inbox_list =
		mail_namespace_find_inbox(user->namespaces)->list;

	return inbox_list->set.volatile_dir;
}

int mail_user_lock_file_create(struct mail_user *user, const char *lock_fname,
			       unsigned int lock_secs,
			       struct file_lock **lock_r, const char **error_r)
{
	const char *home, *path;
	int ret;

	if ((ret = mail_user_get_home(user, &home)) < 0) {
		/* home lookup failed - shouldn't really happen */
		*error_r = "Failed to lookup home directory";
		errno = EINVAL;
		return -1;
	}
	if (ret == 0) {
		*error_r = "User has no home directory";
		errno = EINVAL;
		return -1;
	}

	const struct mail_storage_settings *mail_set =
		mail_user_set_get_storage_set(user);
	struct file_create_settings lock_set = {
		.lock_timeout_secs = lock_secs,
		.lock_settings = {
			.lock_method = mail_set->parsed_lock_method,
		},
	};
	struct mailbox_list *inbox_list =
		mail_namespace_find_inbox(user->namespaces)->list;
	if (inbox_list->set.volatile_dir == NULL)
		path = t_strdup_printf("%s/%s", home, lock_fname);
	else {
		path = t_strdup_printf("%s/%s", inbox_list->set.volatile_dir,
				       lock_fname);
		lock_set.mkdir_mode = 0700;
	}
	return mail_storage_lock_create(path, &lock_set, mail_set, lock_r, error_r);
}

void mail_user_get_anvil_session(struct mail_user *user,
				 struct master_service_anvil_session *session_r)
{
	i_zero(session_r);
	session_r->username = user->username;
	session_r->service_name = master_service_get_name(master_service);
	session_r->alt_usernames = mail_user_get_alt_usernames(user);
	if (user->conn.remote_ip != NULL)
		session_r->ip = *user->conn.remote_ip;
}

const char *const *mail_user_get_alt_usernames(struct mail_user *user)
{
	if (user->_alt_usernames != NULL)
		return user->_alt_usernames;
	if (user->userdb_fields == NULL) {
		user->_alt_usernames = p_new(user->pool, const char *, 1);
		return user->_alt_usernames;
	}

	ARRAY_TYPE(const_string) alt_usernames;
	t_array_init(&alt_usernames, 4);
	for (unsigned int i = 0; user->userdb_fields[i] != NULL; i++) {
		const char *key, *value;
		if (t_split_key_value_eq(user->userdb_fields[i], &key, &value) &&
		    *value != '\0' && str_begins_with(key, "user_")) {
			array_append(&alt_usernames, &key, 1);
			array_append(&alt_usernames, &value, 1);
		}
	}
	array_append_zero(&alt_usernames);

	unsigned int count;
	user->_alt_usernames = array_get_copy(&alt_usernames, user->pool, &count);
	return user->_alt_usernames;
}

static void
mail_user_try_load_class_plugin(struct mail_user *user, const char *name)
{
	struct module_dir_load_settings mod_set;
	struct module *module;
	size_t name_len = strlen(name);

	i_zero(&mod_set);
	mod_set.abi_version = DOVECOT_ABI_VERSION;
	mod_set.binary_name = master_service_get_name(master_service);
	mod_set.setting_name = "<built-in storage lookup>";
	mod_set.require_init_funcs = TRUE;
	mod_set.debug = event_want_debug(user->event);

	mail_storage_service_modules =
		module_dir_load_missing(mail_storage_service_modules,
					user->set->mail_plugin_dir,
					name, &mod_set);
	/* initialize the module (and only this module!) immediately so that
	   the class gets registered */
	for (module = mail_storage_service_modules; module != NULL; module = module->next) {
		if (strncmp(module->name, name, name_len) == 0 &&
		    strcmp(module->name + name_len, "_plugin") == 0) {
			if (!module->initialized) {
				module->initialized = TRUE;
				module->init(module);
			}
			break;
		}
	}
}

struct mail_storage *
mail_user_get_storage_class(struct mail_user *user, const char *name)
{
	struct mail_storage *storage;

	storage = mail_storage_find_class(name);
	if (storage == NULL || storage->v.alloc != NULL)
		return storage;

	/* it's implemented by a plugin. load it and check again. */
	mail_user_try_load_class_plugin(user, name);

	storage = mail_storage_find_class(name);
	if (storage != NULL && storage->v.alloc == NULL) {
		e_error(user->event, "Storage driver '%s' exists as a stub, "
			"but its plugin couldn't be loaded", name);
		return NULL;
	}
	return storage;
}

struct mail_user *mail_user_dup(struct mail_user *user)
{
	struct mail_user *user2;

	user2 = mail_user_alloc(user->service_user);
	if (user->_home != NULL)
		mail_user_set_home(user2, user->_home);
	mail_user_set_vars(user2, user->service, &user->conn);
	user2->uid = user->uid;
	user2->gid = user->gid;
	user2->anonymous = user->anonymous;
	user2->admin = user->admin;
	user2->auth_mech = p_strdup(user2->pool, user->auth_mech);
	user2->auth_token = p_strdup(user2->pool, user->auth_token);
	user2->auth_user = p_strdup(user2->pool, user->auth_user);
	user2->session_id = p_strdup(user2->pool, user->session_id);
	user2->session_create_time = user->session_create_time;
	user2->userdb_fields = user->userdb_fields == NULL ? NULL :
		p_strarray_dup(user2->pool, user->userdb_fields);
	return user2;
}

void mail_user_init_fs_settings(struct mail_user *user,
				struct fs_settings *fs_set,
				struct ssl_iostream_settings *ssl_set_r)
{
	fs_set->event_parent = user->event;
	fs_set->username = user->username;
	fs_set->session_id = user->session_id;
	fs_set->base_dir = user->set->base_dir;
	fs_set->temp_dir = user->set->mail_temp_dir;
	fs_set->debug = event_want_debug(user->event);
	fs_set->enable_timing = user->stats_enabled;

	fs_set->ssl_client_set = ssl_set_r;
	*ssl_set_r = *user->ssl_set;
}

static int
mail_user_home_mkdir_try_ns(struct mail_namespace *ns, const char *home)
{
	const enum mailbox_list_path_type types[] = {
		MAILBOX_LIST_PATH_TYPE_DIR,
		MAILBOX_LIST_PATH_TYPE_ALT_DIR,
		MAILBOX_LIST_PATH_TYPE_CONTROL,
		MAILBOX_LIST_PATH_TYPE_INDEX,
		MAILBOX_LIST_PATH_TYPE_INDEX_PRIVATE,
		MAILBOX_LIST_PATH_TYPE_INDEX_CACHE,
		MAILBOX_LIST_PATH_TYPE_LIST_INDEX,
	};
	size_t home_len = strlen(home);
	const char *path;

	for (unsigned int i = 0; i < N_ELEMENTS(types); i++) {
		if (!mailbox_list_get_root_path(ns->list, types[i], &path))
			continue;
		if (strncmp(path, home, home_len) == 0 &&
		    (path[home_len] == '\0' || path[home_len] == '/')) {
			return mailbox_list_mkdir_root(ns->list, path,
						       types[i]) < 0 ? -1 : 1;
		}
	}
	return 0;
}

int mail_user_home_mkdir(struct mail_user *user)
{
	struct mail_namespace *ns;
	const char *home;
	int ret;

	if ((ret = mail_user_get_home(user, &home)) <= 0) {
		/* If user has no home directory, just return success. */
		return ret;
	}

	/* Try to create the home directory by creating the root directory for
	   a namespace that exists under the home. This way we end up in the
	   special mkdir() code in mailbox_list_try_mkdir_root_parent().
	   Start from INBOX, since that's usually the correct place. */
	ns = mail_namespace_find_inbox(user->namespaces);
	if ((ret = mail_user_home_mkdir_try_ns(ns, home)) != 0)
		return ret < 0 ? -1 : 0;
	/* try other namespaces */
	for (ns = user->namespaces; ns != NULL; ns = ns->next) {
		if ((ns->flags & NAMESPACE_FLAG_INBOX_USER) != 0) {
			/* already tried the INBOX namespace */
			continue;
		}
		if ((ret = mail_user_home_mkdir_try_ns(ns, home)) != 0)
			return ret < 0 ? -1 : 0;
	}
	/* fallback to a safe mkdir() with 0700 mode */
	if (mkdir_parents(home, 0700) < 0 && errno != EEXIST) {
		e_error(user->event, "mkdir_parents(%s) failed: %m", home);
		return -1;
	}
	return 0;
}

const struct dict_op_settings *
mail_user_get_dict_op_settings(struct mail_user *user)
{
	if (user->dict_op_set == NULL) {
		user->dict_op_set = p_new(user->pool, struct dict_op_settings, 1);
		user->dict_op_set->username = p_strdup(user->pool, user->username);
		if (mail_user_get_home(user, &user->dict_op_set->home_dir) <= 0)
			user->dict_op_set->home_dir = NULL;
	}
	return user->dict_op_set;
}

static const struct var_expand_func_table mail_user_var_expand_func_table_arr[] = {
	{ "h", mail_user_var_expand_func_home },
	{ "home", mail_user_var_expand_func_home },
	{ "userdb", mail_user_var_expand_func_userdb },
	{ NULL, NULL }
};
const struct var_expand_func_table *mail_user_var_expand_func_table =
	mail_user_var_expand_func_table_arr;
