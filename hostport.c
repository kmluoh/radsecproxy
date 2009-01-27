/*
 * Copyright (C) 2006-2009 Stig Venaas <venaas@uninett.no>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include "debug.h"
#include "util.h"
#include "list.h"
#include "hostport.h"

static void freehostport(struct hostportres *hp) {
    if (hp) {
	free(hp->host);
	free(hp->port);
	if (hp->addrinfo)
	    freeaddrinfo(hp->addrinfo);
	free(hp);
    }
}

static int parsehostport(struct hostportres *hp, char *hostport, char *default_port) {
    char *p, *field;
    int ipv6 = 0;

    p = hostport;
    /* allow literal addresses and port, e.g. [2001:db8::1]:1812 */
    if (*p == '[') {
	p++;
	field = p;
	for (; *p && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n'; p++);
	if (*p != ']') {
	    debug(DBG_ERR, "no ] matching initial [");
	    return 0;
	}
	ipv6 = 1;
    } else {
	field = p;
	for (; *p && *p != ':' && *p != ' ' && *p != '\t' && *p != '\n'; p++);
    }
    if (field == p) {
	debug(DBG_ERR, "missing host/address");
	return 0;
    }

    hp->host = stringcopy(field, p - field);
    if (ipv6) {
	p++;
	if (*p && *p != ':' && *p != ' ' && *p != '\t' && *p != '\n') {
	    debug(DBG_ERR, "unexpected character after ]");
	    return 0;
	}
    }
    if (*p == ':') {
	    /* port number or service name is specified */;
	    field = ++p;
	    for (; *p && *p != ' ' && *p != '\t' && *p != '\n'; p++);
	    if (field == p) {
		debug(DBG_ERR, "syntax error, : but no following port");
		return 0;
	    }
	    hp->port = stringcopy(field, p - field);
    } else
	hp->port = default_port ? stringcopy(default_port, 0) : NULL;
    return 1;
}
    
static struct hostportres *newhostport(char *hostport, char *default_port, uint8_t prefixok) {
    struct hostportres *hp;
    char *slash, *s;
    int plen;
    
    hp = malloc(sizeof(struct hostportres));
    if (!hp) {
	debug(DBG_ERR, "resolve_newhostport: malloc failed");
	goto errexit;
    }
    memset(hp, 0, sizeof(struct hostportres));

    if (!parsehostport(hp, hostport, default_port))
	goto errexit;

    if (!strcmp(hp->host, "*")) {
	free(hp->host);
	hp->host = NULL;
    }

    slash = hp->host ? strchr(hp->host, '/') : NULL;
    if (slash) {
	if (!prefixok) {
	    debug(DBG_WARN, "newhostport: prefix not allowed here", hp->host);
	    goto errexit;
	}
	s = slash + 1;
	if (!*s) {
	    debug(DBG_WARN, "newhostport: prefix length must be specified after the / in %s", hp->host);
	    goto errexit;
	}
	for (; *s; s++)
	    if (*s < '0' || *s > '9') {
		debug(DBG_WARN, "newhostport: %s in %s is not a valid prefix length", slash + 1, hp->host);
		goto errexit;
	    }
	plen = atoi(slash + 1);
	if (plen < 0 || plen > 128) {
	    debug(DBG_WARN, "newhostport: %s in %s is not a valid prefix length", slash + 1, hp->host);
	    goto errexit;
	}
	hp->prefixlen = plen;
	*slash = '\0';
    } else
	hp->prefixlen = 255;
    return hp;

 errexit:
    freehostport(hp);
    return NULL;
}

