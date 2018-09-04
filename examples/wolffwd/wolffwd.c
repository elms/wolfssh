#include <stdio.h>
#include <termios.h>
#include <errno.h>
#include <sys/select.h>
#include <wolfssh/ssh.h>
#include <wolfssh/test.h>
#include <wolfssh/port.h>
#include "examples/wolffwd/wolffwd.h"


/* The wolffwd tool will be a client or server in the port forwarding
 * interaction.
 *
 * The wolffwd client will connect to an SSH server and request a tunnel.
 * The client acts like a server for the local user application. It forwards
 * the packets received to the SSH server who will then forward the packet
 * to its local application server.
 *
 * The wolffwd server will listen for SSH connections, and when it receives
 * one will only accept forward requests from the connection. All data for
 * the forwarding channel are sent to the local application server and
 * data from the server is forwarded to the client.
 */


static inline int max(int a, int b)
{
    return (a > b) ? a : b;
}

static void ShowUsage(void)
{
    printf("wolffwd %s\n"
           " -?            display this help and exit\n"
           " -h <host>     host to connect to, default %s\n"
           " -p <num>      port to connect on, default %u\n"
           " -u <username> username to authenticate as (REQUIRED)\n"
           " -P <password> password for username, prompted if omitted\n"
           " -F <host>     host to forward from, default 0.0.0.0\n"
           " -f <num>      host port to forward from (REQUIRED)\n"
           " -T <host>     host to forward to, default to host\n"
           " -t <num>      port to forward to (REQUIRED)\n"
           " -d            server mode\n",
           LIBWOLFSSH_VERSION_STRING, wolfSshIp, wolfSshPort);
}


static int SetEcho(int on)
{
#ifndef USE_WINDOWS_API
    static int echoInit = 0;
    static struct termios originalTerm;
    if (!echoInit) {
        if (tcgetattr(STDIN_FILENO, &originalTerm) != 0) {
            printf("Couldn't get the original terminal settings.\n");
            return -1;
        }
        echoInit = 1;
    }
    if (on) {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &originalTerm) != 0) {
            printf("Couldn't restore the terminal settings.\n");
            return -1;
        }
    }
    else {
        struct termios newTerm;
        memcpy(&newTerm, &originalTerm, sizeof(struct termios));

        newTerm.c_lflag &= ~ECHO;
        newTerm.c_lflag |= (ICANON | ECHONL);

        if (tcsetattr(STDIN_FILENO, TCSANOW, &newTerm) != 0) {
            printf("Couldn't turn off echo.\n");
            return -1;
        }
    }
#else
    static int echoInit = 0;
    static DWORD originalTerm;
    HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
    if (!echoInit) {
        if (GetConsoleMode(stdinHandle, &originalTerm) == 0) {
            printf("Couldn't get the original terminal settings.\n");
            return -1;
        }
        echoInit = 1;
    }
    if (on) {
        if (SetConsoleMode(stdinHandle, originalTerm) == 0) {
            printf("Couldn't restore the terminal settings.\n");
            return -1;
        }
    }
    else {
        DWORD newTerm = originalTerm;

        newTerm &= ~ENABLE_ECHO_INPUT;

        if (SetConsoleMode(stdinHandle, newTerm) == 0) {
            printf("Couldn't turn off echo.\n");
            return -1;
        }
    }
#endif

    return 0;
}


byte userPassword[256];


static int wsUserAuth(byte authType,
                      WS_UserAuthData* authData,
                      void* ctx)
{
    const char* defaultPassword = (const char*)ctx;
    word32 passwordSz;
    int ret = WOLFSSH_USERAUTH_SUCCESS;

    (void)authType;
    if (defaultPassword != NULL) {
        passwordSz = (word32)strlen(defaultPassword);
        memcpy(userPassword, defaultPassword, passwordSz);
    }
    else {
        printf("Password: ");
        SetEcho(0);
        if (WFGETS((char*)userPassword, sizeof(userPassword), stdin) == NULL) {
            printf("Getting password failed.\n");
            ret = WOLFSSH_USERAUTH_FAILURE;
        }
        else {
            char* c = strpbrk((char*)userPassword, "\r\n");;
            if (c != NULL)
                *c = '\0';
            passwordSz = (word32)strlen((const char*)userPassword);
        }
        SetEcho(1);
#ifdef USE_WINDOWS_API
        printf("\r\n");
#endif
    }

    if (ret == WOLFSSH_USERAUTH_SUCCESS) {
        authData->sf.password.password = userPassword;
        authData->sf.password.passwordSz = passwordSz;
    }

    return ret;
}


