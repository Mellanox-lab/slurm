/*****************************************************************************\
 *  rpc_mgr.h - functions for processing RPCs.
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2009 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include "src/common/fd.h"
#include "src/common/log.h"
#include "src/common/macros.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_accounting_storage.h"
#include "src/common/slurmdbd_defs.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"
#include "src/slurmdbd/proc_req.h"
#include "src/slurmdbd/read_config.h"
#include "src/slurmdbd/rpc_mgr.h"
#include "src/slurmdbd/slurmdbd.h"

#define MAX_THREAD_COUNT 100

/*
 *  Maximum message size. Messages larger than this value (in bytes)
 *  will not be received.
 */
#define MAX_MSG_SIZE     (16*1024*1024)

/* Local functions */
static bool   _fd_readable(int fd);
static void   _free_server_thread(pthread_t my_tid);
static void * _service_connection(void *arg);
static void   _sig_handler(int signal);
static int    _wait_for_server_thread(void);
static void   _wait_for_thread_fini(void);

/* Local variables */
static pthread_t       master_thread_id = 0, slave_thread_id[MAX_THREAD_COUNT];
static int             thread_count = 0;
static pthread_mutex_t thread_count_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  thread_count_cond = PTHREAD_COND_INITIALIZER;


/* Process incoming RPCs. Meant to execute as a pthread */
extern void *rpc_mgr(void *no_data)
{
	pthread_attr_t thread_attr_rpc_req;
	int sockfd, newsockfd;
	int i, retry_cnt, sigarray[] = {SIGUSR1, 0};
	slurm_addr_t cli_addr;
	slurmdbd_conn_t *conn_arg = NULL;

	slurm_mutex_lock(&thread_count_lock);
	master_thread_id = pthread_self();
	slurm_mutex_unlock(&thread_count_lock);

	(void) pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	(void) pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	/* threads to process individual RPC's are detached */
	slurm_attr_init(&thread_attr_rpc_req);
	if (pthread_attr_setdetachstate
	    (&thread_attr_rpc_req, PTHREAD_CREATE_DETACHED))
		fatal("pthread_attr_setdetachstate %m");

	/* initialize port for RPCs */
	if ((sockfd = slurm_init_msg_engine_port(get_dbd_port()))
	    == SLURM_SOCKET_ERROR)
		fatal("slurm_init_msg_engine_port error %m");

	/* Prepare to catch SIGUSR1 to interrupt accept().
	 * This signal is generated by the slurmdbd signal
	 * handler thread upon receipt of SIGABRT, SIGINT,
	 * or SIGTERM. That thread does all processing of
	 * all signals. */
	xsignal(SIGUSR1, _sig_handler);
	xsignal_unblock(sigarray);

	/*
	 * Process incoming RPCs until told to shutdown
	 */
	while ((i = _wait_for_server_thread()) >= 0) {
		uint16_t orig_port;
		/*
		 * accept needed for stream implementation is a no-op in
		 * message implementation that just passes sockfd to newsockfd
		 */
		if ((newsockfd = slurm_accept_msg_conn(sockfd,
						       &cli_addr)) ==
		    SLURM_SOCKET_ERROR) {
			_free_server_thread((pthread_t) 0);
			if (errno != EINTR)
				error("slurm_accept_msg_conn: %m");
			continue;
		}
		fd_set_nonblocking(newsockfd);

		conn_arg = xmalloc(sizeof(slurmdbd_conn_t));
		conn_arg->conn.fd = newsockfd;
		conn_arg->conn.shutdown = &shutdown_time;
		conn_arg->conn.version = SLURM_MIN_PROTOCOL_VERSION;
		conn_arg->conn.rem_host = xmalloc_nz(sizeof(char) * 32);
		slurm_get_ip_str(&cli_addr, &orig_port,
				 conn_arg->conn.rem_host, sizeof(char) * 32);
		retry_cnt = 0;
		while (pthread_create(&slave_thread_id[i],
				      &thread_attr_rpc_req,
				      _service_connection,
				      (void *) conn_arg)) {
			if (retry_cnt > 0) {
				error("pthread_create failure, "
				      "aborting RPC: %m");
				close(newsockfd);
				break;
			}
			error("pthread_create failure: %m");
			retry_cnt++;
			usleep(1000);	/* retry in 1 msec */
		}
	}

	debug3("rpc_mgr shutting down");
	slurm_attr_destroy(&thread_attr_rpc_req);
	(void) slurm_shutdown_msg_engine(sockfd);
	_wait_for_thread_fini();
	pthread_exit((void *) 0);
	return NULL;
}

/* Wake up the RPC manager and all spawned threads so they can exit */
extern void rpc_mgr_wake(void)
{
	int i;

	slurm_mutex_lock(&thread_count_lock);
	if (master_thread_id)
		pthread_kill(master_thread_id, SIGUSR1);
	for (i=0; i<MAX_THREAD_COUNT; i++) {
		if (slave_thread_id[i])
			pthread_kill(slave_thread_id[i], SIGUSR1);
	}
	slurm_mutex_unlock(&thread_count_lock);
}

