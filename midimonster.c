#include <string.h>
#include <signal.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include "midimonster.h"
#include "config.h"
#include "backend.h"
#include "plugin.h"

static size_t mappings = 0;
static channel_mapping* map = NULL;
static size_t fds = 0;
static managed_fd* fd = NULL;
static size_t ev_alloc = 0;
static size_t evs = 0;
static channel** ev_channel = NULL;
static channel_value* ev_value = NULL;
volatile static sig_atomic_t shutdown_requested = 0;

void signal_handler(int signum){
	shutdown_requested = 1;
}

int mm_map_channel(channel* from, channel* to){
	size_t u, m;
	//find existing source mapping
	for(u = 0; u < mappings; u++){
		if(map[u].from == from){
			break;
		}
	}

	//create new entry
	if(u == mappings){
		map = realloc(map, (mappings + 1) * sizeof(channel_mapping));
		if(!map){
			fprintf(stderr, "Failed to allocate memory\n");
			return 1;
		}
		memset(map + mappings, 0, sizeof(channel_mapping));
		mappings++;
		map[u].from = from;
	}

	//check whether the target is already mapped
	for(m = 0; m < map[u].destinations; m++){
		if(map[u].to[m] == to){
			return 0;
		}
	}

	map[u].to = realloc(map[u].to, (map[u].destinations + 1) * sizeof(channel*));
	if(!map[u].to){
		fprintf(stderr, "Failed to allocate memory\n");
		map[u].destinations = 0;
		return 1;
	}

	map[u].to[map[u].destinations] = to;
	map[u].destinations++;
	return 0;
}

void map_free(){
	size_t u;
	for(u = 0; u < mappings; u++){
		free(map[u].to);
	}
	free(map);
	mappings = 0;
	map = NULL;
}

int mm_manage_fd(int new_fd, char* back, int manage, void* impl){
	backend* b = backend_match(back);
	size_t u;

	if(!b){
		fprintf(stderr, "Unknown backend %s registered for managed fd\n", back);
		return 1;
	}

	//find exact match
	for(u = 0; u < fds; u++){
		if(fd[u].fd == new_fd && fd[u].backend == b){
			if(!manage){
				fd[u].fd = -1;
				fd[u].backend = NULL;
				fd[u].impl = NULL;
			}
			return 0;
		}
	}

	if(!manage){
		return 0;
	}

	//find free slot
	for(u = 0; u < fds; u++){
		if(fd[u].fd < 0){
			break;
		}
	}
	//if necessary expand
	if(u == fds){
		fd = realloc(fd, (fds + 1) * sizeof(managed_fd));
		if(!fd){
			fprintf(stderr, "Failed to allocate memory\n");
			return 1;
		}
		fds++;
	}

	//store new fd
	fd[u].fd = new_fd;
	fd[u].backend = b;
	fd[u].impl = impl;
	return 0;
}

void fds_free(){
	size_t u;
	for(u = 0; u < fds; u++){
		//TODO free impl
		if(fd[u].fd >= 0){
			close(fd[u].fd);
			fd[u].fd = -1;
		}
	}
	free(fd);
	fds = 0;
	fd = NULL;
}

int mm_channel_event(channel* c, channel_value v){
	size_t u, p;
	
	//find mapped channels
	for(u = 0; u < mappings; u++){
		if(map[u].from == c){
			break;
		}
	}

	if(u == mappings){
		//target-only channel
		return 0;
	}

	//resize event structures to fit additional events
	if(evs + map[u].destinations >= ev_alloc){
		ev_channel = realloc(ev_channel, (ev_alloc + map[u].destinations) * sizeof(channel*));
		ev_value = realloc(ev_value, (ev_alloc + map[u].destinations) * sizeof(channel_value));

		if(!ev_channel || !ev_value){
			fprintf(stderr, "Failed to allocate memory\n");
			ev_alloc = 0;
			evs = 0;
			return 1;
		}

		ev_alloc += map[u].destinations;
	}

	//enqueue channel events
	//FIXME this might lead to one channel being mentioned multiple times in an apply call
	for(p = 0; p < map[u].destinations; p++){
		ev_channel[evs + p] = map[u].to[p];
		ev_value[evs + p] = v;
	}

	evs += map[u].destinations;
	return 0;
}

void event_free(){
	free(ev_channel);
	free(ev_value);
	ev_alloc = 0;
}

int usage(char* fn){
	fprintf(stderr, "MIDIMonster v0.1\n");
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\t%s <configfile>\n", fn);
	return EXIT_FAILURE;
}

int main(int argc, char** argv){
	fd_set all_fds, read_fds;
	struct timeval tv;
	size_t u, n;
	managed_fd* signaled_fds = NULL;
	int rv = EXIT_FAILURE, error, maxfd = -1;
	char* cfg_file = DEFAULT_CFG;
	if(argc > 1){
		cfg_file = argv[1];
	}

	//initialize backends
	if(plugins_load(PLUGINS)){
		fprintf(stderr, "Failed to initialize a backend\n");
		goto bail;
	}

	//read config
	if(config_read(cfg_file)){
		fprintf(stderr, "Failed to read configuration file %s\n", cfg_file);
		backends_stop();
		channels_free();
		instances_free();
		map_free();
		fds_free();
		plugins_close();
		return usage(argv[0]);
	}

	//start backends
	if(backends_start()){
		fprintf(stderr, "Failed to start backends\n");
		goto bail;
	}

	signal(SIGINT, signal_handler);

	//allocate data buffers
	signaled_fds = calloc(fds, sizeof(managed_fd));
	if(!signaled_fds){
		fprintf(stderr, "Failed to allocate memory\n");
		goto bail;
	}

	//create initial fd set
	FD_ZERO(&all_fds);
	for(u = 0; u < fds; u++){
		if(fd[u].fd >= 0){
			FD_SET(fd[u].fd, &all_fds);
			maxfd = max(maxfd, fd[u].fd);
		}
	}

	//process events
	while(!shutdown_requested){
		//wait for & translate events
		read_fds = all_fds;
		tv = backend_timeout();
		error = select(maxfd + 1, &read_fds, NULL, NULL, &tv);
		if(error < 0){
			fprintf(stderr, "select failed: %s\n", strerror(errno));
			break;
		}

		//find all signaled fds
		n = 0;
		for(u = 0; u < fds; u++){
			if(fd[u].fd >= 0 && FD_ISSET(fd[u].fd, &read_fds)){
				signaled_fds[n] = fd[u];
				n++;
			}
		}

		//run backend processing, collect events
		if(backends_handle(n, signaled_fds)){
			fprintf(stderr, "Backends failed to handle input\n");
			goto bail;
		}

		//push collected events to target backends
		if(evs && backends_notify(evs, ev_channel, ev_value)){
			fprintf(stderr, "Backends failed to handle output\n");
			goto bail;
		}

		//reset the event count
		evs = 0;
	}
	
	rv = EXIT_SUCCESS;
bail:
	//free all data
	free(signaled_fds);
	backends_stop();
	channels_free();
	instances_free();
	map_free();
	fds_free();
	event_free();
	plugins_close();

	return rv;
}
