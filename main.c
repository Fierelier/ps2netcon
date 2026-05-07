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
#include <elf-loader.h>

#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <ftw.h>
#include <stdint.h>
#include <errno.h>

#include "eth.c"

#define PS2NETCON_RECV_BUFSIZE 1024 * 128
#define PS2NETCON_CP_BUFSIZE 1024 * 1024

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

int sendprint(int sockfd, char * format, ...) {
	va_list args;
	va_start(args,format);
	int length = vsnprintf((char *)NULL,0,format,args);
	if (length < 1) {
		va_end(args);
		return length;
	}
	
	char * str = malloc(length + 1);
	if (str == NULL) {
		va_end(args);
		return -1;
	}
	
	length = vsnprintf(str,length + 1,format,args);
	if (length < 0) {
		free(str);
		va_end(args);
		return length;
	}
	
	scr_printf(str);
	if (sendall(sockfd,str,length,0) < 0) {
		free(str);
		va_end(args);
		return -1;
	}
	
	free(str);
	va_end(args);
	return length;
}

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

int get_first_file(DIR * dh, struct dirent ** entry) {
	errno = 0;
	rewinddir(dh);
	while (1) {
		*entry = readdir(dh);
		if (*entry == NULL) {
			if (errno != 0) { return 1; }
			return 0;
		}
		
		if (strcmp((*entry)->d_name,".") == 0) { continue; }
		if (strcmp((*entry)->d_name,"..") == 0) { continue; }
		return 0;
	}
	return 1;
}

int is_directory_empty(DIR * dh) {
	struct dirent * entry;
	if (get_first_file(dh,&entry)) {
		return -1;
	}
	if (entry == NULL) { return 1; }
	return 0;
}

