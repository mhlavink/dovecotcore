/* Copyright (c) 2002-2018 Dovecot authors, see the included COPYING file */

#include "imap-common.h"
#include "imap-commands.h"

bool cmd_check(struct client_command_context *cmd)
{
	if (!client_verify_open_mailbox(cmd))
		return TRUE;

	return cmd_sync(cmd, MAILBOX_SYNC_FLAG_FULL_READ, IMAP_SYNC_FLAG_SAFE,
			"OK Check completed.");
}
