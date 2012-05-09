/*
**  smcroute - static multicast routing control 
**  Copyright (C) 2001-2005 Carsten Schill <carsten@cschill.de>
**  Copyright (C) 2006-2009 Julien BLACHE <jb@jblache.org>
**  Copyright (C) 2009      Todd Hayton <todd.hayton@gmail.com>
**  Copyright (C) 2009-2011 Micha Lenk <micha@debian.org>
**
**  This program is free software; you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation; either version 2 of the License, or
**  (at your option) any later version.
**
**  This program is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with this program; if not, write to the Free Software
**  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
**
**  $Id: mroute-api.c 85 2011-08-08 16:47:53Z micha $	
**
**  This module contains the interface routines to the Linux mrouted API
**
*/
#include <unistd.h>
#include <arpa/inet.h>
#include "config.h"
#include "mclab.h"

#ifdef HAVE_NETINET6_IP6_MROUTE_H
#include <netinet6/ip6_mroute.h>
#endif

/* MAX_MC_VIFS from mclab.h must have same value as MAXVIFS from mroute.h */
#if MAX_MC_VIFS != MAXVIFS
#error "constants don't match, correct mclab.h"
#endif

#ifdef HAVE_IPV6_MULTICAST_ROUTING
/* MAX_MC_MIFS from mclab.h must have same value as MAXVIFS from mroute6.h */
#if MAX_MC_MIFS != MAXMIFS
#error "constants don't match, correct mclab.h"
#endif
#endif

/*
 * Need a raw IGMP socket as interface for the IPv4 mrouted API
 * Receives IGMP packets and kernel upcall messages.
 */
int mroute4_socket = -1;

/* All user added/configured (*,G) routes that are matched on-demand
 * at runtime. See the mroute4_dyn_list for the actual (S,G) routes
 * set from this "template". */
LIST_HEAD(, mroute4) mroute4_conf_list = LIST_HEAD_INITIALIZER();

/* For dynamically/on-demand set (S,G) routes that we must track
 * if the user removes the configured (*,G) route. */
LIST_HEAD(, mroute4) mroute4_dyn_list = LIST_HEAD_INITIALIZER();

#ifdef HAVE_IPV6_MULTICAST_ROUTING
/*
 * Need a raw ICMPv6 socket as interface for the IPv6 mrouted API
 * Receives MLD packets and kernel upcall messages.
 */
int mroute6_socket = -1;
#endif

/* IPv4 internal virtual interfaces (VIF) descriptor vector */
static struct vif {
	struct iface *iface;
} vif_list[MAXVIFS];

static void mroute4_add_vif(struct iface *iface);

#ifdef HAVE_IPV6_MULTICAST_ROUTING
/* IPv6 internal virtual interfaces (VIF) descriptor vector */
static struct mif {
	struct iface *iface;
} mif_list[MAXMIFS];

static void mroute6_add_mif(struct iface *iface);
#endif

/*
** Initialise the mrouted API and locks the multicast routing
** socket to this program (only!).
**     
** returns: - 0 if the functions succeeds     
**          - the errno value for non-fatal failure condition
*/
int mroute4_enable(void)
{
	int arg = 1;
	unsigned int i;
	struct iface *iface;

	mroute4_socket = socket(AF_INET, SOCK_RAW, IPPROTO_IGMP);
	if (mroute4_socket < 0) {
		if (ENOPROTOOPT == errno)
			smclog(LOG_WARNING, 0, "Kernel does not support IPv4 multicast routing, skipping...");

		return -1;
	}

	if (setsockopt(mroute4_socket, IPPROTO_IP, MRT_INIT, (void *)&arg, sizeof(arg))) {
		switch (errno) {
		case EADDRINUSE:
			smclog(LOG_INIT, errno, "IPv4 multicast routing API already in use");
			break;

#ifdef EOPNOTSUPP
		case EOPNOTSUPP:
			smclog(LOG_INIT, errno, "Unknown socket option MRT_INIT");
			break;
#endif

		default:
			smclog(LOG_INIT, errno, "Failed initializing IPv4 multicast routing API");
			break;
		}

		close(mroute4_socket);
		mroute4_socket = -1;

		return -1;
	}

	/* Initialize virtual interface table */
	memset(&vif_list, 0, sizeof(vif_list));

	/* create VIFs for all IP, non-loop interfaces */
	for (i = 0; (iface = iface_find_by_index(i)); i++) {
		if (iface->flags & IFF_LOOPBACK) {
			iface->vif = -1;
			continue;
		}
		mroute4_add_vif(iface);
	}

	LIST_INIT(&mroute4_conf_list);
	LIST_INIT(&mroute4_dyn_list);

	return 0;
}

