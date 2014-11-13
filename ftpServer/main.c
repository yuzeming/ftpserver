//
//  main.c
//  Wind Ftp Server
//
//  Created by 俞则明 on 14/11/10.
//  Copyright (c) 2014年 ZemingYU. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <assert.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/ftp.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>

#define CHECK(b,s) CHECK_LINE_FILE_(b,s,__LINE__,__FILE__)
#define CHECKRET(b,s)CHECK_LINE_FILE_( (b) == 0,s,__LINE__,__FILE__)

#define CHECK_LINE_FILE_(b,s,l,f)  \
do{ \
    if (!b) \
    { \
        fprintf(stderr,"%s:%d %s\n err:%d:%s\n",f,l,s,errno,strerror(errno)); \
        exit(EXIT_FAILURE); \
    } \
} while(0)

#define SendMsg(fd,str) send(fd,str,strlen(str),0)

#define anonymous_USER "anonymous"

//Config
#define USER_LEN 30
char _Root[MAXPATHLEN] = "/tmp";
char _User[USER_LEN]=anonymous_USER;
char _Pass[USER_LEN];
int _Port = 21;
int _Active_port = 20;
char _Ip[] = "0.0.0.0";

int sockfd;
struct sockaddr_in addr,client_addr;
socklen_t addr_size;


struct session_t
{
    int sockfd;     // 控制连接
    int pasvfd;     // 被动连接
    int datafd;     // 数据连接
    int type;       // 传输类型
    int state;      // 连接状态
    int isAuth;

    struct sockaddr_in dataaddr;
    socklen_t dataaddr_size;
    
    char user[USER_LEN+1];
    char pwd[MAXPATHLEN];
    char root[MAXPATHLEN];
    
    char oldpath[MAXPATHLEN];
};

char * path_join_jo(char* ret,const char *a,const char *b)
{
    size_t la = strlen(a),lb = strlen(b);
    char * tmp = ret;
    
    ret[0]='\0';
    if (strstr(a, "..") != NULL||strstr(b, "..") != NULL||la + lb + 2 > MAXPATHLEN)
        return ret;
    
    tmp = stpcpy(tmp, a);

    if (b[0]=='/')
        strcpy(ret, b);
    else
    {
        if (a[la-1]!='/'&&b[0]!='/')
            tmp = stpcpy(tmp, "/");
        tmp = stpcpy(tmp, b);
    }
    return ret;
}

char * path_join(char* ret,const char *a,const char *b)
{
    size_t la = strlen(a),lb = strlen(b);
    char * tmp = ret;
    
    ret[0]='\0';
    if (strstr(a, "..") != NULL||strstr(b, "..") != NULL||la + lb + 2 > MAXPATHLEN)
        return ret;
    
    tmp = stpcpy(tmp, a);

    if (a[la-1]!='/'&&b[0]!='/')
        tmp = stpcpy(tmp, "/");
    tmp = stpcpy(tmp, b);
    
    return ret;
}

void RESET_FD(struct session_t *session)
{
    if (session->pasvfd !=0)
    {
        close(session->pasvfd);
        session->pasvfd = 0;
    }
    if (session->datafd !=0)
    {
        close(session->datafd);
        session->datafd = 0;
    }
    session->state = 0;
    
}

size_t get_file_size(const char path[])
{
    struct stat tmp;
    if (stat(path,&tmp) != 0  )
        return -1;
    return tmp.st_size;
}

void command_USER(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    strncpy(session->user, param, USER_LEN);
    session->isAuth = 0;
    if (strlen(session->user)==0)
        strcpy(session->user,"anonymous");
    sprintf(buff, "331 User %s OK. Password required\r\n",session->user);
    send(session->sockfd,buff,strlen(buff),0);
}

void command_PASS(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    if (strlen(session->user) == 0)
    {
        strcpy(buff, "530 Please tell me who you are\r\n");
        send(session->sockfd, buff, strlen(buff),0);
        return ;
    }
    if (strcmp(session->user,_User)==0&&
        (strcmp(session->user,anonymous_USER)==0||strcmp(param,_Pass)==0))
    {
        session->isAuth = 1;
        strcpy(session->root, _Root);
        strcpy(session->pwd,"/");
        sprintf(buff,"230 OK. Current directory is %s\r\n",session->pwd);
    }
    else
        sprintf(buff,"530 Login authentication failed\r\n");
    send(session->sockfd, buff, strlen(buff),0);
}

void _command_Welcome(struct session_t *session,char *param)
{
    SendMsg(session->sockfd,"220-Welcome Wind Ftp Server\r\n220 By-StarQoQ\r\n");
}

