#include <jni.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "totedb.h"

typedef struct {
    int engine;
    tote_db_t *tote;
    sqlite3 *sqlite;
} connection_t;

typedef struct {
    JNIEnv *env;
    jobjectArray columns;
    jobject rows;
    jmethodID add;
    jclass cell_array;
    size_t column_count;
    jlong affected;
    int status;
    int code;
    const char *error;
} reply_t;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
    int failed;
} reader_t;

static void throw_error(JNIEnv *env, const char *message)
{
    jclass type = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (type != NULL) (*env)->ThrowNew(env, type, message);
}

static char *copy_string(JNIEnv *env, jbyteArray value)
{
    jsize size = (*env)->GetArrayLength(env, value);
    char *copy = malloc((size_t)size + 1);
    if (copy == NULL) {
        throw_error(env, "string allocation failed");
        return NULL;
    }
    (*env)->GetByteArrayRegion(env, value, 0, size, (jbyte *)copy);
    copy[size] = 0;
    if (memchr(copy, 0, (size_t)size) != NULL) {
        free(copy);
        throw_error(env, "embedded NUL in native string");
        return NULL;
    }
    return copy;
}

static int init_reply(reply_t *reply, JNIEnv *env, size_t count)
{
    memset(reply, 0, sizeof(*reply));
    reply->env = env;
    reply->column_count = count;
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    jclass list_class = (*env)->FindClass(env, "java/util/ArrayList");
    reply->cell_array = (*env)->FindClass(env, "[B");
    if ((*env)->ExceptionCheck(env)) return 0;
    reply->columns = (*env)->NewObjectArray(env, (jsize)count, string_class, NULL);
    jmethodID constructor = (*env)->GetMethodID(env, list_class, "<init>", "()V");
    reply->add = (*env)->GetMethodID(env, list_class, "add", "(Ljava/lang/Object;)Z");
    if ((*env)->ExceptionCheck(env)) return 0;
    reply->rows = (*env)->NewObject(env, list_class, constructor);
    reply->error = "";
    return !(*env)->ExceptionCheck(env);
}

static void set_column(reply_t *reply, size_t index, const uint8_t *bytes, size_t size)
{
    if (size > 256 || memchr(bytes, 0, size) != NULL) {
        reply->status = 2;
        reply->error = "invalid column name";
        return;
    }
    char name[257];
    memcpy(name, bytes, size);
    name[size] = 0;
    JNIEnv *env = reply->env;
    jstring value = (*env)->NewStringUTF(env, name);
    (*env)->SetObjectArrayElement(env, reply->columns, (jsize)index, value);
    (*env)->DeleteLocalRef(env, value);
}

static void set_cell(reply_t *reply, jobjectArray row, size_t index, const void *bytes, size_t size)
{
    JNIEnv *env = reply->env;
    if (size > INT32_MAX) {
        throw_error(env, "cell exceeds JNI array limit");
        return;
    }
    jbyteArray value = (*env)->NewByteArray(env, (jsize)size);
    if (value == NULL) return;
    if (size != 0) (*env)->SetByteArrayRegion(env, value, 0, (jsize)size, bytes);
    (*env)->SetObjectArrayElement(env, row, (jsize)index, value);
    (*env)->DeleteLocalRef(env, value);
}

static jobject finish_reply(reply_t *reply)
{
    JNIEnv *env = reply->env;
    if ((*env)->ExceptionCheck(env)) return NULL;
    jclass list_class = (*env)->GetObjectClass(env, reply->rows);
    jclass row_class = (*env)->FindClass(env, "[[B");
    jmethodID size_method = (*env)->GetMethodID(env, list_class, "size", "()I");
    jmethodID get_method = (*env)->GetMethodID(env, list_class, "get", "(I)Ljava/lang/Object;");
    jint count = (*env)->CallIntMethod(env, reply->rows, size_method);
    if ((*env)->ExceptionCheck(env)) return NULL;
    jobjectArray rows = (*env)->NewObjectArray(env, count, row_class, NULL);
    if (rows == NULL) return NULL;
    for (jint index = 0; index < count; index++) {
        jobject row = (*env)->CallObjectMethod(env, reply->rows, get_method, index);
        if ((*env)->ExceptionCheck(env)) return NULL;
        (*env)->SetObjectArrayElement(env, rows, index, row);
        (*env)->DeleteLocalRef(env, row);
    }
    jclass reply_class = (*env)->FindClass(env, "org/totedb/ycsb/NativeReply");
    jmethodID constructor = (*env)->GetMethodID(env, reply_class, "<init>",
        "(IILjava/lang/String;[Ljava/lang/String;[[[BJ)V");
    jstring error = (*env)->NewStringUTF(env, reply->error);
    if ((*env)->ExceptionCheck(env)) return NULL;
    return (*env)->NewObject(env, reply_class, constructor, reply->status, reply->code,
                            error, reply->columns, rows, reply->affected);
}

