#include <linux/usb.h>
#include <linux/module.h>
#include <linux/init.h>

#include <linux/fs.h>        // alloc_chrdev_region, file_operations
#include <linux/cdev.h>      // cdev_init, cdev_add, cdev_del
#include <linux/device.h>    // class_create, device_create

struct usb_mydev {
	struct usb_device    *udev;
	struct usb_interface *interface;
	
	/* Bulk IN endpoint */
	unsigned char           bulk_in_endpointAddr;
	size_t                  bulk_in_size;
	unsigned char           *bulk_in_buffer;

	/* Bulk OUT endpoint */
	unsigned char           bulk_out_endpointAddr;
	size_t                  bulk_out_size;
	unsigned char           *bulk_out_buffer;
	
	/* syncronization */
	struct mutex         io_mutex;
	
	/* state */
	bool                 disconnected;
};

static struct usb_device_id usb_table[] = {
  {USB_DEVICE(0x346d, 0x5678)},
  {}
};

MODULE_DEVICE_TABLE(usb, usb_table); // For automatice driver loading

static struct usb_mydev *global_dev;

static dev_t dev_number;
static struct cdev my_cdev;
static struct class *my_class;

static int my_open(struct inode *inode, struct file *file) {
	struct usb_mydev *dev;
	
	dev = global_dev;
	
	if (!dev) {
		pr_err("Device not present\n");
		return -ENODEV;
	}
	
	if (dev->disconnected) {
		pr_err("Device disconnected\n");
		return -ENODEV;
	}
	
	file->private_data = dev;
	
	pr_info("Device opened successfully\n");
	return 0;
}

static int my_release(struct inode *inode, struct file *file) {
	file->private_data = NULL;
	
	pr_info("Device closed\n");
	return 0;
}

static ssize_t my_read(struct file *file, char __user *buf, size_t len, loff_t *offset) {
	struct usb_mydev *dev;
	int retval;
	int read_cnt;
	
	dev = file->private_data;
	
	if (!dev) {
		return -ENODEV;
	}
	
	/* Lock */
	mutex_lock(&dev->io_mutex);
	
	if (dev->disconnected) {
		mutex_unlock(&dev->io_mutex);
		return -ENODEV;
	}
	
	/* Limit size */
	if (len > dev->bulk_in_size) {
		len = dev->bulk_in_size;
	}
	
	/* Read from USB */
	retval = usb_bulk_msg(dev->udev,
					 usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr),
					 dev->bulk_in_buffer,
					 len,
					 &read_cnt,
					 2000);
	if (retval) {
		pr_err("Bulk read failed: %d (%pe)\n", retval, ERR_PTR(retval));
		mutex_unlock(&dev->io_mutex);
		return retval;
	}
	
	/* Copy to user */
	if (copy_to_user(buf, dev->bulk_in_buffer, read_cnt)) {
		mutex_unlock(&dev->io_mutex);
		return -EFAULT;
	}
	
	pr_info("Read %d bytes from USB device\n", read_cnt);
	
	mutex_unlock(&dev->io_mutex);
	
	return read_cnt;
}

static ssize_t my_write(struct file *file, const char __user *buf, size_t len, loff_t *offset) {
	struct usb_mydev *dev;
	int retval;
	int wrote_cnt;
	
	dev = file->private_data;
	
	if (!dev) {
		return -ENODEV;
	}
	
	/* Lock */
	mutex_lock(&dev->io_mutex);
	
	if (dev->disconnected) {
		mutex_unlock(&dev->io_mutex);
		return -ENODEV;
	}
	
	/* Limit size */
	if (len > dev->bulk_out_size) {
		len = dev->bulk_out_size;
	}
	
	/* Copy from user */
	if (copy_from_user(dev->bulk_out_buffer, buf, len)) {
		mutex_unlock(&dev->io_mutex);
		return -EFAULT;
	}
	
	/* Send data via USB */
	retval = usb_bulk_msg(dev->udev,
					 usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
					 dev->bulk_out_buffer,
					 len,
					 &wrote_cnt,
					 1000);
	
	if (retval) {
		pr_err("Bulk write failed: %d (%pe)\n", retval, ERR_PTR(retval));
		mutex_unlock(&dev->io_mutex);
		return retval;
	}
	
	pr_info("Wrote %d bytes to USB device\n", wrote_cnt);
	
	mutex_unlock(&dev->io_mutex);
	
	return wrote_cnt;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = my_open,
	.release = my_release,
	.read = my_read,
	.write = my_write,
};

