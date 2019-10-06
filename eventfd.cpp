#ifdef _WIN32
#include <sys/eventfd.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <pthread.h>
#include <vector>
#include <memory>
#include <thread>

#define EVENTFD_INNTER_PORT 65533
#define EVENTFD_INNTER_ADDR "127.0.0.1"

#define switchthread()				\
	do {							\
		std::this_thread::yield();	\
	} while (0);


#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#define getcwd _getcwd
typedef int64_t ssize_t;

#define WAITMS(ms)				\
	do {						\
		switchthread();			\
		if(ms <= 0 ) break;		\
		Sleep((DWORD)ms);		\
	} while (0);


typedef struct __eventfd__
{
	SOCKET  read_sock;
	SOCKET  write_sock;
	eventfd_t count;
	pthread_mutex_t mutex;
	int32_t	flags;
	
	__eventfd__( eventfd_t cnt, int32_t flag )
		: read_sock(0)
		, write_sock(0)
		, count(cnt)
		, mutex(PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP)
		, flags(flag)
	{}
	
	virtual ~__eventfd__()
	{
		if( read_sock != 0)
		{
			shutdown(read_sock, SD_BOTH);
			closesocket(read_sock);
		}
		if( write_sock != 0)
		{
			shutdown(write_sock, SD_BOTH);
			closesocket(write_sock);
		}
	}
} __eventfd;

static eventfd_t htond(const eventfd_t* x)
{
	eventfd_t ret = 0;
	int* ret_overlay = (int *)&ret;
	int* data_overlay = (int *)x;

	ret_overlay[0] = htonl(data_overlay[1]);
	ret_overlay[1] = htonl(data_overlay[0]);
	return ret;
}

static eventfd_t ntohd(const eventfd_t* x)
{
	eventfd_t ret = 0;
	int* ret_overlay = (int *)&ret;
	int* data_overlay = (int *)x;

	ret_overlay[0] = ntohl(data_overlay[1]);
	ret_overlay[1] = ntohl(data_overlay[0]);
	return ret;
}

typedef std::shared_ptr<__eventfd> EventFDPtr;


class eventFD_Server
{
public:
	eventFD_Server();
	virtual ~eventFD_Server();

	int start();
	int stop();


	SOCKET add_eventfd(eventfd_t cnt, int32_t flag);

	EventFDPtr get_eventfd(SOCKET sock);
	int remove_eventfd(SOCKET sock);


private:
	std::vector<EventFDPtr> EventFDs;
	pthread_mutex_t initLock;
	pthread_mutex_t EventFDsLock;
	pthread_t server_thread;
	WSADATA wsaData;
	SOCKET  listen_sock;
	bool serverinit;
	bool runthread;
};

eventFD_Server::eventFD_Server()
	: EventFDs()
	, initLock(PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP)
	, EventFDsLock(PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP)
	, server_thread()
	, wsaData()
	, listen_sock()
	, serverinit(false)
	, runthread(false)
{
}

eventFD_Server::~eventFD_Server()
{
	stop();
}


int eventFD_Server::start()
{
	pthread_mutex_lock(&initLock);

	WSAStartup(MAKEWORD(2, 0), &wsaData);

	listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_sock == INVALID_SOCKET)
	{
		errno = WSAGetLastError();
		return 0;
	}


	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(EVENTFD_INNTER_PORT);
	addr.sin_addr.S_un.S_addr = INADDR_ANY;


	BOOL yes = 1;
	setsockopt(listen_sock,
		SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

	if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
	{
		printf("bind : %d\n", WSAGetLastError());
		return 0;
	}

	if (listen(listen_sock, 10) != 0)
	{
		printf("listen : %d\n", WSAGetLastError());
		return 0;
	}


	fd_set fds, readfds;
	FD_ZERO(&readfds);
	FD_SET(listen_sock, &readfds);
	SOCKET sock;

	struct timeval timeout;
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;

	serverinit = true;
	runthread = true;

	pthread_mutex_unlock(&initLock);

	while (runthread == true)
	{
		memcpy(&fds, &readfds, sizeof(fd_set));
		
		int ret = select(0, &fds, NULL, NULL, &timeout);
		if (ret <= 0)
		{
			continue;
		}

		struct sockaddr_in client;
		int len = sizeof(client);

		if (FD_ISSET(listen_sock, &fds))
		{
			sock = accept(listen_sock, (struct sockaddr *)&client, &len);
			if (sock == INVALID_SOCKET)
			{
				printf("accept : %d\n", WSAGetLastError());
				break;
			}

			EventFDPtr efd = get_eventfd(0);
			if (efd.get() != nullptr)
			{
				if (efd->flags & EFD_NONBLOCK)
				{
					DWORD  dwNonBlocking = 1;
					ioctlsocket(sock, FIONBIO, &dwNonBlocking);
				}

				efd->read_sock = sock;
			}
		}
	}

	return 0;
}

int eventFD_Server::stop()
{
	if (runthread != true)
	{
		return 0;
	}

	pthread_mutex_lock(&initLock);

	runthread = false;
	pthread_join(server_thread, NULL);
	serverinit = false;

	pthread_mutex_unlock(&initLock);

	return 0;
}