static void * _service_connection(void *arg)
{
	slurmdbd_conn_t *conn = (slurmdbd_conn_t *) arg;
	uint32_t nw_size = 0, msg_size = 0, uid = NO_VAL;
	char *msg = NULL;
	ssize_t msg_read = 0, offset = 0;
	bool fini = false, first = true;
	Buf buffer = NULL;
	int rc = SLURM_SUCCESS;
	int fd = conn->conn.fd;

	debug2("Opened connection %d from %s", conn->conn.fd,
	       conn->conn.rem_host);

	while (!fini) {
		if (!_fd_readable(conn->conn.fd))
			break;		/* problem with this socket */
		msg_read = read(conn->conn.fd, &nw_size, sizeof(nw_size));
		if (msg_read == 0)	/* EOF */
			break;
		if (msg_read != sizeof(nw_size)) {
			error("Could not read msg_size from "
			      "connection %d(%s) uid(%d)",
			      conn->conn.fd, conn->conn.rem_host, uid);
			break;
		}
		msg_size = ntohl(nw_size);
		if ((msg_size < 2) || (msg_size > MAX_MSG_SIZE)) {
			error("Invalid msg_size (%u) from "
			      "connection %d(%s) uid(%d)",
			      msg_size, conn->conn.fd,
			      conn->conn.rem_host, uid);
			break;
		}

		msg = xmalloc(msg_size);
		offset = 0;
		while (msg_size > offset) {
			if (!_fd_readable(conn->conn.fd))
				break;		/* problem with this socket */
			msg_read = read(conn->conn.fd, (msg + offset),
					(msg_size - offset));
			if (msg_read <= 0) {
				error("read(%d): %m", conn->conn.fd);
				break;
			}
			offset += msg_read;
		}
		if (msg_size == offset) {
			rc = proc_req(
				conn, msg, msg_size, first, &buffer, &uid);
			first = false;
			if (rc != SLURM_SUCCESS && rc != ACCOUNTING_FIRST_REG) {
				error("Processing last message from "
				      "connection %d(%s) uid(%d)",
				      conn->conn.fd, conn->conn.rem_host, uid);
				if (rc == ESLURM_ACCESS_DENIED
				    || rc == SLURM_PROTOCOL_VERSION_ERROR)
					fini = true;
			}
		} else {
			buffer = slurm_persist_make_rc_msg(
				&conn->conn, SLURM_ERROR, "Bad offset", 0);
			fini = true;
		}

		if (slurm_persist_send_msg(&conn->conn, buffer)
		    != SLURM_SUCCESS) {
			/* This is only an issue on persistent connections, and
			 * really isn't that big of a deal as the slurmctld
			 * will just send the message again. */
			if (conn->conn.rem_port)
				debug("Problem sending response to "
				      "connection %d(%s) uid(%d)",
				      conn->conn.fd, conn->conn.rem_host, uid);
			fini = true;
		}
		free_buf(buffer);
		xfree(msg);
	}

	if (conn->conn.rem_port) {
		if (!shutdown_time) {
			slurmdb_cluster_rec_t cluster_rec;
			ListIterator itr;
			slurmdbd_conn_t *slurmdbd_conn;
			memset(&cluster_rec, 0, sizeof(slurmdb_cluster_rec_t));
			cluster_rec.name = conn->conn.cluster_name;
			cluster_rec.control_host = conn->conn.rem_host;
			cluster_rec.control_port = conn->conn.rem_port;
			cluster_rec.tres_str = conn->tres_str;
			debug("cluster %s has disconnected",
			      conn->conn.cluster_name);

			clusteracct_storage_g_fini_ctld(
				conn->db_conn, &cluster_rec);

			slurm_mutex_lock(&registered_lock);
			itr = list_iterator_create(registered_clusters);
			while ((slurmdbd_conn = list_next(itr))) {
				if (conn == slurmdbd_conn) {
					list_delete_item(itr);
					break;
				}
			}
			list_iterator_destroy(itr);
			slurm_mutex_unlock(&registered_lock);
		}
		/* needs to be the last thing done */
		acct_storage_g_commit(conn->db_conn, 1);
	}

	acct_storage_g_close_connection(&conn->db_conn);
	slurm_persist_conn_members_destroy(&conn->conn);

	debug2("Closed connection %d uid(%d)", fd, uid);

	xfree(conn->tres_str);
	xfree(conn);
	_free_server_thread(pthread_self());
	return NULL;
}