static int usb_probe(struct usb_interface *interface, const struct usb_device_id *id) {
  struct usb_host_interface *iface_desc;
  struct usb_endpoint_descriptor *endpoint;
  
  int retval;
  
  struct usb_mydev *dev;
  
  dev = kzalloc(sizeof(struct usb_mydev), GFP_KERNEL);
  if (!dev) {
  	pr_err("Cannot allocate memory for my device\n");
  	return -ENOMEM;
  }
  
  dev->udev = usb_get_dev(interface_to_usbdev(interface));
  dev->interface = interface;
  
  mutex_init(&dev->io_mutex);
  dev->disconnected = false;
  
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
    		
    		dev->bulk_in_buffer = kzalloc(dev->bulk_in_size, GFP_KERNEL);
				if (!dev->bulk_in_buffer) {
					pr_err("Could not allocate bulk_in_buffer\n");
					goto error;
				}
    		
    		pr_info("  --> Stored as BULK IN endpoint\n");
    	}
    	
    	if (usb_endpoint_is_bulk_out(endpoint)) {
    		dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
    		dev->bulk_out_size = le16_to_cpu(endpoint->wMaxPacketSize);
    		
    		dev->bulk_out_buffer = kzalloc(dev->bulk_out_size, GFP_KERNEL);
    		if (!dev->bulk_out_buffer) {
    			pr_err("Could not allocate bulk_out_buffer\n");
    			goto error;
    		}
    		
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
    pr_info("  MaxPacketSize: %d\n", le16_to_cpu(endpoint->wMaxPacketSize));
  }
  
  if (!(dev->bulk_in_endpointAddr && dev->bulk_out_endpointAddr)) {
  	pr_err("Could not find both bulk-in and bulk-out endpoints\n");
  	usb_put_dev(dev->udev);
  	kfree(dev);
  	return -ENODEV;
  }
  
  pr_info("Using endpoints: IN=0x%02X OUT=0x%02X\n", dev->bulk_in_endpointAddr, dev->bulk_out_endpointAddr);
  
  global_dev = dev;
  
  return 0;
  
  error:
  usb_set_intfdata(interface, NULL);
  if (dev) {
		kfree(dev->bulk_in_buffer);
		usb_put_dev(dev->udev);
		kfree(dev);
	}
	return retval;
}

static void usb_disconnect(struct usb_interface *interface) {
	struct usb_mydev *dev;
	
	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);
	
	if (dev) {
		dev->disconnected = true;
		
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
  
  /* Allocating major/minor numbers */
  if (alloc_chrdev_region(&dev_number, 0, 1, "my_usb_dev") < 0) {
  	pr_err("Cannot allocate chrdev\n");
  	return -1;
  }
  
  /* Init cdev */
  cdev_init(&my_cdev, &fops);
  
  /* Add cdev*/
  if (cdev_add(&my_cdev, dev_number, 1) < 0) {
  	pr_err("Cannot add cdev\n");
  	goto unregister_chrdev;
  }
  
  /* Create class*/
  my_class = class_create("my_usb_class");
  if (IS_ERR(my_class)) {
  	pr_err("Cannot create class\n");
  	goto del_cdev;
  }
  
  /* Create device node*/
  if (device_create(my_class, NULL, dev_number, NULL, "my_usb_device") == NULL) {
  	pr_err("Cannot create device\n");
  	goto destroy_class;
  }
  
  pr_info("Char device created successfully\n");
  
  return 0;
  
  destroy_class:
  	class_destroy(my_class);
  del_cdev:
  	cdev_del(&my_cdev);
  unregister_chrdev:
  	unregister_chrdev_region(dev_number, 1);
  	return -1;
}

static void __exit usb_module_exit(void) {
  usb_deregister(&my_usb_driver);
  pr_info("USB module exited\n");
  
  device_destroy(my_class, dev_number);
	class_destroy(my_class);
	cdev_del(&my_cdev);
	unregister_chrdev_region(dev_number, 1);
	pr_info("Char device destroyed\n");
}

module_init(usb_module_init);
module_exit(usb_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mamun");
MODULE_DESCRIPTION("Simple USB driver module");
