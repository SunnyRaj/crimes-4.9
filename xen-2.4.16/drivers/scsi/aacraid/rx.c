/*
 *	Adaptec AAC series RAID controller driver
 *	(c) Copyright 2001 Red Hat Inc.	<alan@redhat.com>
 *
 * based on the old aacraid driver that is..
 * Adaptec aacraid device driver for Linux.
 *
 * Copyright (c) 2000 Adaptec, Inc. (aacraid@adaptec.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Module Name:
 *  rx.c
 *
 * Abstract: Hardware miniport for Drawbridge specific hardware functions.
 *
 */

#include <xeno/config.h>
#include <xeno/kernel.h>
#include <xeno/init.h>
#include <xeno/types.h>
#include <xeno/sched.h>
#include <xeno/pci.h>
/*  #include <xeno/spinlock.h> */
/*  #include <xeno/slab.h> */
#include <xeno/blk.h>
#include <xeno/delay.h>
/*  #include <xeno/completion.h> */
/*  #include <asm/semaphore.h> */
#include "scsi.h"
#include "hosts.h"

#include "aacraid.h"

static void aac_rx_intr(int irq, void *dev_id, struct pt_regs *regs)
{
    struct aac_dev *dev = dev_id;
    unsigned long bellbits;
    u8 intstat, mask;
    intstat = rx_readb(dev, MUnit.OISR);
    /*
     *	Read mask and invert because drawbridge is reversed.
     *	This allows us to only service interrupts that have 
     *	been enabled.
     */
    mask = ~(rx_readb(dev, MUnit.OIMR));
    /* Check to see if this is our interrupt.  If it isn't just return */
    
    if (intstat & mask) 
    {
	bellbits = rx_readl(dev, OutboundDoorbellReg);
	if (bellbits & DoorBellPrintfReady) {
	    aac_printf(dev, le32_to_cpu(rx_readl (dev, IndexRegs.Mailbox[5])));
	    rx_writel(dev, MUnit.ODR,DoorBellPrintfReady);
	    rx_writel(dev, InboundDoorbellReg,DoorBellPrintfDone);
	}
	else if (bellbits & DoorBellAdapterNormCmdReady) {
	    aac_command_normal(&dev->queues->queue[HostNormCmdQueue]);
	    rx_writel(dev, MUnit.ODR, DoorBellAdapterNormCmdReady);
	}
	else if (bellbits & DoorBellAdapterNormRespReady) {
	    aac_response_normal(&dev->queues->queue[HostNormRespQueue]);
	    rx_writel(dev, MUnit.ODR,DoorBellAdapterNormRespReady);
	}
	else if (bellbits & DoorBellAdapterNormCmdNotFull) {
	    rx_writel(dev, MUnit.ODR, DoorBellAdapterNormCmdNotFull);
	}
	else if (bellbits & DoorBellAdapterNormRespNotFull) {
	    rx_writel(dev, MUnit.ODR, DoorBellAdapterNormCmdNotFull);
	    rx_writel(dev, MUnit.ODR, DoorBellAdapterNormRespNotFull);
	}
    }
}

/**
 *	aac_rx_enable_interrupt	-	Enable event reporting
 *	@dev: Adapter
 *	@event: Event to enable
 *
 *	Enable event reporting from the i960 for a given event.
 */
 
static void aac_rx_enable_interrupt(struct aac_dev * dev, u32 event)
{
    switch (event) {
	
    case HostNormCmdQue:
	dev->irq_mask &= ~(OUTBOUNDDOORBELL_1);
	break;
	
    case HostNormRespQue:
	dev->irq_mask &= ~(OUTBOUNDDOORBELL_2);
	break;
	
    case AdapNormCmdNotFull:
	dev->irq_mask &= ~(OUTBOUNDDOORBELL_3);
	break;
	
    case AdapNormRespNotFull:
	dev->irq_mask &= ~(OUTBOUNDDOORBELL_4);
	break;
    }
}

/**
 *	aac_rx_disable_interrupt	-	Disable event reporting
 *	@dev: Adapter
 *	@event: Event to enable
 *
 *	Disable event reporting from the i960 for a given event.
 */

static void aac_rx_disable_interrupt(struct aac_dev *dev, u32 event)
{
    switch (event) {
	
    case HostNormCmdQue:
	dev->irq_mask |= (OUTBOUNDDOORBELL_1);
	break;
	
    case HostNormRespQue:
	dev->irq_mask |= (OUTBOUNDDOORBELL_2);
	break;
	
    case AdapNormCmdNotFull:
	dev->irq_mask |= (OUTBOUNDDOORBELL_3);
	break;
	
    case AdapNormRespNotFull:
	dev->irq_mask |= (OUTBOUNDDOORBELL_4);
	break;
    }
}

/**
 *	rx_sync_cmd	-	send a command and wait
 *	@dev: Adapter
 *	@command: Command to execute
 *	@p1: first parameter
 *	@ret: adapter status
 *
 *	This routine will send a synchronous comamnd to the adapter and wait 
 *	for its	completion.
 */

