#include <linux/usb.h>
#include <linux/module.h>

static int __init usb_module_init(void) {
  pr_info("USB module initialized\n");
  
  return 0;
}

static void __exit usb_module_exit(void) {
  pr_info("USB module exited\n");
}

module_init(usb_module_init);
module_exit(usb_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mamun");
MODULE_DESCRIPTION("Simple USB driver module");