EventFDPtr eventFD_Server::get_eventfd(SOCKET sock)
{
	EventFDPtr efd;
	pthread_mutex_lock(&EventFDsLock);

	auto itr = EventFDs.begin();
	while (itr != EventFDs.end())
	{
		efd = *itr;
		if (efd->read_sock == sock)
		{
			break;
		}
		else
		{
			itr++;
		}
	}

	pthread_mutex_unlock(&EventFDsLock);
	return efd;
}

int eventFD_Server::remove_eventfd(SOCKET sock)
{
	int ret = -1;
	EventFDPtr efd;
	pthread_mutex_lock(&EventFDsLock);

	auto itr = EventFDs.begin();
	while (itr != EventFDs.end())
	{
		efd = *itr;
		if (efd->read_sock == sock)
		{
			itr = EventFDs.erase(itr);
			ret = 0;
			break;
		}
		else
		{
			itr++;
		}
	}
	pthread_mutex_unlock(&EventFDsLock);

	if (EventFDs.size() <= 0)
	{
		stop();
	}

	return ret;
}


static void* startEventFdDummyServer(void * p)
{
	eventFD_Server*	server = (eventFD_Server*)p;

	server->start();

	return NULL;
}

SOCKET eventFD_Server::add_eventfd(eventfd_t cnt, int32_t flag)
{
	pthread_mutex_lock(&initLock);
	bool init = serverinit;
	pthread_mutex_unlock(&initLock);

	if (init != true)
	{
		pthread_create(&server_thread, NULL, startEventFdDummyServer,this);
	}

	SOCKET  sock;
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == INVALID_SOCKET) 
	{
		printf("socket : %d\n", WSAGetLastError());
		return -1;
	}
	
	struct sockaddr_in server;
	server.sin_family = AF_INET;
	server.sin_port = htons(EVENTFD_INNTER_PORT);
	InetPton(
		AF_INET,
		EVENTFD_INNTER_ADDR,
		&server.sin_addr.S_un.S_addr);
	
	
	EventFDPtr efd( new __eventfd(cnt, flag) );
	efd->write_sock = sock;

	pthread_mutex_lock( &EventFDsLock );
	EventFDs.push_back( efd );
	pthread_mutex_unlock( &EventFDsLock );
	
	if( connect(sock,
	        (struct sockaddr *)&server,
	        sizeof(server)) != 0)
	{
		 printf("connect : %d\n", WSAGetLastError());
		 return -1;
	}

	if (efd->flags & EFD_NONBLOCK)
	{
		DWORD  dwNonBlocking = 1;
		ioctlsocket(sock, FIONBIO, &dwNonBlocking);
	}

	for(int i=0; i<100; i++)
	{
		pthread_mutex_lock( &efd->mutex );
		SOCKET retsock = efd->read_sock;
		pthread_mutex_unlock( &efd->mutex );

		if( retsock != 0 )
		{
			return efd->read_sock;
		}
		
		WAITMS(10);
	}
	
	return -1;
}


eventFD_Server eventFDServer;


#ifdef __cplusplus
extern "C" {
#endif

/* Return file descriptor for generic event channel.  Set initial
   value to COUNT.  */
event_fd_t eventfd (int __count, int __flags)
{
	return eventFDServer.add_eventfd(__count, __flags);
}

/* Read event counter and possibly wait for events.  */
int eventfd_read (event_fd_t __fd, eventfd_t *__value)
{
	char recvbuf[128] = { 0 };
	EventFDPtr efd = eventFDServer.get_eventfd(__fd);
	
	if( efd.get() == nullptr )
	{
		return -1;
	}

	if (__value == NULL)
	{
		return -1;
	}

	pthread_mutex_lock(&efd->mutex);

	int ret = recv(efd->read_sock, recvbuf, sizeof(recvbuf), 0);
	if (ret > 0)
	{
//		eventfd_t count = ntohd((eventfd_t*)recvbuf);
		*__value = efd->count;
		efd->count = 0;
	}
	pthread_mutex_unlock(&efd->mutex);

	if( ret < 0 )
	{
		errno = WSAGetLastError();
		return -1;
	}
		
	return 0;
}

/* Increment event counter.  */
int eventfd_write (event_fd_t __fd, eventfd_t __value)
{
	eventfd_t sendbuf = htond(&__value);

	EventFDPtr efd = eventFDServer.get_eventfd(__fd);
	
	if( efd.get() == nullptr )
	{
		return -1;
	}

	pthread_mutex_lock(&efd->mutex);

	efd->count += __value;
	int64_t ret = send(efd->write_sock, (const char*)&sendbuf, sizeof(sendbuf), 0);

	pthread_mutex_unlock(&efd->mutex);

	if( ret < 0 )
	{
		errno = WSAGetLastError();
		return -1;
	}
	else
	{
		return 0;
	}	
}

int eventfd_close(event_fd_t __fd)
{

	int ret = eventFDServer.remove_eventfd(__fd);

	return ret;
}
#ifdef __cplusplus
}
#endif

#endif