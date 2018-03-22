#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <pthread.h>
#include <arpa/inet.h>
#include <string.h>
#include <string>
#include <signal.h>
#include <vector>
#include <dirent.h>
#include <set>
#include <time.h>
#include <algorithm>
using namespace std;

// message
const char* SERVER_READY    = "220 localhost smtp server ready\r\n";
const char* SERVICE_CLOSE   = "221 localhost service closing transmission channel\r\n";
const char* SERVICE_NA      = "421 localhost service not available, closing transmission channel\r\n";
const char* HELO            = "250 localhost\r\n";
const char* OK              = "250 OK\r\n";
const char* START_MAIL      = "354 Start mail input; end with <CRLF>.\r\n";
const char* UNKNOWN_CMD     = "500 Syntax error, command unrecognized\r\n";
const char* SYNTAX_ERR      = "501 Syntax error in parameters or arguments\r\n";
const char* BAD_SEQ         = "503 Bad sequence of commands\r\n";
const char* MAILBOX_NA      = "550 Requested action not taken: mailbox unavailable\r\n";
const char* NEW_CONN        = "New connection\r\n";
const char* CLOSE_CONN 		= "Connection closed\r\n";

// global
const int BUFF_SIZE = 5000;
const int CMD_SIZE = 5;
const int RSP_SIZE = 100;
const int MAILBOX_SIZE = 50;
bool DEBUG = false;
vector<int> SOCKETS;
vector<pthread_t> THREADS;
set<string> MAILBOXES;
char* MAILBOX_DIR;
static pthread_mutex_t mutexes[1000]; // mutexes for mailbox updating, assume max 1000 mailboxes


int smtp_server(unsigned int port);
void signal_handler(int arg);
void *worker_thread(void *arg);
void handle_helo(int comm_fd, int* state, char* buff);
void handle_from(int comm_fd, int* state, char* buff, string& sender);
void handle_to(int comm_fd, int* state, char* buff, vector<string>& rcpts);
void handle_data(int comm_fd, int* state, char* buff, char* end, string& data, string& sender, vector<string>& rcpts);
void handle_rset(int comm_fd, int* state, string& data, string& sender, vector<string>& rcpts);
void handle_response(int comm_fd, const char* response);
void clear_buffer(char* buffer, char*end);
void parse_mailbox(char* dest, char* src);
void parse_mailbox(char* dest, char* host, char* src);
void load_mailboxes();


int main(int argc, char *argv[]){
	int c;
	unsigned int port = 2500;

	// getopt() for command parsing
	while((c=getopt(argc,argv,"p:av"))!=-1){
		switch(c){
		case 'p': //set port num
			port = atoi(optarg);
			break;
		case 'a': //exit
	    	cerr << "Wudao Ling (wudao) @UPenn\r\n";
	    	exit(1);
		case 'v': //debug mode
			DEBUG = true;
			break;
		default:
			cerr <<"Syntax: "<< argv[0] << " [-p port] [-a] [-v] <mailboxes directory>\r\n";
			exit(1);
		}
	}

	// check mailbox directory: remaining non-option arguments
	if (optind == argc) {
		cerr <<"Syntax: "<< argv[0] << " [-p port] [-a] [-v] <mailbox directory>\r\n";
		exit(1);
	}

	MAILBOX_DIR = new char;
	strcpy(MAILBOX_DIR, argv[optind]);
	load_mailboxes();

    //smtp server
    smtp_server(port);
}

void load_mailboxes(){
	DIR *dir;
	struct dirent *ent;
	if ((dir = opendir(MAILBOX_DIR)) != NULL) {
		while ((ent = readdir (dir)) != NULL) {
			int i = 0;
			if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0){
				string mailbox(ent->d_name);
				MAILBOXES.insert(mailbox);
			}
		}
		closedir (dir);

		// init mutexes
	    for (int i = 0; i < MAILBOXES.size(); i++){
	        pthread_mutex_init(&mutexes[i], NULL);
	    }

	} else {
		cerr << "cannot open mailbox directory\r\n";
	    exit(4);
	}
}

