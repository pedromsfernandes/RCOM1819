#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <ctype.h>

#include "download.h"

int usage(char *argv[])
{
    printf("Usage: %s ftp://[<user>:<password>@]<host>/<url-path>\n", argv[0]);
    return 1;
}

int parseArgument(char *argument, info_t *info)
{
    char *sep;
    int index1 = 6, index2;

    if (strncmp("ftp://", argument, 6) != 0)
        return 1;

    if ((sep = strchr(argument + 6, ':')) != NULL)
    {
        int index;

        index = (int)(sep - argument);

        strncpy(info->user, argument + 6, index - 6);

        if ((sep = strchr(argument, '@')) == NULL)
            return 1;

        int new_index = (int)(sep - argument);
        index++;
        strncpy(info->pass, argument + index, new_index - index);

        index1 = ++new_index;
    }
    else if ((sep = strchr(argument, '@')) != NULL)
        return 1;

    if ((sep = strchr(argument + 6, '/')) == NULL)
        return 1;

    index2 = (int)(sep - argument);

    strncpy(info->serverName, argument + index1, index2 - index1);
    index2++;
    strncpy(info->filePath, argument + index2, strlen(argument) - index2);

    return 0;
}

char *getServerIp(info_t info)
{
    struct hostent *h;

    if ((h = gethostbyname(info.serverName)) == NULL)
    {
        herror("gethostbyname");
        exit(1);
    }

    return inet_ntoa(*((struct in_addr *)h->h_addr_list[0]));
}

int createSocketTCP(char *server_ip, int server_port)
{
    int socketFd;
    struct sockaddr_in server_addr;

    /*server address handling*/
    bzero((char *)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr =
        inet_addr(server_ip); /*32 bit Internet address network byte ordered*/
    server_addr.sin_port =
        htons(server_port); /*server TCP port must be network byte ordered */

    /*open an TCP socket*/
    if ((socketFd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("socket()");
        exit(0);
    }

    /*connect to the server*/
    if (connect(socketFd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
        0)
    {
        perror("connect()");
        exit(0);
    }

    return socketFd;
}

int readServerReply(int socketFd, char *reply)
{
    char c;
    int res = 0, i = 0;
    state_t state = READ_CODE;

    while (state != END)
    {
        if ((res = read(socketFd, &c, 1)) <= 0)
            continue;

        switch (state)
        {
        case READ_CODE:
            if (c == ' ')
            {
                state = READ_LINE;
                i = 0;
            }
            else if (c == '-')
            {
                state = READ_MULT_LINE;
                i = 0;
            }
            else if (isdigit(c))
            {
                reply[i++] = c;
            }
            break;
        case READ_LINE:
            if (c == '\n')
                state = END;
            break;
        case READ_MULT_LINE:
            if (c == reply[i])
            {
                i++;
            }
            else if (i == 3 && c == ' ')
            {
                state = READ_LINE;
            }
            break;
        case END:
            break;
        default:
            break;
        }
    }

    return 0;
}

int getServerPort(int socketFd)
{
    int res = 0;
    state_t state = WAIT_FOR_PORT;
    char c;
    char first_byte[4], second_byte[4];
    int numCommas = 0, i = 0;

    while (state != END)
    {
        if ((res = read(socketFd, &c, 1)) <= 0)
            continue;

        switch (state)
        {
        case WAIT_FOR_PORT:
            if (c == ',')
                numCommas++;

            if (numCommas == 4)
                state = FIRST_PORT_BYTE;
            break;
        case FIRST_PORT_BYTE:
            if (c == ',')
            {
                state = SECOND_PORT_BYTE;
                i = 0;
            }
            else
            {
                first_byte[i++] = c;
            }
            break;
        case SECOND_PORT_BYTE:
            if (c == ')')
                state = END;
            else
            {
                second_byte[i++] = c;
            }
            break;
        case END:
            break;
        default:
            break;
        }
    }

    return atoi(first_byte) * 256 + atoi(second_byte);
}

int sendCommand(int socketFd, char *command, char *argument)
{
    int res = 0;
    char reply[REPLY_CODE_SIZE];
    reply_type_t type;

    write(socketFd, command, strlen(command));
    if (argument != NULL)
        write(socketFd, argument, strlen(argument));
    write(socketFd, "\n", 1);

    while (true)
    {
        if ((res = readServerReply(socketFd, reply)) != 0)
            return 1;

        type = reply[0] - '0';

        switch (type)
        {
        case POSITIVE_PRE:
            break;
        case POSITIVE_INT:
            return 0;
        case POSITIVE_COMP:
            return 1;
        case TRANS_NEGATIVE_COMP:
            write(socketFd, command, strlen(command));
            if (argument != NULL)
                write(socketFd, argument, strlen(argument));
            write(socketFd, "\n", 1);
            break;
        case PERM_NEGATIVE_COMP:
            close(socketFd);
            return -1;
        default:
            break;
        }
    }
}

void createFile(int fd, char *filename)
{
    FILE *file = fopen(filename, "wb+");

    char fileData[SOCKET_BUF_SIZE];
    int nbytes;
    while ((nbytes = read(fd, fileData, SOCKET_BUF_SIZE)) > 0)
    {
        nbytes = fwrite(fileData, nbytes, 1, file);
    }

    fclose(file);
}

int main(int argc, char *argv[])
{
    info_t info;
    char *server_ip;
    char reply[MAX_REPLY_SIZE];
    int fd1, fd2, res, port;

    if (argc != 2 || parseArgument(argv[1], &info) != 0)
        return usage(argv);

    server_ip = getServerIp(info);

    fd1 = createSocketTCP(server_ip, SERVER_PORT);
    readServerReply(fd1, reply);

    if (reply[0] == '2')
        printf("Connection established!\n");

    res = sendCommand(fd1, "user ", info.user);
    debug_print("aqui\n");
    if (res == 1)
    {
        res = sendCommand(fd1, "pass ", info.pass);
    }
    debug_print("aqui\n");

    res = sendCommand(fd1, "pasv", NULL);

    port = getServerPort(fd1);
    debug_print("aqui\n");

    fd2 = createSocketTCP(server_ip, port);
    readServerReply(fd2, reply);

    res = sendCommand(fd1, "retr ", info.filePath);

    if (res == 0)
    {
        createFile(fd2, info.filePath);
    }

    close(fd1);
    close(fd2);

    return 0;
}