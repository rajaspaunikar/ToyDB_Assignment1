
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "tbl.h"
#include "codec.h"
#include "../pflayer/pf.h"

#define FREE_SPACE_OFFSET 0
#define SLOT_COUNT_OFFSET 2
#define SLOT_ARRAY_OFFSET 4
#define SLOT_ENTRY_SIZE 4

#define checkerr(err) {if (err < 0) {PF_PrintError(); exit(EXIT_FAILURE);}}

int  getLen(int slot, byte *pageBuf);
int  getNumSlots(byte *pageBuf);
void setNumSlots(byte *pageBuf, int nslots);
int  getNthSlotOffset(int slot, char* pageBuf);


int getLen(int slot, byte *pageBuf){
    int addrOfSlotEntry = SLOT_ARRAY_OFFSET + slot * SLOT_ENTRY_SIZE;
    int addrOfSlotEntryLen = addrOfSlotEntry + 2;
    return (int)DecodeShort((byte *)(pageBuf + addrOfSlotEntryLen));
}

int getNumSlots(byte* pageBuf){
    return (int)DecodeShort((byte *)(pageBuf + SLOT_COUNT_OFFSET));
}

void setNumSlots(byte* pageBuf, int nslots){
    EncodeShort((short)nslots, (byte *)(pageBuf + SLOT_COUNT_OFFSET));
}

int getNthSlotOffset(int slot, char* pageBuf){
    int addrOfSlotEntry = SLOT_ARRAY_OFFSET + slot * SLOT_ENTRY_SIZE;
    return (int)DecodeShort((byte *)(pageBuf + addrOfSlotEntry));
}

//Extra helper functions to get free space pointer, set free space pointer, set Nth Slot offset , set Nth slot length , page header init

int getFreeSpacePtr(byte *pagebuf){
    return (int)DecodeShort((byte *)(pagebuf + FREE_SPACE_OFFSET));
}

void setFreeSpacePtr(byte * pagebuf, int freeSpacePtr){
    EncodeShort((short)freeSpacePtr, (byte *)(pagebuf + FREE_SPACE_OFFSET));
}

void setLen(int slot, byte *pagebuf, int len){
    int addrOfSlotEntry = SLOT_ARRAY_OFFSET + slot * SLOT_ENTRY_SIZE;
    int addrOfSlotEntryLen = addrOfSlotEntry + 2;
    EncodeShort((short)len, (byte *)(pagebuf + addrOfSlotEntryLen));
}

void initPageHeader(byte* pageBuf){
    setNumSlots(pageBuf, 0);
    setFreeSpacePtr(pageBuf, PF_PAGE_SIZE);
}

/**
   Opens a paged file, creating one if it doesn't exist, and optionally
   overwriting it.
   Returns 0 on success and a negative error code otherwise.
   If successful, it returns an initialized Table*.
 */
int
Table_Open(char *dbname, Schema *schema, bool overwrite, Table **ptable)
{
    int fileDescriptor , error;
    Table *table;

    if(overwrite) {
        PF_DestroyFile(dbname);
    }

    // Initialize PF, create PF file,
    error = PF_CreateFile(dbname);
    checkerr(error);
    fileDescriptor = PF_OpenFile(dbname);
    checkerr(fileDescriptor);

    // allocate Table structure  and initialize and return via ptable
    table = (Table *)malloc(sizeof(Table));

    // The Table structure only stores the schema. The current functionality
    // does not really need the schema, because we are only concentrating
    // on record storage. 
    table->schema = schema;
    table->fd = fileDescriptor;
    table->currPage = -1;
    *ptable = table;

    return 0;
}

void
Table_Close(Table *tbl) {

    int error = PF_CloseFile(tbl->fd);
    checkerr(error);
    free(tbl);

}


