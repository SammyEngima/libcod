#include "gsc_sqlite.hpp"

#if COMPILE_SQLITE == 1

#include <sqlite3.h>
#include <pthread.h>

#define INVALID_ENTITY -1
#define INVALID_STATE 0

#define MAX_SQLITE_FIELDS 256
#define MAX_SQLITE_ROWS 256
#define MAX_SQLITE_TASKS 512

#define SQLITE_TIMEOUT 2000

enum
{
	INT_VALUE,
	FLOAT_VALUE,
	STRING_VALUE,
	VECTOR_VALUE,
	OBJECT_VALUE
};

struct async_sqlite_task
{
	async_sqlite_task *prev;
	async_sqlite_task *next;
	sqlite3 *db;
	sqlite3_stmt *statement;
	char query[COD2_MAX_STRINGLENGTH];
	unsigned int row[MAX_SQLITE_FIELDS][MAX_SQLITE_ROWS];
	int result;
	int timeout;
	int fields_size;
	int rows_size;
	int callback;
	bool done;
	bool complete;
	bool save;
	bool error;
	char errorMessage[COD2_MAX_STRINGLENGTH];
	unsigned int levelId;
	bool hasargument;
	int valueType;
	int intValue;
	float floatValue;
	char stringValue[COD2_MAX_STRINGLENGTH];
	vec3_t vectorValue;
	unsigned int objectValue;
	int entityNum;
	int entityState;
};

struct sqlite_db_store
{
	sqlite_db_store *prev;
	sqlite_db_store *next;
	sqlite3 *db;
};

async_sqlite_task *first_async_sqlite_task = NULL;
sqlite_db_store *first_sqlite_db_store = NULL;
pthread_mutex_t lock_async_sqlite_task_position;
pthread_mutex_t lock_async_sqlite_server_spawn;
int async_sqlite_initialized = 0;

void free_sqlite_db_stores_and_tasks()
{
	pthread_mutex_lock(&lock_async_sqlite_server_spawn);

	async_sqlite_task *current = first_async_sqlite_task;

	while (current != NULL)
	{
		async_sqlite_task *task = current;
		current = current->next;

		if (task->next != NULL)
			task->next->prev = task->prev;

		if (task->prev != NULL)
			task->prev->next = task->next;
		else
			first_async_sqlite_task = task->next;

		if (task->statement != NULL)
			sqlite3_finalize(task->statement);

		delete task;
	}

	sqlite_db_store *current_store = first_sqlite_db_store;

	while (current_store != NULL)
	{
		sqlite_db_store *store = current_store;
		current_store = current_store->next;

		if (store->next != NULL)
			store->next->prev = store->prev;

		if (store->prev != NULL)
			store->prev->next = store->next;
		else
			first_sqlite_db_store = store->next;

		if (store->db != NULL)
			sqlite3_close(store->db);

		delete store;
	}

	pthread_mutex_unlock(&lock_async_sqlite_server_spawn);
}

