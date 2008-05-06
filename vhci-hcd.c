/*
 * vhci_hcd.c -- VHCI USB host controller driver.
 *
 * Copyright (C) by Michael Singer, 2007-2008
 * <singer@conemis.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define DEBUG

// für Kernel Version < 2.6.24
//#define OLD_GIVEBACK_MECH

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/usb.h>
#include <linux/fs.h>
#ifdef KBUILD_EXTMOD
#	include "vhci-hcd.h"
#else
#	include <linux/vhci-hcd.h>
#endif

#include <asm/atomic.h>
#include <asm/bitops.h>
//#include <asm/unaligned.h>
#include <asm/uaccess.h>

#ifdef KBUILD_EXTMOD
#	include INCLUDE_CORE_HCD
#else
#	include "../core/hcd.h"
#endif

#define DRIVER_NAME "vhci_hcd"
#define DRIVER_DESC "USB Virtual Host Controller Interface"
#define DRIVER_VERSION "ALPHA8 (26 April 2008)"

#ifdef vhci_printk
#	undef vhci_printk
#endif
#define vhci_printk(level, fmt, args...) \
	printk(level DRIVER_NAME ": " fmt, ## args)
#ifdef vhci_dbg
#	undef vhci_dbg
#endif
#ifdef DEBUG
#	warning DEBUG is defined
#	define vhci_dbg(fmt, args...) \
		if(debug_output) vhci_printk(KERN_DEBUG, fmt, ## args)
#else
#	define vhci_dbg(fmt, args...) do {} while(0)
#endif
#ifdef trace_function
#	undef trace_function
#endif
#ifdef DEBUG
#	define trace_function(dev) \
		if(debug_output) dev_dbg((dev), "%s%s\n", \
			in_interrupt() ? "IN_INTERRUPT: " : "", __FUNCTION__)
#else
#	define trace_function(dev) do {} while(0)
#endif

static const char driver_name[] = DRIVER_NAME;
static const char driver_desc[] = DRIVER_DESC;
#ifdef DEBUG
static int debug_output = 0;
#endif

MODULE_DESCRIPTION(DRIVER_DESC " driver");
MODULE_AUTHOR("Michael Singer <singer@conemis.com>");
MODULE_LICENSE("GPL");

struct vhci_urb_priv
{
	struct urb *urb;
	struct list_head urbp_list;
	atomic_t status;
};

enum vhci_rh_state
{
	VHCI_RH_RESET     = 0,
	VHCI_RH_SUSPENDED = 1,
	VHCI_RH_RUNNING   = 2
} __attribute__((packed));

struct vhci_port
{
	u16 port_status;
	u16 port_change;
	u8 port_flags;
#define VHCI_PORT_FLAGS_RESUMING         0 // Zeigt den Zustang des Resuming an
};

struct vhci_conf
{
	struct platform_device *pdev;

	u8 port_count;
};

struct vhci
{
	spinlock_t lock;

	enum vhci_rh_state rh_state;

	// TODO: Timer implementieren, um frame_num hochzählen zu lassen.
	//struct timer_list timer;

	struct vhci_port *ports;
	u8 port_count;
	u8 port_sched_offset;
	u32 port_update;

	atomic_t frame_num;

	wait_queue_head_t work_event;

	// URBs, die noch nicht vom Userspace abgeholt wurden, befinden sich in dieser Liste
	struct list_head urbp_list_inbox;

	// URBs, die vom Userspace abgeholt wurden und noch nicht wieder zurückgekommen sind,
	// befinden sich in dieser Liste
	struct list_head urbp_list_fetched;

	// URBs, die vom Userspace abgeholt wurden und noch nicht wieder zurückgekommen sind und
	// vorzeitig abgebrochen werden sollen, befinden sich in dieser Liste
	struct list_head urbp_list_cancel;

	// URBs, die vom Userspace abgeholt wurden und noch nicht wieder zurückgekommen sind und
	// der Userspace bereits darüber informiert wurde, dass sie vorzeitig abgebrochen werden
	// sollen, befinden sich in dieser Liste
	struct list_head urbp_list_canceling;
};

static inline struct vhci *hcd_to_vhci(struct usb_hcd *hcd)
{
	return (struct vhci *)(hcd->hcd_priv);
}

static inline struct usb_hcd *vhci_to_hcd(struct vhci *vhc)
{
	return container_of((void *)vhc, struct usb_hcd, hcd_priv);
}

static inline struct device *vhci_dev(struct vhci *vhc)
{
	return vhci_to_hcd(vhc)->self.controller;
}

static inline struct vhci_conf *dev_to_vhci_conf(struct device *dev)
{
	return (struct vhci_conf *)(*((struct file **)dev->platform_data))->private_data;
}

static void maybe_set_status(struct vhci_urb_priv *urbp, int status)
{
#ifdef OLD_GIVEBACK_MECH
	struct urb *const urb = urbp->urb;
	unsigned long flags;
	spin_lock_irqsave(&urb->lock, flags);
	if(urb->status == -EINPROGRESS)
		urb->status = status;
	spin_unlock_irqrestore(&urb->lock, flags);
#else
	(void)atomic_cmpxchg(&urbp->status, -EINPROGRESS, status);
#endif
}

static void dump_urb(struct urb *urb);

// Gibt den URB an den ursprünglichen Besitzer zurück.
// caller owns vhc->lock and has irq disabled
static void vhci_urb_giveback(struct vhci *vhc, struct vhci_urb_priv *urbp)
{
	struct urb *const urb = urbp->urb;
	struct usb_device *const udev = urb->dev;
#ifndef OLD_GIVEBACK_MECH
	int status;
#endif
	trace_function(vhci_dev(vhc));
#ifndef OLD_GIVEBACK_MECH
	status = atomic_read(&urbp->status);
#endif
	urb->hcpriv = NULL;
	list_del(&urbp->urbp_list);
#ifndef OLD_GIVEBACK_MECH
	usb_hcd_unlink_urb_from_ep(vhci_to_hcd(vhc), urb);
#endif
	spin_unlock(&vhc->lock);
	kfree(urbp);
	dump_urb(urb);
#ifdef OLD_GIVEBACK_MECH
	usb_hcd_giveback_urb(vhci_to_hcd(vhc), urb);
#else
#	ifdef DEBUG
	if(debug_output) vhci_printk(KERN_DEBUG, "status=%d\n", status);
#	endif
	usb_hcd_giveback_urb(vhci_to_hcd(vhc), urb, status);
#endif
	usb_put_dev(udev);
	spin_lock(&vhc->lock);
}

static inline void trigger_work_event(struct vhci *vhc)
{
	wake_up_interruptible(&vhc->work_event);
}

#ifdef OLD_GIVEBACK_MECH
static int vhci_urb_enqueue(struct usb_hcd *hcd, struct usb_host_endpoint *ep, struct urb *urb, gfp_t mem_flags)
#else
static int vhci_urb_enqueue(struct usb_hcd *hcd, struct urb *urb, gfp_t mem_flags)
#endif
{
	struct vhci *vhc;
	struct vhci_urb_priv *urbp;
	unsigned long flags;
#ifndef OLD_GIVEBACK_MECH
	int retval;
#endif

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	if(unlikely(!urb->transfer_buffer && urb->transfer_buffer_length))
		return -EINVAL;

	urbp = kzalloc(sizeof(struct vhci_urb_priv), mem_flags);
	if(unlikely(!urbp))
		return -ENOMEM;
	urbp->urb = urb;

	spin_lock_irqsave(&vhc->lock, flags);
#ifndef OLD_GIVEBACK_MECH
	retval = usb_hcd_link_urb_to_ep(hcd, urb);
	if(unlikely(retval))
	{
		kfree(urbp);
		spin_unlock_irqrestore(&vhc->lock, flags);
		return retval;
	}
#endif
	usb_get_dev(urb->dev);
	list_add_tail(&urbp->urbp_list, &vhc->urbp_list_inbox);
	urb->hcpriv = urbp;
	spin_unlock_irqrestore(&vhc->lock, flags);
	trigger_work_event(vhc);
	return 0;
}

#ifdef OLD_GIVEBACK_MECH
static int vhci_urb_dequeue(struct usb_hcd *hcd, struct urb *urb)
#else
static int vhci_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
#endif
{
	struct vhci *vhc;
	unsigned long flags;
	struct vhci_urb_priv *entry, *urbp = NULL;
#ifndef OLD_GIVEBACK_MECH
	int retval;
#endif

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	spin_lock_irqsave(&vhc->lock, flags);
#ifndef OLD_GIVEBACK_MECH
	retval = usb_hcd_check_unlink_urb(hcd, urb, status);
	if(retval)
	{
		spin_unlock_irqrestore(&vhc->lock, flags);
		return retval;
	}
#endif

	// Die Warteschlange der unbearbeiteten URBs (INBOX) durchsuchen
	list_for_each_entry(entry, &vhc->urbp_list_inbox, urbp_list)
	{
		if(entry->urb == urb)
		{
			urbp = entry;
			break;
		}
	}

	// Falls in INBOX gefunden
	if(urbp)
		vhci_urb_giveback(vhc, urbp);
	else // falls nicht gefunden...
	{
		// ...dann nachschauen, ob der URB gerade durch den Userspace dümpelt
		list_for_each_entry(entry, &vhc->urbp_list_fetched, urbp_list)
		{
			if(entry->urb == urb)
			{
				// In die Cancel-Liste verschieben
				list_move_tail(&entry->urbp_list, &vhc->urbp_list_cancel);
				trigger_work_event(vhc);
				break;
			}
		}
	}

	spin_unlock_irqrestore(&vhc->lock, flags);
	return 0;
}

/*
static void vhci_timer(unsigned long _vhc)
{
	struct vhci *vhc = (struct vhci *)_vhc;
}
*/

