/*
Cubesat Space Protocol - A small network-layer protocol designed for Cubesats
Copyright (C) 2012 GomSpace ApS (http://www.gomspace.com)
Copyright (C) 2012 AAUSAT3 Project (http://aausat3.space.aau.dk)

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/* SocketCAN driver */

#include <stdint.h>
#include <stdio.h>

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>

#include <pthread.h>
#include <semaphore.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/queue.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <net/if.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/socket.h>
#include <bits/socket.h>

#include <csp/csp.h>
#include <csp/interfaces/csp_if_can.h>
#include <csp/drivers/can.h>

#ifdef CSP_HAVE_LIBSOCKETCAN
#include <libsocketcan.h>
#endif

#define MAX_SUPPORTED_CAN_INSTANCES 3

typedef struct csp_can_socket_t {
	int can_socket;         /** SocketCAN socket handle */
	csp_iface_t *csp_if_can;
} csp_can_instance_t;

static csp_can_instance_t can_instances[MAX_SUPPORTED_CAN_INSTANCES]; /** SocketCAN socket handles */
static size_t total_interfaces = 0;

static csp_can_instance_t * get_available_can_instance(void)
{
	csp_can_instance_t *iface_instance = NULL;
	if(total_interfaces < MAX_SUPPORTED_CAN_INSTANCES) {
		iface_instance = &can_instances[total_interfaces];
		total_interfaces++;
	}
	return iface_instance;
}

static int get_interface_socket(const csp_iface_t * iface)
{
	for (size_t i=0; i<total_interfaces; ++i) {
		if (can_instances[i].csp_if_can == iface) {
			return can_instances[i].can_socket;
		}
	}
	return -1;
}

static void * socketcan_rx_thread(void * parameters)
{
	csp_can_frame_t csp_can_frame;
	struct can_frame *frame;
	int nbytes;

	csp_can_instance_t *can_instance = (csp_can_instance_t *) parameters;
	csp_can_frame.interface = can_instance->csp_if_can;
	frame = (struct can_frame*) &csp_can_frame;

	while (1) {
		/* Read CAN frame */
		nbytes = read(can_instance->can_socket, frame, sizeof(*frame));
		if (nbytes < 0) {
			csp_log_error("read: %s", strerror(errno));
			continue;
		}

		if (nbytes != sizeof(*frame)) {
			csp_log_warn("Read incomplete CAN frame");
			continue;
		}

		/* Frame type */
		if (frame->can_id & (CAN_ERR_FLAG | CAN_RTR_FLAG) || !(frame->can_id & CAN_EFF_FLAG)) {
			/* Drop error and remote frames */
			csp_log_warn("Discarding ERR/RTR/SFF frame");
			continue;
		}

		/* Strip flags */
		frame->can_id &= CAN_EFF_MASK;

		/* Call RX callback */
		csp_can_rx_frame(&csp_can_frame, NULL);
	}

	/* We should never reach this point */
	pthread_exit(NULL);
}

int can_send(csp_iface_t *csp_if_can, can_id_t id, uint8_t data[], uint8_t dlc)
{
	struct can_frame frame;
	int i, tries = 0;
	int can_socket;

	can_socket = get_interface_socket(csp_if_can);
	if (can_socket < 0)
		return -1;

	if (dlc > 8)
		return -1;

	/* Copy identifier */
	frame.can_id = id | CAN_EFF_FLAG;

	/* Copy data to frame */
	for (i = 0; i < dlc; i++)
		frame.data[i] = data[i];

	/* Set DLC */
	frame.can_dlc = dlc;

	/* Send frame */
	while (write(can_socket, &frame, sizeof(frame)) != sizeof(frame)) {
		if (++tries < 1000 && errno == ENOBUFS) {
			/* Wait 10 ms and try again */
			usleep(10000);
		} else {
			csp_log_error("write: %s", strerror(errno));
			break;
		}
	}

	return 0;
}

int can_init(csp_iface_t *csp_if_can, uint32_t id, uint32_t mask, struct csp_can_config *conf)
{
	struct ifreq ifr;
	struct sockaddr_can addr;
	pthread_t rx_thread;
	int *can_socket;
	csp_can_instance_t *iface_instance;

	iface_instance = get_available_can_instance();
	if (iface_instance == NULL) {
		return -1;
	}

	csp_if_can->driver = (void *)iface_instance;
	iface_instance->csp_if_can = csp_if_can;
	can_socket = &iface_instance->can_socket;


	csp_assert(conf && conf->ifc);

#ifdef CSP_HAVE_LIBSOCKETCAN
	/* Set interface up */
	if (conf->bitrate > 0) {
		can_do_stop(conf->ifc);
		can_set_bitrate(conf->ifc, conf->bitrate);
		can_do_start(conf->ifc);
	}
#endif

	/* Create socket */
	if ((*can_socket = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
		csp_log_error("socket: %s", strerror(errno));
		return -1;
	}

	/* Locate interface */
	strncpy(ifr.ifr_name, conf->ifc, IFNAMSIZ - 1);
	if (ioctl(*can_socket, SIOCGIFINDEX, &ifr) < 0) {
		csp_log_error("ioctl: %s", strerror(errno));
		return -1;
	}

	/* Bind the socket to CAN interface */
	addr.can_family = AF_CAN;
	addr.can_ifindex = ifr.ifr_ifindex;
	if (bind(*can_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		csp_log_error("bind: %s", strerror(errno));
		return -1;
	}

	/* Set promiscuous mode */
	if (mask) {
		struct can_filter filter;
		filter.can_id   = id;
		filter.can_mask = mask;
		if (setsockopt(*can_socket, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) < 0) {
			csp_log_error("setsockopt: %s", strerror(errno));
			return -1;
		}
	}

	/* Create receive thread */
	if (pthread_create(&rx_thread, NULL, socketcan_rx_thread, NULL) != 0) {
		csp_log_error("pthread_create: %s", strerror(errno));
		return -1;
	}

	return 0;
}