void command_TYPE(struct session_t *session,char *param)
{
    if (param[0]=='I') {
        session->type = 1;
        SendMsg(session->sockfd,"200 TYPE is now 8-bit binary\r\n");
    }
    else if (param[0]=='A') {
        session->type = 2;
        SendMsg(session->sockfd,"200 TYPE is now ASCII binary\r\n");
    }
    else
        SendMsg(session->sockfd,"504 Unknown TYPE.\r\n");
}

void command_CWD(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    char abspwd[MAXPATHLEN];
    char newpwd[MAXPATHLEN];
    struct stat tmp;
    int ret=0;
    path_join_jo(newpwd,session->pwd, param);
    path_join(abspwd, session->root, newpwd);
    if ((ret=stat(abspwd,&tmp)) == 0 && S_ISDIR(tmp.st_mode) )
    {
        strcpy(session->pwd, newpwd);
        sprintf(buff, "250 OK. Current directory is %s\r\n",session->pwd);
    }
    else
    {
        char* res=NULL;
        if (ret==0)
            res = "No such file or directory";
        else
            res = "Not a directory";
        sprintf(buff, "550 Can't change directory to %s:%s\r\n",param,res);
    }
    send(session->sockfd, buff,strlen(buff),0);
}

void command_PWD(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    sprintf(buff, "257 \"%s\" is your current location\r\n",session->pwd);
    send(session->sockfd, buff,strlen(buff),0);
}

void _command_NotLogin(struct session_t *session,char *param)
{
    SendMsg(session->sockfd,"530 You aren't logged in.\r\n");
}

void command_CDUP(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    
    size_t len = strlen(session->pwd);
    while (len > 0 && session->pwd[len]!='/')
        --len;
    session->pwd[len] = '\0';
    if (strlen(session->pwd)==0)
        strcpy(session->pwd, "/");
    
    sprintf(buff, "250 OK. Current directory is %s\r\n",session->pwd);
    send(session->sockfd, buff,strlen(buff),0);
}

void command_PASV(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    unsigned short pasvport = rand() % 40000 + 20000;
    struct sockaddr_in pasvaddr;
    socklen_t addr_len = sizeof(pasvaddr);
    unsigned char* s =(void *) &pasvaddr.sin_addr;
    
    RESET_FD(session);
    
    getsockname(session->sockfd, (struct sockaddr*)&pasvaddr, &addr_len);
    pasvaddr.sin_port = htons(pasvport);
    
    session->state = 1;
    session->pasvfd = socket(PF_INET,SOCK_STREAM,0);
    
    while (bind(session->pasvfd,(struct sockaddr *) (&pasvaddr),sizeof(pasvaddr))==-1 && errno == EADDRINUSE)
    {
        pasvport = rand() % 40000 + 20000;
        pasvaddr.sin_port = htons(pasvport);
    }
    
    listen(session->pasvfd, 10);
    
    sprintf(buff,"227 Entering Passive Mode (%hhu,%hhu,%hhu,%hhu,%d,%d)\r\n",s[0],s[1],s[2],s[3],pasvport/256,pasvport%256);
    send(session->sockfd, buff,strlen(buff),0);
}

void command_PORT(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    int h1,h2,h3,h4,p1,p2;
    
    sscanf(param,"%d,%d,%d,%d,%d,%d",&h1,&h2,&h3,&h4,&p1,&p2);
    sprintf(buff, "%d.%d.%d.%d",h1,h2,h3,h4);
    
    RESET_FD(session);
    
    inet_pton(AF_INET,buff,&(session->dataaddr.sin_addr));
    session->dataaddr.sin_family = AF_INET;
    session->dataaddr.sin_port = htons(p1*256+p2);
    session->dataaddr_size = sizeof(session->dataaddr);
    session->state = 2;
    SendMsg(session->sockfd, "200 PORT command successful.\r\n");
}

void command_QUIT(struct session_t *session,char *param)
{
    session->state = -1;
    SendMsg(session->sockfd, "221 Goodbye.\r\n");
}

size_t CopyTo(int src_fd,int dst_fd)
{
    char buff[BUFSIZ];
    size_t szrd,szwt,tot=0;
    while ( (szrd = read(src_fd, buff, sizeof(buff)) )>0)
    {
        char * tmp = buff;
        szwt=0;
        while(szrd >0 && (szwt = write(dst_fd, buff, szrd)) !=-1)
        {
            tmp += szwt;
            szrd -= szwt;
            tot += szwt;
        }
        if (szwt == -1) break;
    }
    return tot;
}

