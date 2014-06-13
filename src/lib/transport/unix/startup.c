/*
** Copyright 2005-2012  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/**************************************************************************\
*//*! \file
** <L5_PRIVATE L5_SOURCE>
** \author  djr/ctk
**  \brief  Sockets interface to user level TCP
**   \date  
**    \cop  (c) Level 5 Networks Limited.
** </L5_PRIVATE>
*//*
\**************************************************************************/
  
/*! \cidoxg_lib_transport_unix */

#include <internal.h>
#include <ci/app/rawpkt.h>
#include <ci/internal/syscall.h>
#include <unistd.h> /* for getpid() */
#include <sys/stat.h> /* for mkdir() */
#include <sys/types.h>  /* for mkdir() */
#include <ci/internal/efabcfg.h>
#include <onload/version.h>


citp_globals_t citp = {
  /* log_fd */ -1,

  /* And the rest default to zero. */
};


static int citp_setup_logging_early(void)
{
  /* If stderr is a tty, use it.  Else, use ioctl. */
  if( isatty(STDERR_FILENO) )
    ci_log_fn = citp_log_fn_ul;
  else {
    ci_log_fn = citp_log_fn_drv;
  }
  ci_set_log_prefix("onload: ");
  return 0;
}

static void citp_setup_logging_change(void *new_log_fn)
{
  if( ci_log_fn != new_log_fn && citp.log_fd >= 0) {
    ci_sys_close(citp.log_fd);
    citp.log_fd = -1;
  }
  ci_log_fn = new_log_fn;
}

void citp_setup_logging_prefix(void)
{
  static char s0[64];
  sprintf(s0, "oo:%.16s[%d]: ", citp.process_name, (int) getpid());
  ci_set_log_prefix(s0);
}


/* Called to intialise thread-specific state, the first time a thread needs
 * to use part of the per-thread state that requires explicit
 * initialisation.
 *
 * Some members of oo_per_thread are implicitly initialised to zero either
 * because they are static data (if HAVE_CC__THREAD), else when the memory
 * is allocated.  Those members must not be reinitialised here, because
 * they may already have been used and modified.
 */
static void __oo_per_thread_init_thread(struct oo_per_thread* pt)
{
  /* It's possible that we got here because we're not initialised at all! */
  if( citp.init_level < CITP_INIT_SYSCALLS ) {
    if( _citp_do_init_inprogress == 0 )
      citp_do_init(CITP_INIT_ALL);
    else
      citp_do_init(CITP_INIT_SYSCALLS);
  }

  /* [pt->sig] is zero initialised. */

  oo_stackname_thread_init(&pt->stackname);

  pt->spinstate = 0;
#if CI_CFG_UDP
  if( CITP_OPTS.udp_recv_spin )
    pt->spinstate |= (1 << ONLOAD_SPIN_UDP_RECV);
  if( CITP_OPTS.udp_send_spin )
    pt->spinstate |= (1 << ONLOAD_SPIN_UDP_SEND);
#endif
  if( CITP_OPTS.tcp_recv_spin )
    pt->spinstate |= (1 << ONLOAD_SPIN_TCP_RECV);
  if( CITP_OPTS.tcp_send_spin )
    pt->spinstate |= (1 << ONLOAD_SPIN_TCP_SEND);
  if( CITP_OPTS.tcp_accept_spin )
    pt->spinstate |= (1 << ONLOAD_SPIN_TCP_ACCEPT);
  if( CITP_OPTS.pkt_wait_spin )
    pt->spinstate |= (1 << ONLOAD_SPIN_PKT_WAIT);
#if CI_CFG_USERSPACE_PIPE
  if( CITP_OPTS.pipe_recv_spin )
    pt->spinstate |= (1 << ONLOAD_SPIN_PIPE_RECV);
  if( CITP_OPTS.pipe_send_spin )
    pt->spinstate |= (1 << ONLOAD_SPIN_PIPE_SEND);
#endif
  if( CITP_OPTS.ul_select_spin )
    pt->spinstate |= (1 << ONLOAD_SPIN_SELECT);
  if( CITP_OPTS.ul_poll_spin )
    pt->spinstate |= (1 << ONLOAD_SPIN_POLL);
#if CI_CFG_USERSPACE_EPOLL
  if( CITP_OPTS.ul_epoll_spin )
    pt->spinstate |= (1 << ONLOAD_SPIN_EPOLL_WAIT);
#endif
}




