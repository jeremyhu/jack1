/*
    Copyright (C) 2001 Paul Davis
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id$
*/

#include <unistd.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/ipc.h>
#include <signal.h>
#include <sys/types.h>
#include <string.h>
#include <limits.h>
#include <sys/mman.h>

#include <asm/msr.h>

#include <jack/internal.h>
#include <jack/engine.h>
#include <jack/driver.h>

#define MAX_SHM_ID 256 /* likely use is more like 16 */

typedef struct {

    jack_port_internal_t *source;
    jack_port_internal_t *destination;

} jack_connection_internal_t;

typedef struct _jack_client_internal {

    jack_client_control_t *control;

    int      request_fd;
    int      event_fd;
    int      subgraph_start_fd;
    int      subgraph_wait_fd;
    GSList  *ports;    /* protected by engine->client_lock */
    GSList  *fed_by;   /* protected by engine->client_lock */
    int      shm_id;
    int      shm_key;
    unsigned long execution_order;
    struct _jack_client_internal *next_client; /* not a linked list! */
    dlhandle handle;
    int      error;

} jack_client_internal_t;

static int                    jack_port_assign_buffer (jack_engine_t *, jack_port_internal_t *);
static jack_port_internal_t *jack_get_port_by_name (jack_engine_t *, const char *name);

static void jack_client_delete (jack_engine_t *, jack_client_internal_t *);
static void jack_remove_client (jack_engine_t *engine, jack_client_internal_t *client);

static jack_client_internal_t *jack_client_internal_new (jack_engine_t *engine, int fd, jack_client_connect_request_t *);
static jack_client_internal_t *jack_client_internal_by_id (jack_engine_t *engine, jack_client_id_t id);

static void jack_sort_graph (jack_engine_t *engine);
static int  jack_rechain_graph (jack_engine_t *engine);
static int  jack_get_fifo_fd (jack_engine_t *engine, int which_fifo);
static void jack_clear_fifos (jack_engine_t *engine);

static int  jack_port_do_connect (jack_engine_t *engine, const char *source_port, const char *destination_port);
static int  jack_port_do_disconnect (jack_engine_t *engine, const char *source_port, const char *destination_port);
static int  jack_port_do_disconnect_all (jack_engine_t *engine, jack_port_id_t);

static int  jack_port_do_unregister (jack_engine_t *engine, jack_request_t *);
static int  jack_port_do_register (jack_engine_t *engine, jack_request_t *);
static void jack_port_release (jack_engine_t *engine, jack_port_internal_t *);
static void jack_port_clear_connections (jack_engine_t *engine, jack_port_internal_t *port);
static int  jack_port_disconnect_internal (jack_engine_t *engine, jack_port_internal_t *src, 
					    jack_port_internal_t *dst, int sort_graph);

static void jack_port_registration_notify (jack_engine_t *, jack_port_id_t, int);
static int  jack_send_connection_notification (jack_engine_t *, jack_client_id_t, jack_port_id_t, jack_port_id_t, int);
static int  jack_deliver_event (jack_engine_t *, jack_client_internal_t *, jack_event_t *);

static int  jack_engine_process_lock (jack_engine_t *);
static void jack_engine_process_unlock (jack_engine_t *);
static int  jack_engine_post_process (jack_engine_t *);

static int *jack_shm_registry;
static int  jack_shm_id_cnt;

static char *client_state_names[] = {
	"Not triggered",
	"Triggered",
	"Running",
	"Finished"
};

static inline int 
jack_client_is_inprocess (jack_client_internal_t *client)
{
	return (client->control->type == ClientDynamic) || (client->control->type == ClientDriver);
}

static inline void
jack_lock_graph (jack_engine_t *engine)
{
	pthread_mutex_lock (&engine->client_lock);
}

static inline void
jack_unlock_graph (jack_engine_t *engine)
{
	pthread_mutex_unlock (&engine->client_lock);
}

static int
make_sockets (int fd[2])
{
	struct sockaddr_un addr;
	int i;

	/* First, the master server socket */

	if ((fd[0] = socket (AF_UNIX, SOCK_STREAM, 0)) < 0) {
		jack_error ("cannot create server socket (%s)", strerror (errno));
		return -1;
	}

	addr.sun_family = AF_UNIX;
	for (i = 0; i < 999; i++) {
		snprintf (addr.sun_path, sizeof (addr.sun_path) - 1, "%s/jack_%d", jack_temp_dir, i);
		if (access (addr.sun_path, F_OK) != 0) {
			break;
		}
	}

	if (i == 999) {
		jack_error ("all possible server socket names in use!!!");
		close (fd[0]);
		return -1;
	}

	if (bind (fd[0], (struct sockaddr *) &addr, sizeof (addr)) < 0) {
		jack_error ("cannot bind server to socket (%s)", strerror (errno));
		close (fd[0]);
		return -1;
	}

	if (listen (fd[0], 1) < 0) {
		jack_error ("cannot enable listen on server socket (%s)", strerror (errno));
		close (fd[0]);
		return -1;
	}

	/* Now the client/server event ack server socket */

	if ((fd[1] = socket (AF_UNIX, SOCK_STREAM, 0)) < 0) {
		jack_error ("cannot create event ACK socket (%s)", strerror (errno));
		close (fd[0]);
		return -1;
	}

	addr.sun_family = AF_UNIX;
	for (i = 0; i < 999; i++) {
		snprintf (addr.sun_path, sizeof (addr.sun_path) - 1, "%s/jack_ack_%d", jack_temp_dir, i);
		if (access (addr.sun_path, F_OK) != 0) {
			break;
		}
	}

	if (i == 999) {
		jack_error ("all possible server ACK socket names in use!!!");
		close (fd[0]);
		close (fd[1]);
		return -1;
	}

	if (bind (fd[1], (struct sockaddr *) &addr, sizeof (addr)) < 0) {
		jack_error ("cannot bind server to socket (%s)", strerror (errno));
		close (fd[0]);
		close (fd[1]);
		return -1;
	}

	if (listen (fd[1], 1) < 0) {
		jack_error ("cannot enable listen on server socket (%s)", strerror (errno));
		close (fd[0]);
		close (fd[1]);
		return -1;
	}

	return 0;
}

static int
jack_initialize_shm ()
{
	int shmid_id;
	void *addr;

	if (jack_shm_registry != NULL) {
		return 0;
	}

	/* grab a chunk of memory to store shm ids in. this is 
	   to allow our parent to clean up all such ids when
	   if we exit. otherwise, they can get lost in crash
	   or debugger driven exits.
	*/
	
	if ((shmid_id = shmget (random(), sizeof(int) * MAX_SHM_ID, IPC_CREAT|0600)) < 0) {
		jack_error ("cannot create engine shm ID registry (%s)", strerror (errno));
		return -1;
	}
	if ((addr = shmat (shmid_id, 0, 0)) == (void *) -1) {
		jack_error ("cannot attach shm ID registry (%s)", strerror (errno));
		shmctl (shmid_id, IPC_RMID, 0);
		return -1;
	}
	if (shmctl (shmid_id, IPC_RMID, NULL)) {
		jack_error ("cannot mark shm ID registry as destroyed (%s)", strerror (errno));
		return -1;
	}

	jack_shm_registry = (int *) addr;
	jack_shm_id_cnt = 0;

	return 0;
}

static void
jack_register_shm (int shmid)
{
	if (jack_shm_id_cnt < MAX_SHM_ID) {
		jack_shm_registry[jack_shm_id_cnt++] = shmid;
	}
}

void
jack_cleanup_shm ()
{
	int i;

	for (i = 0; i < jack_shm_id_cnt; i++) {
		shmctl (jack_shm_registry[i], IPC_RMID, NULL);
	}
}

void
jack_cleanup_files ()
{
	DIR *dir;
	struct dirent *dirent;

	/* its important that we remove all files that jackd creates
	   because otherwise subsequent attempts to start jackd will
	   believe that an instance is already running.
	*/

	if ((dir = opendir (jack_temp_dir)) == NULL) {
		fprintf (stderr, "jack(%d): cannot open jack FIFO directory (%s)\n", getpid(), strerror (errno));
		return;
	}

	while ((dirent = readdir (dir)) != NULL) {
		if (strncmp (dirent->d_name, "jack-", 5) == 0 || strncmp (dirent->d_name, "jack_", 5) == 0) {
			char fullpath[PATH_MAX+1];
			sprintf (fullpath, "%s/%s", jack_temp_dir, dirent->d_name);
			unlink (fullpath);
		} 
	}

	closedir (dir);
}

static int
jack_add_port_segment (jack_engine_t *engine, unsigned long nports)