int get_data_connect(struct session_t *session)
{
    int true_val = 1;
    struct sockaddr_in addr;
    inet_pton(AF_INET,_Ip,&(addr.sin_addr));
    addr.sin_port = htons(_Active_port);
    addr.sin_family = AF_INET;
    if (session->state == 0)
        return -1;
    if (session->datafd !=0)
    {
        close(session->datafd);
        session->datafd = 0;
    }
    if (session->pasvfd!=0)
        session->datafd = accept(session->pasvfd,(struct sockaddr*) &session->dataaddr, &session->dataaddr_size);
    else
    {
        session->datafd = socket(PF_INET,SOCK_STREAM,0);
        setsockopt(session->datafd, SOL_SOCKET, SO_REUSEPORT, &true_val, sizeof(true_val));
        bind(session->datafd,(struct sockaddr *)&addr,sizeof(addr));
        connect(session->datafd, (struct sockaddr*) &session->dataaddr, session->dataaddr_size);
    }
    return session->datafd;
}

void command_RETR(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    char path[MAXPATHLEN];
    char abspath[MAXPATHLEN];
    path_join_jo(path, session->pwd, param);
    path_join(abspath, session->root, path);
    
    int fd = open(abspath,O_RDONLY|O_SHLOCK);
    if (fd == -1)
    {
        sprintf(buff, "550 Can't open %s: %s\r\n",param,strerror(errno));
        send(session->sockfd, buff,strlen(buff),0);
        return ;
    }
    if (get_data_connect(session) == -1)
    {
        SendMsg(session->sockfd,"425 Can't build data connection ");
        return ;
    }
    
    sprintf(buff, "150 Opening BINARY mode data connection for '%s'. (%zu bytes)\r\n",param,get_file_size(abspath));
    send(session->sockfd, buff,strlen(buff),0);
    
    CopyTo(fd, session->datafd);
    
    close(fd);
    shutdown(session->datafd, SHUT_RDWR);
    session->datafd = 0;
    
    SendMsg(session->sockfd, "226 Transfer complete.\r\n");
}

void command_STOR(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    char path[MAXPATHLEN];
    char abspath[MAXPATHLEN];
    path_join_jo(path, session->pwd, param);
    path_join(abspath, session->root, path);
    
    int fd = open(abspath,O_WRONLY|O_EXLOCK|O_CREAT);
    
    if (fd == -1)
    {
        sprintf(buff, "550 Can't open %s: %s\r\n",param,strerror(errno));
        send(session->sockfd, buff,strlen(buff),0);
        return ;
    }
    if (get_data_connect(session) == -1)
    {
        SendMsg(session->sockfd,"425 Can't build data connection ");
        return ;
    }
    
    sprintf(buff, "150 Opening BINARY mode data connection for '%s'.\r\n",param);
    send(session->sockfd, buff,strlen(buff),0);
    
    CopyTo(session->datafd,fd);
    
    close(fd);
    shutdown(session->datafd, SHUT_RDWR);
    session->datafd = 0;
    chmod(abspath, S_IRWXU|S_IRWXG|S_IRWXO);
    SendMsg(session->sockfd,"226 Transfer complete.\r\n");

}

void command_LIST(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    char path[MAXPATHLEN];
    char abspath[MAXPATHLEN];
    path_join_jo(path, session->pwd, param);
    path_join(abspath, session->root, path);
    strcpy(buff,"ls -l \"");
    strcat(buff, abspath);
    strcat(buff, "\"");
    printf("%s\n",buff);
    FILE* f = popen(buff,"r");
    
    if (get_data_connect(session) == -1)
    {
        SendMsg(session->sockfd,"425 Can't build data connection ");
        return ;
    }
    SendMsg(session->sockfd, "150 Opening ASCII mode data connection for '/bin/ls'.\r\n");
    CopyTo(fileno(f), session->datafd);
    pclose(f);
    shutdown(session->datafd, SHUT_RDWR);
    session->datafd = 0;
    
    SendMsg(session->sockfd,"226 Transfer complete.\r\n");

}

void command_SYST(struct session_t *session,char *param)
{
    SendMsg(session->sockfd,"215 UNIX Type: L8\r\n");
}