static void citp_dump_config(void)
{
  char buf[80];
  confstr(_CS_GNU_LIBC_VERSION, buf, sizeof(buf));
  log("GNU_LIBC_VERSION = %s", buf);
  confstr(_CS_GNU_LIBPTHREAD_VERSION, buf, sizeof(buf));
  log("GNU_LIBPTHREAD_VERSION = %s", buf);
  log("ci_glibc_uses_nptl = %d", ci_glibc_uses_nptl());
  log("ci_is_multithreaded = %d", ci_is_multithreaded());
}

static void citp_dump_opts(citp_opts_t *o)
{
  /* ?? TODO: should be using opts_cittp_def.h here */

# define DUMP_OPT_INT(envstr, name)		\
  ci_log("%s=%d", (envstr), (int) o->name)
# define DUMP_OPT_HEX(envstr, name)		\
  ci_log("%s=%x", (envstr), (unsigned) o->name)

  DUMP_OPT_HEX("EF_UNIX_LOG",		log_level);
  DUMP_OPT_INT("EF_PROBE",		probe);
  DUMP_OPT_INT("EF_TCP",		ul_tcp);
  DUMP_OPT_INT("EF_UDP",		ul_udp);
  DUMP_OPT_INT("EF_UL_SELECT",		ul_select);
  DUMP_OPT_INT("EF_SELECT_SPIN",	ul_select_spin);
  DUMP_OPT_INT("EF_SELECT_FAST",	ul_select_fast);
  DUMP_OPT_INT("EF_UL_POLL",		ul_poll);
  DUMP_OPT_INT("EF_POLL_SPIN",		ul_poll_spin);
  DUMP_OPT_INT("EF_POLL_FAST",		ul_poll_fast);
  DUMP_OPT_INT("EF_POLL_FAST_USEC",	ul_poll_fast_usec);
  DUMP_OPT_INT("EF_POLL_NONBLOCK_FAST_USEC", ul_poll_nonblock_fast_usec);
#if CI_CFG_UDP
  DUMP_OPT_INT("EF_UDP_RECV_SPIN",      udp_recv_spin);
  DUMP_OPT_INT("EF_UDP_SEND_SPIN",      udp_send_spin);
#endif
  DUMP_OPT_INT("EF_TCP_RECV_SPIN",      tcp_recv_spin);
  DUMP_OPT_INT("EF_TCP_SEND_SPIN",      tcp_send_spin);
  DUMP_OPT_INT("EF_TCP_ACCEPT_SPIN",    tcp_accept_spin);
  DUMP_OPT_INT("EF_PKT_WAIT_SPIN",      pkt_wait_spin);
#if CI_CFG_USERSPACE_PIPE
  DUMP_OPT_INT("EF_PIPE_RECV_SPIN",     pipe_recv_spin);
  DUMP_OPT_INT("EF_PIPE_SEND_SPIN",     pipe_send_spin);
#endif
#if CI_CFG_USERSPACE_EPOLL
  DUMP_OPT_INT("EF_UL_EPOLL",	        ul_epoll);
  DUMP_OPT_INT("EF_EPOLL_SPIN",	        ul_epoll_spin);
  DUMP_OPT_INT("EF_EPOLL_CTL_FAST",     ul_epoll_ctl_fast);
  DUMP_OPT_INT("EF_EPOLL_CTL_HANDOFF",  ul_epoll_ctl_handoff);
  DUMP_OPT_INT("EF_EPOLL_MT_SAFE",      ul_epoll_mt_safe);
#endif
  DUMP_OPT_INT("EF_FDTABLE_SIZE",	fdtable_size);
  DUMP_OPT_INT("EF_SPIN_USEC",		ul_spin_usec);
  DUMP_OPT_INT("EF_STACK_PER_THREAD",	stack_per_thread);
  DUMP_OPT_INT("EF_DONT_ACCELERATE",	dont_accelerate);
  DUMP_OPT_INT("EF_FDTABLE_STRICT",	fdtable_strict);
  DUMP_OPT_INT("EF_FDS_MT_SAFE",	fds_mt_safe);
  DUMP_OPT_INT("EF_FORK_NETIF",		fork_netif);
  DUMP_OPT_INT("EF_NETIF_DTOR",		netif_dtor);
  DUMP_OPT_INT("EF_NO_FAIL",		no_fail);
  DUMP_OPT_INT("EF_SA_ONSTACK_INTERCEPT",	sa_onstack_intercept);
  DUMP_OPT_INT("EF_ACCEPT_INHERIT_NONBLOCK", accept_force_inherit_nonblock);
  DUMP_OPT_INT("EF_ACCEPT_INHERIT_NODELAY",  accept_force_inherit_nodelay);
#if CI_CFG_USERSPACE_PIPE
  DUMP_OPT_INT("EF_PIPE", ul_pipe);
#endif
}


