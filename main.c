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
#include <tcpip.h>
#include <delaythread.h>

#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <ftw.h>
#include <stdint.h>

#include "eth.c"

#define PS2NETCON_RECV_BUFSIZE 1024 * 128

// bin2c
extern unsigned char SIO2MAN_irx[];
extern unsigned int size_SIO2MAN_irx;
extern unsigned char MCMAN_irx[];
extern unsigned int size_MCMAN_irx;
extern unsigned char MCSERV_irx[];
extern unsigned int size_MCSERV_irx;
extern unsigned char FILEIO_irx[];
extern unsigned int size_FILEIO_irx;

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
		scr_printf("ERROR: %s\n",err);
		SleepThread();
	}
}

int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    int rv = remove(fpath);
    return rv;
}

void get_ip_config(unsigned char * ip, unsigned char * nm, unsigned char * gw) {
	scr_printf("Reading IP config from mc0:/SYS-CONF/IPCONFIG.DAT ...\n");
	FILE * fh = fopen("mc0:/SYS-CONF/IPCONFIG.DAT","rb");
	if (fh == NULL) {
		goto fail_open;
	}
	
	if (fscanf(fh,"%hhu.%hhu.%hhu.%hhu ",&ip[0],&ip[1],&ip[2],&ip[3]) != 4) goto fail_syntax;
	if (fscanf(fh,"%hhu.%hhu.%hhu.%hhu ",&nm[0],&nm[1],&nm[2],&nm[3]) != 4) goto fail_syntax;
	if (fscanf(fh,"%hhu.%hhu.%hhu.%hhu",&gw[0],&gw[1],&gw[2],&gw[3]) != 4) goto fail_syntax;
	goto exit;
	
	fail_open:;
	scr_printf("Failed to open config file, using standard ...\n");
	goto fail;
	
	fail_syntax:;
	scr_printf("Invalid config syntax, using standard ...\n");
	fclose(fh);
	goto fail;
	
	fail:;
	ip[0] = 192; ip[1] = 168; ip[2] = 0; ip[3] = 10;
	nm[0] = 255; nm[1] = 255; nm[2] = 255; nm[3] = 0;
	gw[0] = 192; gw[1] = 168; gw[2] = 0; gw[3] = 1;
	
	exit:;
}

// Load file into memory
void * allocfile(size_t * size, char * fp) {
	// Open file
	FILE * f = fopen(fp,"rb");
	if (f == NULL) { return NULL; }
	
	// Get file size
	if (fseek(f,0,SEEK_END)) {
		fclose(f);
		return NULL;
	}
	
	long fl = ftell(f);
	if (fl == -1) {
		fclose(f);
		return NULL;
	}
	
	if (fseek(f,0,SEEK_SET)) {
		fclose(f);
		return NULL;
	}
	
	// Allocate memory
	void * filebuf = malloc(fl);
	if (filebuf == NULL) {
		fclose(f);
		return NULL;
	}
	
	// Read file into buffer
	size_t flr = fread(filebuf,1,fl,f);
	fclose(f);
	if (flr != fl) {
		free(filebuf);
		return NULL;
	}
	
	*size = (size_t)fl;
	return filebuf;
}