{
	jack_port_segment_info_t *si;
	key_t key;
	int id;
	char *addr;
	int offset;
	size_t size;
	size_t step;

	key = random();
	size = nports * sizeof (sample_t) * engine->control->buffer_size;

	if ((id = shmget (key, size, IPC_CREAT|0666)) < 0) {
		jack_error ("cannot create new port segment of %d bytes, key = 0x%x (%s)", size, key, strerror (errno));
		return -1;
	}
	
	jack_register_shm (id);

	if ((addr = shmat (id, 0, 0)) == (char *) -1) {
		jack_error ("cannot attach new port segment (%s)", strerror (errno));
		shmctl (id, IPC_RMID, 0);
		return -1;
	}
		
	si = (jack_port_segment_info_t *) malloc (sizeof (jack_port_segment_info_t));
	si->shm_key = key;
	si->address = addr;

	engine->port_segments = g_slist_prepend (engine->port_segments, si);
	engine->port_segment_key = key; /* XXX fix me */
	engine->port_segment_address = addr; /* XXX fix me */

	pthread_mutex_lock (&engine->buffer_lock);

	offset = 0;

	step = engine->control->buffer_size * sizeof (sample_t);

	while (offset < size) {
		jack_port_buffer_info_t *bi;

		bi = (jack_port_buffer_info_t *) malloc (sizeof (jack_port_buffer_info_t));
		bi->shm_key = key;
		bi->offset = offset;

		/* we append because we want the list to be in memory-address order */

		engine->port_buffer_freelist = g_slist_append (engine->port_buffer_freelist, bi);

		offset += step;
	}

	/* convert the first chunk of the segment into a zero-filled area */

	if (engine->silent_buffer == NULL) {
		engine->silent_buffer = (jack_port_buffer_info_t *) engine->port_buffer_freelist->data;

		engine->port_buffer_freelist = g_slist_remove_link (engine->port_buffer_freelist, engine->port_buffer_freelist);

		memset (engine->port_segment_address + engine->silent_buffer->offset, 0, 
			sizeof (sample_t) * engine->control->buffer_size);
	}

	pthread_mutex_unlock (&engine->buffer_lock);

	/* XXX notify all clients of new segment */

	return 0;
}

static int
jack_set_buffer_size (jack_engine_t *engine, nframes_t nframes)
{
	/* XXX this is not really right, since it only works for
	   audio ports. it also doesn't resize the zero filled
	   area.
	*/

	engine->control->buffer_size = nframes;
	jack_add_port_segment (engine, engine->control->port_max);
	return 0;
}

static int
jack_set_sample_rate (jack_engine_t *engine, nframes_t nframes)

{
	engine->control->time.frame_rate = nframes;
	return 0;
}

int
jack_engine_process_lock (jack_engine_t *engine)
{
	return pthread_mutex_trylock (&engine->client_lock);
}

void
jack_engine_process_unlock (jack_engine_t *engine)
{
	pthread_mutex_unlock (&engine->client_lock);
}

static int
jack_process (jack_engine_t *engine, nframes_t nframes)
{
	jack_client_internal_t *client;
	jack_client_control_t *ctl;
	GSList *node;
	char c;
	int status;

	engine->process_errors = 0;

	for (node = engine->clients; node; node = g_slist_next (node)) {
		ctl = ((jack_client_internal_t *) node->data)->control;
		ctl->state = NotTriggered;
		ctl->nframes = nframes;
	}

	if (engine->timebase_client) {
		engine->control->time.frame = engine->timebase_client->control->frame_time;
	} 

	for (node = engine->clients; engine->process_errors == 0 && node; ) {

		client = (jack_client_internal_t *) node->data;

		if (!client->control->active || client->control->dead) {
			node = g_slist_next (node);
			continue;
		}

		ctl = client->control;

		if (jack_client_is_inprocess (client)) {

			/* in-process client ("plugin") */

			if (ctl->process) {

				ctl->state = Running;

				if (ctl->process (nframes, ctl->process_arg) == 0) {
					ctl->state = Finished;
				} else {
					jack_error ("in-process client %s failed", client->control->name);
					engine->process_errors++;
					break;
				}

			} else {
				ctl->state = Finished;
			}

			node = g_slist_next (node);

		} else {

			/* out of process subgraph */

			ctl->state = Triggered; // a race exists if we do this after the write(2) 
			ctl->timed_out = 0;

			if (write (client->subgraph_start_fd, &c, sizeof (c)) != sizeof (c)) {
				jack_error ("cannot initiate graph processing (%s)", strerror (errno));
				engine->process_errors++;
				break;
			} 

			if (engine->asio_mode) {
				engine->driver->wait (engine->driver, client->subgraph_wait_fd, &status);
			} else {
				struct pollfd pfd[1];

				pfd[0].fd = client->subgraph_wait_fd;
				pfd[0].events = POLLERR|POLLIN|POLLHUP|POLLNVAL;
				
				if (poll (pfd, 1, engine->driver->period_interval) < 0) {
					jack_error ("poll on subgraph processing failed (%s)", strerror (errno));
					status = -1; 
				}

				if (pfd[0].revents & ~POLLIN) {
					jack_error ("subgraph starting at %s lost client", client->control->name);
					status = -1; 
				}

				if (pfd[0].revents & POLLIN) {
					status = 0;
				} else {
					jack_error ("subgraph starting at %s timed out (subgraph_wait_fd=%d, status = %d, state = %s)", 
						    client->control->name, client->subgraph_wait_fd, status, 
						    client_state_names[client->control->state]);
					status = 1;
				}
			}

			if (status != 0) {
				client->control->timed_out = 1;
				engine->process_errors++;
				break;
			} else {
				if (read (client->subgraph_wait_fd, &c, sizeof (c)) != sizeof (c)) {
					jack_error ("cannot clean up byte from graph wait fd (%s)", strerror (errno));
					engine->process_errors++;
					break;
				}
			}

			/* Move to next in-process client (or end of client list) */

			while (node) {
				if (jack_client_is_inprocess (((jack_client_internal_t *) node->data))) {
					break;
				}
				node = g_slist_next (node);
			}

		}
	}

	return engine->process_errors > 0;
}

static int
jack_engine_post_process (jack_engine_t *engine)
{
	jack_client_control_t *ctl;
	jack_client_internal_t *client;
	GSList *node;
	int need_remove = FALSE;
	
	jack_lock_graph (engine);
	
	/* find any clients that need removal due to timeouts, etc. */
		
	for (node = engine->clients; node; node = g_slist_next (node) ) {

		client = (jack_client_internal_t *) node->data;
		ctl = client->control;
		
		if (ctl->timed_out || (ctl->state > NotTriggered && ctl->state != Finished)) {
			client->error++;
		}

		if (client->error) {
			need_remove = TRUE;
		}
	}

	if (need_remove) {
		
		GSList *tmp;
		int need_sort = FALSE;
		
		/* remove all dead clients */
		
		for (node = engine->clients; node; ) {
			
			tmp = g_slist_next (node);
			
			client = (jack_client_internal_t *) node->data;
			
			if (client->error) {
				
				if (engine->verbose) {
					fprintf (stderr, "removing failed client %s state = %s\n", 
						 client->control->name, client_state_names[client->control->state]);
				}
				
				jack_remove_client (engine, (jack_client_internal_t *) node->data);
				need_sort = TRUE;
			}
			
			node = tmp;
		}
			
		if (need_sort) {
			jack_sort_graph (engine);
		}
	}

	jack_unlock_graph (engine);

	return 0;
}

static int
jack_load_client (jack_engine_t *engine, jack_client_internal_t *client, const char *path_to_so)
{
	const char *errstr;
	dlhandle handle;

	handle = dlopen (path_to_so, RTLD_NOW|RTLD_GLOBAL);
	
	if (handle == NULL) {
		if ((errstr = dlerror ()) != 0) {
			jack_error ("can't load \"%s\": %s", path_to_so, errstr);
		} else {
			jack_error ("bizarre error loading driver shared object %s", path_to_so);
		}
		return -1;
	}

	client->handle = handle;

#if 0
	initialize = dlsym (handle, "client_initialize");

	if ((errstr = dlerror ()) != 0) {
		jack_error ("no initialize function in shared object %s\n", path_to_so);
		dlclose (handle);
		return -1;
	}

	finish = dlsym (handle, "client_finish");

	if ((errstr = dlerror ()) != 0) {
		jack_error ("no finish function in in shared driver object %s", path_to_so);
		dlclose (handle);
		return -1;
	}
#endif

	return 0;

}

static void
jack_client_unload (jack_client_internal_t *client)
{
	if (client->handle) {
//		client->finish (client);
		dlclose (client->handle);
	}
}

static int
handle_new_client (jack_engine_t *engine, int client_fd)

