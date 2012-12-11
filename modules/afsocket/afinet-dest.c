/*
 * Copyright (c) 2002-2012 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 1998-2012 Balázs Scheidler
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "afinet-dest.h"
#include "messages.h"
#include "misc.h"
#include "gprocess.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#ifdef _GNU_SOURCE
#  define _GNU_SOURCE_DEFINED 1
#  undef _GNU_SOURCE
#endif

#if ENABLE_SPOOF_SOURCE
#include <libnet.h>
#endif

#if _GNU_SOURCE_DEFINED
#  undef _GNU_SOURCE
#  define _GNU_SOURCE 1
#endif

void
afinet_dd_set_localip(LogDriver *s, gchar *ip)
{
  AFInetDestDriver *self = (AFInetDestDriver *) s;

  if (self->bind_ip)
    g_free(self->bind_ip);
  self->bind_ip = g_strdup(ip);
}

void
afinet_dd_set_localport(LogDriver *s, gchar *service)
{
  AFInetDestDriver *self = (AFInetDestDriver *) s;

  if (self->bind_port)
    g_free(self->bind_port);
  self->bind_port = g_strdup(service);
}

void
afinet_dd_set_destport(LogDriver *s, gchar *service)
{
  AFInetDestDriver *self = (AFInetDestDriver *) s;

  if (self->dest_port)
    g_free(self->dest_port);
  self->dest_port = g_strdup(service);
}

void
afinet_dd_set_spoof_source(LogDriver *s, gboolean enable)
{
#if ENABLE_SPOOF_SOURCE
  AFInetDestDriver *self = (AFInetDestDriver *) s;

  self->spoof_source = (self->super.socket_options->type == SOCK_DGRAM) && enable;
#else
  msg_error("Error enabling spoof-source, you need to compile syslog-ng with --enable-spoof-source", NULL);
#endif
}

static gboolean
afinet_dd_apply_transport(AFSocketDestDriver *s)
{
  AFInetDestDriver *self = (AFInetDestDriver *) s;
  GlobalConfig *cfg = log_pipe_get_config(&s->super.super.super);
  gchar *default_dest_port  = NULL;
  struct protoent *ipproto_ent;

  g_sockaddr_unref(self->super.bind_addr);
  g_sockaddr_unref(self->super.dest_addr);

  if (self->super.socket_options->address_family == AF_INET)
    {
      self->super.bind_addr = g_sockaddr_inet_new("0.0.0.0", 0);
      self->super.dest_addr = g_sockaddr_inet_new("0.0.0.0", 0);
    }
#if ENABLE_IPV6
  else if (self->super.socket_options->address_family == AF_INET6)
    {
      self->super.bind_addr = g_sockaddr_inet6_new("::", 0);
      self->super.dest_addr = g_sockaddr_inet6_new("::", 0);
    }
#endif
  else
    {
      /* address family not known */
      g_assert_not_reached();
    }

  if (self->super.transport == NULL)
    {
      if (self->super.socket_options->type == SOCK_STREAM)
        afsocket_dd_set_transport(&self->super.super.super, "tcp");
      else
        afsocket_dd_set_transport(&self->super.super.super, "udp");
    }

  if (strcasecmp(self->super.transport, "udp") == 0)
    {
      static gboolean msg_udp_source_port_warning = FALSE;

      if (!self->dest_port)
        {
          /* NOTE: this version number change has happened in a different
           * major version in OSE vs. PE, thus the update behaviour must
           * be triggered differently.  In OSE it needs to be triggered
           * when the config version has changed to 3.3, in PE when 3.2.
           *
           * This is unfortunate, the only luck we have to be less
           * confusing is that syslog() driver was seldom used.
           *
           */
          if (self->super.syslog_protocol && cfg_is_config_version_older(cfg, 0x0303))
            {
              if (!msg_udp_source_port_warning)
                {
                  msg_warning("WARNING: Default port for syslog(transport(udp)) has changed from 601 to 514 in syslog-ng 3.3, please update your configuration",
                              evt_tag_str("id", self->super.super.super.id),
                              NULL);
                  msg_udp_source_port_warning = TRUE;
                }
              default_dest_port = "601";
            }
          else
            default_dest_port = "514";
        }
      self->super.socket_options->type = SOCK_DGRAM;
      self->super.socket_options->protocol = 0;
      self->super.logproto_name = "dgram";
    }
  else if (strcasecmp(self->super.transport, "tcp") == 0)
    {
      if (self->super.syslog_protocol)
        {
          self->super.logproto_name = "framed";
          default_dest_port = "601";
        }
      else
        {
          self->super.logproto_name = "text";
          default_dest_port = "514";
        }
      self->super.socket_options->type = SOCK_STREAM;
      self->super.socket_options->protocol = 0;
    }
  else if (strcasecmp(self->super.transport, "tls") == 0)
    {
      static gboolean msg_tls_source_port_warning = FALSE;

      g_assert(self->super.syslog_protocol);
      if (!self->dest_port)
        {
          /* NOTE: this version number change has happened in a different
           * major version in OSE vs. PE, thus the update behaviour must
           * be triggered differently.  In OSE it needs to be triggered
           * when the config version has changed to 3.3, in PE when 3.2.
           *
           * This is unfortunate, the only luck we have to be less
           * confusing is that syslog() driver was seldom used.
           *
           */

          if (cfg_is_config_version_older(cfg, 0x0303))
            {
              if (!msg_tls_source_port_warning)
                {
                  msg_warning("WARNING: Default port for syslog(transport(tls)) is modified from 601 to 6514",
                              evt_tag_str("id", self->super.super.super.id),
                              NULL);
                  msg_tls_source_port_warning = TRUE;
                }
              default_dest_port = "601";
            }
          else
            default_dest_port = "6514";
        }
      self->super.require_tls = TRUE;
      self->super.socket_options->type = SOCK_STREAM;
      self->super.logproto_name = "framed";
    }
  else
    {
      self->super.socket_options->type = SOCK_STREAM;
      self->super.logproto_name = self->super.transport;
    }

  if ((self->bind_ip && !resolve_hostname(&self->super.bind_addr, self->bind_ip)))
    return FALSE;

  if (!self->super.socket_options->protocol)
    {
      if (self->super.socket_options->type == SOCK_STREAM)
        self->super.socket_options->protocol = IPPROTO_TCP;
      else
        self->super.socket_options->protocol = IPPROTO_UDP;
    }

  ipproto_ent = getprotobynumber(self->super.socket_options->protocol);
  afinet_set_port(self->super.dest_addr, self->dest_port ? : default_dest_port,
                  ipproto_ent ? ipproto_ent->p_name
                              : (self->super.socket_options->type == SOCK_STREAM) ? "tcp" : "udp");

  if (!self->super.dest_name)
    self->super.dest_name = g_strdup_printf("%s:%d", self->super.hostname,
                                            g_sockaddr_inet_check(self->super.dest_addr) ? g_sockaddr_inet_get_port(self->super.dest_addr)
#if ENABLE_IPV6
                                            : g_sockaddr_inet6_get_port(self->super.dest_addr)
#else
                                            : 0
#endif
                                            );