static int vhci_hub_status(struct usb_hcd *hcd, char *buf)
{
	struct vhci *vhc;
	unsigned long flags;
	u8 port;
	int retval = 0;

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	memset(buf, 0, 1 + vhc->port_count / 8);

	spin_lock_irqsave(&vhc->lock, flags);
	if(!test_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags))
	{
		spin_unlock_irqrestore(&vhc->lock, flags);
		return 0;
	}

	for(port = 0; port < vhc->port_count; port++)
	{
		if(vhc->ports[port].port_change)
		{
			__set_bit(port + 1, (unsigned long *)buf);
			retval = 1;
		}
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "port %d status 0x%04x has changes at 0x%04x\n", (int)(port + 1), (int)vhc->ports[port].port_status, (int)vhc->ports[port].port_change);
#endif
	}

	if(vhc->rh_state == VHCI_RH_SUSPENDED)
		usb_hcd_resume_root_hub(hcd);

	spin_unlock_irqrestore(&vhc->lock, flags);
	return retval;
}

// caller has lock
// called in vhci_hub_control only
static inline void hub_descriptor(const struct vhci *vhc, char *buf, u16 len)
{
	u16 l = sizeof(struct usb_hub_descriptor);
	char temp[USB_DT_HUB_NONVAR_SIZE];
	struct usb_hub_descriptor *const desc = (struct usb_hub_descriptor *const)temp;

	if(likely(len > USB_DT_HUB_NONVAR_SIZE))
	{
		if(unlikely(len < l)) l = len;
		if(likely(l > USB_DT_HUB_NONVAR_SIZE))
			memset(buf + USB_DT_HUB_NONVAR_SIZE, 0xff, l - USB_DT_HUB_NONVAR_SIZE);
	}
	else l = len;

	memset(temp, 0, USB_DT_HUB_NONVAR_SIZE);
	desc->bDescLength = l;
	desc->bDescriptorType = 0x29;
	desc->bNbrPorts = vhc->port_count;
	desc->wHubCharacteristics = __constant_cpu_to_le16(0x0009); // Per port power and overcurrent
	memcpy(buf, temp, l);
}

// caller has lock
// first port is port# 1 (not 0)
static inline void userspace_needs_port_update(struct vhci *vhc, u8 port)
{
	__set_bit(port, (unsigned long *)&vhc->port_update);
	trigger_work_event(vhc);
}

static int vhci_hub_control(struct usb_hcd *hcd,
                            u16 typeReq,
                            u16 wValue,
                            u16 wIndex,
                            char *buf,
                            u16 wLength)
{
	struct vhci *vhc;
	int retval = 0;
	unsigned long flags;
	u16 *ps, *pc;
	u8 *pf;
	u8 port, has_changes = 0;

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	if(unlikely(!test_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags)))
		return -ETIMEDOUT;

	spin_lock_irqsave(&vhc->lock, flags);

	switch(typeReq)
	{
	case ClearHubFeature:
	case SetHubFeature:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: %sHubFeature [wValue=0x%04x]\n", __FUNCTION__, (typeReq == ClearHubFeature) ? "Clear" : "Set", (int)wValue);
#endif
		if(unlikely(wIndex || wLength || (wValue != C_HUB_LOCAL_POWER && wValue != C_HUB_OVER_CURRENT)))
			goto err;
		break;
	case ClearPortFeature:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: ClearPortFeature [wValue=0x%04x, wIndex=%d]\n", __FUNCTION__, (int)wValue, (int)wIndex);
#endif
		if(unlikely(!wIndex || wIndex > vhc->port_count || wLength))
			goto err;
		ps = &vhc->ports[wIndex - 1].port_status;
		pc = &vhc->ports[wIndex - 1].port_change;
		pf = &vhc->ports[wIndex - 1].port_flags;
		switch(wValue)
		{
		case USB_PORT_FEAT_SUSPEND:
			// (siehe USB 2.0 spec Sektion 11.5 und 11.24.2.7.1.3)
			if(*ps & USB_PORT_STAT_SUSPEND)
			{
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d resuming\n", (int)wIndex);
#endif
				__set_bit(VHCI_PORT_FLAGS_RESUMING, (unsigned long *)pf);
				userspace_needs_port_update(vhc, wIndex);
			}
			break;
		case USB_PORT_FEAT_POWER:
			// (siehe USB 2.0 spec Sektion 11.11 und 11.24.2.7.1.6)
			if(*ps & USB_PORT_STAT_POWER)
			{
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d power-off\n", (int)wIndex);
#endif
				// Alle status bits löschen, außer overcurrent (siehe USB 2.0 spec Sektion 11.24.2.7.1)
				*ps &= USB_PORT_STAT_OVERCURRENT;
				// Alle change bits löschen, außer overcurrent (siehe USB 2.0 spec Sektion 11.24.2.7.2)
				*pc &= USB_PORT_STAT_C_OVERCURRENT;
				// Falls resuming gesetzt
				__clear_bit(VHCI_PORT_FLAGS_RESUMING, (unsigned long *)pf);
				userspace_needs_port_update(vhc, wIndex);
			}
			break;
		case USB_PORT_FEAT_ENABLE:
			// (siehe USB 2.0 spec Sektion 11.5.1.4 und 11.24.2.7.{1,2}.2)
			if(*ps & USB_PORT_STAT_ENABLE)
			{
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d disabled\n", (int)wIndex);
#endif
				// Enable und suspend bits löschen (siehe Sektion 11.24.2.7.1.{2,3})
				*ps &= ~(USB_PORT_STAT_ENABLE | USB_PORT_STAT_SUSPEND);
				// Nicht ganz sicher, ob suspend change bit auch gelöscht werden soll (siehe Sektion 11.24.2.7.2.{2,3})
				*pc &= ~(USB_PORT_STAT_C_ENABLE | USB_PORT_STAT_C_SUSPEND);
				// Falls resuming gesetzt
				__clear_bit(VHCI_PORT_FLAGS_RESUMING, (unsigned long *)pf);
				// TODO: Vielleicht hier die low und high speed bits löschen (Sektion 11.24.2.7.1.{7,8})
				userspace_needs_port_update(vhc, wIndex);
			}
			break;
		case USB_PORT_FEAT_CONNECTION:
		case USB_PORT_FEAT_OVER_CURRENT:
		case USB_PORT_FEAT_RESET:
		case USB_PORT_FEAT_LOWSPEED:
		case USB_PORT_FEAT_HIGHSPEED:
		case USB_PORT_FEAT_INDICATOR:
			break; // no-op
		case USB_PORT_FEAT_C_CONNECTION:
		case USB_PORT_FEAT_C_ENABLE:
		case USB_PORT_FEAT_C_SUSPEND:
		case USB_PORT_FEAT_C_OVER_CURRENT:
		case USB_PORT_FEAT_C_RESET:
			if(__test_and_clear_bit(wValue - 16, (unsigned long *)pc))
				userspace_needs_port_update(vhc, wIndex);
			break;
		//case USB_PORT_FEAT_TEST:
		default:
			goto err;
		}
		break;
	case GetHubDescriptor:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: GetHubDescriptor [wValue=0x%04x, wLength=%d]\n", __FUNCTION__, (int)wValue, (int)wLength);
#endif
		if(unlikely(wIndex))
			goto err;
		hub_descriptor(vhc, buf, wLength);
		break;
	case GetHubStatus:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: GetHubStatus\n", __FUNCTION__);
#endif
		if(unlikely(wValue || wIndex || wLength != 4))
			goto err;
		*(__le32 *)buf = __constant_cpu_to_le32(0);
		break;
	case GetPortStatus:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: GetPortStatus [wIndex=%d]\n", __FUNCTION__, (int)wIndex);
#endif
		if(unlikely(wValue || !wIndex || wIndex > vhc->port_count || wLength != 4))
			goto err;
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: ==> [port_status=0x%04x] [port_change=0x%04x]\n", __FUNCTION__, (int)vhc->ports[wIndex - 1].port_status, (int)vhc->ports[wIndex - 1].port_change);
#endif
		((__le16 *)buf)[0] = cpu_to_le16(vhc->ports[wIndex - 1].port_status);
		((__le16 *)buf)[1] = cpu_to_le16(vhc->ports[wIndex - 1].port_change);
		break;
	case SetPortFeature:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: SetPortFeature [wValue=0x%04x, wIndex=%d]\n", __FUNCTION__, (int)wValue, (int)wIndex);
#endif
		if(unlikely(!wIndex || wIndex > vhc->port_count || wLength))
			goto err;
		ps = &vhc->ports[wIndex - 1].port_status;
		pc = &vhc->ports[wIndex - 1].port_change;
		pf = &vhc->ports[wIndex - 1].port_flags;
		switch(wValue)
		{
		case USB_PORT_FEAT_SUSPEND:
			// USB 2.0 spec Sektion 11.24.2.7.1.3:
			//  "This bit can be set only if the port’s PORT_ENABLE bit is set and the hub receives
			//  a SetPortFeature(PORT_SUSPEND) request."
			// Aus dem darauf folgendem Satz geht außerdem hervor, dass das suspend bit gelöscht werden soll,
			// wann immer das enable bit gelöscht wird.
			// (siehe auch Sektion 11.5)
			if((*ps & USB_PORT_STAT_ENABLE) && !(*ps & USB_PORT_STAT_SUSPEND))
			{
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d suspended\n", (int)wIndex);
#endif
				*ps |= USB_PORT_STAT_SUSPEND;
				userspace_needs_port_update(vhc, wIndex);
			}
			break;
		case USB_PORT_FEAT_POWER:
			// (siehe USB 2.0 spec Sektion 11.11 und 11.24.2.7.1.6)
			if(!(*ps & USB_PORT_STAT_POWER))
			{
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d power-on\n", (int)wIndex);
#endif
				*ps |= USB_PORT_STAT_POWER;
				userspace_needs_port_update(vhc, wIndex);
			}
			break;
		case USB_PORT_FEAT_RESET:
			// (siehe USB 2.0 spec Sektion 11.24.2.7.1.5)
			// Reset nur dann durchführen, wenn ein Device am Port hängt und das Reset Signal nicht bereits anliegt
			if((*ps & USB_PORT_STAT_CONNECTION) && !(*ps & USB_PORT_STAT_RESET))
			{
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d resetting\n", (int)wIndex);
#endif

				// Den Zustand dieser Bits beibehalten und alle anderen löschen
				*ps &= USB_PORT_STAT_POWER
					 | USB_PORT_STAT_CONNECTION
					 | USB_PORT_STAT_LOW_SPEED
					 | USB_PORT_STAT_HIGH_SPEED
					 | USB_PORT_STAT_OVERCURRENT;

				*ps |= USB_PORT_STAT_RESET; // Reset Vorgang eingeleitet

				// Falls resuming gesetzt
				__clear_bit(VHCI_PORT_FLAGS_RESUMING, (unsigned long *)pf);

				userspace_needs_port_update(vhc, wIndex);
			}
#ifdef DEBUG
			else if(debug_output) dev_dbg(vhci_dev(vhc), "Port %d reset not possible because of port_state=%04x\n", (int)wIndex, (int)*ps);
#endif
			break;
		case USB_PORT_FEAT_CONNECTION:
		case USB_PORT_FEAT_OVER_CURRENT:
		case USB_PORT_FEAT_LOWSPEED:
		case USB_PORT_FEAT_HIGHSPEED:
		case USB_PORT_FEAT_INDICATOR:
			break; // no-op
		case USB_PORT_FEAT_C_CONNECTION:
		case USB_PORT_FEAT_C_ENABLE:
		case USB_PORT_FEAT_C_SUSPEND:
		case USB_PORT_FEAT_C_OVER_CURRENT:
		case USB_PORT_FEAT_C_RESET:
			if(!__test_and_set_bit(wValue - 16, (unsigned long *)pc))
				userspace_needs_port_update(vhc, wIndex);
			break;
		//case USB_PORT_FEAT_ENABLE: // Port wird nur nach einem Reset enabled. (USB 2.0 spec Sektion 11.24.2.7.1.2)
		//case USB_PORT_FEAT_TEST:
		default:
			goto err;
		}
		break;
	default:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: +++UNHANDLED_REQUEST+++ [req=0x%04x, v=0x%04x, i=0x%04x, l=%d]\n", __FUNCTION__, (int)typeReq, (int)wValue, (int)wIndex, (int)wLength);
#endif
err:
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "%s: STALL\n", __FUNCTION__);
#endif
		// "protocol stall" on error
		retval = -EPIPE;
	}

	for(port = 0; port < vhc->port_count; port++)
		if(vhc->ports[port].port_change)
			has_changes = 1;

	spin_unlock_irqrestore(&vhc->lock, flags);

	if(has_changes)
		usb_hcd_poll_rh_status(hcd);
	return retval;
}

