#include <stdlib.h>
#include <stdio.h>
#include <openssl/md5.h>
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
const char* SERVER_READY    = "+OK localhost pop3 server ready\r\n";
const char* SERVICE_CLOSE   = "+OK localhost service closing transmission channel\r\n";
const char* SERVICE_NA      = "-ERR localhost service not available, closing transmission channel\r\n";
const char* UNKNOWN_CMD     = "-ERR command not supported\r\n";
const char* SYNTAX_ERR      = "-ERR Syntax error in parameters or arguments\r\n";
const char* BAD_SEQ         = "-ERR Bad sequence of commands\r\n";
const char* MAILBOX_NA      = "-ERR No such mailbox\r\n";
const char* MAILBOX_EXIST   = "+OK Mailbox exists\r\n";
const char* INVALID_PASS	= "-ERR Invalid password\r\n";
const char* VALID_PASS	    = "+OK Valid password, mailbox ready\r\n";
const char* MSG_NA	        = "-ERR No such message\r\n";
const char* MSG_DELETED     = "+OK Message deleted\r\n";
const char* MSG_RESET 	    = "+OK Messages reseted\r\n";
const char* OK              = "+OK\r\n";
const char* NEW_CONN        = "New connection\r\n";
const char* CLOSE_CONN      = "Connection closed\r\n";

// global
const int BUFF_SIZE = 5000;
const int CMD_SIZE = 5;
const int RSP_SIZE = 100;
const int ARG_SIZE = 50;
bool DEBUG = false;
vector<int> SOCKETS;
vector<pthread_t> THREADS;
set<string> MAILBOXES;
char* MAILBOX_DIR;
static pthread_mutex_t mutexes[1000]; // mutexes for mailbox updating, assume max 1000 mailboxes

// Message struct for easier delete and reset
struct Message{
	string data;
	bool deleted;
	Message(const string _data){
		data = _data;
		deleted = false;
	}
};

void load_mailboxes();
int pop3_server(unsigned int port);
void signal_handler(int arg);
void *worker_thread(void *arg);
void handle_user(int comm_fd, int* state, char* buff, string& user);
void handle_pass(int comm_fd, int* state, char* buff, string& user, vector<Message>& messages, vector<string>& headers);
void handle_stat(int comm_fd, int* state, vector<Message>& messages);
void handle_list(int comm_fd, int* state, char* buff, vector<Message>& messages);
void handle_uidl(int comm_fd, int* state, char* buff, vector<Message>& messages);
void handle_retr(int comm_fd, int* state, char* buff, vector<Message>& messages);
void handle_dele(int comm_fd, int* state, char* buff, vector<Message>& messages);
void handle_rset(int comm_fd, int* state, vector<Message>& messages);
void handle_quit(int comm_fd, int* state, string& user, vector<Message>& messages, vector<string>& headers, bool* QUIT);
void handle_response(int comm_fd, const char* response);
void clear_buffer(char* buffer, char*end);
void parse_command(char* extra, char* src);
void list_msg(int comm_fd, int idx, vector<Message>& messages, bool prefix);
void uidl_msg(int comm_fd, int idx, vector<Message>& messages, bool prefix);
void computeDigest(char *data, int dataLengthBytes, unsigned char *digestBuffer);
void read_mailbox(string& user, vector<Message>& messages, vector<string>& headers);
void update_mailbox(string& user, vector<Message>& messages, vector<string>& headers);

int main(int argc, char *argv[]){
	int c;
	unsigned int port = 11000;

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
    pop3_server(port);
}