#define INVALID_FWD_PORT 0
static const char defaultFwdFromHost[] = "0.0.0.0";

/*
 * fwdFromHost - address to bind the local listener socket to (default: any)
 * fwdFromHostPort - port number to bind the local listener socket to
 * fwdToHost - address to tell the remote peer to connect to on behalf of the
 *      client (actual server address)
 * fwdToHostPort - port number to tell the remote peer to connect to on behalf
 *      of the client (actual server port)
 * host - peer SSH server address to connect to
 * hostPort - peer SSH server port number to connect to
 */

THREAD_RETURN WOLFSSH_THREAD wolffwd_worker(void* args)
{
    WOLFSSH* ssh;
    WOLFSSH_CTX* ctx;
    char* host = (char*)wolfSshIp;
    word16 port = wolfSshPort;
    word16 fwdFromPort = INVALID_FWD_PORT;
    word16 fwdToPort = INVALID_FWD_PORT;
    const char* fwdFromHost = defaultFwdFromHost;
    const char* fwdToHost = NULL;
    const char* username = NULL;
    const char* password = NULL;
    SOCKADDR_IN_T hostAddr;
    socklen_t hostAddrSz = sizeof(hostAddr);
    SOCKET_T sshFd;
    SOCKADDR_IN_T fwdFromHostAddr;
    socklen_t fwdFromHostAddrSz = sizeof(fwdFromHostAddr);
    SOCKET_T listenFd;
    SOCKET_T appFd = -1;
    int argc = ((func_args*)args)->argc;
    char** argv = ((func_args*)args)->argv;
    int serverMode = 0;
    fd_set templateFds;
    fd_set rxFds;
    fd_set errFds;
    int nFds;
    int ret;
    char ch;
    int done = 0;
    int appFdSet = 0;
    struct timeval to;
    WOLFSSH_CHANNEL* fwdChannel = NULL;
    byte buffer[4096];
    word32 bufferSz = sizeof(buffer);
    word32 bufferUsed = 0;

    ((func_args*)args)->return_code = 0;

    while ((ch = mygetopt(argc, argv, "?df:h:p:t:u:F:P:T:")) != -1) {
        switch (ch) {
            case 'd':
                serverMode = 1;
                break;

            case 'h':
                host = myoptarg;
                break;

            case 'f':
                fwdFromPort = (word16)atoi(myoptarg);
                break;

            case 'p':
                port = (word16)atoi(myoptarg);
                #if !defined(NO_MAIN_DRIVER) || defined(USE_WINDOWS_API)
                    if (port == 0)
                        err_sys("port number cannot be 0");
                #endif
                break;

            case 't':
                fwdToPort = (word16)atoi(myoptarg);
                break;

            case 'u':
                username = myoptarg;
                break;

            case 'F':
                fwdFromHost = myoptarg;
                break;

            case 'P':
                password = myoptarg;
                break;

            case 'T':
                fwdToHost = myoptarg;
                break;

            case '?':
                ShowUsage();
                exit(EXIT_SUCCESS);

            default:
                ShowUsage();
                exit(MY_EX_USAGE);
        }
    }
    myoptind = 0;

    if (username == NULL)
        err_sys("client requires a username parameter.");
    if (fwdToPort == INVALID_FWD_PORT)
        err_sys("requires a port to forward to");
    if (fwdFromPort == INVALID_FWD_PORT)
        err_sys("requires a port to forward from");

    if (fwdToHost == NULL)
        fwdToHost = host;

    printf("wolffwd options\n"
           " * ssh host: %s:%u\n"
           " * username: %s\n"
           " * password: %s\n"
           " * forward from: %s:%u\n"
           " * forward to: %s:%u\n"
           " * server mode: %s\n",
           host, port, username, password,
           fwdFromHost, fwdFromPort,
           fwdToHost, fwdToPort,
           serverMode ? "yes" : "no");

    ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
    if (ctx == NULL)
        err_sys("couldn't create the ssh client");

    if (((func_args*)args)->user_auth == NULL)
        wolfSSH_SetUserAuth(ctx, wsUserAuth);
    else
        wolfSSH_SetUserAuth(ctx, ((func_args*)args)->user_auth);

    ssh = wolfSSH_new(ctx);
    if (ssh == NULL)
        err_sys("Couldn't create wolfSSH session.");

    if (password != NULL)
        wolfSSH_SetUserAuthCtx(ssh, (void*)password);

    ret = wolfSSH_SetUsername(ssh, username);
    if (ret != WS_SUCCESS)
        err_sys("Couldn't set the username.");

    build_addr(&hostAddr, host, port);
    build_addr(&fwdFromHostAddr, fwdFromHost, fwdFromPort);

    tcp_socket(&sshFd); /* Socket to SSH peer. */
    tcp_socket(&listenFd); /* Either receive from client application or connect
                              to server application. */
    tcp_listen(&listenFd, &fwdFromPort, 1);

    printf("Connecting to the SSH server...\n");
    ret = connect(sshFd, (const struct sockaddr *)&hostAddr, hostAddrSz);
    if (ret != 0)
        err_sys("Couldn't connect to server.");

    ret = wolfSSH_set_fd(ssh, (int)sshFd);
    if (ret != WS_SUCCESS)
        err_sys("Couldn't set the session's socket.");

    ret = wolfSSH_connect(ssh);
    if (ret != WS_SUCCESS)
        err_sys("Couldn't connect SFTP");

    FD_ZERO(&templateFds);
    FD_SET(sshFd, &templateFds);
    FD_SET(listenFd, &templateFds);
    nFds = max(sshFd, listenFd) + 1;

    printf("Entering the run loop...\n");
    while (!done) {
        rxFds = templateFds;
        to.tv_sec = 1;
        to.tv_usec = 0;
        ret = select(nFds, &rxFds, NULL, &errFds, &to);
        if (ret == 0) {
            printf("select timed out\n");
            ret = wolfSSH_SendIgnore(ssh, NULL, 0);
            if (ret != WS_SUCCESS)
                printf("Couldn't send an ignore message.\n");
            continue;
        }
        else if (ret < 0) {
            printf("select failed\n");
            break;
        }
        else
            printf("select returned something: %d\n", ret);

        if (appFdSet && FD_ISSET(appFd, &rxFds)) {
            int rxd;
            printf("application packet received\n");
            rxd = (int)recv(appFd, buffer + bufferUsed, bufferSz - bufferUsed, 0);
            if (rxd > 0)
                bufferUsed += rxd;
            else
                break;
        }
        if (FD_ISSET(sshFd, &rxFds)) {
            word32 channelId = 0;

            ret = wolfSSH_worker(ssh, &channelId);
            if (ret == WS_CHAN_RXD) {
                WOLFSSH_CHANNEL* readChannel;

                bufferSz = sizeof(buffer);
                readChannel = wolfSSH_ChannelFind(ssh,
                        channelId, WS_CHANNEL_ID_SELF);
                ret = (readChannel == NULL) ? WS_INVALID_CHANID : WS_SUCCESS;

                if (ret == WS_SUCCESS)
                    ret = wolfSSH_ChannelRead(readChannel, buffer, bufferSz);
                if (ret > 0) {
                    bufferSz = (word32)ret;
                    if (appFd != -1) {
                        ret = (int)send(appFd, buffer, bufferSz, 0);
                        if (ret != (int)bufferSz)
                            break;
                    }
                }
            }
        }
        if (!appFdSet && FD_ISSET(listenFd, &rxFds)) {
            appFd = accept(listenFd,
                    (struct sockaddr*)&fwdFromHostAddr, &fwdFromHostAddrSz);
            FD_SET(appFd, &templateFds);
            nFds = appFd + 1;
            appFdSet = 1;
            fwdChannel = wolfSSH_ChannelFwdNew(ssh, fwdToHost, fwdToPort,
                    fwdFromHost, fwdFromPort);
        }
        if (bufferUsed > 0) {
            ret = wolfSSH_ChannelSend(fwdChannel, buffer, bufferUsed);
            if (ret > 0)
                bufferUsed -= ret;
        }
    }

    ret = wolfSSH_shutdown(ssh);
    if (ret != WS_SUCCESS)
        err_sys("Closing stream failed.");

    WCLOSESOCKET(sshFd);
    WCLOSESOCKET(listenFd);
    WCLOSESOCKET(appFd);
    wolfSSH_free(ssh);
    wolfSSH_CTX_free(ctx);

    return 0;
}


#ifndef NO_MAIN_DRIVER

int main(int argc, char** argv)
{
    func_args args;

    args.argc = argc;
    args.argv = argv;
    args.return_code = 0;
    args.user_auth = NULL;

    WSTARTTCP();

    #ifdef DEBUG_WOLFSSH
        wolfSSH_Debugging_ON();
    #endif

    wolfSSH_Init();

    ChangeToWolfSshRoot();
    wolffwd_worker(&args);

    wolfSSH_Cleanup();

    return args.return_code;
}

int myoptind = 0;
char* myoptarg = NULL;


#endif /* NO_MAIN_DRIVER */
