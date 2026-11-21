/* LinuxKPI: /proc entries of Linux code are not created. */
#pragma once
#include <linux/lkpi_all.h>

struct proc_dir_entry;
struct seq_operations;
#define proc_create_seq_data(name, mode, parent, ops, data) ((struct proc_dir_entry *)NULL)
#define proc_create_seq(name, mode, parent, ops) ((struct proc_dir_entry *)NULL)
#define proc_create_data(name, mode, parent, fops, data) ((struct proc_dir_entry *)NULL)
#define proc_mkdir(name, parent) ((struct proc_dir_entry *)NULL)
#define remove_proc_entry(name, parent) do { } while (0)
#define pde_data(inode) ((void *)NULL)
