/* utils.c - various functions
   Copyright (C) 1999-2002, 2007, 2015, 2016 Free Software Foundation, Inc.

   This file is part of GNU JWhois.

   GNU JWhois is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   GNU JWhois is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GNU JWhois.  If not, see <http://www.gnu.org/licenses/>.  */

#include <config.h>
#include "system.h"

/* Specification.  */
#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <regex.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "init.h"
#include "jconfig.h"
#include "whois.h"

/*
 *  This creates a string.  Text book example :-)
 */
char *
create_string(const char *fmt, ...)
{
  int n, size = 100;
  char *p;
  va_list ap;

  p = malloc(size);
  if (!p)
    return NULL;

  while (1)
    {
      va_start(ap, fmt);
      n = vsprintf(p, fmt, ap);
      va_end(ap);
      if (n > -1 && n < size)
	return p;
      if (n > -1)
	size = n+1;
      else
	size *= 2;

      p = realloc(p, size);
      if (!p)
	return NULL;
    }
}

char *
strjoinv (const char *delim, int strc, const char *strv[])
{
  size_t bufsize = 0;
  /* Add space for STRVs.  */
  for (int i = 0; i < strc; i++)
    bufsize += strlen (strv[i]);

  bufsize += (strc - 1) * strlen (delim); /* Add space for DELIMs.  */
  bufsize += 1;		  /* Add space for '\0' at the end of BUF.  */

  char *buf = xmalloc (bufsize);

  /* Copy strings from STRV to BUF with DELIM between them.  */
  int idx = 0;
  size_t delim_len = strlen (delim);
  for (int i = 0; i < (strc - 1); i++)
    {
      size_t len = strlen (strv[i]);
      strncpy (buf + idx, strv[i], len);
      idx += len;
      strncpy (buf + idx, delim, delim_len);
      idx += delim_len;
    }
  strcpy (buf + idx, strv[strc - 1]);

  return buf;
}

/*
 *  This adds text to a buffer.
 */
int
add_text_to_buffer(char **buffer, const char *text)
{
  if (!*buffer)
    {
      *buffer = xmalloc (strlen (text) + 1);
      strncpy(*buffer, text, strlen(text)+1);
    }
  else
    {
      *buffer = xrealloc (*buffer, strlen (*buffer) + strlen (text) + 1);
      strncat(*buffer, text, strlen(text)+1);
    }
  return 0;
}

/*
 *  This will search the jwhois.server-options base in the configuration
 *  file and return the base domain value for the given hostname.
 */
char *
get_whois_server_domain_path(const char *hostname)
{
  struct jconfig *j;
  struct re_pattern_buffer      rpb;
  char *error;
  int ind, i;
  unsigned char case_fold[256];

  for (i = 0; i < 256; i++)
    case_fold[i] = toupper(i);

  jconfig_set();

  while ((j = jconfig_next_all("jwhois|server-options")) != NULL)
    {
      rpb.allocated = 0;
      rpb.buffer = NULL;
      rpb.translate = case_fold;
      rpb.fastmap = (char *)NULL;

      error = (char *)re_compile_pattern(j->domain+22,
					 strlen(j->domain+22), &rpb);
      if (error != 0)
	return NULL;
	
      ind = re_search(&rpb, hostname, strlen(hostname), 0, 0, NULL);
      if (ind == 0)
	return j->domain;
      else if (ind == -2)
	return NULL;
      
    }
  return NULL;
  jconfig_end();
}

/*
 *  This will search the jwhois.server-options base in the configuration
 *  file and return the value of the key corresponding to the given hostname.
 */
char *
get_whois_server_option(const char *hostname, const char *key)
{
  struct jconfig *j;
  char *base;

  base = get_whois_server_domain_path(hostname);
  
  if (!base)
    return NULL;
  
  jconfig_set();
  j = jconfig_getone(base, key);
  if (!j)
    return NULL;

  return j->value;
}

/* Lookup HOST using PORT.  HOST can be either a hostname or an IP address.
   Set *RES and return 0 if lookup have succeeded.  Otherwise return the
   corresponding error code which can be interpreted by 'gai_strerror'.  */
static int
lookup_host_addrinfo (struct addrinfo **res, const char *host, int port)
{
  struct addrinfo hints;
  char ascport[10] = "whois";
  int error;

  memset (&hints, 0, sizeof (hints));
  hints.ai_family = PF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (port)
    sprintf(ascport, "%9.9d", port);

  error = getaddrinfo (host, ascport, &hints, res);
  if (error)
    printf ("[%s: %s]\n", host, gai_strerror (error));
  return error;
}