/* Wait until a file is readable, return false if can not be read */
static bool _fd_readable(int fd)
{
	struct pollfd ufds;
	int rc;

	ufds.fd     = fd;
	ufds.events = POLLIN;
	while (1) {
		rc = poll(&ufds, 1, -1);
		if (shutdown_time)
			return false;
		if (rc == -1) {
			if ((errno == EINTR) || (errno == EAGAIN))
				continue;
			error("poll: %m");
			return false;
		}
		if ((ufds.revents & POLLHUP) &&
		    ((ufds.revents & POLLIN) == 0)) {
			debug3("Read connection %d closed", fd);
			return false;
		}
		if (ufds.revents & POLLNVAL) {
			error("Connection %d is invalid", fd);
			return false;
		}
		if (ufds.revents & POLLERR) {
			error("Connection %d experienced an error", fd);
			return false;
		}
		if ((ufds.revents & POLLIN) == 0) {
			error("Connection %d events %d", fd, ufds.revents);
			return false;
		}
		break;
	}
	errno = 0;
	return true;
}

/* Increment thread_count and don't return until its value is no larger
 *	than MAX_THREAD_COUNT,
 * RET index of free index in slave_pthread_id or -1 to exit */
static int _wait_for_server_thread(void)
{
	bool print_it = true;
	int i, rc = -1;

	slurm_mutex_lock(&thread_count_lock);
	while (1) {
		if (shutdown_time)
			break;

		if (thread_count < MAX_THREAD_COUNT) {
			thread_count++;
			for (i=0; i<MAX_THREAD_COUNT; i++) {
				if (slave_thread_id[i])
					continue;
				rc = i;
				break;
			}
			if (rc == -1) {
				/* thread_count and slave_thread_id
				 * out of sync */
				fatal("No free slave_thread_id");
			}
			break;
		} else {
			/* wait for state change and retry,
			 * just a delay and not an error.
			 * This can happen when the epilog completes
			 * on a bunch of nodes at the same time, which
			 * can easily happen for highly parallel jobs. */
			if (print_it) {
				static time_t last_print_time = 0;
				time_t now = time(NULL);
				if (difftime(now, last_print_time) > 2) {
					verbose("thread_count over "
						"limit (%d), waiting",
						thread_count);
					last_print_time = now;
				}
				print_it = false;
			}
			slurm_cond_wait(&thread_count_cond, &thread_count_lock);
		}
	}
	slurm_mutex_unlock(&thread_count_lock);
	return rc;
}

/* my_tid IN - Thread ID of spawned thread, 0 if no thread spawned */
static void _free_server_thread(pthread_t my_tid)
{
	int i;

	slurm_mutex_lock(&thread_count_lock);
	if (thread_count > 0)
		thread_count--;
	else
		error("thread_count underflow");

	if (my_tid) {
		for (i=0; i<MAX_THREAD_COUNT; i++) {
			if (slave_thread_id[i] != my_tid)
				continue;
			slave_thread_id[i] = (pthread_t) 0;
			break;
		}
		if (i >= MAX_THREAD_COUNT)
			error("Could not find slave_thread_id");
	}

	slurm_cond_broadcast(&thread_count_cond);
	slurm_mutex_unlock(&thread_count_lock);
}

/* Wait for all RPC handler threads to exit.
 * After one second, start sending SIGKILL to the threads. */
static void _wait_for_thread_fini(void)
{
	int j;

	if (thread_count == 0)
		return;
	usleep(500000);	/* Give the threads 500 msec to clean up */

	/* Interupt any hung I/O */
	slurm_mutex_lock(&thread_count_lock);
	for (j=0; j<MAX_THREAD_COUNT; j++) {
		if (slave_thread_id[j] == 0)
			continue;
		pthread_kill(slave_thread_id[j], SIGUSR1);
	}
	slurm_mutex_unlock(&thread_count_lock);

	/* Can't send SIGKILL to threads as it goes to the process. Since this
	 * is called only when the rpc_mgr is quitting, just let the threads die
	 * by the dbd dying.  If it's the backup and it's giving up control, let
	 * the threads finish. thread_count will be decremented when the thread
	 * finishes -- even if the rpc_mgr is gone.
	 */
	/*usleep(100000); */	/* Give the threads 100 msec to clean up */
	/*
	for (i=0; ; i++) {
		if (thread_count == 0)
			return;

		slurm_mutex_lock(&thread_count_lock);
		for (j=0; j<MAX_THREAD_COUNT; j++) {
			if (slave_thread_id[j] == 0)
				continue;
			info("rpc_mgr sending SIGKILL to thread %lu",
			     (unsigned long) slave_thread_id[j]);
			if (pthread_kill(slave_thread_id[j], SIGKILL)) {
				slave_thread_id[j] = 0;
				if (thread_count > 0)
					thread_count--;
				else
					error("thread_count underflow");
			}
		}
		slurm_mutex_unlock(&thread_count_lock);
		sleep(1);
	}
	*/
}

static void _sig_handler(int signal)
{
}
