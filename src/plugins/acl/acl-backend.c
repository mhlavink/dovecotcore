/* Copyright (c) 2006-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "llist.h"
#include "array.h"
#include "hash.h"
#include "sort.h"
#include "settings.h"
#include "mail-storage-settings.h"
#include "mailbox-list-private.h"
#include "mail-namespace.h"
#include "mail-user.h"
#include "acl-cache.h"
#include "acl-api-private.h"


extern const struct acl_backend_vfuncs acl_backend_vfile;
static struct acl_backend_entry {
	struct acl_backend_entry *prev, *next;
	const struct acl_backend_vfuncs *v;
} *acl_backend_list_head = NULL;

struct event_category event_category_acl = {
	.name = "acl",
};


static const char *const *owner_mailbox_rights = all_mailbox_rights;
static const char *const non_owner_mailbox_rights[] = { NULL };

static struct acl_backend_entry *acl_backend_find(const char *name)
{
	struct acl_backend_entry *be = acl_backend_list_head;

	while (be != NULL) {
		if (strcmp(be->v->name, name) == 0)
			break;
		be = be->next;
	}

	if (be == NULL)
		 i_fatal("Unknown ACL backend: %s", name);
	return be;
}


int acl_backend_init_auto(struct mailbox_list *list, struct acl_backend **backend_r,
			  const char **error_r)
{
	const struct acl_settings *set;
	struct event *event = event_create(list->event);
	event_add_category(event, &event_category_acl);
	event_set_append_log_prefix(event, "acl: ");

	/* try to get settings again */
	if (settings_get(event, &acl_setting_parser_info, 0, &set, error_r) < 0) {
		event_unref(&event);
		return -1;
	}

	if (*set->acl_driver == '\0') {
		e_debug(event, "No acl_driver setting - ACLs are disabled");
		settings_free(set);
		event_unref(&event);
		return 0;
	}

	struct acl_backend_entry *be = acl_backend_find(set->acl_driver);
	struct acl_backend *backend = be->v->alloc();

	const char *owner_username = list->ns->user->username;
	backend->username = set->acl_user;
	if (*backend->username == '\0') {
		backend->username = owner_username;
		backend->owner = TRUE;
	} else
		backend->owner = strcmp(backend->username, owner_username) == 0;
	if (list->ns->type != MAIL_NAMESPACE_TYPE_PRIVATE)
		backend->owner = FALSE;

	backend->v = be->v;
	backend->list = list;
	backend->set = set;
	backend->event = event;

	e_debug(backend->event, "initializing backend %s", backend->v->name);
	e_debug(backend->event, "acl username = %s", backend->username);
	e_debug(backend->event, "owner = %s", backend->owner ? "yes" : "no");
	e_debug(backend->event, "ignore = %s", set->acl_ignore ? "yes" : "no");
	if (event_want_debug(backend->event) && array_is_created(&set->acl_groups)) {
		const char *group;
		array_foreach_elem(&set->acl_groups, group)
			e_debug(backend->event, "group added: %s", group);
	}

	if (backend->v->init(backend, error_r) < 0) {
		*error_r = t_strdup_printf("acl %s: %s", backend->v->name, *error_r);
		acl_backend_deinit(&backend);
		return -1;
	}

	backend->default_rights = backend->owner ? owner_mailbox_rights :
		non_owner_mailbox_rights;
	backend->default_aclmask =
		acl_cache_mask_init(backend->cache, backend->pool,
				    backend->default_rights);

	*backend_r = backend;

	return 1;
}

struct acl_backend *
acl_backend_init(const char *data, struct mailbox_list *list,
		 const char *acl_username, const struct acl_settings *set,
		 bool owner)
{
	struct mail_user *user = mailbox_list_get_user(list);
	struct acl_backend_entry *be;
	struct acl_backend *backend;
	const char *be_name;

	e_debug(user->event, "acl: initializing backend with data: %s", data);
	e_debug(user->event, "acl: acl username = %s", acl_username);
	e_debug(user->event, "acl: owner = %d", owner ? 1 : 0);

	be_name = strchr(data, ':');
	if (be_name == NULL)
		be_name = data;
	else {
		be_name = t_strdup_until(data, be_name);
		data = be_name++;
	}

	be = acl_backend_find(be_name);

	backend = be->v->alloc();
	backend->event = event_create(user->event);
	event_add_category(backend->event, &event_category_acl);

	backend->v = be->v;
	backend->list = list;
	backend->username = p_strdup(backend->pool, acl_username);
	backend->owner = owner;

	if (event_want_debug(user->event) && array_is_created(&set->acl_groups)) {
		const char *group;
		array_foreach_elem(&set->acl_groups, group) {
			e_debug(user->event, "acl: group added: %s", group);
		}
	}

	backend->set = set;

	T_BEGIN {
		if (backend->v->init_legacy(backend, data) < 0)
			i_fatal("acl: backend %s init failed with data: %s",
				backend->v->name, data);
	} T_END;

	backend->default_rights = owner ? owner_mailbox_rights :
		non_owner_mailbox_rights;
	backend->default_aclmask =
		acl_cache_mask_init(backend->cache, backend->pool,
				    backend->default_rights);
	return backend;
}