{
	GSList *node;
	jack_client_internal_t *client = NULL;
	jack_client_connect_request_t req;
	jack_client_connect_result_t res;

	if (read (client_fd, &req, sizeof (req)) != sizeof (req)) {
		jack_error ("cannot read connection request from client");
		return -1;
	}

	res.status = 0;

	for (node = engine->clients; node; node = g_slist_next (node)) {
	        client = (jack_client_internal_t *) node->data;

		if (strncmp(req.name, (char*)client->control->name, sizeof(req.name)) == 0) {
		        jack_error ("cannot create new client; %s already exists", client->control->name);

			res.status = -1;
		}
	}

	if (res.status == 0) {

	        if ((client = jack_client_internal_new (engine, client_fd, &req)) == NULL) {
		        jack_error ("cannot create new client object");
			return -1;
		}

		if (engine->verbose) {
			fprintf (stderr, "new client: %s, id = %d type %d @ %p fd = %d\n", 
				 client->control->name, client->control->id, 
				 req.type, client->control, client_fd);
		}

		res.client_key = client->shm_key;
		res.control_key = engine->control_key;
		res.port_segment_key = engine->port_segment_key;
		res.realtime = engine->control->real_time;
		res.realtime_priority = engine->rtpriority - 1;
		
		if (jack_client_is_inprocess (client)) {
		        res.client_control = client->control;
			res.engine_control = engine->control;
		} else {
		        strcpy (res.fifo_prefix, engine->fifo_prefix);
		}
	}

	if (client == NULL) {
		return -1;
	}

	if (write (client->request_fd, &res, sizeof (res)) != sizeof (res)) {
		jack_error ("cannot write connection response to client");
		jack_client_delete (engine, client);
		return -1;
	}

	if (res.status) {
	        return res.status;
	}

	jack_lock_graph (engine);

	engine->clients = g_slist_prepend (engine->clients, client);

	if (client->control->type != ClientDynamic) {
		if (engine->pfd_max >= engine->pfd_size) {
			engine->pfd = (struct pollfd *) realloc (engine->pfd, sizeof (struct pollfd) * engine->pfd_size + 16);
			engine->pfd_size += 16;
		}
		
		engine->pfd[engine->pfd_max].fd = client->request_fd;
		engine->pfd[engine->pfd_max].events = POLLIN|POLLPRI|POLLERR|POLLHUP|POLLNVAL;
		engine->pfd_max++;
	}

	jack_unlock_graph (engine);

	return 0;
}

static int
handle_client_ack_connection (jack_engine_t *engine, int client_fd)

{
	jack_client_internal_t *client;
	jack_client_connect_ack_request_t req;
	jack_client_connect_ack_result_t res;

	if (read (client_fd, &req, sizeof (req)) != sizeof (req)) {
		jack_error ("cannot read ACK connection request from client");
		return -1;
	}

	if ((client = jack_client_internal_by_id (engine, req.client_id)) == NULL) {
		jack_error ("unknown client ID in ACK connection request");
		return -1;
	}

	client->event_fd = client_fd;

	res.status = 0;

	if (write (client->event_fd, &res, sizeof (res)) != sizeof (res)) {
		jack_error ("cannot write ACK connection response to client");
		return -1;
	}

	return 0;
}

static int
jack_client_activate (jack_engine_t *engine, jack_client_id_t id)

{
	jack_client_internal_t *client;
	GSList *node;
	int ret = -1;
	
	jack_lock_graph (engine);

	for (node = engine->clients; node; node = g_slist_next (node)) {

		if (((jack_client_internal_t *) node->data)->control->id == id) {
		       
			client = (jack_client_internal_t *) node->data;
			client->control->active = TRUE;

			/* we call this to make sure the
			   FIFO is built+ready by the time
			   the client needs it. we don't
			   care about the return value at
			   this point.  
			*/

			jack_get_fifo_fd (engine, ++engine->external_client_cnt);
			jack_sort_graph (engine);

			ret = 0;
			break;
		}
	}

	jack_unlock_graph (engine);
	return ret;
}	

static int
jack_client_do_deactivate (jack_engine_t *engine, jack_client_internal_t *client, int sort_graph)

{
	/* called must hold engine->client_lock and must have checked for and/or
	   cleared all connections held by client.
	*/
	
	client->control->active = FALSE;

	if (!jack_client_is_inprocess (client) && engine->external_client_cnt > 0) {	
		engine->external_client_cnt--;
	}

	if (sort_graph) {
		jack_sort_graph (engine);
	}
	return 0;
}

static void
jack_client_disconnect (jack_engine_t *engine, jack_client_internal_t *client)

{
	GSList *node;
	jack_port_internal_t *port;

	/* call tree **** MUST HOLD *** engine->client_lock */

	for (node = client->ports; node; node = g_slist_next (node)) {
		port = (jack_port_internal_t *) node->data;
		jack_port_clear_connections (engine, port);
		jack_port_release (engine, port);
	}

	g_slist_free (client->ports);
	g_slist_free (client->fed_by);
	client->fed_by = 0;
	client->ports = 0;
}			

static int
jack_client_deactivate (jack_engine_t *engine, jack_client_id_t id)

{
	GSList *node;
	int ret = -1;

	jack_lock_graph (engine);

	for (node = engine->clients; node; node = g_slist_next (node)) {

		jack_client_internal_t *client = (jack_client_internal_t *) node->data;

		if (client->control->id == id) {
		        
	        	GSList *portnode;
			jack_port_internal_t *port;

			if (client == engine->timebase_client) {
				engine->timebase_client = 0;
				engine->control->time.frame = 0;
			}
			
			for (portnode = client->ports; portnode; portnode = g_slist_next (portnode)) {
				port = (jack_port_internal_t *) portnode->data;
				jack_port_clear_connections (engine, port);
 			}

			ret = jack_client_do_deactivate (engine, node->data, TRUE);
			break;
		}
	}

	jack_unlock_graph (engine);

	return ret;
}	

static int
jack_set_timebase (jack_engine_t *engine, jack_client_id_t client)
{
	int ret = -1;

	jack_lock_graph (engine);

	if ((engine->timebase_client = jack_client_internal_by_id (engine, client)) != 0) {
		engine->control->time.frame = engine->timebase_client->control->frame_time;
		ret = 0;
	}

	jack_unlock_graph (engine);

	return ret;
}

static int
handle_client_jack_error (jack_engine_t *engine, int fd)
{
	jack_client_internal_t *client = 0;
	GSList *node;

	jack_lock_graph (engine);

	for (node = engine->clients; node; node = g_slist_next (node)) {
		if (((jack_client_internal_t *) node->data)->request_fd == fd) {
			client = (jack_client_internal_t *) node->data;
			client->error++;
			break;
		}
	}

	jack_unlock_graph (engine);

	return 0;
}

static int
handle_client_request (jack_engine_t *engine, int fd)
{
	jack_request_t req;
	jack_client_internal_t *client = 0;
	int reply_fd;
	GSList *node;
	int might_reorder = FALSE;

	jack_lock_graph (engine);
	
	for (node = engine->clients; node; node = g_slist_next (node)) {
		if (((jack_client_internal_t *) node->data)->request_fd == fd) {
			client = (jack_client_internal_t *) node->data;
			break;
		}
	}

	jack_unlock_graph (engine);

	if (client == NULL) {
		jack_error ("client input on unknown fd %d!", fd);
		return -1;
	}

	if (read (client->request_fd, &req, sizeof (req)) < sizeof (req)) {
		jack_error ("cannot read request from client");
		client->error++;
		return -1;
	}

	reply_fd = client->request_fd;

	switch (req.type) {
	case RegisterPort:
		req.status = jack_port_do_register (engine, &req);
		break;

	case UnRegisterPort:
		req.status = jack_port_do_unregister (engine, &req);
		break;

	case ConnectPorts:
		might_reorder = TRUE;
		req.status = jack_port_do_connect (engine, req.x.connect.source_port, req.x.connect.destination_port);
		break;

	case DisconnectPort:
		might_reorder = TRUE;
		req.status = jack_port_do_disconnect_all (engine, req.x.port_info.port_id);
		break;

	case DisconnectPorts:
		might_reorder = TRUE;
		req.status = jack_port_do_disconnect (engine, req.x.connect.source_port, req.x.connect.destination_port);
		break;

	case ActivateClient:
		req.status = jack_client_activate (engine, req.x.client_id);
		break;

	case DeactivateClient:
		might_reorder = TRUE;
		req.status = jack_client_deactivate (engine, req.x.client_id);
		break;

	case SetTimeBaseClient:
		req.status = jack_set_timebase (engine, req.x.client_id);
		break;

	default:
		/* some requests are handled entirely on the client side,
		   by adjusting the shared memory area(s)
		*/
		break;
	}

	if (reply_fd >= 0) {
		if (write (reply_fd, &req, sizeof (req)) < sizeof (req)) {
			jack_error ("cannot write request result to client");
			return -1;
		}
	}

	return 0;
}