void load_mailboxes(){
	DIR *dir;
	struct dirent *ent;
	if ((dir = opendir(MAILBOX_DIR)) != NULL) {
		while ((ent = readdir (dir)) != NULL) {
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

int pop3_server(unsigned int port){
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
	// 0 - AUTHORIZATION
	// 1 - TRANSACTION
	// 2 - UPDATE


	// maintain a buffer
	char buff[BUFF_SIZE];
	char* curr = buff;
	bool QUIT = false;

	// user data
	string user;
	vector<Message> messages;
	vector<string> headers;

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
		    if (strcasecmp(command, "user ") == 0){
		    	// USER name, tells the server which user is logging in;
			    handle_user(comm_fd, &state, buff, user);
		    } else if (strcasecmp(command, "pass ") == 0){
		    	// PASS str, specifies the user's password;
		    	handle_pass(comm_fd, &state, buff, user, messages, headers);
			} else if (strcasecmp(command, "stat\r") == 0){
				// STAT, returns the number of messages and the size of the mailbox;
	            handle_stat(comm_fd, &state, messages);
			} else if (strcasecmp(command, "list ") == 0 || strcasecmp(command, "list\r") == 0){
				// LIST [msg], shows the size of a particular message, or all the messages;
				handle_list(comm_fd, &state, buff, messages);
			} else if (strcasecmp(command, "uidl") == 0 || strcasecmp(command, "uidl\r") == 0){
				// UIDL [msg], shows a list of messages, along with a unique ID for each message;
				handle_uidl(comm_fd, &state, buff, messages);
			} else if (strcasecmp(command, "retr ") == 0){
				// RETR msg, retrieves a particular message;
				handle_retr(comm_fd, &state, buff, messages);
			} else if (strcasecmp(command, "dele ") == 0){
				// DELE msg, deletes a message;
				handle_dele(comm_fd, &state, buff, messages);
			} else if (strcasecmp(command, "rset\r") == 0){
				// RSET, undelete all the messages that have been deleted with DELE;
				handle_rset(comm_fd, &state, messages);
			} else if (strcasecmp(command, "quit\r") == 0 ) {
				// QUIT, which terminates the connection
				handle_quit(comm_fd, &state, user, messages, headers, &QUIT);
			} else if (strcasecmp(command, "noop\r") == 0){
				// NOOP, which does nothing
				handle_response(comm_fd, OK);
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

void handle_user(int comm_fd, int* state, char* buff, string& user) {
	if (*state != 0 || user.length()!=0) {
		handle_response(comm_fd, BAD_SEQ);
	} else {
		// parse user name
		char rcpt[ARG_SIZE];
		parse_command(rcpt, buff);
		string mailbox(rcpt);
		mailbox += ".mbox";

		if (MAILBOXES.find(mailbox)==MAILBOXES.end()){
			handle_response(comm_fd, MAILBOX_NA);
		} else {
			user = mailbox;
			handle_response(comm_fd, MAILBOX_EXIST);
		}
	}
}

void handle_pass(int comm_fd, int* state, char* buff, string& user, vector<Message>& messages, vector<string>& headers){
	if (*state != 0 || user.length()==0){
		handle_response(comm_fd, BAD_SEQ);
	} else {
		// parse password
		char password[ARG_SIZE];
		parse_command(password, buff);

		// check password
		if (strcmp(password, "cis505")==0){
			*state = 1;
			int j = distance(MAILBOXES.begin(), MAILBOXES.find(user)); // get a constant index for a mailbox
			pthread_mutex_lock(&mutexes[j]);// mutex lock
			read_mailbox(user, messages, headers); // right, read messages and headers from mailbox
			handle_response(comm_fd, VALID_PASS);
		} else {
			user.clear(); // wrong, clear user name
			handle_response(comm_fd, INVALID_PASS);
		}
	}
}

void handle_stat(int comm_fd, int* state, vector<Message>& messages){
	if (*state != 1) {
		handle_response(comm_fd, BAD_SEQ);
	} else {
		// build response
		int count = 0, size = 0;
		for (int i = 0; i < messages.size(); i++) {
			if (!messages[i].deleted) {
				count++;
				size += messages[i].data.length();
			}
		}
		string ans = "+OK " + to_string(count) + " " + to_string(size) + "\r\n";

		handle_response(comm_fd, ans.c_str());
	}
}

void handle_list(int comm_fd, int* state, char* buff, vector<Message>& messages){
	if (*state != 1) {
		handle_response(comm_fd, BAD_SEQ);
	} else {
		// parse message
		char msg[ARG_SIZE];
		parse_command(msg, buff);
		if (strlen(msg) == 0) {
			string header = "+OK " + to_string(messages.size()) + " messages\r\n";
			handle_response(comm_fd, header.c_str());
			for (int i=0; i<messages.size();i++){
				list_msg(comm_fd, i+1, messages, false);
			}
			handle_response(comm_fd, ".\r\n");
		} else {
			list_msg(comm_fd, atoi(msg), messages, true);
		}
	}
}

void handle_uidl(int comm_fd, int* state, char* buff, vector<Message>& messages){
	if (*state != 1) {
		handle_response(comm_fd, BAD_SEQ);
	} else {
		// parse message
		char msg[ARG_SIZE];
		parse_command(msg, buff);
		if (strlen(msg) == 0) {
			string header = "+OK " + to_string(messages.size()) + " messages\r\n";
			handle_response(comm_fd, header.c_str());
			for (int i=0; i<messages.size();i++){
				uidl_msg(comm_fd, i+1, messages, false);
			}
			handle_response(comm_fd, ".\r\n");
		} else {
			uidl_msg(comm_fd, atoi(msg), messages, true);
		}
	}
}

void handle_retr(int comm_fd, int* state, char* buff, vector<Message>& messages){
	if (*state != 1) {
		handle_response(comm_fd, BAD_SEQ);
	} else {
		// parse message
		char msg[ARG_SIZE];
		parse_command(msg, buff);

		if (strlen(msg)==0) {
			// no argument on message index
			handle_response(comm_fd, SYNTAX_ERR);
		} else {
			int idx = atoi(msg);
			if (idx<1 || idx>messages.size() || messages[idx-1].deleted){
				// message not available
				handle_response(comm_fd, MSG_NA);
			} else {
				string data = messages[idx - 1].data;
				string header = "+OK " + to_string(data.length()) + " octets\r\n";
				handle_response(comm_fd, header.c_str());

				// write mail line by line
				int end;
				string line;
				for (int start=0;start<data.length();){
					end = data.find('\n',start);
					line = data.substr(start,end-start+1);
					handle_response(comm_fd, line.c_str());
					start = end+1;
				}

				handle_response(comm_fd, ".\r\n");
			}
		}
	}
}

void handle_dele(int comm_fd, int* state, char* buff, vector<Message>& messages){
	if (*state != 1) {
		handle_response(comm_fd, BAD_SEQ);
	} else {
		// parse message
		char msg[ARG_SIZE];
		parse_command(msg, buff);

		if (strlen(msg)==0) {
			// no argument on message index
			handle_response(comm_fd, SYNTAX_ERR);
		} else {
			int idx = atoi(msg);
			if (idx<1 || idx>messages.size() || messages[idx-1].deleted){
				// message not available
				handle_response(comm_fd, MSG_NA);
			} else {
				messages[idx-1].deleted = true;
				handle_response(comm_fd, MSG_DELETED);
			}
		}
	}
}

void handle_rset(int comm_fd, int* state, vector<Message>& messages){
	if (*state != 1) {
		handle_response(comm_fd, BAD_SEQ);
	} else {
		for(int i=0; i<messages.size();i++){
			messages[i].deleted = false;
		}
		handle_response(comm_fd, MSG_RESET);
	}
}

void handle_quit(int comm_fd, int* state, string& user, vector<Message>& messages, vector<string>& headers, bool* QUIT){
	if (*state == 0){
		*QUIT = true;
		handle_response(comm_fd, SERVICE_CLOSE);
	} else if (*state ==2){
		handle_response(comm_fd, BAD_SEQ);
	} else { //*state==1
		*state = 2;
		*QUIT = true;
		handle_response(comm_fd, SERVICE_CLOSE);
		update_mailbox(user, messages, headers);

		int j = distance(MAILBOXES.begin(), MAILBOXES.find(user)); // get a constant index for a mailbox
		pthread_mutex_unlock(&mutexes[j]); //mutex release
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

void parse_command(char* arg, char* src){
	int i = 0, j = 0;
	int cmd_len = strlen(src);
	while(src[i] != ' ' && i<cmd_len){ // 2 cases
		i++;
	}
	// no argument
	if (i == cmd_len) {
		arg[0] = '\0';
		return;
	}

	// with argument
	i++; // ' ' not included
	while(src[i] != '\r'){
		arg[j++] = src[i++];
	}
	arg[j] = '\0';
}

void list_msg(int comm_fd, int idx, vector<Message>& messages, bool prefix){
	if (idx < 1 || idx > messages.size() || messages[idx - 1].deleted) {
		handle_response(comm_fd, MSG_NA);
	} else {
		int len = messages[idx-1].data.length();

		string ans;
		if (prefix){
			ans = "+OK " + to_string(idx) + " " + to_string(len) + "\r\n";
		} else {
			ans = to_string(idx) + " " + to_string(len) + "\r\n";
		}
		handle_response(comm_fd, ans.c_str());
	}
}

void uidl_msg(int comm_fd, int idx, vector<Message>& messages, bool prefix){
	if (idx < 1 || idx > messages.size() || messages[idx - 1].deleted) {
		handle_response(comm_fd, MSG_NA);
	} else {
		unsigned char* digest = new unsigned char[MD5_DIGEST_LENGTH];
		char* uid = new char[MD5_DIGEST_LENGTH + 1];

		// hashing message; type requirement
		char msg[messages[idx-1].data.length() + 1];
		strcpy(msg, messages[idx-1].data.c_str());
		computeDigest(msg, strlen(msg), digest);

		// pop3 protocol use hex, format digest to uid
		for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
			sprintf(uid + i, "%02x", digest[i]);
		}

		string ans;
		if (prefix){
			ans = "+OK " + to_string(idx) + " " + string(uid) + "\r\n";
		} else {
			ans = to_string(idx) + " " + string(uid) + "\r\n";
		}
		handle_response(comm_fd, ans.c_str());

		delete[] digest;
		delete[] uid;
	}
}

void computeDigest(char *data, int dataLengthBytes, unsigned char *digestBuffer)
{
	/* The digest will be written to digestBuffer, which must be at least MD5_DIGEST_LENGTH bytes long */

	MD5_CTX c;
	MD5_Init(&c);
	MD5_Update(&c, data, dataLengthBytes);
	MD5_Final(digestBuffer, &c);
}

void read_mailbox(string& user, vector<Message>& messages, vector<string>& headers){
	ifstream mailbox;
	mailbox.open(string(MAILBOX_DIR) + "/" + user, ios_base::in);

	// read mailbox line by line until reading header, then save a message
	string data, line;
	string header = "From ";
	while (getline(mailbox, line)) {
		if (line.compare(0, header.size(), header) == 0) {
			// end with last message
			messages.push_back(Message(data));
			data = "";

			// replace \n to \r\n and save line to headers
			line.pop_back();
			line += "\r\n";
			headers.push_back(line);
		} else {
			// replace \n to \r\n and add line to data
			line.pop_back();
			line += "\r\n";
			data += line;
		}
	}
	mailbox.close();

	// drop first message(empty), and add last message, except no messages
	if (!messages.empty()) {
		messages.erase(messages.begin());
		messages.push_back(Message(data));
	}
}

void update_mailbox(string& user, vector<Message>& messages, vector<string>& headers){
	// discard old and write new
	ofstream mailbox;
	mailbox.open(string(MAILBOX_DIR) + "/" + user, ios_base::out | ios_base::trunc);

	for (int i=0; i<messages.size();i++){
		if (messages[i].deleted) continue; // drop deleted message
		mailbox << headers[i];
		mailbox << messages[i].data;
	}
	mailbox.close();
}