static const uint8_t *take(reader_t *reader, size_t count)
{
    if (reader->failed || count > reader->size - reader->offset) {
        reader->failed = 1;
        return NULL;
    }
    const uint8_t *value = reader->data + reader->offset;
    reader->offset += count;
    return value;
}

static uint64_t number(reader_t *reader, size_t size)
{
    const uint8_t *bytes = take(reader, size);
    if (bytes == NULL) return 0;
    uint64_t value = 0;
    for (size_t index = 0; index < size; index++) value |= (uint64_t)bytes[index] << (8 * index);
    return value;
}

static void decode_cell(reply_t *reply, reader_t *reader, jobjectArray row, size_t index)
{
    unsigned type = (unsigned)number(reader, 1);
    if (type == 0 || reader->failed) return;
    if (type == 3 || type == 4) {
        size_t size = (size_t)number(reader, 4);
        const uint8_t *bytes = take(reader, size);
        if (bytes != NULL) set_cell(reply, row, index, bytes, size);
        return;
    }
    if (type == 1) {
        int64_t value = (int64_t)number(reader, 8);
        char text[32];
        int size = snprintf(text, sizeof(text), "%lld", (long long)value);
        if (!reader->failed) set_cell(reply, row, index, text, (size_t)size);
        return;
    }
    reader->failed = 1;
}

static jobject decode_tote(JNIEnv *env, const uint8_t *data, size_t size)
{
    reader_t reader = {data, size, 0, 0};
    unsigned kind = (unsigned)number(&reader, 1);
    size_t count = kind == 0 ? (size_t)number(&reader, 4) : 0;
    if (count > 1024) reader.failed = 1;
    reply_t reply;
    if (!init_reply(&reply, env, reader.failed ? 0 : count)) return NULL;
    char error[1025] = {0};
    if (kind == 1) {
        size_t tag_size = (size_t)number(&reader, 4);
        take(&reader, tag_size);
        reply.affected = (jlong)number(&reader, 8);
    } else if (kind == 2) {
        unsigned category = (unsigned)number(&reader, 4);
        reply.code = (int32_t)number(&reader, 4);
        size_t error_size = (size_t)number(&reader, 4);
        const uint8_t *message = take(&reader, error_size);
        if (message != NULL && error_size < sizeof(error)) memcpy(error, message, error_size);
        else reader.failed = 1;
        reply.status = category == 7 ? 1 : 2;
        reply.error = error;
    } else if (kind != 0) {
        reader.failed = 1;
    }
    for (size_t index = 0; index < count && !reader.failed; index++) {
        size_t name_size = (size_t)number(&reader, 4);
        const uint8_t *name = take(&reader, name_size);
        unsigned type = (unsigned)number(&reader, 1);
        unsigned nullable = (unsigned)number(&reader, 1);
        if (type > 4 || nullable > 1) reader.failed = 1;
        if (name != NULL) set_column(&reply, index, name, name_size);
    }
    while (kind == 0 && !reader.failed && !(*env)->ExceptionCheck(env)) {
        unsigned marker = (unsigned)number(&reader, 1);
        if (marker == 0) break;
        if (marker != 1) { reader.failed = 1; break; }
        jobjectArray row = (*env)->NewObjectArray(env, (jsize)count, reply.cell_array, NULL);
        if (row == NULL) break;
        for (size_t index = 0; index < count && !reader.failed; index++) decode_cell(&reply, &reader, row, index);
        if (!reader.failed && !(*env)->ExceptionCheck(env)) (*env)->CallBooleanMethod(env, reply.rows, reply.add, row);
        (*env)->DeleteLocalRef(env, row);
    }
    if (reader.failed || reader.offset != reader.size) {
        reply.status = 2;
        reply.code = -1;
        reply.error = "malformed ToteDB result";
    }
    return finish_reply(&reply);
}

static jobject execute_tote(JNIEnv *env, connection_t *connection, const char *sql,
                            tote_param_t *params, size_t count)
{
    uint8_t *data = NULL;
    size_t size = 0;
    int status = tote_exec(connection->tote, NULL, sql, params, count, &data, &size);
    jobject result = NULL;
    if (status == TOTE_OK) {
        result = decode_tote(env, data, size);
    } else {
        reply_t reply;
        if (init_reply(&reply, env, 0)) {
            reply.status = status == TOTE_BUSY ? 1 : 2;
            reply.code = status;
            reply.error = tote_status_name(status);
            result = finish_reply(&reply);
        }
    }
    tote_free(data);
    return result;
}