static int vhci_bus_suspend(struct usb_hcd *hcd)
{
	struct vhci *vhc;
	unsigned long flags;
	u8 port;

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	spin_lock_irqsave(&vhc->lock, flags);

	// Ports suspenden
	for(port = 0; port < vhc->port_count; port++)
	{
		if((vhc->ports[port].port_status & USB_PORT_STAT_ENABLE) &&
			!(vhc->ports[port].port_status & USB_PORT_STAT_SUSPEND))
		{
			dev_dbg(vhci_dev(vhc), "Port %d suspended\n", (int)port + 1);
			vhc->ports[port].port_status |= USB_PORT_STAT_SUSPEND;
			__clear_bit(VHCI_PORT_FLAGS_RESUMING, (unsigned long *)&vhc->ports[port].port_flags);
			userspace_needs_port_update(vhc, port + 1);
		}
	}

	// TODO: Irgendwie verhindern, dass einzelne Ports resumed werden, während der Bus suspended ist.

	vhc->rh_state = VHCI_RH_SUSPENDED;
	hcd->state = HC_STATE_SUSPENDED;

	spin_unlock_irqrestore(&vhc->lock, flags);

	return 0;
}

static int vhci_bus_resume(struct usb_hcd *hcd)
{
	struct vhci *vhc;
	int rc = 0;
	unsigned long flags;

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	spin_lock_irqsave(&vhc->lock, flags);
	if(unlikely(!test_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags)))
	{
		dev_warn(&hcd->self.root_hub->dev, "HC isn't running! You have to resume the host controller device before you resume the root hub.\n");
		rc = -ENODEV;
	}
	else
	{
		vhc->rh_state = VHCI_RH_RUNNING;
		//set_link_state(vhc);
		hcd->state = HC_STATE_RUNNING;
	}
	spin_unlock_irqrestore(&vhc->lock, flags);

	return rc;
}

static inline ssize_t show_urb(char *buf, size_t size, struct urb *urb)
{
	int ep = usb_pipeendpoint(urb->pipe);

	return snprintf(buf, size,
		"urb/%p %s ep%d%s%s len %d/%d\n",
		urb,
		({
			char *s;
			switch(urb->dev->speed)
			{
			case USB_SPEED_LOW:  s = "ls"; break;
			case USB_SPEED_FULL: s = "fs"; break;
			case USB_SPEED_HIGH: s = "hs"; break;
			default:             s = "?";  break;
			};
			s;
		}),
		ep, ep ? (usb_pipein(urb->pipe) ? "in" : "out") : "",
		({
			char *s;
			switch(usb_pipetype(urb->pipe))
			{
			case PIPE_CONTROL:   s = "";      break;
			case PIPE_BULK:      s = "-bulk"; break;
			case PIPE_INTERRUPT: s = "-int";  break;
			default:             s = "-iso";  break;
			};
			s;
		}),
		urb->actual_length, urb->transfer_buffer_length);
}

