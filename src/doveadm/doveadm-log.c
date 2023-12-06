/* Copyright (c) 2010-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "istream.h"
#include "hash.h"
#include "str.h"
#include "strescape.h"
#include "time-util.h"
#include "master-service-private.h"
#include "master-service-settings.h"
#include "log-error-buffer.h"
#include "doveadm.h"
#include "doveadm-print.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>

#define LAST_LOG_TYPE LOG_TYPE_PANIC
#define TEST_LOG_MSG_PREFIX "This is Dovecot's "
#define LOG_ERRORS_FNAME "log-errors"
#define LOG_TIMESTAMP_FORMAT "%b %d %H:%M:%S"

static void ATTR_NULL(2)
cmd_log_test(struct doveadm_cmd_context *cctx ATTR_UNUSED)
{
	struct failure_context ctx;
	unsigned int i;

	master_service->log_initialized = FALSE;
	master_service->flags |= MASTER_SERVICE_FLAG_DONT_LOG_TO_STDERR;
	master_service_init_log(master_service);

	i_zero(&ctx);
	for (i = 0; i < LAST_LOG_TYPE; i++) {
		const char *prefix = failure_log_type_prefixes[i];

		/* add timestamp so that syslog won't just write
		   "repeated message" text */
		ctx.type = i;
		i_log_type(&ctx, TEST_LOG_MSG_PREFIX"%s log (%u)",
			   t_str_lcase(t_strcut(prefix, ':')),
			   ioloop_time32);
	}
}

static void cmd_log_reopen(struct doveadm_cmd_context *cctx)
{
	doveadm_master_send_signal(SIGUSR1, cctx->event);
}

struct log_find_file {
	const char *path;
	uoff_t size;

	/* 1 << enum log_type */
	unsigned int mask;
};

struct log_find_context {
	pool_t pool;
	HASH_TABLE(char *, struct log_find_file *) files;
};

static void cmd_log_find_add(struct log_find_context *ctx,
			     const char *path, enum log_type type)
{
	struct log_find_file *file;
	char *key;

	file = hash_table_lookup(ctx->files, path);
	if (file == NULL) {
		file = p_new(ctx->pool, struct log_find_file, 1);
		file->path = key = p_strdup(ctx->pool, path);
		hash_table_insert(ctx->files, key, file);
	}

	file->mask |= 1 << type;
}

static void
cmd_log_find_syslog_files(struct log_find_context *ctx, const char *path,
			  struct event *event)
{
	struct log_find_file *file;
	DIR *dir;
	struct dirent *d;
	struct stat st;
	char *key;
	string_t *full_path;
	size_t dir_len;

	dir = opendir(path);
	if (dir == NULL) {
		e_error(event, "opendir(%s) failed: %m", path);
		return;
	}

	full_path = t_str_new(256);
	str_append(full_path, path);
	str_append_c(full_path, '/');
	dir_len = str_len(full_path);

	while ((d = readdir(dir)) != NULL) {
		if (d->d_name[0] == '.')
			continue;

		str_truncate(full_path, dir_len);
		str_append(full_path, d->d_name);
		if (stat(str_c(full_path), &st) < 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			/* recursively go through all subdirectories */
			cmd_log_find_syslog_files(ctx, str_c(full_path), event);
		} else if (hash_table_lookup(ctx->files,
					     str_c(full_path)) == NULL) {
			file = p_new(ctx->pool, struct log_find_file, 1);
			file->size = st.st_size;
			file->path = key =
				p_strdup(ctx->pool, str_c(full_path));
			hash_table_insert(ctx->files, key, file);
		}
	}

	(void)closedir(dir);
}

static bool log_type_find(const char *str, enum log_type *type_r)
{
	const char *suffix;
	unsigned int i;

	for (i = 0; i < LAST_LOG_TYPE; i++) {
		if (str_begins_icase(failure_log_type_prefixes[i], str, &suffix) &&
		    suffix[0] == ':') {
			*type_r = i;
			return TRUE;
		}
	}
	return FALSE;
}

static void cmd_log_find_syslog_file_messages(struct log_find_file *file)
{
	struct istream *input;
	const char *line, *p;
	enum log_type type;
	int fd;

	fd = open(file->path, O_RDONLY);
	if (fd == -1)
		return;

	input = i_stream_create_fd_autoclose(&fd, 1024);
	i_stream_seek(input, file->size);
	while ((line = i_stream_read_next_line(input)) != NULL) {
		p = strstr(line, TEST_LOG_MSG_PREFIX);
		if (p == NULL)
			continue;
		p += strlen(TEST_LOG_MSG_PREFIX);

		/* <type> log */
		T_BEGIN {
			if (log_type_find(t_strcut(p, ' '), &type))
				file->mask |= 1 << type;
		} T_END;
	}
	i_stream_destroy(&input);
}

