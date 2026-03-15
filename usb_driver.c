#include <linux/usb.h>
#include <linux/module.h>

static struct usb_device *device;

static struct usb_device_id usb_table[] = {
  {USB_DEVICE(0x781, 0x5567)},
  {}
}

static int usb_probe(struct usb_interface *interface, const struct usb_device_id) {
  struct usb_host_interface *iface_desc;
  struct usb_endpoint_descriptor *endpoint;
  
  iface_desc = interface->cur_altsetting;
  
  pr_info("Pen drive i/f %d now probed: {%04X:%04X}\n",
  iface_desc->desc.bInterfaceNumber, id->idVendor, id->idProduct);
  
  pr_info("Number of endpoints: %02X\n", iface_desc->desc.bNumEndpoints);
  
  pr_info("Interface class: %02X\n", iface_desc->desc.bInterfaceClass);
  
  for (int i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
    endpoint = &iface_desc->endpoint[i].desc;
    
    pr_info("ED[%d]->bEndpointAddress: 0x%02X\n", i, endpoint->bEndpointAddress);
    
    pr_info("ED[%d]->bmAttributes: 0x%02X\n", i, endpoint->bmAttributes);
    
    pr_info("ED[%d]->wMaxPacketSize: 0x%02X\n", i, endpoint->wMaxPacketSize);
  }
  
  device = interface_to_usbdev(interface);
  
  return 0;
}

static void usb_disconnect(struct usb_interface *interface) {
  usb_put_dev(device);
  pr_info("Pen drive removed\n");
}

static struct usb_driver my_usb_driver = {
  .name = "usb_driver",
  .probe = usb_probe,
  .disconnect = usb_disconnect,
  .id_table = usb_table,
  .supports_autosuspend = 1,
};

static int __init usb_module_init(void) {
  int result = usb_register(&my_usb_driver);
  
  if (result < 0) {
    pr_err("USB registration failed for %s\n", my_usb_driver.name);
    return -1;
  }
  
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
