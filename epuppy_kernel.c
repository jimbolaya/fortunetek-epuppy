/*
 * A driver for the FortuneTek ePuppy stuffed animal dog.
 *
 * v0.1, (c)2007 James Klaas <jklaas@appalachian.dyndns.org>
 *
 * This device is a stuffed animal that connects over USB. It has buttons on each paw
 * and one in it's head (for bopping).  It has a two color LED (red and green) in it's 
 * tummy that has a solid on state and a flash state.  Combining the two colors creates 
 * a third color, amber.  Each of the two LEDs can flash alternately or in tandem.
 * There is also an internal microphone and speaker.  The speaker can be turned on or off.
 *
 * The capabilities for this were determined through reverse engineering by sniffing
 * the USB messages in another operating system using USB sniffer drivers.
 *
 * The basis for this driver is William R Sowerbutts PowerMate driver.
 *
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/usb_input.h>


/* Dev #1: (05e3)FORTUNETEK (fd51) ePuppy */

#define USB_EPUPPY_VENDOR 0x05e3
#define USB_EPUPPY_PRODUCT 0xfd51

/* Set LED & speaker state
 *
 * requesttype = 64; -- Host to Device, Class Request, Device recipient
 * request = 4; -- Clear feature and Set feature?
 * value = 1089;
 * index = 0;
 * size = 2;
 * timeout = 500000;
 *
 */
/* buffer[1] - byte 1 */
#define RED 0x08
#define GREEN 0x04
#define SPEAKER 0x02

/*
 * 0x01 - Don't know - mic?
 * 0x00 - Don't know, I think off.
 */

/* buffer[0] - byte 0 */
#define BLINK 0x02
#define SOLID 0x01
#define OFF 0x00
#define SP_ON 0x00
#define SP_OFF 0x01

/* Read switch (paw/bop) state
 *
 * requesttype = 192; -- Device to Host, Class Request, Device recipient
 * request = 12; -- Get configuration and ?
 * value = 320;
 * index = 0;
 * size = 1;
 * timeout = 500000;
 *
 * Return values below:
 */ 

#define NO_PUSH 0x00
#define RIGHT_PAW 0x01
#define HEAD_BOP 0x02
#define LEFT_PAW 0x04
#define RIGHT_FOOT 0x08
#define LEFT_FOOT 0x10

#define EPUPPY_PAYLOAD_SIZE 0x01
#define UPDATE_SPEAKER 0x0F
#define UPDATE_LED 0xF0

struct epuppy_device {
	signed char *data;
	dma_addr_t data_dma;
	struct urb *irq, *config;
	struct usb_ctrlrequest *configcr;
	dma_addr_t configcr_dma;
	struct usb_device *udev;
	struct input_dev input;
	spinlock_t lock;
	int led_state; // OFF|SOLID|BLINK
	int led_color; // RED|GREEN|AMBER
	int speaker_state; //  SP_ON|SP_OFF
	int command_buffer[2]; // byte1 = led_state or speaker_state byte2 = led_color or speaker
	char phys[64];
};

static char ep_name_epuppy[] = "FortuneTek ePuppy";

static void epuppy_config_complete(struct urb *urb, struct pt_regs *regs);

/* Callback for data arriving from the PowerMate over the USB interrupt pipe */
static void epuppy_irq(struct urb *urb, struct pt_regs *regs)
{
	struct epuppy_device *ep = urb->context;
	int retval;

	switch (urb->status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dbg("%s - urb shutting down with status: %d", __FUNCTION__, urb->status);
		return;
	default:
		dbg("%s - nonzero urb status received: %d", __FUNCTION__, urb->status);
		goto exit;
	}

	/* handle updates to device state */
	input_regs(&ep->input, regs);
	input_report_key(&ep->input, BTN_0, ep->data[0] & 0x01);
	input_sync(&ep->input);

exit:
	retval = usb_submit_urb (urb, GFP_ATOMIC);
	if (retval)
		err ("%s - usb_submit_urb failed with result %d",
		     __FUNCTION__, retval);
}

/* Decide if we need to issue a control message and do so. Must be called with ep->lock taken */
static void epuppy_sync_state(struct epuppy_device *ep)
{
	int buffer[2];
	if (ep->requires_update == 0)
		return; /* no updates are required */
	if (ep->config->status == -EINPROGRESS)
		return; /* an update is already in progress; it'll issue this update when it completes */

	/* Setting the state goes through the transfer buffer and doesn't use the wValue which is static */

	if (ep->requires_update & UPDATE_SPEAKER){
		/* set requested state */
		buffer[0] = ep->speaker_state;
		buffer[1] = SPEAKER;
		ep->requires_update &= ~UPDATE_SPEAKER;
	} else if (ep->requires_update & UPDATE_LED) {
		buffer[0] |= ep->led_state;
		buffer[1] |= ep->led_color;
		ep->requires_update &= ~UPDATE_LED;
	}else{
		printk(KERN_ERR "epuppy: unknown update required");
		ep->requires_update = 0; /* fudge the bug */
		return;
	}


	ep->configcr->wValue = 0x441;
	ep->configcr->wIndex =  0x0;
	ep->configcr->bRequestType = 0x40;
	ep->configcr->bRequest = 0x01;
	ep->configcr->wLength = 0;

	usb_fill_control_urb(ep->config, ep->udev, usb_sndctrlpipe(ep->udev, 0),
			     (void *) ep->configcr, buffer, 2,
			     epuppy_config_complete, ep);
	ep->config->setup_dma = ep->configcr_dma;
	ep->config->transfer_flags |= URB_NO_SETUP_DMA_MAP;

	if (usb_submit_urb(ep->config, GFP_ATOMIC))
		printk(KERN_ERR "epuppy: usb_submit_urb(config) failed");
}

