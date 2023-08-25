/* Copyright (c) 2009-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "sort.h"
#include "ostream.h"
#include "env-util.h"
#include "execv-const.h"
#include "dict.h"
#include "master-service-private.h"
#include "master-service-settings.h"
#include "settings-parser.h"
#include "doveadm-print-private.h"
#include "doveadm-dump.h"
#include "doveadm-mail.h"
#include "doveadm-settings.h"
#include "doveadm-dsync.h"
#include "doveadm.h"

#include <getopt.h>
#include <unistd.h>

const struct doveadm_print_vfuncs *doveadm_print_vfuncs_all[] = {
	&doveadm_print_flow_vfuncs,
	&doveadm_print_tab_vfuncs,
	&doveadm_print_table_vfuncs,
	&doveadm_print_pager_vfuncs,
	&doveadm_print_json_vfuncs,
	&doveadm_print_formatted_vfuncs,
	NULL
};

int doveadm_exit_code = 0;

static void failure_exit_callback(int *status)
{
	enum fatal_exit_status fatal_status = *status;

	switch (fatal_status) {
	case FATAL_LOGWRITE:
	case FATAL_LOGERROR:
	case FATAL_LOGOPEN:
	case FATAL_OUTOFMEM:
	case FATAL_EXEC:
	case FATAL_DEFAULT:
		*status = EX_TEMPFAIL;
		break;
	}
}

static void
usage_commands_write(FILE *out, const ARRAY_TYPE(doveadm_cmd_ver2_p) *cmds,
		     const char *prefix)
{
	const struct doveadm_cmd_ver2 *cur_cmd;
	const char *p, *short_name, *sub_name;
	const char *prev_name = "", *prev_sub_name = "";
	size_t prefix_len = strlen(prefix);

	/* print lines, compress subcommands into a single line */
	array_foreach_elem(cmds, cur_cmd) {
		const char *cmd = cur_cmd->name;
		const char *args = cur_cmd->usage;

		if (*prefix != '\0') {
			if (strncmp(cmd, prefix, prefix_len) != 0 ||
			    cmd[prefix_len] != ' ')
				continue;
			cmd += prefix_len + 1;
		}

		p = strchr(cmd, ' ');
		if (p == NULL) {
			if (*prev_name != '\0') {
				fprintf(out, "\n");
				prev_name = "";
			}
			fprintf(out, USAGE_CMDNAME_FMT" %s\n", cmd, args);
		} else {
			short_name = t_strdup_until(cmd, p);
			if (strcmp(prev_name, short_name) != 0) {
				if (*prev_name != '\0')
					fprintf(out, "\n");
				fprintf(out, USAGE_CMDNAME_FMT" %s",
					short_name, t_strcut(p + 1, ' '));
				prev_name = short_name;
				prev_sub_name = "";
			} else {
				sub_name = t_strcut(p + 1, ' ');
				if (strcmp(prev_sub_name, sub_name) != 0) {
					fprintf(out, "|%s", sub_name);
					prev_sub_name = sub_name;
				}
			}
		}
	}
	if (*prev_name != '\0')
		fprintf(out, "\n");
}

static int doveadm_cmd_cmp(const struct doveadm_cmd_ver2 *const *cmd1,
			   const struct doveadm_cmd_ver2 *const *cmd2)
{
	return strcmp((*cmd1)->name, (*cmd2)->name);
}

static void ATTR_NORETURN
usage_prefix(FILE *out, const char *prefix, int return_code)
{
	const struct doveadm_cmd_ver2 *cmd2;
	ARRAY_TYPE(doveadm_cmd_ver2_p) visible_cmds;

	t_array_init(&visible_cmds, array_count(&doveadm_cmds_ver2));

	fprintf(out, "usage: doveadm [-Dv] [-f <formatter>] ");
	if (*prefix != '\0')
		fprintf(out, "%s ", prefix);
	fprintf(out, "<command> [<args>]\n");

	array_foreach(&doveadm_cmds_ver2, cmd2) {
		if ((cmd2->flags & CMD_FLAG_HIDDEN) == 0)
			array_push_back(&visible_cmds, &cmd2);
	}
	/* sort commands */
	array_sort(&visible_cmds, doveadm_cmd_cmp);

	usage_commands_write(out, &visible_cmds, prefix);

	lib_exit(return_code);
}

void usage(void)
{
	usage_prefix(stderr, "", EX_USAGE);
}

static void ATTR_NORETURN
print_usage_and_exit(FILE *out, const struct doveadm_cmd_ver2 *cmd, int exit_code)
{
	fprintf(out, "doveadm %s %s\n", cmd->name, cmd->usage);
	lib_exit(exit_code);
}

void help_ver2(const struct doveadm_cmd_ver2 *cmd)
{
	print_usage_and_exit(stderr, cmd, EX_USAGE);
}

