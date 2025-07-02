/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "net.h"
#include "str.h"
#include "hash.h"
#include "llist.h"
#include "array.h"
#include "ioloop.h"
#include "istream.h"
#include "istream-timeout.h"
#include "ostream.h"
#include "time-util.h"
#include "file-lock.h"
#include "settings.h"
#include "iostream-rawlog.h"
#include "iostream-ssl.h"
#include "http-response-parser.h"

#include "http-client-private.h"

/*
 * Connection
 */

static void http_client_connection_ready(struct http_client_connection *conn);
static void http_client_connection_input(struct connection *_conn);
static void
http_client_connection_disconnect(struct http_client_connection *conn);

static inline const struct http_client_settings *
http_client_connection_get_settings(struct http_client_connection *conn)
{
	return conn->set;
}

static inline void
http_client_connection_ref_request(struct http_client_connection *conn,
				   struct http_client_request *req)
{
	i_assert(req->conn == NULL);
	req->conn = conn;
	http_client_request_ref(req);
}

static inline bool
http_client_connection_unref_request(struct http_client_connection *conn,
				     struct http_client_request **_req)
{
	struct http_client_request *req = *_req;

	i_assert(req->conn == conn);
	req->conn = NULL;
	return http_client_request_unref(_req);
}

static void
http_client_connection_unlist_pending(struct http_client_connection *conn)
{
	struct http_client_peer *peer = conn->peer;
	struct http_client_peer_pool *ppool = conn->ppool;
	unsigned int idx;

	/* Remove from pending lists */

	if (!array_lsearch_ptr_idx(&ppool->pending_conns, conn, &idx))
		i_unreached();
	array_delete(&ppool->pending_conns, idx, 1);

	if (peer == NULL)
		return;

	if (!array_lsearch_ptr_idx(&peer->pending_conns, conn, &idx))
		i_unreached();
	array_delete(&peer->pending_conns, idx, 1);
}

static inline void
http_client_connection_failure(struct http_client_connection *conn,
			       const char *reason)
{
	struct http_client_peer *peer = conn->peer;

	conn->connect_failed = TRUE;
	http_client_connection_unlist_pending(conn);
	http_client_peer_connection_failure(peer, reason);
}

unsigned int
http_client_connection_count_pending(struct http_client_connection *conn)
{
	unsigned int pending_count = array_count(&conn->request_wait_list);

	if (conn->in_req_callback || conn->pending_request != NULL)
		pending_count++;
	return pending_count;
}

bool http_client_connection_is_idle(struct http_client_connection *conn)
{
	return conn->idle;
}

bool http_client_connection_is_active(struct http_client_connection *conn)
{
	if (!conn->connected)
		return FALSE;

	if (conn->in_req_callback || conn->pending_request != NULL)
		return TRUE;

	return (array_is_created(&conn->request_wait_list) &&
		array_count(&conn->request_wait_list) > 0);
}

static void
http_client_connection_retry_requests(struct http_client_connection *conn,
				      unsigned int status, const char *error)
{
	struct http_client_request *req, **req_idx;

	if (!array_is_created(&conn->request_wait_list))
		return;

	e_debug(conn->event, "Retrying pending requests");

	array_foreach_modifiable(&conn->request_wait_list, req_idx) {
		req = *req_idx;
		/* Drop reference from connection */
		if (!http_client_connection_unref_request(conn, req_idx))
			continue;
		/* Retry the request, which may drop it */
		if (req->state < HTTP_REQUEST_STATE_FINISHED)
			http_client_request_retry(req, status, error);
	}
	array_clear(&conn->request_wait_list);
}

static void
http_client_connection_server_close(struct http_client_connection **_conn)
{
	struct http_client_connection *conn = *_conn;
	struct http_client_peer *peer = conn->peer;
	struct http_client_request *req, **req_idx;

	e_debug(conn->event, "Server explicitly closed connection");

	array_foreach_modifiable(&conn->request_wait_list, req_idx) {
		req = *req_idx;
		/* Drop reference from connection */
		if (!http_client_connection_unref_request(conn, req_idx))
			continue;
		/* Resubmit the request, which may drop it */
		if (req->state < HTTP_REQUEST_STATE_FINISHED)
			http_client_request_resubmit(req);
	}
	array_clear(&conn->request_wait_list);

	if (peer != NULL) {
		struct http_client *client = peer->client;

		if (client->waiting)
			io_loop_stop(client->ioloop);
	}

	http_client_connection_close(_conn);
}

static void
http_client_connection_abort_error(struct http_client_connection **_conn,
				   unsigned int status, const char *error)
{
	struct http_client_connection *conn = *_conn;
	struct http_client_request *req, **req_idx;

	e_debug(conn->event, "Aborting connection: %s", error);

	array_foreach_modifiable(&conn->request_wait_list, req_idx) {
		req = *req_idx;
		i_assert(req->submitted);
		/* Drop reference from connection */
		if (!http_client_connection_unref_request(conn, req_idx))
			continue;
		/* Drop request if not already aborted */
		http_client_request_error(&req, status, error);
	}
	array_clear(&conn->request_wait_list);
	http_client_connection_close(_conn);
}

static void
http_client_connection_abort_any_requests(struct http_client_connection *conn)
{
	struct http_client_request *req, **req_idx;

	if (array_is_created(&conn->request_wait_list)) {
		array_foreach_modifiable(&conn->request_wait_list, req_idx) {
			req = *req_idx;
			i_assert(req->submitted);
			/* Drop reference from connection */
			if (!http_client_connection_unref_request(conn, req_idx))
				continue;
			/* Drop request if not already aborted */
			http_client_request_error(
				&req, HTTP_CLIENT_REQUEST_ERROR_ABORTED,
				"Aborting");
		}
		array_clear(&conn->request_wait_list);
	}
	if (conn->pending_request != NULL) {
		req = conn->pending_request;
		/* Drop reference from connection */
		if (http_client_connection_unref_request(
			conn, &conn->pending_request)) {
			/* Drop request if not already aborted */
			http_client_request_error(
				&req, HTTP_CLIENT_REQUEST_ERROR_ABORTED,
				"Aborting");
		}
	}
}

static const char *
http_client_connection_get_timing_info(struct http_client_connection *conn)
{
	struct http_client_request *const *requestp;
	long long connected_msecs;
	string_t *str = t_str_new(64);

	if (array_count(&conn->request_wait_list) > 0) {
		requestp = array_front(&conn->request_wait_list);

		str_append(str, "Request ");
		http_client_request_append_stats_text(*requestp, str);
	} else {
		str_append(str, "No requests");
		if (conn->conn.last_input != 0) {
			str_printfa(str, ", last input %d secs ago",
				    (int)(ioloop_time - conn->conn.last_input));
		}
	}
	connected_msecs = timeval_diff_msecs(&ioloop_timeval,
					     &conn->connected_timestamp);
	str_printfa(str, ", connected %lld.%03lld secs ago",
		    connected_msecs/1000, connected_msecs%1000);
	return str_c(str);
}

static void
http_client_connection_abort_temp_error(struct http_client_connection **_conn,
					unsigned int status, const char *error)
{
	struct http_client_connection *conn = *_conn;

	error = t_strdup_printf("%s (%s)", error,
				http_client_connection_get_timing_info(conn));

	e_debug(conn->event,
		"Aborting connection with temporary error: %s", error);

	http_client_connection_disconnect(conn);
	http_client_connection_retry_requests(conn, status, error);
	http_client_connection_close(_conn);
}

