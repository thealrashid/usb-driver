#include <linux/usb.h>
#include <linux/module.h>
#include <linux/init.h>

#include <linux/fs.h>        // alloc_chrdev_region, file_operations
#include <linux/cdev.h>      // cdev_init, cdev_add, cdev_del
#include <linux/device.h>    // class_create, device_create

#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/idr.h>
#include <linux/poll.h>

#define BUF_SIZE 4096
#define BUF_COUNT(dev) ((dev->head - dev->tail + BUF_SIZE) % BUF_SIZE)
#define BUF_SPACE(dev) (BUF_SIZE - BUF_COUNT(dev) - 1)

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

	/* URB */
	struct urb *bulk_in_urb;
	wait_queue_head_t read_queue;
	unsigned char circ_buf[BUF_SIZE];
	size_t head;
	size_t tail;
	
	/* Char device */
	struct cdev             cdev;
	dev_t                   devt;
	int                     minor;
	
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

static struct class *my_class;
static DEFINE_IDA(usb_minor_ida);
static int usb_major;

static int my_open(struct inode *inode, struct file *file) {
	struct usb_mydev *dev;
	
	dev = container_of(inode->i_cdev, struct usb_mydev, cdev);
	
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
	size_t count;
	
	dev = file->private_data;
	
	if (!dev) {
		return -ENODEV;
	}
	
	if (len == 0) {
	 return 0;
	}
	
	/* Lock */
	mutex_lock(&dev->io_mutex);

	if (file->f_flags & O_NONBLOCK) {
		if (BUF_COUNT(dev) == 0) {
			mutex_unlock(&dev->io_mutex);
			return -EAGAIN;
		}
	}
	
	/* Read from USB */
	retval = wait_event_interruptible(dev->read_queue,
																		BUF_COUNT(dev) > 0 || dev->disconnected);
		
	if (retval) {
		mutex_unlock(&dev->io_mutex);
		return retval;
	}

	if (dev->disconnected) {
		mutex_unlock(&dev->io_mutex);
		return -ENODEV;
	}

	/* Limit size*/
	count = BUF_COUNT(dev);
	if (len > count) {
		len = count;
	}

	/* Copy to user */
	for (size_t i = 0; i < len; i++) {
		if (copy_to_user(buf + i, &dev->circ_buf[dev->tail], 1)) {
			mutex_unlock(&dev->io_mutex);
			return -EFAULT;
		}
	}

	/*
	if (len > dev->bulk_in_size) {
		len = dev->bulk_in_size;
	}

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
	*/
	
	pr_info("Read %zu bytes from USB device\n", len);
	
	mutex_unlock(&dev->io_mutex);
	
	return len;
}