/*
** Diable the mrouted API and relase the kernel lock.
*/
void mroute4_disable(void)
{
	mroute4_t *entry;

	if (mroute4_socket < 0)
		return;

	/* Drop all kernel routes set by smcroute */
	if (setsockopt(mroute4_socket, IPPROTO_IP, MRT_DONE, NULL, 0))
		smclog(LOG_ERR, errno, "MRT_DONE");

	close(mroute4_socket);
	mroute4_socket = -1;

	/* Free list of (*,G) routes on SIGHUP */
	LIST_FOREACH(entry, &mroute4_conf_list, link) {
		LIST_REMOVE(entry, link);
		free(entry);
	}
	LIST_FOREACH(entry, &mroute4_dyn_list, link) {
		LIST_REMOVE(entry, link);
		free(entry);
	}
}


/*
** Adds the interface '*iface' as virtual interface to the mrouted API
*/
static void mroute4_add_vif(struct iface *iface)
{
	struct vifctl vc;
	int vif = -1;
	char buf[16];
	size_t i;

	/* search free vif */
	for (i = 0; i < ARRAY_ELEMENTS(vif_list); i++) {
		if (!vif_list[i].iface) {
			vif = i;
			break;
		}
	}

	/* no more space */
	if (vif == -1) {
		smclog(LOG_ERR, ENOMEM, "%s: out of VIF space", __FUNCTION__);
		return;
	}

	memset(&vc, 0, sizeof(vc));
	vc.vifc_vifi = vif;
	vc.vifc_flags = 0;      /* no tunnel, no source routing, register ? */
	vc.vifc_threshold = 1;  /* Packet TTL must be at least 1 to pass them */
	vc.vifc_rate_limit = 0;	/* hopefully no limit */
	vc.vifc_lcl_addr.s_addr = iface->inaddr.s_addr;
	vc.vifc_rmt_addr.s_addr = INADDR_ANY;

	smclog(LOG_NOTICE, 0, "Add VIF: %d Ifindex: %d Flags: 0x%04x IP: %s Ifname: %s",
	       vc.vifc_vifi, iface->ifindex, vc.vifc_flags,
	       inet_ntop (AF_INET, &vc.vifc_lcl_addr, buf, sizeof(buf)),
	       iface->name);

	if (setsockopt(mroute4_socket, IPPROTO_IP, MRT_ADD_VIF, (void *)&vc, sizeof(vc))) {
		smclog(LOG_ERR, errno, "MRT_ADD_VIF %s", iface->name);
	} else {
		iface->vif = vif;
		vif_list[vif].iface = iface;
	}
}

/* Actually set in kernel - called by mroute4_add() and mroute4_check_add() */
static int __mroute4_add (mroute4_t *ptr)
{
	int result = 0;
	char origin[INET_ADDRSTRLEN], group[INET_ADDRSTRLEN];
	struct mfcctl mc;

	memset(&mc, 0, sizeof(mc));

	mc.mfcc_origin = ptr->sender;
	mc.mfcc_mcastgrp = ptr->group;
	mc.mfcc_parent = ptr->inbound;

	/* copy the TTL vector */
	if (sizeof(mc.mfcc_ttls) != sizeof(ptr->ttl) || ARRAY_ELEMENTS(mc.mfcc_ttls) != ARRAY_ELEMENTS(ptr->ttl))
		smclog(LOG_ERR, 0, "Data types does not match in %s, source adaption needed!", __FILE__);

	memcpy(mc.mfcc_ttls, ptr->ttl, sizeof(mc.mfcc_ttls));

	smclog(LOG_NOTICE, 0, "Add MFC: %s -> %s, inbound VIF: %d",
	       inet_ntop(AF_INET, &mc.mfcc_origin,   origin, INET_ADDRSTRLEN),
	       inet_ntop(AF_INET, &mc.mfcc_mcastgrp, group,  INET_ADDRSTRLEN), mc.mfcc_parent);

	if (setsockopt(mroute4_socket, IPPROTO_IP, MRT_ADD_MFC, (void *)&mc, sizeof(mc))) {
		result = errno;
		smclog(LOG_WARNING, errno, "MRT_ADD_MFC");
	}

	return result;
}

