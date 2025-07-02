/* Copyright (c) 2010-2022 Dovecot authors, see the included COPYING file */

#include "imap-common.h"
#include "imap-commands.h"
#include "istream.h"
#include "ostream.h"
#include "ostream-multiplex.h"
#include "iostream-rawlog.h"
#include "str.h"
#include "strescape.h"
#include "compression.h"

static void client_skip_line(struct client *client)
{
	const unsigned char *data;
	size_t data_size;

	data = i_stream_get_data(client->input, &data_size);
	i_assert(data_size > 0);
	if (data[0] == '\n')
		i_stream_skip(client->input, 1);
	else if (data[0] == '\r' && data_size > 1 && data[1] == '\n')
		i_stream_skip(client->input, 2);
	else
		i_unreached();
	client->input_skip_line = FALSE;
}

static void client_update_imap_parser_streams(struct client *client)
{
	struct client_command_context *cmd;

	if (client->free_parser != NULL) {
		imap_parser_set_streams(client->free_parser,
					client->input, client->output);
	}

	for (cmd = client->command_queue; cmd != NULL; cmd = cmd->next) {
		imap_parser_set_streams(cmd->parser,
					client->input, client->output);
	}
}

bool cmd_compress(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	const struct compression_handler *handler;
	const struct imap_arg *args;
	struct istream *old_input;
	struct ostream *old_output;
	const char *mechanism;

	/* <mechanism> */
	if (!client_read_args(cmd, 0, 0, &args))
		return FALSE;

	if (!imap_arg_get_atom(args, &mechanism) ||
	    !IMAP_ARG_IS_EOL(&args[1])) {
		client_send_command_error(cmd, "Invalid arguments.");
		return TRUE;
	}

	if (client->compress_handler != NULL) {
		client_send_tagline(cmd, t_strdup_printf(
			"NO [COMPRESSIONACTIVE] COMPRESSION=%s already enabled.",
			t_str_ucase(client->compress_handler->name)));
		return TRUE;
	}
	int ret = compression_lookup_handler(t_str_lcase(mechanism), &handler);
	if (ret <= 0) {
		const char * tagline =
			t_strdup_printf("NO %s compression mechanism",
					ret == 0 ? "Unsupported" : "Unknown");
		client_send_tagline(cmd, tagline);
		return TRUE;
	}

	client_skip_line(client);

	/* Client cannot assume that COMPRESS works and just start sending
	   compressed data pipelined. Add an explicit sanity check for this. */
	if (i_stream_get_data_size(client->input) > 0) {
		client_send_tagline(cmd, "BAD Client did not wait for COMPRESS reply before sending more data");
		return TRUE;
	}

	if (client->multiplex_output != NULL) {
		/* Let imap-login process handle the COMPRESS. It's the one
		   that will send the tagged reply to the client. */
		client->compress_handler = handler;
		if (client->side_channel_output == NULL) {
			client->side_channel_output =
				o_stream_multiplex_add_channel(
					client->multiplex_output, 1);
			o_stream_set_no_error_handling(
				client->side_channel_output, TRUE);
		}
		string_t *str = t_str_new(64);
		str_append(str, "compress\t");
		str_append_tabescaped(str, handler->name);
		str_append_c(str, '\t');
		str_append_tabescaped(str, cmd->tag);
		str_append_c(str, '\n');
		o_stream_nsend(client->side_channel_output,
			       str_data(str), str_len(str));
		return TRUE;
	}

	client_send_tagline(cmd, "OK Begin compression.");

	uoff_t prev_out_offset = client->output->offset;
	if (client->pre_rawlog_input != NULL) {
		/* Rawlogging is currently enabled. Stop it. */
		i_assert(client->pre_rawlog_output != NULL);
		i_assert(client->pre_rawlog_input != client->post_rawlog_input);
		i_assert(client->pre_rawlog_output != client->post_rawlog_output);
		uoff_t prev_in_offset = client->input->v_offset;
		/* Make sure the rawlog is the outermost stream, since we
		   can't remove it from the middle */
		i_assert(client->post_rawlog_input == client->input);
		i_assert(client->post_rawlog_output == client->output);
		/* Pre-rawlog streams are referenced only by the outermost
		   stream, so make sure they don't get destroyed */
		client->input = client->pre_rawlog_input;
		client->output = client->pre_rawlog_output;
		i_stream_ref(client->input);
		o_stream_ref(client->output);
		/* Destroy the rawlog streams. This closes the rawlogs, but
		   not the parent streams. */
		i_stream_destroy(&client->post_rawlog_input);
		o_stream_destroy(&client->post_rawlog_output);
		io_remove(&client->io);
		/* Make sure istream-rawlog updated the parent stream's seek
		   offset. */
		i_assert(client->input->v_offset == prev_in_offset);
	}

	old_input = client->input;
	old_output = client->output;
	client->input = handler->create_istream(old_input);
	client->output = handler->create_ostream_auto(old_output, cmd->event);
	/* preserve output offset so that the bytes out counter in logout
	   message doesn't get reset here */
	client->output->offset = old_output->offset;
	i_stream_unref(&old_input);
	o_stream_unref(&old_output);

	if (client->pre_rawlog_input != NULL) {
		(void)iostream_rawlog_create(client->set->rawlog_dir,
					     &client->input, &client->output);
		client->post_rawlog_input = client->input;
		client->post_rawlog_output = client->output;
		/* retain previous output offset, used in command_stats_flush()
		   to correctly calculate the already written bytes */
		client->prev_output_size = prev_out_offset;
		client_add_missing_io(client);
	}

	client_update_imap_parser_streams(client);
	client->compress_handler = handler;
	return TRUE;
}
