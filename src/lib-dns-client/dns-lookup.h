#ifndef DNS_LOOKUP_H
#define DNS_LOOKUP_H

#include <netdb.h>

#define DNS_CLIENT_SOCKET_NAME "dns-client"

#ifndef EAI_CANCELED
#  define EAI_CANCELED INT_MIN
#endif

struct dns_lookup;
struct dns_client;

struct dns_client_settings {
	pool_t pool;
	const char *dns_client_socket_path;
	const char *base_dir;
	unsigned int timeout_msecs;
};

struct dns_client_parameters {
	/* the idle_timeout_msecs works only with the dns_client_* API.
	   0 = disconnect immediately */
	unsigned int idle_timeout_msecs;
	/* Non-zero enables caching for the client, is not supported with
	   dns_lookup() or dns_lookup_ptr(). Note that DNS TTL is ignored. */
	unsigned int cache_ttl_secs;
};

struct dns_lookup_result {
	/* all is ok if ret=0, otherwise it contains net_gethosterror()
	   compatible error code. error string is always set if ret != 0. */
	int ret;
	const char *error;

	/* how many milliseconds the lookup took. */
	unsigned int msecs;

	/* for IP lookup: */
	unsigned int ips_count; /* guaranteed to be >0 on success */
	const struct ip_addr *ips;
	/* for PTR lookup: */
	const char *name;
};

typedef void dns_lookup_callback_t(const struct dns_lookup_result *result,
				   void *context);

/* Do asynchronous DNS lookup via dns-client UNIX socket. Returns 0 if lookup
   started, -1 if there was an error communicating with the UNIX socket.
   When failing with -1, the callback is called before returning from the
   function. */
int dns_lookup(const char *host, const struct dns_client_parameters *params,
	       struct event *event_parent, dns_lookup_callback_t *callback,
	       void *context, struct dns_lookup **lookup_r) ATTR_NULL(4);
#define dns_lookup(host, params, event_parent, callback, context, lookup_r) \
	dns_lookup(host - \
		CALLBACK_TYPECHECK(callback, void (*)( \
			const struct dns_lookup_result *, typeof(context))), \
		params, event_parent, (dns_lookup_callback_t *)callback, context, lookup_r)
int dns_lookup_ptr(const struct ip_addr *ip,
		   const struct dns_client_parameters *params,
		   struct event *event_parent,
		   dns_lookup_callback_t *callback, void *context,
		   struct dns_lookup **lookup_r) ATTR_NULL(4);
#define dns_lookup_ptr(host, params, event_parent, callback, context, lookup_r) \
	dns_lookup_ptr(host - \
		CALLBACK_TYPECHECK(callback, void (*)( \
			const struct dns_lookup_result *, typeof(context))), \
		params, event_parent, \
		(dns_lookup_callback_t *)callback, context, lookup_r)
/* Abort the DNS lookup without calling the callback. */
void dns_lookup_abort(struct dns_lookup **lookup);

void dns_lookup_switch_ioloop(struct dns_lookup *lookup);

/* Alternative API for clients that need to do multiple DNS lookups. */
int dns_client_init(const struct dns_client_parameters *params,
		    struct event *event_parent,
		    struct dns_client **client_r,
		    const char **error_r);
void dns_client_deinit(struct dns_client **client);

/* Connect immediately to the dns-lookup socket. */
int dns_client_connect(struct dns_client *client, const char **error_r);
int dns_client_lookup(struct dns_client *client, const char *host,
		      struct event *event,
		      dns_lookup_callback_t *callback, void *context,
		      struct dns_lookup **lookup_r) ATTR_NULL(4);
#define dns_client_lookup(client, host, event, callback, context, lookup_r) \
	dns_client_lookup(client, host - \
		CALLBACK_TYPECHECK(callback, void (*)( \
			const struct dns_lookup_result *, typeof(context))), \
		event, (dns_lookup_callback_t *)callback, context, lookup_r)
int dns_client_lookup_ptr(struct dns_client *client, const struct ip_addr *ip,
			  struct event *event,
			  dns_lookup_callback_t *callback, void *context,
			  struct dns_lookup **lookup_r) ATTR_NULL(4);
#define dns_client_lookup_ptr(client, ip, event, callback, context, lookup_r) \
	dns_client_lookup_ptr(client, ip - \
		CALLBACK_TYPECHECK(callback, void (*)( \
			const struct dns_lookup_result *, typeof(context))), \
		event, (dns_lookup_callback_t *)callback, context, lookup_r)

/* Returns true if the DNS client has any pending queries */
bool dns_client_has_pending_queries(struct dns_client *client);

void dns_client_switch_ioloop(struct dns_client *client);

extern const struct setting_parser_info dns_client_setting_parser_info;

#endif
