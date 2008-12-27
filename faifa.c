/*
 * faifa.c - core library functions
 *
 * Copyright (C) 2007-2008 
 *	Xavier Carcelle <xavier.carcelle@gmail.com>
 *	Florian Fainelli <florian@openwrt.org>
 *	Nicolas Thill <nico@openwrt.org>
 *
 * License:
 *	GPLv2
 */

#include <sys/types.h>
#ifndef __CYGWIN__
#include <net/ethernet.h>
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "faifa.h"
#include "faifa_compat.h"
#include "faifa_priv.h"
#include "homeplug_av.h"

extern struct hpav_frame_ops hpav_frame_ops[64];

void faifa_set_error(faifa_t *faifa, char *format, ...)
{
	va_list ap;

	if (!faifa)
		return;

	va_start(ap, format);
	vsnprintf(faifa->error, sizeof(faifa->error), format, ap);
	va_end(ap);
}


faifa_t *faifa_init(void)
{
	faifa_t *faifa;

	faifa = calloc(1, sizeof(faifa_t));
	if (faifa == NULL)
		goto __error_malloc;
	
	return faifa;

	free(faifa);
__error_malloc:
	return NULL;
}


void faifa_free(faifa_t *faifa)
{
	free(faifa);
}


char *faifa_error(faifa_t *faifa)
{
	if (faifa)
		return faifa->error;
	else
		return NULL;
}


int faifa_open(faifa_t *faifa, char *name)
{
	char pcap_errbuf[PCAP_ERRBUF_SIZE];
	int pcap_snaplen = ETHER_MAX_LEN;

#ifndef __CYGWIN__
	if (getuid() > 0) {
		faifa_set_error(faifa, "Must be root to execute this program");
		goto __error_pcap_lookupdev;
	}
#endif

	if (!pcap_lookupdev(name)) {
		faifa_set_error(faifa, "pcap_lookupdev: can't find device %s", name);
		goto __error_pcap_lookupdev;
	}

	faifa->pcap = pcap_open_live(name, pcap_snaplen , 1, 100, pcap_errbuf);
	if (faifa->pcap == NULL) {
		faifa_set_error(faifa, "pcap_open_live: %s", pcap_errbuf);
		goto __error_pcap_open_live;
	}

	if (pcap_datalink(faifa->pcap) != DLT_EN10MB) {
		faifa_set_error(faifa, "pcap: device %s is not Ethernet", name);
		goto __error_device_not_ethernet;
	}

	strncpy(faifa->ifname, name, sizeof(faifa->ifname));
	
	return 0;
	
__error_device_not_ethernet:
	pcap_close(faifa->pcap);
__error_pcap_open_live:
__error_pcap_lookupdev:
	return -1;
}


int faifa_recv(faifa_t *faifa, void *buf, int len)
{
	struct pcap_pkthdr *pcap_header;
	u_char *pcap_data;
	int n;
	
	n = pcap_next_ex(faifa->pcap, &pcap_header, (const u_char **)&pcap_data);
	if (n < 0) {
		faifa_set_error(faifa, "pcap_next_ex: %s", pcap_geterr(faifa->pcap));
		return -1;
	}
	if (n == 0 )
		return 0;

	if ((u_int32_t)len > (u_int32_t)(pcap_header->caplen))
		len = pcap_header->caplen;

	memcpy(buf, pcap_data, len);
	
	return len;
}


int faifa_send(faifa_t *faifa, void *buf, int len)
{
	int n;

	n = pcap_sendpacket(faifa->pcap, buf, len);
	if (n == -1) {
		faifa_set_error(faifa, "pcap_inject: %s", pcap_geterr(faifa->pcap));
	}

	return n;
}


typedef struct faifa_loop_data {
	faifa_t *faifa;
	faifa_loop_handler_t handler;
	void *user;
} faifa_loop_data_t;

void faifa_loop_handler(faifa_loop_data_t *loop_data, struct pcap_pkthdr *pcap_header, void *pcap_data)
{
	loop_data->handler(loop_data->faifa, pcap_data, pcap_header->caplen, loop_data->user);
}

int faifa_loop(faifa_t *faifa, faifa_loop_handler_t handler, void *user)
{
	faifa_loop_data_t loop_data = { faifa, handler, user };
	int n;

	n = pcap_loop(faifa->pcap, -1, (void *)faifa_loop_handler, (u_char *)&loop_data);
	if (n == -1) {
		faifa_set_error(faifa, "pcap_loop: %s", pcap_geterr(faifa->pcap));
	}

	return n;
}


int faifa_close(faifa_t *faifa)
{
	pcap_close(faifa->pcap);
	memset(faifa->ifname, '\0', sizeof(faifa->ifname));

	return 0;
}


int faifa_sprint_hex(char *str, void *buf, int len, char *sep)
{
	int avail = len;
	u_int8_t *pbuf = buf;
	char *pstr = str;

	while (avail > 0) {
		pstr += sprintf((char *)pstr, "%02hX%s", (unsigned short int)*pbuf, (avail > 1) ? sep : "");
		pbuf++;
		avail--;
	}

	return (pstr - str);
}


#ifdef __IS_THIS_REALLY_NEEDED__

int faifa_get_hwaddr(faifa_t *faifa, u_int8_t *hwaddr)
{
	int fd;
	struct ifreq ifr;

	fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd == -1) {
		faifa_set_error(faifa, "socket: %s", strerror(errno));
		goto __error_socket;
	}

	strncpy(ifr.ifr_name, faifa->ifname, sizeof(ifr.ifr_name) - 1);
	ifr.ifr_name[sizeof(ifr.ifr_name) - 1] = '\0';

	if (ioctl(fd, SIOCGIFHWADDR, &ifr) == -1) {
		faifa_set_error(faifa, "ioctl: %s", strerror(errno));
		goto __error_ioctl;
	}
	memcpy(hwaddr, &(ifr.ifr_hwaddr), ETHER_ADDR_LEN);

	close(fd);

	return 0;

__error_ioctl:
	close(fd);
__error_socket:
	return -1;
}

#endif /* __IS_THIS_REALLY_NEEDED__ */