void http_client_connection_lost(struct http_client_connection **_conn,
				 const char *error)
{
	struct http_client_connection *conn = *_conn;
	const char *sslerr;

	if (error == NULL)
		error = "Connection lost";
	else
		error = t_strdup_printf("Connection lost: %s", error);

	if (conn->ssl_iostream != NULL) {
		sslerr = ssl_iostream_get_last_error(conn->ssl_iostream);
		if (sslerr != NULL) {
			error = t_strdup_printf("%s (last SSL error: %s)",
						error, sslerr);
		}
		if (ssl_iostream_has_handshake_failed(conn->ssl_iostream)) {
			/* This isn't really a "connection lost", but that we
			   don't trust the remote's SSL certificate. don't
			   retry. */
			http_client_connection_abort_error(
				_conn,
				HTTP_CLIENT_REQUEST_ERROR_BAD_RESPONSE, error);
			return;
		}
	}

	conn->lost_prematurely =
		(conn->conn.input != NULL &&
		 conn->conn.input->v_offset == 0 &&
		 i_stream_get_data_size(conn->conn.input) == 0);
	http_client_connection_abort_temp_error(
		_conn, HTTP_CLIENT_REQUEST_ERROR_CONNECTION_LOST, error);
}

void http_client_connection_handle_output_error(
	struct http_client_connection *conn)
{
	struct ostream *output = conn->conn.output;

	if (output->stream_errno != EPIPE &&
	    output->stream_errno != ECONNRESET) {
		http_client_connection_lost(
			&conn,
			t_strdup_printf("write(%s) failed: %s",
					o_stream_get_name(output),
					o_stream_get_error(output)));
	} else {
		http_client_connection_lost(&conn, "Remote disconnected");
	}
}

int http_client_connection_check_ready(struct http_client_connection *conn)
{
	const struct http_client_settings *set =
		http_client_connection_get_settings(conn);

	if (conn->in_req_callback) {
		/* This can happen when a nested ioloop is created inside
		   request callback. we currently don't reuse connections that
		   are occupied this way, but theoretically we could, although
		   that would add quite a bit of complexity.
		 */
		return 0;
	}

	if (!conn->connected || conn->output_locked || conn->output_broken ||
	    conn->close_indicated || conn->tunneling ||
	    (http_client_connection_count_pending(conn) >=
	     set->max_pipelined_requests))
		return 0;

	if (conn->last_ioloop != NULL && conn->last_ioloop != current_ioloop) {
		conn->last_ioloop = current_ioloop;
		/* Active ioloop is different from what we saw earlier; we may
		   have missed a disconnection event on this connection. Verify
		   status by reading from connection. */
		if (i_stream_read(conn->conn.input) == -1) {
			int stream_errno = conn->conn.input->stream_errno;

			i_assert(conn->conn.input->stream_errno != 0 ||
				 conn->conn.input->eof);
			http_client_connection_lost(
				&conn,
				t_strdup_printf(
					"read(%s) failed: %s",
					i_stream_get_name(conn->conn.input),
					(stream_errno != 0 ?
					 i_stream_get_error(conn->conn.input) :
					 "EOF")));
			return -1;
		}

		/* We may have read some data */
		if (i_stream_get_data_size(conn->conn.input) > 0)
			i_stream_set_input_pending(conn->conn.input, TRUE);
	}
	return 1;
}

static void
http_client_connection_detach_peer(struct http_client_connection *conn)
{
	struct http_client_peer *peer = conn->peer;
	unsigned int idx;

	if (peer == NULL)
		return;

	http_client_peer_ref(peer);
	if (!array_lsearch_ptr_idx(&peer->conns, conn, &idx))
		i_unreached();
	array_delete(&peer->conns, idx, 1);

	if (array_lsearch_ptr_idx(&peer->pending_conns, conn, &idx))
		array_delete(&peer->pending_conns, idx, 1);

	conn->peer = NULL;
	e_debug(conn->event, "Detached peer");

	if (conn->connect_succeeded)
		http_client_peer_connection_lost(peer, conn->lost_prematurely);
	http_client_peer_unref(&peer);
}

static void
http_client_connection_idle_timeout(struct http_client_connection *conn)
{
	e_debug(conn->event, "Idle connection timed out");

	/* Cannot get here unless connection was established at some point */
	i_assert(conn->connect_succeeded);

	http_client_connection_close(&conn);
}

static unsigned int
http_client_connection_start_idle_timeout(struct http_client_connection *conn)
{
	const struct http_client_settings *set =
		http_client_connection_get_settings(conn);
	struct http_client_peer_pool *ppool = conn->ppool;
	struct http_client_peer_shared *pshared = ppool->peer;
	unsigned int timeout, count, idle_count, max;

	i_assert(conn->to_idle == NULL);

	if (set->max_idle_time_msecs == 0)
		return UINT_MAX;

	count = array_count(&ppool->conns);
	idle_count = array_count(&ppool->idle_conns);
	max = http_client_peer_shared_max_connections(pshared);
	i_assert(count > 0);
	i_assert(count >= idle_count + 1);
	i_assert(max > 0);

	/* Set timeout for this connection */
	if (idle_count == 0 || max == UINT_MAX) {
		/* No idle connections yet or infinite connections allowed;
		   use the maximum idle time. */
		timeout = set->max_idle_time_msecs;
	} else if (count > max || idle_count >= max) {
		/* Instant death for (urgent) connections above limit */
		timeout = 0;
	} else {
		unsigned int idle_slots_avail;
		double idle_time_per_slot;

		/* Kill duplicate connections quicker;
		   linearly based on the number of connections */
		idle_slots_avail = max - idle_count;
		idle_time_per_slot = (double)set->max_idle_time_msecs / max;
		timeout = (unsigned int)(idle_time_per_slot * idle_slots_avail);
		if (timeout < HTTP_CLIENT_MIN_IDLE_TIMEOUT_MSECS)
			timeout = HTTP_CLIENT_MIN_IDLE_TIMEOUT_MSECS;
	}

	conn->to_idle = timeout_add_short_to(
		conn->conn.ioloop, timeout,
		http_client_connection_idle_timeout, conn);
	return timeout;
}

static void
http_client_connection_start_idle(struct http_client_connection *conn,
				  const char *reason)
{
	struct http_client_peer_pool *ppool = conn->ppool;
	unsigned int timeout;

	if (conn->idle) {
		e_debug(conn->event, "%s; already idle", reason);
		return;
	}

	timeout = http_client_connection_start_idle_timeout(conn);
	if (timeout == UINT_MAX)
		e_debug(conn->event, "%s; going idle", reason);
	else {
		e_debug(conn->event, "%s; going idle (timeout = %u msecs)",
			reason, timeout);
	}

	conn->idle = TRUE;
	array_push_back(&ppool->idle_conns, &conn);
}

void http_client_connection_lost_peer(struct http_client_connection *conn)
{
	if (!conn->connected) {
		http_client_connection_unref(&conn);
		return;
	}

	i_assert(!conn->in_req_callback);

	http_client_connection_start_idle(conn, "Lost peer");
	http_client_connection_detach_peer(conn);
}