#if BUILD_WITH_SSL
  if (self->super.require_tls && !self->super.tls_context)
    {
      msg_error("transport(tls) was specified, but tls() options missing",
                evt_tag_str("id", self->super.super.super.id),
                NULL);
      return FALSE;
    }
#endif

  return TRUE;
}

static gboolean
afinet_dd_setup_socket(AFSocketDestDriver *s, gint fd)
{
  AFInetDestDriver *self = (AFInetDestDriver *) s;

  if (!resolve_hostname(&self->super.dest_addr, self->super.hostname))
    return FALSE;

  return afinet_setup_socket(fd, self->super.dest_addr, &self->inet_socket_options, AFSOCKET_DIR_SEND);
}

static gboolean
afinet_dd_init(LogPipe *s)
{
  AFInetDestDriver *self G_GNUC_UNUSED = (AFInetDestDriver *) s;
  gboolean success;

#if ENABLE_SPOOF_SOURCE
  if (self->spoof_source)
    self->super.connections_kept_alive_accross_reloads = TRUE;
#endif

  success = afsocket_dd_init(s);
#if ENABLE_SPOOF_SOURCE
  if (success)
    {
      if (self->spoof_source && !self->lnet_ctx)
        {
          gchar error[LIBNET_ERRBUF_SIZE];
          cap_t saved_caps;

          saved_caps = g_process_cap_save();
          g_process_cap_modify(CAP_NET_RAW, TRUE);
          self->lnet_ctx = libnet_init(self->super.bind_addr->sa.sa_family == AF_INET ? LIBNET_RAW4 : LIBNET_RAW6, NULL, error);
          g_process_cap_restore(saved_caps);
          if (!self->lnet_ctx)
            {
              msg_error("Error initializing raw socket, spoof-source support disabled",
                        evt_tag_str("error", NULL),
                        NULL);
            }
        }
    }
#endif

  return success;
}

