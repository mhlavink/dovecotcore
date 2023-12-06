/* Copyright (c) 2008-2018 Dovecot authors, see the included COPYING redis */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "istream.h"
#include "ostream.h"
#include "connection.h"
#include "settings.h"
#include "settings-parser.h"
#include "dict-private.h"

#define DICT_USERNAME_SEPARATOR '/'

enum redis_input_state {
	/* expecting +OK reply for AUTH */
	REDIS_INPUT_STATE_AUTH,
	/* expecting +OK reply for SELECT */
	REDIS_INPUT_STATE_SELECT,
	/* expecting $-1 / $<size> followed by GET reply */
	REDIS_INPUT_STATE_GET,
	/* expecting +QUEUED */
	REDIS_INPUT_STATE_MULTI,
	/* expecting +OK reply for DISCARD */
	REDIS_INPUT_STATE_DISCARD,
	/* expecting *<nreplies> */
	REDIS_INPUT_STATE_EXEC,
	/* expecting EXEC reply */
	REDIS_INPUT_STATE_EXEC_REPLY
};

struct redis_connection {
	struct connection conn;
	struct redis_dict *dict;

	string_t *last_reply;
	unsigned int bytes_left;
	bool value_not_found;
	bool value_received;
};

struct redis_dict_reply {
	unsigned int reply_count;
	dict_transaction_commit_callback_t *callback;
	void *context;
};

struct redis_dict {
	struct dict dict;
	struct dict_redis_settings *set;

	struct redis_connection conn;
	struct timeout *to;

	ARRAY(enum redis_input_state) input_states;
	ARRAY(struct redis_dict_reply) replies;
	char *last_error;

	bool connected;
	bool transaction_open;
	bool db_id_set;
};

struct redis_dict_transaction_context {
	struct dict_transaction_context ctx;
	unsigned int cmd_count;
	char *error;
};

struct dict_redis_settings {
	pool_t pool;

	const char *dict_redis_socket_path;
	const char *dict_redis_host;
	in_port_t dict_redis_port;
	const char *dict_redis_key_prefix;
	const char *dict_redis_password;
	unsigned int dict_redis_db_id;
	unsigned int dict_redis_expire;
	unsigned int dict_redis_request_timeout;

	struct ip_addr dict_redis_ip;
};

static bool dict_redis_settings_check(void *_set, pool_t pool, const char **error_r);

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct dict_redis_settings)
static const struct setting_define dict_redis_setting_defines[] = {
	DEF(STR, dict_redis_socket_path),
	DEF(STR, dict_redis_host),
	DEF(IN_PORT, dict_redis_port),
	DEF(STR, dict_redis_key_prefix),
	DEF(STR, dict_redis_password),
	DEF(UINT, dict_redis_db_id),
	DEF(TIME, dict_redis_expire),
	DEF(TIME_MSECS, dict_redis_request_timeout),

	SETTING_DEFINE_LIST_END
};
static const struct dict_redis_settings dict_redis_default_settings = {
	.dict_redis_socket_path = "",
	.dict_redis_host = "127.0.0.1",
	.dict_redis_port = 6379,
	.dict_redis_key_prefix = "",
	.dict_redis_password = "",
	.dict_redis_db_id = 0,
	.dict_redis_expire = 0,
	.dict_redis_request_timeout = 30 * 1000,
};
const struct setting_parser_info dict_redis_setting_parser_info = {
	.name = "dict_redis",

	.defines = dict_redis_setting_defines,
	.defaults = &dict_redis_default_settings,

	.struct_size = sizeof(struct dict_redis_settings),
	.pool_offset1 = 1 + offsetof(struct dict_redis_settings, pool),
	.check_func = dict_redis_settings_check,
};

static struct connection_list *redis_connections;

static void redis_dict_wait(struct dict *_dict);

static void
redis_input_state_add(struct redis_dict *dict, enum redis_input_state state)
{
	array_push_back(&dict->input_states, &state);
}

static void redis_input_state_remove(struct redis_dict *dict)
{
	array_pop_front(&dict->input_states);
}

