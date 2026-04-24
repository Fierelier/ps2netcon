/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2001-2004, ps2dev - http://www.ps2dev.org
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
*/

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <stdio.h>
#include <string.h>

#include <tcpip.h>
#include "ps2ips.h"

#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <ftw.h>
#include <stdint.h>

#define PS2NETCON_RECV_BUFSIZE 1024

// bin2c
extern unsigned char ps2ips_irx[];
extern unsigned int size_ps2ips_irx;

int dir_exists(char * name) {
	int result = 0;
	DIR * d = opendir(name);
	if (d) {
		closedir(d);
		result = 1;
	}
	return result;
}

ssize_t sendall(int sockfd, void * buffer, size_t size, int flags) {
	size_t total = 0;
	size_t bytes_left = size;
	ssize_t n;
	
	while (total < size) {
		n = send(sockfd,(char *)buffer + total,bytes_left,flags);
		if (n < 1) { return -1; }
		total += n;
		bytes_left -= n;
	}
	
	return total;
}

#define sendstr(sockfd,s) sendall(sockfd,s,sizeof(s),0)

ssize_t recvall(int sockfd, void * buffer, size_t size, int flags) {
	size_t total = 0;
	size_t bytes_left = size;
	ssize_t n;
	
	while (total < size) {
		n = recv(sockfd,(char *)buffer + total,bytes_left,flags);
		if (n < 1) { return -1; }
		total += n;
		bytes_left -= n;
	}
	
	return total;
}

char * recvarg(int * is_end, int sockfd) {
	char * arg = malloc(64);
	if (arg == NULL) { return NULL; }
	ssize_t total = 0;
	ssize_t bytes_left = 64;
	ssize_t n;
	int quoted = 0;
	int escaped = 0;
	while (1) {
		n = recv(sockfd,&arg[total],1,0);
		if (n != 1) {
			free(arg);
			return NULL;
		}
		
		if (arg[total] == '\r') {
			continue;
		}
		
		if (quoted == 0 && escaped == 0) {
			if (arg[total] == ' ' || arg[total] == '\t') {
				arg[total] = 0;
				*is_end = 0;
				break;
			}
			
			if (arg[total] == '\n') {
				arg[total] = 0;
				*is_end = 1;
				break;
			}
		}
		
		if (escaped == 0) {
			if (arg[total] == '\\') {
				escaped = 1;
				continue;
			}
			
			if (quoted) {
				if (arg[total] == quoted) {
					quoted = 0;
					continue;
				}
			} else {
				if (arg[total] == '"') {
					quoted = '"';
					continue;
				}
				
				if (arg[total] == '\'') {
					quoted = '\'';
					continue;
				}
			}
		} else {
			escaped = 0;
		}
		
		total += 1;
		bytes_left -= 1;
		if (bytes_left == 0) {
			char * arg_new = realloc(arg,total + 64);
			if (arg_new == NULL) {
				free(arg);
				return NULL;
			}
			arg = arg_new;
			bytes_left = 64;
		}
	}
	return arg;
}

void freecmd(char ** cmd) {
	ssize_t n = 0;
	while (cmd[n] != NULL) {
		free(cmd[n]);
		++n;
	}
	free(cmd);
}

ssize_t lencmd(char ** cmd) {
	ssize_t n = 0;
	while (cmd[n] != NULL) {
		++n;
	}
	return n;
}

char ** recvcmd(int sockfd) {
	ssize_t i = 0;
	int is_end;
	char ** rtn = malloc(1);
	if (rtn == NULL) { return NULL; }
	
	while (1) {
		char ** new_rtn = realloc(rtn,sizeof(char *) * (i + 2));
		if (new_rtn == NULL) {
			freecmd(rtn);
			return NULL;
		}
		rtn = new_rtn;
		rtn[i + 1] = NULL;
		rtn[i] = recvarg(&is_end,sockfd);
		if (rtn[i] == NULL) {
			freecmd(rtn);
			return NULL;
		}
		
		if (rtn[i][0] == 0) {
			rtn[i] = NULL;
			--i;
		}
		
		if (is_end) { return rtn; }
		++i;
	}
	return NULL;
}