static void citp_log_to_file(const char *s)
{
  int fd;
  ci_assert(!CITP_OPTS.log_via_ioctl);
  fd = open(s, O_WRONLY | O_CREAT | O_TRUNC, S_IREAD | S_IWRITE);
  if( fd >= 0 ) {
    if( citp.log_fd >= 0 )
      ci_sys_close(citp.log_fd);
    citp.log_fd = fd;
  }
}

static void citp_get_process_name(void)
{
  citp.process_name = citp.process_path;

  ci_sprintf(citp.process_path, "<unknown-proc>");

  {
    int n;

    n = readlink("/proc/self/exe", citp.process_path,
                 sizeof(citp.process_path));
    if (n < 0)
      return;

    n = CI_MIN(n + 1, sizeof(citp.process_path));
    citp.process_path[n - 1] = '\0';
    citp.process_name = citp.process_path + n - 2;
    while (citp.process_name > citp.process_path &&
           citp.process_name[-1] != '/')
      --citp.process_name;
  }

}


static int get_env_opt_int(const char* name, int old_val, int hex)
{ const char* s;
  int new_val;
  char dummy;
  if( (s = getenv(name)) ) {
    if( sscanf(s, hex ? "%x %c" : "%d %c", &new_val, &dummy) == 1 )
      /*! TODO: should use option value range checking here */
      return new_val;
    else if (s[0] != '\0')
      ci_log("citp: bad option '%s=%s'", name, s);
  }
  return old_val;
}


#define GET_ENV_OPT_INT(envstr, var)					\
  do{ opts->var = get_env_opt_int((envstr), opts->var, 0); }while(0)

#define GET_ENV_OPT_HEX(envstr, var)					\
  do{ opts->var = get_env_opt_int((envstr), opts->var, 1); }while(0)