static void redis_reply_callback(struct redis_connection *conn,
				 const struct redis_dict_reply *reply,
				 const struct dict_commit_result *result)
{
	i_assert(reply->callback != NULL);
	if (conn->dict->dict.prev_ioloop != NULL)
		io_loop_set_current(conn->dict->dict.prev_ioloop);
	reply->callback(result, reply->context);
	if (conn->dict->dict.prev_ioloop != NULL)
		io_loop_set_current(conn->dict->dict.ioloop);
}

static void
redis_disconnected(struct redis_connection *conn, const char *reason)
{
	const struct dict_commit_result result = {
		DICT_COMMIT_RET_FAILED,
		/* t_strdup() in case reason points to istream, which gets
		   freed by connection_disconnect() */
		t_strdup(reason)
	};
	const struct redis_dict_reply *reply;

	if (conn->dict->last_error == NULL)
		conn->dict->last_error = i_strdup(reason);

	conn->dict->db_id_set = FALSE;
	conn->dict->connected = FALSE;
	connection_disconnect(&conn->conn);

	array_foreach(&conn->dict->replies, reply)
		redis_reply_callback(conn, reply, &result);
	array_clear(&conn->dict->replies);
	array_clear(&conn->dict->input_states);
	timeout_remove(&conn->dict->to);

	if (conn->dict->dict.ioloop != NULL)
		io_loop_stop(conn->dict->dict.ioloop);
}

static void redis_conn_destroy(struct connection *_conn)
{
	struct redis_connection *conn = (struct redis_connection *)_conn;

	redis_disconnected(conn, connection_disconnect_reason(_conn));
}

static void redis_dict_wait_timeout(struct redis_dict *dict)
{
	const char *reason = t_strdup_printf(
		"redis: Commit timed out in %u.%03u secs",
		dict->set->dict_redis_request_timeout / 1000,
		dict->set->dict_redis_request_timeout % 1000);
	redis_disconnected(&dict->conn, reason);
}

static const char *redis_wait(struct redis_dict *dict)
{
	i_assert(dict->dict.ioloop == NULL);

	i_free(dict->last_error);
	dict->dict.prev_ioloop = current_ioloop;
	dict->dict.ioloop = io_loop_create();
	if (dict->to != NULL)
		dict->to = io_loop_move_timeout(&dict->to);
	else {
		dict->to = timeout_add(dict->set->dict_redis_request_timeout,
				       redis_dict_wait_timeout, dict);
	}
	connection_switch_ioloop(&dict->conn.conn);

	do {
		io_loop_run(dict->dict.ioloop);
	} while (array_count(&dict->input_states) > 0);

	timeout_remove(&dict->to);
	io_loop_set_current(dict->dict.prev_ioloop);
	connection_switch_ioloop(&dict->conn.conn);
	io_loop_set_current(dict->dict.ioloop);
	io_loop_destroy(&dict->dict.ioloop);
	dict->dict.prev_ioloop = NULL;
	const char *error = t_strdup(dict->last_error);
	i_free(dict->last_error);
	return error;
}

static int redis_input_get(struct redis_connection *conn, const char **error_r)
{
	const unsigned char *data;
	size_t size;
	const char *line;

	if (conn->bytes_left == 0) {
		/* read the size first */
		line = i_stream_next_line(conn->conn.input);
		if (line == NULL)
			return 0;
		if (strcmp(line, "$-1") == 0) {
			conn->value_received = TRUE;
			conn->value_not_found = TRUE;
			if (conn->dict->dict.ioloop != NULL)
				io_loop_stop(conn->dict->dict.ioloop);
			redis_input_state_remove(conn->dict);
			return 1;
		}
		if (line[0] != '$' || str_to_uint(line+1, &conn->bytes_left) < 0) {
			*error_r = t_strdup_printf(
				"redis: Unexpected input (wanted $size): %s", line);
			return -1;
		}
		conn->bytes_left += 2; /* include trailing CRLF */
	}

	data = i_stream_get_data(conn->conn.input, &size);
	if (size > conn->bytes_left)
		size = conn->bytes_left;
	str_append_data(conn->last_reply, data, size);

	conn->bytes_left -= size;
	i_stream_skip(conn->conn.input, size);

	if (conn->bytes_left > 0)
		return 0;

	/* reply fully read - drop trailing CRLF */
	conn->value_received = TRUE;
	str_truncate(conn->last_reply, str_len(conn->last_reply)-2);

	if (conn->dict->dict.ioloop != NULL)
		io_loop_stop(conn->dict->dict.ioloop);
	redis_input_state_remove(conn->dict);
	return 1;
}