/* Called when our asynchronous control message completes. We may need to issue another immediately */
static void epuppy_config_complete(struct urb *urb, struct pt_regs *regs)
{
	struct epuppy_device *ep = urb->context;
	unsigned long flags;

	if (urb->status)
		printk(KERN_ERR "epuppy: config urb returned %d\n", urb->status);

	spin_lock_irqsave(&ep->lock, flags);
	epuppy_sync_state(ep);
	spin_unlock_irqrestore(&ep->lock, flags);
}

/* Set the LED up as described and begin the sync with the hardware if required */
static void epuppy_set_led_spk(struct epuppy_device *ep, int led_color, int led_state, int speaker_state)
{

	spin_lock_irqsave(&ep->lock, flags);

	/* mark state updates which are required */
	if ( led_color == SPEAKER ) { 
		ep->speaker_state = speaker_state; 
		ep->requires_update |= UPDATE_SPEAKER;
	} else { 
		ep->led_color = led_color; 
		ep->led_state = led_state; 
		ep->requires_update |= UPDATE_LED;
	}

	epuppy_sync_state(ep);

	spin_unlock_irqrestore(&ep->lock, flags);
}

/* Callback from the Input layer when an event arrives from userspace to configure the LED */
/* I.E. someone sends a command to the event device file */
static int epuppy_input_event(struct input_dev *dev, unsigned int type, unsigned int code, int _value)
{
	unsigned int command = (unsigned int)_value;
	int buffer[2];

	/* 
	 * -- LED STATES --
	 * BLINK 0x02
	 * SOLID 0x01
	 * OFF   0x00
	 * -- SPEAKER STATES --
	 * OFF 0x01
	 * ON  0x00
	 */

	/*
	 * -- LED COLOR --
	 * RED     0x08
	 * GREEN   0x04
	 * AMBER = RED + GREEN = 0x0c
	 *
	 * -- SPEAKER --
	 * SPEAKER 0x02
	 *
	 * Of course, this begs the question, since the LEDs can be specified more
	 * than one at a time, along with the speaker, should the LEDs be mutually
	 * exclusive from the speaker?  Especially since the speaker off command 
	 * is the same as the LED solid command?
	 *
	 */

 	 struct epuppy_device *ep = dev->private;



	if (type == EV_LED && code == LED_MISC){
		/* check current state and update as necessary */
		/* int led_state; // OFF|SOLID|BLINK - saved state
		 * int led_color; // RED|GREEN|AMBER - saved state
		 * int speaker_state; //  SP_ON|SP_OFF - saved state
		 * int command_buffer[2]; // byte1 = led_state or speaker_state byte2 = led_color or speaker - command state to send to the device
		 */

		int led_color = command & 0xFF; // RED|GREEN|AMBER or SPEAKER
		if ( led_color == SPEAKER ) {
			int speaker_state = (command >> 8) & 0xFF; //  SP_ON|SP_OFF
		} else {
			int led_state = (command >> 8) & 0xFF; // OFF|SOLID|BLINK
		}
	

		epuppy_set_led_spk(ep, led_color, led_state, speaker_state);
	}

	return 0;
}

static int epuppy_alloc_buffers(struct usb_device *udev, struct epuppy_device *ep)
{
	ep->data = usb_buffer_alloc(udev, EPUPPY_PAYLOAD_SIZE,
				    SLAB_ATOMIC, &ep->data_dma);
	if (!ep->data)
		return -1;
	ep->configcr = usb_buffer_alloc(udev, sizeof(*(ep->configcr)),
					SLAB_ATOMIC, &ep->configcr_dma);
	if (!ep->configcr)
		return -1;

	return 0;
}

static void epuppy_free_buffers(struct usb_device *udev, struct epuppy_device *ep)
{
	if (ep->data)
		usb_buffer_free(udev, EPUPPY_PAYLOAD_SIZE,
				ep->data, ep->data_dma);
	if (ep->configcr)
		usb_buffer_free(udev, sizeof(*(ep->configcr)),
				ep->configcr, ep->configcr_dma);
}