#ifdef DEBUG
static void dump_urb(struct urb *urb)
{
	if(!debug_output) return;
	int i;
	int max = urb->transfer_buffer_length;
	int in = usb_pipein(urb->pipe);
	vhci_printk(KERN_DEBUG, "dump_urb 0x%016llx:\n", (u64)(unsigned long)urb);
	vhci_printk(KERN_DEBUG, "dvadr=0x%02x epnum=%d epdir=%s eptpe=%s\n", (int)usb_pipedevice(urb->pipe), (int)usb_pipeendpoint(urb->pipe), (in ? "IN" : "OUT"), (usb_pipecontrol(urb->pipe) ? "CTRL" : (usb_pipebulk(urb->pipe) ? "BULK" : (usb_pipeint(urb->pipe) ? "INT" : (usb_pipeisoc(urb->pipe) ? "ISO" : "INV!")))));
#ifdef OLD_GIVEBACK_MECH
	vhci_printk(KERN_DEBUG, "status=%d flags=0x%08x buflen=%d/%d\n", urb->status, urb->transfer_flags, urb->actual_length, max);
#else
	vhci_printk(KERN_DEBUG, "flags=0x%08x buflen=%d/%d\n", urb->transfer_flags, urb->actual_length, max);
#endif
	vhci_printk(KERN_DEBUG, "tbuf=0x%p tdma=0x%016llx sbuf=0x%p sdma=0x%016llx\n", urb->transfer_buffer, (u64)urb->transfer_dma, urb->setup_packet, (u64)urb->setup_dma);
	if(usb_pipeisoc(urb->pipe) || usb_pipeint(urb->pipe))
		vhci_printk(KERN_DEBUG, "interval=%d\n", urb->interval);
	else if(usb_pipecontrol(urb->pipe))
	{
		max = urb->setup_packet[6] | (urb->setup_packet[7] << 8);
		in = urb->setup_packet[0] & 0x80;
		if(urb->setup_packet == NULL)
			vhci_printk(KERN_DEBUG, "(!!!) setup_packet is NULL\n");
		else
		{
			vhci_printk(KERN_DEBUG, "bRequestType=0x%02x bRequest=0x%02x\n", (int)urb->setup_packet[0], (int)urb->setup_packet[1]);
			vhci_printk(KERN_DEBUG, "wValue=0x%04x wIndex=0x%04x wLength=0x%04x\n", urb->setup_packet[2] | (urb->setup_packet[3] << 8), urb->setup_packet[4] | (urb->setup_packet[5] << 8), max);
		}
	}
	vhci_printk(KERN_DEBUG, "data stage (%d/%d bytes %s):\n", urb->actual_length, max, in ? "received" : "transmitted");
	vhci_printk(KERN_DEBUG, "");
	if(in) max = urb->actual_length;
	for(i = 0; i < max; i++)
		printk("%02x ", (unsigned int)((unsigned char*)urb->transfer_buffer)[i]);
	printk("\n");
}
#else
static inline void dump_urb(struct urb *urb) {/* do nothing */}
#endif

static ssize_t show_urbs(struct device *dev, struct device_attribute *attr, char *buf);
static DEVICE_ATTR(urbs_inbox,     S_IRUSR, show_urbs, NULL);
static DEVICE_ATTR(urbs_fetched,   S_IRUSR, show_urbs, NULL);
static DEVICE_ATTR(urbs_cancel,    S_IRUSR, show_urbs, NULL);
static DEVICE_ATTR(urbs_canceling, S_IRUSR, show_urbs, NULL);

static ssize_t show_urbs(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct usb_hcd *hcd;
	struct vhci *vhc;
	struct vhci_urb_priv *urbp;
	size_t size = 0;
	unsigned long flags;
	struct list_head *list;

	hcd = dev_get_drvdata(dev);
	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	if(attr == &dev_attr_urbs_inbox)
		list = &vhc->urbp_list_inbox;
	else if(attr == &dev_attr_urbs_fetched)
		list = &vhc->urbp_list_fetched;
	else if(attr == &dev_attr_urbs_cancel)
		list = &vhc->urbp_list_cancel;
	else if(attr == &dev_attr_urbs_canceling)
		list = &vhc->urbp_list_canceling;
	else
	{
		dev_err(vhci_dev(vhc), "unreachable code reached... wtf?\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&vhc->lock, flags);
	list_for_each_entry(urbp, list, urbp_list)
	{
		size_t temp;

		temp = PAGE_SIZE - size;
		if(unlikely(temp <= 0)) break;

		temp = show_urb(buf, temp, urbp->urb);
		buf += temp;
		size += temp;
	}
	spin_unlock_irqrestore(&vhc->lock, flags);

	return size;
}

static int vhci_start(struct usb_hcd *hcd)
{
	struct vhci *vhc;
	int retval;
	struct vhci_port *ports;
	struct vhci_conf *conf;

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	conf = dev_to_vhci_conf(vhci_dev(vhc));

	ports = kzalloc(conf->port_count * sizeof(struct vhci_port), GFP_KERNEL);
	if(unlikely(ports == NULL)) return -ENOMEM;

	spin_lock_init(&vhc->lock);
	//init_timer(&vhc->timer);
	//vhc->timer.function = vhci_timer;
	//vhc->timer.data = (unsigned long)vhc;
	vhc->ports = ports;
	vhc->port_count = conf->port_count;
	vhc->port_sched_offset = 0;
	vhc->port_update = 0;
	atomic_set(&vhc->frame_num, 0);
	init_waitqueue_head(&vhc->work_event);
	INIT_LIST_HEAD(&vhc->urbp_list_inbox);
	INIT_LIST_HEAD(&vhc->urbp_list_fetched);
	INIT_LIST_HEAD(&vhc->urbp_list_cancel);
	INIT_LIST_HEAD(&vhc->urbp_list_canceling);
	vhc->rh_state = VHCI_RH_RUNNING;

	hcd->power_budget = 30000; // haben genug davon, weil virtuell
	hcd->state = HC_STATE_RUNNING;
	hcd->uses_new_polling = 1;

	retval = device_create_file(vhci_dev(vhc), &dev_attr_urbs_inbox);
	if(unlikely(retval != 0)) goto kfree_port_arr;

	retval = device_create_file(vhci_dev(vhc), &dev_attr_urbs_fetched);
	if(unlikely(retval != 0)) goto rem_file_inbox;

	retval = device_create_file(vhci_dev(vhc), &dev_attr_urbs_cancel);
	if(unlikely(retval != 0)) goto rem_file_fetched;

	retval = device_create_file(vhci_dev(vhc), &dev_attr_urbs_canceling);
	if(unlikely(retval != 0)) goto rem_file_cancel;

	return 0;

rem_file_cancel:
	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_cancel);

rem_file_fetched:
	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_fetched);

rem_file_inbox:
	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_inbox);

kfree_port_arr:
	kfree(ports);
	vhc->ports = NULL;
	vhc->port_count = 0;
	return retval;
}

static void vhci_stop(struct usb_hcd *hcd)
{
	struct vhci *vhc;

	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_canceling);
	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_cancel);
	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_fetched);
	device_remove_file(vhci_dev(vhc), &dev_attr_urbs_inbox);

	if(likely(vhc->ports))
	{
		kfree(vhc->ports);
		vhc->ports = NULL;
		vhc->port_count = 0;
	}

	vhc->rh_state = VHCI_RH_RESET;

	dev_info(vhci_dev(vhc), "stopped\n");
}

static int vhci_get_frame(struct usb_hcd *hcd)
{
	struct vhci *vhc;
	vhc = hcd_to_vhci(hcd);
	trace_function(vhci_dev(vhc));
	return atomic_read(&vhc->frame_num);
}

static const struct hc_driver vhci_hcd = {
	.description      = driver_name,
	.product_desc     = "VHCI Host Controller",
	.hcd_priv_size    = sizeof(struct vhci),

	.flags            = HCD_USB2,

	.start            = vhci_start,
	.stop             = vhci_stop,

	.urb_enqueue      = vhci_urb_enqueue,
	.urb_dequeue      = vhci_urb_dequeue,

	.get_frame_number = vhci_get_frame,

	.hub_status_data  = vhci_hub_status,
	.hub_control      = vhci_hub_control,
	.bus_suspend      = vhci_bus_suspend,
	.bus_resume       = vhci_bus_resume
};

static int vhci_hcd_probe(struct platform_device *pdev)
{
	struct usb_hcd *hcd;
	int retval;

#ifdef DEBUG
	if(debug_output) dev_dbg(&pdev->dev, "%s\n", __FUNCTION__);
#endif
	dev_info(&pdev->dev, DRIVER_DESC "  Ver. " DRIVER_VERSION "\n");

	hcd = usb_create_hcd(&vhci_hcd, &pdev->dev, pdev->dev.bus_id);
	if(unlikely(!hcd)) return -ENOMEM;

	retval = usb_add_hcd(hcd, 0, 0);
	if(unlikely(retval != 0)) usb_put_hcd(hcd);

	return retval;
}

static int vhci_hcd_remove(struct platform_device *pdev)
{
	unsigned long flags;
	struct usb_hcd *hcd;
	struct vhci *vhc;
	struct vhci_urb_priv *urbp;

    hcd = platform_get_drvdata(pdev);
	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	spin_lock_irqsave(&vhc->lock, flags);
	while(!list_empty(&vhc->urbp_list_inbox))
	{
		urbp = list_entry(vhc->urbp_list_inbox.next, struct vhci_urb_priv, urbp_list);
		maybe_set_status(urbp, -ESHUTDOWN);
		vhci_urb_giveback(vhc, urbp);
	}
	while(!list_empty(&vhc->urbp_list_fetched))
	{
		urbp = list_entry(vhc->urbp_list_fetched.next, struct vhci_urb_priv, urbp_list);
		maybe_set_status(urbp, -ESHUTDOWN);
		vhci_urb_giveback(vhc, urbp);
	}
	while(!list_empty(&vhc->urbp_list_cancel))
	{
		urbp = list_entry(vhc->urbp_list_cancel.next, struct vhci_urb_priv, urbp_list);
		maybe_set_status(urbp, -ESHUTDOWN);
		vhci_urb_giveback(vhc, urbp);
	}
	while(!list_empty(&vhc->urbp_list_canceling))
	{
		urbp = list_entry(vhc->urbp_list_canceling.next, struct vhci_urb_priv, urbp_list);
		maybe_set_status(urbp, -ESHUTDOWN);
		vhci_urb_giveback(vhc, urbp);
	}
	spin_unlock_irqrestore(&vhc->lock, flags);

    usb_remove_hcd(hcd);
    usb_put_hcd(hcd);

	return 0;
}