static int
redis_conn_input_more(struct redis_connection *conn, const char **error_r)
{
	struct redis_dict *dict = conn->dict;
	struct redis_dict_reply *reply;
	const enum redis_input_state *states;
	enum redis_input_state state;
	unsigned int count, num_replies;
	const char *line;

	states = array_get(&dict->input_states, &count);
	if (count == 0) {
		line = i_stream_next_line(conn->conn.input);
		if (line == NULL)
			return 0;
		*error_r = t_strdup_printf(
			"redis: Unexpected input (expected nothing): %s", line);
		return -1;
	}
	state = states[0];
	if (state == REDIS_INPUT_STATE_GET)
		return redis_input_get(conn, error_r);

	line = i_stream_next_line(conn->conn.input);
	if (line == NULL)
		return 0;

	if (dict->to != NULL)
		timeout_reset(dict->to);

	redis_input_state_remove(dict);
	switch (state) {
	case REDIS_INPUT_STATE_GET:
		i_unreached();
	case REDIS_INPUT_STATE_AUTH:
	case REDIS_INPUT_STATE_SELECT:
	case REDIS_INPUT_STATE_MULTI:
	case REDIS_INPUT_STATE_DISCARD:
		if (line[0] != '+')
			break;
		return 1;
	case REDIS_INPUT_STATE_EXEC:
		if (line[0] != '*' || str_to_uint(line+1, &num_replies) < 0)
			break;

		reply = array_front_modifiable(&dict->replies);
		i_assert(reply->reply_count > 0);
		if (reply->reply_count != num_replies) {
			*error_r = t_strdup_printf(
				"redis: EXEC expected %u replies, not %u",
				reply->reply_count, num_replies);
			return -1;
		}
		return 1;
	case REDIS_INPUT_STATE_EXEC_REPLY:
		if (*line != '+' && *line != ':')
			break;
		/* success, just ignore the actual reply */
		reply = array_front_modifiable(&dict->replies);
		i_assert(reply->reply_count > 0);
		if (--reply->reply_count == 0) {
			const struct dict_commit_result result = {
				DICT_COMMIT_RET_OK, NULL
			};
			redis_reply_callback(conn, reply, &result);
			array_pop_front(&dict->replies);
			/* if we're running in a dict-ioloop, we're handling a
			   synchronous commit and need to stop now */
			if (array_count(&dict->replies) == 0) {
				timeout_remove(&dict->to);
				if (conn->dict->dict.ioloop != NULL)
					io_loop_stop(conn->dict->dict.ioloop);
			}
		}
		return 1;
	}
	str_truncate(dict->conn.last_reply, 0);
	str_append(dict->conn.last_reply, line);
	*error_r = t_strdup_printf("redis: Unexpected input (state=%d): %s", state, line);
	return -1;
}

static void redis_conn_input(struct connection *_conn)
{
	struct redis_connection *conn = (struct redis_connection *)_conn;
	const char *error = NULL;
	int ret;

	switch (i_stream_read(_conn->input)) {
	case 0:
		return;
	case -1:
		redis_disconnected(conn, i_stream_get_error(_conn->input));
		return;
	default:
		break;
	}

	while ((ret = redis_conn_input_more(conn, &error)) > 0) ;
	if (ret < 0) {
		i_assert(error != NULL);
		redis_disconnected(conn, error);
	}
}

