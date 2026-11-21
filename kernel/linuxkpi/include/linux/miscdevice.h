/* LinuxKPI: see linux/lkpi_binder.h */
#pragma once
#include <linux/lkpi_binder.h>

#ifndef VHCI_MINOR
#define VHCI_MINOR 137
#endif
#ifndef UHID_MINOR
#define UHID_MINOR 239
#endif
#ifndef module_misc_device
#define module_misc_device(__misc) \
    static int __init __misc##_init(void) { return misc_register(&__misc); } \
    module_init(__misc##_init); \
    static void __exit __misc##_exit(void) { misc_deregister(&__misc); } \
    module_exit(__misc##_exit)
#endif
#define MODULE_ALIAS_MISCDEV(minor)
