# Upenn CIS505 HW2  
A multi-thread mail server with SMTP protocol for transmission and POP3 protocol for retrieve.  

## Syntax
./smtp [-p port] [-a] [-v] <mailboxes directory>   
./pop3 [-p port] [-a] [-v] <mailboxes directory>  
-p followed by port number, -a is the flag to return author name, -v is the flag that enable information for debug

## Usage
### create mailboxes in terminal
mkdir mailboxes
touch mailboxes/wudao.mbox
### set up thunderbird account and outgoing server
1. create account with *@localhost* and password *cis505*  
2. set incoming protocol as POP3, port *11000*, None for SSL and Normal Password for authentication  
3. set outgoing protocol as SMTP, port *2500*, None for SSL and No authentication  
4. both incoming and outgoing server name are set to *localhost*
### write and read emails among these mailboxes
+ through thunderbird
+ through tests inside ./test
+ through telnet localhost *port* in terminal and protocol command


