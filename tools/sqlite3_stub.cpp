#include <sqlite3.h>

extern "C" {

int sqlite3_open_v2(const char*, sqlite3** ppDb, int, const char*) {
    if (ppDb != nullptr) {
        *ppDb = nullptr;
    }
    return SQLITE_MISUSE;
}

const char* sqlite3_errmsg(sqlite3*) {
    return "SQLite is not linked in this CUDA layer-only build";
}

int sqlite3_busy_timeout(sqlite3*, int) {
    return SQLITE_MISUSE;
}

int sqlite3_exec(sqlite3*, const char*, sqlite3_callback, void*, char**) {
    return SQLITE_MISUSE;
}

void sqlite3_free(void*) {
}

int sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**) {
    return SQLITE_MISUSE;
}

int sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*)) {
    return SQLITE_MISUSE;
}

int sqlite3_step(sqlite3_stmt*) {
    return SQLITE_MISUSE;
}

int sqlite3_reset(sqlite3_stmt*) {
    return SQLITE_MISUSE;
}

int sqlite3_clear_bindings(sqlite3_stmt*) {
    return SQLITE_MISUSE;
}

int sqlite3_finalize(sqlite3_stmt*) {
    return SQLITE_MISUSE;
}

int sqlite3_close_v2(sqlite3*) {
    return SQLITE_MISUSE;
}

}
