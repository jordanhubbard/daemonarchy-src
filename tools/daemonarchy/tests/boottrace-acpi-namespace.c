/* SPDX-License-Identifier: BSD-2-Clause */
/* Exercise the production bounded reader, without evaluating AML. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "boottrace_acpi.h"

struct region { uint64_t address; size_t size; uint8_t data[BT_ACPI_MAX]; };
static struct region regions[40];
static unsigned nr, reads, forbidden_reads;
struct result { char name[8]; size_t size; char why[128]; uint8_t data[BT_ACPI_MAX]; };
static struct result results[40];
static unsigned emitted;
static bool change_signature;

static void put32(uint8_t *p, uint32_t v) { for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(i*8)); }
static void put64(uint8_t *p, uint64_t v) { put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32)); }
static void seal(uint8_t *p,size_t n) { unsigned sum=0;p[9]=0;for(size_t i=0;i<n;i++)sum+=p[i];p[9]=(uint8_t)-sum; }
static struct region *add(uint64_t a,const char *signature,size_t n) {
 assert(nr<40 && n<=BT_ACPI_MAX);struct region *r=&regions[nr++];
 r->address=a;r->size=n;memcpy(r->data,signature,4);put32(r->data+4,(uint32_t)n);seal(r->data,n);return r;
}
static int read_memory(void *arg,uint64_t a,void *out,size_t n) {
 (void)arg;reads++;
 for(unsigned i=0;i<nr;i++) {struct region *r=&regions[i];
  if(a>=r->address && a-r->address<r->size && n<=r->size-(size_t)(a-r->address)) {
   memcpy(out,r->data+(size_t)(a-r->address),n);
   if(change_signature && a==0x2000 && n==148) {memcpy(out,"NOPE",4);seal(out,n);}
   return 0;
  }
 }
 forbidden_reads++;return -1;
}
static void emit(void *arg,const char *name,const void *data,size_t n,const char *why) {
 (void)arg;assert(emitted<40 && strlen(name)<8 && n<=BT_ACPI_MAX);
 struct result *r=&results[emitted++];strcpy(r->name,name);r->size=n;
 if(data) {assert(!why);memcpy(r->data,data,n);} else {assert(why && n==0);snprintf(r->why,sizeof(r->why),"%s",why);}
}
static struct result *get(const char *name) {
 for(unsigned i=0;i<emitted;i++)if(!strcmp(results[i].name,name))return &results[i];return NULL;
}
static void reset(void) {
 memset(regions,0,sizeof(regions));memset(results,0,sizeof(results));nr=reads=forbidden_reads=emitted=0;change_signature=false;
 struct region *r=add(0x100,"RSD ",36);memcpy(r->data,"RSD PTR ",8);r->data[15]=2;put32(r->data+20,36);put64(r->data+24,0x1000);
 r->data[8]=0;r->data[9]=0;unsigned sum=0;for(unsigned i=0;i<20;i++)sum+=r->data[i];r->data[8]=(uint8_t)-sum;
 r->data[32]=0;sum=0;for(unsigned i=0;i<36;i++)sum+=r->data[i];r->data[32]=(uint8_t)-sum;
 r=add(0x1000,"XSDT",44);put64(r->data+36,0x2000);seal(r->data,r->size);
 r=add(0x2000,"FACP",148);put32(r->data+40,0x3000);put64(r->data+140,UINT64_C(0x100004000));seal(r->data,r->size);
 add(0x3000,"DSDT",48)->data[40]=1;seal(regions[3].data,48);
 add(UINT64_C(0x100004000),"DSDT",48)->data[40]=2;seal(regions[4].data,48);
}
static void root_ssdts(unsigned count) {
 struct region *r=&regions[1];r->size=44+count*8;put32(r->data+4,(uint32_t)r->size);
 for(unsigned i=0;i<count;i++) {put64(r->data+44+i*8,0x10000+i*0x100);struct region *s=add(0x10000+i*0x100,"SSDT",48);s->data[40]=(uint8_t)i;seal(s->data,48);}
 seal(r->data,r->size);
}
static void capture(void) {assert(bt_acpi_namespace_snapshot(0x100,read_memory,emit,NULL)==0);}
static void no_dsdt(void) {capture();assert(get("DSDT") && get("DSDT")->size==0 && get("DSDT")->why[0]);}

/* This production function is inserted verbatim by the runner. */
struct bt_map_ent { char path[24]; size_t size; };
static struct bt_map_ent slots[18];
static unsigned slot_count;
static int bt_mapv;
static const struct bt_map_ent *bt_map_find(int *map,const char *path) {
 assert(map==&bt_mapv);for(unsigned i=0;i<slot_count;i++)if(!strcmp(slots[i].path,path))return &slots[i];return NULL;
}
#include "namespace-slots-function.h"
static void all_slots(void) {
 slot_count=18;strcpy(slots[0].path,"\\ACPI\\DSDT.BIN");strcpy(slots[1].path,"\\ACPI\\SSOV.BIN");
 for(unsigned i=0;i<16;i++)snprintf(slots[i+2].path,sizeof(slots[i+2].path),"\\ACPI\\SS0%X.BIN",i);
 for(unsigned i=0;i<18;i++)slots[i].size=BT_ACPI_MAX;
}
int main(void) {
 unsigned cases=0;
 reset();assert(bt_acpi_snapshot(0x100,read_memory,emit,NULL)==0);assert(!get("DSDT") && !get("SS00"));cases++;
 reset();root_ssdts(2);capture();assert(get("DSDT")->size==48 && get("DSDT")->data[40]==2);assert(get("SS00")->data[40]==0 && get("SS01")->data[40]==1);assert(!get("SS02")->size && !strcmp(get("SSOV")->why,"not present"));assert(emitted==27);cases++;
 reset();put64(regions[2].data+140,0);seal(regions[2].data,148);capture();assert(get("DSDT")->data[40]==1);cases++;
 reset();regions[2].size=44;put32(regions[2].data+4,44);seal(regions[2].data,44);capture();assert(get("DSDT")->data[40]==1);cases++;
 reset();regions[2].size=40;put32(regions[2].data+4,40);seal(regions[2].data,40);no_dsdt();assert(!forbidden_reads);cases++;
 reset();put64(regions[2].data+140,0xfffffff000ULL);seal(regions[2].data,148);no_dsdt();assert(forbidden_reads==1);cases++;
 reset();regions[2].data[50]^=1;no_dsdt();assert(!get("FACP")->size && !forbidden_reads);cases++;
 reset();memcpy(regions[4].data,"SSDT",4);seal(regions[4].data,48);no_dsdt();cases++;
 reset();regions[4].data[41]^=1;no_dsdt();cases++;
 reset();put32(regions[4].data+4,BT_ACPI_MAX+1);no_dsdt();assert(!forbidden_reads);cases++;
 reset();change_signature=true;no_dsdt();assert(strstr(get("FACP")->why,"changed"));cases++;
 reset();root_ssdts(17);capture();assert(get("SS0F")->size==48 && get("SS0F")->data[40]==15);assert(strstr(get("SSOV")->why,"limit exceeded"));assert(emitted==27);cases++;
 reset();root_ssdts(2);regions[5].data[41]^=1;capture();assert(!get("SS00")->size && get("SS01")->size==48);cases++;
 reset();regions[1].size=36;put32(regions[1].data+4,36);seal(regions[1].data,36);no_dsdt();assert(!get("SS00")->size);cases++;
 reset();put64(regions[2].data+140,UINT64_MAX-8);seal(regions[2].data,148);no_dsdt();assert(forbidden_reads==1);cases++;
 reset();root_ssdts(1);regions[5].size=36;put32(regions[5].data+4,BT_ACPI_MAX+1);capture();assert(!get("SS00")->size && strstr(get("SS00")->why,"limit"));cases++;
 all_slots();assert(bt_acpi_namespace_slots());cases++;
 for(unsigned i=0;i<18;i++) {all_slots();strcpy(slots[i].path,"unrelated");assert(!bt_acpi_namespace_slots());}cases++;
 for(unsigned i=0;i<18;i++) {all_slots();slots[i].size--;assert(!bt_acpi_namespace_slots());}cases++;
 slot_count=0;assert(!bt_acpi_namespace_slots());cases++;
 reset();root_ssdts(1);put64(regions[1].data+44,0xfffffff000ULL);seal(regions[1].data,regions[1].size);capture();assert(strstr(get("SSOV")->why,"unreadable"));cases++;
 reset();root_ssdts(1);put64(regions[1].data+44,0);seal(regions[1].data,regions[1].size);capture();assert(strstr(get("SSOV")->why,"unreadable"));cases++;
 reset();root_ssdts(1);put64(regions[1].data+44,0x2000);seal(regions[1].data,regions[1].size);capture();assert(strstr(get("SSOV")->why,"ambiguous"));cases++;
 reset();regions[0].data[15]=0;put32(regions[0].data+16,0x1000);regions[0].data[8]=0;unsigned sum=0;for(unsigned i=0;i<20;i++)sum+=regions[0].data[i];regions[0].data[8]=(uint8_t)-sum;
 memcpy(regions[1].data,"RSDT",4);regions[1].size=40;put32(regions[1].data+4,40);put32(regions[1].data+36,0x2000);seal(regions[1].data,40);capture();assert(get("RSDP")->size==20 && get("DSDT")->data[40]==2);cases++;
 printf("%u namespace reader and legacy-media guard cases passed\n",cases);return 0;
}