static void *
jack_server_thread (void *arg)

{
	jack_engine_t *engine = (jack_engine_t *) arg;
	struct sockaddr_un client_addr;
	socklen_t client_addrlen;
	struct pollfd *pfd;
	int client_socket;
	int done = 0;
	int i;
	int max;
	
	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	engine->pfd[0].fd = engine->fds[0];
	engine->pfd[0].events = POLLIN|POLLERR;
	engine->pfd[1].fd = engine->fds[1];
	engine->pfd[1].events = POLLIN|POLLERR;
	engine->pfd_max = 2;

	while (!done) {

		/* XXX race here with new external clients
		   causing engine->pfd to be reallocated.
		   I don't know how to solve this
		   short of copying the entire
		   contents of the pfd struct. Ick.
		*/

		max = engine->pfd_max;
		pfd = engine->pfd;
	
		if (poll (pfd, max, 10000) < 0) {
			if (errno == EINTR) {
				continue;
			}
			jack_error ("poll failed (%s)", strerror (errno));
			break;
		}

		/* check the master server socket */

		if (pfd[0].revents & POLLERR) {
			jack_error ("error on server socket");
			break;
		}

		if (pfd[0].revents & POLLIN) {

			memset (&client_addr, 0, sizeof (client_addr));
			client_addrlen = sizeof (client_addr);

			if ((client_socket = accept (engine->fds[0], (struct sockaddr *) &client_addr, &client_addrlen)) < 0) {
				jack_error ("cannot accept new connection (%s)", strerror (errno));
			} else if (handle_new_client (engine, client_socket) < 0) {
				jack_error ("cannot complete new client connection process");
				close (client_socket);
			}
		}

		/* check the ACK server socket */

		if (pfd[1].revents & POLLERR) {
			jack_error ("error on server ACK socket");
			break;
		}

		if (pfd[1].revents & POLLIN) {

			memset (&client_addr, 0, sizeof (client_addr));
			client_addrlen = sizeof (client_addr);

			if ((client_socket = accept (engine->fds[1], (struct sockaddr *) &client_addr, &client_addrlen)) < 0) {
				jack_error ("cannot accept new ACK connection (%s)", strerror (errno));
			} else if (handle_client_ack_connection (engine, client_socket)) {
				jack_error ("cannot complete client ACK connection process");
				close (client_socket);
			}
		}

		/* check each client socket */

		for (i = 2; i < max; i++) {
			if (pfd[i].fd < 0) {
				continue;
			}

			if (pfd[i].revents & ~POLLIN) {
				handle_client_jack_error (engine, pfd[i].fd);
			} else if (pfd[i].revents & POLLIN) {
				if (handle_client_request (engine, pfd[i].fd)) {
					jack_error ("bad hci\n");
				}
			}
		}
	}

	return 0;
}

static void
jack_start_server (jack_engine_t *engine)

{
	pthread_create (&engine->server_thread, 0, &jack_server_thread, engine);
	pthread_detach (engine->server_thread);
}

jack_engine_t *
jack_engine_new (int realtime, int rtpriority, int verbose)
{
	jack_engine_t *engine;
	size_t control_size;
	void *addr;
	int i;

	engine = (jack_engine_t *) malloc (sizeof (jack_engine_t));

	engine->driver = 0;
	engine->process = jack_process;
	engine->set_sample_rate = jack_set_sample_rate;
	engine->set_buffer_size = jack_set_buffer_size;
	engine->process_lock = jack_engine_process_lock;
	engine->process_unlock = jack_engine_process_unlock;
	engine->post_process = jack_engine_post_process;

	engine->next_client_id = 1;
	engine->timebase_client = 0;
	engine->port_max = 128;
	engine->rtpriority = rtpriority;
	engine->silent_buffer = 0;
	engine->verbose = verbose;
	engine->asio_mode = FALSE;

	pthread_mutex_init (&engine->client_lock, 0);
	pthread_mutex_init (&engine->buffer_lock, 0);
	pthread_mutex_init (&engine->port_lock, 0);

	engine->clients = 0;

	engine->port_segments = 0;
	engine->port_buffer_freelist = 0;

	engine->pfd_size = 16;
	engine->pfd_max = 0;
	engine->pfd = (struct pollfd *) malloc (sizeof (struct pollfd) * engine->pfd_size);

	engine->fifo_size = 16;
	engine->fifo = (int *) malloc (sizeof (int) * engine->fifo_size);
	for (i = 0; i < engine->fifo_size; i++) {
		engine->fifo[i] = -1;
	}

	engine->external_client_cnt = 0;

	srandom (time ((time_t *) 0));

	engine->control_key = random();
	control_size = sizeof (jack_control_t) + (sizeof (jack_port_shared_t) * engine->port_max);

	if (jack_initialize_shm (engine)) {
		return 0;
	}

	if ((engine->control_shm_id = shmget (engine->control_key, control_size, IPC_CREAT|0644)) < 0) {
		jack_error ("cannot create engine control shared memory segment (%s)", strerror (errno));
		return 0;
	}

	jack_register_shm (engine->control_shm_id);
	
	if ((addr = shmat (engine->control_shm_id, 0, 0)) == (void *) -1) {
		jack_error ("cannot attach control shared memory segment (%s)", strerror (errno));
		shmctl (engine->control_shm_id, IPC_RMID, 0);
		return 0;
	}

	engine->control = (jack_control_t *) addr;

	/* Mark all ports as available */

	for (i = 0; i < engine->port_max; i++) {
		engine->control->ports[i].in_use = 0;
		engine->control->ports[i].id = i;
	}

	/* allocate internal port structures so that we can keep
	   track of port connections.
	*/

	engine->internal_ports = (jack_port_internal_t *) malloc (sizeof (jack_port_internal_t) * engine->port_max);

	for (i = 0; i < engine->port_max; i++) {
		engine->internal_ports[i].connections = 0;
	}

	if (make_sockets (engine->fds) < 0) {
		jack_error ("cannot create server sockets");
		return 0;
	}

	engine->control->port_max = engine->port_max;
	engine->control->real_time = realtime;
	engine->control->client_priority = engine->rtpriority - 1;
 
	engine->control->buffer_size = 0;
	engine->control->time.frame_rate = 0;
	engine->control->time.frame = 0;

	snprintf (engine->fifo_prefix, sizeof (engine->fifo_prefix), "%s/jack-ack-fifo-%d", jack_temp_dir, getpid());

	(void) jack_get_fifo_fd (engine, 0);
	jack_start_server (engine);

	return engine;
}

static int
jack_become_real_time (pthread_t thread, int priority)

{
	struct sched_param rtparam;
	int x;

	memset (&rtparam, 0, sizeof (rtparam));
	rtparam.sched_priority = priority;

	if ((x = pthread_setschedparam (thread, SCHED_FIFO, &rtparam)) != 0) {
		jack_error ("cannot set thread to real-time priority (FIFO/%d) (%d: %s)", rtparam.sched_priority, x, strerror (errno));
		return -1;
	}

	if (mlockall (MCL_CURRENT | MCL_FUTURE) != 0) {
	    jack_error ("cannot lock down memory for RT thread (%s)", strerror (errno));
	    return -1;
	}

	return 0;
}

static void
cancel_cleanup (int status, void *arg)

{
	jack_engine_t *engine = (jack_engine_t *) arg;
	engine->driver->stop (engine->driver);
	engine->driver->finish (engine->driver);
}

static void *
jack_main_thread (void *arg)

{
	jack_engine_t *engine = (jack_engine_t *) arg;
	jack_driver_t *driver = engine->driver;

	if (engine->control->real_time) {
		if (jack_become_real_time (pthread_self(), engine->rtpriority)) {
			engine->control->real_time = 0;
		}
	}

	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	on_exit (cancel_cleanup, engine);

	if (driver->start (driver)) {
		jack_error ("cannot start driver");
		pthread_exit (0);
	}

	while (1) {
		int status;
		nframes_t nframes;

		nframes = driver->wait (driver, -1, &status);

		if (status != 0) {
			jack_error ("driver wait function failed, exiting");
			pthread_exit (0);
		}

		switch (driver->process (driver, nframes)) {
		case -1:
			jack_error ("driver process function failed, exiting");
			pthread_exit (0);
			break;

		case  1:
			if (driver->start (driver)) {
				jack_error ("cannot restart driver");
				pthread_exit (0);
			}
			break;

		default:
			break;
		}
	}

	pthread_exit (0);
}

int
jack_run (jack_engine_t *engine)

{
	if (engine->driver == NULL) {
		jack_error ("engine driver not set; cannot start");
		return -1;
	}
	return pthread_create (&engine->main_thread, 0, jack_main_thread, engine);
}
int
jack_wait (jack_engine_t *engine)

