/* Copyright (c) 2021 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "settings.h"
#include "sql-api-private.h"
#include "test-common.h"
#include "sql-api-private.h"

void driver_sqlite_init(void);
void driver_sqlite_deinit(void);

static const char sql_create_db[] =
"CREATE TABLE bar(\n"
"  foo VARCHAR(255)\n"
");\n";

static void setup_database(struct sql_db *sql)
{
	sql_disconnect(sql);
	i_unlink_if_exists("test-database.db");
	sql_exec(sql, sql_create_db);
}

static void test_sql_sqlite(void)
{
	test_begin("test sql api");

	struct settings_simple set;
	settings_simple_init(&set, (const char *const []) {
		"sql_driver", "sqlite",
		"sqlite_path", "test-database.db",
		"sqlite_journal_mode", "wal",
		NULL,
	});
	struct sql_db *sql = NULL;
	const char *error = NULL;

	sql_drivers_init_without_drivers();
	driver_sqlite_init();

	if (sql_init_auto(set.event, &sql, &error) <= 0)
		i_fatal("%s", error);
	test_assert(sql != NULL && error == NULL);
	setup_database(sql);

	/* insert data */
	struct sql_transaction_context *t = sql_transaction_begin(sql);
	sql_update(t, "INSERT INTO bar VALUES(\"value1\")");
	sql_update(t, "INSERT INTO bar VALUES(\"value2\")");
	test_assert(sql_transaction_commit_s(&t, &error) == 0);

	struct sql_result *cursor = sql_query_s(sql, "SELECT foo FROM bar");

	test_assert(sql_result_next_row(cursor) == SQL_RESULT_NEXT_OK);
	test_assert_ucmp(sql_result_get_fields_count(cursor), ==, 1);
	test_assert_strcmp(sql_result_get_field_name(cursor, 0), "foo");
	test_assert_strcmp(sql_result_get_field_value(cursor, 0), "value1");
	test_assert(sql_result_next_row(cursor) == SQL_RESULT_NEXT_OK);
	test_assert_ucmp(sql_result_get_fields_count(cursor), ==, 1);
	test_assert_strcmp(sql_result_get_field_name(cursor, 0), "foo");
	test_assert_strcmp(sql_result_get_field_value(cursor, 0), "value2");
	test_assert(sql_result_next_row(cursor) == SQL_RESULT_NEXT_LAST);

	sql_result_unref(cursor);
	sql_unref(&sql);

	driver_sqlite_deinit();
	sql_drivers_deinit_without_drivers();
	settings_simple_deinit(&set);

	test_end();
}

int main(void) {
	static void (*const test_functions[])(void) = {
		test_sql_sqlite,
		NULL
	};
	return test_run(test_functions);
}