static void redis_conn_connected(struct connection *_conn, bool success)
{
	struct redis_connection *conn = (struct redis_connection *)_conn;

	if (!success) {
		e_error(conn->conn.event, "connect() failed: %m");
	} else {
		conn->dict->connected = TRUE;
	}
	if (conn->dict->dict.ioloop != NULL)
		io_loop_stop(conn->dict->dict.ioloop);
}

static const struct connection_settings redis_conn_set = {
	.input_max_size = SIZE_MAX,
	.output_max_size = SIZE_MAX,
	.client = TRUE
};

static const struct connection_vfuncs redis_conn_vfuncs = {
	.destroy = redis_conn_destroy,
	.input = redis_conn_input,
	.client_connected = redis_conn_connected
};

static const char *redis_escape_username(const char *username)
{
	const char *p;
	string_t *str = t_str_new(64);

	for (p = username; *p != '\0'; p++) {
		switch (*p) {
		case DICT_USERNAME_SEPARATOR:
			str_append(str, "\\-");
			break;
		case '\\':
			str_append(str, "\\\\");
			break;
		default:
			str_append_c(str, *p);
		}
	}
	return str_c(str);
}

static bool dict_redis_settings_check(void *_set, pool_t pool ATTR_UNUSED,
				      const char **error_r)
{
	struct dict_redis_settings *set = _set;

	if (net_addr2ip(set->dict_redis_host, &set->dict_redis_ip) < 0) {
		*error_r = "dict_redis_host is not a valid IP";
		return FALSE;
	}
	return TRUE;
}

static struct dict *
redis_dict_init_common(const struct dict *dict_driver, struct event *event,
		       struct dict_redis_settings *set)
{
	if (redis_connections == NULL) {
		redis_connections =
			connection_list_init(&redis_conn_set,
					     &redis_conn_vfuncs);
	}

	struct redis_dict *dict = i_new(struct redis_dict, 1);
	dict->set = set;
	dict->conn.conn.event_parent = event;

	if (set->dict_redis_socket_path[0] != '\0') {
		connection_init_client_unix(redis_connections, &dict->conn.conn,
					    set->dict_redis_socket_path);
	} else {
		connection_init_client_ip(redis_connections, &dict->conn.conn,
					  NULL, &set->dict_redis_ip,
					  set->dict_redis_port);
	}
	event_set_append_log_prefix(dict->conn.conn.event, "redis: ");
	dict->dict = *dict_driver;
	dict->conn.last_reply = str_new(default_pool, 256);
	dict->conn.dict = dict;

	i_array_init(&dict->input_states, 4);
	i_array_init(&dict->replies, 4);

	return &dict->dict;
}

static int
redis_dict_init(const struct dict *dict_driver, struct event *event,
		struct dict **dict_r, const char **error_r)
{
	struct dict_redis_settings *set;

	if (settings_get(event, &dict_redis_setting_parser_info, 0,
			 &set, error_r) < 0)
		return -1;
	*dict_r = redis_dict_init_common(dict_driver, event, set);
	return 0;
}

static int
redis_dict_init_legacy(struct dict *dict_driver, const char *uri,
		       const struct dict_legacy_settings *legacy_set,
		       struct dict **dict_r, const char **error_r)
{
	pool_t pool = pool_alloconly_create("dict_redis_settings", 128);
	struct dict_redis_settings *set =
		p_new(pool, struct dict_redis_settings, 1);
	*set = dict_redis_default_settings;
	set->pool = pool;
	if (net_addr2ip(set->dict_redis_host, &set->dict_redis_ip) < 0)
		i_unreached();