void http_client_connection_check_idle(struct http_client_connection *conn)
{
	struct http_client_peer *peer;

	peer = conn->peer;
	if (peer == NULL) {
		i_assert(conn->idle);
		return;
	}

	if (conn->idle) {
		/* Already idle */
		return;
	}

	if (conn->connected && !http_client_connection_is_active(conn)) {
		struct http_client *client = peer->client;

		i_assert(conn->to_requests == NULL);

		if (client->waiting)
			io_loop_stop(client->ioloop);

		http_client_connection_start_idle(
			conn, "No more requests queued");
	}
}

static void
http_client_connection_stop_idle(struct http_client_connection *conn)
{
	unsigned int idx;

	timeout_remove(&conn->to_idle);
	conn->idle = FALSE;

	if (array_lsearch_ptr_idx(&conn->ppool->idle_conns, conn, &idx))
		array_delete(&conn->ppool->idle_conns, idx, 1);
}

void http_client_connection_claim_idle(struct http_client_connection *conn,
				       struct http_client_peer *peer)
{
	e_debug(conn->event, "Claimed as idle");

	i_assert(peer->ppool == conn->ppool);
	http_client_connection_stop_idle(conn);

	if (conn->peer == NULL || conn->peer != peer) {
		http_client_connection_detach_peer(conn);

		settings_free(conn->set);
		conn->set = peer->client->set;
		pool_ref(conn->set->pool);

		conn->peer = peer;
		array_push_back(&peer->conns, &conn);
	}
}

static void
http_client_connection_request_timeout(struct http_client_connection *conn)
{
	conn->conn.input->stream_errno = ETIMEDOUT;
	http_client_connection_abort_temp_error(
		&conn, HTTP_CLIENT_REQUEST_ERROR_TIMED_OUT,
		"Request timed out");
}

void http_client_connection_start_request_timeout(
	struct http_client_connection *conn)
{
	struct http_client_request *const *requestp;
	unsigned int timeout_msecs;

	if (conn->pending_request != NULL)
		return;

	i_assert(array_is_created(&conn->request_wait_list));
	i_assert(array_count(&conn->request_wait_list) > 0);
	requestp = array_front(&conn->request_wait_list);
	timeout_msecs = (*requestp)->attempt_timeout_msecs;

	if (timeout_msecs == 0)
		;
	else if (conn->to_requests != NULL)
		timeout_reset(conn->to_requests);
	else {
		conn->to_requests = timeout_add_to(
			conn->conn.ioloop, timeout_msecs,
			http_client_connection_request_timeout, conn);
	}
}

void http_client_connection_reset_request_timeout(
	struct http_client_connection *conn)
{
	if (conn->to_requests != NULL)
		timeout_reset(conn->to_requests);
}

void http_client_connection_stop_request_timeout(
	struct http_client_connection *conn)
{
	timeout_remove(&conn->to_requests);
}

static void
http_client_connection_continue_timeout(struct http_client_connection *conn)
{
	struct http_client_peer_shared *pshared = conn->ppool->peer;
	struct http_client_request *const *wait_reqs;
	struct http_client_request *req;
	unsigned int wait_count;

	i_assert(conn->pending_request == NULL);

	timeout_remove(&conn->to_response);
	pshared->no_payload_sync = TRUE;

	e_debug(conn->event,
		"Expected 100-continue response timed out; "
		"sending payload anyway");

	wait_reqs = array_get(&conn->request_wait_list, &wait_count);
	i_assert(wait_count == 1);
	req = wait_reqs[wait_count-1];

	req->payload_sync_continue = TRUE;
	if (conn->conn.output != NULL)
		o_stream_set_flush_pending(conn->conn.output, TRUE);
}

int http_client_connection_next_request(struct http_client_connection *conn)
{
	struct http_client_connection *tmp_conn;
	struct http_client_peer *peer = conn->peer;
	struct http_client_peer_shared *pshared = conn->ppool->peer;
	struct http_client_request *req = NULL;
	bool pipelined;
	int ret;

	if ((ret = http_client_connection_check_ready(conn)) <= 0) {
		if (ret == 0)
			e_debug(conn->event, "Not ready for next request");
		return ret;
	}

	/* Claim request, but no urgent request can be second in line */
	pipelined = (array_count(&conn->request_wait_list) > 0 ||
		     conn->pending_request != NULL);
	req = http_client_peer_claim_request(peer, pipelined);
	if (req == NULL)
		return 0;

	i_assert(req->state == HTTP_REQUEST_STATE_QUEUED);

	http_client_connection_stop_idle(conn);

	req->payload_sync_continue = FALSE;
	if (pshared->no_payload_sync)
		req->payload_sync = FALSE;

	/* Add request to wait list and add a reference */
	array_push_back(&conn->request_wait_list, &req);
	http_client_connection_ref_request(conn, req);

	e_debug(conn->event, "Claimed request %s",
		http_client_request_label(req));

	tmp_conn = conn;
	http_client_connection_ref(tmp_conn);
	ret = http_client_request_send(req, pipelined);
	if (ret == 0 && conn->conn.output != NULL)
		o_stream_set_flush_pending(conn->conn.output, TRUE);
	if (!http_client_connection_unref(&tmp_conn) || ret < 0)
		return -1;

	if (req->connect_tunnel)
		conn->tunneling = TRUE;

	/* RFC 7231, Section 5.1.1: Expect

	    o  A client that sends a 100-continue expectation is not required to
	       wait for any specific length of time; such a client MAY proceed
	       to send the message body even if it has not yet received a
	       response. Furthermore, since 100 (Continue) responses cannot be
	       sent through an HTTP/1.0 intermediary, such a client SHOULD NOT
	       wait for an indefinite period before sending the message body.
	 */
	if (req->payload_sync && !pshared->seen_100_response) {
		i_assert(!pipelined);
		i_assert(req->payload_chunked || req->payload_size > 0);
		i_assert(conn->to_response == NULL);
		conn->to_response = timeout_add_to(
			conn->conn.ioloop, HTTP_CLIENT_CONTINUE_TIMEOUT_MSECS,
			http_client_connection_continue_timeout, conn);
	}

	return 1;
}

static void http_client_connection_destroy(struct connection *_conn)
{
	struct http_client_connection *conn =
		(struct http_client_connection *)_conn;
	const char *error;
	long long msecs;

	switch (_conn->disconnect_reason) {
	case CONNECTION_DISCONNECT_CONNECT_TIMEOUT:
		if (conn->connected_timestamp.tv_sec == 0) {
			msecs = timeval_diff_msecs(
				&ioloop_timeval,
				&conn->connect_start_timestamp);
			error = t_strdup_printf(
				"connect(%s) failed: "
				"Connection timed out in %lld.%03lld secs",
				_conn->name, msecs/1000, msecs%1000);
		} else {
			msecs = timeval_diff_msecs(&ioloop_timeval,
						   &conn->connected_timestamp);
			error = t_strdup_printf(
				"SSL handshaking with %s failed: "
				"Connection timed out in %lld.%03lld secs",
				_conn->name, msecs/1000, msecs%1000);
		}
		e_debug(conn->event, "%s", error);
		http_client_connection_failure(conn, error);
		break;
	case CONNECTION_DISCONNECT_CONN_CLOSED:
		if (conn->connect_failed) {
			i_assert(!array_is_created(&conn->request_wait_list) ||
				 array_count(&conn->request_wait_list) == 0);
			break;
		}
		http_client_connection_lost(
			&conn, (_conn->input == NULL ?
				NULL : i_stream_get_error(_conn->input)));
		return;
	default:
		break;
	}

	http_client_connection_close(&conn);
}