void qrtn(int comp, char * err) {
	if (comp) {
		printf("ERROR: %s\n",err);
		SleepThread();
	}
}

int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    int rv = remove(fpath);
    return rv;
}

//char buffer[100];
void client_loop(int client_handler)
{
	while (1) {
		sendstr(client_handler,">");
		char ** cmd = recvcmd(client_handler);
		if (cmd == NULL) { return; }
		ssize_t i = 0;
		while (1) {
			if (cmd[i] == NULL) { break; }
			printf("arg: '%s'\n",cmd[i]);
			i++;
		}
		
		ssize_t args = lencmd(cmd);
		printf("END: %d args\n",(int)args);
		
		if (args < 1) { goto loop; }
		
		// EXIT
		if (strcmp(cmd[0],"exit") == 0) {
			sendstr(client_handler,"goodbye!\n");
			goto exit;
		}
		
		// CD
		if (strcmp(cmd[0],"cd") == 0) {
			if (args != 2) {
				sendstr(client_handler,"syntax: cd <path>\n");
				goto loop;
			}
			
			if (chdir(cmd[1])) {
				sendstr(client_handler,"failed to change directory!\n");
			}
			
			goto loop;
		}
		
		// MKDIR
		if (strcmp(cmd[0],"mkdir") == 0) {
			if (args != 2) {
				sendstr(client_handler,"syntax: mkdir <path>\n");
				goto loop;
			}
			
			if (mkdir(cmd[1],0755)) {
				sendstr(client_handler,"failed to create directory!\n");
			}
			
			goto loop;
		}
		
		// RMDIR
		if (strcmp(cmd[0],"rmdir") == 0) {
			if (args != 2) {
				sendstr(client_handler,"syntax: rmdir <path>\n");
				goto loop;
			}
			
			/*
			sendstr(client_handler,"are you sure you want to delete '");
			sendall(client_handler,cmd[1],strlen(cmd[1]),0);
			sendstr(client_handler,"'? [Y/N]: ");
			
			char choice;
			if (recvall(client_handler,&choice,1,0) != 1) {
				goto loop;
			}
			
			
			if (choice != 'Y') {
				sendstr(client_handler,"canceled.\n");
				goto loop;
			}
			*/
			
			if (nftw(cmd[1],unlink_cb,64,FTW_DEPTH | FTW_PHYS)) {
				sendstr(client_handler,"deleting the directory failed.\n");
			}
			
			goto loop;
		}
		
		// RM
		if (strcmp(cmd[0],"rm") == 0) {
			if (args != 2) {
				sendstr(client_handler,"syntax: rm <path>\n");
				goto loop;
			}
			
			remove(cmd[1]);
			
			goto loop;
		}
		
		// CWD
		if (strcmp(cmd[0],"cwd") == 0) {
			char * cwd = malloc(PATH_MAX);
			if (cwd == NULL) {
				sendstr(client_handler,"could not allocate memory!\n");
				goto loop;
			}
			
			if (getcwd(cwd,PATH_MAX) != cwd) {
				free(cwd);
				sendstr(client_handler,"could not get current working directory!\n");
				goto loop;
			}
			
			sendall(client_handler,cwd,strlen(cwd),0);
			free(cwd);
			sendstr(client_handler,"\n");
			goto loop;
		}
		
		// LS
		if (strcmp(cmd[0],"ls") == 0) {
			if (args > 2) {
				sendstr(client_handler,"syntax: ls [path]\n");
				goto loop;
			}
			
			char * path = malloc(PATH_MAX);
			if (path == NULL) {
				sendstr(client_handler,"could not allocate memory!\n");
				goto loop;
			}
			
			if (args < 2) {
				if (getcwd(path,PATH_MAX) != path) {
					free(path);
					sendstr(client_handler,"could not get current working directory!\n");
					goto loop;
				}
			} else {
				strncpy(path,cmd[1],PATH_MAX);
				path[PATH_MAX - 1] = 0;
			}
			
			struct dirent * entry;
			DIR *dh = opendir(path);
			if (dh == NULL) {
				sendstr(client_handler,"opendir() failed on directory\n");
				free(path);
				goto loop;
			}
			
			while (1) {
				entry = readdir(dh);
				if (entry == NULL) { break; }
				sendall(client_handler,entry->d_name,strlen(entry->d_name),0);
				sendstr(client_handler,"\n");
			}
			
			closedir(dh);
			free(path);
			
			goto loop;
		}
		
		// IRX
		if (strcmp(cmd[0],"irx") == 0) {
			if (args != 2) {
				sendstr(client_handler,"syntax: irx <path>\n");
				goto loop;
			}
			
			if (SifLoadModule(cmd[1],0,NULL) < 0) {
				sendstr(client_handler,"failed to load module!\n");
			}
			
			goto loop;
		}
		
		// RECV
		if (strcmp(cmd[0],"recv") == 0) {
			if (args != 3) {
				sendstr(client_handler,"syntax: recv <path> <bytes>\n");
				goto exit;
			}
			
			FILE * fh = fopen(cmd[1],"wb");
			if (fh == NULL) {
				sendstr(client_handler,"creating '");
				sendall(client_handler,cmd[1],strlen(cmd[1]),0);
				sendstr(client_handler,"' failed!\n");
				goto exit;
			}
			
			char * buf = malloc(PS2NETCON_RECV_BUFSIZE);
			if (buf == NULL) {
				sendstr(client_handler,"allocating memory failed!\n");
				fclose(fh);
				goto exit;
			}
			
			long long bytes_left = strtoll(cmd[2],NULL,10);
			while (bytes_left > 0) {
				size_t bytes_read = PS2NETCON_RECV_BUFSIZE;
				if (bytes_left < bytes_read) { bytes_read = bytes_left; }
				// I would like to use recvall here, but it crashes the system.
				ssize_t n = recv(client_handler,buf,bytes_read,0);
				if (n < 1) { // connection closed
					fclose(fh);
					free(buf);
					goto exit;
				}
				
				//fwrite(buf,n,1,fh)
				//size_t status = ps2_fwrite(buf,n,fh);
				size_t status = fwrite(buf,n,1,fh);
				if (status != 1) {
					fclose(fh);
					free(buf);
					sendstr(client_handler,"writing to '");
					sendall(client_handler,cmd[1],strlen(cmd[1]),0);
					sendstr(client_handler,"' failed!\n");
					goto exit;
				}
				
				bytes_left -= n;
			}
			
			fclose(fh);
			free(buf);
			goto loop;
		}
		
		sendstr(client_handler,"command '");
		sendall(client_handler,cmd[0],strlen(cmd[0]),0);
		sendstr(client_handler,"' not found!\n");
		
		loop:;
		freecmd(cmd);
		continue;
		
		exit:;
		freecmd(cmd);
		return;
	}
}

