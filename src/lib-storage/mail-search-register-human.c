/* Copyright (c) 2010-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "str.h"
#include "unichar.h"
#include "str-parse.h"
#include "mail-storage.h"
#include "mail-search-register.h"
#include "mail-search-parser.h"
#include "mail-search-build.h"

#include <time.h>
#include <ctype.h>

struct mail_search_register *mail_search_register_human;

static struct mail_search_arg *
human_search_or(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;

	/* this changes the parent arg to be an OR block instead of AND block */
	ctx->parent->type = SEARCH_OR;
	if (mail_search_build_key(ctx, ctx->parent, &sarg) < 0)
		return NULL;
	return sarg;
}

static struct mail_search_arg *
arg_new_human_date(struct mail_search_build_context *ctx,
		   enum mail_search_arg_type type,
		   enum mail_search_date_type date_type)
{
	struct mail_search_arg *sarg;
	const char *value;
	bool utc;

	sarg = mail_search_build_new(ctx, type);
	if (mail_search_parse_string(ctx->parser, &value) < 0)
		return NULL;

	if (mail_parse_human_timestamp(value, &sarg->value.time, &utc) < 0)
		sarg->value.time = (time_t)-1;
	if (utc)
		sarg->value.search_flags = MAIL_SEARCH_ARG_FLAG_UTC_TIMES;

	if (sarg->value.time == (time_t)-1) {
		ctx->_error = p_strconcat(ctx->pool,
			"Invalid search date parameter: ", value, NULL);
		return NULL;
	}
	sarg->value.date_type = date_type;
	return sarg;
}

#define CALLBACK_DATE(_func, _type, _date_type) \
static struct mail_search_arg *\
human_search_##_func(struct mail_search_build_context *ctx) \
{ \
	return arg_new_human_date(ctx, _type, _date_type); \
}
CALLBACK_DATE(before, SEARCH_BEFORE, MAIL_SEARCH_DATE_TYPE_RECEIVED)
CALLBACK_DATE(on, SEARCH_ON, MAIL_SEARCH_DATE_TYPE_RECEIVED)
CALLBACK_DATE(since, SEARCH_SINCE, MAIL_SEARCH_DATE_TYPE_RECEIVED)

CALLBACK_DATE(sentbefore, SEARCH_BEFORE, MAIL_SEARCH_DATE_TYPE_SENT)
CALLBACK_DATE(senton, SEARCH_ON, MAIL_SEARCH_DATE_TYPE_SENT)
CALLBACK_DATE(sentsince, SEARCH_SINCE, MAIL_SEARCH_DATE_TYPE_SENT)

CALLBACK_DATE(savedbefore, SEARCH_BEFORE, MAIL_SEARCH_DATE_TYPE_SAVED)
CALLBACK_DATE(savedon, SEARCH_ON, MAIL_SEARCH_DATE_TYPE_SAVED)
CALLBACK_DATE(savedsince, SEARCH_SINCE, MAIL_SEARCH_DATE_TYPE_SAVED)

static struct mail_search_arg *
human_search_savedatesupported(struct mail_search_build_context *ctx)
{
	return mail_search_build_new(ctx, SEARCH_SAVEDATESUPPORTED);
}

static struct mail_search_arg *
arg_new_human_size(struct mail_search_build_context *ctx,
		   enum mail_search_arg_type type)
{
	struct mail_search_arg *sarg;
	const char *value, *error;

	sarg = mail_search_build_new(ctx, type);
	if (mail_search_parse_string(ctx->parser, &value) < 0)
		return NULL;

	if (str_parse_get_size(value, &sarg->value.size, &error) < 0) {
		ctx->_error = p_strdup(ctx->pool, error);
		return NULL;
	}
	return sarg;
}

static struct mail_search_arg *
human_search_larger(struct mail_search_build_context *ctx)
{
	return arg_new_human_size(ctx, SEARCH_LARGER);
}

static struct mail_search_arg *
human_search_smaller(struct mail_search_build_context *ctx)
{
	return arg_new_human_size(ctx, SEARCH_SMALLER);
}

static struct mail_search_arg *
human_search_guid(struct mail_search_build_context *ctx)
{
	return mail_search_build_str(ctx, SEARCH_GUID);
}