void *async_sqlite_query_handler(void* dummy)
{
	while(1)
	{
		pthread_mutex_lock(&lock_async_sqlite_server_spawn);

		async_sqlite_task *current = first_async_sqlite_task;

		while (current != NULL)
		{
			async_sqlite_task *task = current;
			current = current->next;

			if (!task->done)
			{
				task->result = sqlite3_prepare_v2(task->db, task->query, COD2_MAX_STRINGLENGTH, &task->statement, 0);

				if (task->result == SQLITE_OK)
				{
					if (task->save && task->callback)
						task->fields_size = 0;

					task->timeout = Sys_MilliSeconds();
					task->result = sqlite3_step(task->statement);

					while (task->result != SQLITE_DONE)
					{
						if (task->result == SQLITE_BUSY)
						{
							if ((Sys_MilliSeconds() - task->timeout) > SQLITE_TIMEOUT)
							{
								task->error = true;
								task->result = SQLITE_ERROR;

								strncpy(task->errorMessage, sqlite3_errmsg(task->db), COD2_MAX_STRINGLENGTH - 1);
								task->errorMessage[COD2_MAX_STRINGLENGTH - 1] = '\0';

								break;
							}
						}
						else if (task->result < SQLITE_NOTICE)
						{
							task->error = true;

							strncpy(task->errorMessage, sqlite3_errmsg(task->db), COD2_MAX_STRINGLENGTH - 1);
							task->errorMessage[COD2_MAX_STRINGLENGTH - 1] = '\0';

							break;
						}
						else if (task->result == SQLITE_ROW && task->save && task->callback)
						{
							if (task->fields_size > MAX_SQLITE_FIELDS - 1)
								continue;

							task->rows_size = 0;

							for (int i = 0; i < sqlite3_column_count(task->statement); i++)
							{
								if (task->rows_size > MAX_SQLITE_ROWS - 1)
									continue;

								task->row[task->fields_size][task->rows_size] = SL_GetString((char *)sqlite3_column_text(task->statement, i), 0);
								task->rows_size++;
							}

							task->fields_size++;
						}

						task->result = sqlite3_step(task->statement);
					}
				}
				else
				{
					if (task->result != SQLITE_BUSY)
					{
						task->error = true;

						strncpy(task->errorMessage, sqlite3_errmsg(task->db), COD2_MAX_STRINGLENGTH - 1);
						task->errorMessage[COD2_MAX_STRINGLENGTH - 1] = '\0';
					}
				}

				if (task->result != SQLITE_BUSY)
					task->done = true;
			}

			if (task->complete)
			{
				pthread_mutex_lock(&lock_async_sqlite_task_position);

				if (task->next != NULL)
					task->next->prev = task->prev;

				if (task->prev != NULL)
					task->prev->next = task->next;
				else
					first_async_sqlite_task = task->next;

				if (task->statement != NULL)
					sqlite3_finalize(task->statement);

				delete task;

				pthread_mutex_unlock(&lock_async_sqlite_task_position);
			}
		}

		pthread_mutex_unlock(&lock_async_sqlite_server_spawn);

		usleep(10000);
	}

	return NULL;
}

void gsc_async_sqlite_initialize()
{
	if (!async_sqlite_initialized)
	{
		if (pthread_mutex_init(&lock_async_sqlite_task_position, NULL) != 0)
		{
			stackError("gsc_async_sqlite_initialize() task position mutex initialization failed!");
			stackPushUndefined();
			return;
		}

		if (pthread_mutex_init(&lock_async_sqlite_server_spawn, NULL) != 0)
		{
			stackError("gsc_async_sqlite_initialize() server spawn mutex initialization failed!");
			stackPushUndefined();
			return;
		}

		pthread_t async_handler;

		if (pthread_create(&async_handler, NULL, async_sqlite_query_handler, NULL) != 0)
		{
			stackError("gsc_async_sqlite_initialize() error creating async handler thread!");
			stackPushUndefined();
			return;
		}

		if (pthread_detach(async_handler) != 0)
		{
			stackError("gsc_async_sqlite_initialize() error detaching async handler thread!");
			stackPushUndefined();
			return;
		}

		async_sqlite_initialized = 1;
	}
	else
		Com_DPrintf("gsc_async_sqlite_initialize() async handler already initialized.\n");

	stackPushInt(async_sqlite_initialized);
}