	const char *const *args = t_strsplit(uri, ":");
	const char *value;
	int ret = 0;
	for (; *args != NULL; args++) {
		if (str_begins(*args, "path=", &value)) {
			set->dict_redis_socket_path = p_strdup(pool, value);
		} else if (str_begins(*args, "host=", &value)) {
			if (net_addr2ip(value, &set->dict_redis_ip) < 0) {
				*error_r = t_strdup_printf("Invalid IP: %s",
							   value);
				ret = -1;
			} else {
				set->dict_redis_host = p_strdup(pool, value);
			}
		} else if (str_begins(*args, "port=", &value)) {
			if (net_str2port(value, &set->dict_redis_port) < 0) {
				*error_r = t_strdup_printf("Invalid port: %s",
							   value);
				ret = -1;
			}
		} else if (str_begins(*args, "prefix=", &value)) {
			set->dict_redis_key_prefix = p_strdup(pool, value);
		} else if (str_begins(*args, "db=", &value)) {
			if (str_to_uint(value, &set->dict_redis_db_id) < 0) {
				*error_r = t_strdup_printf(
					"Invalid db number: %s", value);
				ret = -1;
			}
		} else if (str_begins(*args, "expire_secs=", &value)) {
			if (str_to_uint(value, &set->dict_redis_expire) < 0 ||
			    set->dict_redis_expire == 0) {
				*error_r = t_strdup_printf(
					"Invalid expire_secs: %s", value);
				ret = -1;
			}
		} else if (str_begins(*args, "timeout_msecs=", &value)) {
			if (str_to_uint(value, &set->dict_redis_request_timeout) < 0) {
				*error_r = t_strdup_printf(
					"Invalid timeout_msecs: %s", value);
				ret = -1;
			}
		} else if (str_begins(*args, "password=", &value)) {
			set->dict_redis_password = p_strdup(pool, value);
		} else {
			*error_r = t_strdup_printf("Unknown parameter: %s",
						   *args);
			ret = -1;
		}
	}
	if (ret < 0) {
		pool_unref(&pool);
		return -1;
	}

	*dict_r = redis_dict_init_common(dict_driver, legacy_set->event_parent,
					 set);
	return 0;
}

static void redis_dict_deinit(struct dict *_dict)
{
	struct redis_dict *dict = (struct redis_dict *)_dict;

	if (array_count(&dict->input_states) > 0) {
		i_assert(dict->connected);
		redis_dict_wait(_dict);
	}
	i_assert(dict->to == NULL); /* wait triggers the timeout */

	connection_deinit(&dict->conn.conn);
	str_free(&dict->conn.last_reply);
	array_free(&dict->replies);
	array_free(&dict->input_states);
	settings_free(dict->set);
	i_free(dict->last_error);
	i_free(dict);

	if (redis_connections->connections == NULL)
		connection_list_deinit(&redis_connections);
}

static void redis_dict_wait(struct dict *_dict)
{
	struct redis_dict *dict = (struct redis_dict *)_dict;

	if (array_count(&dict->input_states) > 0) {
		/* any errors are already logged by the callbacks */
		(void)redis_wait(dict);
	}
}

static void redis_dict_lookup_timeout(struct redis_dict *dict)
{
	const char *reason = t_strdup_printf(
		"redis: Lookup timed out in %u.%03u secs",
		dict->set->dict_redis_request_timeout / 1000,
		dict->set->dict_redis_request_timeout % 1000);
	redis_disconnected(&dict->conn, reason);
}

static const char *
redis_dict_get_full_key(struct redis_dict *dict, const char *username,
			const char *key)
{
	const char *username_sp = strchr(username, DICT_USERNAME_SEPARATOR);

	if (str_begins(key, DICT_PATH_SHARED, &key))
		;
	else if (str_begins(key, DICT_PATH_PRIVATE, &key)) {
		key = t_strdup_printf("%s%c%s",
				      username_sp == NULL ? username :
						redis_escape_username(username),
				      DICT_USERNAME_SEPARATOR, key);
	} else {
		i_unreached();
	}
	if (*dict->set->dict_redis_key_prefix != '\0')
		key = t_strconcat(dict->set->dict_redis_key_prefix, key, NULL);
	return key;
}

static void redis_dict_auth(struct redis_dict *dict)
{
	const char *cmd;

	if (*dict->set->dict_redis_password == '\0')
		return;

	cmd = t_strdup_printf("*2\r\n$4\r\nAUTH\r\n$%zu\r\n%s\r\n",
			      strlen(dict->set->dict_redis_password),
			      dict->set->dict_redis_password);
	o_stream_nsend_str(dict->conn.conn.output, cmd);
	redis_input_state_add(dict, REDIS_INPUT_STATE_AUTH);
}

