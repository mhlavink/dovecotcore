#ifndef CONFIG_FILTER_H
#define CONFIG_FILTER_H

#include "net.h"

/* A single filter in configuration. Only one of the fields should be set at
   a time. Use parent-hierarchy to set multiple filters. */
struct config_filter {
	/* Parent filter, which is ANDed to this filter */
	struct config_filter *parent;

	const char *service;
	/* local_name is for TLS SNI requests.
	   both local_name and local_bits can't be set at the same time. */
	const char *local_name;
	/* the hosts are used only in doveconf output */
	const char *local_host, *remote_host;
	struct ip_addr local_net, remote_net;
	unsigned int local_bits, remote_bits;

	/* named_filter { .. } */
	const char *filter_name;
	/* named_list_filter key { .. } */
	bool filter_name_array;

	/* TRUE if default settings are being accessed. These will be stored in
	   separate filters so they can be ordered before global settings. */
	bool default_settings;
};

/* Each unique config_filter (including its parents in hierarchy) has its own
   config_filter_parser. */
struct config_filter_parser {
	/* Filter parser tree. These are used only for doveconf's human output
	   to write the filters in nice nested hierarchies. */
	struct config_filter_parser *parent;
	struct config_filter_parser *children_head, *children_tail, *prev, *next;

	/* Filter for this parser. Its parent filters must also match. */
	struct config_filter filter;
	/* NULL-terminated array of parsers for settings. All parsers have the
	   same number of module_parsers. Each module parser is initialized
	   lazily after the first setting in the module is changed. */
	struct config_module_parser *module_parsers;
	/* Named [list] filters may have required_setting. If they do, this
	   boolean tracks whether that setting has been changed in this
	   filter. */
	bool filter_required_setting_seen;
};

/* Returns TRUE if filter matches mask. */
bool config_filter_match(const struct config_filter *mask,
			 const struct config_filter *filter);
/* Returns TRUE if two filters are fully equal. */
bool config_filters_equal(const struct config_filter *f1,
			  const struct config_filter *f2);
/* Returns TRUE if filter is empty, and it has no parent filters. */
bool config_filter_is_empty(const struct config_filter *filter);

#endif