static void cmd_log_find_syslog_messages(struct log_find_context *ctx)
{
	struct hash_iterate_context *iter;
	struct stat st;
	char *key;
	struct log_find_file *file;

	iter = hash_table_iterate_init(ctx->files);
	while (hash_table_iterate(iter, ctx->files, &key, &file)) {
		if (stat(file->path, &st) < 0 ||
		    (uoff_t)st.st_size <= file->size)
			continue;

		cmd_log_find_syslog_file_messages(file);
	}
	hash_table_iterate_deinit(&iter);
}

static void
cmd_log_find_syslog(struct log_find_context *ctx,
		    struct doveadm_cmd_context *cctx)
{
	const char *log_dir;
	struct stat st;

	if (doveadm_cmd_param_str(cctx, "log-dir", &log_dir))
		;
	else if (stat("/var/log", &st) == 0 && S_ISDIR(st.st_mode))
		log_dir = "/var/log";
	else if (stat("/var/adm", &st) == 0 && S_ISDIR(st.st_mode))
		log_dir = "/var/adm";
	else
		return;

	printf("Looking for log files from %s\n", log_dir);
	cmd_log_find_syslog_files(ctx, log_dir, cctx->event);
	cmd_log_test(cctx);

	/* give syslog some time to write the messages to files */
	sleep(1);
	cmd_log_find_syslog_messages(ctx);
}

static void cmd_log_find(struct doveadm_cmd_context *cctx)
{
	const struct master_service_settings *set;
	const char *log_file_path;
	struct log_find_context ctx;
	unsigned int i;

	i_zero(&ctx);
	ctx.pool = pool_alloconly_create("log file", 1024*32);
	hash_table_create(&ctx.files, ctx.pool, 0, str_hash, strcmp);

	/* first get the paths that we know are used */
	set = master_service_get_service_settings(master_service);
	log_file_path = set->log_path;
	if (strcmp(log_file_path, "syslog") == 0)
		log_file_path = "";
	if (*log_file_path != '\0') {
		cmd_log_find_add(&ctx, log_file_path, LOG_TYPE_WARNING);
		cmd_log_find_add(&ctx, log_file_path, LOG_TYPE_ERROR);
		cmd_log_find_add(&ctx, log_file_path, LOG_TYPE_FATAL);
	}

	if (strcmp(set->info_log_path, "syslog") != 0) {
		if (*set->info_log_path != '\0')
			log_file_path = set->info_log_path;
		if (*log_file_path != '\0')
			cmd_log_find_add(&ctx, log_file_path, LOG_TYPE_INFO);
	}

	if (strcmp(set->debug_log_path, "syslog") != 0) {
		if (*set->debug_log_path != '\0')
			log_file_path = set->debug_log_path;
		if (*log_file_path != '\0')
			cmd_log_find_add(&ctx, log_file_path, LOG_TYPE_DEBUG);
	}

	if (*set->log_path == '\0' ||
	    strcmp(set->log_path, "syslog") == 0 ||
	    strcmp(set->info_log_path, "syslog") == 0 ||
	    strcmp(set->debug_log_path, "syslog") == 0) {
		/* at least some logs were logged via syslog */
		cmd_log_find_syslog(&ctx, cctx);
	}

	/* print them */
	for (i = 0; i < LAST_LOG_TYPE; i++) {
		struct hash_iterate_context *iter;
		char *key;
		struct log_find_file *file;
		bool found = FALSE;

		iter = hash_table_iterate_init(ctx.files);
		while (hash_table_iterate(iter, ctx.files, &key, &file)) {
			if ((file->mask & (1 << i)) != 0) {
				printf("%s%s\n", failure_log_type_prefixes[i],
				       file->path);
				found = TRUE;
			}
		}
		hash_table_iterate_deinit(&iter);

		if (!found)
			printf("%sNot found\n", failure_log_type_prefixes[i]);
	}
	hash_table_destroy(&ctx.files);
	pool_unref(&ctx.pool);
}

static const char *t_cmd_log_error_trim(const char *orig)
{
	size_t pos;

	/* Trim whitespace from suffix and remove ':' if it exists */
	for (pos = strlen(orig); pos > 0; pos--) {
		if (orig[pos-1] != ' ') {
			if (orig[pos-1] == ':')
				pos--;
			break;
		}
	}
	return orig[pos] == '\0' ? orig : t_strndup(orig, pos);
}

static bool
cmd_log_error_next(struct event *event, struct istream *input,
		   struct log_error *error_r)
{
	if (error_r->text != NULL) {
		/* already set */
		return TRUE;
	}

	i_zero(error_r);
	if (input == NULL || input->closed) {
		/* already logged failure */
		return FALSE;
	}

	const char *line = i_stream_read_next_line(input);
	if (line == NULL) {
		if (input->stream_errno != 0) {
			e_error(event, "read() failed: %s",
				i_stream_get_error(input));
		}
		return FALSE;
	}

	if (line[0] == '\0') {
		/* end of lines reply from master */
		i_stream_close(input);
		return FALSE;
	}