static int resolvehostport(struct hostportres *hp, int socktype, uint8_t passive) {
    struct addrinfo hints, *res;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = socktype;
    hints.ai_family = AF_UNSPEC;
    if (passive)
	hints.ai_flags = AI_PASSIVE;
    
    if (!hp->host && !hp->port) {
	/* getaddrinfo() doesn't like host and port to be NULL */
	if (getaddrinfo(hp->host, "1812" /* can be anything */, &hints, &hp->addrinfo)) {
	    debug(DBG_WARN, "resolvehostport: can't resolve (null) port (null)");
	    goto errexit;
	}
	for (res = hp->addrinfo; res; res = res->ai_next)
	    port_set(res->ai_addr, 0);
    } else {
	if (hp->prefixlen != 255)
	    hints.ai_flags |= AI_NUMERICHOST;
	if (getaddrinfo(hp->host, hp->port, &hints, &hp->addrinfo)) {
	    debug(DBG_WARN, "resolvehostport: can't resolve %s port %s", hp->host ? hp->host : "(null)", hp->port ? hp->port : "(null)");
	    goto errexit;
	}
	if (hp->prefixlen != 255) {
	    switch (hp->addrinfo->ai_family) {
	    case AF_INET:
		if (hp->prefixlen > 32) {
		    debug(DBG_WARN, "resolvehostport: prefix length must be <= 32 in %s", hp->host);
		    goto errexit;
		}
		break;
	    case AF_INET6:
		break;
	    default:
		debug(DBG_WARN, "resolvehostport: prefix must be IPv4 or IPv6 in %s", hp->host);
		goto errexit;
	    }
	}
    }
    return 1;

 errexit:
    if (hp->addrinfo)
	freeaddrinfo(hp->addrinfo);
    return 0;
}	  

int addhostport(struct list **hostports, char *hostport, char *portdefault, uint8_t prefixok) {
    struct hostportres *hp;

    hp = newhostport(hostport, portdefault, prefixok);
    if (!hp)
	return 0;
    if (!*hostports)
	*hostports = list_create();
    if (!*hostports || !list_push(*hostports, hp)) {
	freehostport(hp);
	debug(DBG_ERR, "addhostport: malloc failed");
	return 0;
    }
    return 1;
}

void freehostports(struct list *hostports) {
    struct hostportres *hp;

    while ((hp = (struct hostportres *)list_shift(hostports)))
	freehostport(hp);
    list_destroy(hostports);
}

int resolvehostports(struct list *hostports, int socktype) {
    struct list_node *entry;
    struct hostportres *hp;
    
    for (entry = list_first(hostports); entry; entry = list_next(entry)) {
	hp = (struct hostportres *)entry->data;
	if (!hp->addrinfo && !resolvehostport(hp, socktype, 0))
	    return 0;
    }
    return 1;
}

struct addrinfo *resolvepassiveaddrinfo(char *hostport, char *default_port, int socktype) {
    struct addrinfo *ai = NULL;
    struct hostportres *hp = newhostport(hostport, default_port, 0);
    if (hp && resolvehostport(hp, socktype, 1)) {
	ai = hp->addrinfo;
	hp->addrinfo = NULL;
    }
    freehostport(hp);
    return ai;
}

/* returns 1 if the len first bits are equal, else 0 */
static int prefixmatch(void *a1, void *a2, uint8_t len) {
    static uint8_t mask[] = { 0, 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe };
    uint8_t r, l = len / 8;
    if (l && memcmp(a1, a2, l))
	return 0;
    r = len % 8;
    if (!r)
	return 1;
    return (((uint8_t *)a1)[l] & mask[r]) == (((uint8_t *)a2)[l] & mask[r]);
}

int addressmatches(struct list *hostports, struct sockaddr *addr) {
    struct sockaddr_in6 *sa6 = NULL;
    struct in_addr *a4 = NULL;
    struct addrinfo *res;
    struct list_node *entry;
    struct hostportres *hp = NULL;
    
    if (addr->sa_family == AF_INET6) {
        sa6 = (struct sockaddr_in6 *)addr;
        if (IN6_IS_ADDR_V4MAPPED(&sa6->sin6_addr)) {
            a4 = (struct in_addr *)&sa6->sin6_addr.s6_addr[12];
	    sa6 = NULL;
	}
    } else
	a4 = &((struct sockaddr_in *)addr)->sin_addr;

    for (entry = list_first(hostports); entry; entry = list_next(entry)) {
	hp = (struct hostportres *)entry->data;
	for (res = hp->addrinfo; res; res = res->ai_next)
	    if (hp->prefixlen == 255) {
		if ((a4 && res->ai_family == AF_INET &&
		     !memcmp(a4, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4)) ||
		    (sa6 && res->ai_family == AF_INET6 &&
		     !memcmp(&sa6->sin6_addr, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, 16)))
		    return 1;
	    } else {
		if ((a4 && res->ai_family == AF_INET &&
		     prefixmatch(a4, &((struct sockaddr_in *)res->ai_addr)->sin_addr, hp->prefixlen)) ||
		    (sa6 && res->ai_family == AF_INET6 &&
		     prefixmatch(&sa6->sin6_addr, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, hp->prefixlen)))
		    return 1;
	    }
    }
    return 0;
}