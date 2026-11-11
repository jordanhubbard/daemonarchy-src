/* SPDX-License-Identifier: BSD-2-Clause */
static struct bcm2712_pcie_softc sc;
static void fresh(void)
{
 memset(&sc,0,sizeof(sc));memset(resources,0,sizeof(resources));
 physical_tag=(struct tag){0};translated_tag=(struct tag){0};
 host=(struct device){&sc,FIXTURE_HOST,"bcm2712_pcie",true,false};
 msi=(struct device){NULL,FIXTURE_MSI,"bcm2712_mip",true,false};
 gpio=(struct device){NULL,FIXTURE_GPIO,"bcmstbgpio",true,false};
 pci=(struct device){0};sc.dev=&host;sc.node=FIXTURE_HOST;sc.msi=&msi;sc.msi_node=FIXTURE_MSI;sc.gpio=&gpio;sc.gpio_node=FIXTURE_GPIO;
 reset_provider[0]=(struct device){NULL,OF_node_from_xref(0x23),"bcmstb_reset",true,false};reset_provider[1]=(struct device){NULL,OF_node_from_xref(0x24),"bcmstb_reset",true,false};provider_fixture=false;missing_provider=0;
 sc.plan.outbound[0]=(struct bcm2712_pcie_window){0,UINT64_C(0x1f00000000),0xfffffffc};
 sc.plan.outbound[1]=(struct bcm2712_pcie_window){UINT64_C(0x400000000),UINT64_C(0x1c00000000),UINT64_C(0x300000000)};
 sc.plan.raw_msi=(struct bcm2712_dma_msi_route){UINT64_C(0xfffffff000),UINT64_C(0x1000130000),0x1000};
 resources[0].start=UINT64_C(0x1000120000);resources[0].size=0x9310;
 step=fail_step=writes=reads=index_writes=data_accesses=children=delegates=of_scans=0;
 domain_error=resource_error=destroy_error=fini_error=release_error=msi_error=child_error=delete_error=0;
 unknown_property=property_loop=child_missing=child_unattached=false;memset(entries,0,sizeof(entries));msi_allocations=msi_releases=cleanup_calls=cleanup_fail_at=0;
 mapped_address=UINT64_C(0xfffffff000);mapped_data=63;
}
static void empty(void)
{
 assert(!physical_tag.alive&&!translated_tag.alive&&!sc.lock_initialized);
 for(unsigned i=0;i<3;i++)assert(!resources[i].allocated&&!sc.rman_initialized[i]);
 assert(!entries[0]&&!entries[1]&&!sc.bridge&&!sc.rescal&&!sc.mem);
}
int main(void)
{
 for(unsigned i=1;i<=8;i++){
  fresh();assert(software_setup(&sc)==0);cleanup_fail_at=i;
  assert(software_cleanup(&sc)==EBUSY&&sc.bridge&&sc.rescal&&sc.lock_initialized);
  cleanup_fail_at=0;assert(software_cleanup(&sc)==0);empty();
 }
 fresh();provider_fixture=true;assert(preflight(&sc)==0&&of_scans==25&&writes==0);
 for(int x=0x23;x<=0x27;x++){if(x==0x25||x==0x26)continue;missing_provider=x;assert(preflight(&sc)==ENXIO&&writes==0&&step==0);}missing_provider=0;
 missing_provider=OF_xref_from_node(gpio.node);assert(preflight(&sc)==ENXIO&&writes==0&&step==0);missing_provider=0;
 msi_error=ESRCH;assert(preflight(&sc)==ESRCH&&writes==0&&step==0);
 fresh();assert(property_plan(10)==0&&of_scans==2);
 unknown_property=true;assert(property_plan(10)==EOPNOTSUPP);
 property_loop=true;assert(property_plan(10)==EINVAL);
 fresh();assert(software_setup(&sc)==0);unsigned count=step;assert(count>10);assert(writes==0);
 assert(sc.generic.base.ecam==2&&sc.generic.base.dmat==&translated_tag&&physical_tag.children==1);
 assert(sc.generic.base.mem_rman.last==0xfffffffb&&sc.generic.base.pmem_rman.last==UINT64_C(0x6ffffffff));
 assert(bcm2712_pcie_get_rman(&host,SYS_RES_MEMORY,RF_PREFETCHABLE)==&sc.generic.base.pmem_rman);
 assert(bcm2712_pcie_get_rman(&host,SYS_RES_MEMORY,0)==&sc.generic.base.mem_rman);
 assert(generic_pcie_containing_range(&host,SYS_RES_MEMORY,0xfffffffb,0xfffffffb)==&sc.generic.base.ranges[0]);
 for(uint64_t tail=0xfffffffc;tail<=0xffffffff;tail++)assert(generic_pcie_containing_range(&host,SYS_RES_MEMORY,tail,tail)==NULL);
 uint64_t translated=0;assert(generic_pcie_translate_resource(&host,SYS_RES_MEMORY,UINT64_C(0x6ffffffff),&translated)==0&&translated==UINT64_C(0x1effffffff));
 struct resource endpoint={.start=UINT64_C(0x400001000),.size=0x1000,.type=SYS_RES_MEMORY,.flags=RF_ACTIVE};struct resource_map map;struct resource_map_request request={.offset=32,.length=16};
 assert(generic_pcie_map_resource(&host,&pci,&endpoint,&request,&map)==0&&mapped_backing==resources+2&&mapped_offset==0x1020&&mapped_length==16);
 assert(generic_pcie_unmap_resource(&host,&pci,&endpoint,&map)==0&&!mapped_backing);
 assert(generic_pcie_get_dma_tag(&host,&pci)==&translated_tag);
 endpoint.start=0xfffffffc;endpoint.size=4;assert(generic_pcie_map_resource(&host,&pci,&endpoint,NULL,&map)==ENOENT);
 assert(software_cleanup(&sc)==0);empty();
 for(unsigned i=1;i<=count;i++){fresh();fail_step=i;assert(software_setup(&sc)!=0);assert(writes==0);assert(software_cleanup(&sc)==0);empty();}
 fresh();domain_error=EIO;assert(software_setup(&sc)==EIO&&!translated_tag.alive);assert(software_cleanup(&sc)==0);empty();
 fresh();domain_error=ENOENT;assert(software_setup(&sc)==0);assert(software_cleanup(&sc)==0);empty();
 fresh();resource_error=EIO;assert(software_setup(&sc)==EIO&&!entries[0]);assert(software_cleanup(&sc)==0);empty();
 fresh();entries[0]=true;assert(software_setup(&sc)==EEXIST&&entries[0]);assert(software_cleanup(&sc)==0);assert(entries[0]);entries[0]=false;empty();
 for(unsigned which=0;which<3;which++){
  fresh();assert(software_setup(&sc)==0);
  if(which==0)destroy_error=EBUSY;else if(which==1)fini_error=EBUSY;else release_error=EBUSY;
  assert(software_cleanup(&sc)==EBUSY&&sc.mem&&sc.lock_initialized&&sc.bridge&&sc.rescal);
  assert(resources[1].allocated&&resources[2].allocated);
  destroy_error=fini_error=release_error=0;assert(software_cleanup(&sc)==0);empty();
 }
 fresh();assert(software_setup(&sc)==0);sc.ready=true;sc.state.link=true;resources[0].words[0x4068/4]=0x30;
 bcm2712_pcie_write_config(&host,1,0,0,4,0xaabbccdd,4);assert(index_writes==1&&data_accesses==1);
 assert(resources[0].words[0x9000/4]==0x100000&&bcm2712_pcie_read_config(&host,1,0,0,4,4)==0xaabbccdd);
 unsigned indices=index_writes,data=data_accesses;
 resources[0].words[0x4068/4]=0;assert(bcm2712_pcie_read_config(&host,1,0,0,4,4)==UINT32_MAX);
 bcm2712_pcie_write_config(&host,1,0,0,4,0,4);assert(index_writes==indices&&data_accesses==data);
 bcm2712_pcie_write_config(&host,0,0,0,4,0x1122,2);assert(bcm2712_pcie_read_config(&host,0,0,0,4,2)==0x1122);
 indices=index_writes;assert(bcm2712_pcie_read_config(&host,0,1,0,4,4)==UINT32_MAX&&index_writes==indices);
 uint64_t addr=123;uint32_t message=456;assert(bcm2712_pcie_map_msi(&host,&pci,1,&addr,&message)==0&&addr==mapped_address&&message==63);
 mapped_address=0x1000130000;addr=123;message=456;assert(bcm2712_pcie_map_msi(&host,&pci,1,&addr,&message)==EINVAL&&addr==123&&message==456);
 mapped_address=UINT64_C(0xfffffff000);mapped_data=64;assert(bcm2712_pcie_map_msi(&host,&pci,1,&addr,&message)==EINVAL);
 int irq=1;assert(bcm2712_pcie_alloc_msi(&host,&pci,1,1,&irq)==0&&msi_allocations==1);
 sc.ready=false;assert(bcm2712_pcie_alloc_msix(&host,&pci,&irq)==ENXIO&&msi_allocations==1);
 assert(bcm2712_pcie_release_msi(&host,&pci,1,&irq)==0&&msi_releases==1);
 assert(!bcm2712_pcie_alloc_resource(&host,&pci,SYS_RES_MEMORY,0,0,UINT64_MAX,4,0));
 bcm2712_pcie_new_pass(&host);bcm2712_pcie_driver_added(&host,NULL);assert(delegates==0);
 assert(bcm2712_pcie_refuse(&host)==EBUSY);assert(software_cleanup(&sc)==0);empty();
 for(unsigned mode=0;mode<3;mode++){
  fresh();assert(software_setup(&sc)==0);sc.state.touched=true;sc.state.link=true;
  child_missing=mode==0;child_unattached=mode==1;
  int result=publish_children(&sc);
  if(mode<2){assert(result==(mode==0?ENOMEM:ENXIO)&&!sc.ready);if(mode==1)assert(pci.disabled&&sc.child==&pci);}
  else {assert(result==0&&sc.ready&&pci.attached);bcm2712_pcie_new_pass(&host);bcm2712_pcie_driver_added(&host,NULL);assert(delegates==2);}
  assert(software_cleanup(&sc)==EBUSY&&sc.mem&&physical_tag.alive&&translated_tag.alive);
 }
 fresh();assert(software_setup(&sc)==0);sc.state.touched=true;sc.state.link=true;child_error=EIO;
 assert(publish_children(&sc)==EIO&&children==1&&sc.child==&pci&&pci.disabled&&!sc.ready);
 bcm2712_pcie_new_pass(&host);bcm2712_pcie_driver_added(&host,NULL);assert(delegates==0);
 assert(software_cleanup(&sc)==EBUSY&&physical_tag.alive&&translated_tag.alive&&resources[0].allocated);
 puts("PASS actual frontend property iteration, all allocation rollback, lookup errors, busy owner retry, live config link loss, raw MSI and guarded child retention");
 return 0;
}
