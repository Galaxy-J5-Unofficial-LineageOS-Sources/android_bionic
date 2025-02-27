/*
 * Copyright (C) 2016 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>
#include <pthread.h>

#include <netinet/in6.h>
#include <arpa/inet.h>

#include "hostent.h"
#include "resolv_private.h"

#define MAX_ADDRLEN	(INET6_ADDRSTRLEN - (1 + 5))
#define MAX_HOSTLEN	MAXHOSTNAMELEN

#define ESTIMATED_LINELEN	32
#define HCFILE_ALLOC_SIZE	256

/* From sethostent.c */
#define ALIGNBYTES	(sizeof(uintptr_t) - 1)

/*
 * Host cache entry for hcfile.c_data.
 * Offsets are into hcfile.h_data.
 * Strings are *not* terminated by NULL, but by whitespace (isspace) or '#'.
 * Use hstr* functions with these.
 */
struct hcent
{
	uint32_t	addr;
	uint32_t	name;
};

/*
 * Overall host cache file state.
 */
struct hcfile
{
	int		h_fd;
	struct stat	h_st;
	char		*h_data;

	uint32_t	c_alloc;
	uint32_t	c_len;
	struct hcent	*c_data;
};
static struct hcfile hcfile;
static pthread_mutex_t hclock = PTHREAD_MUTEX_INITIALIZER;

static size_t hstrlen(const char *s)
{
	const char *p = s;
	while (*p && *p != '#' && !isspace(*p))
		++p;
	return p - s;
}

static int hstrcmp(const char *a, const char *b)
{
	size_t alen = hstrlen(a);
	size_t blen = hstrlen(b);
	int res = strncmp(a, b, MIN(alen, blen));
	if (res == 0)
		res = alen - blen;
	return res;
}

static char *hstrcpy(char *dest, const char *src)
{
	size_t len = hstrlen(src);
	memcpy(dest, src, len);
	dest[len] = '\0';
	return dest;
}

static char *hstrdup(const char *s)
{
	size_t len = hstrlen(s);
	char *dest = (char *)malloc(len + 1);
	if (!dest)
		return NULL;
	memcpy(dest, s, len);
	dest[len] = '\0';
	return dest;
}

static int cmp_hcent_name(const void *a, const void *b)
{
	struct hcent *ea = (struct hcent *)a;
	const char *na = hcfile.h_data + ea->name;
	struct hcent *eb = (struct hcent *)b;
	const char *nb = hcfile.h_data + eb->name;

	return hstrcmp(na, nb);
}

static struct hcent *_hcfindname_exact(const char *name)
{
	size_t first, last, mid;
	struct hcent *cur = NULL;
	int cmp;

	if (hcfile.c_len == 0)
		return NULL;

	first = 0;
	last = hcfile.c_len - 1;
	mid = (first + last) / 2;
	while (first <= last) {
		cur = hcfile.c_data + mid;
		cmp = hstrcmp(hcfile.h_data + cur->name, name);
		if (cmp == 0)
			goto found;
		if (cmp < 0)
			first = mid + 1;
		else {
			if (mid > 0)
				last = mid - 1;
			else
				return NULL;
		}
		mid = (first + last) / 2;
	}
	return NULL;

found:
	while (cur > hcfile.c_data) {
		struct hcent *prev = cur - 1;
		cmp = cmp_hcent_name(cur, prev);
		if (cmp)
			break;
		cur = prev;
	}

	return cur;
}

static struct hcent *_hcfindname(const char *name)
{
	struct hcent *ent;
	char namebuf[MAX_HOSTLEN];
	char *p;
	char *dot;

	ent = _hcfindname_exact(name);
	if (!ent && strlen(name) < sizeof(namebuf)) {
		strcpy(namebuf, name);
		p = namebuf;
		do {
			dot = strchr(p, '.');
			if (!dot)
				break;
			if (dot > p) {
				*(dot - 1) = '*';
				ent = _hcfindname_exact(dot - 1);
			}
			p = dot + 1;
		}
		while (!ent);
	}

	return ent;
}

/*
 * Find next name on line, if any.
 *
 * Assumes that line is terminated by LF.
 */
static const char *_hcnextname(const char *name)
{
	while (!isspace(*name)) {
		if (*name == '#')
			return NULL;
		++name;
	}
	while (isspace(*name)) {
		if (*name == '\n')
			return NULL;
		++name;
	}
	if (*name == '#')
		return NULL;
	return name;
}