static void http_client_payload_finished(struct http_client_connection *conn)
{
	timeout_remove(&conn->to_input);
	connection_input_resume(&conn->conn);
	if (array_count(&conn->request_wait_list) > 0)
		http_client_connection_start_request_timeout(conn);
	else
		http_client_connection_stop_request_timeout(conn);
}

static void
http_client_payload_destroyed_timeout(struct http_client_connection *conn)
{
	if (conn->close_indicated) {
		http_client_connection_server_close(&conn);
		return;
	}
	http_client_connection_input(&conn->conn);
}

static void http_client_payload_destroyed(struct http_client_request *req)
{
	struct http_client_connection *conn = req->conn;

	i_assert(conn != NULL);
	i_assert(conn->pending_request == req);
	i_assert(conn->incoming_payload != NULL);
	i_assert(conn->conn.io == NULL);

	e_debug(conn->event,
		"Response payload stream destroyed "
		"(%lld ms after initial response)",
		timeval_diff_msecs(&ioloop_timeval, &req->response_time));

	/* Caller is allowed to change the socket fd to blocking while reading
	   the payload. make sure here that it's switched back. */
	net_set_nonblock(conn->conn.fd_in, TRUE);

	i_assert(req->response_offset < conn->conn.input->v_offset);
	req->bytes_in = conn->conn.input->v_offset - req->response_offset;

	/* Drop reference from connection */
	if (http_client_connection_unref_request(
		conn, &conn->pending_request)) {
		/* Finish request if not already aborted */
		http_client_request_finish(req);
	}

	conn->incoming_payload = NULL;

	/* Input stream may have pending input. make sure input handler
	   gets called (but don't do it directly, since we get here
	   somewhere from the API user's code, which we can't really know what
	   state it is in). this call also triggers sending a new request if
	   necessary. */
	if (!conn->disconnected) {
		conn->to_input = timeout_add_short_to(
			conn->conn.ioloop, 0,
			http_client_payload_destroyed_timeout, conn);
	}

	/* Room for new requests */
	if (http_client_connection_check_ready(conn) > 0)
		http_client_peer_trigger_request_handler(conn->peer);
}

void http_client_connection_request_destroyed(
	struct http_client_connection *conn, struct http_client_request *req)
{
	struct istream *payload;

	i_assert(req->conn == conn);
	if (conn->pending_request != req)
		return;

	e_debug(conn->event, "Pending request destroyed prematurely");

	payload = conn->incoming_payload;
	if (payload == NULL) {
		/* Payload already gone */
		return;
	}

	/* Destroy the payload, so that the timeout istream is closed */
	i_stream_ref(payload);
	i_stream_destroy(&payload);

	payload = conn->incoming_payload;
	if (payload == NULL) {
		/* Not going to happen, but check for it anyway */
		return;
	}

	/* The application still holds a reference to the payload stream, but it
	   is closed and we don't care about it anymore, so act as though it is
	   destroyed. */
	i_stream_remove_destroy_callback(payload,
					 http_client_payload_destroyed);
	http_client_payload_destroyed(req);
}

static bool
http_client_connection_return_response(struct http_client_connection *conn,
				       struct http_client_request *req,
				       struct http_response *response)
{
	struct http_client_peer_shared *pshared = conn->ppool->peer;
	struct istream *payload;
	bool retrying;

	i_assert(!conn->in_req_callback);
	i_assert(conn->incoming_payload == NULL);
	i_assert(conn->pending_request == NULL);

	http_client_connection_ref(conn);
	http_client_connection_ref_request(conn, req);

	if (response->payload != NULL) {
		/* Wrap the stream to capture the destroy event without
		   destroying the actual payload stream. we are already expected
		   to be on the correct ioloop, so there should be no need to
		   switch the stream's ioloop here. */
		conn->incoming_payload = response->payload =
			i_stream_create_timeout(response->payload,
						req->attempt_timeout_msecs);
		i_stream_add_destroy_callback(response->payload,
					      http_client_payload_destroyed,
					      req);
		/* The callback may add its own I/O, so we need to remove
		   our one before calling it */
		connection_input_halt(&conn->conn);
		/* We've received the request itself, and we can't reset the
		   timeout during the payload reading. */
		http_client_connection_stop_request_timeout(conn);
	}

	conn->in_req_callback = TRUE;
	retrying = !http_client_request_callback(req, response);
	if (conn->disconnected) {
		/* The callback managed to get this connection disconnected */
		if (!retrying)
			http_client_request_finish(req);
		http_client_connection_unref_request(conn, &req);
		http_client_connection_unref(&conn);
		return FALSE;
	}
	conn->in_req_callback = FALSE;

	if (retrying) {
		/* Retrying, don't destroy the request */
		if (response->payload != NULL) {
			i_stream_remove_destroy_callback(
				conn->incoming_payload,
				http_client_payload_destroyed);
			i_stream_unref(&conn->incoming_payload);
			connection_input_resume(&conn->conn);
		}
		http_client_connection_unref_request(conn, &req);
		return http_client_connection_unref(&conn);
	}

	if (response->payload != NULL) {
		req->state = HTTP_REQUEST_STATE_PAYLOAD_IN;
		payload = response->payload;
		response->payload = NULL;

		/* Maintain request reference while payload is pending */
		conn->pending_request = req;

		/* Request is dereferenced in payload destroy callback */
		i_stream_unref(&payload);

		if (conn->to_input != NULL && conn->conn.input != NULL) {
			/* Already finished reading the payload */
			http_client_payload_finished(conn);
		}
	} else {
		http_client_request_finish(req);
		http_client_connection_unref_request(conn, &req);
	}

	if (conn->incoming_payload == NULL && conn->conn.input != NULL) {
		i_assert(conn->conn.io != NULL ||
			 pshared->addr.type == HTTP_CLIENT_PEER_ADDR_RAW);
		return http_client_connection_unref(&conn);
	}
	http_client_connection_unref(&conn);
	return FALSE;
}

static bool
http_client_connection_handle_response(struct http_client_connection *conn,
				       struct http_client_request *req,
				       struct http_response *resp)
{
	struct http_client_peer_shared *pshared = conn->ppool->peer;

	/* Don't redirect/retry if we're sending data in small blocks via
	   http_client_request_send_payload() and we're not waiting for
	   100-continue. */
	if (req->payload_wait &&
	    (!req->payload_sync || req->payload_sync_continue))
		return FALSE;