void command_MKD(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    char path[MAXPATHLEN];
    char abspath[MAXPATHLEN];
    path_join_jo(path, session->pwd, param);
    path_join(abspath, session->root, path);
    int ret = mkdir(abspath, S_IRWXU|S_IRWXG|S_IRWXO);
    if (ret == 0)
        sprintf(buff, "257 \"%s\" directory created.\r\n",param);
    else
        sprintf(buff, "550 %s: %s\r\n",param,strerror(errno));
    send(session->sockfd, buff, strlen(buff), 0);
}

void command_RMD(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    char path[MAXPATHLEN];
    char abspath[MAXPATHLEN];
    path_join_jo(path, session->pwd, param);
    path_join(abspath, session->root, path);
    int ret = rmdir(abspath);
    if (ret == 0)
        sprintf(buff, "250 RMD command successful.\r\n");
    else
        sprintf(buff, "550 %s: %s\r\n",param,strerror(errno));
    send(session->sockfd, buff, strlen(buff), 0);
}

void command_DELE(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    char path[MAXPATHLEN];
    char abspath[MAXPATHLEN];
    path_join_jo(path, session->pwd, param);
    path_join(abspath, session->root, path);
    int ret = unlink(abspath);
    if (ret == 0)
        sprintf(buff, "250 DELE command successful.\r\n");
    else
        sprintf(buff, "550 %s: %s\r\n",param,strerror(errno));
    send(session->sockfd, buff, strlen(buff), 0);
}

void command_RNFR(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    char path[MAXPATHLEN];
    char abspath[MAXPATHLEN];
    struct stat tmpstat;
    session->oldpath[0]='\0';
    path_join_jo(path, session->pwd, param);
    path_join(abspath, session->root, path);
    int ret = stat(abspath, &tmpstat);
    if (ret == 0)
    {
        sprintf(buff, "350 File exists, ready for destination name\r\n");
        strcpy(session->oldpath, abspath);
    }
    else
        sprintf(buff, "550 %s: %s\r\n",param,strerror(errno));
    send(session->sockfd, buff, strlen(buff), 0);
}

void command_RNTO(struct session_t *session,char *param)
{
    char buff[BUFSIZ];
    char path[MAXPATHLEN];
    char abspath[MAXPATHLEN];

    path_join_jo(path, session->pwd, param);
    path_join(abspath, session->root, path);
    if (session->oldpath[0]=='\0')
    {
        SendMsg(session->sockfd, "503 Bad sequence of commands.\r\n");
        return ;
    }
    int ret = rename(session->oldpath,abspath);
    if (ret == 0)
    {
        sprintf(buff, "250 RNTO command successful.\r\n");
        session->oldpath[0]='\0';
    }
    else
        sprintf(buff, "550 %s: %s\r\n",param,strerror(errno));
    send(session->sockfd, buff, strlen(buff), 0);
}


void command_UNKNOWN(struct session_t *session,char *param)
{
    SendMsg(session->sockfd,"500 Unknown command\r\n");
}

int telnet_get_verb_parm(int sock,char *verb,char *param)
{
    int code;
    size_t len = 0;
    char buff[BUFSIZ];
    verb[0]=param[0]='\0';
    while (len < BUFSIZ && (buff[len-2]!='\r' || buff[len-1] != '\n'))
    {
        size_t sz;
        while ( (sz = recv(sock,buff+len,1,0)) !=1 )
            if (sz == -1)
                return -1;

        if (buff[len]=='\\')
        {
            recv(sock,buff+len,3,0);
            buff[len+3]='\0';
            sscanf(buff+len,"%d",&code);
            if (code!=377)
            {
                buff[len] = (unsigned char)code;
                ++len;
            }
        }
        else
            ++len;
    }

    len -=2;
    buff[len] = '\0';
    printf("buff:%s\n",buff);
    char *spit = strchr(buff, ' ');
    
    if (spit == NULL)
        strcpy(verb, buff);
    else
    {
        strncpy(verb, buff,spit - buff);
        verb[spit - buff]='\0';
        while (*spit ==' ')
            ++spit;
        strcpy(param, spit);
    }
    return 0;
}