static void cmd_help(struct doveadm_cmd_context *cctx)
{
	const char *cmd, *man_argv[3];

	if (!doveadm_cmd_param_str(cctx, "cmd", &cmd))
		usage_prefix(stdout, "", EX_OK);

	env_put("MANPATH", MANDIR);
	man_argv[0] = "man";
	man_argv[1] = t_strconcat("doveadm-", cmd, NULL);
	man_argv[2] = NULL;
	execvp_const(man_argv[0], man_argv);
}

static struct doveadm_cmd_ver2 doveadm_cmd_help = {
	.name = "help",
	.cmd = cmd_help,
	.usage = "[<cmd>]",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "cmd", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
};

static void cmd_config(struct doveadm_cmd_context *cctx)
{
	const char *const *args, **argv;

	if (!doveadm_cmd_param_array(cctx, "args", &args))
		args = NULL;

	env_put(MASTER_CONFIG_FILE_ENV,
		master_service_get_config_path(master_service));

	unsigned int len = str_array_length(args);
	argv = t_new(const char *, len + 2);
	argv[0] = BINDIR"/doveconf";
	if (len > 0) {
		i_assert(args != NULL);
		memcpy(argv+1, args, len * sizeof(args[0]));
	}
	execv_const(argv[0], argv);
}

static struct doveadm_cmd_ver2 doveadm_cmd_config = {
	.name = "config",
	.cmd = cmd_config,
	.usage = "[doveconf parameters]",
	.flags = CMD_FLAG_NO_OPTIONS,
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "args", CMD_PARAM_ARRAY, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
};

static void cmd_exec(struct doveadm_cmd_context *cctx);
static struct doveadm_cmd_ver2 doveadm_cmd_exec = {
	.name = "exec",
	.cmd = cmd_exec,
	.usage = "<binary> [binary parameters]",
	.flags = CMD_FLAG_NO_OPTIONS,
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "binary", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "args", CMD_PARAM_ARRAY, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
};

static void cmd_exec(struct doveadm_cmd_context *cctx)
{
	const char *path, *binary, *const *args, **argv;

	if (!doveadm_cmd_param_str(cctx, "binary", &binary))
		help_ver2(&doveadm_cmd_exec);
	if (!doveadm_cmd_param_array(cctx, "args", &args))
		args = NULL;

	/* Avoid re-executing doveconf after the binary is executed */
	int config_fd = doveadm_settings_get_config_fd();
	fd_close_on_exec(config_fd, FALSE);
	env_put(DOVECOT_CONFIG_FD_ENV, dec2str(config_fd));

	path = t_strdup_printf("%s/%s", doveadm_settings->libexec_dir, binary);

	unsigned int len = str_array_length(args);
	argv = t_new(const char *, len + 2);
	argv[0] = path;
	if (len > 0) {
		i_assert(args != NULL);
		memcpy(argv+1, args, len * sizeof(args[0]));
	}
	execv_const(argv[0], argv);
}

static bool doveadm_has_subcommands(const char *cmd_name)
{
	const struct doveadm_cmd_ver2 *cmd2;
	size_t len = strlen(cmd_name);

	array_foreach(&doveadm_cmds_ver2, cmd2) {
		if (strncmp(cmd2->name, cmd_name, len) == 0 &&
		    cmd2->name[len] == ' ')
			return TRUE;
	}
	return FALSE;
}

static struct doveadm_cmd_ver2 *doveadm_cmdline_commands_ver2[] = {
	&doveadm_cmd_config,
	&doveadm_cmd_dump,
	&doveadm_cmd_exec,
	&doveadm_cmd_help,
	&doveadm_cmd_pw,
	&doveadm_cmd_compress_connect,
};