static void citp_opts_getenv(citp_opts_t* opts)
{
  /* ?? TODO: would like to use opts_citp_def.h here */

  const char* s;
  unsigned v;

  opts->log_via_ioctl = 3;
  /* TODO: Old name.  Keeping reading 'til 2011, then purge. */
  GET_ENV_OPT_HEX("EF_Log_VIA_IOCTL",	log_via_ioctl);
  GET_ENV_OPT_INT("EF_LOG_VIA_IOCTL",	log_via_ioctl);

  if( (s = getenv("EF_LOG_FILE")) && opts->log_via_ioctl == 3) {
    opts->log_via_ioctl = 0;
    citp_log_to_file(s);
  } else if( opts->log_via_ioctl == 3 ) {
    /* citp_setup_logging_early() have already detected stderr as
     * tty/non-tty, so just trust it. */
    if( ci_log_fn == citp_log_fn_drv )
      opts->log_via_ioctl = 1;
    else
      opts->log_via_ioctl = 0;
  }

  if( opts->log_via_ioctl ) {
    ci_log_options &=~ CI_LOG_PID;
    citp_setup_logging_change(citp_log_fn_drv);
  } else {
    if( getenv("EF_LOG_TIMESTAMPS") )
      ci_log_options |= CI_LOG_TIME;
    citp_setup_logging_change(citp_log_fn_ul);
  }

  if( getenv("EF_POLL_NONBLOCK_FAST_LOOPS") &&
      ! getenv("EF_POLL_NONBLOCK_FAST_USEC") )
    log("ERROR: EF_POLL_NONBLOCK_FAST_LOOPS is deprecated, use"
        " EF_POLL_NONBLOCK_FAST_USEC instead");

  if( getenv("EF_POLL_FAST_LOOPS") && ! getenv("EF_POLL_FAST_USEC") )
    log("ERROR: EF_POLL_FAST_LOOPS is deprecated, use"
        " EF_POLL_FAST_USEC instead");

  if( (s = getenv("EF_POLL_USEC")) && atoi(s) ) {
    GET_ENV_OPT_INT("EF_POLL_USEC", ul_spin_usec);
    opts->ul_select_spin = 1;
    opts->ul_poll_spin = 1;
#if CI_CFG_USERSPACE_EPOLL
    opts->ul_epoll_spin = 1;
#endif
#if CI_CFG_UDP
    opts->udp_recv_spin = 1;
    opts->udp_send_spin = 1;
#endif
    opts->tcp_recv_spin = 1;
    opts->tcp_send_spin = 1;
    opts->pkt_wait_spin = 1;
  }

  GET_ENV_OPT_HEX("EF_UNIX_LOG",	log_level);
  GET_ENV_OPT_INT("EF_PROBE",		probe);
  GET_ENV_OPT_INT("EF_TCP",		ul_tcp);
  GET_ENV_OPT_INT("EF_UDP",		ul_udp);
  GET_ENV_OPT_INT("EF_UL_SELECT",	ul_select);
  GET_ENV_OPT_INT("EF_SELECT_SPIN",	ul_select_spin);
  GET_ENV_OPT_INT("EF_SELECT_FAST",	ul_select_fast);
  GET_ENV_OPT_INT("EF_UL_POLL",		ul_poll);
  GET_ENV_OPT_INT("EF_POLL_SPIN",	ul_poll_spin);
  GET_ENV_OPT_INT("EF_POLL_FAST",	ul_poll_fast);
  GET_ENV_OPT_INT("EF_POLL_FAST_USEC",  ul_poll_fast_usec);
  GET_ENV_OPT_INT("EF_POLL_NONBLOCK_FAST_USEC", ul_poll_nonblock_fast_usec);
#if CI_CFG_UDP
  GET_ENV_OPT_INT("EF_UDP_RECV_SPIN",   udp_recv_spin);
  GET_ENV_OPT_INT("EF_UDP_SEND_SPIN",   udp_send_spin);
#endif
  GET_ENV_OPT_INT("EF_TCP_RECV_SPIN",   tcp_recv_spin);
  GET_ENV_OPT_INT("EF_TCP_SEND_SPIN",   tcp_send_spin);
  GET_ENV_OPT_INT("EF_TCP_ACCEPT_SPIN", tcp_accept_spin);
  GET_ENV_OPT_INT("EF_PKT_WAIT_SPIN",   pkt_wait_spin);
#if CI_CFG_USERSPACE_PIPE
  GET_ENV_OPT_INT("EF_PIPE_RECV_SPIN",  pipe_recv_spin);
  GET_ENV_OPT_INT("EF_PIPE_SEND_SPIN",  pipe_send_spin);
#endif
#if CI_CFG_USERSPACE_EPOLL
  GET_ENV_OPT_INT("EF_UL_EPOLL",        ul_epoll);
  GET_ENV_OPT_INT("EF_EPOLL_SPIN",      ul_epoll_spin);
  GET_ENV_OPT_INT("EF_EPOLL_CTL_FAST",  ul_epoll_ctl_fast);
  GET_ENV_OPT_INT("EF_EPOLL_CTL_HANDOFF",ul_epoll_ctl_handoff);
  GET_ENV_OPT_INT("EF_EPOLL_MT_SAFE",   ul_epoll_mt_safe);
#endif
  GET_ENV_OPT_INT("EF_FDTABLE_SIZE",	fdtable_size);
  GET_ENV_OPT_INT("EF_SPIN_USEC",	ul_spin_usec);
  GET_ENV_OPT_INT("EF_STACK_PER_THREAD",stack_per_thread);
  GET_ENV_OPT_INT("EF_DONT_ACCELERATE",	dont_accelerate);
  GET_ENV_OPT_INT("EF_FDTABLE_STRICT",	fdtable_strict);
  GET_ENV_OPT_INT("EF_FDS_MT_SAFE",	fds_mt_safe);
  GET_ENV_OPT_INT("EF_NO_FAIL",		no_fail);
  GET_ENV_OPT_INT("EF_SA_ONSTACK_INTERCEPT",	sa_onstack_intercept);
  GET_ENV_OPT_INT("EF_ACCEPT_INHERIT_NONBLOCK",	accept_force_inherit_nonblock);
  GET_ENV_OPT_INT("EF_ACCEPT_INHERIT_NODELAY",	accept_force_inherit_nodelay);
#if CI_CFG_USERSPACE_PIPE
  GET_ENV_OPT_INT("EF_PIPE",        ul_pipe);
#endif

  if( (s = getenv("EF_FORK_NETIF")) && sscanf(s, "%x", &v) == 1 ) {
    opts->fork_netif = CI_MIN(v, CI_UNIX_FORK_NETIF_BOTH);
  }
  if( (s = getenv("EF_NETIF_DTOR")) && sscanf(s, "%x", &v) == 1 ) {
    opts->netif_dtor = CI_MIN(v, CITP_NETIF_DTOR_ALL);
  }

}