static void redis_dict_select_db(struct redis_dict *dict)
{
	const char *cmd, *db_str;

	if (dict->db_id_set)
		return;
	dict->db_id_set = TRUE;
	if (dict->set->dict_redis_db_id == 0) {
		/* 0 is the default */
		return;
	}
	db_str = dec2str(dict->set->dict_redis_db_id);
	cmd = t_strdup_printf("*2\r\n$6\r\nSELECT\r\n$%zu\r\n%s\r\n",
			      strlen(db_str), db_str);
	o_stream_nsend_str(dict->conn.conn.output, cmd);
	redis_input_state_add(dict, REDIS_INPUT_STATE_SELECT);
}

static int redis_dict_lookup(struct dict *_dict,
			     const struct dict_op_settings *set,
			     pool_t pool, const char *key,
			     const char *const **values_r, const char **error_r)
{
	struct redis_dict *dict = (struct redis_dict *)_dict;
	struct timeout *to;
	const char *cmd;

	key = redis_dict_get_full_key(dict, set->username, key);

	dict->conn.value_received = FALSE;
	dict->conn.value_not_found = FALSE;

	i_assert(dict->dict.ioloop == NULL);

	dict->dict.prev_ioloop = current_ioloop;
	dict->dict.ioloop = io_loop_create();
	connection_switch_ioloop(&dict->conn.conn);

	if (dict->conn.conn.fd_in == -1 &&
	    connection_client_connect(&dict->conn.conn) < 0) {
		e_error(dict->conn.conn.event, "Couldn't connect");
	} else {
		to = timeout_add(dict->set->dict_redis_request_timeout,
				 redis_dict_lookup_timeout, dict);
		if (!dict->connected) {
			/* wait for connection */
			io_loop_run(dict->dict.ioloop);
			if (dict->connected)
				redis_dict_auth(dict);
		}

		if (dict->connected) {
			redis_dict_select_db(dict);
			cmd = t_strdup_printf("*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
					      strlen(key), key);
			o_stream_nsend_str(dict->conn.conn.output, cmd);

			str_truncate(dict->conn.last_reply, 0);
			redis_input_state_add(dict, REDIS_INPUT_STATE_GET);
			do {
				io_loop_run(dict->dict.ioloop);
			} while (array_count(&dict->input_states) > 0);
		}
		timeout_remove(&to);
	}

	io_loop_set_current(dict->dict.prev_ioloop);
	connection_switch_ioloop(&dict->conn.conn);
	io_loop_set_current(dict->dict.ioloop);
	io_loop_destroy(&dict->dict.ioloop);
	dict->dict.prev_ioloop = NULL;

	if (!dict->conn.value_received) {
		/* we failed in some way. make sure we disconnect since the
		   connection state isn't known anymore */
		*error_r = t_strdup_printf("redis: Communication failure (last reply: %s)",
					   str_c(dict->conn.last_reply));
		redis_disconnected(&dict->conn, *error_r);
		return -1;
	}
	if (dict->conn.value_not_found)
		return 0;

	const char **values = p_new(pool, const char *, 2);
	values[0] = p_strdup(pool, str_c(dict->conn.last_reply));
	*values_r = values;
	return 1;
}

static struct dict_transaction_context *
redis_transaction_init(struct dict *_dict)
{
	struct redis_dict *dict = (struct redis_dict *)_dict;
	struct redis_dict_transaction_context *ctx;

	i_assert(!dict->transaction_open);
	dict->transaction_open = TRUE;

	ctx = i_new(struct redis_dict_transaction_context, 1);
	ctx->ctx.dict = _dict;

	if (dict->conn.conn.fd_in == -1 &&
	    connection_client_connect(&dict->conn.conn) < 0) {
		ctx->error = i_strdup_printf("connect() failed: %m");
		e_error(dict->conn.conn.event, "%s", ctx->error);
	} else if (!dict->connected) {
		/* wait for connection */
		ctx->error = i_strdup(redis_wait(dict));
		if (dict->connected)
			redis_dict_auth(dict);
	}
	if (dict->connected)
		redis_dict_select_db(dict);
	return &ctx->ctx;
}