static int vhci_hcd_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct usb_hcd *hcd;
	struct vhci *vhc;
	int rc = 0;

	hcd = platform_get_drvdata(pdev);
	vhc = hcd_to_vhci(hcd);

	trace_function(vhci_dev(vhc));

	if(unlikely(vhc->rh_state == VHCI_RH_RUNNING))
	{
		dev_warn(&pdev->dev, "Root hub isn't suspended! You have to suspend the root hub before you suspend the host controller device.\n");
		rc = -EBUSY;
	}
	else
		clear_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);

	return rc;
}

static int vhci_hcd_resume(struct platform_device *pdev)
{
	struct usb_hcd *hcd;
	struct vhci *vhc;

	hcd = platform_get_drvdata(pdev);
	vhc = hcd_to_vhci(hcd);
	trace_function(vhci_dev(vhc));

	set_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
	usb_hcd_poll_rh_status(hcd);
	return 0;
}

static struct platform_driver vhci_hcd_driver = {
	.probe      = vhci_hcd_probe,
	.remove     = vhci_hcd_remove,
	.suspend    = vhci_hcd_suspend,
	.resume     = vhci_hcd_resume,
	.driver     = {
		.name   = driver_name,
		.owner  = THIS_MODULE
	}
};

// Callback Funktion für driver_for_each_device(..) in ioc_register(...).
// data zeigt auf device id, die geprüft werden soll.
// Funktion liefert Fehler zurück, wenn das Device die zu prüfende id belegt (Enumeration wird bei
// Fehler abgebrochen.).
static int device_enum(struct device *dev, void *data)
{
	struct platform_device *pdev;
	pdev = to_platform_device(dev);
	return unlikely(*((const int *)data) == pdev->id) ? -EINVAL : 0;
}

static spinlock_t dev_enum_lock = SPIN_LOCK_UNLOCKED;

static int device_open(struct inode *inode, struct file *file)
{
	vhci_dbg("%s(inode=%p, file=%p)\n", __FUNCTION__, inode, file);

	if(unlikely(file->private_data != NULL))
	{
		vhci_printk(KERN_ERR, "file->private_data != NULL (Da is schon vor mir einer drueber grutscht.)\n");
		return -EINVAL;
	}

	try_module_get(THIS_MODULE);
	return 0;
}

// called in device_ioctl only
static inline int ioc_register(struct file *file, struct vhci_ioc_register __user *arg)
{
	int retval, i;
	struct platform_device *pdev;
	struct vhci_conf *conf;
	u8 pc;

	vhci_dbg("cmd=VHCI_HCD_IOCREGISTER\n");

	if(unlikely(file->private_data != NULL))
	{
		vhci_printk(KERN_ERR, "file->private_data != NULL (VHCI_HCD_IOCREGISTER already done?)\n");
		return -EPROTO;
	}

	__get_user(pc, &arg->port_count);
	if(pc > 31)
		return -EINVAL;

	// Nach freier device id suchen
	spin_lock(&dev_enum_lock);
	for(i = 0; i < 10000; i++)
	{
		retval = driver_for_each_device(&vhci_hcd_driver.driver, NULL, &i, device_enum);
		if(unlikely(retval == 0)) break;
	}
	if(unlikely(i >= 10000))
	{
		spin_unlock(&dev_enum_lock);
		vhci_printk(KERN_ERR, "there are too much devices!\n");
		return -EBUSY;
	}

	vhci_dbg("allocate platform_device %s.%d\n", driver_name, i);
	pdev = platform_device_alloc(driver_name, i);
	if(unlikely(!pdev))
	{
		spin_unlock(&dev_enum_lock);
		return -ENOMEM;
	}

	vhci_dbg("associate ptr to file structure with platform_device\n");
	retval = platform_device_add_data(pdev, &file, sizeof(struct file *));
	if(unlikely(retval < 0))
	{
		spin_unlock(&dev_enum_lock);
		goto pdev_put;
	}

	vhci_dbg("allocate and associate vhci_conf structure with file->private_data\n");
	conf = kmalloc(sizeof(struct vhci_conf), GFP_KERNEL);
	if(unlikely(conf == NULL))
	{
		spin_unlock(&dev_enum_lock);
		retval = -ENOMEM;
		goto pdev_put;
	}
	conf->pdev = pdev;
	conf->port_count = pc;
	file->private_data = conf;

	vhci_dbg("register platform_device %s.%d\n", pdev->name, pdev->id);
	retval = platform_device_register(pdev);
	spin_unlock(&dev_enum_lock);
	if(unlikely(retval < 0))
	{
		vhci_printk(KERN_ERR, "register platform_device %s.%d failed\n", pdev->name, pdev->id);
		kfree(conf);
		file->private_data = NULL;
		goto pdev_put;
	}

	// ID in Userspace kopieren
	__put_user(pdev->id, &arg->id);

	// Bus-ID in Userspace kopieren
	i = (BUS_ID_SIZE < 20) ? BUS_ID_SIZE : 20;
	if(copy_to_user(arg->bus_id, pdev->dev.bus_id, i - 1))
	{
		vhci_printk(KERN_WARNING, "Failed to copy bus_id to userspace.\n");
		__put_user('\0', arg->bus_id);
	}
	// Dafür sorgen, dass letztes Zeichen auf jeden Fall Null ist
	__put_user('\0', arg->bus_id + i - 1);

	return 0;

pdev_put:
	platform_device_put(pdev);
	return retval;
}

static int device_release(struct inode *inode, struct file *file)
{
	struct vhci_conf *conf;

	vhci_dbg("%s(inode=%p, file=%p)\n", __FUNCTION__, inode, file);

	conf = file->private_data;
	file->private_data = NULL;

	if(likely(conf))
	{
		vhci_dbg("unregister platform_device %s\n", conf->pdev->dev.bus_id);
		platform_device_unregister(conf->pdev);

		kfree(conf);
	}
	else
	{
		vhci_dbg("was not configured\n");
	}

	module_put(THIS_MODULE);
	return 0;
}

static ssize_t device_read(struct file *file,
                           char __user *buffer,
                           size_t length,
                           loff_t *offset)
{
	vhci_dbg("%s(file=%p)\n", __FUNCTION__, file);
	return -ENODEV;
}

static ssize_t device_write(struct file *file,
                            const char __user *buffer,
                            size_t length,
                            loff_t *offset)
{
	vhci_dbg("%s(file=%p)\n", __FUNCTION__, file);
	return -ENODEV;
}