int main(int argc, char *argv[])
{
	enum master_service_flags service_flags =
		MASTER_SERVICE_FLAG_STANDALONE |
		MASTER_SERVICE_FLAG_NO_SSL_INIT |
		MASTER_SERVICE_FLAG_NO_INIT_DATASTACK_FRAME;
	const char *cmd_name;
	unsigned int i;
	bool quick_init = FALSE;
	int c;

	/* "+" is GNU extension to stop at the first non-option.
	   others just accept -+ option. */
	master_service = master_service_init("doveadm", service_flags,
					     &argc, &argv, "+Df:hv");
	const struct option longopts[] = {
		master_service_helpopt,
		{NULL, 0, NULL, 0},
	};
	master_service_register_long_options(master_service, longopts);

	i_set_failure_exit_callback(failure_exit_callback);

	bool help_requested = FALSE;
	const char *longopt = NULL;
	while ((c = master_getopt_long(master_service, &longopt)) >= 0) {
		switch (c) {
		case 0:
			if (strcmp(longopt, "help") == 0)
				help_requested = TRUE;
			break;
		case 'D':
			doveadm_debug = TRUE;
			doveadm_verbose = TRUE;
			break;
		case 'f':
			doveadm_print_init(optarg);
			break;
		case 'h':
			doveadm_print_hide_titles = TRUE;
			break;
		case 'v':
			doveadm_verbose = TRUE;
			break;
		default:
			fprintf(stderr, "%s\n",
				"Use doveadm --help for a list of available "
				"options and commands.");
			return EX_USAGE;
		}
	}
	cmd_name = argv[optind];

	if (cmd_name != NULL && strcmp(cmd_name, "help") == 0 &&
	    argv[optind+1] != NULL) {
		/* "help cmd" doesn't need any configuration */
		quick_init = TRUE;
	}
	master_service_init_log(master_service);

	doveadm_settings_init();
	doveadm_cmds_init();
	for (i = 0; i < N_ELEMENTS(doveadm_cmdline_commands_ver2); i++)
		doveadm_cmd_register_ver2(doveadm_cmdline_commands_ver2[i]);
	doveadm_register_auth_commands();

	if (cmd_name != NULL && (quick_init ||
				 strcmp(cmd_name, "config") == 0 ||
				 strcmp(cmd_name, "stop") == 0 ||
				 strcmp(cmd_name, "reload") == 0)) {
		/* special case commands: even if there is something wrong
		   with the config (e.g. mail_plugins), don't fail these
		   commands */
		master_service->flags |= MASTER_SERVICE_FLAG_DONT_SEND_STATS;
		if (strcmp(cmd_name, "help") != 0)
			doveadm_read_settings();
		quick_init = TRUE;
	} else {
		quick_init = FALSE;
		doveadm_print_ostream = o_stream_create_fd(STDOUT_FILENO, 0);
		o_stream_set_no_error_handling(doveadm_print_ostream, TRUE);
		o_stream_cork(doveadm_print_ostream);
		doveadm_dump_init();
		doveadm_mail_init();
		dict_drivers_register_builtin();
		doveadm_load_modules();

		/* read settings only after loading doveadm plugins, which
		   may modify what settings are read */
		doveadm_read_settings();
		if (doveadm_debug && getenv("LOG_STDERR_TIMESTAMP") == NULL)
			i_set_failure_timestamp_format(master_service->set->log_timestamp);
		master_service_init_stats_client(master_service, TRUE);
		/* Load mail_plugins */
		doveadm_mail_init_finish();
		/* kludgy: Load the rest of the doveadm plugins after
		   mail_plugins have been loaded. */
		doveadm_load_modules();

		/* show usage after registering all plugins */
		if (cmd_name == NULL) {
			FILE *out = help_requested ? stdout : stderr;
			int exit_code = help_requested ? EX_OK : EX_USAGE;
			usage_prefix(out, "", exit_code);
		}
	}

	argc -= optind;
	argv += optind;
	i_getopt_reset();

	master_service_init_finish(master_service);
	if (!doveadm_debug) {
		/* disable debugging unless -D is given */
		i_set_debug_file("/dev/null");
	}

	struct doveadm_cmd_context *cctx = doveadm_cmd_context_create(
		DOVEADM_CONNECTION_TYPE_CLI, doveadm_debug);
	/* this has to be done here because proctitle hack can break
	   the env pointer */
	cctx->username = getenv("USER");

	if (!doveadm_cmdline_try_run(cmd_name, argc, (const char**)argv, cctx)) {
		help_requested = cctx->help_requested != DOVEADM_CMD_VER2_NO_HELP;
		FILE *out = help_requested ? stdout : stderr;
		int exit_code = help_requested ? EX_OK : EX_USAGE;
		if (cctx->help_requested == DOVEADM_CMD_VER2_HELP_ARGUMENT &&
		    cctx->cmd != NULL) {
			print_usage_and_exit(stdout, cctx->cmd, EX_OK);
		}
		if (doveadm_has_subcommands(cmd_name))
			usage_prefix(out, cmd_name, exit_code);

		if (doveadm_has_unloaded_plugin(cmd_name)) {
			i_fatal("Unknown command '%s', but plugin %s exists. "
				"Try to set mail_plugins=%s",
				cmd_name, cmd_name, cmd_name);
		}
		usage();
	}
	if (cctx->referral != NULL)
		i_fatal("Command requested referral: %s", cctx->referral);

	doveadm_cmd_context_unref(&cctx);
	if (!quick_init) {
		doveadm_mail_deinit();
		doveadm_dump_deinit();
		doveadm_unload_modules();
		dict_drivers_unregister_builtin();
		doveadm_print_deinit();
		o_stream_unref(&doveadm_print_ostream);
	}
	doveadm_cmds_deinit();
	doveadm_settings_deinit();
	master_service_deinit(&master_service);
	return doveadm_exit_code;
}