int smtp_server(unsigned int port){
	// handle ctrl+c signal
	signal(SIGINT, signal_handler);

    // create a new socket (TCP)
	int listen_fd = socket(PF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		cerr << "cannot open socket\r\n";
	    exit(2);
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
	listen(listen_fd, 100); // length of queue of pending connections
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

void signal_handler(int arg) {
	// close listen_fd first to prevent incoming sockets
	close(SOCKETS[0]);
	delete MAILBOX_DIR;

	for (int i = 1; i < SOCKETS.size(); i++) {
		write(SOCKETS[i], SERVICE_NA, strlen(SERVICE_NA));
		close(SOCKETS[i]);
		pthread_kill(THREADS[i - 1], 0);
	}
	exit(3);
}

void *worker_thread(void *arg){
	int comm_fd = *(int*)arg;

	// send greeting message
	write(comm_fd, SERVER_READY, strlen(SERVER_READY));
	if (DEBUG){
		cerr << "["<< comm_fd << "] " << NEW_CONN;
	}

	int state = 0;
	// 0 - INIT
	// 1 - HELO/RSET
	// 2 - MAIL
	// 3 - RCPT
	// 4 - DATA start
	// 5 - DATA end
    // 6 - QUIT

	// maintain a buffer
	char buff[BUFF_SIZE];
	char* curr = buff;
	bool QUIT = false;

	// mail data
	string sender;
	vector<string> rcpts;
	string data;

	while(true){
		char* end = new char;
		// expect to read (BUF_SIZE-curr_len) bytes to curr, assuming already read (curr_len) bytes
		int len = read(comm_fd, curr, BUFF_SIZE-strlen(buff));

		while((end=strstr(buff,"\r\n"))!=NULL){ // check whether command terminate
			// strstr return 1st index, move to real end
			end += 2;
			// read command
			char *command = new char[CMD_SIZE];
			strncpy(command, buff, CMD_SIZE);
			command[CMD_SIZE] = '\0'; // strncpy doesn't append \0 at the en

			if (DEBUG) cerr << "["<< comm_fd << "] " << "C: "<< command <<endl;

			// handle command
		    if (strcasecmp(command, "data\r") == 0 || state ==4){
		    	// DATA, which is followed by the text of the email and then a dot (.) on a line by itself
			    handle_data(comm_fd, &state, buff, end, data, sender, rcpts);
		    } else if (strcasecmp(command, "helo ") == 0){
		    	// HELO <domain>, which starts a connection
				handle_helo(comm_fd, &state, buff);
			} else if (strcasecmp(command, "mail ") == 0){
				// MAIL FROM:, which tells the server who the sender of the email is
				handle_from(comm_fd, &state, buff, sender);
			} else if (strcasecmp(command, "rcpt ") == 0){
				// RCPT TO:, which specifies the recipient
				handle_to(comm_fd, &state, buff, rcpts);
			} else if (strcasecmp(command, "rset\r") == 0){
				// RSET, aborts a mail transaction
				handle_rset(comm_fd, &state, data, sender, rcpts);
			} else if (strcasecmp(command, "noop\r") == 0){
				// NOOP, which does nothing
				handle_response(comm_fd, OK);
			} else if (strcasecmp(command, "quit\r") == 0 ) {
				// QUIT, which terminates the connection
				state = 6;
				QUIT = true;
				handle_response(comm_fd, SERVICE_CLOSE);
			} else { // unknown command
				handle_response(comm_fd, UNKNOWN_CMD);
			}

			delete[] command;
			if (QUIT) break;
			clear_buffer(buff, end);
		}

		if (QUIT) break;
		// free end after break, because end is NULL pointer here
		delete end;
		// no full line in buffer now, move curr to end
		curr = buff;
		while (*curr != '\0') {
			curr++;
		}
	}

    // terminate socket
	close(comm_fd);
	if (DEBUG) {
		cerr << "[" << comm_fd << "] " << CLOSE_CONN;
	}
	pthread_exit(NULL);
}

void handle_helo(int comm_fd, int* state, char* buff){
	// further check <domain>
	if (strlen(buff) <= CMD_SIZE){
		handle_response(comm_fd, SYNTAX_ERR);
	}

	if (*state > 1){ // already connect
		handle_response(comm_fd, BAD_SEQ);
	} else {
		*state = 1;
		handle_response(comm_fd, HELO);
	}
}

void handle_from(int comm_fd, int* state, char* buff, string& sender) {
	// further check command
	char extra[5];
	strncpy(extra, buff+CMD_SIZE, 5);
	extra[5] = '\0';
	if (strcasecmp(extra, "from:") != 0){
		handle_response(comm_fd, UNKNOWN_CMD);
	}

	if (*state != 1) {
		handle_response(comm_fd, BAD_SEQ);
	} else {
		char send[MAILBOX_SIZE];
		parse_mailbox(send, buff);

		// check sender mail address
		string candidate(send);
		if (count(candidate.begin(),candidate.end(),'@')!=1 || candidate.find("@")==0 || candidate.find("@")==candidate.length()-1){
			// only one '@', not at start or end
			handle_response(comm_fd, SYNTAX_ERR);
		} else {
			*state = 2;
			sender = candidate;
			handle_response(comm_fd, OK);
		}
	}
}

void handle_to(int comm_fd, int* state, char* buff, vector<string>& rcpts) {
	// further check command
	char extra[3];
	strncpy(extra, buff+CMD_SIZE, 3);
	extra[3] = '\0';
	if (strcasecmp(extra, "to:") != 0){
		handle_response(comm_fd, UNKNOWN_CMD);
	}

	if (*state < 2 || *state > 3) {
		handle_response(comm_fd, BAD_SEQ);
	} else {
		char rcpt[MAILBOX_SIZE];
		char host[MAILBOX_SIZE];
		parse_mailbox(rcpt, host, buff);
		string mailbox(rcpt);
		mailbox += ".mbox";

		if (strcmp(host, "localhost") != 0 || MAILBOXES.find(mailbox)==MAILBOXES.end() ){
			handle_response(comm_fd, MAILBOX_NA);
		} else {
			// TODO: check duplicate recipients?
			*state = 3;
			rcpts.push_back(mailbox);
			handle_response(comm_fd, OK);
		}
	}
}

void handle_data(int comm_fd, int* state, char* buff, char* end, string& data, string& sender, vector<string>& rcpts){
	if (*state < 3 || *state > 4){
		handle_response(comm_fd, BAD_SEQ);
	} else if(*state ==3){
		*state = 4;
		handle_response(comm_fd, START_MAIL);
	} else if(strcmp(buff,".\r\n")!=0){ // data continue
		string line(buff,end-buff);
		data += line;
	} else { // data ends
		cout << "data ends"<<endl;
		*state = 5;

		// prepare mail
		time_t now = time(0);
		string time = ctime(&now); // convert raw time to calendar time
		string header = "From <" + sender + "> " + time;

		// append mail to each mailbox with mutex
		// TODO: add flock for smtp/pop3 sync
		for (int i=0; i<rcpts.size();i++){
			int j = distance(MAILBOXES.begin(), MAILBOXES.find(rcpts[i])); // get a constant index for a mailbox
			cout << "start lock"<<endl;
		    pthread_mutex_lock(&mutexes[j]); // lock
		    cout << "after lock"<<endl;
			ofstream mailbox;
			mailbox.open(string(MAILBOX_DIR) + "/" + rcpts[i], ios_base::app);
			mailbox << header << data;
			mailbox.close();
			cout << "finish mailbox writing"<<endl;
		    pthread_mutex_unlock(&mutexes[j]); // release
		    cout << "release lock"<<endl;
		}

		// clear all
		data.clear();
		sender.clear();
		rcpts.clear();
		cout << "finish clear"<<endl;

		handle_response(comm_fd, OK);
	}

}

void handle_rset(int comm_fd, int* state, string& data, string& sender, vector<string>& rcpts) {
	if (*state == 0) {
		handle_response(comm_fd, BAD_SEQ);
	} else {
		*state = 1;
		// clear all
		data.clear();
		sender.clear();
		rcpts.clear();

		handle_response(comm_fd, OK);
	}
}

void handle_response(int comm_fd, const char* response){
	write(comm_fd, response, strlen(response));
	if (DEBUG) {
		cerr << "["<< comm_fd << "] " << "S: "<< response;
	}
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

void parse_mailbox(char* dest, char* src){
	int i = 0, j = 0;
	while(src[i] != '<'){
		i++;
	}
	i++; // <> not included
	while(src[i] != '>'){
		dest[j++] = src[i++];
	}
	dest[j] = '\0';
}

void parse_mailbox(char* dest, char* host, char* src){
	int i = 0, j = 0;
	while(src[i] != '<'){
		i++;
	}
	i++; // <> not included
	while(src[i] != '@'){
		dest[j++] = src[i++];
	}
	dest[j] = '\0';
	i++; // @ not included
	j = 0;
	while(src[i] != '>'){
		host[j++] = src[i++];
	}
	host[j] = '\0';
}