static int rx_sync_cmd(struct aac_dev *dev, u32 command, u32 p1, u32 *status)
{
    unsigned long start;
    int ok;
    /*
     *	Write the command into Mailbox 0
     */
    rx_writel(dev, InboundMailbox0, cpu_to_le32(command));
    /*
     *	Write the parameters into Mailboxes 1 - 4
     */
    rx_writel(dev, InboundMailbox1, cpu_to_le32(p1));
    rx_writel(dev, InboundMailbox2, 0);
    rx_writel(dev, InboundMailbox3, 0);
    rx_writel(dev, InboundMailbox4, 0);
    /*
     *	Clear the synch command doorbell to start on a clean slate.
     */
    rx_writel(dev, OutboundDoorbellReg, OUTBOUNDDOORBELL_0);
    /*
     *	Disable doorbell interrupts
     */
    rx_writeb(dev, MUnit.OIMR, rx_readb(dev, MUnit.OIMR) | 0x04);
    /*
     *	Force the completion of the mask register write before issuing
     *	the interrupt.
     */
    rx_readb (dev, MUnit.OIMR);
    /*
     *	Signal that there is a new synch command
     */
    rx_writel(dev, InboundDoorbellReg, INBOUNDDOORBELL_0);
    
    ok = 0;
    start = jiffies;
    
    /*
     *	Wait up to 30 seconds
     */
    while (time_before(jiffies, start+30*HZ)) 
    {
	/* Delay 5 microseconds to let Mon960 get info. */
	udelay(5);	
	/*
	 *	Mon960 will set doorbell0 bit when its completed the command.
	 */
	if (rx_readl(dev, OutboundDoorbellReg) & OUTBOUNDDOORBELL_0) {
	    /*
	     *	Clear the doorbell.
	     */
	    rx_writel(dev, OutboundDoorbellReg, OUTBOUNDDOORBELL_0);
	    ok = 1;
	    break;
	}
#if 0
	/*
	 *	Yield the processor in case we are slow 
	 */
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(1);
#else 
	/* XXX SMH: not in xen we don't */
	mdelay(50); 
#endif
	
    }
    if (ok != 1) {
	/*
	 *	Restore interrupt mask even though we timed out
	 */
	rx_writeb(dev, MUnit.OIMR, rx_readl(dev, MUnit.OIMR) & 0xfb);
	return -ETIMEDOUT;
    }
    /*
     *	Pull the synch status from Mailbox 0.
     */
    *status = le32_to_cpu(rx_readl(dev, IndexRegs.Mailbox[0]));
    /*
     *	Clear the synch command doorbell.
     */
    rx_writel(dev, OutboundDoorbellReg, OUTBOUNDDOORBELL_0);
    /*
     *	Restore interrupt mask
     */
    rx_writeb(dev, MUnit.OIMR, rx_readl(dev, MUnit.OIMR) & 0xfb);
    return 0;
    
}

/**
 *	aac_rx_interrupt_adapter	-	interrupt adapter
 *	@dev: Adapter
 *
 *	Send an interrupt to the i960 and breakpoint it.
 */

static void aac_rx_interrupt_adapter(struct aac_dev *dev)
{
    u32 ret;
    rx_sync_cmd(dev, BREAKPOINT_REQUEST, 0, &ret);
}

/**
 *	aac_rx_notify_adapter		-	send an event to the adapter
 *	@dev: Adapter
 *	@event: Event to send
 *
 *	Notify the i960 that something it probably cares about has
 *	happened.
 */

static void aac_rx_notify_adapter(struct aac_dev *dev, u32 event)
{
    switch (event) {
	
    case AdapNormCmdQue:
	rx_writel(dev, MUnit.IDR,INBOUNDDOORBELL_1);
	break;
    case HostNormRespNotFull:
	rx_writel(dev, MUnit.IDR,INBOUNDDOORBELL_4);
	break;
    case AdapNormRespQue:
	rx_writel(dev, MUnit.IDR,INBOUNDDOORBELL_2);
	break;
    case HostNormCmdNotFull:
	rx_writel(dev, MUnit.IDR,INBOUNDDOORBELL_3);
	break;
    case HostShutdown:
//		rx_sync_cmd(dev, HOST_CRASHING, 0, 0, 0, 0, &ret);
	break;
    case FastIo:
	rx_writel(dev, MUnit.IDR,INBOUNDDOORBELL_6);
	break;
    case AdapPrintfDone:
	rx_writel(dev, MUnit.IDR,INBOUNDDOORBELL_5);
	break;
    default:
	BUG();
	break;
    }
}

/**
 *	aac_rx_start_adapter		-	activate adapter
 *	@dev:	Adapter
 *
 *	Start up processing on an i960 based AAC adapter
 */