{
	void *ret = 0;
	int err;

	if ((err = pthread_join (engine->main_thread, &ret)) != 0)  {
		switch (err) {
		case EINVAL:
			jack_error ("cannot join with audio thread (thread detached, or another thread is waiting)");
			break;
		case ESRCH:
			jack_error ("cannot join with audio thread (thread no longer exists)");
			break;
		case EDEADLK:
			jack_error ("programming error: jack_wait() called by audio thread");
			break;
		default:
			jack_error ("cannot join with audio thread (%s)", strerror (errno));
		}
	}
	return (int) ret;
}

int 
jack_engine_delete (jack_engine_t *engine)

{
	if (engine) {
		return pthread_cancel (engine->main_thread);
	}
	return 0;
}

static jack_client_internal_t *
jack_client_internal_new (jack_engine_t *engine, int fd, jack_client_connect_request_t *req)

{
	jack_client_internal_t *client;
	key_t shm_key = 0;
	int shm_id = 0;
	void *addr = 0;

	switch (req->type) {
	case ClientDynamic:
	case ClientDriver:
		break;

	case ClientOutOfProcess:

		shm_key = random();
		
		if ((shm_id = shmget (shm_key, sizeof (jack_client_control_t), IPC_CREAT|0666)) < 0) {
			jack_error ("cannot create client control block");
			return 0;
		}
		
		jack_register_shm (shm_id);

		if ((addr = shmat (shm_id, 0, 0)) == (void *) -1) {
			jack_error ("cannot attach new client control block");
			shmctl (shm_id, IPC_RMID, 0);
			return 0;
		}

		break;
	}

	client = (jack_client_internal_t *) malloc (sizeof (jack_client_internal_t));

	client->request_fd = fd;
	client->event_fd = -1;
	client->ports = 0;
	client->fed_by = 0;
	client->execution_order = UINT_MAX;
	client->next_client = NULL;
	client->handle = NULL;
	client->error = 0;

	if (req->type != ClientOutOfProcess) {
		
		client->control = (jack_client_control_t *) malloc (sizeof (jack_client_control_t));		

	} else {

		client->shm_id = shm_id;
		client->shm_key = shm_key;
		client->control = (jack_client_control_t *) addr;
	}

	client->control->type = req->type;
	client->control->active = 0;
	client->control->dead = 0;
	client->control->timed_out = 0;
	client->control->id = engine->next_client_id++;
	strcpy ((char *) client->control->name, req->name);

	client->control->process = NULL;
	client->control->process_arg = NULL;
	client->control->bufsize = NULL;
	client->control->bufsize_arg = NULL;
	client->control->srate = NULL;
	client->control->srate_arg = NULL;
	client->control->port_register = NULL;
	client->control->port_register_arg = NULL;
	client->control->graph_order = NULL;
	client->control->graph_order_arg = NULL;

	if (req->type == ClientDynamic) {
		if (jack_load_client (engine, client, req->object_path)) {
			jack_error ("cannot dynamically load client from \"%s\"", req->object_path);
			jack_client_delete (engine, client);
			return 0;
		}
	}

	return client;
}

static void
jack_port_clear_connections (jack_engine_t *engine, jack_port_internal_t *port)
{
	GSList *node, *next;

	for (node = port->connections; node; ) {
		next = g_slist_next (node);
		jack_port_disconnect_internal (engine, 
						((jack_connection_internal_t *) node->data)->source,
						((jack_connection_internal_t *) node->data)->destination, 
						FALSE);
		node = next;
	}

	g_slist_free (port->connections);
	port->connections = 0;
}

static void
jack_remove_client (jack_engine_t *engine, jack_client_internal_t *client)
{
	GSList *node;
	int i;

	if (engine->verbose) {
		fprintf (stderr, "adios senor %s\n", client->control->name);
	}
			 
	/* caller must hold the client_lock */

	/* this stops jack_deliver_event() from doing anything */

	client->control->dead = TRUE;

	if (client == engine->timebase_client) {
		engine->timebase_client = 0;
		engine->control->time.frame = 0;
	}

	jack_client_disconnect (engine, client);

	/* try to force the server thread to return from poll */

	close (client->event_fd);
	close (client->request_fd);

	for (node = engine->clients; node; node = g_slist_next (node)) {
		if (((jack_client_internal_t *) node->data)->control->id == client->control->id) {
			engine->clients = g_slist_remove_link (engine->clients, node);
			g_slist_free_1 (node);
			break;
		}
	}
	
	jack_client_do_deactivate (engine, client, FALSE);

	/* rearrange the pollfd array so that things work right the 
	   next time we go into poll(2).
	*/
	
	for (i = 0; i < engine->pfd_max; i++) {
		if (engine->pfd[i].fd == client->request_fd) {
			if (i+1 < engine->pfd_max) {
				memmove (&engine->pfd[i], &engine->pfd[i+1], sizeof (struct pollfd) * (engine->pfd_max - i));
			}
			engine->pfd_max--;
		}
	}

	jack_client_delete (engine, client);
}

static void
jack_client_delete (jack_engine_t *engine, jack_client_internal_t *client)

{
	if (jack_client_is_inprocess (client)) {
		jack_client_unload (client);
		free ((char *) client->control);
	} else {
		shmdt ((void *) client->control);
	}

	free (client);
}

jack_client_internal_t *
jack_client_by_name (jack_engine_t *engine, const char *name)

{
	jack_client_internal_t *client = NULL;
	GSList *node;

	jack_lock_graph (engine);

	for (node = engine->clients; node; node = g_slist_next (node)) {
		if (strcmp ((const char *) ((jack_client_internal_t *) node->data)->control->name, name) == 0) {
			client = (jack_client_internal_t *) node->data;
			break;
		}
	}

	jack_unlock_graph (engine);
	return client;
}

jack_client_internal_t *
jack_client_internal_by_id (jack_engine_t *engine, jack_client_id_t id)

{
	jack_client_internal_t *client = NULL;
	GSList *node;

	/* call tree ***MUST HOLD*** engine->client_lock */

	for (node = engine->clients; node; node = g_slist_next (node)) {
		if (((jack_client_internal_t *) node->data)->control->id == id) {
			client = (jack_client_internal_t *) node->data;
			break;
		}
	}

	return client;
}

static int
jack_deliver_event (jack_engine_t *engine, jack_client_internal_t *client, jack_event_t *event)
{
	char status;

	/* caller must hold the client_lock */

	if (client->control->dead) {
		return 0;
	}

	if (jack_client_is_inprocess (client)) {

		switch (event->type) {
		case PortConnected:
		case PortDisconnected:
			jack_client_handle_port_connection (client->control->private_internal_client, event);
			break;

		case BufferSizeChange:
			if (client->control->bufsize) {
				client->control->bufsize (event->x.n, client->control->bufsize_arg);
			}
			break;

		case SampleRateChange:
			if (client->control->srate) {
				client->control->srate (event->x.n, client->control->bufsize_arg);
			}
			break;

		case GraphReordered:
			if (client->control->graph_order) {
				client->control->graph_order (client->control->graph_order_arg);
			}
			break;

		default:
			/* internal clients don't need to know */
			break;
		}

	} else {

		if (write (client->event_fd, event, sizeof (*event)) != sizeof (*event)) {
			jack_error ("cannot send event to client [%s] (%s)", client->control->name, strerror (errno));
			client->error++;
		}
		
		if (!client->error && (read (client->event_fd, &status, sizeof (status)) != sizeof (status))) {
			jack_error ("cannot read event response from client [%s] (%s)", client->control->name, strerror (errno));
			client->error++;
		}

		if (status != 0) {
			jack_error ("bad status for client event handling (type = %d)", event->type);
			client->error++;
		}
	}

	return 0;
}

