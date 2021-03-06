/*
 * testepuppy.c
 *
 *  Test program for the Fortunetek E-Puppy
 *  Modified from the testlibusb.c and wireless usb lock
 *  Released under the GPL.
 *  James Klaas (jklaas_at_gmail.com)
 *
 */

#include <stdio.h>
#include <string.h>
#include <usb.h>
#include <string.h>
#include <asm/errno.h>

/* Dev #1: (05e3)FORTUNETEK (fd51) ePuppy */

#define USB_EPUPPY_VENDOR 0x05e3
#define USB_EPUPPY_PRODUCT 0xfd51

/* Set LED & speaker state
 *
 * requesttype = 64;
 * request = 4;
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
 * 0x01 - Don't know 
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
 * requesttype = 192;
 * request = 12;
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

typedef struct {
   int requesttype;
   int request;
   int value;
   int index;
   int size;
   int timeout;
} my_request_values ;


#define MAX_USB_EPUPPYS 10
struct usb_device* usb_epuppys[MAX_USB_EPUPPYS];
int epuppysFound = 0;

int verbose = 0;

void usage ( void ) {
	printf("testepuppy2 [-speaker [on|off]] [-red|green|amber [solid|blink|off]] [-read] \n");
	return;
}

void do_epuppy(struct usb_device *dev, char *buffer, my_request_values for_request) {
	usb_dev_handle *epuppy_device = 0;
	int ret = 0;
	int i, j ;
	int requesttype, request, value, index, size, timeout;

/*	printf("Doing epuppy\n"); */
	if(dev->descriptor.idVendor != USB_EPUPPY_VENDOR ||
	   dev->descriptor.idProduct != USB_EPUPPY_PRODUCT) {
	  printf("ERR: Wasn't passed a usbepuppy!\n");
	  return;
	}

	epuppy_device = usb_open(dev);
	if(epuppy_device > 0) {
	  ret = usb_claim_interface(epuppy_device, 0);
	  if(ret < 0) {
            printf("Failed to claim interface 0: %d EBUS: %d ENONM: %d\n", ret, EBUSY, ENOMEM);
	    goto bail;
          }

	  requesttype = for_request.requesttype;
	  request = for_request.request;
	  value = for_request.value;
	  index = for_request.index;
	  size = for_request.size;
	  timeout = for_request.timeout;


	  i = 1000;
	  while(i--) {
		  ret = usb_control_msg(epuppy_device, requesttype, request, value, index, buffer, size, timeout);
	  if(ret < 0) {
		  printf("Failure usb_interrupt_read  %d '%s'\n", ret, strerror(-ret));
		  goto bail;
	  } 

bail:
	  usb_close(epuppy_device);
          return ;
	}
      }
}

void found_usb_epuppy(struct usb_device *dev) {
  if(epuppysFound < MAX_USB_EPUPPYS) {
    usb_epuppys[epuppysFound++] = dev;
    if(verbose) printf("Found usb epuppy fob #%d\n", epuppysFound);
  }
}

