#include <stdio.h>
#include <stdlib.h>
#include "codec.h"
#include "tbl.h"
#include "util.h"
#include "../pflayer/pf.h"
#include "../amlayer/am.h"
#define checkerr(err)        \
    {                        \
        if (err < 0)         \
        {                    \
            PF_PrintError("dblayer error"); \
            exit(1);         \
        }                    \
    }

void printRow(void *callbackObj, RecId rid, byte *row, int len)
{
    Schema *schema = (Schema *)callbackObj;
    byte *cursor = row;

    for (int columnIndex = 0; columnIndex < schema->numColumns; columnIndex++)
    {
        ColumnDesc *col = schema->columns[columnIndex];

        if (columnIndex > 0)
        {
            printf(",");
        }
        switch (col->type)
        {
        case VARCHAR:
        {
            char str[PF_PAGE_SIZE];
            int strLen = DecodeCString(cursor, str, sizeof(str));
            printf("%s", str);
            cursor += strLen + 2;
            break;
        }
        case INT:
        {
            int val = DecodeInt(cursor);
            printf("%d", val);
            cursor += sizeof(int);
            break;
        }
        case LONG:
        {
            long val = DecodeLong(cursor);
            printf("%ld", val);
            cursor += 8;
            break;
        }
        default:
            fprintf(stderr, "Unknown column type %d\n", col->type);
            exit(EXIT_FAILURE);
        }
    }
    printf("\n");
}

#define DB_NAME "data.db"
#define INDEX_NAME "data.db.0"

void index_scan(Table *tbl, Schema *schema, int indexFD, int op, int value)
{
    /*
    Open index ...
    while (true) {
    find next entry in index
    fetch rid from table
        printRow(...)
    }
    close index ...
    */
    int scanDesc = AM_OpenIndexScan(indexFD, 'i', sizeof(int), op, (char *)&value);
    if (scanDesc < 0)
    {
        AM_PrintError("AM_OpenIndexScan failed");
        exit(EXIT_FAILURE);
    }
    while (1)
    {
        int errVal = AM_FindNextEntry(scanDesc);
        if (errVal == AME_EOF)
        {
            break;
        }
        else if (errVal < 0)
        {
            AM_PrintError("AM_FindNextEntry failed");
            exit(EXIT_FAILURE);
        }
        RecId rid = errVal;
        byte record[PF_PAGE_SIZE];
        int len = Table_Get(tbl, rid, (byte *)record, sizeof(record));
        printRow(schema, rid, (byte *)record, len);
    }
    // Close the scan
    int errVal = AM_CloseIndexScan(scanDesc);
    if (errVal < 0)
    {
        AM_PrintError("AM_CloseIndexScan failed");
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char **argv)
{
    char *schemaTxt = "Country:varchar,Capital:varchar,Population:int";
    Schema *schema = parseSchema(schemaTxt);
    Table *tbl;

    int err = Table_Open(DB_NAME, schema, FALSE, &tbl);
    checkerr(err);
    if (argc == 2 && *(argv[1]) == 's')
    {
        // invoke Table_Scan with printRow, which will be invoked for each row in the table.
        Table_Scan(tbl, schema, printRow);
    }
    else
    {
        // index scan by default
        int indexFD = PF_OpenFile(INDEX_NAME);
        checkerr(indexFD);

        // Ask for populations less than 100000, then more than 100000. Together they should
        // yield the complete database.
        index_scan(tbl, schema, indexFD, LESS_THAN_EQUAL, 100000);
        index_scan(tbl, schema, indexFD, GREATER_THAN, 100000);
    }
    Table_Close(tbl);
}
