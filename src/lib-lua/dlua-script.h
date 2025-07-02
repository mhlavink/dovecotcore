#ifndef LUA_SCRIPT_H
#define LUA_SCRIPT_H 1

struct dlua_script;

extern const struct setting_parser_info dlua_setting_parser_info;

struct dlua_settings {
	pool_t pool;

	const char *file;
	ARRAY_TYPE(const_string) settings;
};

/* Parse and load a lua script, without actually running it. */
int dlua_script_create_auto(struct event *event_parent,
			    struct dlua_script **script_r,
			    const char **error_r);
int dlua_script_create_string(const char *str, struct dlua_script **script_r,
			      struct event *event_parent, const char **error_r);
int dlua_script_create_file(const char *file, struct dlua_script **script_r,
			    struct event *event_parent, const char **error_r);
/* Remember to set script name using i_stream_set_name */
int dlua_script_create_stream(struct istream *is, struct dlua_script **script_r,
			      struct event *event_parent, const char **error_r);

/* run dlua_script_init function */
int dlua_script_init(struct dlua_script *script, const char **error_r);

/* Reference lua script */
void dlua_script_ref(struct dlua_script *script);

/* Unreference a script, calls deinit and frees when no more
   references exist */
void dlua_script_unref(struct dlua_script **_script);

/* see if particular function is registered */
bool dlua_script_has_function(struct dlua_script *script, const char *fn);


struct istream;
struct ostream;

/* stream wrappers */
int dlua_push_istream(struct dlua_script *script, struct istream *is);
int dlua_push_ostream(struct dlua_script *script, struct ostream *os);

#endif
