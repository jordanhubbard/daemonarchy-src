/* SPDX-License-Identifier: BSD-2-Clause */
int main(void)
{
 struct devclass generic={.name="gpio"},selected={.name="bcmstbgpio"},childclass={.name="gpiobus"};
 TAILQ_INIT(&generic.drivers);TAILQ_INIT(&selected.drivers);TAILQ_INIT(&childclass.drivers);
 driver_t ofw={.name="gpiobus"};struct driverlink generic_link={.driver=&ofw,.pass=10},selected_link={.driver=&ofw,.pass=10};
 TAILQ_INSERT_TAIL(&generic.drivers,&generic_link,link);
 struct device parent={.devclass=&selected,.node=123},child={.devclass=&childclass,.parent=&parent},other={.parent=&parent};controller=&parent;sc.busdev=&child;
 node_method=ofw_bus_default_get_node;
 assert(device_probe_child(&parent,&child)==ENXIO&&probes==0);
 puts("PASS original bcmstbgpio devclass cannot find drivers registered only on gpio");
 if(ACTUAL_REGISTRATION)TAILQ_INSERT_TAIL(&selected.drivers,&selected_link,link);
 assert(device_probe_child(&parent,&child)==ENXIO);
 assert(ACTUAL_REGISTRATION?probes==1:probes==0);
 puts("PASS registration alone preserves OFW probe refusal when child node method is absent");
 node_method=bcmstb_gpio_get_node;probes=0;
 int error=device_probe_child(&parent,&child);
 if(EXPECT_UNBOUND){assert(!ACTUAL_REGISTRATION&&!ACTUAL_FORWARDING&&error==ENXIO);puts("PASS actual frozen0231 source reproduces ENXIO without child probe");}
 else {assert(ACTUAL_REGISTRATION&&ACTUAL_FORWARDING&&error==0&&child.state==DS_ALIVE&&probes==1);assert(bcmstb_gpio_get_node(&parent,&child)==123);assert(bcmstb_gpio_get_node(&parent,&other)==(phandle_t)-1);sc.busdev=NULL;assert(bcmstb_gpio_get_node(&parent,&child)==(phandle_t)-1);puts("PASS actual fixed registration+child-only node forwarding reaches OFW bus probe");}
 return 0;
}