/*
 *  This function creates a connection to the indicated host/port and
 *  returns a file descriptor or -1 if error.
 */
int
make_connect(const char *host, int port)
{
  int sockfd, error, flags, retval;
  unsigned int retlen;
  fd_set fdset;
  struct timeval timeout = { arguments->connect_timeout, 0 };
  struct addrinfo *res;

  error = lookup_host_addrinfo(&res, host, port);
  if (error < 0)
    {
      return -1;
    }
  for (; res; res = res->ai_next)
    {
      sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
      if (sockfd == -1 && res->ai_family == PF_INET6 && res->ai_next)
	/* Operating system seems to lack IPv6 support, try next entry */
	continue;

      if (sockfd == -1)
	{
	  printf("[%s]\n", _("Error creating socket"));
	  return -1;
	}
      
      flags = fcntl(sockfd, F_GETFL, 0);
      if (fcntl(sockfd, F_SETFL, flags|O_NONBLOCK) == -1)
	{
	  close (sockfd);
	  return -1;
	}


      error = connect(sockfd, res->ai_addr, res->ai_addrlen);
      if (error == 0)
	return sockfd;
      
      if (error < 0 && errno != EINPROGRESS)
	{
	  close (sockfd);
	  continue;
	}

      FD_ZERO(&fdset);
      FD_SET(sockfd, &fdset);

      error = select(FD_SETSIZE, NULL, &fdset, NULL, &timeout);
      if (error == 0)
	{
	  close (sockfd);
	  continue;
	}

      retlen = sizeof(retval);
      error = getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &retval, &retlen);
      if (error == 0 && retval == 0)
	return sockfd;
      
      close (sockfd);
    }

  return -1;
}

/*
 *  This function takes a string gotten from the commandline, splits
 *  out a hostname if one is found after an '@' sign which is not escaped
 *  by '\'.  Returns 1 is successful, else 0. qstrins is reformatted
 *  to hold only the query without hostname.
 */
int
split_host_from_query (whois_query_t wq)
{
  char *tmpptr;

  tmpptr = strchr(wq->query, '@');
  if (!tmpptr)
    return 0;

  tmpptr--;
  if (*tmpptr == '\\')
    return 0;

  tmpptr++;
  *tmpptr = '\0';
  tmpptr++;
  wq->host = tmpptr;
  return 1;
}

/* This initialises the timeout value from options in the configuration
   file.  */
void
timeout_init (void)
{
  jconfig_set ();
  struct jconfig *j = jconfig_getone ("jwhois", "connect-timeout");

  char *buf;
  const char *ret = j ? j->value : "75";
  arguments->connect_timeout = strtol (ret , &buf, 10);
  if (*buf != '\0')
    {
      if (arguments->verbose)
        printf ("[%s: %s]\n", _("Invalid connect timeout value"), ret);
      arguments->connect_timeout = 75;
    }
}

int
dump_arguments (struct arguments *args)
{
  int limit = args->rwhois_limit;

  return printf ("[Debug: args {\n"
                 "  Cache = %s,\n"
                 "  Force lookup = %s,\n"
                 "  Force host = %s,\n"
                 "  Force port = %s,\n"
                 "  Config file name = %s,\n"
                 "  Follow redirections = %s,\n"
                 "  Display redirections = %s,\n"
                 "  Whois-servers.net service support = %s,\n"
                 "  Whois-servers domain = %s,\n"
                 "  Raw query = %s,\n"
                 "  Rwhois display = %s,\n"
                 "  Rwhois limit = %s,\n"
                 "  Force rwhois = %s,\n"
                 "}]\n",
                 args->cache ? "On" : "Off",
                 args->forcelookup ? "Yes" : "No",
                 args->ghost? args->ghost : "(None)",
                 args->gport ? create_string ("%d", args->gport) : "(None)",
                 args->config ? args->config : "(None)",
                 args->redirect ? "Yes" : "No",
                 args->display_redirections ? "Yes" : "No",
                 args->enable_whoisservers ? "Yes" : "No",
                 args->whoisservers ? args->whoisservers : WHOIS_SERVERS,
                 args->raw_query ? "Yes" : "No",
                 args->rwhois_display ? args->rwhois_display : "(None)",
                 args->rwhois_limit ? create_string ("%d", limit) : "(None)",
                 args->rwhois ? "Yes" : "No");
}
