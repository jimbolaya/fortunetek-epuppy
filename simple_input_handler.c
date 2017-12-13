#include <linux/config.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/module.h>
static struct input_handler printdev_handler;
void printdev_event(struct input_handle *handle, unsigned int type,
		unsigned int code, int down)
{
	printk(KERN_INFO "device: %p, type: %d, code: %d, value: %d",
			handle->dev, type, code, value);
}
static struct input_handle *printdev_connect(struct input_handler *handler,
		struct input_dev *dev)
{
	struct input_handle *handle;
	if (!test_bit(EV_KEY, dev->evbit))
		return NULL;
	if (!(handle = kmalloc(sizeof(struct input_handle), GFP_KERNEL)))
		return NULL;
	memset(handle, 0, sizeof(struct input_handle));
	handle->dev = dev;
	handle->handler = handler;
	input_open_device(handle);
	return handle;
}
static void printdev_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	kfree(handle);
}
static struct input_handler printdev_handler = {
event: printdev_event,
       connect: printdev_connect,
       disconnect: printdev_disconnect,
};
static int __init printdev_init(void)
{
	input_register_handler(&printdev_handler);
	return 0;
}
static void __exit printdev_exit(void)
{
	input_unregister_handler(&printdev_handler);
}
module_init(printdev_init);
module_exit(printdev_exit);