int main(int argc, char *argv[])
{
  struct usb_bus *bus;
  int bus_changes = 0;
  int dev_changes = 0;
  int request = 0;
  int doread = 0;
  int dowrite = 0;
  int getarg;
  int i;
  char *pbuffer;
  char read_buffer = 0;
  char write_buffer[2];
  write_buffer[0] = 0;
  write_buffer[1] = 0; 
  my_request_values datasend;

  usb_init();

  bus_changes = usb_find_busses();
  if(verbose) printf("Bus changes: %d\n", bus_changes);
  dev_changes = usb_find_devices();
  if(verbose) printf("Dev changes: %d\n", dev_changes);

  for (bus = usb_busses; bus; bus = bus->next) {
      struct usb_device *dev;
      for (dev = bus->devices; dev; dev = dev->next) {
        if(dev->descriptor.idVendor == USB_EPUPPY_VENDOR &&
           dev->descriptor.idProduct == USB_EPUPPY_PRODUCT) {
          found_usb_epuppy(dev);
        }
    }
  }

  if (argc > 1  ) {
    for (getarg = 1; getarg < argc; getarg ++) {
      if ( !strcmp(argv[getarg], "-v")) {
        verbose = 1;
      } else if ( !strcmp(argv[getarg], "-read")) {
         if ( dowrite != 1 ) {
           doread = 1;

           datasend.requesttype = 192;
           datasend.request = 12;
           datasend.value = 320;
           datasend.index = 0;
           datasend.size = 1;
           datasend.timeout = 500000;
           pbuffer = &read_buffer;
         }
      } else {
          if ( doread != 1 ) {
            dowrite = 1;
            if ( !strcmp(argv[getarg], "-speaker")) {
              write_buffer[1] |= SPEAKER;
              getarg ++;
              if ( !strcmp(argv[getarg], "on")) {
                write_buffer[0] &= SP_ON ;
              } else if ( !strcmp(argv[getarg], "off")) {
                write_buffer[0] |= SP_OFF ;
              } else {
                printf ("%s is not a valid option.\n", argv[getarg] );
		usage();
		exit(1);
              }
            } else if ( !strcmp(argv[getarg], "-red")) {
              write_buffer[1] |= RED;
              getarg ++;
              if ( !strcmp(argv[getarg], "blink")) {
                write_buffer[0] |= BLINK ;
              } else if ( !strcmp(argv[getarg], "solid")) {
                write_buffer[0] |= SOLID ;
              } else if ( !strcmp(argv[getarg], "off")) {
                write_buffer[0] &= OFF ;
              } else {
                printf ("%s is not a valid option.\n", argv[getarg] );
		usage();
		exit(2);
              }
            } else if ( !strcmp(argv[getarg], "-green")) {
              write_buffer[1] |= GREEN;
              getarg ++;
              if ( !strcmp(argv[getarg], "blink")) {
                write_buffer[0] |= BLINK ;
              } else if ( !strcmp(argv[getarg], "solid")) {
                write_buffer[0] |= SOLID ;
              } else if ( !strcmp(argv[getarg], "off")) {
                write_buffer[0] &= OFF ;
              } else {
                printf ("%s is not a valid option.\n", argv[getarg] );
		usage();
		exit(3);
              }
            } else if ( !strcmp(argv[getarg], "-amber")) {
              write_buffer[1] |= ( RED + GREEN );
              getarg ++;
              if ( !strcmp(argv[getarg], "blink")) {
                write_buffer[0] |= BLINK ;
              } else if ( !strcmp(argv[getarg], "solid")) {
                write_buffer[0] |= SOLID ;
              } else if ( !strcmp(argv[getarg], "off")) {
                write_buffer[0] &= OFF ;
              } else {
                printf ("%s is not a valid option.\n", argv[getarg] );
		usage();
		exit(4);
              }
            } else {
              printf ("%s is not a valid option.\n", argv[getarg] );
	      usage();
	      exit(5);
            }

	    pbuffer = write_buffer;

            datasend.requesttype = 64;
            datasend.request = 4;
            datasend.value = 1089;
            datasend.index = 0;
            datasend.size = 2;
            datasend.timeout = 500000;

         }
       }
     }
  } else {

     usage();
     exit(6);
  }

  for(i = 0; i < epuppysFound; i++) {
    do_epuppy(usb_epuppys[i], pbuffer, datasend);
    if (doread == 1) {
      read_buffer = *pbuffer;
/*       printf ( "%x \n", read_buffer );  */
      if ( read_buffer & LEFT_PAW) {
        printf ( "The Left Paw was pushed.\n" );
      }
      if ( read_buffer & RIGHT_PAW) {
        printf ( "The Right Paw was pushed.\n" );
      }
      if ( read_buffer & LEFT_FOOT) {
        printf ( "The Left Foot was pushed.\n" );
      }
      if ( read_buffer & RIGHT_FOOT) {
        printf ( "The Right Foot was pushed.\n" );
      }
      if ( read_buffer & HEAD_BOP) {
        printf ( "The Head was Bopped.\n" );
      }
      if (read_buffer == 0 ) {
        printf ( "No switches triggered.\n" );
      }
    } 
  } 

  exit (0) ;
}