static int
citp_cfg_init(void)
{
  int cfgerr = 0;
 /* FIXME: if return code is non-zero, must not allow
           no-intercept to be overriden by environment variable */
  ci_cfg_query(NULL, &cfgerr);
  return 0;
}


static int
citp_transport_init(void)
{
  const char* s;

  citp_get_process_name();
  citp_setup_logging_prefix();

  CITP_OPTS.load_env = 1;
  if( (s = getenv("EF_LOAD_ENV")) )
    CITP_OPTS.load_env = atoi(s);
  if( CITP_OPTS.load_env )
    citp_opts_getenv(&CITP_OPTS);

  /* NB. We only look at EF_CONFIG_DUMP if EF_LOAD_ENV. */
  if( CITP_OPTS.load_env && getenv("EF_CONFIG_DUMP") ) {
    citp_dump_opts(&CITP_OPTS);
    citp_dump_config();
    /* ?? ci_netif_config_opts_dump(&citp.netif_opts); */
  }

  ci_get_cpu_khz(NULL);
  citp.spin_cycles = ci_usec_to_cycles64(CITP_OPTS.ul_spin_usec);
  citp.poll_nonblock_fast_cycles = 
    ci_usec_to_cycles64(CITP_OPTS.ul_poll_nonblock_fast_usec);
  citp.poll_fast_cycles = 
    ci_usec_to_cycles64(CITP_OPTS.ul_poll_fast_usec);
  ci_tp_init(__oo_per_thread_init_thread);
  return 0;
}


static int citp_transport_register(void)
{
  if( CITP_OPTS.ul_tcp )
    citp_protocol_manager_add(&citp_tcp_protocol_impl, 1);
  if( CITP_OPTS.ul_udp )
    citp_protocol_manager_add(&citp_udp_protocol_impl, 0);
  return 0;
}


int _citp_do_init_inprogress = 0;

typedef int (*cipt_init_func_t)(void);
cipt_init_func_t cipt_init_funcs[] =
{
#define STARTUP_ITEM(level, func) func,
#include "startup_order.h"
#undef STARTUP_ITEM
};

int citp_do_init(int max_init_level)
{
  int rc = 0;
  int level;
  int saved_errno = errno;

  _citp_do_init_inprogress++;

  for (level = citp.init_level;
       level < CI_MIN(max_init_level, CITP_INIT_ALL);
       level++) {
    rc = cipt_init_funcs[level]();
    if (rc < 0)
      break;
    citp.init_level = level + 1;
  }

  --_citp_do_init_inprogress;
  Log_S(log("%s: reached level %d", __FUNCTION__, citp.init_level));
  if( rc == 0 )
    errno = saved_errno;
  return rc;
}

void _init(void)
{
  /* must not do any logging yet... */
  if( citp_do_init(CITP_INIT_ALL) < 0 )
    ci_fail(("EtherFabric transport library: failed to initialise (%d)",
             citp.init_level));

  Log_S(log("citp: initialisation done."));
}


void _fini(void)
{
  Log_S(log("citp: finishing up"));
}


/* This is called if the library is run as an executable!
   Ensure that no libc() functions are used */
void onload_version_msg(void)
{
  struct iovec v[1];
  static const char msg0[] =
    ONLOAD_PRODUCT" "ONLOAD_VERSION"\n"
    ONLOAD_COPYRIGHT"\n"
    "Built: "__DATE__" "__TIME__" "
#ifdef NDEBUG
    "(release)"
#else
    "(debug)"
#endif
    "\n";

  v[0].iov_base = (void*) msg0;
  v[0].iov_len  = sizeof(msg0)-1;

  my_syscall3(writev, STDOUT_FILENO, (long) v, 1);
  my_syscall3(exit, 0, 0, 0); 
}


const char*const onload_version = ONLOAD_VERSION;

/*! \cidoxg_end */