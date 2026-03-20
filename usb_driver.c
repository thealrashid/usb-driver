#include <linux/usb.h>
#include <linux/module.h>

struct usb_mydev {
	struct usb_device *udev;
	struct usb_interface *interface;
	
	__u8 bulk_in_endpointAddr;
	__u8 bulk_out_endpointAddr;
	
	size_t bulk_in_size;
	unsigned char *bulk_in_buffer;
};

static struct usb_device_id usb_table[] = {
  {USB_DEVICE(0x346d, 0x5678)},
  {}
};

MODULE_DEVICE_TABLE(usb, usb_table); // For automatice driver loading

static int usb_probe(struct usb_interface *interface, const struct usb_device_id *id) {
  struct usb_host_interface *iface_desc;
  struct usb_endpoint_descriptor *endpoint;
  
  int retval_in;
  int actual_length;
  int retval_out;
  
  struct usb_mydev *dev;
  
  dev = kmalloc(sizeof(struct usb_mydev), GFP_KERNEL);
  if (!dev) {
  	pr_err("Cannot allocate memory for my device\n");
  	return -ENOMEM;
  }
  
  dev->udev = usb_get_dev(interface_to_usbdev(interface));
  dev->interface = interface;
  
  usb_set_intfdata(interface, dev);
  
  iface_desc = interface->cur_altsetting;
  
  pr_info("Device plugged: {%04X:%04X}\n",
  dev->udev->descriptor.idVendor, dev->udev->descriptor.idProduct);
  
  pr_info("Number of endpoints: %02X\n", iface_desc->desc.bNumEndpoints);
  
  pr_info("Interface class: %02X\n", iface_desc->desc.bInterfaceClass);
  
  dev->bulk_in_endpointAddr = 0;
	dev->bulk_out_endpointAddr = 0;
  
  for (int i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
    endpoint = &iface_desc->endpoint[i].desc;
    
    pr_info("Endpoint %d:\n", i);
    
    // Address
    pr_info("  Address: 0x%02X\n", endpoint->bEndpointAddress);
    
    // Direction
    if (usb_endpoint_dir_in(endpoint))
    	pr_info("  Direction: IN\n");
    else
			pr_info("  Direction: OUT\n");
    
    // Transfer type
    if (usb_endpoint_xfer_bulk(endpoint)) {
    	pr_info("  Type: Bulk\n");
    	
    	if (usb_endpoint_is_bulk_in(endpoint)) {
    		dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
    		dev->bulk_in_size = le16_to_cpu(endpoint->wMaxPacketSize);
    		
    		pr_info("  --> Stored as BULK IN endpoint\n");
    	}
    	
    	if (usb_endpoint_is_bulk_out(endpoint)) {
    		dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
    		
    		pr_info("  --> Stored as BULK OUT endpoint\n");
    	}
    }
    else if (usb_endpoint_xfer_int(endpoint))
			pr_info("  Type: Interrupt\n");
    else if (usb_endpoint_xfer_control(endpoint))
			pr_info("  Type: Control\n");
    else if (usb_endpoint_xfer_isoc(endpoint))
			pr_info("  Type: Isochronous\n");
    
    // Packet size    
    pr_info("  MaxPacketSize: %d\n",
			le16_to_cpu(endpoint->wMaxPacketSize));
  }
  
  if (!(dev->bulk_in_endpointAddr && dev->bulk_out_endpointAddr)) {
  	pr_err("Could not find both bulk-in and bulk-out endpoints\n");
  	kfree(dev);
  	return -ENODEV;
  }
  
  dev->bulk_in_buffer = kzalloc(dev->bulk_in_size, GFP_KERNEL);
  if (!dev->bulk_in_buffer) {
  	pr_err("Could not allocate bulk_in_buffer\n");
  	
  	usb_put_dev(dev->udev);
  	kfree(dev);
  	return -ENOMEM;
  }
  
  retval_in = usb_bulk_msg(dev->udev,
  					usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr),
  					dev->bulk_in_buffer,
  					dev->bulk_in_size,
  					&actual_length,
  					1000); // timeout in ms
  
  if (retval_in) {
  	pr_err("Bulk read failed with error %d\n", retval_in);
  } else {
  	pr_info("Bulk read successful, received %d bytes\n", actual_length);
  	
  	// Print received data (hex)
  	for (int i = 0; i < actual_length; i++) {
  		pr_info("0x%02X ", dev->bulk_in_buffer[i]);
  	}
  	pr_info("\n");
  }
  
  char *data;
	int data_len = 10;
	
	data = kmalloc(data_len, GFP_KERNEL);
	if (!data) {
		pr_err("Failed to allocate write buffer\n");
		return -ENOMEM;
	}

	memcpy(data, "Hello USB", data_len);
  
  retval_out = usb_bulk_msg(dev->udev,
  							usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
  							data,
  							data_len,
  							NULL,
  							1000);
  
  if (retval_out) {
		pr_err("Bulk write failed with error %d\n", retval_out);
	} else {
		pr_info("Bulk write successful\n");
	}
  
  return 0;
}

static void usb_disconnect(struct usb_interface *interface) {
	struct usb_mydev *dev;
	
	dev = usb_get_intfdata(interface);
	
	usb_set_intfdata(interface, NULL);
	
	if (dev) {
		kfree(dev->bulk_in_buffer);
  	usb_put_dev(dev->udev);
  	kfree(dev);
  }
  
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
  usb_deregister(&my_usb_driver);
  pr_info("USB module exited\n");
}

module_init(usb_module_init);
module_exit(usb_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mamun");
MODULE_DESCRIPTION("Simple USB driver module");