static int _hcfilemmap(void)
{
	struct stat st;
	int h_fd;
	char *h_addr;
	const char *p, *pend;
	uint32_t c_alloc;

	h_fd = open(_PATH_HOSTS, O_RDONLY);
	if (h_fd < 0)
		return -1;
	if (flock(h_fd, LOCK_EX) != 0) {
		close(h_fd);
		return -1;
	}

	if (hcfile.h_data) {
		memset(&st, 0, sizeof(st));
		if (fstat(h_fd, &st) == 0) {
			if (st.st_size == hcfile.h_st.st_size &&
			    st.st_mtime == hcfile.h_st.st_mtime) {
				flock(h_fd, LOCK_UN);
				close(h_fd);
				return 0;
			}
		}
		free(hcfile.c_data);
		munmap(hcfile.h_data, hcfile.h_st.st_size);
		close(hcfile.h_fd);
		memset(&hcfile, 0, sizeof(struct hcfile));
	}

	if (fstat(h_fd, &st) != 0) {
		flock(h_fd, LOCK_UN);
		close(h_fd);
		return -1;
	}
	h_addr = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, h_fd, 0);
	if (h_addr == MAP_FAILED) {
		flock(h_fd, LOCK_UN);
		close(h_fd);
		return -1;
	}

	hcfile.h_fd = h_fd;
	hcfile.h_st = st;
	hcfile.h_data = h_addr;

	c_alloc = 0;
	/*
	 * Do an initial allocation if the file is "large".  Estimate
	 * 32 bytes per line and define "large" as more than half of
	 * the alloc growth size (256 entries).
	 */
	if (st.st_size >= ESTIMATED_LINELEN * HCFILE_ALLOC_SIZE / 2) {
		c_alloc = st.st_size / ESTIMATED_LINELEN;
		hcfile.c_data = malloc(c_alloc * sizeof(struct hcent));
		if (!hcfile.c_data) {
			goto oom;
		}
	}

	p = (const char *)h_addr;
	pend = p + st.st_size;
	while (p < pend) {
		const char *eol, *addr, *name;
		size_t len;
		addr = p;
		eol = memchr(p, '\n', pend - p);
		if (!eol)
			break;
		p = eol + 1;
		if (*addr == '#' || *addr == '\n')
			continue;
		len = hstrlen(addr);
		if (len > MAX_ADDRLEN)
			continue;
		name = addr + len;
		while (name < eol && isspace(*name))
			++name;
		while (name < eol) {
			len = hstrlen(name);
			if (len == 0)
				break;
			if (len < MAX_HOSTLEN) {
				struct hcent *ent;
				if (c_alloc <= hcfile.c_len) {
					struct hcent *c_data;
					c_alloc += HCFILE_ALLOC_SIZE;
					c_data = realloc(hcfile.c_data, c_alloc * sizeof(struct hcent));
					if (!c_data) {
						goto oom;
					}
					hcfile.c_data = c_data;
				}
				ent = hcfile.c_data + hcfile.c_len;
				ent->addr = addr - h_addr;
				ent->name = name - h_addr;
				++hcfile.c_len;
			}
			name += len;
			while (name < eol && isspace(*name))
				++name;
		}
	}

	qsort(hcfile.c_data, hcfile.c_len,
	    sizeof(struct hcent), cmp_hcent_name);

	flock(h_fd, LOCK_UN);

	return 0;

oom:
	free(hcfile.c_data);
	munmap(hcfile.h_data, hcfile.h_st.st_size);
	flock(hcfile.h_fd, LOCK_UN);
	close(hcfile.h_fd);
	memset(&hcfile, 0, sizeof(struct hcfile));
	return -1;
}

/*
 * Caching version of getaddrinfo.
 *
 * If we find the requested host name in the cache, use getaddrinfo to
 * populate the result for each address we find.
 *
 * Note glibc and bionic differ in the handling of ai_canonname.  POSIX
 * says that ai_canonname is only populated in the first result entry.
 * glibc does this.  bionic populates ai_canonname in all result entries.
 * We choose the POSIX/glibc way here.
 */
int hc_getaddrinfo(const char *host, const char *service,
		   const struct addrinfo *hints,
		   struct addrinfo **result)
{
	int ret = 0;
	struct hcent *ent, *cur;
	struct addrinfo *ai;
	struct addrinfo rhints;
	struct addrinfo *last;
	int canonname = 0;
	int cmp;

	if (getenv("ANDROID_HOSTS_CACHE_DISABLE") != NULL)
		return EAI_SYSTEM;

	/* Avoid needless work and recursion */
	if (hints && (hints->ai_flags & AI_NUMERICHOST))
		return EAI_SYSTEM;
	if (!host)
		return EAI_SYSTEM;

	pthread_mutex_lock(&hclock);

	if (_hcfilemmap() != 0) {
		ret = EAI_SYSTEM;
		goto out;
	}
	ent = _hcfindname(host);
	if (!ent) {
		ret = EAI_NONAME;
		goto out;
	}

	if (hints) {
		canonname = (hints->ai_flags & AI_CANONNAME);
		memcpy(&rhints, hints, sizeof(rhints));
		rhints.ai_flags &= ~AI_CANONNAME;
	}
	else {
		memset(&rhints, 0, sizeof(rhints));
	}
	rhints.ai_flags |= AI_NUMERICHOST;

