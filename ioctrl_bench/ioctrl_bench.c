#include <linux/module.h>	/* Needed by all modules */
#include <linux/kernel.h>	/* Needed for KERN_INFO */
#include <linux/moduleparam.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/slab.h>
#include "ioctrl_bench.h"

#ifndef CONFIG_X86_MODULE_RERANDOMIZE
  #define SPECIAL_VAR(x) x
  #define SPECIAL_CONST_VAR(x) x
  #define SPECIAL_FUNCTION_PROTO(ret, name, args...) ret name (args)
  #define SPECIAL_FUNCTION(ret, name, args...) ret name (args)
#endif

static struct dentry *dir;

SPECIAL_FUNCTION(long, unlocked_ioctl, struct file *filp, unsigned int cmd, unsigned long argp){
    void __user *arg_user;
	int arg;

    arg_user = (void __user *)argp;
    switch (cmd) {
        case LKMC_IOCTL_INC:
            if (copy_from_user(&arg, arg_user, sizeof(arg)))  return -EFAULT;
            arg++;
            if (copy_to_user(arg_user, &arg, sizeof(arg)))  return -EFAULT;
        break;
        case LKMC_IOCTL_DEC:
            if (copy_from_user(&arg, arg_user, sizeof(arg)))  return -EFAULT;
            arg--;
            if (copy_to_user(arg_user, &arg, sizeof(arg)))  return -EFAULT;
        break;
        default:
            return -EINVAL;
        break;
    }
    return 0;
}


SPECIAL_VAR(struct file_operations fops) = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = unlocked_ioctl
};

SPECIAL_CONST_VAR(const char DIR_NAME[]) = "ioctl_bench";

int init_module(void){
    printk("ioctrl_bench: init_module\n");

    /* Register IOCTRL */
    dir = debugfs_create_dir(DIR_NAME, 0);
    debugfs_create_file("f", 0, dir, NULL, &fops);

    return 0;
}

void cleanup_module(void){
    printk("ioctrl_bench: cleanup_module\n");

    debugfs_remove_recursive(dir);
}

#ifdef CONFIG_X86_MODULE_RERANDOMIZE
  MODULE_INFO(randomizable, "Y");
#endif
MODULE_LICENSE("GPL v2");