	/* Failed Expect: */
	if (resp->status == 417 && req->payload_sync) {
		/* Drop Expect: continue */
		req->payload_sync = FALSE;
		conn->output_locked = FALSE;
		pshared->no_payload_sync = TRUE;
		if (http_client_request_try_retry(req))
			return TRUE;
	/* Redirection */
	} else if (req->client->set->auto_redirect &&
		   resp->status / 100 == 3 && resp->status != 304 &&
		   resp->location != NULL) {
		/* Redirect (possibly after delay) */
		if (http_client_request_delay_from_response(req, resp) >= 0) {
			http_client_request_redirect(req, resp->status,
						     resp->location);
			return TRUE;
		}
	/* Service unavailable */
	} else if (resp->status == 503) {
		/* Automatically retry after delay if indicated */
		if (resp->retry_after != (time_t)-1 &&
		    http_client_request_delay_from_response(req, resp) > 0 &&
		    http_client_request_try_retry(req))
			return TRUE;
	/* Request timeout (by server) */
	} else if (resp->status == 408) {
		/* Automatically retry */
		if (http_client_request_try_retry(req))
			return TRUE;
		/* Connection close is implicit, although server should indicate
		   that explicitly */
		conn->close_indicated = TRUE;
	}
	return FALSE;
}

/* Process incoming response. Returns -1 when no more responses should be
   handled (e.g. upon error or connection close), zero if this (1xx) response is
   ignored, or 1 when this response was accepted as the final response and
   the request was finished.
 */
static int
http_client_connection_process_response(struct http_client_connection *conn,
					struct http_client_request *req,
					struct http_response *resp)
{
	struct http_client_request *req_ref;
	bool aborted, early = FALSE;
	int ret;

	if (req == NULL) {
		/* Server sent response without any requests in the wait
		   list */
		if (resp->status == 408) {
			e_debug(conn->event,
				"Server explicitly closed connection: "
				"408 %s", resp->reason);
		} else {
			e_debug(conn->event,
				"Got unexpected input from server: "
				"%u %s", resp->status,
				resp->reason);
		}
		http_client_connection_close(&conn);
		return -1;
	}

	req->response_time = ioloop_timeval;
	req->response_offset =
		http_response_parser_get_last_offset(conn->http_parser);
	i_assert(req->response_offset != UOFF_T_MAX);
	i_assert(req->response_offset < conn->conn.input->v_offset);
	req->bytes_in = conn->conn.input->v_offset - req->response_offset;

	/* Got some response; cancel response timeout */
	timeout_remove(&conn->to_response);

	/* Perform response pre-checks */
	ret = http_client_request_check_response(req, resp, &early);
	if (ret <= 0)
		return ret;

	/* Remove request from queue */
	array_pop_front(&conn->request_wait_list);
	aborted = (req->state == HTTP_REQUEST_STATE_ABORTED);
	req_ref = req;
	if (!http_client_connection_unref_request(conn, &req_ref)) {
		i_assert(aborted);
		req = NULL;
	}

	conn->close_indicated = resp->connection_close;

	if (aborted) {
		/* Request is already aborted. */
		return 1;
	}

	/* Response cannot be 2xx if request payload was not
	   completely sent */
	if (early && resp->status / 100 == 2) {
		http_client_request_error(
			&req, HTTP_CLIENT_REQUEST_ERROR_BAD_RESPONSE,
			"Server responded with success response "
			"before all payload was sent");
		http_client_connection_close(&conn);
		return -1;
	}

	/* Check whether response needs to be handled internally. */
	if (http_client_connection_handle_response(conn, req, resp)) {
		/* Handled internally */
		return 1;
	}

	/* Response handled by application */
	if (!http_client_connection_return_response(conn, req, resp))
		return -1;
	return 1;
}

static void http_client_connection_input(struct connection *_conn)
{
	struct http_client_connection *conn =
		(struct http_client_connection *)_conn;
	struct http_client_peer *peer = conn->peer;
	struct http_client_peer_shared *pshared = conn->ppool->peer;
	struct http_response response;
	struct http_client_request *const *reqs;
	struct http_client_request *req = NULL;
	enum http_response_payload_type payload_type;
	unsigned int count;
	int finished = 0, ret;
	const char *error;

	i_assert(conn->incoming_payload == NULL);

	_conn->last_input = ioloop_time;

	if (conn->ssl_iostream != NULL &&
	    !ssl_iostream_is_handshaked(conn->ssl_iostream)) {
		/* Finish SSL negotiation by reading from input stream */
		while ((ret = i_stream_read(conn->conn.input)) > 0 ||
		       ret == -2) {
			if (ssl_iostream_is_handshaked(conn->ssl_iostream))
				break;
		}
		if (ret == -1) {
			int stream_errno = conn->conn.input->stream_errno;

			/* Failed somehow */
			i_assert(ret != -2);
			error = t_strdup_printf(
				"SSL handshaking with %s failed: "
				"read(%s) failed: %s",
				_conn->name,
				i_stream_get_name(conn->conn.input),
				(stream_errno != 0 ?
				 i_stream_get_error(conn->conn.input) : "EOF"));
			http_client_connection_failure(conn, error);
			e_debug(conn->event, "%s", error);
			http_client_connection_close(&conn);
			return;
		}

		if (!ssl_iostream_is_handshaked(conn->ssl_iostream)) {
			/* Not finished */
			i_assert(ret == 0);
			return;
		}
	}

	if (!conn->connect_succeeded) {
		/* Just got ready for first request */
		http_client_connection_ready(conn);
	}

	if (conn->to_input != NULL) {
		/* We came here from a timeout added by
		   http_client_payload_destroyed(). The IO couldn't be added
		   back immediately in there, because the HTTP API user may
		   still have had its own IO pointed to the same fd. It should
		   be removed by now, so we can add it back. */
		http_client_payload_finished(conn);
		finished++;
	}

	/* We've seen activity from the server; reset request timeout */
	http_client_connection_reset_request_timeout(conn);

	/* Get first waiting request */
	reqs = array_get(&conn->request_wait_list, &count);
	if (count > 0) {
		req = reqs[0];

		/* Determine whether to expect a response payload */
		payload_type = http_client_request_get_payload_type(req);
	} else {
		req = NULL;
		payload_type = HTTP_RESPONSE_PAYLOAD_TYPE_ALLOWED;
		i_assert(conn->to_requests == NULL);
	}

	/* Drop connection with broken output if last possible input was
	   received */
	if (conn->output_broken && (count == 0 ||
	    (count == 1 && req->state == HTTP_REQUEST_STATE_ABORTED))) {
		http_client_connection_server_close(&conn);
		return;
	}

	while ((ret = http_response_parse_next(conn->http_parser, payload_type,
					       &response, &error)) > 0) {
		ret = http_client_connection_process_response(conn, req,
							      &response);
		if (ret < 0)
			return;
		if (ret == 0)
			continue;

		finished++;

		/* Server closing connection? */
		if (conn->close_indicated) {
			http_client_connection_server_close(&conn);
			return;
		}

		/* Get next waiting request */
		reqs = array_get(&conn->request_wait_list, &count);
		if (count > 0) {
			req = reqs[0];

			/* Determine whether to expect a response payload */
			payload_type = http_client_request_get_payload_type(req);
		} else {
			/* No more requests waiting for the connection */
			req = NULL;
			payload_type = HTTP_RESPONSE_PAYLOAD_TYPE_ALLOWED;
			http_client_connection_stop_request_timeout(conn);
		}

		/* Drop connection with broken output if last possible input was
		   received */
		if (conn->output_broken && (count == 0 ||
		    (count == 1 && req->state == HTTP_REQUEST_STATE_ABORTED))) {
			http_client_connection_server_close(&conn);
			return;
		}
	}

	if (ret <= 0 &&
	    (conn->conn.input->eof || conn->conn.input->stream_errno != 0)) {
		int stream_errno = conn->conn.input->stream_errno;

		http_client_connection_lost(
			&conn,
			t_strdup_printf("read(%s) failed: %s",
					i_stream_get_name(conn->conn.input),
					(stream_errno != 0 ?
					 i_stream_get_error(conn->conn.input) :
					 "EOF")));
		return;
	}

	if (ret < 0) {
		http_client_connection_abort_error(
			&conn, HTTP_CLIENT_REQUEST_ERROR_BAD_RESPONSE, error);
		return;
	}

	if (finished > 0) {
		/* Connection still alive after (at least one) request;
		   we can pipeline -> mark for subsequent connections */
		pshared->allows_pipelining = TRUE;

		/* Room for new requests */
		if (peer != NULL &&
		    http_client_connection_check_ready(conn) > 0)
			http_client_peer_trigger_request_handler(peer);
	}
}