static void
redis_transaction_commit(struct dict_transaction_context *_ctx, bool async,
			 dict_transaction_commit_callback_t *callback,
			 void *context)
{
	struct redis_dict_transaction_context *ctx =
		(struct redis_dict_transaction_context *)_ctx;
	struct redis_dict *dict = (struct redis_dict *)_ctx->dict;
	struct redis_dict_reply *reply;
	unsigned int i;
	struct dict_commit_result result = { .ret = DICT_COMMIT_RET_OK };

	i_assert(dict->transaction_open);
	dict->transaction_open = FALSE;

	if (ctx->error != NULL) {
		/* make sure we're disconnected */
		redis_disconnected(&dict->conn, ctx->error);
		result.ret = -1;
		result.error = ctx->error;
		callback(&result, context);
	} else if (_ctx->changed) {
		i_assert(ctx->cmd_count > 0);

		o_stream_nsend_str(dict->conn.conn.output,
				   "*1\r\n$4\r\nEXEC\r\n");
		reply = array_append_space(&dict->replies);
		reply->callback = callback;
		reply->context = context;
		reply->reply_count = ctx->cmd_count;
		redis_input_state_add(dict, REDIS_INPUT_STATE_EXEC);
		for (i = 0; i < ctx->cmd_count; i++)
			redis_input_state_add(dict, REDIS_INPUT_STATE_EXEC_REPLY);
		if (async) {
			if (dict->to == NULL) {
				dict->to = timeout_add(
					dict->set->dict_redis_request_timeout,
					redis_dict_wait_timeout, dict);
			}
			i_free(ctx);
			return;
		}
		ctx->error = i_strdup(redis_wait(dict));
		if (ctx->error != NULL)
			result.ret = DICT_COMMIT_RET_FAILED;
	} else {
		callback(&result, context);
	}
	i_free(ctx->error);
	i_free(ctx);
}

static void redis_transaction_rollback(struct dict_transaction_context *_ctx)
{
	struct redis_dict_transaction_context *ctx =
		(struct redis_dict_transaction_context *)_ctx;
	struct redis_dict *dict = (struct redis_dict *)_ctx->dict;

	i_assert(dict->transaction_open);
	dict->transaction_open = FALSE;

	if (ctx->error != NULL) {
		/* make sure we're disconnected */
		redis_disconnected(&dict->conn, ctx->error);
	} else if (_ctx->changed) {
		o_stream_nsend_str(dict->conn.conn.output,
				   "*1\r\n$7\r\nDISCARD\r\n");
		redis_input_state_add(dict, REDIS_INPUT_STATE_DISCARD);
	}
	i_free(ctx->error);
	i_free(ctx);
}

static int redis_check_transaction(struct redis_dict_transaction_context *ctx)
{
	struct redis_dict *dict = (struct redis_dict *)ctx->ctx.dict;

	if (ctx->error != NULL)
		return -1;
	if (!dict->connected) {
		ctx->error = i_strdup("Disconnected during transaction");
		return -1;
	}
	if (ctx->ctx.changed)
		return 0;

	redis_input_state_add(dict, REDIS_INPUT_STATE_MULTI);
	if (o_stream_send_str(dict->conn.conn.output,
			      "*1\r\n$5\r\nMULTI\r\n") < 0) {
		ctx->error = i_strdup_printf("write() failed: %s",
			o_stream_get_error(dict->conn.conn.output));
		return -1;
	}
	return 0;
}

static void
redis_append_expire(struct redis_dict_transaction_context *ctx,
		    string_t *cmd, const char *key)
{
	struct redis_dict *dict = (struct redis_dict *)ctx->ctx.dict;
	unsigned int expire_secs = dict->set->dict_redis_expire;

	if (ctx->ctx.set.expire_secs > 0)
		expire_secs = ctx->ctx.set.expire_secs;
	if (expire_secs == 0)
		return;
	const char *expire_value = dec2str(expire_secs);

	str_printfa(cmd, "*3\r\n$6\r\nEXPIRE\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
		    strlen(key), key, strlen(expire_value), expire_value);
	redis_input_state_add(dict, REDIS_INPUT_STATE_MULTI);
	ctx->cmd_count++;
}