void* thread_ftp(void* arg)
{
    int sock = *(int*)arg;
    free((int*)arg);

    struct session_t session;
    bzero(&session, sizeof(struct session_t));
    session.sockfd = sock;

    char param[BUFSIZ],verb[BUFSIZ];
    
    _command_Welcome(&session,NULL);
    
    while (session.state>=0)
    {
        param[0]=verb[0]='\0';
        
        if (telnet_get_verb_parm(sock,verb,param) == -1)
            break;
        printf("V:%s P:%s\n",verb,param);
        
        if (strcmp(verb, "USER")==0)
            command_USER(&session,param);
        else if (strcmp(verb, "PASS")==0)
            command_PASS(&session,param);
        else if (strcmp(verb, "QUIT")==0||strcmp(verb, "ABOR")==0)
            command_QUIT(&session,NULL);
        else if (strcmp(verb,"SYST") == 0)
            command_SYST(&session,NULL);
        
        else if (session.isAuth == 1)
        {
            if (strcmp(verb,"TYPE")==0)
                command_TYPE(&session,param);
            else if (strcmp(verb,"PWD")==0)
                command_PWD(&session,param);
            else if (strcmp(verb,"CWD")==0)
                command_CWD(&session,param);
            else if (strcmp(verb,"CDUP")==0)
                command_CDUP(&session,NULL);
            else if (strcmp(verb, "PASV")==0)
                command_PASV(&session,NULL);
            else if (strcmp(verb, "PORT")==0)
                command_PORT(&session,param);
            else if (strcmp(verb, "RETR")==0)
                command_RETR(&session,param);
            else if (strcmp(verb, "STOR")==0)
                command_STOR(&session,param);
            else if (strcmp(verb, "LIST")==0)
                command_LIST(&session,param);
            else if (strcmp(verb, "MKD")==0||strcmp(verb, "XMKD")==0)
                command_MKD(&session,param);
            else if (strcmp(verb, "RMD")==0||strcmp(verb, "XRMD")==0)
                command_RMD(&session,param);
            else if (strcmp(verb, "RNFR")==0)
                command_RNFR(&session,param);
            else if (strcmp(verb, "RNTO")==0)
                command_RNTO(&session,param);
            else if (strcmp(verb, "DELE")==0)
                command_DELE(&session,param);
            else
                command_UNKNOWN(&session,NULL);
        }
        else
            _command_NotLogin(&session,NULL);
    }
    close(sock);
    return NULL;
}

static struct option longopts[] = {
    { "dir",       required_argument,      NULL,   'd'},
    { "user",      required_argument,      NULL,   'u'},
    { "password",  required_argument,      NULL,   'p'},
    { "bind",      required_argument,      NULL,   'b'},
    { "port",      required_argument,      NULL,   1},
    { "actport",   required_argument,      NULL,   2},
    { NULL,         0,                     NULL,   0 }
    
};

void usage()
{
    printf("mei xie.\n");
}

int main(int argc,char *argv[])
{
    int ch;
    struct sockaddr_in addr;
    while ( (ch=getopt_long(argc,argv,"d:u:p:b:",longopts,NULL)) != -1)
        switch (ch) {
            case 'd':
                CHECK(strlen(optarg) < MAXPATHLEN,"dir too long.\n");
                strcpy(_Root,optarg);
                break;
            case 'b':
                strcpy(_Ip, optarg);
                break;
            case 'u':
                CHECK(strlen(optarg) < USER_LEN,"user too long.\n");
                strcpy(_User,optarg);
                break;
            case 'p':
                CHECK(strlen(optarg) < USER_LEN,"password too long.\n");
                strcpy(_Pass,optarg);
                break;
            case 1:
                CHECK(sscanf(optarg,"%u",&_Port) ==1 && (_Port >> 16) == 0 ,"bind port invalid.\n");
                break;
            case 2:
                CHECK(sscanf(optarg,"%u",&_Active_port) ==1 && (_Active_port >> 16) == 0 ,"active bind port invalid.\n");
                break;
            default:
                usage();
                exit(0);
        }
    inet_pton(AF_INET,_Ip,&(addr.sin_addr));
    addr.sin_port = htons(_Port);
    addr.sin_family = AF_INET;
    sockfd = socket(PF_INET,SOCK_STREAM,0);
    
    printf("Start Ftp Server At %s : %u\n",_Ip,_Port);
    printf("user:%s password:%s\n",_User,_Pass);
    printf("workdir:%s\n",_Root);
    
    CHECK(sockfd, "socket failure.");
    CHECKRET(bind(sockfd,(struct sockaddr *) (&addr),sizeof(addr)),"bind");
    CHECKRET(listen(sockfd, 10),"listen");
    
    int *new_fd = malloc(sizeof(int));
    while ( (*new_fd = accept(sockfd, (struct sockaddr *) &client_addr, &addr_size) )!=-1 || errno == EINTR) {
        pthread_t tid;
        pthread_create(&tid,NULL,thread_ftp,new_fd);
        printf("new socket:%d",*new_fd );
        new_fd = malloc(sizeof(int));
    }

    printf("exit %d:%s\n",errno,strerror(errno));
    return 0;
}

