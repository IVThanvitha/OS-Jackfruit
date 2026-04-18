
# 🚀 Multi-Container Runtime (OS Jackfruit)

A lightweight **Linux container runtime written in C**, featuring:

* A **user-space supervisor (engine)**
* A **kernel-space memory monitor module**
* Support for running **multiple isolated containers**
* Resource monitoring and control mechanisms

This project demonstrates core **Operating Systems concepts** including:

* Process isolation (namespaces)
* Resource control
* Kernel-user communication (ioctl)
* Container lifecycle management

---

## 📌 Features

* 🧠 Custom container runtime (no Docker used)
* ⚙️ Supervisor process to manage containers
* 🧩 Kernel module for memory monitoring
* 🔄 Multiple container support
* 📊 CPU & memory stress testing utilities
* 🔐 Namespace-based isolation

---

## 🛠️ Tech Stack

* **Language:** C
* **OS:** Linux (Ubuntu 22.04 / 24.04)
* **Concepts Used:**

  * Linux Namespaces
  * cgroups (optional extension)
  * Kernel Modules
  * System Calls & ioctl
  * Process Scheduling

---

## ⚙️ Prerequisites

Make sure you have:

* Ubuntu **22.04 or 24.04** (VM recommended)
* **Secure Boot disabled**
* VirtualBox / VMware (WSL not supported ❌)

Install required packages:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

---

## 🧪 Environment Setup

Run the environment check:

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

Fix any warnings/errors before proceeding.

---

## 📂 Project Structure

```
OS-Jackfruit/
│
├── boilerplate/
│   ├── engine.c              # Main runtime + supervisor
│   ├── monitor.c             # Kernel module
│   ├── monitor_ioctl.h       # Shared interface (ioctl)
│   ├── Makefile              # Build system
│   ├── cpu_hog.c             # CPU stress test
│   ├── memory_hog.c          # Memory stress test
│   ├── io_pulse.c            # I/O simulation
│   └── environment-check.sh  # Setup validator
│
├── project-guide.md          # Detailed project explanation
└── README.md                 # Documentation
```

---

## 🧱 Root Filesystem Setup

Download and prepare a minimal root filesystem:

```bash
mkdir rootfs-base

wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz

tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

Create container instances:

```bash
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

⚠️ Do NOT commit these directories to Git.

---

## 🔨 Build Instructions

Navigate to boilerplate:

```bash
cd boilerplate
make
```

This will build:

* `engine` → user-space runtime
* `monitor.ko` → kernel module
* test programs

---

## 🧩 Load Kernel Module

```bash
sudo insmod monitor.ko
```

Verify:

```bash
lsmod | grep monitor
```

---

## ▶️ Running the Runtime

Start the supervisor:

```bash
sudo ./engine supervisor ../rootfs-alpha
```

If multiple containers are supported:

```bash
sudo ./engine supervisor ../rootfs-alpha ../rootfs-beta
```

---

## 🔍 How It Works

### 1. Supervisor (`engine.c`)

* Creates containers using **fork + namespaces**
* Manages lifecycle (start, stop, monitor)
* Communicates with kernel module

### 2. Kernel Module (`monitor.c`)

* Tracks memory usage
* Provides interface via **ioctl**
* Helps enforce resource awareness

### 3. Isolation Mechanism

* PID namespace → separate process trees
* Mount namespace → isolated filesystem
* (Optional) Network namespace

---

## 📡 Kernel–User Communication

* Implemented using **ioctl interface**
* Defined in:

```c
monitor_ioctl.h
```

Used for:

* Sending process info
* Querying memory stats

---

## ⚠️ Common Issues & Fixes

### ❌ `/dev/container_monitor` not found

➡️ Ensure kernel module is loaded:

```bash
sudo insmod monitor.ko
```

---

### ❌ Permission errors

➡️ Always run with `sudo`

---

### ❌ Make shows “Nothing to be done”

➡️ Clean and rebuild:

```bash
make clean
make
```

---

### ❌ VM is slow

➡️ Increase:

* RAM (≥ 4GB)
* CPU cores (≥ 2)

---

## 📈 Possible Improvements

* Add **cgroups** for strict resource limits
* Implement container networking
* Add CLI commands (`start`, `stop`, `status`)
* Logging & monitoring dashboard
* Docker-like UX

---

## 📚 Learning Outcomes

This project helps you understand:

* Container internals (without Docker)
* Kernel module development
* OS-level resource management
* Process isolation techniques

---

## 👨‍💻 Author

**I.V. THANVITHA**

---

## 📜 License

This project is for academic and educational purposes.

---
