# 🌐 EquinoxOS Network Stack Implementation Plan

This plan details the transition from the current experimental prototype to a professional-grade, layered network stack (TCP/IP model).

---

## 🛠 Phase 1: Link Layer & ARP (Local Discovery)
**Goal:** Remove hardcoded MAC addresses and manage local network discovery dynamically.

### 1.1 Network Interface Abstraction
- Create `struct net_interface` to store:
  - Hardware device pointers (send/receive).
  - MAC address.
  - Assigned IP, Subnet Mask, and Default Gateway.
- Support multiple interfaces (e.g., RTL8139 + Loopback).

### 1.2 Dynamic ARP Cache
- Implement an **ARP Table** to map IP addresses to MAC addresses.
- **Logic:** Before sending an IP packet:
  1. Check ARP cache for destination IP.
  2. If found, wrap in Ethernet frame and send.
  3. If not found, queue the IP packet and send an **ARP Request**.
  4. Upon **ARP Reply**, update cache and send all queued packets for that IP.

---

## 📡 Phase 2: Network Layer (Routing & ICMP)
**Goal:** Proper packet routing and basic diagnostics.

### 2.1 Routing Logic
- Implement basic subnet logic:
  - `if (dest_ip & subnet_mask == local_ip & subnet_mask)` -> Send directly (ARP).
  - `else` -> Send to **Default Gateway** MAC.

### 2.2 ICMP (Ping)
- Create `src/net/icmp.c`.
- **ICMP Echo Reply:** Automatically respond to pings so the OS is "visible" to the host.
- **Ping Command:** Add a `ping` shell command to test connectivity.

---

## 🏗 Phase 3: Transport Layer (State Management)
**Goal:** Reliable data streams and port multiplexing.

### 3.1 UDP Port Registry
- Create a registry for bound UDP ports so incoming packets can be delivered to the correct application/buffer.

### 3.2 TCP State Machine
- Move beyond the current "Happy Path" implementation.
- Implement a **TCP Control Block (TCB)** for each connection.
- Support states: `SYN_SENT`, `ESTABLISHED`, `FIN_WAIT`, `TIME_WAIT`.
- Implement **Packet Retransmission** (if no ACK is received within timeout).

---

## 🔌 Phase 4: Socket API (User-Space Interface)
**Goal:** Allow standard applications to use the network.

### 4.1 System Calls
- Implement standard POSIX-like syscalls:
  - `sys_socket()`
  - `sys_bind()`
  - `sys_connect()`
  - `sys_send()` / `sys_recv()`
- Integrated with the Task Manager to block tasks waiting for data.

### 4.2 VFS Sockets
- (Optional) Implement `sockfs` so sockets can be treated as file descriptors (allowing `read()` and `write()` calls).

---

## 🚀 Phase 5: Auto-Configuration (DHCP & DNS)
**Goal:** No more hardcoded IPs.

### 5.1 DHCP Client
- Implement the DHCP protocol (UDP 67/68).
- Automatically discover network settings on boot (IP, Mask, Gateway, DNS).

### 5.2 DNS Resolver
- Implement a basic DNS client.
- Create a kernel-level cache for resolved domains to speed up repeated requests.