// called in device_ioctl only
static inline int ioc_port_stat(struct vhci *vhc, struct vhci_ioc_port_stat __user *arg)
{
	unsigned long flags;
	u8 index;
	u16 status, change, overcurrent;

#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=VHCI_HCD_IOCPORTSTAT\n");
#endif

	__get_user(status, &arg->status);
	__get_user(change, &arg->change);
	__get_user(index, &arg->index);
	if(unlikely(!index || index > vhc->port_count))
		return -EINVAL;

	if(unlikely(change != USB_PORT_STAT_C_CONNECTION &&
	            change != USB_PORT_STAT_C_ENABLE &&
	            change != USB_PORT_STAT_C_SUSPEND &&
	            change != USB_PORT_STAT_C_OVERCURRENT &&
	            change != USB_PORT_STAT_C_RESET &&
	            change != (USB_PORT_STAT_C_RESET | USB_PORT_STAT_C_ENABLE)))
		return -EINVAL;

	spin_lock_irqsave(&vhc->lock, flags);
	if(unlikely(!(vhc->ports[index - 1].port_status & USB_PORT_STAT_POWER)))
	{
		spin_unlock_irqrestore(&vhc->lock, flags);
		return -EPROTO;
	}

#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "performing PORT_STAT [port=%d ~status=0x%04x ~change=0x%04x]\n", (int)index, (int)status, (int)change);
#endif

	switch(change)
	{
	case USB_PORT_STAT_C_CONNECTION:
		overcurrent = vhc->ports[index - 1].port_status & USB_PORT_STAT_OVERCURRENT;
		vhc->ports[index - 1].port_change |= USB_PORT_STAT_C_CONNECTION;
		if(status & USB_PORT_STAT_CONNECTION)
			vhc->ports[index - 1].port_status = USB_PORT_STAT_POWER | USB_PORT_STAT_CONNECTION |
				((status & USB_PORT_STAT_LOW_SPEED) ? USB_PORT_STAT_LOW_SPEED :
				((status & USB_PORT_STAT_HIGH_SPEED) ? USB_PORT_STAT_HIGH_SPEED : 0)) |
				overcurrent;
		else
			vhc->ports[index - 1].port_status = USB_PORT_STAT_POWER | overcurrent;
		__clear_bit(VHCI_PORT_FLAGS_RESUMING, (unsigned long *)&vhc->ports[index - 1].port_flags);
		break;

	case USB_PORT_STAT_C_ENABLE:
		if(unlikely(!(vhc->ports[index - 1].port_status & USB_PORT_STAT_CONNECTION) ||
			(vhc->ports[index - 1].port_status & USB_PORT_STAT_RESET) ||
			(status & USB_PORT_STAT_ENABLE)))
		{
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -EPROTO;
		}
		vhc->ports[index - 1].port_change |= USB_PORT_STAT_C_ENABLE;
		vhc->ports[index - 1].port_status &= ~USB_PORT_STAT_ENABLE;
		__clear_bit(VHCI_PORT_FLAGS_RESUMING, (unsigned long *)&vhc->ports[index - 1].port_flags);
		vhc->ports[index - 1].port_status &= ~USB_PORT_STAT_SUSPEND;
		break;

	case USB_PORT_STAT_C_SUSPEND:
		if(unlikely(!(vhc->ports[index - 1].port_status & USB_PORT_STAT_CONNECTION) ||
			!(vhc->ports[index - 1].port_status & USB_PORT_STAT_ENABLE) ||
			(vhc->ports[index - 1].port_status & USB_PORT_STAT_RESET) ||
			(status & USB_PORT_STAT_SUSPEND)))
		{
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -EPROTO;
		}
		__clear_bit(VHCI_PORT_FLAGS_RESUMING, (unsigned long *)&vhc->ports[index - 1].port_flags);
		vhc->ports[index - 1].port_change |= USB_PORT_STAT_C_SUSPEND;
		vhc->ports[index - 1].port_status &= ~USB_PORT_STAT_SUSPEND;
		break;

	case USB_PORT_STAT_C_OVERCURRENT:
		vhc->ports[index - 1].port_change |= USB_PORT_STAT_C_OVERCURRENT;
		vhc->ports[index - 1].port_status &= ~USB_PORT_STAT_OVERCURRENT;
		vhc->ports[index - 1].port_status |= status & USB_PORT_STAT_OVERCURRENT;
		break;

	default: // USB_PORT_STAT_C_RESET [| USB_PORT_STAT_C_ENABLE]
		if(unlikely(!(vhc->ports[index - 1].port_status & USB_PORT_STAT_CONNECTION) ||
			!(vhc->ports[index - 1].port_status & USB_PORT_STAT_RESET) ||
			(status & USB_PORT_STAT_RESET)))
		{
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -EPROTO;
		}
		if(change & USB_PORT_STAT_C_ENABLE)
		{
			if(status & USB_PORT_STAT_ENABLE)
			{
				spin_unlock_irqrestore(&vhc->lock, flags);
				return -EPROTO;
			}
			vhc->ports[index - 1].port_change |= USB_PORT_STAT_C_ENABLE;
		}
		else
			vhc->ports[index - 1].port_status |= status & USB_PORT_STAT_ENABLE;
		vhc->ports[index - 1].port_change |= USB_PORT_STAT_C_RESET;
		vhc->ports[index - 1].port_status &= ~USB_PORT_STAT_RESET;
		break;
	}

	userspace_needs_port_update(vhc, index);
	spin_unlock_irqrestore(&vhc->lock, flags);

	usb_hcd_poll_rh_status(vhci_to_hcd(vhc));
	return 0;
}

static inline u8 conv_urb_type(u8 type)
{
	switch(type & 0x3)
	{
	case PIPE_ISOCHRONOUS: return VHCI_IOC_URB_TYPE_ISO;
	case PIPE_INTERRUPT:   return VHCI_IOC_URB_TYPE_INT;
	case PIPE_BULK:        return VHCI_IOC_URB_TYPE_BULK;
	default:               return VHCI_IOC_URB_TYPE_CONTROL;
	}
}

static inline u16 conv_urb_flags(unsigned int flags)
{
	return ((flags & URB_SHORT_NOT_OK) ? VHCI_IOC_URB_FLAGS_SHORT_NOT_OK : 0) |
	       ((flags & URB_ISO_ASAP)     ? VHCI_IOC_URB_FLAGS_ISO_ASAP     : 0) |
	       ((flags & URB_ZERO_PACKET)  ? VHCI_IOC_URB_FLAGS_ZERO_PACKET  : 0);
}

static int has_work(struct vhci *vhc)
{
	unsigned long flags;
	int y = 0;
	spin_lock_irqsave(&vhc->lock, flags);
	if(vhc->port_update ||
		!list_empty(&vhc->urbp_list_cancel) ||
		!list_empty(&vhc->urbp_list_inbox))
		y = 1;
	spin_unlock_irqrestore(&vhc->lock, flags);
	return y;
}

// called in device_ioctl only
static inline int ioc_fetch_work(struct vhci *vhc, struct vhci_ioc_work __user *arg)
{
	unsigned long flags;
	u8 _port, port;
	struct vhci_urb_priv *urbp;
	long wret;

#ifdef DEBUG
	// Floods the logs
	//if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=VHCI_HCD_IOCFETCHWORK\n");
#endif

	wret = wait_event_interruptible_timeout(vhc->work_event, has_work(vhc), msecs_to_jiffies(100));
	if(unlikely(wret < 0))
	{
		if(likely(wret == -ERESTARTSYS))
			return -EINTR;
		return wret;
	}
	else if(!wret)
		return -ETIMEDOUT;

	spin_lock_irqsave(&vhc->lock, flags);
	if(!list_empty(&vhc->urbp_list_cancel))
	{
		urbp = list_entry(vhc->urbp_list_cancel.next, struct vhci_urb_priv, urbp_list);
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=VHCI_HCD_IOCFETCHWORK [work=CANCEL_URB handle=0x%016llx]\n", (u64)(unsigned long)urbp->urb);
#endif
		__put_user(VHCI_IOC_WORK_TYPE_CANCEL_URB, &arg->type);
		__put_user((u64)(unsigned long)urbp->urb, &arg->handle);
		list_move_tail(&urbp->urbp_list, &vhc->urbp_list_canceling);
		spin_unlock_irqrestore(&vhc->lock, flags);
		return 0;
	}

	if(vhc->port_update)
	{
		if(vhc->port_sched_offset >= vhc->port_count)
			vhc->port_sched_offset = 0;
		for(_port = 0; _port < vhc->port_count; _port++)
		{
			port = (_port + vhc->port_sched_offset) % vhc->port_count;
			if(__test_and_clear_bit(port + 1, (unsigned long *)&vhc->port_update))
			{
				vhc->port_sched_offset = port + 1;
#ifdef DEBUG
				if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=VHCI_HCD_IOCFETCHWORK [work=PORT_STAT port=%d status=0x%04x change=0x%04x]\n", (int)(port + 1), (int)vhc->ports[port].port_status, (int)vhc->ports[port].port_change);
#endif
				__put_user(VHCI_IOC_WORK_TYPE_PORT_STAT, &arg->type);
				__put_user(port + 1, &arg->work.port.index);
				__put_user(vhc->ports[port].port_status, &arg->work.port.status);
				__put_user(vhc->ports[port].port_change, &arg->work.port.change);
				__put_user(test_bit(VHCI_PORT_FLAGS_RESUMING, (unsigned long *)&vhc->ports[port].port_flags)
					? (1 << VHCI_IOC_PORT_STAT_FLAGS_RESUMING) : 0, &arg->work.port.flags);
				spin_unlock_irqrestore(&vhc->lock, flags);
				return 0;
			}
		}
	}

repeat:
	if(!list_empty(&vhc->urbp_list_inbox))
	{
		urbp = list_entry(vhc->urbp_list_inbox.next, struct vhci_urb_priv, urbp_list);
		__put_user(VHCI_IOC_WORK_TYPE_PROCESS_URB, &arg->type);
		__put_user((u64)(unsigned long)urbp->urb, &arg->handle);
		__put_user(usb_pipedevice(urbp->urb->pipe), &arg->work.urb.address);
		__put_user(usb_pipeendpoint(urbp->urb->pipe) | (usb_pipein(urbp->urb->pipe) ? 0x80 : 0x00), &arg->work.urb.endpoint);
		__put_user(conv_urb_type(usb_pipetype(urbp->urb->pipe)), &arg->work.urb.type);
		__put_user(conv_urb_flags(urbp->urb->transfer_flags), &arg->work.urb.flags);
		if(usb_pipecontrol(urbp->urb->pipe))
		{
			const struct usb_ctrlrequest *cmd;
			u16 wValue, wIndex, wLength;
			if(unlikely(!urbp->urb->setup_packet))
				goto invalid_urb;
			cmd = (struct usb_ctrlrequest *)urbp->urb->setup_packet;
			wValue = le16_to_cpu(cmd->wValue);
			wIndex = le16_to_cpu(cmd->wIndex);
			wLength = le16_to_cpu(cmd->wLength);
			if(unlikely(wLength > urbp->urb->transfer_buffer_length))
				goto invalid_urb;
			if(cmd->bRequestType & 0x80)
			{
				if(unlikely(!wLength || !urbp->urb->transfer_buffer))
					goto invalid_urb;
			}
			else
			{
				if(unlikely(wLength && !urbp->urb->transfer_buffer))
					goto invalid_urb;
			}
			__put_user(wLength, &arg->work.urb.buffer_length);
			__put_user(cmd->bRequestType, &arg->work.urb.setup_packet.bmRequestType);
			__put_user(cmd->bRequest, &arg->work.urb.setup_packet.bRequest);
			__put_user(wValue, &arg->work.urb.setup_packet.wValue);
			__put_user(wIndex, &arg->work.urb.setup_packet.wIndex);
			__put_user(wLength, &arg->work.urb.setup_packet.wLength);
		}
		else
		{
			if(usb_pipein(urbp->urb->pipe))
			{
				if(unlikely(!urbp->urb->transfer_buffer_length || !urbp->urb->transfer_buffer))
					goto invalid_urb;
			}
			else
			{
				if(unlikely(urbp->urb->transfer_buffer_length && !urbp->urb->transfer_buffer))
					goto invalid_urb;
			}
			__put_user(urbp->urb->transfer_buffer_length, &arg->work.urb.buffer_length);
		}
		__put_user(urbp->urb->interval, &arg->work.urb.interval);

#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=VHCI_HCD_IOCFETCHWORK [work=PROCESS_URB handle=0x%016llx]\n", (u64)(unsigned long)urbp->urb);
#endif
		dump_urb(urbp->urb);
		list_move_tail(&urbp->urbp_list, &vhc->urbp_list_fetched);
		spin_unlock_irqrestore(&vhc->lock, flags);
		return 0;

	invalid_urb:
		// Ungueltige URBs gleich wieder abschieben
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=VHCI_HCD_IOCFETCHWORK  <<< THROWING AWAY INVALID URB >>>  [handle=0x%016llx]\n", (u64)(unsigned long)urbp->urb);
#endif
		maybe_set_status(urbp, -EPIPE);
		vhci_urb_giveback(vhc, urbp);
		goto repeat;
	}

	spin_unlock_irqrestore(&vhc->lock, flags);
	return -ENODATA;
}