void gsc_async_sqlite_create_query()
{
	int db;
	char *query;

	if ( ! stackGetParams("is", &db, &query))
	{
		stackError("gsc_async_sqlite_create_query() one or more arguments is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	if (!async_sqlite_initialized)
	{
		stackError("gsc_async_sqlite_create_query() async handler has not been initialized");
		stackPushUndefined();
		return;
	}

	pthread_mutex_lock(&lock_async_sqlite_task_position);

	async_sqlite_task *current = first_async_sqlite_task;

	int task_count = 0;

	while (current != NULL && current->next != NULL)
	{
		if (task_count > MAX_SQLITE_TASKS - 1)
		{
			stackError("gsc_async_sqlite_create_query() exceeded async task limit");
			stackPushUndefined();
			return;
		}

		current = current->next;
		task_count++;
	}

	async_sqlite_task *newtask = new async_sqlite_task;

	newtask->prev = current;
	newtask->next = NULL;

	newtask->db = (sqlite3 *)db;

	strncpy(newtask->query, query, COD2_MAX_STRINGLENGTH - 1);
	newtask->query[COD2_MAX_STRINGLENGTH - 1] = '\0';

	int callback;

	if (!stackGetParamFunction(2, &callback))
		newtask->callback = 0;
	else
		newtask->callback = callback;

	newtask->done = false;
	newtask->complete = false;
	newtask->save = true;
	newtask->error = false;
	newtask->levelId = scrVarPub.levelId;
	newtask->hasargument = true;
	newtask->entityNum = INVALID_ENTITY;
	newtask->entityState = INVALID_STATE;

	int valueInt;
	float valueFloat;
	char *valueString;
	vec3_t valueVector;
	unsigned int valueObject;

	if (stackGetParamInt(3, &valueInt))
	{
		newtask->valueType = INT_VALUE;
		newtask->intValue = valueInt;
	}
	else if (stackGetParamFloat(3, &valueFloat))
	{
		newtask->valueType = FLOAT_VALUE;
		newtask->floatValue = valueFloat;
	}
	else if (stackGetParamString(3, &valueString))
	{
		newtask->valueType = STRING_VALUE;
		strcpy(newtask->stringValue, valueString);
	}
	else if (stackGetParamVector(3, valueVector))
	{
		newtask->valueType = VECTOR_VALUE;
		newtask->vectorValue[0] = valueVector[0];
		newtask->vectorValue[1] = valueVector[1];
		newtask->vectorValue[2] = valueVector[2];
	}
	else if (stackGetParamObject(3, &valueObject))
	{
		newtask->valueType = OBJECT_VALUE;
		newtask->objectValue = valueObject;
	}
	else
		newtask->hasargument = false;

	if (current != NULL)
		current->next = newtask;
	else
		first_async_sqlite_task = newtask;

	pthread_mutex_unlock(&lock_async_sqlite_task_position);

	stackPushInt(1);
}

void gsc_async_sqlite_create_query_nosave()
{
	int db;
	char *query;

	if ( ! stackGetParams("is", &db, &query))
	{
		stackError("gsc_async_sqlite_create_query_nosave() one or more arguments is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	if (!async_sqlite_initialized)
	{
		stackError("gsc_async_sqlite_create_query_nosave() async handler has not been initialized");
		stackPushUndefined();
		return;
	}

	pthread_mutex_lock(&lock_async_sqlite_task_position);

	async_sqlite_task *current = first_async_sqlite_task;

	int task_count = 0;

	while (current != NULL && current->next != NULL)
	{
		if (task_count > MAX_SQLITE_TASKS - 1)
		{
			stackError("gsc_async_sqlite_create_query_nosave() exceeded async task limit");
			stackPushUndefined();
			return;
		}

		current = current->next;
		task_count++;
	}

	async_sqlite_task *newtask = new async_sqlite_task;

	newtask->prev = current;
	newtask->next = NULL;

	newtask->db = (sqlite3 *)db;

	strncpy(newtask->query, query, COD2_MAX_STRINGLENGTH - 1);
	newtask->query[COD2_MAX_STRINGLENGTH - 1] = '\0';

	int callback;

	if (!stackGetParamFunction(2, &callback))
		newtask->callback = 0;
	else
		newtask->callback = callback;

	newtask->done = false;
	newtask->complete = false;
	newtask->save = false;
	newtask->error = false;
	newtask->levelId = scrVarPub.levelId;
	newtask->hasargument = true;
	newtask->entityNum = INVALID_ENTITY;
	newtask->entityState = INVALID_STATE;

	int valueInt;
	float valueFloat;
	char *valueString;
	vec3_t valueVector;
	unsigned int valueObject;

	if (stackGetParamInt(3, &valueInt))
	{
		newtask->valueType = INT_VALUE;
		newtask->intValue = valueInt;
	}
	else if (stackGetParamFloat(3, &valueFloat))
	{
		newtask->valueType = FLOAT_VALUE;
		newtask->floatValue = valueFloat;
	}
	else if (stackGetParamString(3, &valueString))
	{
		newtask->valueType = STRING_VALUE;
		strcpy(newtask->stringValue, valueString);
	}
	else if (stackGetParamVector(3, valueVector))
	{
		newtask->valueType = VECTOR_VALUE;
		newtask->vectorValue[0] = valueVector[0];
		newtask->vectorValue[1] = valueVector[1];
		newtask->vectorValue[2] = valueVector[2];
	}
	else if (stackGetParamObject(3, &valueObject))
	{
		newtask->valueType = OBJECT_VALUE;
		newtask->objectValue = valueObject;
	}
	else
		newtask->hasargument = false;

	if (current != NULL)
		current->next = newtask;
	else
		first_async_sqlite_task = newtask;

	pthread_mutex_unlock(&lock_async_sqlite_task_position);

	stackPushInt(1);
}

void gsc_async_sqlite_create_entity_query(int entid)
{
	int db;
	char *query;

	if ( ! stackGetParams("is", &db, &query))
	{
		stackError("gsc_async_sqlite_create_entity_query() one or more arguments is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	if (!async_sqlite_initialized)
	{
		stackError("gsc_async_sqlite_create_entity_query() async handler has not been initialized");
		stackPushUndefined();
		return;
	}

	pthread_mutex_lock(&lock_async_sqlite_task_position);

	async_sqlite_task *current = first_async_sqlite_task;

	int task_count = 0;

	while (current != NULL && current->next != NULL)
	{
		if (task_count > MAX_SQLITE_TASKS - 1)
		{
			stackError("gsc_async_sqlite_create_entity_query() exceeded async task limit");
			stackPushUndefined();
			return;
		}

		current = current->next;
		task_count++;
	}

	async_sqlite_task *newtask = new async_sqlite_task;

	newtask->prev = current;
	newtask->next = NULL;

	newtask->db = (sqlite3 *)db;

	strncpy(newtask->query, query, COD2_MAX_STRINGLENGTH - 1);
	newtask->query[COD2_MAX_STRINGLENGTH - 1] = '\0';

	int callback;

	if (!stackGetParamFunction(2, &callback))
		newtask->callback = 0;
	else
		newtask->callback = callback;

	newtask->done = false;
	newtask->complete = false;
	newtask->save = true;
	newtask->error = false;
	newtask->levelId = scrVarPub.levelId;
	newtask->hasargument = true;
	newtask->entityNum = entid;
	newtask->entityState = *(int *)(G_ENTITY(newtask->entityNum) + 1);

	int valueInt;
	float valueFloat;
	char *valueString;
	vec3_t valueVector;
	unsigned int valueObject;

	if (stackGetParamInt(3, &valueInt))
	{
		newtask->valueType = INT_VALUE;
		newtask->intValue = valueInt;
	}
	else if (stackGetParamFloat(3, &valueFloat))
	{
		newtask->valueType = FLOAT_VALUE;
		newtask->floatValue = valueFloat;
	}
	else if (stackGetParamString(3, &valueString))
	{
		newtask->valueType = STRING_VALUE;
		strcpy(newtask->stringValue, valueString);
	}
	else if (stackGetParamVector(3, valueVector))
	{
		newtask->valueType = VECTOR_VALUE;
		newtask->vectorValue[0] = valueVector[0];
		newtask->vectorValue[1] = valueVector[1];
		newtask->vectorValue[2] = valueVector[2];
	}
	else if (stackGetParamObject(3, &valueObject))
	{
		newtask->valueType = OBJECT_VALUE;
		newtask->objectValue = valueObject;
	}
	else
		newtask->hasargument = false;

	if (current != NULL)
		current->next = newtask;
	else
		first_async_sqlite_task = newtask;

	pthread_mutex_unlock(&lock_async_sqlite_task_position);

	stackPushInt(1);
}

void gsc_async_sqlite_create_entity_query_nosave(int entid)
{
	int db;
	char *query;

	if ( ! stackGetParams("is", &db, &query))
	{
		stackError("gsc_async_sqlite_create_entity_query_nosave() one or more arguments is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	if (!async_sqlite_initialized)
	{
		stackError("gsc_async_sqlite_create_entity_query_nosave() async handler has not been initialized");
		stackPushUndefined();
		return;
	}

	pthread_mutex_lock(&lock_async_sqlite_task_position);

	async_sqlite_task *current = first_async_sqlite_task;

	int task_count = 0;

	while (current != NULL && current->next != NULL)
	{
		if (task_count > MAX_SQLITE_TASKS - 1)
		{
			stackError("gsc_async_sqlite_create_entity_query_nosave() exceeded async task limit");
			stackPushUndefined();
			return;
		}

		current = current->next;
		task_count++;
	}

	async_sqlite_task *newtask = new async_sqlite_task;

	newtask->prev = current;
	newtask->next = NULL;

	newtask->db = (sqlite3 *)db;

	strncpy(newtask->query, query, COD2_MAX_STRINGLENGTH - 1);
	newtask->query[COD2_MAX_STRINGLENGTH - 1] = '\0';

	int callback;

	if (!stackGetParamFunction(2, &callback))
		newtask->callback = 0;
	else
		newtask->callback = callback;

	newtask->done = false;
	newtask->complete = false;
	newtask->save = false;
	newtask->error = false;
	newtask->levelId = scrVarPub.levelId;
	newtask->hasargument = true;
	newtask->entityNum = entid;
	newtask->entityState = *(int *)(G_ENTITY(newtask->entityNum) + 1);

	int valueInt;
	float valueFloat;
	char *valueString;
	vec3_t valueVector;
	unsigned int valueObject;

	if (stackGetParamInt(3, &valueInt))
	{
		newtask->valueType = INT_VALUE;
		newtask->intValue = valueInt;
	}
	else if (stackGetParamFloat(3, &valueFloat))
	{
		newtask->valueType = FLOAT_VALUE;
		newtask->floatValue = valueFloat;
	}
	else if (stackGetParamString(3, &valueString))
	{
		newtask->valueType = STRING_VALUE;
		strcpy(newtask->stringValue, valueString);
	}
	else if (stackGetParamVector(3, valueVector))
	{
		newtask->valueType = VECTOR_VALUE;
		newtask->vectorValue[0] = valueVector[0];
		newtask->vectorValue[1] = valueVector[1];
		newtask->vectorValue[2] = valueVector[2];
	}
	else if (stackGetParamObject(3, &valueObject))
	{
		newtask->valueType = OBJECT_VALUE;
		newtask->objectValue = valueObject;
	}
	else
		newtask->hasargument = false;

	if (current != NULL)
		current->next = newtask;
	else
		first_async_sqlite_task = newtask;

	pthread_mutex_unlock(&lock_async_sqlite_task_position);

	stackPushInt(1);
}

void gsc_async_sqlite_checkdone()
{
	async_sqlite_task *current = first_async_sqlite_task;

	while (current != NULL)
	{
		async_sqlite_task *task = current;
		current = current->next;

		if (task->done && !task->complete)
		{
			if (Scr_IsSystemActive() && (scrVarPub.levelId == task->levelId))
			{
				if (!task->error)
				{
					if (task->save && task->callback)
					{
						if (task->entityNum != INVALID_ENTITY)
						{
							if (task->entityState != INVALID_STATE)
							{
								int state = *(int *)(G_ENTITY(task->entityNum) + 1);

								if (state != INVALID_STATE && state == task->entityState)
								{
									if (task->hasargument)
									{
										switch(task->valueType)
										{
										case INT_VALUE:
											stackPushInt(task->intValue);
											break;

										case FLOAT_VALUE:
											stackPushFloat(task->floatValue);
											break;

										case STRING_VALUE:
											stackPushString(task->stringValue);
											break;

										case VECTOR_VALUE:
											stackPushVector(task->vectorValue);
											break;

										case OBJECT_VALUE:
											stackPushObject(task->objectValue);
											break;

										default:
											stackPushUndefined();
											break;
										}
									}

									stackPushArray();

									for (int i = 0; i < task->fields_size; i++)
									{
										stackPushArray();

										for (int x = 0; x < task->rows_size; x++)
										{
											stackPushString(SL_ConvertToString(task->row[i][x]));
											SL_RemoveRefToString(task->row[i][x]);
											stackPushArrayLast();
										}

										stackPushArrayLast();
									}

									short ret = Scr_ExecEntThread(G_ENTITY(task->entityNum), task->callback, task->save + task->hasargument);
									Scr_FreeThread(ret);
								}
							}
						}
						else
						{
							if (task->hasargument)
							{
								switch(task->valueType)
								{
								case INT_VALUE:
									stackPushInt(task->intValue);
									break;

								case FLOAT_VALUE:
									stackPushFloat(task->floatValue);
									break;

								case STRING_VALUE:
									stackPushString(task->stringValue);
									break;

								case VECTOR_VALUE:
									stackPushVector(task->vectorValue);
									break;

								default:
									stackPushUndefined();
									break;
								}
							}

							stackPushArray();

							for (int i = 0; i < task->fields_size; i++)
							{
								stackPushArray();

								for (int x = 0; x < task->rows_size; x++)
								{
									stackPushString(SL_ConvertToString(task->row[i][x]));
									SL_RemoveRefToString(task->row[i][x]);
									stackPushArrayLast();
								}

								stackPushArrayLast();
							}

							short ret = Scr_ExecThread(task->callback, task->save + task->hasargument);
							Scr_FreeThread(ret);
						}
					}
				}
				else
					stackError("gsc_async_sqlite_checkdone() query error in '%s' - '%s'", task->query, task->errorMessage);
			}

			task->complete = true;
		}
	}
}

void gsc_sqlite_open()
{
	char *database;

	if ( ! stackGetParams("s", &database))
	{
		stackError("gsc_sqlite_open() argument is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	sqlite3 *db;

	int rc = sqlite3_open(database, &db);

	if (rc != SQLITE_OK)
	{
		stackError("gsc_sqlite_open() cannot open database: %s", sqlite3_errmsg(db));
		stackPushUndefined();
		return;
	}

	sqlite_db_store *current = first_sqlite_db_store;

	while (current != NULL && current->next != NULL)
		current = current->next;

	sqlite_db_store *newstore = new sqlite_db_store;

	newstore->prev = current;
	newstore->next = NULL;

	newstore->db = db;

	if (current != NULL)
		current->next = newstore;
	else
		first_sqlite_db_store = newstore;

	stackPushInt((int)db);
}

void gsc_sqlite_query()
{
	int db;
	char *query;

	if ( ! stackGetParams("is", &db, &query))
	{
		stackError("gsc_sqlite_query() one or more arguments is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	sqlite3_stmt *statement;
	int timeout;
	int result;

	timeout = Sys_MilliSeconds();
	result = sqlite3_prepare_v2((sqlite3 *)db, query, COD2_MAX_STRINGLENGTH, &statement, 0);

	while (result != SQLITE_OK)
	{
		if (result == SQLITE_BUSY)
		{
			if ((Sys_MilliSeconds() - timeout) > SQLITE_TIMEOUT)
			{
				stackError("gsc_sqlite_query() timeout to fetch query data: %s", sqlite3_errmsg((sqlite3 *)db));
				stackPushUndefined();
				return;
			}
		}
		else if (result < SQLITE_NOTICE)
		{
			stackError("gsc_sqlite_query() failed to fetch query data: %s", sqlite3_errmsg((sqlite3 *)db));
			stackPushUndefined();
			return;
		}

		result = sqlite3_prepare_v2((sqlite3 *)db, query, COD2_MAX_STRINGLENGTH, &statement, 0);
	}

	stackPushArray();

	timeout = Sys_MilliSeconds();
	result = sqlite3_step(statement);

	while (result != SQLITE_DONE)
	{
		if (result == SQLITE_BUSY)
		{
			if ((Sys_MilliSeconds() - timeout) > SQLITE_TIMEOUT)
			{
				stackError("gsc_sqlite_query() timeout to execute query: %s", sqlite3_errmsg((sqlite3 *)db));
				stackPushUndefined();
				sqlite3_finalize(statement);
				return;
			}
		}
		else if (result < SQLITE_NOTICE)
		{
			stackError("gsc_sqlite_query() failed to execute query: %s", sqlite3_errmsg((sqlite3 *)db));
			stackPushUndefined();
			sqlite3_finalize(statement);
			return;
		}
		else if (result == SQLITE_ROW)
		{
			stackPushArray();

			for (int i = 0; i < sqlite3_column_count(statement); i++)
			{
				stackPushString((char *)sqlite3_column_text(statement, i));
				stackPushArrayLast();
			}

			stackPushArrayLast();
		}

		result = sqlite3_step(statement);
	}

	sqlite3_finalize(statement);
}

void gsc_sqlite_close()
{
	int db;

	if ( ! stackGetParams("i", &db))
	{
		stackError("gsc_sqlite_close() argument is undefined or has a wrong type");
		stackPushUndefined();
		return;
	}

	sqlite_db_store *current = first_sqlite_db_store;

	while (current != NULL)
	{
		sqlite_db_store *store = current;
		current = current->next;

		if (store->db == (sqlite3 *)db)
		{
			if (store->next != NULL)
				store->next->prev = store->prev;

			if (store->prev != NULL)
				store->prev->next = store->next;
			else
				first_sqlite_db_store = store->next;

			delete store;
		}
	}

	int rc = sqlite3_close((sqlite3 *)db);

	if (rc != SQLITE_OK)
	{
		stackError("gsc_sqlite_close() cannot close database: %s", sqlite3_errmsg((sqlite3 *)db));
		stackPushUndefined();
		return;
	}

	stackPushInt(1);
}

#endif