int
jack_rechain_graph (jack_engine_t *engine)
{
	GSList *node, *next;
	unsigned long n;
	int err = 0;
	jack_client_internal_t *client, *subgraph_client, *next_client;
	jack_event_t event;

	jack_clear_fifos (engine);

	subgraph_client = 0;

	if (engine->verbose) {
		fprintf(stderr, "-- jack_rechain_graph():\n");
	}

	event.type = GraphReordered;

	for (n = 0, node = engine->clients, next = NULL; node; node = next) {

		next = g_slist_next (node);

		if (((jack_client_internal_t *) node->data)->control->active) {

			client = (jack_client_internal_t *) node->data;

			/* find the next active client. its ok for this to be NULL */
			
			while (next) {
				if (((jack_client_internal_t *) next->data)->control->active) {
					break;
				}
				next = g_slist_next (next);
			};

			if (next == NULL) {
				next_client = NULL;
			} else {
				next_client = (jack_client_internal_t *) next->data;
			}

			client->execution_order = n;
			client->next_client = next_client;
			
			if (jack_client_is_inprocess (client)) {
				
				/* break the chain for the current subgraph. the server
				   will wait for chain on the nth FIFO, and will
				   then execute this in-process client.
				*/
				
				if (subgraph_client) {
					subgraph_client->subgraph_wait_fd = jack_get_fifo_fd (engine, n);
					if (engine->verbose) {
						fprintf(stderr, "client %s: wait_fd=%d, execution_order=%lu.\n", 
							subgraph_client->control->name, subgraph_client->subgraph_wait_fd, n);
					}
					n++;
				}

				if (engine->verbose) {
					fprintf(stderr, "client %s: inprocess client, execution_order=%lu.\n", 
						client->control->name, n);
				}

				/* this does the right thing for in-process clients too */

				jack_deliver_event (engine, client, &event);

				subgraph_client = 0;
				
			} else {
				
				if (subgraph_client == NULL) {
					
				        /* start a new subgraph. the engine will start the chain
					   by writing to the nth FIFO.
					*/
					
					subgraph_client = client;
					subgraph_client->subgraph_start_fd = jack_get_fifo_fd (engine, n);
					if (engine->verbose) {
						fprintf(stderr, "client %s: start_fd=%d, execution_order=%lu.\n",
							subgraph_client->control->name, subgraph_client->subgraph_start_fd, n);
					}
				} 
				else {
					if (engine->verbose) {
						fprintf(stderr, "client %s: in subgraph after %s, execution_order=%lu.\n",
							client->control->name, subgraph_client->control->name, n);
					}

				}

				/* make sure fifo for 'n + 1' exists 
				 * before issuing client reorder 
				 */
				
				(void) jack_get_fifo_fd(engine, client->execution_order + 1);
				
				event.x.n = client->execution_order;
				
				jack_deliver_event (engine, client, &event);
				n++;
			}
		}
	}

	if (subgraph_client) {
		subgraph_client->subgraph_wait_fd = jack_get_fifo_fd (engine, n);
		if (engine->verbose) {
			fprintf(stderr, "client %s: wait_fd=%d, execution_order=%lu (last client).\n", 
				subgraph_client->control->name, subgraph_client->subgraph_wait_fd, n);
		}
	}

	return err;
}

static void
jack_trace_terminal (jack_client_internal_t *c1, jack_client_internal_t *rbase)
{
	jack_client_internal_t *c2;

	/* make a copy of the existing list of routes that feed c1. this provides
	   us with an atomic snapshot of c1's "fed-by" state, which will be
	   modified as we progress ...
	*/

	GSList *existing;
	GSList *node;

	if (c1->fed_by == NULL) {
		return;
	}

	existing = g_slist_copy (c1->fed_by);

	/* for each route that feeds c1, recurse, marking it as feeding
	   rbase as well.
	*/

	for (node = existing; node; node = g_slist_next  (node)) {

		c2 = (jack_client_internal_t *) node->data;

		/* c2 is a route that feeds c1 which somehow feeds base. mark
		   base as being fed by c2, but don't do it more than
		   once.
		*/

		if (c2 != rbase && c2 != c1) {

			if (g_slist_find (rbase->fed_by, c2) == NULL) {
				rbase->fed_by = g_slist_prepend (rbase->fed_by, c2);
			}

			/* FIXME: if c2->fed_by is not up-to-date, we may end up
			          recursing infinitely (kaiv)
			*/

			if (g_slist_find (c2->fed_by, c1) == NULL) {
				/* now recurse, so that we can mark base as being fed by
				   all routes that feed c2
				*/
				jack_trace_terminal (c2, rbase);
			}
		}
	}

	g_slist_free (existing);
}

static int 
jack_client_sort (jack_client_internal_t *a, jack_client_internal_t *b)

{
	if (g_slist_find (a->fed_by, b)) {
		
		if (g_slist_find (b->fed_by, a)) {

			/* feedback loop: if `a' is the driver
			   client, let that execute first.
			*/

			if (a->control->type == ClientDriver) {
				/* b comes after a */
				return -1;
			}
		}

		/* a comes after b */
		return 1;

	} else if (g_slist_find (b->fed_by, a)) {
		
		if (g_slist_find (a->fed_by, b)) {

			/* feedback loop: if `b' is the driver
			   client, let that execute first.
			*/

			if (b->control->type == ClientDriver) {
				/* b comes before a */
				return 1;
			}
		}

		/* b comes after a */
		return -1;
	} else {
		/* we don't care */
		return 0;
	}
}

static int
jack_client_feeds (jack_client_internal_t *might, jack_client_internal_t *target)
{
	GSList *pnode, *cnode;

	/* Check every port of `might' for an outbound connection to `target'
	*/

	for (pnode = might->ports; pnode; pnode = g_slist_next (pnode)) {

		jack_port_internal_t *port;
		
		port = (jack_port_internal_t *) pnode->data;

		for (cnode = port->connections; cnode; cnode = g_slist_next (cnode)) {

			jack_connection_internal_t *c;

			c = (jack_connection_internal_t *) cnode->data;

			if (c->source->shared->client_id == might->control->id &&
			    c->destination->shared->client_id == target->control->id) {
				return 1;
			}
		}
	}
	
	return 0;
}

static nframes_t
jack_get_port_total_latency (jack_engine_t *engine, jack_port_internal_t *port)
{
	GSList *node;
	nframes_t latency;
	nframes_t max_latency = 0;

	/* call tree must hold engine->client_lock. */
	
	latency = port->shared->latency;

	for (node = port->connections; node; node = g_slist_next (node)) {

		nframes_t this_latency;
		jack_connection_internal_t *connection;

		connection = (jack_connection_internal_t *) node->data;

		/* if we're a destination in the connection, recurse on the source to
		   get its total latency
		*/
		
		if (connection->destination == port) {

			if (connection->source->shared->flags & JackPortIsTerminal) {
				this_latency = connection->source->shared->latency;
			} else {
				this_latency = jack_get_port_total_latency (engine, connection->source);
			}

		} else {

			/* "port" is the source, so get the latency of the destination */

			if (connection->destination->shared->flags & JackPortIsTerminal) {
				this_latency = connection->destination->shared->latency;
			} else {
				this_latency = jack_get_port_total_latency (engine, connection->destination);
			}
		}

		if (this_latency > max_latency) {
			max_latency = this_latency;
		}
	}

	return latency + max_latency;
}

static void
jack_compute_all_port_total_latencies (jack_engine_t *engine)
{
	jack_port_shared_t *shared = engine->control->ports;
	int i;

	for (i = 0; i < engine->control->port_max; i++) {
		if (shared[i].in_use) {
			shared[i].total_latency = jack_get_port_total_latency (engine, &engine->internal_ports[i]);
		}
	}
}

/**
 * Sorts the network of clients using the following 
 * algorithm:
 *
 * 1) figure out who is connected to whom:
 *    
 *    foreach client1
 *       foreach input port
 *           foreach client2
 *              foreach output port
 *                 if client1->input port connected to client2->output port
 *                     mark client1 fed by client 2
 *
 * 2) trace the connections as terminal arcs in the graph so that
 *    if client A feeds client B who feeds client C, mark client C
 *    as fed by client A as well as client B, and so forth.
 *
 * 3) now sort according to whether or not client1->fed_by (client2) is true.
 *    if the condition is true, client2 must execute before client1
 *
 */

static void
jack_sort_graph (jack_engine_t *engine)
{
	GSList *node, *onode;
	jack_client_internal_t *client;
	jack_client_internal_t *oclient;

	/* called, obviously, must hold engine->client_lock */

	for (node = engine->clients; node; node = g_slist_next (node)) {

		client = (jack_client_internal_t *) node->data;

		g_slist_free (client->fed_by);
		client->fed_by = 0;

		for (onode = engine->clients; onode; onode = g_slist_next (onode)) {
			
			oclient = (jack_client_internal_t *) onode->data;

			if (jack_client_feeds (oclient, client)) {
				client->fed_by = g_slist_prepend (client->fed_by, oclient);
			}
		}
	}

	for (node = engine->clients; node; node = g_slist_next (node)) {
		jack_trace_terminal ((jack_client_internal_t *) node->data,
				     (jack_client_internal_t *) node->data);
	}

	engine->clients = g_slist_sort (engine->clients, (GCompareFunc) jack_client_sort);

	jack_compute_all_port_total_latencies (engine);

	jack_rechain_graph (engine);
}

/**
 * Dumps current engine configuration to stderr.
 */
