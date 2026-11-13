/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tiny loopback HTTP server: serves files from argv[2] on 127.0.0.1:argv[1];
   GET /log?<msg> appends the decoded msg to argv[3]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
static void urldecode(char *s){char *o=s;while(*s){if(*s=='%'&&s[1]&&s[2]){char h[3]={s[1],s[2],0};*o++=(char)strtol(h,0,16);s+=3;}else if(*s=='+'){*o++=' ';s++;}else *o++=*s++;}*o=0;}
int main(int argc,char**argv){
  if(argc<4){fprintf(stderr,"usage: port dir logfile\n");return 1;}
  signal(SIGPIPE,SIG_IGN);
  int s=socket(AF_INET,SOCK_STREAM,0),one=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
  struct sockaddr_in a={0};a.sin_family=AF_INET;a.sin_port=htons(atoi(argv[1]));a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(bind(s,(void*)&a,sizeof a)||listen(s,16)){perror("bind");return 1;}
  for(;;){int c=accept(s,0,0);if(c<0)continue;char buf[16384];int n=read(c,buf,sizeof buf-1);if(n<=0){close(c);continue;}buf[n]=0;
    char path[8192]={0};sscanf(buf,"GET %8191s",path);
    if(!strncmp(path,"/log?",5)){urldecode(path+5);FILE*f=fopen(argv[3],"a");if(f){fprintf(f,"%s\n",path+5);fclose(f);}
      const char*r="HTTP/1.0 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";write(c,r,strlen(r));}
    else{char fp[9000];if(strstr(path,".."))path[0]=0;char*q=strchr(path,'?');if(q)*q=0;snprintf(fp,sizeof fp,"%s%s",argv[2],path);
      FILE*f=fopen(fp,"rb");if(!f){const char*r="HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n";write(c,r,strlen(r));}
      else{const char*ct=strstr(fp,".html")?"text/html; charset=utf-8":strstr(fp,".js")?"text/javascript":"application/octet-stream";
        char h[256];int hl=snprintf(h,sizeof h,"HTTP/1.0 200 OK\r\nContent-Type: %s\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",ct);write(c,h,hl);
        char b[65536];size_t k;while((k=fread(b,1,sizeof b,f))>0)if(write(c,b,k)<0)break;fclose(f);}}
    close(c);}
}
