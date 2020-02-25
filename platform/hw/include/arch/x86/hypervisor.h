unsigned hypervisor_detect(void);
uint32_t hypervisor_base(unsigned);

#define HYPERVISOR_XEN 0
#define HYPERVISOR_VMWARE 1
#define HYPERVISOR_HYPERV 2
#define HYPERVISOR_KVM 3
#define HYPERVISOR_NONE 4
#define HYPERVISOR_NUM HYPERVISOR_NONE