/* Called whenever a USB device matching one in our supported devices table is connected */
static int epuppy_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev (intf);
	struct usb_host_interface *interface;
	struct usb_endpoint_descriptor *endpoint;
	struct epuppy_device *ep;
	int pipe, maxp;
	char path[64];

	interface = intf->cur_altsetting;
	endpoint = &interface->endpoint[0].desc;
	if (!(endpoint->bEndpointAddress & 0x80))
		return -EIO;
	if ((endpoint->bmAttributes & 3) != 3)
		return -EIO;

	usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
		0x0a, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
		0, interface->desc.bInterfaceNumber, NULL, 0,
		USB_CTRL_SET_TIMEOUT);

	if (!(ep = kmalloc(sizeof(struct epuppy_device), GFP_KERNEL)))
		return -ENOMEM;

	memset(ep, 0, sizeof(struct epuppy_device));
	ep->udev = udev;

	if (epuppy_alloc_buffers(udev, ep)) {
		epuppy_free_buffers(udev, ep);
		kfree(ep);
		return -ENOMEM;
	}

	ep->irq = usb_alloc_urb(0, GFP_KERNEL);
	if (!ep->irq) {
		epuppy_free_buffers(udev, ep);
		kfree(ep);
		return -ENOMEM;
	}

	ep->config = usb_alloc_urb(0, GFP_KERNEL);
	if (!ep->config) {
		usb_free_urb(ep->irq);
		epuppy_free_buffers(udev, ep);
		kfree(ep);
		return -ENOMEM;
	}

	spin_lock_init(&ep->lock);
	init_input_dev(&ep->input);

	/* get a handle to the interrupt data pipe */
	pipe = usb_rcvintpipe(udev, endpoint->bEndpointAddress);
	maxp = usb_maxpacket(udev, pipe, usb_pipeout(pipe));

	if(maxp < EPUPPY_PAYLOAD_SIZE){
		printk("epuppy: Expected payload of %d bytes, found %d bytes!\n",
			EPUPPY_PAYLOAD_SIZE, maxp);
		maxp = EPUPPY_PAYLOAD_SIZE;
	}

	usb_fill_int_urb(ep->irq, udev, pipe, ep->data,
			 maxp, epuppy_irq,
			 ep, endpoint->bInterval);
	ep->irq->transfer_dma = ep->data_dma;
	ep->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	/* register our interrupt URB with the USB system */
	if (usb_submit_urb(ep->irq, GFP_KERNEL)) {
		epuppy_free_buffers(udev, ep);
		kfree(ep);
		return -EIO; /* failure */
	}

	/* register input capabilities with the kernel */

	ep->input.name = ep_name_epuppy; 
	if (le16_to_cpu(udev->descriptor.idProduct) != USB_EPUPPY_PRODUCT) {
		printk(KERN_WARNING "epuppy: unknown product id %04x\n",
		       le16_to_cpu(udev->descriptor.idProduct));
	}

	ep->input.private = ep;
	ep->input.evbit[0] = BIT(EV_KEY);
	ep->input.keybit[LONG(BTN_0)] = BIT(BTN_0);
	usb_to_input_id(udev, &ep->input.id);
	/* callback when an event occurs */
	ep->input.event = epuppy_input_event;
	ep->input.dev = &intf->dev;
	ep->input.phys = ep->phys;

	input_register_device(&ep->input);

	usb_make_path(udev, path, 64);
	snprintf(ep->phys, 64, "%s/input0", path);
	printk(KERN_INFO "input: %s on %s\n", ep->input.name, ep->input.phys);

	/* force an update of everything */
	ep->requires_update = 0x00 ;
	epuppy_set_led_spk(ep, 0x00, 0x00 ); // set default switch parameters

	usb_set_intfdata(intf, ep);
	return 0;
}

/* Called when a USB device we've accepted ownership of is removed */
static void epuppy_disconnect(struct usb_interface *intf)
{
	struct epuppy_device *ep = usb_get_intfdata (intf);

	usb_set_intfdata(intf, NULL);
	if (ep) {
		ep->requires_update = 0;
		usb_kill_urb(ep->irq);
		input_unregister_device(&ep->input);
		usb_free_urb(ep->irq);
		usb_free_urb(ep->config);
		epuppy_free_buffers(interface_to_usbdev(intf), ep);

		kfree(ep);
	}
}

static struct usb_device_id epuppy_devices [] = {
	{ USB_DEVICE(EPUPPY_VENDOR, EPUPPY_PRODUCT) },
	{ } /* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, epuppy_devices);

static struct usb_driver epuppy_driver = {
	.owner =	THIS_MODULE,
        .name =         "epuppy",
        .probe =        epuppy_probe,
        .disconnect =   epuppy_disconnect,
        .id_table =     epuppy_devices,
};

static int __init epuppy_init(void)
{
	return usb_register(&epuppy_driver);
}

static void __exit epuppy_cleanup(void)
{
	usb_deregister(&epuppy_driver);
}

module_init(epuppy_init);
module_exit(epuppy_cleanup);

MODULE_AUTHOR( "James T Klaas" );
MODULE_DESCRIPTION( "FortuneTek ePuppy driver" );
MODULE_LICENSE("GPL");