static int
http_client_connection_continue_request(struct http_client_connection *conn)
{
	struct http_client_connection *tmp_conn;
	struct http_client_request *const *reqs;
	unsigned int count;
	struct http_client_request *req;
	bool pipelined;
	int ret;

	reqs = array_get(&conn->request_wait_list, &count);
	i_assert(count > 0 || conn->to_requests == NULL);
	if (count == 0 || !conn->output_locked)
		return 1;

	req = reqs[count-1];
	pipelined = (count > 1 || conn->pending_request != NULL);

	if (req->state == HTTP_REQUEST_STATE_ABORTED) {
		e_debug(conn->event,
			"Request aborted before sending payload was complete.");
		if (count == 1) {
			http_client_connection_close(&conn);
			return -1;
		}
		o_stream_unset_flush_callback(conn->conn.output);
		conn->output_broken = TRUE;
		return -1;
	}

	if (req->payload_sync && !req->payload_sync_continue)
		return 1;

	tmp_conn = conn;
	http_client_connection_ref(tmp_conn);
	ret = http_client_request_send_more(req, pipelined);
	if (!http_client_connection_unref(&tmp_conn) || ret < 0)
		return -1;

	if (!conn->output_locked) {
		/* Room for new requests */
		if (http_client_connection_check_ready(conn) > 0)
			http_client_peer_trigger_request_handler(conn->peer);
	}
	return ret;
}

int http_client_connection_output(struct http_client_connection *conn)
{
	struct ostream *output = conn->conn.output;
	int ret;

	/* We've seen activity from the server; reset request timeout */
	http_client_connection_reset_request_timeout(conn);

	if ((ret = o_stream_flush(output)) <= 0) {
		if (ret < 0)
			http_client_connection_handle_output_error(conn);
		return ret;
	}

	i_assert(!conn->output_broken);

	if (conn->ssl_iostream != NULL &&
	    !ssl_iostream_is_handshaked(conn->ssl_iostream))
		return 1;

	return http_client_connection_continue_request(conn);
}

void http_client_connection_start_tunnel(struct http_client_connection **_conn,
					 struct http_client_tunnel *tunnel)
{
	struct http_client_connection *conn = *_conn;

	i_assert(conn->tunneling);

	/* Claim connection streams */
	i_zero(tunnel);
	tunnel->input = conn->conn.input;
	tunnel->output = conn->conn.output;
	tunnel->fd_in = conn->conn.fd_in;
	tunnel->fd_out = conn->conn.fd_out;

	/* Detach from connection */
	conn->conn.input = NULL;
	conn->conn.output = NULL;
	conn->conn.fd_in = -1;
	conn->conn.fd_out = -1;
	conn->closing = TRUE;
	conn->connected = FALSE;
	connection_disconnect(&conn->conn);

	http_client_connection_unref(_conn);
}

static void http_client_connection_ready(struct http_client_connection *conn)
{
	struct http_client_peer *peer = conn->peer;
	struct http_client_peer_pool *ppool = conn->ppool;
	struct http_client_peer_shared *pshared = ppool->peer;
	const struct http_client_settings *set =
		http_client_connection_get_settings(conn);

	e_debug(conn->event, "Ready for requests");
	i_assert(!conn->connect_succeeded);

	/* Connected */
	conn->connected = TRUE;
	conn->last_ioloop = current_ioloop;
	timeout_remove(&conn->to_connect);

	/* Indicate connection success */
	conn->connect_succeeded = TRUE;
	http_client_connection_unlist_pending(conn);
	http_client_peer_connection_success(peer);

	/* Start raw log */
	if (ppool->rawlog_dir != NULL) {
		iostream_rawlog_create(ppool->rawlog_dir,
				       &conn->conn.input, &conn->conn.output);
	}

	/* Direct tunneling connections handle connect requests just by
	   providing a raw connection */
	if (pshared->addr.type == HTTP_CLIENT_PEER_ADDR_RAW) {
		struct http_client_request *req;

		req = http_client_peer_claim_request(conn->peer, FALSE);
		if (req != NULL) {
			struct http_response response;

			conn->tunneling = TRUE;
			req->state = HTTP_REQUEST_STATE_WAITING;

			i_zero(&response);
			response.status = 200;
			response.reason = "OK";

			(void)http_client_connection_return_response(conn, req,
								     &response);
			return;
		}

		e_debug(conn->event,
			"No raw connect requests pending; "
			"closing useless connection");
		http_client_connection_close(&conn);
		return;
	}

	/* Start protocol I/O */
	struct http_header_limits limits = {
		.max_size = set->response_hdr_max_size,
		.max_field_size = set->response_hdr_max_field_size,
		.max_fields = set->response_hdr_max_fields,
	};
	conn->http_parser = http_response_parser_init(
		conn->conn.input, &limits, 0);
	o_stream_set_finish_via_child(conn->conn.output, FALSE);
	o_stream_set_flush_callback(conn->conn.output,
				    http_client_connection_output, conn);
}

static int
http_client_connection_ssl_handshaked(const char **error_r, void *context)
{
	struct http_client_connection *conn = context;
	struct http_client_peer_shared *pshared = conn->ppool->peer;
	const char *error, *host = pshared->addr.a.tcp.https_name;

	if (ssl_iostream_check_cert_validity(conn->ssl_iostream,
					     host, &error) == 0)
		e_debug(conn->event, "SSL handshake successful");
	else if (ssl_iostream_get_allow_invalid_cert(conn->ssl_iostream)) {
		e_debug(conn->event, "SSL handshake successful, "
			"ignoring invalid certificate: %s", error);
	} else {
		*error_r = error;
		return -1;
	}
	return 0;
}