// caller has lock
static inline struct vhci_urb_priv *urbp_from_handle(struct vhci *vhc, const void *handle)
{
	struct vhci_urb_priv *entry;
	list_for_each_entry(entry, &vhc->urbp_list_fetched, urbp_list)
		if(entry->urb == handle)
			return entry;
	return NULL;
}

// caller has lock
static inline struct vhci_urb_priv *urbp_from_handle_in_cancel(struct vhci *vhc, const void *handle)
{
	struct vhci_urb_priv *entry;
	list_for_each_entry(entry, &vhc->urbp_list_cancel, urbp_list)
		if(entry->urb == handle)
			return entry;
	return NULL;
}

// caller has lock
static inline struct vhci_urb_priv *urbp_from_handle_in_canceling(struct vhci *vhc, const void *handle)
{
	struct vhci_urb_priv *entry;
	list_for_each_entry(entry, &vhc->urbp_list_canceling, urbp_list)
		if(entry->urb == handle)
			return entry;
	return NULL;
}

// caller has lock
static inline int is_urb_dir_in(const struct urb *urb)
{
	if(unlikely(usb_pipecontrol(urb->pipe)))
	{
		const struct usb_ctrlrequest *cmd = (struct usb_ctrlrequest *)urb->setup_packet;
		return cmd->bRequestType & 0x80;
	}
	else
		return usb_pipein(urb->pipe);
}

// -ECANCELED stellt keinen Fehler dar, sondern zeigt an, dass sich der URB in der
// Cancel- oder Canceling-Liste befand.
// Im Fehlerfall wird der URB, falls der Handle gefunden wurde, ebenfalls an den Erzeuger
// zurückgegeben.
// called in ioc_giveback{,32} only
static inline int ioc_giveback_common(struct vhci *vhc, const void *handle, int status, int act, const void __user *buf)
{
	unsigned long flags;
	int retval = 0;
	struct vhci_urb_priv *urbp;

	spin_lock_irqsave(&vhc->lock, flags);
	if(unlikely(!(urbp = urbp_from_handle(vhc, handle))))
	{
		// Falls nicht gefunden, nachschauen, ob in Cancel{,ing}-Liste
		if(likely((urbp = urbp_from_handle_in_canceling(vhc, handle)) ||
			(urbp = urbp_from_handle_in_cancel(vhc, handle))))
		{
#ifdef DEBUG
			if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: urb was canceled\n");
#endif
			retval = -ECANCELED;
		}
		else
		{
#ifdef DEBUG
			if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: handle not found\n");
#endif
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -ENOENT;
		}
	}

	if(unlikely(act > urbp->urb->transfer_buffer_length))
	{
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: invalid: buffer_actual > buffer_length\n");
#endif
		retval = is_urb_dir_in(urbp->urb) ? -ENOBUFS : -EINVAL;
		goto done_with_errors;
	}
	if(is_urb_dir_in(urbp->urb))
	{
		if(unlikely(act && !buf))
		{
#ifdef DEBUG
			if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: buf must not be zero\n");
#endif
			retval = -EINVAL;
			goto done_with_errors;
		}
		if(unlikely(copy_from_user(urbp->urb->transfer_buffer, buf, act)))
		{
#ifdef DEBUG
			if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: copy_from_user(buf) failed\n");
#endif
			retval = -EFAULT;
			goto done_with_errors;
		}
	}
	else if(unlikely(buf))
	{
#ifdef DEBUG
		if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: invalid: buf should be NULL\n");
#endif
		// Erwartet keine Daten, also sollte buf NULL sein
		retval = -EINVAL;
		goto done_with_errors;
	}
	urbp->urb->actual_length = act;

	// Jetzt ist der URB fertig und darf wieder zu seinem Erzeuger zurück
	maybe_set_status(urbp, status);
	vhci_urb_giveback(vhc, urbp);
	spin_unlock_irqrestore(&vhc->lock, flags);
#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: done\n");
#endif
	return retval;

done_with_errors:
	vhci_urb_giveback(vhc, urbp);
	spin_unlock_irqrestore(&vhc->lock, flags);
#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "GIVEBACK: done (with errors)\n");
#endif
	return retval;
}

// called in device_ioctl only
static inline int ioc_giveback(struct vhci *vhc, const struct vhci_ioc_giveback __user *arg)
{
	u64 handle64;
	const void *handle;
	const void __user *buf;
	int status, act;

#ifdef DEBUF
	if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=VHCI_HCD_IOCGIVEBACK\n");
#endif

	if(sizeof(void *) > 4)
		__get_user(handle64, &arg->handle);
	else
	{
		u32 handle1, handle2;
		__get_user(handle1, (u32 __user *)&arg->handle);
		__get_user(handle2, (u32 __user *)&arg->handle + 1);
		*((u32 *)&handle64) = handle1;
		*((u32 *)&handle64 + 1) = handle2;
		if(handle64 >> 32)
			return -EINVAL;
	}
	__get_user(status, &arg->status);
	__get_user(act, &arg->buffer_actual);
	__get_user(buf, &arg->buffer);
	handle = (const void *)(unsigned long)handle64;
	if(unlikely(!handle))
		return -EINVAL;
	return ioc_giveback_common(vhc, handle, status, act, buf);
}

// called in ioc_fetch_data{,32} only
static inline int ioc_fetch_data_common(struct vhci *vhc, const void *handle, void __user *user_buf, int user_len)
{
	unsigned long flags;
	int tb_len;
	struct vhci_urb_priv *urbp;

	spin_lock_irqsave(&vhc->lock, flags);
	if(unlikely(!(urbp = urbp_from_handle(vhc, handle))))
	{
		// Falls nicht gefunden, nachschauen, ob in Cancel{,ing}-Liste
		if(likely((urbp = urbp_from_handle_in_cancel(vhc, handle)) ||
			(urbp = urbp_from_handle_in_canceling(vhc, handle))))
		{
			// URB an Erzeuger zurückgeben, weil Userspace weis jetzt, dass abgebrochen
			vhci_urb_giveback(vhc, urbp);
			spin_unlock_irqrestore(&vhc->lock, flags);
			return -ECANCELED;
		}
		spin_unlock_irqrestore(&vhc->lock, flags);
		return -ENOENT;
	}

	tb_len = urbp->urb->transfer_buffer_length;
	if(unlikely(usb_pipecontrol(urbp->urb->pipe)))
	{
		const struct usb_ctrlrequest *cmd = (struct usb_ctrlrequest *)urbp->urb->setup_packet;
		tb_len = le16_to_cpu(cmd->wLength);
	}

	if(unlikely(is_urb_dir_in(urbp->urb) || !tb_len || !urbp->urb->transfer_buffer))
	{
		spin_unlock_irqrestore(&vhc->lock, flags);
		return -ENODATA;
	}

	if(unlikely(!user_buf || user_len < tb_len))
	{
		spin_unlock_irqrestore(&vhc->lock, flags);
		return -EINVAL;
	}
	if(unlikely(copy_to_user(user_buf, urbp->urb->transfer_buffer, tb_len)))
	{
		spin_unlock_irqrestore(&vhc->lock, flags);
		return -EFAULT;
	}
	spin_unlock_irqrestore(&vhc->lock, flags);
	return 0;
}