#if ENABLE_SPOOF_SOURCE
static gboolean
afinet_dd_construct_ipv4_packet(AFInetDestDriver *self, LogMessage *msg, GString *msg_line)
{
  libnet_ptag_t ip, udp;
  struct sockaddr_in *src, *dst;

  if (msg->saddr->sa.sa_family != AF_INET)
    return FALSE;

  src = (struct sockaddr_in *) &msg->saddr->sa;
  dst = (struct sockaddr_in *) &self->super.dest_addr->sa;

  libnet_clear_packet(self->lnet_ctx);

  udp = libnet_build_udp(ntohs(src->sin_port),
                         ntohs(dst->sin_port),
                         LIBNET_UDP_H + msg_line->len,
                         0,
                         (guchar *) msg_line->str,
                         msg_line->len,
                         self->lnet_ctx,
                         0);
  if (udp == -1)
    return FALSE;

  ip = libnet_build_ipv4(LIBNET_IPV4_H + msg_line->len + LIBNET_UDP_H,
                         IPTOS_LOWDELAY,         /* IP tos */
                         0,                      /* IP ID */
                         0,                      /* frag stuff */
                         64,                     /* TTL */
                         IPPROTO_UDP,            /* transport protocol */
                         0,
                         src->sin_addr.s_addr,   /* source IP */
                         dst->sin_addr.s_addr,   /* destination IP */
                         NULL,                   /* payload (none) */
                         0,                      /* payload length */
                         self->lnet_ctx,
                         0);
  if (ip == -1)
    return FALSE;

  return TRUE;
}

#if ENABLE_IPV6
static gboolean
afinet_dd_construct_ipv6_packet(AFInetDestDriver *self, LogMessage *msg, GString *msg_line)
{
  libnet_ptag_t ip, udp;
  struct sockaddr_in *src4;
  struct sockaddr_in6 src, *dst;
  struct libnet_in6_addr ln_src, ln_dst;

  switch (msg->saddr->sa.sa_family)
    {
    case AF_INET:
      src4 = (struct sockaddr_in *) &msg->saddr->sa;
      memset(&src, 0, sizeof(src));
      src.sin6_family = AF_INET6;
      src.sin6_port = src4->sin_port;
      ((guint32 *) &src.sin6_addr)[0] = 0;
      ((guint32 *) &src.sin6_addr)[1] = 0;
      ((guint32 *) &src.sin6_addr)[2] = htonl(0xffff);
      ((guint32 *) &src.sin6_addr)[3] = src4->sin_addr.s_addr;
      break;
    case AF_INET6:
      src = *((struct sockaddr_in6 *) &msg->saddr->sa);
      break;
    default:
      g_assert_not_reached();
      break;
    }

  dst = (struct sockaddr_in6 *) &self->super.dest_addr->sa;

  libnet_clear_packet(self->lnet_ctx);

  udp = libnet_build_udp(ntohs(src.sin6_port),
                         ntohs(dst->sin6_port),
                         LIBNET_UDP_H + msg_line->len,
                         0,
                         (guchar *) msg_line->str,
                         msg_line->len,
                         self->lnet_ctx,
                         0);
  if (udp == -1)
    return FALSE;

  /* There seems to be a bug in libnet 1.1.2 that is triggered when
   * checksumming UDP6 packets. This is a workaround below. */

  libnet_toggle_checksum(self->lnet_ctx, udp, LIBNET_OFF);

  memcpy(&ln_src, &src.sin6_addr, sizeof(ln_src));
  memcpy(&ln_dst, &dst->sin6_addr, sizeof(ln_dst));
  ip = libnet_build_ipv6(0, 0,
                         LIBNET_UDP_H + msg_line->len,
                         IPPROTO_UDP,            /* IPv6 next header */
                         64,                     /* hop limit */
                         ln_src, ln_dst,
                         NULL, 0,                /* payload and its length */
                         self->lnet_ctx,
                         0);

  if (ip == -1)
    return FALSE;

  return TRUE;
}
#endif

