#include "db.h"

uint32_t db_abi_version(void) { return DB_ABI_VERSION; }

size_t db_abi_sizeof(unsigned kind) {
    switch (kind) {
        case 1: return sizeof(DbRow);
        case 2: return sizeof(DbResult);
        case 3: return sizeof(DbError);
        case 4: return sizeof(DbStats);
        default: return 0;
    }
}