static int
http_client_connection_ssl_init(struct http_client_connection *conn,
				const char **error_r)
{
	struct http_client_peer_pool *ppool = conn->ppool;
	struct http_client_peer_shared *pshared = ppool->peer;
	struct ssl_iostream_context *ssl_ctx = ppool->ssl_ctx;
	const char *error;

	i_assert(ssl_ctx != NULL);

	e_debug(conn->event, "Starting SSL handshake");

	connection_input_halt(&conn->conn);
	if (io_stream_create_ssl_client(ssl_ctx, pshared->addr.a.tcp.https_name,
					conn->event, 0,
					&conn->conn.input, &conn->conn.output,
					&conn->ssl_iostream, &error) < 0) {
		*error_r = t_strdup_printf(
			"Couldn't initialize SSL client for %s: %s",
			conn->conn.name, error);
		return -1;
	}
	connection_input_resume(&conn->conn);
	ssl_iostream_set_handshake_callback(
		conn->ssl_iostream,
		http_client_connection_ssl_handshaked, conn);
	if (ssl_iostream_handshake(conn->ssl_iostream) < 0) {
		*error_r = t_strdup_printf(
			"SSL handshake to %s failed: %s", conn->conn.name,
			ssl_iostream_get_last_error(conn->ssl_iostream));
		return -1;
	}

	if (ssl_iostream_is_handshaked(conn->ssl_iostream)) {
		http_client_connection_ready(conn);
	} else {
		/* Wait for handshake to complete; connection input handler does
		   the rest by reading from the input stream */
		o_stream_set_flush_callback(
			conn->conn.output, http_client_connection_output, conn);
	}
	return 0;
}

static void
http_client_connection_connected(struct connection *_conn, bool success)
{
	struct http_client_connection *conn =
		(struct http_client_connection *)_conn;
	struct http_client_peer_shared *pshared = conn->ppool->peer;
	const struct http_client_settings *set =
		http_client_connection_get_settings(conn);
	const char *error;

	if (!success) {
		http_client_connection_failure(
			conn, t_strdup_printf("connect(%s) failed: %m",
					      _conn->name));
	} else {
		conn->connected_timestamp = ioloop_timeval;
		e_debug(conn->event, "Connected");

		(void)net_set_tcp_nodelay(_conn->fd_out, TRUE);
		if (set->socket_send_buffer_size > 0 &&
		    net_set_send_buffer_size(
			_conn->fd_out, set->socket_send_buffer_size) < 0) {
			e_error(conn->event,
				"net_set_send_buffer_size(%"PRIuUOFF_T") failed: %m",
				set->socket_send_buffer_size);
		}
		if (set->socket_recv_buffer_size > 0 &&
		    net_set_recv_buffer_size(
			_conn->fd_in, set->socket_recv_buffer_size) < 0) {
			e_error(conn->event,
				"net_set_recv_buffer_size(%"PRIuUOFF_T") failed: %m",
				set->socket_recv_buffer_size);
		}

		if (http_client_peer_addr_is_https(&pshared->addr)) {
			if (http_client_connection_ssl_init(conn, &error) < 0) {
				e_debug(conn->event, "%s", error);
				http_client_connection_failure(conn, error);
				http_client_connection_close(&conn);
			}
			return;
		}
		http_client_connection_ready(conn);
	}
}

static const struct connection_settings http_client_connection_set = {
	.input_max_size = SIZE_MAX,
	.output_max_size = SIZE_MAX,
	.client = TRUE,
	.delayed_unix_client_connected_callback = TRUE,
	.log_connection_id = TRUE,
};

static const struct connection_vfuncs http_client_connection_vfuncs = {
	.destroy = http_client_connection_destroy,
	.input = http_client_connection_input,
	.client_connected = http_client_connection_connected,
};

struct connection_list *http_client_connection_list_init(void)
{
	return connection_list_init(&http_client_connection_set,
				    &http_client_connection_vfuncs);
}

static void http_client_connect_timeout(struct http_client_connection *conn)
{
	conn->conn.disconnect_reason = CONNECTION_DISCONNECT_CONNECT_TIMEOUT;
	http_client_connection_destroy(&conn->conn);
}

static void
http_client_connection_connect(struct http_client_connection *conn,
			       unsigned int timeout_msecs)
{
	struct http_client_context *cctx = conn->ppool->peer->cctx;

	conn->connect_start_timestamp = ioloop_timeval;
	if (connection_client_connect_async(&conn->conn) < 0) {
		e_debug(conn->event, "Connect failed: %m");
		return;
	}

	/* Don't use connection.h timeout because we want this timeout
	   to include also the SSL handshake */
	if (timeout_msecs > 0) {
		conn->to_connect = timeout_add_to(
			cctx->ioloop, timeout_msecs,
			http_client_connect_timeout, conn);
	}
}

static void
http_client_connect_tunnel_timeout(struct http_client_connection *conn)
{
	struct http_client_peer_shared *pshared = conn->ppool->peer;
	const char *error, *name = http_client_peer_addr2str(&pshared->addr);
	long long msecs;

	msecs = timeval_diff_msecs(&ioloop_timeval,
				   &conn->connect_start_timestamp);
	error = t_strdup_printf("Tunnel connect(%s) failed: "
				"Connection timed out in %lld.%03lld secs",
				name, msecs/1000, msecs%1000);

	e_debug(conn->event, "%s", error);
	http_client_connection_failure(conn, error);
	http_client_connection_close(&conn);
}

static void
http_client_connection_tunnel_response(const struct http_response *response,
				       struct http_client_connection *conn)
{
	struct http_client_peer_shared *pshared = conn->ppool->peer;
	struct http_client_context *cctx = pshared->cctx;
	struct http_client_tunnel tunnel;
	const char *name = http_client_peer_addr2str(&pshared->addr);
	struct http_client_request *req = conn->connect_request;

	conn->connect_request = NULL;

	if (response->status != 200) {
		http_client_connection_failure(
			conn,
			t_strdup_printf("Tunnel connect(%s) failed: %s", name,
					http_response_get_message(response)));
		return;
	}

	http_client_request_start_tunnel(req, &tunnel);

	conn->conn.event_parent = conn->event;
	connection_init_from_streams(cctx->conn_list, &conn->conn,
				     name, tunnel.input, tunnel.output);
	connection_switch_ioloop_to(&conn->conn, cctx->ioloop);
	i_stream_unref(&tunnel.input);
	o_stream_unref(&tunnel.output);
}

static void
http_client_connection_connect_tunnel(struct http_client_connection *conn,
				      const struct ip_addr *ip, in_port_t port,
				      unsigned int timeout_msecs)
{
	struct http_client_context *cctx = conn->ppool->peer->cctx;
	struct http_client *client = conn->peer->client;

	conn->connect_start_timestamp = ioloop_timeval;

	conn->connect_request = http_client_request_connect_ip(
		client, ip, port, http_client_connection_tunnel_response, conn);
	http_client_request_set_urgent(conn->connect_request);
	http_client_request_submit(conn->connect_request);

	/* Don't use connection.h timeout because we want this timeout
	   to include also the SSL handshake */
	if (timeout_msecs > 0) {
		conn->to_connect = timeout_add_to(
			cctx->ioloop, timeout_msecs,
			http_client_connect_tunnel_timeout, conn);
	}
}