void acl_backend_deinit(struct acl_backend **_backend)
{
	struct acl_backend *backend = *_backend;

	if (backend == NULL)
		return;
	*_backend = NULL;

	acl_object_deinit(&backend->default_aclobj);
	acl_cache_deinit(&backend->cache);
	event_unref(&backend->event);
	settings_free(backend->set);
	backend->v->deinit(backend);
}

const char *acl_backend_get_acl_username(struct acl_backend *backend)
{
	return backend->username;
}

bool acl_backend_user_is_authenticated(struct acl_backend *backend)
{
	return backend->username != NULL;
}

bool acl_backend_user_is_owner(struct acl_backend *backend)
{
	return backend->owner;
}

bool acl_backend_user_name_equals(struct acl_backend *backend,
				  const char *username)
{
	if (backend->username == NULL) {
		/* anonymous user never matches */
		return FALSE;
	}

	return strcmp(backend->username, username) == 0;
}

bool acl_backend_user_is_in_group(struct acl_backend *backend,
				  const char *group_name)
{
	unsigned int group_count;
	const char *const *groups = array_get(&backend->set->acl_groups, &group_count);
	return i_bsearch(group_name, groups, group_count,
			 sizeof(const char *), bsearch_strcmp) != NULL;
}

bool acl_backend_rights_match_me(struct acl_backend *backend,
				 const struct acl_rights *rights)
{
	switch (rights->id_type) {
	case ACL_ID_ANYONE:
		return TRUE;
	case ACL_ID_AUTHENTICATED:
		return acl_backend_user_is_authenticated(backend);
	case ACL_ID_GROUP:
	case ACL_ID_GROUP_OVERRIDE:
		return acl_backend_user_is_in_group(backend, rights->identifier);
	case ACL_ID_USER:
		return acl_backend_user_name_equals(backend, rights->identifier);
	case ACL_ID_OWNER:
		return acl_backend_user_is_owner(backend);
	case ACL_ID_TYPE_COUNT:
		break;
	}
	i_unreached();
}

unsigned int acl_backend_lookup_right(struct acl_backend *backend,
				      const char *right)
{
	return acl_cache_right_lookup(backend->cache, right);
}

struct acl_object *acl_backend_get_default_object(struct acl_backend *backend)
{
	struct mail_namespace *ns = mailbox_list_get_namespace(backend->list);
	const char *default_name = "";

	if (backend->default_aclobj != NULL)
		return backend->default_aclobj;

	if (backend->set->acl_defaults_from_inbox) {
		if (ns->type == MAIL_NAMESPACE_TYPE_PRIVATE ||
		    ns->type == MAIL_NAMESPACE_TYPE_SHARED)
			default_name = "INBOX";
	}
	backend->default_aclobj =
		acl_object_init_from_name(backend, default_name);
	return backend->default_aclobj;
}

int acl_backend_get_default_rights(struct acl_backend *backend,
				   const struct acl_mask **mask_r)
{
	struct acl_object *aclobj = acl_backend_get_default_object(backend);

	if (backend->v->object_refresh_cache(aclobj) < 0)
		return -1;

	*mask_r = acl_cache_get_my_rights(backend->cache, aclobj->name);
	if (*mask_r == NULL)
		*mask_r = backend->default_aclmask;
	return 0;
}

void acl_backend_register(const struct acl_backend_vfuncs *v)
{
	struct acl_backend_entry *be = i_new(struct acl_backend_entry, 1);
	be->v = v;
	DLLIST_PREPEND(&acl_backend_list_head, be);
}

void acl_backend_unregister(const char *name)
{
	struct acl_backend_entry *be = acl_backend_find(name);
	i_assert(be != NULL);
	DLLIST_REMOVE(&acl_backend_list_head, be);
	i_free(be);
}