	/* <type> <timestamp> <prefix> <text> */
	const char *const *args = t_strsplit_tabescaped(line);

	if (str_array_length(args) != 4) {
		e_error(event, "Invalid input from log: %s", line);
		doveadm_exit_code = EX_PROTOCOL;
		i_stream_close(input);
		return FALSE;
	}

	/* find type's prefix */
	enum log_type type;
	for (type = 0; type < LOG_TYPE_COUNT; type++) {
		if (strcmp(args[0], failure_log_type_names[type]) == 0) {
			error_r->type = type;
			break;
		}
	}
	if (type == LOG_TYPE_COUNT) {
		e_error(event, "Invalid log type: %s", args[0]);
		error_r->type = LOG_TYPE_ERROR;
	}

	if (str_to_timeval(args[1], &error_r->timestamp) < 0)
		e_error(event, "Invalid timestamp: %s", args[1]);
	error_r->prefix = args[2];
	error_r->text = args[3];
	return TRUE;
}

static void cmd_log_error_write(const struct log_error *error)
{
	const char *ts_secs =
		t_strflocaltime(LOG_TIMESTAMP_FORMAT, error->timestamp.tv_sec);
	doveadm_print(t_strdup_printf("%s.%06u", ts_secs,
				      (unsigned int)error->timestamp.tv_usec));
	if (error->prefix[0] == '\0')
		doveadm_print("");
	else {
		const char *prefix = t_strconcat(
			t_cmd_log_error_trim(error->prefix), ": ", NULL);
		doveadm_print(prefix);
	}
	doveadm_print(t_cmd_log_error_trim(failure_log_type_prefixes[error->type]));
	doveadm_print(error->text);
}

static void cmd_log_errors(struct doveadm_cmd_context *cctx)
{
	struct istream *input1 = NULL, *input2 = NULL;
	const char *error, *path1;
	time_t min_timestamp = 0;
	int64_t since_int64;
	int fd;

	if (doveadm_cmd_param_int64(cctx, "since", &since_int64))
		min_timestamp = since_int64;

	path1 = t_strconcat(doveadm_settings->base_dir,
			    "/"LOG_ERRORS_FNAME, NULL);
	fd = net_connect_unix(path1);
	if (fd == -1)
		e_error(cctx->event, "net_connect_unix(%s) failed: %m", path1);
	else {
		net_set_nonblock(fd, FALSE);
		input1 = i_stream_create_fd_autoclose(&fd, SIZE_MAX);
	}

	if (master_service_send_cmd("ERROR-LOG", &input2, &error) < 0) {
		e_error(cctx->event, "%s", error);
		input2 = NULL;
	}

	doveadm_print_init(DOVEADM_PRINT_TYPE_FORMATTED);
	doveadm_print_formatted_set_format("%{timestamp} %{type}: %{prefix}%{text}\n");

	doveadm_print_header_simple("timestamp");
	doveadm_print_header_simple("prefix");
	doveadm_print_header_simple("type");
	doveadm_print_header_simple("text");

	struct log_error error1, error2;
	i_zero(&error1);
	i_zero(&error2);
	for (;;) {
		bool have1 = cmd_log_error_next(cctx->event, input1, &error1);
		bool have2 = cmd_log_error_next(cctx->event, input2, &error2);
		if (!have1 && !have2)
			break;

		struct log_error *error;
		if (error2.text == NULL ||
		    (error1.text != NULL &&
		     timeval_cmp(&error1.timestamp, &error2.timestamp) < 0)) {
			i_assert(error1.text != NULL);
			error = &error1;
		} else {
			if (error2.prefix[0] == '\0')
				error2.prefix = "master: ";
			error = &error2;
		}
		if (error->timestamp.tv_sec >= min_timestamp) T_BEGIN {
			cmd_log_error_write(error);
		} T_END;
		i_zero(error);
	}
	i_stream_destroy(&input1);
	i_stream_destroy(&input2);
}

struct doveadm_cmd_ver2 doveadm_cmd_log[] = {
{
	.name = "log test",
	.cmd = cmd_log_test,
	.usage = "",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAMS_END
},
{
	.name = "log reopen",
	.cmd = cmd_log_reopen,
	.usage = "",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAMS_END
},
{
	.name = "log find",
	.cmd = cmd_log_find,
	.usage = "[<dir>]",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "log-dir", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
},
{
	.name = "log errors",
	.usage = "[-s <min_timestamp>]",
	.cmd = cmd_log_errors,
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('s', "since", CMD_PARAM_INT64, CMD_PARAM_FLAG_UNSIGNED)
DOVEADM_CMD_PARAMS_END
}
};

void doveadm_register_log_commands(void)
{
	unsigned int i;

	for (i = 0; i < N_ELEMENTS(doveadm_cmd_log); i++)
		doveadm_cmd_register_ver2(&doveadm_cmd_log[i]);
}