/* Actually remove from kernel - called by mroute4_del() */
static int __mroute4_del (mroute4_t *ptr)
{
	int result = 0;
	char origin[INET_ADDRSTRLEN], group[INET_ADDRSTRLEN];
	struct mfcctl mc;

	memset(&mc, 0, sizeof(mc));
	mc.mfcc_origin = ptr->sender;
	mc.mfcc_mcastgrp = ptr->group;

	smclog(LOG_NOTICE, 0, "Del MFC: %s -> %s",
	       inet_ntop(AF_INET, &mc.mfcc_origin,  origin, INET_ADDRSTRLEN),
	       inet_ntop(AF_INET, &mc.mfcc_mcastgrp, group, INET_ADDRSTRLEN));

	if (setsockopt(mroute4_socket, IPPROTO_IP, MRT_DEL_MFC, (void *)&mc, sizeof(mc))) {
		result = errno;
		smclog(LOG_WARNING, errno, "MRT_DEL_MFC");
	}

	return result;
}

/*
** Add mcroute to kernel if it matches a known (*,G) route.
**
** returns: - 0 if the function succeeds
**          - the errno value for non-fatal failure condition
*/
int mroute4_dyn_add(mroute4_t *ptr)
{
	mroute4_t *entry;

	LIST_FOREACH(entry, &mroute4_conf_list, link) {
		/* Find matching (*,G) ... and interface. */
		if (!memcmp (&entry->group, &ptr->group, sizeof(entry->group)) && entry->inbound == ptr->inbound) {
			smclog(LOG_DEBUG, 0, "Found (*,G) match for (0x%x, 0x%x)!", ptr->sender.s_addr, ptr->group.s_addr);

			/* Add to list of dynamically added routes. Necessary if the user
			 * removes the (*,G) using the command line interface rather than
			 * updating the conf file and SIGHUP. Note: if we fail to alloc()
			 * memory we don't do anything, just add kernel route silently. */
			entry = malloc(sizeof(mroute4_t));
			if (entry) {
				memcpy(entry, ptr, sizeof(mroute4_t));
				LIST_INSERT_HEAD(&mroute4_dyn_list, entry, link);
			}

			return __mroute4_add(ptr);
		}
	}

	smclog(LOG_DEBUG, 0, "No (*,G) match for (0x%x, 0x%x)!", ptr->sender.s_addr, ptr->group.s_addr);

	errno = ENOENT;
	return -1;
}

/*
** Adds the multicast route '*ptr' to the kernel multicast routing table
** unless the source IP is INADDR_ANY, i.e., a (*,G) route. Those we save
** for later and check against at runtime when the kernel signals us.
**
** returns: - 0 if the function succeeds
**          - the errno value for non-fatal failure condition
*/
int mroute4_add(mroute4_t *ptr)
{
	/* For (*,G) we save to a linked list to be added on-demand
	 * when the kernel sends IGMPMSG_NOCACHE. */
	if (ptr->sender.s_addr == INADDR_ANY) {
		mroute4_t *entry = malloc(sizeof(mroute4_t));

		if (!entry) {
			smclog(LOG_WARNING, errno, "Failed allocating (*,G) entry to linked list");
			return errno;
		}

		memcpy(entry, ptr, sizeof(mroute4_t));
		smclog(LOG_DEBUG, 0, "Adding (*,G) mroute to dynamic list => (0x%x, 0x%x) vif:%d ",
		       ptr->sender.s_addr, ptr->group.s_addr, ptr->inbound);
		LIST_INSERT_HEAD(&mroute4_conf_list, entry, link);

		return 0;
	}

	return __mroute4_add (ptr);
}