void jack_dump_configuration(jack_engine_t *engine, int take_lock)
{
        GSList *clientnode, *portnode, *connectionnode;
	jack_client_internal_t *client;
	jack_client_control_t *ctl;
	jack_port_internal_t *port;
	jack_connection_internal_t* connection;
	int n, m, o;
	
	fprintf(stderr, "engine.c: <-- dump begins -->\n");

	if (take_lock) {
		jack_lock_graph (engine);
	}
	
	for (n = 0, clientnode = engine->clients; clientnode; clientnode = g_slist_next (clientnode)) {
	        client = (jack_client_internal_t *) clientnode->data;
		ctl = client->control;

		fprintf (stderr, "client #%d: %s (type: %d, process? %s, fed by %d clients)\n",
			 ++n,
			 ctl->name,
			 ctl->type,
			 ctl->process ? "yes" : "no",
			 g_slist_length(client->fed_by));
		
		for(m = 0, portnode = client->ports; portnode; portnode = g_slist_next (portnode)) {
		        port = (jack_port_internal_t *) portnode->data;

			fprintf(stderr, "\t port #%d: %s\n", ++m, port->shared->name);

			for(o = 0, connectionnode = port->connections; 
			    connectionnode; 
			    connectionnode = g_slist_next (connectionnode)) {
			        connection = (jack_connection_internal_t *) connectionnode->data;
	
				fprintf(stderr, "\t\t connection #%d: %s %s\n",
					++o,
					(port->shared->flags & JackPortIsInput) ? "<-" : "->",
					(port->shared->flags & JackPortIsInput) ?
					  connection->source->shared->name :
					  connection->destination->shared->name);
			}
		}
	}

	if (take_lock) {
		jack_unlock_graph (engine);
	}

	
	fprintf(stderr, "engine.c: <-- dump ends -->\n");
}

static int 
jack_port_do_connect (jack_engine_t *engine,
		       const char *source_port,
		       const char *destination_port)
{
	jack_connection_internal_t *connection;
	jack_port_internal_t *srcport, *dstport;
	jack_port_id_t src_id, dst_id;

	if ((srcport = jack_get_port_by_name (engine, source_port)) == NULL) {
		jack_error ("unknown source port in attempted connection [%s]", source_port);
		return -1;
	}

	if ((dstport = jack_get_port_by_name (engine, destination_port)) == NULL) {
		jack_error ("unknown destination port in attempted connection [%s]", destination_port);
		return -1;
	}

	if ((dstport->shared->flags & JackPortIsInput) == 0) {
		jack_error ("destination port in attempted connection of %s and %s is not an input port", 
			    source_port, destination_port);
		return -1;
	}

	if ((srcport->shared->flags & JackPortIsOutput) == 0) {
		jack_error ("source port in attempted connection of %s and %s is not an output port",
			    source_port, destination_port);
		return -1;
	}

	if (srcport->shared->locked) {
		jack_error ("source port %s is locked against connection changes", source_port);
		return -1;
	}

	if (dstport->shared->locked) {
		jack_error ("destination port %s is locked against connection changes", destination_port);
		return -1;
	}

	if (strcmp (srcport->shared->type_info.type_name,
		    dstport->shared->type_info.type_name) != 0) {
		jack_error ("ports used in attemped connection are not of the same data type");
		return -1;
	}

	connection = (jack_connection_internal_t *) malloc (sizeof (jack_connection_internal_t));

	connection->source = srcport;
	connection->destination = dstport;

	src_id = srcport->shared->id;
	dst_id = dstport->shared->id;

	jack_lock_graph (engine);

	if (dstport->connections && dstport->shared->type_info.mixdown == NULL) {
		jack_error ("cannot make multiple connections to a port of type [%s]", dstport->shared->type_info.type_name);
		free (connection);
		return -1;
	} else {
		if (engine->verbose) {
			fprintf (stderr, "connect %s and %s\n",
				 srcport->shared->name,
				 dstport->shared->name);
		}

		dstport->connections = g_slist_prepend (dstport->connections, connection);
		srcport->connections = g_slist_prepend (srcport->connections, connection);
		
		jack_sort_graph (engine);

		jack_send_connection_notification (engine, srcport->shared->client_id, src_id, dst_id, TRUE);
		jack_send_connection_notification (engine, dstport->shared->client_id, dst_id, src_id, TRUE);
	}

	jack_unlock_graph (engine);
	return 0;
}

int
jack_port_disconnect_internal (jack_engine_t *engine, 
			       jack_port_internal_t *srcport, 
			       jack_port_internal_t *dstport, 
			       int sort_graph)

{
	GSList *node;
	jack_connection_internal_t *connect;
	int ret = -1;
	jack_port_id_t src_id, dst_id;

	/* call tree **** MUST HOLD **** engine->client_lock. */
	
	for (node = srcport->connections; node; node = g_slist_next (node)) {

		connect = (jack_connection_internal_t *) node->data;

		if (connect->source == srcport && connect->destination == dstport) {

			if (engine->verbose) {
				fprintf (stderr, "DIS-connect %s and %s\n",
					 srcport->shared->name,
					 dstport->shared->name);
			}
			
			srcport->connections = g_slist_remove (srcport->connections, connect);
			dstport->connections = g_slist_remove (dstport->connections, connect);

			src_id = srcport->shared->id;
			dst_id = dstport->shared->id;

			/* this is a bit harsh, but it basically says that if we actually
			   do a disconnect, and its the last one, then make sure that
			   any input monitoring is turned off on the srcport. this isn't
			   ideal for all situations, but it works better for most of them.
			*/

			if (srcport->connections == NULL) {
				srcport->shared->monitor_requests = 0;
			}

			jack_send_connection_notification (engine, srcport->shared->client_id, src_id, dst_id, FALSE);
			jack_send_connection_notification (engine, dstport->shared->client_id, dst_id, src_id, FALSE);

			free (connect);
			ret = 0;
			break;
		}
	}

	if (sort_graph) {
		jack_sort_graph (engine);
	}

	return ret;
}

static int
jack_port_do_disconnect_all (jack_engine_t *engine,
			     jack_port_id_t port_id)
{
	if (port_id >= engine->control->port_max) {
		jack_error ("illegal port ID in attempted disconnection [%u]", port_id);
		return -1;
	}

	if (engine->verbose) {
		fprintf (stderr, "clear connections for %s\n", engine->internal_ports[port_id].shared->name);
	}

	jack_lock_graph (engine);
	jack_port_clear_connections (engine, &engine->internal_ports[port_id]);
	jack_sort_graph (engine);
	jack_unlock_graph (engine);

	return 0;
}

static int 
jack_port_do_disconnect (jack_engine_t *engine,
			 const char *source_port,
			 const char *destination_port)
{
	jack_port_internal_t *srcport, *dstport;
	int ret = -1;

	if ((srcport = jack_get_port_by_name (engine, source_port)) == NULL) {
		jack_error ("unknown source port in attempted disconnection [%s]", source_port);
		return -1;
	}

	if ((dstport = jack_get_port_by_name (engine, destination_port)) == NULL) {
		jack_error ("unknown destination port in attempted connection [%s]", destination_port);
		return -1;
	}

	jack_lock_graph (engine);

	ret = jack_port_disconnect_internal (engine, srcport, dstport, TRUE);

	jack_unlock_graph (engine);

	return ret;
}

static int 
jack_get_fifo_fd (jack_engine_t *engine, int which_fifo)

{
	/* caller must hold client_lock */

	char path[PATH_MAX+1];
	struct stat statbuf;

	sprintf (path, "%s-%d", engine->fifo_prefix, which_fifo);

	if (stat (path, &statbuf)) {
		if (errno == ENOENT) {
			if (mknod (path, 0666|S_IFIFO, 0) < 0) {
				jack_error ("cannot create inter-client FIFO [%s] (%s)\n", path, strerror (errno));
				return -1;
			}
		} else {
			jack_error ("cannot check on FIFO %d\n", which_fifo);
			return -1;
		}
	} else {
		if (!S_ISFIFO(statbuf.st_mode)) {
			jack_error ("FIFO %d (%s) already exists, but is not a FIFO!\n", which_fifo, path);
			return -1;
		}
	}

	if (which_fifo >= engine->fifo_size) {
		int i;

		engine->fifo = (int *) realloc (engine->fifo, sizeof (int) * engine->fifo_size + 16);
		for (i = engine->fifo_size; i < engine->fifo_size + 16; i++) {
			engine->fifo[i] = -1;
		}
		engine->fifo_size += 16;
	}

	if (engine->fifo[which_fifo] < 0) {
		if ((engine->fifo[which_fifo] = open (path, O_RDWR|O_CREAT|O_NONBLOCK, 0666)) < 0) {
			jack_error ("cannot open fifo [%s] (%s)", path, strerror (errno));
			return -1;
		}
	}

	return engine->fifo[which_fifo];
}

static void
jack_clear_fifos (jack_engine_t *engine)
{
	/* caller must hold client_lock */

	int i;
	char buf[16];

	/* this just drains the existing FIFO's of any data left in them
	   by aborted clients, etc. there is only ever going to be
	   0, 1 or 2 bytes in them, but we'll allow for up to 16.
	*/

	for (i = 0; i < engine->fifo_size; i++) {
		if (engine->fifo[i] >= 0) {
			int nread = read (engine->fifo[i], buf, sizeof (buf));

			if (nread < 0 && errno != EAGAIN) {
				jack_error ("clear fifo[%d] error: %s", i, strerror (errno));
			} 
		}
	}
}

