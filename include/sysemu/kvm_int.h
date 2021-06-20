/*
 * Internal definitions for a target's KVM support
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_KVM_INT_H
#define QEMU_KVM_INT_H

#include "sysemu/sysemu.h"
#include "sysemu/accel.h"
#include "sysemu/kvm.h"

/*
 * QEMU对guest物理内存分段的的描述
 *
 * KVMSlot就是建立了GPA到HVA的映射关系
 *
 * 在kvm_memory_listener_register中分配
 *
 * 在kvm_set_user_memory_region中转成kvm_userspace_memory_region对象
 *
 *
 */
typedef struct KVMSlot
{
    /* 虚机内存区间起始地址（GPA） */
    hwaddr start_addr;
	/* 虚机内存区间长度 */
    ram_addr_t memory_size;
	/* 虚机内存区间对应的主机虚拟地址起始内存的指针，通过该指针可以查看内存页内容 */	
    void *ram;
	/* 在虚机所拥有的内存slot数组的索引 */
    int slot;
    int flags;
} KVMSlot;

typedef struct KVMMemoryListener {
    MemoryListener listener;
	/*
	 * 一个虚拟机中所有的KVMSlot
	 *
	 * kvm_state->nr_slots表示slots数组的数量
	 */
    KVMSlot *slots;
    int as_id;
} KVMMemoryListener;

#define TYPE_KVM_ACCEL ACCEL_CLASS_NAME("kvm")

#define KVM_STATE(obj) \
    OBJECT_CHECK(KVMState, (obj), TYPE_KVM_ACCEL)

void kvm_memory_listener_register(KVMState *s, KVMMemoryListener *kml,
                                  AddressSpace *as, int as_id);

#endif