/*
** Removes the multicast routed '*ptr' from the kernel routes
**
** returns: - 0 if the function succeeds
**          - the errno value for non-fatal failure condition
*/
int mroute4_del(mroute4_t *ptr)
{
	/* For (*,G) we have saved all dynamically added kernel routes
	 * to a linked list which we need to traverse again and remove
	 * all matches. From kernel dyn list before we remove the conf
	 * entry. */
	if (ptr->sender.s_addr == INADDR_ANY) {
		mroute4_t *entry;

		LIST_FOREACH(entry, &mroute4_conf_list, link) {
			/* Find matching (*,G) ... and interface. */
			if (!memcmp (&entry->group, &ptr->group, sizeof(entry->group)) && entry->inbound == ptr->inbound) {
				mroute4_t *set;

				smclog(LOG_DEBUG, 0, "Found (*,G) match for (0x%x, 0x%x) - now find any set routes!", ptr->sender.s_addr, ptr->group.s_addr);
				LIST_FOREACH(set, &mroute4_dyn_list, link) {
					if (!memcmp (&entry->group, &set->group, sizeof(entry->group)) && entry->inbound == set->inbound) {
						smclog(LOG_DEBUG, 0, "Found match (0x%x, 0x%x) - removing, unlinking and freeing.", set->sender.s_addr, set->group.s_addr);
						__mroute4_del(set);
						LIST_REMOVE(set, link);
						free(set);
					}
				}

				LIST_REMOVE(entry, link);
				free(entry);
			}
		}
	}

	return __mroute4_del(ptr);
}


#ifdef HAVE_IPV6_MULTICAST_ROUTING
#define IPV6_ALL_MC_FORWARD "/proc/sys/net/ipv6/conf/all/mc_forwarding"

static int proc_set_val(char *file, int val)
{
	int fd;

	fd = open(file, O_WRONLY);
	if (fd < 0) {
		return 1;
	} else {
		if (-1 == write(fd, "1", val)) {
			(void)close(fd);
			return 1;
		}
		(void)close(fd);
	}

	return 0;
}
#endif /* HAVE_IPV6_MULTICAST_ROUTING */

/*
** Initialises the mrouted API and locks it by this exclusively.
**     
** returns: - 0 if the functions succeeds     
**          - the errno value for non-fatal failure condition
*/
int mroute6_enable(void)
{
#ifndef HAVE_IPV6_MULTICAST_ROUTING
	return -1;
#else
	int arg = 1;
	unsigned int i;
	struct iface *iface;

	if ((mroute6_socket = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6)) < 0) {
		if (ENOPROTOOPT == errno)
			smclog(LOG_WARNING, 0, "Kernel does not support IPv6 multicast routing, skipping...");

		return -1;
	}
	if (setsockopt(mroute6_socket, IPPROTO_IPV6, MRT6_INIT, (void *)&arg, sizeof(arg))) {
		switch (errno) {
		case EADDRINUSE:
			smclog(LOG_INIT, errno, "IPv6 multicast routing API already in use");
			break;

#ifdef EOPNOTSUPP
		case EOPNOTSUPP:
			smclog(LOG_INIT, errno, "Unknown socket option MRT6_INIT");
			break;
#endif

		default:
			smclog(LOG_INIT, errno, "Failed initializing IPv6 multicast routing API");
			break;
		}

		close(mroute6_socket);
		mroute6_socket = -1;

		return -1;
	}

	/* Initialize virtual interface table */
	memset(&mif_list, 0, sizeof(mif_list));

	/* On Linux pre 2.6.29 kernels net.ipv6.conf.all.mc_forwarding
	 * is not set on MRT6_INIT so we have to do this manually */
	if (proc_set_val(IPV6_ALL_MC_FORWARD, 1)) {
		if (errno != EACCES)
			smclog(LOG_ERR, errno, "Failed enabling IPv6 mc_forwarding");
	}

	/* create MIFs for all IP, non-loop interfaces */
	for (i = 0; (iface = iface_find_by_index(i)); i++) {
		if (iface->flags & IFF_LOOPBACK) {
			iface->vif = -1;
			continue;
		}
		mroute6_add_mif(iface);
	}

	return 0;
#endif /* HAVE_IPV6_MULTICAST_ROUTING */
}

/*
** Diables the mrouted API and relases by this the lock.
**          
*/
void mroute6_disable(void)
{
#ifdef HAVE_IPV6_MULTICAST_ROUTING
	if (mroute6_socket < 0)
		return;

	if (setsockopt(mroute6_socket, IPPROTO_IPV6, MRT6_DONE, NULL, 0))
		smclog(LOG_ERR, errno, "MRT6_DONE");

	close(mroute6_socket);
	mroute6_socket = -1;
#endif /* HAVE_IPV6_MULTICAST_ROUTING */
}