int
Table_Insert(Table *tbl, byte *record, int len, RecId *rid) {
    int pageNum , err;
    char * pageBuf;
    int numSlots , freeSpacePtr , headerEnd , spaceNeeded , spaceAvailable , newDataStart;
    bool needNewPage = TRUE;

    if(tbl->currPage != -1) {
        err = PF_GetThisPage(tbl->fd, tbl->currPage, &pageBuf);
        checkerr(err);

        numSlots = getNumSlots((byte *)pageBuf);
        freeSpacePtr = getFreeSpacePtr((byte *)pageBuf);
        headerEnd = SLOT_ARRAY_OFFSET + numSlots * SLOT_ENTRY_SIZE;
        spaceNeeded = len + SLOT_ENTRY_SIZE;
        spaceAvailable = freeSpacePtr - headerEnd;

        if(spaceAvailable >= spaceNeeded && numSlots < 256*256) {
            pageNum = tbl->currPage;
            needNewPage = FALSE;
        }
        else{
            err = PF_UnfixPage(tbl->fd, tbl->currPage, FALSE);
            checkerr(err);
        }

    }

    // Allocate a fresh page if len is not enough for remaining space

    if(needNewPage) {
        err = PF_AllocPage(tbl->fd, &pageNum, &pageBuf);
        checkerr(err);
        initPageHeader((byte *)pageBuf);
        numSlots = 0;
        freeSpacePtr = PF_PAGE_SIZE;
        tbl->currPage = pageNum;
    }

    newDataStart = freeSpacePtr - len;
    memcpy(pageBuf + newDataStart, record, len);

    setNthSlotOffset(numSlots, pageBuf, newDataStart);
    setLen(numSlots, pageBuf, len);
    setNumSlots(pageBuf, numSlots + 1);
    setFreeSpacePtr(pageBuf, newDataStart);

    *rid = (pageNum << 16) | numSlots;

    err = PF_UnfixPage(tbl->fd, pageNum, TRUE);
    checkerr(err);

    return 0;

    // Get the next free slot on page, and copy record in the free
    // space
    // Update slot and free space index information on top of page.
}


/*
  Given an rid, fill in the record (but at most maxlen bytes).
  Returns the number of bytes copied.
 */
int
Table_Get(Table *tbl, RecId rid, byte *record, int maxlen) {
    int slot = rid & 0xFFFF;
    int pageNum = rid >> 16;
    char * pageBuf;

        // PF_GetThisPage(pageNum)
    int err = PF_GetThisPage(tbl->fd, pageNum, &pageBuf);
    checkerr(err);

        // In the page get the slot offset of the record
    int offset = getNthSlotOffset(slot, pageBuf);
    int len = getLen(slot, (byte *)pageBuf);

        // memcpy bytes into the record supplied.
    len = min(len, maxlen);
    memcpy(record, pageBuf + offset, len);

      // Unfix the page
    err = PF_UnfixPage(tbl->fd, pageNum, FALSE);
    checkerr(err);
  
    return len; // return size of record
}

void
Table_Scan(Table *tbl, void *callbackObj, ReadFunc callbackfn) {

    int pageNum , err , slot , numSlots , offset , len;
    char * pageBuf;
    RecId rid;

    err = PF_GetFirstPage(tbl->fd, &pageNum, &pageBuf);
    while(err == PFE_OK) {
        numSlots = getNumSlots((byte *)pageBuf);
        for(slot = 0; slot < numSlots; slot++) {
            offset = getNthSlotOffset(slot, pageBuf);
            len = getLen(slot, (byte *)pageBuf);
            rid = (pageNum << 16) | slot;
            callbackfn(callbackObj, rid, (byte *)(pageBuf + offset), len);
        }
        err = PF_UnfixPage(tbl->fd, pageNum, FALSE);
        checkerr(err);
        err = PF_GetNextPage(tbl->fd, &pageNum, &pageBuf);
    }
    if(err != PFE_EOF) {
        checkerr(err);
    }
    // For each page obtained using PF_GetFirstPage and PF_GetNextPage
    //    for each record in that page,
    //          callbackfn(callbackObj, rid, record, recordLen)
}