static int bind_sqlite(sqlite3_stmt *statement, tote_param_t *params, size_t count)
{
    if ((size_t)sqlite3_bind_parameter_count(statement) != count) return SQLITE_MISUSE;
    for (size_t index = 0; index < count; index++) {
        int status = params[index].type == TOTE_PARAM_TEXT
            ? sqlite3_bind_text(statement, (int)index + 1, (const char *)params[index].bytes, (int)params[index].size, SQLITE_STATIC)
            : sqlite3_bind_blob(statement, (int)index + 1, params[index].bytes, (int)params[index].size, SQLITE_STATIC);
        if (status != SQLITE_OK) return status;
    }
    return SQLITE_OK;
}

static jobject execute_sqlite(JNIEnv *env, connection_t *connection, const char *sql,
                              tote_param_t *params, size_t count)
{
    sqlite3_stmt *statement = NULL;
    const char *tail = NULL;
    int status = sqlite3_prepare_v2(connection->sqlite, sql, -1, &statement, &tail);
    if (status == SQLITE_OK && (statement == NULL || *tail != 0)) status = SQLITE_MISUSE;
    if (status == SQLITE_OK) status = bind_sqlite(statement, params, count);
    size_t columns = statement == NULL ? 0 : (size_t)sqlite3_column_count(statement);
    reply_t reply;
    if (!init_reply(&reply, env, columns)) {
        sqlite3_finalize(statement);
        return NULL;
    }
    for (size_t index = 0; index < columns; index++) {
        const char *name = sqlite3_column_name(statement, (int)index);
        set_column(&reply, index, (const uint8_t *)name, strlen(name));
    }
    while (status == SQLITE_OK && !(*env)->ExceptionCheck(env)) {
        status = sqlite3_step(statement);
        if (status != SQLITE_ROW) break;
        jobjectArray row = (*env)->NewObjectArray(env, (jsize)columns, reply.cell_array, NULL);
        if (row == NULL) break;
        for (size_t index = 0; index < columns; index++) {
            int type = sqlite3_column_type(statement, (int)index);
            if (type == SQLITE_NULL) continue;
            const void *bytes = type == SQLITE_BLOB ? sqlite3_column_blob(statement, (int)index)
                                                   : sqlite3_column_text(statement, (int)index);
            set_cell(&reply, row, index, bytes, (size_t)sqlite3_column_bytes(statement, (int)index));
            if ((*env)->ExceptionCheck(env)) break;
        }
        if (!(*env)->ExceptionCheck(env)) (*env)->CallBooleanMethod(env, reply.rows, reply.add, row);
        (*env)->DeleteLocalRef(env, row);
        status = SQLITE_OK;
    }
    if (status == SQLITE_DONE) {
        reply.affected = columns == 0 ? sqlite3_changes64(connection->sqlite) : 0;
    } else {
        reply.status = status == SQLITE_BUSY ? 1 : 2;
        reply.code = status;
        reply.error = sqlite3_errmsg(connection->sqlite);
    }
    jobject result = finish_reply(&reply);
    int finalized = sqlite3_finalize(statement);
    if (finalized != SQLITE_OK && status == SQLITE_DONE) throw_error(env, "sqlite finalize failed");
    return result;
}

JNIEXPORT jlong JNICALL Java_org_totedb_ycsb_Native_open(JNIEnv *env, jclass type, jint engine,
                                                       jbyteArray path, jboolean create)
{
    (void)type;
    char *name = copy_string(env, path);
    if (name == NULL) return 0;
    connection_t *connection = calloc(1, sizeof(*connection));
    if (connection == NULL) { free(name); throw_error(env, "connection allocation failed"); return 0; }
    connection->engine = engine;
    int status;
    if (engine == 0) status = tote_open(name, create, &connection->tote);
    else status = sqlite3_open_v2(name, &connection->sqlite,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | (create ? SQLITE_OPEN_CREATE : 0), NULL);
    free(name);
    if (status != 0) {
        if (connection->sqlite != NULL) sqlite3_close(connection->sqlite);
        free(connection);
        return -(jlong)status;
    }
    return (jlong)(intptr_t)connection;
}

JNIEXPORT jint JNICALL Java_org_totedb_ycsb_Native_close(JNIEnv *env, jclass type, jlong handle)
{
    (void)env;
    (void)type;
    connection_t *connection = (connection_t *)(intptr_t)handle;
    int status = connection->engine == 0 ? tote_close(&connection->tote) : sqlite3_close(connection->sqlite);
    if (status == 0) free(connection);
    return status;
}