struct http_client_connection *
http_client_connection_create(struct http_client_peer *peer)
{
	struct http_client_peer_shared *pshared = peer->shared;
	struct http_client_peer_pool *ppool = peer->ppool;
	struct http_client_context *cctx = pshared->cctx;
	struct http_client *client = peer->client;
	const struct http_client_settings *set = client->set;
	struct http_client_connection *conn;
	const struct http_client_peer_addr *addr = &pshared->addr;
	const char *conn_type;
	unsigned int timeout_msecs;

	switch (pshared->addr.type) {
	case HTTP_CLIENT_PEER_ADDR_HTTP:
		conn_type = "HTTP";
		break;
	case HTTP_CLIENT_PEER_ADDR_HTTPS:
		conn_type = "HTTPS";
		break;
	case HTTP_CLIENT_PEER_ADDR_HTTPS_TUNNEL:
		conn_type = "Tunneled HTTPS";
		break;
	case HTTP_CLIENT_PEER_ADDR_RAW:
		conn_type = "Raw";
		break;
	case HTTP_CLIENT_PEER_ADDR_UNIX:
		conn_type = "Unix";
		break;
	default:
		conn_type = "UNKNOWN";
		break;
	}

	timeout_msecs = set->connect_timeout_msecs;
	if (timeout_msecs == 0)
		timeout_msecs = set->request_timeout_msecs;

	conn = i_new(struct http_client_connection, 1);
	conn->refcount = 1;
	conn->ppool = ppool;
	conn->peer = peer;
	conn->set = client->set;
	pool_ref(conn->set->pool);
	if (pshared->addr.type != HTTP_CLIENT_PEER_ADDR_RAW)
		i_array_init(&conn->request_wait_list, 16);
	conn->io_wait_timer = io_wait_timer_add_to(cctx->ioloop);

	conn->conn.event_parent = ppool->peer->cctx->event;
	connection_init(cctx->conn_list, &conn->conn,
			http_client_peer_shared_label(pshared));
	conn->event = conn->conn.event;

	switch (pshared->addr.type) {
	case HTTP_CLIENT_PEER_ADDR_HTTPS_TUNNEL:
		http_client_connection_connect_tunnel(
			conn, &addr->a.tcp.ip, addr->a.tcp.port, timeout_msecs);
		break;
	case HTTP_CLIENT_PEER_ADDR_UNIX:
		connection_init_client_unix(cctx->conn_list, &conn->conn,
					    addr->a.un.path);
		connection_switch_ioloop_to(&conn->conn, cctx->ioloop);
		http_client_connection_connect(conn, timeout_msecs);
		break;
	default:
		connection_init_client_ip(cctx->conn_list, &conn->conn, NULL,
					  &addr->a.tcp.ip, addr->a.tcp.port);
		connection_switch_ioloop_to(&conn->conn, cctx->ioloop);
		http_client_connection_connect(conn, timeout_msecs);
	}

	array_push_back(&ppool->pending_conns, &conn);
	array_push_back(&ppool->conns, &conn);
	array_push_back(&peer->pending_conns, &conn);
	array_push_back(&peer->conns, &conn);

	http_client_peer_pool_ref(ppool);

	e_debug(conn->event,
		"%s connection created (%d parallel connections exist)%s",
		conn_type, array_count(&ppool->conns),
		(conn->to_input == NULL ? "" : " [broken]"));
	return conn;
}

void http_client_connection_ref(struct http_client_connection *conn)
{
	i_assert(conn->refcount > 0);
	conn->refcount++;
}

static void
http_client_connection_disconnect(struct http_client_connection *conn)
{
	struct http_client_peer_pool *ppool = conn->ppool;
	unsigned int idx;

	if (conn->disconnected)
		return;
	conn->disconnected = TRUE;

	e_debug(conn->event, "Connection disconnect");

	conn->closing = TRUE;
	conn->connected = FALSE;

	http_client_request_abort(&conn->connect_request);

	if (conn->incoming_payload != NULL) {
		/* The stream is still accessed by lib-http caller. */
		i_stream_remove_destroy_callback(conn->incoming_payload,
						 http_client_payload_destroyed);
		conn->incoming_payload = NULL;
	}

	if (conn->http_parser != NULL)
		http_response_parser_deinit(&conn->http_parser);

	connection_disconnect(&conn->conn);

	io_remove(&conn->io_req_payload);
	timeout_remove(&conn->to_requests);
	timeout_remove(&conn->to_connect);
	timeout_remove(&conn->to_input);
	timeout_remove(&conn->to_response);

	/* Remove this connection from the lists */
	if (!array_lsearch_ptr_idx(&ppool->conns, conn, &idx))
		i_unreached();
	array_delete(&ppool->conns, idx, 1);

	if (array_lsearch_ptr_idx(&ppool->pending_conns, conn, &idx))
		array_delete(&ppool->pending_conns, idx, 1);

	http_client_connection_detach_peer(conn);

	http_client_connection_stop_idle(conn); // FIXME: needed?
}

bool http_client_connection_unref(struct http_client_connection **_conn)
{
	struct http_client_connection *conn = *_conn;
	struct http_client_peer_pool *ppool = conn->ppool;

	i_assert(conn->refcount > 0);

	*_conn = NULL;

	if (--conn->refcount > 0)
		return TRUE;

	e_debug(conn->event, "Connection destroy");

	http_client_connection_disconnect(conn);
	http_client_connection_abort_any_requests(conn);

	i_assert(conn->io_req_payload == NULL);
	i_assert(conn->to_requests == NULL);
	i_assert(conn->to_connect == NULL);
	i_assert(conn->to_input == NULL);
	i_assert(conn->to_idle == NULL);
	i_assert(conn->to_response == NULL);

	if (array_is_created(&conn->request_wait_list))
		array_free(&conn->request_wait_list);

	ssl_iostream_destroy(&conn->ssl_iostream);
	connection_deinit(&conn->conn);
	io_wait_timer_remove(&conn->io_wait_timer);
	settings_free(conn->set);

	i_free(conn);

	http_client_peer_pool_unref(&ppool);
	return FALSE;
}

void http_client_connection_close(struct http_client_connection **_conn)
{
	struct http_client_connection *conn = *_conn;

	e_debug(conn->event, "Connection close");

	http_client_connection_disconnect(conn);
	http_client_connection_abort_any_requests(conn);
	http_client_connection_unref(_conn);
}

void http_client_connection_switch_ioloop(struct http_client_connection *conn)
{
	struct http_client_peer_shared *pshared = conn->ppool->peer;
	struct http_client_context *cctx = pshared->cctx;
	struct ioloop *ioloop = cctx->ioloop;

	connection_switch_ioloop_to(&conn->conn, ioloop);
	if (conn->io_req_payload != NULL) {
		conn->io_req_payload =
			io_loop_move_io_to(ioloop, &conn->io_req_payload);
	}
	if (conn->to_requests != NULL) {
		conn->to_requests =
			io_loop_move_timeout_to(ioloop, &conn->to_requests);
	}
	if (conn->to_connect != NULL) {
		conn->to_connect =
			io_loop_move_timeout_to(ioloop, &conn->to_connect);
	}
	if (conn->to_input != NULL) {
		conn->to_input =
			io_loop_move_timeout_to(ioloop, &conn->to_input);
	}
	if (conn->to_idle != NULL) {
		conn->to_idle =
			io_loop_move_timeout_to(ioloop, &conn->to_idle);
	}
	if (conn->to_response != NULL) {
		conn->to_response =
			io_loop_move_timeout_to(ioloop, &conn->to_response);
	}
	if (conn->incoming_payload != NULL)
		i_stream_switch_ioloop_to(conn->incoming_payload, ioloop);
	conn->io_wait_timer =
		io_wait_timer_move_to(&conn->io_wait_timer, ioloop);
}
