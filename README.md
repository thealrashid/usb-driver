# USB Character Driver (Linux Kernel)

## 📌 Overview

This project implements a Linux USB device driver with a character device interface, supporting asynchronous data transfer using URBs.

The driver demonstrates core kernel concepts such as USB subsystem interaction, wait queues, circular buffering, and poll/select mechanisms.

---

## 🧠 Key Features

* USB driver registration (`usb_driver`)
* Device detection using `probe()` / `disconnect()`
* Endpoint parsing (Bulk IN/OUT)
* Character device interface (`/dev/my_usb_deviceX`)
* Per-device `cdev` structure
* Multi-device support using dynamic minor allocation
* Asynchronous USB transfers using URB
* Continuous data streaming via URB resubmission
* Circular buffer for safe producer-consumer handling
* Blocking and non-blocking read support
* `poll()` / `select()` support
* Proper cleanup and error handling

---

## 🏗️ Architecture

```
User Space
   │
   ▼
read() / poll()
   │
   ▼
Wait Queue
   │
   ▼
Circular Buffer (head/tail)
   │
   ▼
URB Callback (Producer)
   │
   ▼
USB Device
```

---

## 🔄 Data Flow

1. URB is submitted to USB core
2. Device sends data (if available)
3. URB callback receives data
4. Data stored in circular buffer
5. Waiting user processes are woken up
6. `read()` fetches data from buffer

---

## ⚙️ Build & Run

### Build

```bash
make
```

### Insert Module

```bash
sudo insmod usb_driver.ko
```

### Check Logs

```bash
dmesg -w
```

### Device Node

```bash
ls /dev/my_usb_device*
```

---

## 🧪 Testing

### Blocking Read

```bash
cat /dev/my_usb_device0
```

### Non-blocking Read

```bash
dd if=/dev/my_usb_device0 of=/dev/null iflag=nonblock
```

### Poll Test

Use a simple poll-based C program to check readiness.

---

## ⚠️ Limitations

* Tested with USB mass storage device (Class 08)
* These devices require **SCSI protocol over USB**
* Raw data transfer is not demonstrated without implementing SCSI commands

---

## 🧠 Concepts Covered

* USB subsystem in Linux
* URB (USB Request Block)
* Asynchronous vs synchronous I/O
* Wait queues
* Circular buffer (ring buffer)
* Blocking vs non-blocking I/O
* poll/select mechanism
* Kernel synchronization (mutex)

---

## 🧹 Cleanup

```bash
sudo rmmod usb_driver
```

---

## 🚀 Future Work (Optional)

* Async write using URB
* Multiple URB queue for performance
* ioctl support
* Advanced buffering

---

## 👨‍💻 Author

Mamun