#endif

static void
afinet_dd_queue(LogPipe *s, LogMessage *msg, const LogPathOptions *path_options, gpointer user_data)
{
#if ENABLE_SPOOF_SOURCE
  AFInetDestDriver *self = (AFInetDestDriver *) s;

  /* NOTE: this code should probably become a LogTransport instance so that
   * spoofed packets are also going through the LogWriter queue */

  if (self->spoof_source && self->lnet_ctx && msg->saddr && (msg->saddr->sa.sa_family == AF_INET || msg->saddr->sa.sa_family == AF_INET6) && log_writer_opened((LogWriter *) self->super.writer))
    {
      gboolean success = FALSE;

      g_assert(self->super.socket_options->type == SOCK_DGRAM);

      g_static_mutex_lock(&self->lnet_lock);
      if (!self->lnet_buffer)
        self->lnet_buffer = g_string_sized_new(256);
      log_writer_format_log((LogWriter *) self->super.writer, msg, self->lnet_buffer);

      switch (self->super.dest_addr->sa.sa_family)
        {
        case AF_INET:
          success = afinet_dd_construct_ipv4_packet(self, msg, self->lnet_buffer);
          break;
#if ENABLE_IPV6
        case AF_INET6:
          success = afinet_dd_construct_ipv6_packet(self, msg, self->lnet_buffer);
          break;
#endif
        default:
          g_assert_not_reached();
        }
      if (success)
        {
          if (libnet_write(self->lnet_ctx) >= 0)
            {
              /* we have finished processing msg */
              log_msg_ack(msg, path_options);
              log_msg_unref(msg);

              g_static_mutex_unlock(&self->lnet_lock);
              return;
            }
          else
            {
              msg_error("Error sending raw frame",
                        evt_tag_str("error", libnet_geterror(self->lnet_ctx)),
                        NULL);
            }
        }
      g_static_mutex_unlock(&self->lnet_lock);
    }
#endif
  log_dest_driver_queue_method(s, msg, path_options, user_data);
}

void
afinet_dd_free(LogPipe *s)
{
  AFInetDestDriver *self = (AFInetDestDriver *) s;

  g_free(self->bind_ip);
  g_free(self->bind_port);
  g_free(self->dest_port);
#if ENABLE_SPOOF_SOURCE
  if (self->lnet_buffer)
    g_string_free(self->lnet_buffer, TRUE);
  g_static_mutex_free(&self->lnet_lock);
#endif
  afsocket_dd_free(s);
}


AFInetDestDriver *
afinet_dd_new_instance(gint af, gint sock_type, gchar *host)
{
  AFInetDestDriver *self = g_new0(AFInetDestDriver, 1);

  afsocket_dd_init_instance(&self->super, &self->inet_socket_options.super, af, sock_type, host);

  self->super.super.super.super.init = afinet_dd_init;
  self->super.super.super.super.queue = afinet_dd_queue;
  self->super.super.super.super.free_fn = afinet_dd_free;
  self->super.setup_socket = afinet_dd_setup_socket;
  self->super.apply_transport = afinet_dd_apply_transport;
  if (sock_type == SOCK_STREAM)
    {
      self->inet_socket_options.super.so_keepalive = TRUE;
#if defined(TCP_KEEPTIME) && defined(TCP_KEEPIDLE) && defined(TCP_KEEPCNT)
      self->inet_socket_options.tcp_keepalive_time = 60;
      self->inet_socket_options.tcp_keepalive_intvl = 10;
      self->inet_socket_options.tcp_keepalive_probes = 6;
#endif
    }

#if ENABLE_SPOOF_SOURCE
  g_static_mutex_init(&self->lnet_lock);
#endif
  return self;
}

AFInetDestDriver *
afinet_dd_new(gint af, gint sock_type, gchar *host)
{
  return afinet_dd_new_instance(af, sock_type, host);
}

AFInetDestDriver *
afsyslog_dd_new(gchar *host)
{
  AFInetDestDriver *self = afinet_dd_new_instance(AF_INET, SOCK_STREAM, host);

  self->super.syslog_protocol = TRUE;
  return self;
}

AFInetDestDriver *
afnetwork_dd_new(gchar *host)
{
  AFInetDestDriver *self = afinet_dd_new_instance(AF_INET, SOCK_STREAM, host);

  return self;
}