// called in device_ioctl only
static inline int ioc_fetch_data(struct vhci *vhc, struct vhci_ioc_urb_data __user *arg)
{
	u64 handle64;
	const void *handle;
	void __user *user_buf;
	int user_len;

#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=VHCI_HCD_IOCFETCHDATA\n");
#endif
	if(sizeof(void *) > 4)
		__get_user(handle64, &arg->handle);
	else
	{
		u32 handle1, handle2;
		__get_user(handle1, (u32 __user *)&arg->handle);
		__get_user(handle2, (u32 __user *)&arg->handle + 1);
		*((u32 *)&handle64) = handle1;
		*((u32 *)&handle64 + 1) = handle2;
		if(handle64 >> 32)
			return -EINVAL;
	}
	__get_user(user_len, &arg->buffer_length);
	__get_user(user_buf, &arg->buffer);
	handle = (const void *)(unsigned long)handle64;
	if(unlikely(!handle))
		return -EINVAL;
	return ioc_fetch_data_common(vhc, handle, user_buf, user_len);
}

#ifdef CONFIG_COMPAT
// called in device_ioctl only
static inline int ioc_giveback32(struct vhci *vhc, const struct vhci_ioc_giveback32 __user *arg)
{
	u64 handle64;
	const void *handle;
	u32 buf32;
	const void __user *buf;
	int status, act;

#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=VHCI_HCD_IOCGIVEBACK32\n");
#endif
	__get_user(handle64, (const u64 __user *)&arg->handle1);
	__get_user(status, &arg->status);
	__get_user(act, &arg->buffer_actual);
	__get_user(buf32, &arg->buffer);
	handle = (const void *)(unsigned long)handle64;
	if(unlikely(!handle))
		return -EINVAL;
	buf = compat_ptr(buf32);
	return ioc_giveback_common(vhc, handle, status, act, buf);
}

// called in device_ioctl only
static inline int ioc_fetch_data32(struct vhci *vhc, struct vhci_ioc_urb_data32 __user *arg)
{
	u64 handle64;
	const void *handle;
	u32 user_buf32;
	void __user *user_buf;
	int user_len;

#ifdef DEBUG
	if(debug_output) dev_dbg(vhci_dev(vhc), "cmd=VHCI_HCD_IOCFETCHDATA32\n");
#endif

	__get_user(handle64, &arg->handle);
	__get_user(user_len, &arg->buffer_length);
	__get_user(user_buf32, &arg->buffer);
	handle = (const void *)(unsigned long)handle64;
	if(unlikely(!handle))
		return -EINVAL;
	user_buf = compat_ptr(user_buf32);
	return ioc_fetch_data_common(vhc, handle, user_buf, user_len);
}
#endif

static int device_ioctl(struct inode *inode,
                        struct file *file,
                        unsigned int cmd,
                        unsigned long arg)
{
	struct vhci_conf *conf;
	struct usb_hcd *hcd;
	struct vhci *vhc;
	int ret = 0;

	// Floods the logs
	//vhci_dbg("%s(file=%p)\n", __FUNCTION__, file);

	if(unlikely(_IOC_TYPE(cmd) != VHCI_HCD_IOC_MAGIC)) return -ENOTTY;
	if(unlikely(_IOC_NR(cmd) > VHCI_HCD_IOC_MAXNR)) return -ENOTTY;

	if(unlikely((_IOC_DIR(cmd) & _IOC_READ) && !access_ok(VERIFY_WRITE, (void *)arg, _IOC_SIZE(cmd))))
		return -EFAULT;
	if(unlikely((_IOC_DIR(cmd) & _IOC_WRITE) && !access_ok(VERIFY_READ, (void *)arg, _IOC_SIZE(cmd))))
		return -EFAULT;

	if(unlikely(cmd == VHCI_HCD_IOCREGISTER))
		return ioc_register(file, (struct vhci_ioc_register __user *)arg);

	conf = file->private_data;

	if(unlikely(!conf))
		return -EPROTO;

	hcd = platform_get_drvdata(conf->pdev);
	vhc = hcd_to_vhci(hcd);

	switch(__builtin_expect(cmd, VHCI_HCD_IOCFETCHWORK))
	{
	case VHCI_HCD_IOCPORTSTAT:
		ret = ioc_port_stat(vhc, (struct vhci_ioc_port_stat __user *)arg);
		break;

	case VHCI_HCD_IOCFETCHWORK:
		ret = ioc_fetch_work(vhc, (struct vhci_ioc_work __user *)arg);
		break;

	case VHCI_HCD_IOCGIVEBACK:
		ret = ioc_giveback(vhc, (struct vhci_ioc_giveback __user *)arg);
		break;

	case VHCI_HCD_IOCFETCHDATA:
		ret = ioc_fetch_data(vhc, (struct vhci_ioc_urb_data __user *)arg);
		break;

#ifdef CONFIG_COMPAT
	case VHCI_HCD_IOCGIVEBACK32:
		ret = ioc_giveback32(vhc, (struct vhci_ioc_giveback32 __user *)arg);
		break;

	case VHCI_HCD_IOCFETCHDATA32:
		ret = ioc_fetch_data32(vhc, (struct vhci_ioc_urb_data32 __user *)arg);
		break;
#endif

	default:
		ret = -ENOTTY;
	}

	return ret;
}

static loff_t device_llseek(struct file *file, loff_t offset, int origin)
{
	vhci_dbg("%s(file=%p)\n", __FUNCTION__, file);
	return -ESPIPE;
}

static struct file_operations fops = {
	.owner   = THIS_MODULE,
	.llseek  = device_llseek,
	.read    = device_read,
	.write   = device_write,
	.ioctl   = device_ioctl,
	.open    = device_open,
	.release = device_release // a.k.a. close
};

#ifdef DEBUG
static ssize_t show_debug_output(struct device_driver *drv, char *buf)
{
	if(buf != NULL) *buf = debug_output ? '1' : '0';
	return 1;
}

static ssize_t store_debug_output(struct device_driver *drv, const char *buf, size_t count)
{
	if(count != 1 || buf == NULL) return -EINVAL;
	if(*buf == '0')
	{
		debug_output = 0;
		return 1;
	}
	else if(*buf == '1')
	{
		debug_output = 1;
		return 1;
	}
	return -EINVAL;
}

static DRIVER_ATTR(debug_output, S_IRUSR | S_IWUSR, show_debug_output, store_debug_output);
#endif

static int __init init(void)
{
	int	retval;

	if(usb_disabled()) return -ENODEV;

	vhci_printk(KERN_INFO, DRIVER_DESC "  Ver. " DRIVER_VERSION "\n");

#ifdef DEBUG
	vhci_printk(KERN_DEBUG, "register platform_driver %s\n", driver_name);
#endif
	retval = platform_driver_register(&vhci_hcd_driver);
	if(unlikely(retval < 0))
	{
		vhci_printk(KERN_ERR, "register platform_driver failed\n");
		return retval;
	}

	retval = register_chrdev(VHCI_HCD_MAJOR_NUM, driver_name, &fops);
	if(unlikely(retval < 0))
	{
		vhci_printk(KERN_ERR, "Sorry, registering the character device failed with %d.\n", retval);
#ifdef DEBUG
		vhci_printk(KERN_DEBUG, "unregister platform_driver %s\n", driver_name);
#endif
		platform_driver_unregister(&vhci_hcd_driver);
		return retval;
	}

	vhci_printk(KERN_INFO, "Successfully registered the character device.\n");
	vhci_printk(KERN_INFO, "The major device number is %d.\n", VHCI_HCD_MAJOR_NUM);

#ifdef DEBUG
	retval = driver_create_file(&vhci_hcd_driver.driver, &driver_attr_debug_output);
	if(unlikely(retval != 0))
	{
		vhci_printk(KERN_DEBUG, "driver_create_file(&vhci_hcd_driver, &driver_attr_debug_output) failed\n");
		vhci_printk(KERN_DEBUG, "==> ignoring\n");
	}
#endif

	return 0;
}
module_init(init);

static void __exit cleanup(void)
{
#ifdef DEBUG
	driver_remove_file(&vhci_hcd_driver.driver, &driver_attr_debug_output);
#endif
	unregister_chrdev(VHCI_HCD_MAJOR_NUM, driver_name);
	vhci_dbg("unregister platform_driver %s\n", driver_name);
	platform_driver_unregister(&vhci_hcd_driver);
	vhci_dbg("bin weg\n");
}
module_exit(cleanup);
