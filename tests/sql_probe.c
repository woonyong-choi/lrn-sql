#include "db.h"
#include "storage/pager.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: sql_probe DB_PATH SQL\n");
        return 2;
    }

    pager_t pager;
    if (pager_open(&pager, argv[1], access(argv[1], F_OK) != 0) != 0) {
        fprintf(stderr, "database open failed\n");
        return 2;
    }
    db_init();
    exec_result_t result = db_execute(&pager, argv[2]);
    if (result.status == 0) {
        puts("OK");
        if (result.out_buf != NULL) fputs(result.out_buf, stdout);
    } else {
        puts("ERROR");
    }
    if (result.message[0] != '\0') fprintf(stderr, "%s\n", result.message);
    free(result.out_buf);
    db_destroy();
    pager_close(&pager);
    return 0;
}