static struct mail_search_arg *
human_search_mailbox(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;

	sarg = mail_search_build_str(ctx, SEARCH_MAILBOX);
	if (sarg == NULL)
		return NULL;

	if (strchr(sarg->value.str, '*') != NULL ||
	    strchr(sarg->value.str, '%') != NULL)
		sarg->type = SEARCH_MAILBOX_GLOB;

	if (!uni_utf8_str_is_valid(sarg->value.str)) {
		ctx->_error = p_strconcat(ctx->pool,
			"Mailbox name not valid UTF-8: ",
			sarg->value.str, NULL);
		return NULL;
	}
	return sarg;
}

static struct mail_search_arg *
human_search_mailbox_guid(struct mail_search_build_context *ctx)
{
	return mail_search_build_str(ctx, SEARCH_MAILBOX_GUID);
}

static struct mail_search_arg *
human_search_oldestonly(struct mail_search_build_context *ctx)
{
	ctx->args->stop_on_nonmatch = TRUE;
	return mail_search_build_new(ctx, SEARCH_ALL);
}

static const struct mail_search_register_arg human_register_args[] = {
	{ "OR", human_search_or, 0},

	/* dates */
	{ "BEFORE", human_search_before, 0},
	{ "ON", human_search_on, 0},
	{ "SINCE", human_search_since, 0},
	{ "SENTBEFORE", human_search_sentbefore, 0},
	{ "SENTON", human_search_senton, 0},
	{ "SENTSINCE", human_search_sentsince, 0},
	{ "SAVEDBEFORE", human_search_savedbefore, 0},
	{ "SAVEDON", human_search_savedon, 0},
	{ "SAVEDSINCE", human_search_savedsince, 0},
	{ "SAVEDATESUPPORTED", human_search_savedatesupported, 0},
	{ "X-SAVEDBEFORE", human_search_savedbefore, 0},
	{ "X-SAVEDON", human_search_savedon, 0},
	{ "X-SAVEDSINCE", human_search_savedsince, 0},

	/* sizes */
	{ "LARGER", human_search_larger, 0},
	{ "SMALLER", human_search_smaller, 0},

	/* Other Dovecot extensions: */
	{ "GUID", human_search_guid, 0},
	{ "MAILBOX", human_search_mailbox, 0},
	{ "MAILBOX-GUID", human_search_mailbox_guid, 0},
	{ "OLDESTONLY", human_search_oldestonly, 0}
};

static struct mail_search_register *
mail_search_register_init_human(struct mail_search_register *imap_register)
{
	struct mail_search_register *reg;
	mail_search_register_fallback_t *fallback;
	ARRAY(struct mail_search_register_arg) copy_args;
	const struct mail_search_register_arg *human_args, *imap_args;
	unsigned int i, j, human_count, imap_count;
	int ret;

	reg = mail_search_register_init();
	mail_search_register_add(reg, human_register_args,
				 N_ELEMENTS(human_register_args));

	/* find and register args in imap that don't exist in human */
	imap_args = mail_search_register_get(imap_register, &imap_count);
	human_args = mail_search_register_get(reg, &human_count);
	t_array_init(&copy_args, imap_count);
	for (i = j = 0; i < imap_count && j < human_count; ) {
		ret = strcmp(imap_args[i].key, human_args[j].key);
		if (ret < 0) {
			array_push_back(&copy_args, &imap_args[i]);
			i++;
		} else if (ret > 0) {
			j++;
		} else {
			i++; j++;
		}
	}
	for (; i < imap_count; i++)
		array_push_back(&copy_args, &imap_args[i]);

	imap_args = array_get(&copy_args, &imap_count);
	mail_search_register_add(reg, imap_args, imap_count);

	if (mail_search_register_get_fallback(imap_register, &fallback))
		mail_search_register_fallback(reg, fallback);
	return reg;
}

struct mail_search_register *mail_search_register_get_human(void)
{
	if (mail_search_register_human == NULL) {
		struct mail_search_register *imap_reg =
			mail_search_register_get_imap4rev1();

		mail_search_register_human =
			mail_search_register_init_human(imap_reg);
	}
	return mail_search_register_human;
}