void server_loop()
{
	int server_handler;
	int client_handler;
	struct sockaddr_in server_addr;
	struct sockaddr_in client_addr;
	int client_len;
	
	server_handler = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	//int blen = 1024 * 16;
	//setsockopt(server_handler, SOL_SOCKET, SO_RCVBUF, &blen, (socklen_t)sizeof(int));
	//setsockopt(server_handler, SOL_SOCKET, SO_SNDBUF, &blen, (socklen_t)sizeof(int));
	
	if (server_handler < 0)
	{
		printf("Could not create socket\n");
		SleepThread();
	}
	
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(1234);
	
	qrtn((bind(server_handler,(struct sockaddr *)&server_addr,sizeof(server_addr)) < 0),"Failed to bind socket");
	qrtn((listen(server_handler,64) < 0),"Failed to listen on socket");
	
	while(1)
	{
		client_handler = accept(server_handler, (struct sockaddr *)&client_addr, &client_len);
		client_loop(client_handler);
		close(client_handler);
	}
}

int main(int argc, char *argv[])
{
	sceSifInitRpc(0);
	SifExecModuleBuffer(ps2ips_irx, size_ps2ips_irx, 0, NULL, NULL); // TODO: Handle failure
	qrtn((ps2ip_init() < 0),"ps2ip_init failed");
	
	if (!dir_exists("host:")) {
		// TODO: initialize network hardware (see tcpip-dhcp example)
		printf("(!!!) no ps2link detected - TODO\n");
	} else {
		printf("ps2link detected\n");
	}
	
	server_loop();
	return 0;
}