// TODO: check pathbuf bounds
int get_first_removable(char * pathbuf, char * path) {
	struct dirent * entry;
	DIR *dh = NULL;
	strcpy(pathbuf,path);
	
	while (1) {
		dh = opendir(pathbuf);
		if (dh == NULL) { return 1; }
		if (get_first_file(dh,&entry)) {
			closedir(dh);
			return 1;
		}
		
		if (entry == NULL) {
			closedir(dh);
			return 0;
		}
		
		strcat(pathbuf,"/");
		strcat(pathbuf,entry->d_name);
		
		if (entry->d_type == DT_DIR) {
			closedir(dh);
			continue;
		}
		
		closedir(dh);
		return 0;
	}
	
	return 1;
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
		sendprint(client_handler,">");
		char ** cmd = recvcmd(client_handler);
		if (cmd == NULL) { return; }
		scr_clear();
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
			sendprint(client_handler,"goodbye!\n");
			goto exit;
		}
		
		// RESET
		if (strcmp(cmd[0],"reset") == 0) {
			FlushCache(0);
			FlushCache(2);
			sendprint(client_handler,"goodbye!\n");
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
			sendprint(client_handler,"* exit - leave session\n");
			sendprint(client_handler,"* reset - restart system\n");
			sendprint(client_handler,"* help - this help\n");
			sendprint(client_handler,"* cd - change working directory\n");
			sendprint(client_handler,"* mkdir - make directory\n");
			sendprint(client_handler,"* rmdir - remove directory\n");
			sendprint(client_handler,"* rm - remove file\n");
			sendprint(client_handler,"* mv - BROKEN. move/rename file\n");
			sendprint(client_handler,"* pwd - print working directory\n");
			sendprint(client_handler,"* ls - list files\n");
			sendprint(client_handler,"* irx - load IRX module\n");
			sendprint(client_handler,"* elf - launch ELF file\n");
			sendprint(client_handler,"* recv - receive file\n");
			goto loop;
		}
		
		// CD
		if (strcmp(cmd[0],"cd") == 0) {
			if (args != 2) {
				sendprint(client_handler,"syntax: cd <path>\n");
				goto loop;
			}
			
			if (chdir(cmd[1])) {
				sendprint(client_handler,"failed to change directory!\n");
			}
			
			goto loop;
		}
		
		// MKDIR
		if (strcmp(cmd[0],"mkdir") == 0) {
			if (args != 2) {
				sendprint(client_handler,"syntax: mkdir <path>\n");
				goto loop;
			}
			
			if (mkdir(cmd[1],0755)) {
				sendprint(client_handler,"failed to create directory!\n");
			}
			
			goto loop;
		}
		
		// RMDIR
		if (strcmp(cmd[0],"rmdir") == 0) {
			if (args != 2) {
				sendprint(client_handler,"syntax: rmdir <path>\n");
				goto loop;
			}
			
			sendprint(client_handler,"are you sure you want to delete '%s' and its contents? [Y/N]: ",cmd[1]);
			
			char choice[2];
			if (recvall(client_handler,choice,2,0) != 2) {
				goto loop;
			}
			
			if (strcmp(choice,"Y\n") != 0) {
				sendprint(client_handler,"canceled.\n");
				goto loop;
			}
			
			char * pathbuf = malloc(PATH_MAX);
			if (pathbuf == NULL) {
				sendprint(client_handler,"failed to allocate memory!\n");
				goto loop;
			}
			
			while (1) {
				if (get_first_removable(pathbuf,cmd[1])) {
					sendprint(client_handler,"finding removable file failed.\n");
					free(pathbuf);
					goto loop;
				}
				
				int rtn = remove(pathbuf);
				if (rtn) {
					sendprint(client_handler,"removing '%s' failed (error: %d).\n",pathbuf,rtn,rtn);
					free(pathbuf);
					goto loop;
				}
				
				if (strcmp(cmd[1],pathbuf) == 0) {
					free(pathbuf);
					goto loop;
				}
			}
			
			free(pathbuf);
			goto loop;
		}
		
		// RM
		if (strcmp(cmd[0],"rm") == 0) {
			if (args != 2) {
				sendprint(client_handler,"syntax: rm <path>\n");
				goto loop;
			}
			
			int rtn = remove(cmd[1]);
			if (rtn) {
				sendprint(client_handler,"removing '%s' failed (error: %d).\n",cmd[1],rtn);
			}
			
			goto loop;
		}
		
		// MV
		if (strcmp(cmd[0],"mv") == 0) {
			if (args != 3) {
				sendprint(client_handler,"syntax: mv <oldpath> <newpath>\n");
				goto loop;
			}
			
			int rtn = rename(cmd[1],cmd[2]);
			if (rtn) {
				sendprint(client_handler,"renaming '%s' to '%s' failed (error: %d).\n",cmd[1],cmd[2],rtn);
			}
			
			goto loop;
		}
		
		// CP
		if (strcmp(cmd[0],"cp") == 0) {
			if (args != 3) {
				sendprint(client_handler,"syntax: cp <frompath> <topath>\n");
				goto loop;
			}
			
			FILE * ffrom = fopen(cmd[1],"rb");
			if (ffrom == NULL) {
				sendprint(client_handler,"could not open '%s' (error: %d).\n",cmd[1],errno);
				goto loop;
			}
			
			FILE * fto = fopen(cmd[2],"wb");
			if (fto == NULL) {
				sendprint(client_handler,"could not open '%s' (error: %d).\n",cmd[1],errno);
				fclose(ffrom);
				goto loop;
			}
			
			void * buf = malloc(PS2NETCON_CP_BUFSIZE);
			if (buf == NULL) {
				sendprint(client_handler,"could not allocate memory.\n");
				fclose(ffrom);
				fclose(fto);
				goto loop;
			}
			
			struct stat srs;
			if (fstat(fileno(ffrom),&srs)) {
				sendprint(client_handler,"could not stat '%s'.\n",cmd[1]);
				fclose(ffrom);
				fclose(fto);
				free(buf);
			}
			
			long long size = (long long)(srs.st_size);
			long long total = 0;
			u64 current_time;
			u64 last_print = GetTimerSystemTime() / 147456;
			
			sendprint(client_handler,"progress: %lld / %lld KB\r",total,(size / 1000) + 1);
			
			while (1) {
				size_t sr = fread(buf,1,PS2NETCON_CP_BUFSIZE,ffrom);
				if (sr > 0) {
					size_t sw = fwrite(buf,1,sr,fto);
					total += sw;
					
					current_time = GetTimerSystemTime() / 147456;
					if (last_print > current_time) { last_print = current_time; }
					
					if (current_time - last_print >= 1000) {
						sendprint(client_handler,"progress: %lld / %lld KB\r",(total / 1000) + 1,(size / 1000) + 1);
						last_print = current_time;
					}
					
					if (sw < sr) {
						sendprint(client_handler,"progress: %lld / %lld KB\r",(total / 1000) + 1,(size / 1000) + 1);
						sendprint(client_handler,"\nwriting to '%s' failed (error: %d).\n",cmd[2],ferror(fto));
						break;
					}
				}
				
				if (sr < PS2NETCON_CP_BUFSIZE) {
					sendprint(client_handler,"progress: %lld / %lld KB\r",(total / 1000) + 1,(size / 1000) + 1);
					if (ferror(ffrom)) {
						sendprint(client_handler,"\nreading from '%s' failed (error: %d).\n",cmd[1],ferror(ffrom));
					} else {
						sendprint(client_handler,"\n");
					}
					break;
				}
			}
			
			fclose(ffrom);
			fclose(fto);
			free(buf);
			FlushCache(0);
			FlushCache(2);
			goto loop;
		}
		
		// PWD
		if (strcmp(cmd[0],"pwd") == 0) {
			char * cwd = malloc(PATH_MAX);
			if (cwd == NULL) {
				sendprint(client_handler,"could not allocate memory!\n");
				goto loop;
			}
			
			if (getcwd(cwd,PATH_MAX) != cwd) {
				free(cwd);
				sendprint(client_handler,"could not get current working directory!\n");
				goto loop;
			}
			
			sendprint(client_handler,"%s\n",cwd);
			free(cwd);
			goto loop;
		}
		
		// LS
		if (strcmp(cmd[0],"ls") == 0) {
			if (args > 2) {
				sendprint(client_handler,"syntax: ls [path]\n");
				goto loop;
			}
			
			char * path = malloc(PATH_MAX);
			if (path == NULL) {
				sendprint(client_handler,"could not allocate memory!\n");
				goto loop;
			}
			
			if (args < 2) {
				if (getcwd(path,PATH_MAX) != path) {
					free(path);
					sendprint(client_handler,"could not get current working directory!\n");
					goto loop;
				}
			} else {
				strncpy(path,cmd[1],PATH_MAX);
				path[PATH_MAX - 1] = 0;
			}
			
			struct dirent * entry;
			DIR *dh = opendir(path);
			if (dh == NULL) {
				sendprint(client_handler,"opendir() failed on directory\n");
				free(path);
				goto loop;
			}
			
			while (1) {
				entry = readdir(dh);
				if (entry == NULL) { break; }
				if (strcmp(entry->d_name,".") == 0 || strcmp(entry->d_name,"..") == 0) {
					continue;
				}
				sendprint(client_handler,entry->d_name);
				if (entry->d_type == DT_DIR) { sendprint(client_handler,"/"); }
				sendprint(client_handler,"\n");
			}
			
			closedir(dh);
			free(path);
			
			goto loop;
		}
		
		// IRX
		if (strcmp(cmd[0],"irx") == 0) {
			if (args != 2) {
				sendprint(client_handler,"syntax: irx <path>\n");
				goto loop;
			}
			
			size_t size;
			void * file = allocfile(&size,cmd[1]);
			if (file == NULL) {
				sendprint(client_handler,"failed to load file into memory!\n");
				goto loop;
			}
			
			if (SifExecModuleBuffer(file,size,0,NULL,NULL) < 0) {
				sendprint(client_handler,"failed to load module!\n");
			}
			
			free(file);
			goto loop;
		}
		
		// ELF
		if (strcmp(cmd[0],"elf") == 0) {
			if (args < 2) {
				sendprint(client_handler,"syntax: elf <file> [arg 1] ...\n");
				goto loop;
			}
			
			char ** cmd_elf = malloc((args - 1) * sizeof(char *));
			if (cmd_elf == NULL) {
				sendprint(client_handler,"failed to allocate memory!\n");
				goto loop;
			}
			
			size_t i = 1;
			while (i < args) {
				cmd_elf[i - 1] = cmd[i];
				++i;
			}
			
			FlushCache(0);
			FlushCache(2);
			sendprint(client_handler,"goodbye!\n");
			close(client_handler);
			
			// Wait for the connection to terminate (find a better way?)
			DelayThread(2 * 1000 * 1000);
			
			// Destroy networking
			ps2ipDeinit();
			NetManDeinit();
			
			// Launch ELF
			LoadELFFromFile(cmd_elf[0], args - 1, cmd_elf);
			goto shutdown;
		}
		
		// RECV
		if (strcmp(cmd[0],"recv") == 0) {
			if (args != 3) {
				sendprint(client_handler,"syntax: recv <path> <bytes>\n");
				goto exit;
			}
			
			FILE * fh = fopen(cmd[1],"wb");
			if (fh == NULL) {
				sendprint(client_handler,"creating '%s' failed!\n",cmd[1]);
				goto exit;
			}
			
			char * buf = malloc(PS2NETCON_RECV_BUFSIZE);
			if (buf == NULL) {
				sendprint(client_handler,"allocating memory failed!\n");
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
					sendprint(client_handler,"writing to '%s' failed!\n",cmd[1]);
					goto exit;
				}
				
				bytes_left -= n;
			}
			
			fclose(fh);
			free(buf);
			FlushCache(0);
			FlushCache(2);
			goto loop;
		}
		
		sendprint(client_handler,"command '%s' not found!\n",cmd[0]);
		
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