static void aac_rx_start_adapter(struct aac_dev *dev)
{
    u32 status;
    struct aac_init *init;
    
    init = dev->init;
    printk("aac_rx_start: dev is %p, init is %p\n", dev, init); 
    init->HostElapsedSeconds = cpu_to_le32(jiffies/HZ);
    /*
     *	Tell the adapter we are back and up and running so it will scan
     *	its command queues and enable our interrupts
     */
    dev->irq_mask = (DoorBellPrintfReady | OUTBOUNDDOORBELL_1 | 
		     OUTBOUNDDOORBELL_2 | OUTBOUNDDOORBELL_3 | 
		     OUTBOUNDDOORBELL_4);
    /*
     *	First clear out all interrupts.  Then enable the one's that we
     *	can handle.
     */
    rx_writeb(dev, MUnit.OIMR, 0xff);
    rx_writel(dev, MUnit.ODR, 0xffffffff);
//	rx_writeb(dev, MUnit.OIMR, ~(u8)OUTBOUND_DOORBELL_INTERRUPT_MASK);
    rx_writeb(dev, MUnit.OIMR, 0xfb);
    
    // We can only use a 32 bit address here
    rx_sync_cmd(dev, INIT_STRUCT_BASE_ADDRESS, 
		(u32)(ulong)dev->init_pa, &status);
}

/**
 *	aac_rx_init	-	initialize an i960 based AAC card
 *	@dev: device to configure
 *	@devnum: adapter number
 *
 *	Allocate and set up resources for the i960 based AAC variants. The 
 *	device_interface in the commregion will be allocated and linked 
 *	to the comm region.
 */

int aac_rx_init(struct aac_dev *dev, unsigned long num)
{
    unsigned long start;
    unsigned long status;
    int instance;
    const char * name;
    
    dev->devnum = num;
    instance = dev->id;
    name     = dev->name;
    
    dprintk((KERN_ERR "aac_rx_init called, num %ld, scsi host ptr = %p\n", 
	     num, (void *)(dev->scsi_host_ptr))); 
    
    dprintk((KERN_ERR "scsi_host_ptr->base is %p\n", 
	     (void *)dev->scsi_host_ptr->base)); 
    /*
     *	Map in the registers from the adapter.
     */
    if((dev->regs.rx = (struct rx_registers *)
	ioremap((unsigned long)dev->scsi_host_ptr->base, 8192))==NULL)
    {	
	printk(KERN_WARNING "aacraid: unable to map i960.\n" );
	return -1;
    }
    
//	dprintk((KERN_ERR "aac_rx_init: AAA\n")); 
    /*
     *	Check to see if the board failed any self tests.
     */
    if (rx_readl(dev, IndexRegs.Mailbox[7]) & SELF_TEST_FAILED) {
	printk(KERN_ERR "%s%d: adapter self-test failed.\n", 
	       dev->name, instance);
	return -1;
    }
    
    
//	dprintk((KERN_ERR "aac_rx_init: BBB\n")); 
    /*
     *	Check to see if the board panic'd while booting.
     */
    if (rx_readl(dev, IndexRegs.Mailbox[7]) & KERNEL_PANIC) {
	printk(KERN_ERR "%s%d: adapter kernel panic'd.\n", 
	       dev->name, instance);
	return -1;
    }
    start = jiffies;
    
//	dprintk((KERN_ERR "aac_rx_init: DDD\n")); 
    /*
     *	Wait for the adapter to be up and running. Wait up to 3 minutes
     */
    while (!(rx_readl(dev, IndexRegs.Mailbox[7]) & KERNEL_UP_AND_RUNNING)) 
    {
	if(time_after(jiffies, start+180*HZ))
	{
	    status = rx_readl(dev, IndexRegs.Mailbox[7]) >> 16;
	    printk(KERN_ERR "%s%d: adapter kernel failed to start,"
		   "init status = %ld.\n", dev->name, 
		   instance, status);
	    return -1;
	}
// dprintk((KERN_ERR "aac_rx_init: XXX\n")); 
	
#if 0 
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(1);
#else
	/* XXX SMH: no sleeping for us (we're the xen idle task) */
	mdelay(50); 
#endif
	
    }
    
//	dprintk((KERN_ERR "aac_rx_init: ZZZ!\n")); 
    if (request_irq(dev->scsi_host_ptr->irq, aac_rx_intr, 
		    SA_SHIRQ|SA_INTERRUPT, "aacraid", (void *)dev) < 0) 
    {
	printk(KERN_ERR "%s%d: Interrupt unavailable.\n", 
	       name, instance);
	return -1;
    }
    /*
     *	Fill in the function dispatch table.
     */
    dev->a_ops.adapter_interrupt = aac_rx_interrupt_adapter;
    dev->a_ops.adapter_enable_int = aac_rx_enable_interrupt;
    dev->a_ops.adapter_disable_int = aac_rx_disable_interrupt;
    dev->a_ops.adapter_notify = aac_rx_notify_adapter;
    dev->a_ops.adapter_sync_cmd = rx_sync_cmd;
    
    if (aac_init_adapter(dev) == NULL)
	return -1;
#if 0
    /*
     *	Start any kernel threads needed
     */
    dev->thread_pid = kernel_thread((int (*)(void *))aac_command_thread, 
				    dev, 0);
#else 
    /* XXX SMH: just put in a softirq handler instead... */
    open_softirq(SCSI_LOW_SOFTIRQ, aac_command_thread, dev); 
#endif

    /*
     *	Tell the adapter that all is configured, and it can start
     *	accepting requests
     */
    aac_rx_start_adapter(dev);
    return 0;
}