JNIEXPORT jobject JNICALL Java_org_totedb_ycsb_Native_execute(JNIEnv *env, jclass type, jlong handle,
    jbyteArray sql, jobjectArray values, jbooleanArray text)
{
    (void)type;
    connection_t *connection = (connection_t *)(intptr_t)handle;
    jsize count = (*env)->GetArrayLength(env, values);
    if ((*env)->GetArrayLength(env, text) != count) { throw_error(env, "parameter count mismatch"); return NULL; }
    char *query = copy_string(env, sql);
    if (query == NULL) return NULL;
    tote_param_t *params = calloc((size_t)count + 1, sizeof(*params));
    jboolean *types = (*env)->GetBooleanArrayElements(env, text, NULL);
    if (params == NULL || types == NULL) {
        if (types != NULL) (*env)->ReleaseBooleanArrayElements(env, text, types, JNI_ABORT);
        free(query); free(params);
        throw_error(env, "parameter allocation failed");
        return NULL;
    }
    for (jsize index = 0; index < count; index++) {
        jbyteArray value = (*env)->GetObjectArrayElement(env, values, index);
        jsize size = (*env)->GetArrayLength(env, value);
        uint8_t *bytes = malloc((size_t)size + 1);
        if (bytes == NULL) { (*env)->DeleteLocalRef(env, value); throw_error(env, "value allocation failed"); break; }
        (*env)->GetByteArrayRegion(env, value, 0, size, (jbyte *)bytes);
        (*env)->DeleteLocalRef(env, value);
        params[index] = (tote_param_t){.type = types[index] ? TOTE_PARAM_TEXT : TOTE_PARAM_BLOB, .bytes = bytes, .size = (size_t)size};
    }
    (*env)->ReleaseBooleanArrayElements(env, text, types, JNI_ABORT);
    jobject result = NULL;
    if (!(*env)->ExceptionCheck(env)) {
        result = connection->engine == 0 ? execute_tote(env, connection, query, params, (size_t)count)
                                        : execute_sqlite(env, connection, query, params, (size_t)count);
    }
    for (jsize index = 0; index < count; index++) free((void *)params[index].bytes);
    free(params);
    free(query);
    return result;
}

JNIEXPORT jstring JNICALL Java_org_totedb_ycsb_Native_version(JNIEnv *env, jclass type, jint engine)
{
    (void)type;
    return (*env)->NewStringUTF(env, engine == 0 ? tote_version() : sqlite3_libversion());
}

JNIEXPORT jlong JNICALL Java_org_totedb_ycsb_Native_begin(JNIEnv *env, jclass type, jlong handle)
{
    (void)env;
    (void)type;
    connection_t *connection = (connection_t *)(intptr_t)handle;
    if (connection->engine == 0) {
        tote_txn_t *transaction = NULL;
        int status = tote_begin(connection->tote, &transaction);
        return status == 0 ? (jlong)(intptr_t)transaction : -(jlong)status;
    }
    int status = sqlite3_exec(connection->sqlite, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    return status == SQLITE_OK ? 1 : -(jlong)status;
}

JNIEXPORT jint JNICALL Java_org_totedb_ycsb_Native_rollback(JNIEnv *env, jclass type, jlong handle, jlong token)
{
    (void)env;
    (void)type;
    connection_t *connection = (connection_t *)(intptr_t)handle;
    if (connection->engine == 0) {
        tote_txn_t *transaction = (tote_txn_t *)(intptr_t)token;
        return tote_rollback(&transaction);
    }
    return sqlite3_exec(connection->sqlite, "ROLLBACK", NULL, NULL, NULL);
}

JNIEXPORT jobject JNICALL Java_org_totedb_ycsb_Native_decode(JNIEnv *env, jclass type, jbyteArray encoded)
{
    (void)type;
    jsize size = (*env)->GetArrayLength(env, encoded);
    jbyte *bytes = (*env)->GetByteArrayElements(env, encoded, NULL);
    if (bytes == NULL) return NULL;
    jobject result = decode_tote(env, (const uint8_t *)bytes, (size_t)size);
    (*env)->ReleaseByteArrayElements(env, encoded, bytes, JNI_ABORT);
    return result;
}

JNIEXPORT jobjectArray JNICALL Java_org_totedb_ycsb_Native_compileOptions(JNIEnv *env, jclass type)
{
    (void)type;
    int count = 0;
    while (sqlite3_compileoption_get(count) != NULL) count++;
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (string_class == NULL) return NULL;
    jobjectArray options = (*env)->NewObjectArray(env, count, string_class, NULL);
    if (options == NULL) return NULL;
    for (int index = 0; index < count; index++) {
        jstring value = (*env)->NewStringUTF(env, sqlite3_compileoption_get(index));
        if (value == NULL) return NULL;
        (*env)->SetObjectArrayElement(env, options, index, value);
        (*env)->DeleteLocalRef(env, value);
    }
    return options;
}