	last = NULL;
	cur = ent;
	do {
		char addrstr[MAX_ADDRLEN];
		struct addrinfo *res;

		hstrcpy(addrstr, hcfile.h_data + cur->addr);

		if (getaddrinfo(addrstr, service, &rhints, &res) == 0) {
			if (!last)
				(*result)->ai_next = res;
			else
				last->ai_next = res;
			last = res;
			while (last->ai_next)
				last = last->ai_next;
		}

		if(cur + 1 >= hcfile.c_data + hcfile.c_len)
			break;
		cmp = cmp_hcent_name(cur, cur + 1);
		cur = cur + 1;
	}
	while (!cmp);

	if (last == NULL) {
		/* This check is equivalent to (*result)->ai_next == NULL */
		ret = EAI_NODATA;
		goto out;
	}

	if (canonname) {
		ai = (*result)->ai_next;
		free(ai->ai_canonname);
		ai->ai_canonname = hstrdup(hcfile.h_data + ent->name);
	}

out:
	pthread_mutex_unlock(&hclock);
	return ret;
}

/*
 * Caching version of gethtbyname.
 *
 * Note glibc and bionic differ in the handling of aliases.  glibc returns
 * all aliases for all entries, regardless of whether they match h_addrtype.
 * bionic returns only the aliases for the first hosts entry.  We return all
 * aliases for all IPv4 entries.
 *
 * Additionally, if an alias is IPv6 and the primary name for an alias also
 * has an IPv4 entry, glibc will return the IPv4 address(es), but bionic
 * will not.  Neither do we.
 */
int hc_gethtbyname(const char *host, int af, struct getnamaddr *info)
{
	int ret = NETDB_SUCCESS;
	struct hcent *ent, *cur;
	int cmp;
	size_t addrlen;
	unsigned int naliases = 0;
	char *aliases[MAXALIASES];
	unsigned int naddrs = 0;
	char *addr_ptrs[MAXADDRS];
	unsigned int n;

	if (getenv("ANDROID_HOSTS_CACHE_DISABLE") != NULL)
		return NETDB_INTERNAL;

	switch (af) {
	case AF_INET:  addrlen = NS_INADDRSZ;  break;
	case AF_INET6: addrlen = NS_IN6ADDRSZ; break;
	default:
		return NETDB_INTERNAL;
	}

	pthread_mutex_lock(&hclock);

	if (_hcfilemmap() != 0) {
		ret = NETDB_INTERNAL;
		goto out;
	}

	ent = _hcfindname(host);
	if (!ent) {
		ret = HOST_NOT_FOUND;
		goto out;
	}

	cur = ent;
	do {
		char addr[16];
		char addrstr[MAX_ADDRLEN];
		char namestr[MAX_HOSTLEN];
		const char *name;

		hstrcpy(addrstr, hcfile.h_data + cur->addr);
		if (inet_pton(af, addrstr, &addr) == 1) {
			char *aligned;
			/* First match is considered the official hostname */
			if (naddrs == 0) {
				hstrcpy(namestr, hcfile.h_data + cur->name);
				HENT_SCOPY(info->hp->h_name, namestr, info->buf, info->buflen);
			}
			for (name = hcfile.h_data + cur->name; name; name = _hcnextname(name)) {
				if (!hstrcmp(name, host))
					continue;
				hstrcpy(namestr, name);
				HENT_SCOPY(aliases[naliases], namestr, info->buf, info->buflen);
				++naliases;
				if (naliases >= MAXALIASES)
					goto nospc;
			}
			aligned = (char *)ALIGN(info->buf);
			if (info->buf != aligned) {
				if ((ptrdiff_t)info->buflen < (aligned - info->buf))
					goto nospc;
				info->buflen -= (aligned - info->buf);
				info->buf = aligned;
			}
			HENT_COPY(addr_ptrs[naddrs], addr, addrlen, info->buf, info->buflen);
			++naddrs;
			if (naddrs >= MAXADDRS)
				goto nospc;
		}

		if(cur + 1 >= hcfile.c_data + hcfile.c_len)
			break;
		cmp = cmp_hcent_name(cur, cur + 1);
		cur = cur + 1;
	}
	while (!cmp);

	if (naddrs == 0) {
		ret = HOST_NOT_FOUND;
		goto out;
	}

	addr_ptrs[naddrs++] = NULL;
	aliases[naliases++] = NULL;

	/* hp->h_name already populated */
	HENT_ARRAY(info->hp->h_aliases, naliases, info->buf, info->buflen);
	for (n = 0; n < naliases; ++n) {
		info->hp->h_aliases[n] = aliases[n];
	}
	info->hp->h_addrtype = af;
	info->hp->h_length = addrlen;
	HENT_ARRAY(info->hp->h_addr_list, naddrs, info->buf, info->buflen);
	for (n = 0; n < naddrs; ++n) {
		info->hp->h_addr_list[n] = addr_ptrs[n];
	}

out:
	pthread_mutex_unlock(&hclock);
	*info->he = ret;
	return ret;

nospc:
	ret = NETDB_INTERNAL;
	goto out;
}