static void redis_set(struct dict_transaction_context *_ctx,
		      const char *key, const char *value)
{
	struct redis_dict_transaction_context *ctx =
		(struct redis_dict_transaction_context *)_ctx;
	struct redis_dict *dict = (struct redis_dict *)_ctx->dict;
	const struct dict_op_settings_private *set = &_ctx->set;
	string_t *cmd;

	if (redis_check_transaction(ctx) < 0)
		return;

	key = redis_dict_get_full_key(dict, set->username, key);
	cmd = t_str_new(128);
	str_printfa(cmd, "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
		    strlen(key), key, strlen(value), value);
	redis_input_state_add(dict, REDIS_INPUT_STATE_MULTI);
	ctx->cmd_count++;
	redis_append_expire(ctx, cmd, key);
	if (o_stream_send(dict->conn.conn.output, str_data(cmd), str_len(cmd)) < 0) {
		ctx->error = i_strdup_printf("write() failed: %s",
			o_stream_get_error(dict->conn.conn.output));
	}
}

static void redis_unset(struct dict_transaction_context *_ctx,
			const char *key)
{
	struct redis_dict_transaction_context *ctx =
		(struct redis_dict_transaction_context *)_ctx;
	struct redis_dict *dict = (struct redis_dict *)_ctx->dict;
	const struct dict_op_settings_private *set = &_ctx->set;
	const char *cmd;

	if (redis_check_transaction(ctx) < 0)
		return;

	key = redis_dict_get_full_key(dict, set->username, key);
	cmd = t_strdup_printf("*2\r\n$3\r\nDEL\r\n$%zu\r\n%s\r\n",
			      strlen(key), key);
	if (o_stream_send_str(dict->conn.conn.output, cmd) < 0) {
		ctx->error = i_strdup_printf("write() failed: %s",
			o_stream_get_error(dict->conn.conn.output));
	}
	redis_input_state_add(dict, REDIS_INPUT_STATE_MULTI);
	ctx->cmd_count++;
}

static void redis_atomic_inc(struct dict_transaction_context *_ctx,
			     const char *key, long long diff)
{
	struct redis_dict_transaction_context *ctx =
		(struct redis_dict_transaction_context *)_ctx;
	struct redis_dict *dict = (struct redis_dict *)_ctx->dict;
	const struct dict_op_settings_private *set = &_ctx->set;
	const char *diffstr;
	string_t *cmd;

	if (redis_check_transaction(ctx) < 0)
		return;

	key = redis_dict_get_full_key(dict, set->username, key);
	diffstr = t_strdup_printf("%lld", diff);
	cmd = t_str_new(128);
	str_printfa(cmd, "*3\r\n$6\r\nINCRBY\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
		    strlen(key), key, strlen(diffstr), diffstr);
	redis_input_state_add(dict, REDIS_INPUT_STATE_MULTI);
	ctx->cmd_count++;
	redis_append_expire(ctx, cmd, key);
	if (o_stream_send(dict->conn.conn.output, str_data(cmd), str_len(cmd)) < 0) {
		ctx->error = i_strdup_printf("write() failed: %s",
			o_stream_get_error(dict->conn.conn.output));
	}
}

struct dict dict_driver_redis = {
	.name = "redis",
	.flags = DICT_DRIVER_FLAG_SUPPORT_EXPIRE_SECS,
	.v = {
		.init = redis_dict_init,
		.init_legacy = redis_dict_init_legacy,
		.deinit = redis_dict_deinit,
		.wait = redis_dict_wait,
		.lookup = redis_dict_lookup,
		.transaction_init = redis_transaction_init,
		.transaction_commit = redis_transaction_commit,
		.transaction_rollback = redis_transaction_rollback,
		.set = redis_set,
		.unset = redis_unset,
		.atomic_inc = redis_atomic_inc,
	}
};
