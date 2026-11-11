/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "boottrace_fat.h"

#define BYTES (128ULL << 20)
#define FAT_OFF (32ULL * 512)
#define FAT_BYTES (2048ULL * 512)
#define DATA_OFF (FAT_OFF + 2 * FAT_BYTES)
static unsigned char boot[512], fats[2][1024], root[512], normal[512];
static unsigned reads;
static int fail_read;

static void put16(unsigned char *p, unsigned v) { p[0]=v; p[1]=v>>8; }
static void put32(unsigned char *p, uint32_t v) { put16(p,v); put16(p+2,v>>16); }
static void link_cluster(unsigned cluster, uint32_t next) {
    put32(fats[0]+cluster*4,next); put32(fats[1]+cluster*4,next);
}
static void entry(unsigned char *p, const char *name, unsigned attr, unsigned first, unsigned size) {
    memcpy(p,name,11);p[11]=attr;put16(p+20,first>>16);put16(p+26,first);put32(p+28,size);
}
static int read_at(void *arg, uint64_t off, void *out, size_t n) {
    (void)arg;
    reads++;
    assert(n==512 && off%512==0 && off<=BYTES-n);
    if(fail_read) return -1;
    memset(out,0,n);
    if(off==0)memcpy(out,boot,n);
    else if(off>=FAT_OFF && off<FAT_OFF+sizeof(fats[0]))memcpy(out,fats[0]+(off-FAT_OFF),n);
    else if(off>=FAT_OFF+FAT_BYTES && off<FAT_OFF+FAT_BYTES+sizeof(fats[1]))
        memcpy(out,fats[1]+(off-FAT_OFF-FAT_BYTES),n);
    else if(off==DATA_OFF)memcpy(out,root,n);
    else if(off==DATA_OFF+512)memcpy(out,normal,n);
    return 0;
}
static void fixture(void) {
    unsigned i;
    memset(boot,0,sizeof(boot));memset(fats,0,sizeof(fats));memset(root,0,sizeof(root));memset(normal,0,sizeof(normal));
    reads=0;fail_read=0;
    put16(boot+11,512);boot[13]=1;put16(boot+14,32);boot[16]=2;boot[21]=0xf8;
    put32(boot+32,BYTES/512);put32(boot+36,2048);put32(boot+44,2);boot[510]=0x55;boot[511]=0xaa;
    link_cluster(0,0x0ffffff8);link_cluster(1,0x0fffffff);
    for(i=2;i<=5;i++)link_cluster(i,0x0fffffff);
    for(i=10;i<41;i++)link_cluster(i,i+1);
    link_cluster(41,0x0fffffff);
    for(i=42;i<73;i++)link_cluster(i,i+1);
    link_cluster(73,0x0fffffff);
    entry(root,"NORMAL     ",0x10,3,0);entry(root+32,"INSTALL ID ",0x20,4,181);
    entry(root+64,"STATE   TXT",0x20,5,512);
    entry(normal,"0001    TXT",0x20,10,16384);entry(normal+32,"0002    TXT",0x20,42,16384);
}
static void valid_open(struct bt_fat *f) { assert(bt_fat_open(f,read_at,NULL,BYTES)==0); }
int main(void) {
    struct bt_fat f;
    struct bt_fat_entry e,dir;
    unsigned checks=0;
    fixture();valid_open(&f);
    assert(bt_fat_find(&f,0,"NORMAL     ",&dir)==0 && dir.first==3);
    assert(bt_fat_file(&f,dir.first,"0001    TXT",&e)==0);
    assert(e.off==DATA_OFF+8*512 && e.size==16384);checks++;
    assert(bt_fat_file(&f,0,"INSTALL ID ",&e)==0 && e.size==181);checks++;
    assert(bt_fat_file(&f,3,"0002    TXT",&e)==0 && e.off==DATA_OFF+40*512);checks++;
    fixture();boot[510]=0;assert(bt_fat_open(&f,read_at,NULL,BYTES)!=0);checks++;
    fixture();put16(boot+11,513);assert(bt_fat_open(&f,read_at,NULL,BYTES)!=0);checks++;
    fixture();boot[40]=0x80;assert(bt_fat_open(&f,read_at,NULL,BYTES)!=0);checks++;
    fixture();link_cluster(1,0x07ffffff);assert(bt_fat_open(&f,read_at,NULL,BYTES)!=0);checks++;
    fixture();put32(fats[1]+4,0x07ffffff);assert(bt_fat_open(&f,read_at,NULL,BYTES)!=0);checks++;
    fixture();put32(fats[1]+10*4,12);valid_open(&f);
    assert(bt_fat_file(&f,3,"0001    TXT",&e)!=0);checks++;
    fixture();link_cluster(10,12);valid_open(&f);assert(bt_fat_file(&f,3,"0001    TXT",&e)!=0);checks++;
    fixture();link_cluster(10,0x0fffffff);valid_open(&f);assert(bt_fat_file(&f,3,"0001    TXT",&e)!=0);checks++;
    fixture();link_cluster(41,10);valid_open(&f);assert(bt_fat_file(&f,3,"0001    TXT",&e)!=0);checks++;
    fixture();link_cluster(10,0);valid_open(&f);assert(bt_fat_file(&f,3,"0001    TXT",&e)!=0);checks++;
    fixture();memcpy(normal+64,normal,32);valid_open(&f);
    assert(bt_fat_file(&f,3,"0001    TXT",&e)!=0);checks++;
    fixture();memset(normal,0xe5,sizeof(normal));link_cluster(3,3);valid_open(&f);
    assert(bt_fat_file(&f,3,"0001    TXT",&e)!=0 && reads<80);checks++;
    fixture();link_cluster(2,0);valid_open(&f);
    assert(bt_fat_find(&f,0,"NORMAL     ",&e)!=0);checks++;
    fixture();normal[11]|=1;valid_open(&f);
    assert(bt_fat_file(&f,3,"0001    TXT",&e)==0 && (e.attributes&1));checks++;
    fixture();fail_read=1;assert(bt_fat_open(&f,read_at,NULL,BYTES)!=0);checks++;
    fixture();assert(bt_fat_open(&f,read_at,NULL,512)!=0);checks++;
    printf("normal FAT extents: %u controls passed\n",checks);
    return 0;
}
