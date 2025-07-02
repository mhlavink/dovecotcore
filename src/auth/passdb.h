#ifndef PASSDB_H
#define PASSDB_H

#define IS_VALID_PASSWD(pass) \
	((pass)[0] != '\0' && (pass)[0] != '*' && (pass)[0] != '!')

#define STATIC_PASS_SCHEME "PLAIN"

struct auth_request;
struct auth_passdb_settings;
struct passdb_module;

enum passdb_result {
	PASSDB_RESULT_INTERNAL_FAILURE = -1,
	PASSDB_RESULT_SCHEME_NOT_AVAILABLE = -2,

	PASSDB_RESULT_USER_UNKNOWN = -3,
	PASSDB_RESULT_USER_DISABLED = -4,
	PASSDB_RESULT_PASS_EXPIRED = -5,
	PASSDB_RESULT_NEXT = -6,

	PASSDB_RESULT_PASSWORD_MISMATCH = 0,
	PASSDB_RESULT_OK = 1
};

typedef void verify_plain_callback_t(enum passdb_result result,
				     struct auth_request *request);
typedef void verify_plain_continue_callback_t(struct auth_request *request,
					      verify_plain_callback_t *callback);
typedef void lookup_credentials_callback_t(enum passdb_result result,
					   const unsigned char *credentials,
					   size_t size,
					   struct auth_request *request);
typedef void set_credentials_callback_t(bool success,
					struct auth_request *request);

struct passdb_module_interface {
	const char *name;

	/* Create a new passdb_module based on the settings looked up via the
	   given event. */
	int (*preinit)(pool_t pool, struct event *event,
		       struct passdb_module **module_r, const char **error_r);
	void (*init)(struct passdb_module *module);
	void (*deinit)(struct passdb_module *module);

	/* Check if plaintext password matches */
	void (*verify_plain)(struct auth_request *request, const char *password,
			     verify_plain_callback_t *callback);

	/* Return authentication credentials, set in
	   auth_request->credentials. */
	void (*lookup_credentials)(struct auth_request *request,
				   lookup_credentials_callback_t *callback);

	/* Update credentials */
	void (*set_credentials)(struct auth_request *request,
				const char *new_credentials,
				set_credentials_callback_t *callback);
};

struct passdb_module {
	/* The default caching key for this module, or NULL if caching isn't
	   wanted. This is updated by settings in auth_passdb. */
	const char *default_cache_key;
	/* Default password scheme for this module.
	   If default_cache_key is set, must not be NULL. */
	const char *default_pass_scheme;

	/* If blocking is set to TRUE, use child processes to access
	   this passdb. */
	bool blocking;
        /* id is used by blocking passdb to identify the passdb */
	unsigned int id;

	/* number of time init() has been called */
	int init_refcount;

	struct passdb_module_interface iface;
};

const char *passdb_result_to_string(enum passdb_result result);

/* Try to get credentials in wanted scheme (request->credentials_scheme) from
   given input. Returns FALSE if this wasn't possible (unknown scheme,
   conversion not possible or invalid credentials).

   If wanted scheme is "", the credentials are returned as-is without any
   checks. This is useful mostly just to see if there exist any credentials
   at all. */
bool passdb_get_credentials(struct auth_request *auth_request,
			    const char *input, const char *input_scheme,
			    const unsigned char **credentials_r,
			    size_t *size_r);

void passdb_handle_credentials(enum passdb_result result,
			       const char *password, const char *scheme,
			       lookup_credentials_callback_t *callback,
                               struct auth_request *auth_request);

struct passdb_module *
passdb_preinit(pool_t pool, struct event *event,
	       const struct auth_passdb_settings *set);
void passdb_init(struct passdb_module *passdb);
void passdb_deinit(struct passdb_module *passdb);

void passdb_register_module(struct passdb_module_interface *iface);
void passdb_unregister_module(struct passdb_module_interface *iface);

void passdbs_init(void);
void passdbs_deinit(void);

#include "auth-request.h"

#endif
