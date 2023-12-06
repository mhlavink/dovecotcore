/* Copyright (c) 2020 Dovecot authors, see the included COPYING file */

%define api.pure
%define api.prefix {event_filter_parser_}
%define parse.error verbose
%lex-param {void *scanner}
%parse-param {struct event_filter_parser_state *state}

%defines

%{
#include <ctype.h>

#include "lib.h"
#include "strescape.h"
#include "str-parse.h"
#include "wildcard-match.h"
#include "lib-event-private.h"
#include "event-filter-private.h"

#define scanner state->scanner

#define YYERROR_VERBOSE

extern int event_filter_parser_lex(void *, void *);

void event_filter_parser_error(void *scan, const char *e)
{
	struct event_filter_parser_state *state = scan;

	state->error = t_strdup_printf("event filter: %s", e);
}

static struct event_filter_node *key_value(struct event_filter_parser_state *state,
					   const char *a, const char *b,
					   enum event_filter_node_op op)
{
	struct event_filter_node *node;
	enum event_filter_node_type type;

	if (strcmp(a, "event") == 0)
		type = EVENT_FILTER_NODE_TYPE_EVENT_NAME_WILDCARD;
	else if (strcmp(a, "category") == 0)
		type = EVENT_FILTER_NODE_TYPE_EVENT_CATEGORY;
	else if (strcmp(a, "source_location") == 0)
		type = EVENT_FILTER_NODE_TYPE_EVENT_SOURCE_LOCATION;
	else
		type = EVENT_FILTER_NODE_TYPE_EVENT_FIELD_WILDCARD;

	/* only fields support comparators other than EQ */
	if ((type != EVENT_FILTER_NODE_TYPE_EVENT_FIELD_WILDCARD) &&
	    (op != EVENT_FILTER_OP_CMP_EQ)) {
		state->error = "Only fields support inequality comparisons";
		return NULL;
	}

	node = p_new(state->pool, struct event_filter_node, 1);
	node->type = type;
	node->op = op;
	node->case_sensitive = state->case_sensitive;

	switch (type) {
	case EVENT_FILTER_NODE_TYPE_LOGIC:
		i_unreached();
	case EVENT_FILTER_NODE_TYPE_EVENT_NAME_WILDCARD: {
		if (wildcard_is_escaped_literal(b)) {
			node->type = EVENT_FILTER_NODE_TYPE_EVENT_NAME_EXACT;
			node->field.value.str = str_unescape(p_strdup(state->pool, b));
		} else {
			node->field.value.str = p_strdup(state->pool, b);
		}
		break;
	}
	case EVENT_FILTER_NODE_TYPE_EVENT_SOURCE_LOCATION: {
		node->field.value.str = str_unescape(p_strdup(state->pool, b));
		break;
	}
	case EVENT_FILTER_NODE_TYPE_EVENT_CATEGORY:
		if (!event_filter_category_to_log_type(b, &node->category.log_type)) {
			node->category.name = str_unescape(p_strdup(state->pool, b));
			node->category.ptr = event_category_find_registered(b);
		}
		break;
	case EVENT_FILTER_NODE_TYPE_EVENT_FIELD_WILDCARD: {
		node->field.key = p_strdup(state->pool, a);

		/* Filter currently supports only comparing strings
		   and numbers. */
		if (str_to_intmax(b, &node->field.value.intmax) == 0) {
			/* Leave a hint that this is in fact a valid number. */
			node->field.value_type = EVENT_FIELD_VALUE_TYPE_INTMAX;
			node->type = EVENT_FILTER_NODE_TYPE_EVENT_FIELD_EXACT;
		} else if (net_parse_range(b, &node->field.value.ip,
					   &node->field.value.ip_bits) == 0) {
			/* Leave a hint that this is in fact a valid IP. */
			node->field.value_type = EVENT_FIELD_VALUE_TYPE_IP;
			node->type = EVENT_FILTER_NODE_TYPE_EVENT_FIELD_EXACT;
		} else {
			/* This field contains no valid number.
			   Either this is a string that contains a size unit, a
			   number with wildcard or another arbitrary string. */

			/* If the field contains a size unit, take that. */
			uoff_t bytes;
			const char *error;
			int ret = str_parse_get_size(b, &bytes, &error);
			if (ret == 0 && i_toupper(b[strlen(b)-1]) == 'M') {
				/* Don't accept <num>M, since it's ambiguous
				   whether it's MB or minutes. A warning will
				   be logged later on about this. */
				node->field.value_type = EVENT_FIELD_VALUE_TYPE_STR;
				node->field.value.str = p_strdup(state->pool, b);
				node->ambiguous_unit = TRUE;
				break;
			}
			if (ret == 0 && bytes <= INTMAX_MAX) {
				node->field.value.intmax = (intmax_t) bytes;
				node->field.value_type = EVENT_FIELD_VALUE_TYPE_INTMAX;
				node->type = EVENT_FILTER_NODE_TYPE_EVENT_FIELD_EXACT;
				break;
			}

			/* As a second step try to parse the value as an
			   interval. The string-parser returns values as
			   milliseconds, but the events usually report values
			   as microseconds, which needs to be accounted for. */
			unsigned int intval;
			ret = str_parse_get_interval_msecs(b, &intval, &error);
			if (ret == 0) {
				node->field.value.intmax = (intmax_t) intval * 1000;
				node->field.value_type = EVENT_FIELD_VALUE_TYPE_INTMAX;
				node->type = EVENT_FILTER_NODE_TYPE_EVENT_FIELD_EXACT;
				break;
			}

			char *b_duped = p_strdup(state->pool, b);
			if (wildcard_is_escaped_literal(b)) {
				node->type = EVENT_FILTER_NODE_TYPE_EVENT_FIELD_EXACT;
				str_unescape(b_duped);
			} else if (strspn(b, "0123456789*?") == strlen(b)) {
				node->type = EVENT_FILTER_NODE_TYPE_EVENT_FIELD_NUMERIC_WILDCARD;
			}
			node->field.value.str = b_duped;
			node->field.value_type = EVENT_FIELD_VALUE_TYPE_STR;
		}

		break;
	}
	case EVENT_FILTER_NODE_TYPE_EVENT_NAME_EXACT:
	case EVENT_FILTER_NODE_TYPE_EVENT_FIELD_EXACT:
	case EVENT_FILTER_NODE_TYPE_EVENT_FIELD_NUMERIC_WILDCARD:
		i_unreached();
	}

	return node;
}

static struct event_filter_node *logic(struct event_filter_parser_state *state,
				       struct event_filter_node *a,
				       struct event_filter_node *b,
				       enum event_filter_node_op op)
{
	struct event_filter_node *node;

	node = p_new(state->pool, struct event_filter_node, 1);
	node->type = EVENT_FILTER_NODE_TYPE_LOGIC;
	node->op = op;
	node->children[0] = a;
	node->children[1] = b;

	return node;
}

/* ignore strict bool warnings in generated code */
#ifdef HAVE_STRICT_BOOL
#  pragma GCC diagnostic ignored "-Wstrict-bool"
#endif

%}

