#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <pthread.h>
#include <arpa/inet.h>
#include <string.h>
#include <signal.h>
#include <vector>
using namespace std;

// message
const char* PREFIX 			= "+OK ";
const char* GREETING 		= "+OK Server ready (Author: Wudao Ling / wudao)\r\n";
const char* GOODBYE 		= "+OK Goodbye!\r\n";
const char* NEW_CONN        = "New connection\r\n";
const char* CLOSE_CONN 		= "Connection closed\r\n";
const char* UNKNOWN_COMMAND = "-ERR Unknown command\r\n";
const char* SHUT_DOWN       = "-ERR Server shutting down\r\n";

// global
const int BUFF_SIZE = 1000;
bool DEBUG = false;
vector<int> SOCKETS;
vector<pthread_t> THREADS;

int echo_server(unsigned int port);
void *worker_thread(void *arg);
void clear_buffer(char* buffer, char*end);
void signal_handler(int arg);


int main(int argc, char *argv[]){
	int c;
	unsigned int port = 10000;

	// getopt() for command parsing
	while((c=getopt(argc,argv,"p:av"))!=-1){
		switch(c){
		case 'p': //set port no
			port = atoi(optarg);
			break;
		case 'a': //exit
	    	cerr << "Wudao Ling (wudao) @UPenn\r\n";
	    	exit(-1);
		case 'v': //debug mode
			DEBUG = true;
			break;
		case '?':
			if (optopt == 'p')
				fprintf (stderr, "Option -%c requires an argument.\n", optopt);
			else if (isprint (optopt))
				fprintf (stderr, "Unknown option `-%c'.\n", optopt);
			else
				fprintf (stderr, "Unknown option character `\\x%x'.\n",optopt);
		default:
			abort();
		}
	}
    echo_server(port);
}

int echo_server(unsigned int port){
	// handle ctrl+c signal
	signal(SIGINT, signal_handler);

    // create a new socket (TCP)
	int listen_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		cerr << "cannot open socket\r\n";
	    exit(1);
	}
	SOCKETS.push_back(listen_fd);

    // set port for reuse
	const int REUSE = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &REUSE, sizeof(REUSE));

	// bind server with a port
	struct sockaddr_in servaddr;
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htons(INADDR_ANY);
	servaddr.sin_port = htons(port);
	bind(listen_fd, (struct sockaddr*)&servaddr, sizeof(servaddr));

	// listen and accept
	listen(listen_fd, 10); // length of queue of pending connections
	while(true){
		struct sockaddr_in clientaddr;
		socklen_t clientaddrlen = sizeof(clientaddr);
		int fd = accept(listen_fd, (struct sockaddr*)&clientaddr, &clientaddrlen);
		SOCKETS.push_back(fd);

		// dispatcher assign connection to worker threads, concurrent <=100
		pthread_t thread;
		pthread_create(&thread, NULL, worker_thread, &fd);
		THREADS.push_back(thread);
		// mark resource of thread for reclaim when it terminates
		pthread_detach(thread);
    }
    return 0;
}

void *worker_thread(void *arg){
	int comm_fd = *(int*)arg;

	// send greeting message
	write(comm_fd, GREETING, strlen(GREETING));
	if (DEBUG){
		cerr << "["<< comm_fd << "] " << NEW_CONN;
	}

	// maintain a buffer
	char buff[BUFF_SIZE];
	char* curr = buff;
	bool QUIT = false;

	while(true){
		char* end = new char;
		// expect to read (BUF_SIZE-curr_len) bytes to curr, assuming already read (curr_len) bytes
		read(comm_fd, curr, BUFF_SIZE-strlen(buff));

		while((end=strstr(buff,"\r\n"))!=NULL){ // check whether command terminate
			// strstr return 1st index, move to real end
			end += 2;
			// read command
			char *command = new char[5];
			strncpy(command, buff, 5);
			command[5] = '\0'; // strncpy doesn't append \0 at the end

			char text[BUFF_SIZE];
			char* start = buff + strlen(command);

            // handle command
			if (strcasecmp(command, "echo ") == 0){ // ECHO_
				strcpy(text, PREFIX);
				strncpy(&text[4], start, end-start); // append after prefix
				text[end-start + 4] = '\0';
				write(comm_fd, text, strlen(text));

			} else if (strcasecmp(command, "quit\r") == 0 && buff[5]=='\n') { // QUIT<CR><LF>
				QUIT = true;
				strcpy(text, GOODBYE);
				write(comm_fd, text, strlen(text));
				if (DEBUG) {
					cerr << "["<< comm_fd << "] " << "C: "<< command <<endl;
					cerr << "["<< comm_fd << "] " << "S: "<< text;
				}
				break;

			} else { // unknown command
				strcpy(text, UNKNOWN_COMMAND);
				write(comm_fd, text, strlen(text));
			}

			if (DEBUG) {
				cerr << "["<< comm_fd << "] " << "C: "<< command <<endl;
				cerr << "["<< comm_fd << "] " << "S: "<< text;
			}
			clear_buffer(buff, end);
			delete command;
		}

		if (QUIT) break;

		// no full line in buffer now, move curr to end
		curr = buff;
		while (*curr != '\0') {
			curr++;
		}
		delete end;
	}

    // terminate socket
	close(comm_fd);
	if (DEBUG) {
		cerr << "[" << comm_fd << "] " << CLOSE_CONN;
	}
	pthread_exit(NULL);
}

void clear_buffer(char *buff, char *end){
	char* curr = buff;
	// move remaining to the start
	while (*end != '\0') {
		*curr++ = *end;
		*end++ = '\0';
	}
	// clear rest of last command
	while (*curr != '\0') {
		*curr++ = '\0';
	}
}

void signal_handler(int arg) {
	// close listen_fd first to prevent incoming sockets
	close(SOCKETS[0]);
	cout << "\n" << SHUT_DOWN;

	for (int i = 1; i < SOCKETS.size(); i++) {
		write(SOCKETS[i], SHUT_DOWN, strlen(SHUT_DOWN));
		close(SOCKETS[i]);
		pthread_kill(THREADS[i - 1], 0);
	}
	exit(2);
}