//char buffer[100];
void client_loop(int client_handler)
{
	while (1) {
		sendstr(client_handler,">");
		char ** cmd = recvcmd(client_handler);
		if (cmd == NULL) { return; }
		/*ssize_t i = 0;
		while (1) {
			if (cmd[i] == NULL) { break; }
			scr_printf("arg: '%s'\n",cmd[i]);
			i++;
		}*/
		
		ssize_t args = lencmd(cmd);
		//scr_printf("END: %d args\n",(int)args);
		
		if (args < 1) { goto loop; }
		
		// EXIT
		if (strcmp(cmd[0],"exit") == 0) {
			sendstr(client_handler,"goodbye!\n");
			goto exit;
		}
		
		// RESET
		if (strcmp(cmd[0],"reset") == 0) {
			sendstr(client_handler,"goodbye!\n");
			close(client_handler);
			
			// Wait for the connection to terminate (find a better way?)
			DelayThread(2 * 1000 * 1000);
			
			// Destroy networking
			ps2ipDeinit();
			NetManDeinit();
			sceSifExitRpc();
			
			// Reset iop
			sceSifInitRpc(0);
			while(!SifIopReset("", 0)){};
			while(!SifIopSync()){};
			sceSifInitRpc(0);
			
			// Launch OSDSYS
			LoadExecPS2("rom0:OSDSYS", 0, NULL);
			goto shutdown;
		}
		
		// HELP
		if (strcmp(cmd[0],"help") == 0) {
			sendstr(client_handler,"* exit - leave session\n");
			sendstr(client_handler,"* reset - restart system\n");
			sendstr(client_handler,"* help - this help\n");
			sendstr(client_handler,"* cd - change working directory\n");
			sendstr(client_handler,"* mkdir - make directory\n");
			sendstr(client_handler,"* rmdir - BROKEN. remove directory\n");
			sendstr(client_handler,"* rm - remove file\n");
			sendstr(client_handler,"* mv - BROKEN. move/rename file\n");
			sendstr(client_handler,"* pwd - print working directory\n");
			sendstr(client_handler,"* ls - list files\n");
			sendstr(client_handler,"* irx - load IRX module\n");
			sendstr(client_handler,"* recv - receive file\n");
			goto loop;
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
			
			if (remove(cmd[1])) {
				sendstr(client_handler,"removing file failed.\n");
			}
			
			goto loop;
		}
		
		// MV
		if (strcmp(cmd[0],"mv") == 0) {
			if (args != 3) {
				sendstr(client_handler,"syntax: mv <oldpath> <newpath>\n");
				goto loop;
			}
			
			if (rename(cmd[1],cmd[2])) {
				sendstr(client_handler,"renaming file failed.\n");
			}
			
			goto loop;
		}
		
		// PWD
		if (strcmp(cmd[0],"pwd") == 0) {
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
				if (entry->d_type == DT_DIR) { sendstr(client_handler,"/"); }
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
			
			size_t size;
			void * file = allocfile(&size,cmd[1]);
			if (file == NULL) {
				sendstr(client_handler,"failed to load file into memory!\n");
				goto loop;
			}
			
			if (SifExecModuleBuffer(file,size,0,NULL,NULL) < 0) {
				sendstr(client_handler,"failed to load module!\n");
			}
			
			free(file);
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
				ssize_t n = recvall(client_handler,buf,bytes_read,0);
				if (n < 1) { // connection closed
					fclose(fh);
					free(buf);
					goto exit;
				}
				
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
		
		shutdown:;
		freecmd(cmd);
		exit(0);
		
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
		scr_printf("Could not create socket\n");
		SleepThread();
	}
	
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(1234);
	
	qrtn((bind(server_handler,(struct sockaddr *)&server_addr,sizeof(server_addr)) < 0),"Failed to bind socket");
	qrtn((listen(server_handler,64) < 0),"Failed to listen on socket");
	scr_printf("Listening on TCP port 1234.\n");
	
	while(1)
	{
		client_handler = accept(server_handler, (struct sockaddr *)&client_addr, &client_len);
		client_loop(client_handler);
		close(client_handler);
	}
}

int main(int argc, char *argv[])
{
	while(1) {
		unsigned char ip[4];
		unsigned char netmask[4];
		unsigned char gateway[4];
		
		// Reboot IOP
		sceSifInitRpc(0);
		while(!SifIopReset("", 0)){};
		while(!SifIopSync()){};

		// Initialize SIF services
		sceSifInitRpc(0);
		SifLoadFileInit();
		SifInitIopHeap();
		sbv_patch_enable_lmb(); // Enable loading modules from mem buffer
		sbv_patch_fileio(); // Patch file I/O bugs
		
		// Initialize debug screen
		init_scr();
		scr_setbgcolor(0xFFFFFF);
		scr_setfontcolor(0x000000);
		scr_clear();
		
		// Load Memory card Modules
		SifExecModuleBuffer(SIO2MAN_irx, size_SIO2MAN_irx, 0, NULL, NULL);
		SifExecModuleBuffer(MCMAN_irx, size_MCMAN_irx, 0, NULL, NULL);
		SifExecModuleBuffer(MCSERV_irx, size_MCSERV_irx, 0, NULL, NULL);
		SifExecModuleBuffer(FILEIO_irx, size_FILEIO_irx, 0, NULL, NULL);
		
		// Get IP config
		get_ip_config(ip,netmask,gateway);
		
		// Initialize ethernet
		ethStart(ip,netmask,gateway);
		
		// Go into server loop
		server_loop();
	}
	return 0;
}