%union {
	const char *str;
	enum event_filter_node_op op;
	struct event_filter_node *node;
};

%token <str> TOKEN STRING
%token AND OR NOT

%type <str> key value
%type <op> op
%type <node> expr key_value

%left AND OR
%right NOT

%%
filter : expr			{ state->output = $1; }
       | %empty			{ state->output = NULL; }
       ;

expr : expr AND expr		{ $$ = logic(state, $1, $3, EVENT_FILTER_OP_AND); }
     | expr OR expr		{ $$ = logic(state, $1, $3, EVENT_FILTER_OP_OR); }
     | NOT expr			{ $$ = logic(state, $2, NULL, EVENT_FILTER_OP_NOT); }
     | '(' expr ')'		{ $$ = $2; }
     | key_value		{ $$ = $1; }
     ;

key_value : key op value	{
					$$ = key_value(state, $1, $3, $2);
					if ($$ == NULL) {
						yyerror(state, state->error);
						/* avoid compiler warning about yynerrs being set, but not used */
						(void)yynerrs;
						YYERROR;
					}
				}
	  ;

key : TOKEN			{ $$ = $1; }
    | STRING			{ $$ = str_unescape(t_strdup_noconst($1)); }
    ;

value : TOKEN			{ $$ = $1; }
      | STRING			{ $$ = $1; }
      | AND			{ $$ = "and"; }
      | OR			{ $$ = "or"; }
      | NOT			{ $$ = "not"; }
      ;

op : '='			{ $$ = EVENT_FILTER_OP_CMP_EQ; }
   | '>'			{ $$ = EVENT_FILTER_OP_CMP_GT; }
   | '<'			{ $$ = EVENT_FILTER_OP_CMP_LT; }
   | '>' '='			{ $$ = EVENT_FILTER_OP_CMP_GE; }
   | '<' '='			{ $$ = EVENT_FILTER_OP_CMP_LE; }
   ;
%%