static ssize_t my_write(struct file *file, const char __user *buf, size_t len, loff_t *offset) {
	struct usb_mydev *dev;
	int retval;
	int wrote_cnt;
	
	dev = file->private_data;
	
	if (!dev) {
		return -ENODEV;
	}
	
	if (len == 0) {
	 return 0;
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

static __poll_t my_poll(struct file *file, poll_table *wait) {
	struct usb_mydev *dev = file->private_data;
	__poll_t mask = 0;

	if (!dev) {
		return EPOLLERR;
	}

	/* Register wait queue */
	poll_wait(file, &dev->read_queue, wait);

	mutex_lock(&dev->io_mutex);

	if (dev->disconnected) {
		mask |= EPOLLHUP | EPOLLERR;
	} else if (BUF_COUNT(dev) > 0) {
		mask |= EPOLLIN | EPOLLRDNORM;
	}

	mutex_unlock(&dev->io_mutex);

	return mask;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = my_open,
	.release = my_release,
	.read = my_read,
	.write = my_write,
	.poll = my_poll,
};

static void bulk_in_callback(struct urb *urb) {
	struct usb_mydev *dev = urb->context;
	int retval;

	if (!dev) {
		return;
	}

	unsigned char *data = dev->bulk_in_buffer;
	size_t len = urb->actual_length;

	mutex_lock(&dev->io_mutex);

	/* Copy into circular buffer */
	for (size_t i; i < len; i++) {
		if (BUF_SPACE(dev) == 0) {
			pr_warn("Buffer full, dropping data\n");
			break;
		}

		dev->circ_buf[dev->head] = data[i];
		dev->head = (dev->head + 1) % BUF_SIZE;
	}

	mutex_unlock(&dev->io_mutex);

	if (urb->status) {
		if (urb->status == -ESHUTDOWN || urb->status == -ENOENT) {
			pr_info("URB stopped (device removed)\n");
		} else {
			pr_err("URB error: %d\n", urb->status);
		}
		
		return;
	}

	wake_up_interruptible(&dev->read_queue);

	pr_info("URB received %zu bytes\n", len);

	if (!dev->disconnected) {
		retval = usb_submit_urb(urb, GFP_ATOMIC);
		if (retval) {
			pr_err("Failed to resumbit urb: %d\n", retval);
		}
	}
}

static int usb_probe(struct usb_interface *interface, const struct usb_device_id *id) {
  struct usb_host_interface *iface_desc;
  struct usb_endpoint_descriptor *endpoint;
  
  int retval;
  
  struct usb_mydev *dev;

	int minor;
	dev_t dev_number;
  
  dev = kzalloc(sizeof(struct usb_mydev), GFP_KERNEL);
  if (!dev) {
  	pr_err("Cannot allocate memory for my device\n");
  	return -ENOMEM;
  }
  
  dev->udev = usb_get_dev(interface_to_usbdev(interface));
  dev->interface = interface;
  
  mutex_init(&dev->io_mutex);

  dev->disconnected = false;

	init_waitqueue_head(&dev->read_queue);

	dev->head = 0;
	dev->tail = 0;
  
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
					goto error_buf;
				}
    		
    		pr_info("  --> Stored as BULK IN endpoint\n");
    	}
    	
    	if (usb_endpoint_is_bulk_out(endpoint)) {
    		dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
    		dev->bulk_out_size = le16_to_cpu(endpoint->wMaxPacketSize);
    		
    		dev->bulk_out_buffer = kzalloc(dev->bulk_out_size, GFP_KERNEL);
    		if (!dev->bulk_out_buffer) {
    			pr_err("Could not allocate bulk_out_buffer\n");
    			goto error_buf;
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
  	
  	retval = -ENODEV;
  	goto error_buf;
  }
  
  pr_info("Using endpoints: IN=0x%02X OUT=0x%02X\n", dev->bulk_in_endpointAddr, dev->bulk_out_endpointAddr);

	dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_in_urb) {
		pr_err("Failed to allocate URB\n");
		retval = -ENOMEM;
		goto error_buf;
	}

	usb_fill_bulk_urb(dev->bulk_in_urb,
										dev->udev,
										usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr),
										dev->bulk_in_buffer,
										dev->bulk_in_size,
										bulk_in_callback,
										dev);
	
	retval = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
	if (retval) {
		pr_err("Failed to submit URB: %d\n", retval);
		usb_free_urb(dev->bulk_in_urb);
		goto error_buf;
	}
  
	/* Allocate unique minor */
	minor = ida_alloc(&usb_minor_ida, GFP_KERNEL);
	if (minor < 0) {
		pr_err("Failed to allocate minor\n");
		return minor;
	}

	dev->minor = minor;

	/* Create dev_t */
	dev_number = MKDEV(usb_major, minor);

	dev->devt = dev_number;
	pr_info("Assigning device number %d:%d\n", MAJOR(dev_number), MINOR(dev_number));
  
  cdev_init(&dev->cdev, &fops);
  dev->cdev.owner = THIS_MODULE;
  
  retval = cdev_add(&dev->cdev, dev->devt, 1);
  if (retval) {
  	pr_err("Failed to add cdev\n");
  	goto error_buf;
  }
  
  /* Create device node*/
  if (IS_ERR(device_create(my_class, &interface->dev, dev->devt, NULL, "my_usb_device%d", dev->minor))) {
  	pr_err("Cannot create device\n");
  	retval = -EINVAL;
  	goto error_device;
  }
  
  usb_set_intfdata(interface, dev);
  
  pr_info("Char device created successfully\n");
  
  return 0;
  
  error_device:
  	cdev_del(&dev->cdev);
  
  error_buf:
		kfree(dev->bulk_out_buffer);
		kfree(dev->bulk_in_buffer);

		if(dev->bulk_in_urb) {
			usb_free_urb(dev->bulk_in_urb);
		}

		ida_free(&usb_minor_ida, dev->minor);
		usb_put_dev(dev->udev);
		kfree(dev);
		
		return retval;
}

static void usb_disconnect(struct usb_interface *interface) {
	struct usb_mydev *dev;
	
	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);
	
	if (!dev) {
		return;
	}
	
	dev->disconnected = true;
	
	device_destroy(my_class, dev->devt);
  cdev_del(&dev->cdev);
	ida_free(&usb_minor_ida, dev->minor);

	usb_kill_urb(dev->bulk_in_urb);
	usb_free_urb(dev->bulk_in_urb);
	wake_up_interruptible(&dev->read_queue);
  
  kfree(dev->bulk_in_buffer);
  kfree(dev->bulk_out_buffer);
  
  usb_put_dev(dev->udev);
  kfree(dev);
  
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
  int result;
	dev_t dev;
  
  pr_info("USB module initialized\n");
  
  /* Allocating major/minor numbers */
  if (alloc_chrdev_region(&dev, 0, 256, "my_usb_dev") < 0) {
  	pr_err("Cannot allocate chrdev\n");
  	return -1;
  }

	usb_major = MAJOR(dev);
  
  /* Create class*/
  my_class = class_create("my_usb_class");
  if (IS_ERR(my_class)) {
  	pr_err("Cannot create class\n");
  	unregister_chrdev_region(dev, 1);
  	return -1;
  }

	result = usb_register(&my_usb_driver);
  if (result < 0) {
    pr_err("USB registration failed for %s\n", my_usb_driver.name);
		class_destroy(my_class);
    unregister_chrdev_region(dev, 256);
    return -result;
  }
  
  return 0;
}

static void __exit usb_module_exit(void) {
	dev_t dev_number = MKDEV(usb_major, 0);

  usb_deregister(&my_usb_driver);
  pr_info("USB module exited\n");
  
	class_destroy(my_class);
	unregister_chrdev_region(dev_number, 256);
	pr_info("Char device destroyed\n");
}

module_init(usb_module_init);
module_exit(usb_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mamun");
MODULE_DESCRIPTION("Simple USB driver module");