#ifdef HAVE_IPV6_MULTICAST_ROUTING
/*
** Adds the interface '*iface' as virtual interface to the mrouted API
*/
static void mroute6_add_mif(struct iface *iface)
{
	struct mif6ctl mc;
	int mif = -1;
	size_t i;

	/* find a free mif */
	for (i = 0; i < ARRAY_ELEMENTS(mif_list); i++) {
		if (!mif_list[i].iface) {
			mif = i;
			break;
		}
	}

	/* no more space */
	if (mif == -1) {
		smclog(LOG_ERR, ENOMEM, "%s: out of MIF space", __FUNCTION__);
		return;
	}

	memset(&mc, 0, sizeof(mc));
	mc.mif6c_mifi = mif;
	mc.mif6c_flags = 0;	/* no register */
#ifdef HAVE_MIF6CTL_VIFC_THRESHOLD
	mc.vifc_threshold = 1;	/* Packet TTL must be at least 1 to pass them */
#endif
	mc.mif6c_pifi = iface->ifindex;	/* physical interface index */
#ifdef HAVE_MIF6CTL_VIFC_RATE_LIMIT
	mc.vifc_rate_limit = 0;	/* hopefully no limit */
#endif

	smclog(LOG_NOTICE, 0, "Add MIF: %d Ifindex: %d Flags: 0x%04x Ifname: %s",
	       mc.mif6c_mifi, mc.mif6c_pifi, mc.mif6c_flags, iface->name);

	if (setsockopt(mroute6_socket, IPPROTO_IPV6, MRT6_ADD_MIF, (void *)&mc, sizeof(mc))) {
		smclog(LOG_ERR, errno, "MRT6_ADD_MIF %s", iface->name);
	} else {
		iface->mif = mif;
		mif_list[mif].iface = iface;
	}
}

/*
** Adds the multicast routed '*ptr' to the kernel routes
**
** returns: - 0 if the function succeeds
**          - the errno value for non-fatal failure condition
*/
int mroute6_add(mroute6_t *ptr)
{
	int result = 0;
	size_t i;
	char origin[INET6_ADDRSTRLEN], group[INET6_ADDRSTRLEN];
	struct mf6cctl mc;

	memset(&mc, 0, sizeof(mc));
	mc.mf6cc_origin   = ptr->sender;
	mc.mf6cc_mcastgrp = ptr->group;
	mc.mf6cc_parent   = ptr->inbound;

	/* copy the outgoing MIFs */
	for (i = 0; i < ARRAY_ELEMENTS(ptr->ttl); i++) {
		if (ptr->ttl[i] > 0)
			IF_SET(i, &mc.mf6cc_ifset);
	}

	smclog(LOG_NOTICE, 0, "Add MFC: %s -> %s, Inbound MIF: %d",
	       inet_ntop(AF_INET6, &mc.mf6cc_origin.sin6_addr,
			 origin, INET6_ADDRSTRLEN),
	       inet_ntop(AF_INET6, &mc.mf6cc_mcastgrp.sin6_addr,
			 group, INET6_ADDRSTRLEN), mc.mf6cc_parent);

	if (setsockopt(mroute6_socket, IPPROTO_IPV6, MRT6_ADD_MFC, (void *)&mc, sizeof(mc))) {
		result = errno;
		smclog(LOG_WARNING, errno, "MRT6_ADD_MFC");
	}

	return result;
}

/*
** Removes the multicast routed '*ptr' from the kernel routes
**
** returns: - 0 if the function succeeds
**          - the errno value for non-fatal failure condition
*/
int mroute6_del(mroute6_t *ptr)
{
	int result = 0;
	char origin[INET_ADDRSTRLEN], group[INET_ADDRSTRLEN];
	struct mf6cctl mc;

	memset(&mc, 0, sizeof(mc));
	mc.mf6cc_origin = ptr->sender;
	mc.mf6cc_mcastgrp = ptr->group;

	smclog(LOG_NOTICE, 0, "Del MFC: %s -> %s",
	       inet_ntop(AF_INET6, &mc.mf6cc_origin.sin6_addr,
			 origin, INET6_ADDRSTRLEN),
	       inet_ntop(AF_INET6, &mc.mf6cc_mcastgrp.sin6_addr,
			 group, INET6_ADDRSTRLEN));

	if (setsockopt(mroute6_socket, IPPROTO_IPV6, MRT6_DEL_MFC, (void *)&mc, sizeof(mc))) {
		result = errno;
		smclog(LOG_WARNING, errno, "MRT_DEL_MFC");
	}

	return result;
}
#endif /* HAVE_IPV6_MULTICAST_ROUTING */

/**
 * Local Variables:
 *  version-control: t
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