int
jack_use_driver (jack_engine_t *engine, jack_driver_t *driver)

{
	if (engine->driver) {
		engine->driver->detach (engine->driver, engine);
		engine->driver = 0;
	}

	if (driver) {
		if (driver->attach (driver, engine)) {
			return -1;
		}
	}

	engine->driver = driver;
	return 0;
}

/* PORT RELATED FUNCTIONS */


jack_port_id_t
jack_get_free_port (jack_engine_t *engine)

{
	jack_port_id_t i;

	pthread_mutex_lock (&engine->port_lock);

	for (i = 0; i < engine->port_max; i++) {
		if (engine->control->ports[i].in_use == 0) {
			engine->control->ports[i].in_use = 1;
			break;
		}
	}
	
	pthread_mutex_unlock (&engine->port_lock);
	
	if (i == engine->port_max) {
		return NoPort;
	}

	return i;
}

static void
jack_port_release (jack_engine_t *engine, jack_port_internal_t *port)

{
	pthread_mutex_lock (&engine->port_lock);
	port->shared->in_use = 0;

	if (port->buffer_info) {
		pthread_mutex_lock (&engine->buffer_lock);
		engine->port_buffer_freelist = g_slist_prepend (engine->port_buffer_freelist, port->buffer_info);
		port->buffer_info = NULL;
		pthread_mutex_unlock (&engine->buffer_lock);
	}
	pthread_mutex_unlock (&engine->port_lock);
}

jack_port_internal_t *
jack_get_port_internal_by_name (jack_engine_t *engine, const char *name)
{
	jack_port_id_t id;

	pthread_mutex_lock (&engine->port_lock);

	for (id = 0; id < engine->port_max; id++) {
		if (strcmp (engine->control->ports[id].name, name) == 0) {
			break;
		}
	}

	pthread_mutex_unlock (&engine->port_lock);
	
	if (id != engine->port_max) {
		return &engine->internal_ports[id];
	} else {
		return NULL;
	}
}

int
jack_port_do_register (jack_engine_t *engine, jack_request_t *req)
	
{
	jack_port_id_t port_id;
	jack_port_shared_t *shared;
	jack_port_internal_t *port;
	jack_client_internal_t *client;

	jack_lock_graph (engine);
	if ((client = jack_client_internal_by_id (engine, req->x.port_info.client_id)) == NULL) {
		jack_error ("unknown client id in port registration request");
		return -1;
	}
	jack_unlock_graph (engine);

	if ((port_id = jack_get_free_port (engine)) == NoPort) {
		jack_error ("no ports available!");
		return -1;
	}

	shared = &engine->control->ports[port_id];

	strcpy (shared->name, req->x.port_info.name);

	shared->client_id = req->x.port_info.client_id;
	shared->flags = req->x.port_info.flags;
	shared->buffer_size = req->x.port_info.buffer_size;
	shared->latency = 0;
	shared->monitor_requests = 0;
	shared->locked = 0;

	port = &engine->internal_ports[port_id];

	port->shared = shared;
	port->connections = 0;

	if (jack_port_assign_buffer (engine, port)) {
		jack_error ("cannot assign buffer for port");
		return -1;
	}

	jack_lock_graph (engine);
	client->ports = g_slist_prepend (client->ports, port);
	jack_port_registration_notify (engine, port_id, TRUE);
	jack_unlock_graph (engine);

	if (engine->verbose) {
		fprintf (stderr, "registered port %s, offset = %u\n", shared->name, shared->offset);
	}

	req->x.port_info.port_id = port_id;

	return 0;
}

int
jack_port_do_unregister (jack_engine_t *engine, jack_request_t *req)
{
	jack_client_internal_t *client;
	jack_port_shared_t *shared;
	jack_port_internal_t *port;

	if (req->x.port_info.port_id < 0 || req->x.port_info.port_id > engine->port_max) {
		jack_error ("invalid port ID %d in unregister request", req->x.port_info.port_id);
		return -1;
	}

	shared = &engine->control->ports[req->x.port_info.port_id];

	if (shared->client_id != req->x.port_info.client_id) {
		jack_error ("Client %d is not allowed to remove port %s", req->x.port_info.client_id, shared->name);
		return -1;
	}

	jack_lock_graph (engine);
	if ((client = jack_client_internal_by_id (engine, shared->client_id)) == NULL) {
		jack_error ("unknown client id in port registration request");
		jack_unlock_graph (engine);
		return -1;
	}

	port = &engine->internal_ports[req->x.port_info.port_id];

	jack_port_clear_connections (engine, port);
	jack_port_release (engine, &engine->internal_ports[req->x.port_info.port_id]);
	
	client->ports = g_slist_remove (client->ports, port);
	jack_port_registration_notify (engine, req->x.port_info.port_id, FALSE);
	jack_unlock_graph (engine);

	return 0;
}

void
jack_port_registration_notify (jack_engine_t *engine, jack_port_id_t port_id, int yn)

{
	jack_event_t event;
	jack_client_internal_t *client;
	GSList *node;

	event.type = (yn ? PortRegistered : PortUnregistered);
	event.x.port_id = port_id;
	
	for (node = engine->clients; node; node = g_slist_next (node)) {
		
		client = (jack_client_internal_t *) node->data;

		if (!client->control->active) {
			continue;
		}

		if (client->control->port_register) {
			if (jack_deliver_event (engine, client, &event)) {
				jack_error ("cannot send port registration notification to %s (%s)",
					     client->control->name, strerror (errno));
			}
		}
	}
}

int
jack_port_assign_buffer (jack_engine_t *engine, jack_port_internal_t *port)
{
	GSList *node;
	jack_port_segment_info_t *psi = 0;
	jack_port_buffer_info_t *bi;

	port->shared->shm_key = -1;

	if (port->shared->flags & JackPortIsInput) {
		port->shared->offset = 0;
		return 0;
	}
	
	pthread_mutex_lock (&engine->buffer_lock);

	if (engine->port_buffer_freelist == NULL) {
		jack_error ("all port buffers in use!");
		pthread_mutex_unlock (&engine->buffer_lock);
		return -1;
	}

	bi = (jack_port_buffer_info_t *) engine->port_buffer_freelist->data;

	for (node = engine->port_segments; node; node = g_slist_next (node)) {

		psi = (jack_port_segment_info_t *) node->data;

		if (bi->shm_key == psi->shm_key) {
			port->shared->shm_key = psi->shm_key;
			port->shared->offset = bi->offset;
			port->buffer_info = bi;
			break;
		}
	}

	if (engine->verbose) {
		fprintf (stderr, "port %s buf shm key 0x%x at offset %d bi = %p\n", 
			 port->shared->name,
			 port->shared->shm_key,
			 port->shared->offset,
			 port->buffer_info);
	}

	if (port->shared->shm_key >= 0) {
		engine->port_buffer_freelist = g_slist_remove (engine->port_buffer_freelist, bi);
		
	} else {
		jack_error ("port segment info for 0x%x:%d not found!", bi->shm_key, bi->offset);
	}

	pthread_mutex_unlock (&engine->buffer_lock);

	if (port->shared->shm_key < 0) {
		return -1;
	} else {
		return 0;
	}
}

static jack_port_internal_t *
jack_get_port_by_name (jack_engine_t *engine, const char *name)

{
	jack_port_id_t id;

	/* Note the potential race on "in_use". Other design
	   elements prevent this from being a problem.
	*/

	for (id = 0; id < engine->port_max; id++) {
		if (engine->control->ports[id].in_use && strcmp (engine->control->ports[id].name, name) == 0) {
			return &engine->internal_ports[id];
		}
	}

	return NULL;
}

static int
jack_send_connection_notification (jack_engine_t *engine, jack_client_id_t client_id, 
				   jack_port_id_t self_id, jack_port_id_t other_id, int connected)

{
	jack_client_internal_t *client;
	jack_event_t event;

	if ((client = jack_client_internal_by_id (engine, client_id)) == NULL) {
		jack_error ("no such client %d during connection notification", client_id);
		return -1;
	}

	event.type = (connected ? PortConnected : PortDisconnected);
	event.x.self_id = self_id;
	event.y.other_id = other_id;

	if (jack_deliver_event (engine, client, &event)) {
		jack_error ("cannot send port connection notification to client %s (%s)", 
			     client->control->name, strerror (errno));
		return -1;
	}

	return 0;
}

void
jack_set_asio_mode (jack_engine_t *engine, int yn)
{
	engine->asio_mode = yn;
}
