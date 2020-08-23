// Winsock tutorial
// https://docs.microsoft.com/en-us/windows/win32/winsock/finished-server-and-client-code

// This grabs IP address, it will be needed
// for master servers to know how to redirect
// clients to dedicated servers, by getting the 
// IPs of the dedicated servers. It will not be
// used to find the IPs of clients
#if 0
inline std::string getAddress(sockaddr_in* sin)
{
	std::string res = std::to_string(sin->sin_addr.S_un.S_un_b.s_b1) + '.' + std::to_string(sin->sin_addr.S_un.S_un_b.s_b2) + '.' + std::to_string(sin->sin_addr.S_un.S_un_b.s_b3) + '.' + std::to_string(sin->sin_addr.S_un.S_un_b.s_b4);
	return res;
}

void acceptTCP(SOCKET& origSock)
{
	SOCKET tempSock = SOCKET_ERROR;
	struct sockaddr* sa = new sockaddr();
	int size = sizeof(*sa);
	while (tempSock == SOCKET_ERROR)
	{
		tempSock = accept(origSock, sa, &size);
		int err = WSAGetLastError();
		if (err != 0 && err != WSAEWOULDBLOCK) std::cout << "\r\n" << err;
	}
	struct sockaddr_in* sin = (struct sockaddr_in*)sa;
	std::cout << "\r\nConnected to " << getAddress(sin) << ":" << htons(sin->sin_port);
	origSock = tempSock;
}
#endif

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>

#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Mswsock.lib")
#pragma comment (lib, "AdvApi32.lib")

#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_DEBUG 0

// one short, for controller input
const int Type3_Size = 7;

struct Message
{
	// [server -> client]
	// 0 for track index
	//		track, driver index, num characters, characters
	// 1 for lap index
	//		row index
	// 2 for Start Loading
	//		[null]

	// [bidirectional]
	// 3 for controller input
	//		2 bytes of buttons

	// [client -> server]
	// 4 for kart IDs
	//		client character

	// [bidirectional]
	// 5 for start-line sync
	//		[null]

	unsigned char type;
	unsigned char size;

	// Server and client MUST match
	char data[Type3_Size * 3];
};

struct SocketCtr
{
	SOCKET socket;
	Message sendBuf;
	Message sendBufPrev;
	Message recvBuf;
	Message recvBufPrev;
};

SocketCtr CtrMain;
int receivedByteCount = 0;
bool inGame = false;
short characterIDs[8];
#define MAX_CLIENTS 4

unsigned char clientCount = 0;
SocketCtr CtrClient[MAX_CLIENTS];

struct NetworkGate
{
	bool clientsHere[MAX_CLIENTS];
	bool anyoneHere()
	{
		for (int i = 0; i < clientCount; i++)
			if (clientsHere[i]) 
				return true;

		return false;
	}
	bool allHere()
	{
		for (int i = 0; i < clientCount; i++)
			if (!clientsHere[i]) 
				return false;

		return true;
	}
	void reset()
	{
		memset(&clientsHere[0], 0, sizeof(bool)*MAX_CLIENTS);
	}
};

NetworkGate trackSel;
NetworkGate startLine;

void BuildListeningSocket()
{
	// TCP, port 1234, from any address
	struct sockaddr_in socketIn;
	socketIn.sin_family = AF_INET;
	socketIn.sin_port = htons(1234);
	socketIn.sin_addr.S_un.S_addr = INADDR_ANY;

	// Create a SOCKET for connecting to server
	CtrMain.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	// Setup the TCP listening socket
	bind(CtrMain.socket, (struct sockaddr*)&socketIn, sizeof(socketIn));

	// make this socket listen for new connections
	listen(CtrMain.socket, SOMAXCONN);
}

fd_set master;

void ResetClients()
{
	// close sockets, if open
	for (int i = 0; i < clientCount; i++)
	{
		closesocket(CtrClient[i].socket);
		FD_CLR(CtrClient[i].socket, &master);
	}

	// set all connections to INVALID_SOCKET
	memset(&CtrClient[0], 0xFF, sizeof(CtrClient[0]) * MAX_CLIENTS);

	// reset server
	clientCount = 0;
	inGame = false;
	startLine.reset();
	trackSel.reset();

	printf("\nClientCount: 0\n");
}

void initialize()
{
	int choice = 0;
	HWND console = GetConsoleWindow();
	RECT r;
	GetWindowRect(console, &r); //stores the console's current dimensions

	const int winW = TEST_DEBUG ? 800 : 400;

	// 300 + height of bar (35)
	MoveWindow(console, r.left, r.top, winW, 240+35, TRUE);

	WSADATA wsaData;
	int iResult;

	// Initialize Winsock
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf("WSAStartup failed with error: %d\n", iResult);
		system("pause");
	}

	BuildListeningSocket();

	printf("NodeServer ready on port 1234\n\n");

	// set LISTENING socket to non-blocking
	unsigned long nonBlocking = 1;
	iResult = ioctlsocket(CtrMain.socket, FIONBIO, &nonBlocking);

	FD_ZERO(&master);
	FD_SET(CtrMain.socket, &master);

	ResetClients();
}

void CheckForNewClients()
{
	fd_set copy = master;

	// See who's talking to us
	int socketCount = select(0, &copy, nullptr, nullptr, nullptr);

	// Loop through all the current connections / potential connect
	for (int i = 0; i < socketCount; i++)
	{
		// Makes things easy for us doing this assignment
		SOCKET sock = copy.fd_array[i];

		// Is it an inbound communication?
		if (sock == CtrMain.socket)
		{
			// Accept a new connection
			SOCKET client = accept(CtrMain.socket, nullptr, nullptr);

			if (clientCount == MAX_CLIENTS)
			{
				closesocket(client);
				FD_CLR(CtrClient[i].socket, &master);
				return;
			}

			// Add the new connection to the list of connected clients
			FD_SET(client, &master);

			// This is a bad idea but I'll fix it later
			CtrClient[clientCount].socket = client;


			// set socket to non-blocking
			unsigned long nonBlocking = 1;
			ioctlsocket(client, FIONBIO, &nonBlocking);

			// Let the first client know that they are the host
			if (clientCount == 0)
			{
				CtrClient[0].sendBuf.type = 0;
				CtrClient[0].sendBuf.size = 2;

				// send a message to the client
				send(CtrClient[0].socket, (char*)&CtrClient[0].sendBuf, sizeof(Message), 0);
			}

			clientCount++;
			printf("ClientCount: %d\n", clientCount);
		}
	}
}

// globals
unsigned char type = 0xFF;
unsigned char size = 0xFF;

void RecvTrackMessage(int sender)
{
	unsigned char trackID = CtrClient[sender].recvBuf.data[0];
	characterIDs[0] =		CtrClient[sender].recvBuf.data[1];
	char lapRowSelected = CtrClient[sender].recvBuf.data[2] & 0b11;
	char startRace = (CtrClient[sender].recvBuf.data[2] >> 2) & 1;
	char lapRowOpen = (CtrClient[sender].recvBuf.data[2] >> 3) & 1;

#if TEST_DEBUG
	printf("Recv -- Tag: %d, size: %d, -- %d %d\n", type, size,
		trackID,
		characterIDs[0]);
#endif

	// send to all clients (host too), since it includes characters
	for (int i = 0; i < clientCount; i++)
	{
		// track, driver index, num characters, characters

		CtrClient[i].sendBuf.type = 0;
		CtrClient[i].sendBuf.size = 6;

		CtrClient[i].sendBuf.data[0] = 0;
		CtrClient[i].sendBuf.data[1] = 0;

		int j = 0;
		int k = 0;
		for (; j < clientCount; j++)
		{
			if (i == j)
				continue;

			CtrClient[i].sendBuf.data[k / 2] += (char)characterIDs[j] << (4 * (k%2));
			k++;
		}

		CtrClient[i].sendBuf.data[1] += lapRowSelected << 4;
		CtrClient[i].sendBuf.data[1] += 1 << 6; //come back to this
		CtrClient[i].sendBuf.data[1] += startRace << 7;

		CtrClient[i].sendBuf.data[2] = trackID;
		CtrClient[i].sendBuf.data[2] += lapRowOpen << 5;
		CtrClient[i].sendBuf.data[2] += i << 6;

		CtrClient[i].sendBuf.data[3] = clientCount;
	}
}

void RecvPosMessage(int sender)
{
#if TEST_DEBUG
	printf("Recv -- Tag: %d, size: %d, -- %04X from %d\n", type, size,
		*(short*)&CtrClient[sender].recvBuf.data[0],
		sender);
#endif
}

void RecvCharacterMesssage(int sender)
{
	// Get characterID for this player
	// for characters 0 - 7:
	// CharacterID[i] : 0x1608EA4 + 2 * i
	characterIDs[sender] = (short)CtrClient[sender].recvBuf.data[0];

#if TEST_DEBUG
	printf("Recv -- Tag: %d, size: %d, -- %d from %d\n", type, size,
		CtrClient[sender].recvBuf.data[0],
		sender);
#endif
}

void RecvStartRaceMessage(int sender)
{
	// if all clients send a 5 message,
	// then stop waiting and start race
	startLine.clientsHere[sender] = true;

#if TEST_DEBUG
	printf("Recv -- Tag: %d, size: %d, from %d\n", type, size, sender);
#endif
}

void RecvReturnToMenuMessage(int sender)
{
	// if all clients send a 5 message,
	// then stop waiting and start race
	startLine.clientsHere[sender] = false;

#if TEST_DEBUG
	printf("Recv -- Tag: %d, size: %d\n", type, size);
#endif
}

void (*RecvMessage[7])(int sender) =
{
	// These will only be received by the host
	RecvTrackMessage,
	nullptr,
	nullptr,

	// These are gotten by all
	RecvPosMessage,
	RecvCharacterMesssage,
	RecvStartRaceMessage,
	RecvReturnToMenuMessage,
};

void HandleClient(int i)
{
	// Get a message
	receivedByteCount = recv(CtrClient[i].socket, (char*)&CtrClient[i].recvBuf, sizeof(Message), 0);

	// check for errors
	if (receivedByteCount == -1)
	{
		int err = WSAGetLastError();

#if TEST_DEBUG
		if (err != WSAEWOULDBLOCK)
		{
			printf("Error %d\n", err);
		}
#endif
		// if someone disconnected
		if (err == WSAECONNRESET)
		{
			// If one person disconnects, boot everybody
#if 1
			ResetClients();
			return;
#endif

			// This will be used in MasterServer, where we dont kick all players
			// because of one disconnection. However, we can't do that in NodeServer rn
#if 0
			// if this is not the last client
			if (i != clientCount - 1)
			{
				// shift all existing clients
				for (int j = i; j < clientCount; j++)
					memcpy(&CtrClient[i], &CtrClient[i + 1], sizeof(CtrClient));

				// repeat the loop for this socket index,
				// since a new socket is in that place
				i--;
			}

			clientCount--;
			CtrClient[clientCount].socket = INVALID_SOCKET;

			return;
#endif
		}

		goto SendToClient;
	}

	// check for incomplete message
	if (receivedByteCount < CtrClient[i].recvBuf.size)
	{
#if TEST_DEBUG
		printf("Bug! -- Tag: %d, recvBuf.size: %d, recvCount: %d\n",
			CtrClient[i].recvBuf.type, CtrClient[i].recvBuf.size, receivedByteCount);
#endif

		goto SendToClient;
	}

	// By now, we can confirm we have a valid message

	// dont parse same message twice
	if (CtrClient[i].recvBuf.size == CtrClient[i].recvBufPrev.size)
		if (memcmp(&CtrClient[i].recvBuf, &CtrClient[i].recvBufPrev, CtrClient[i].recvBuf.size) == 0)
			
			// If this is "return", then sending '5' will fail
			goto SendToClient;

	// make a backup
	memcpy(&CtrClient[i].recvBufPrev, &CtrClient[i].recvBuf, sizeof(Message));

	// message 0, 1, 2 will not come from client
	type = CtrClient[i].recvBuf.type;
	size = CtrClient[i].recvBuf.size;

	// parse message, depending on type
	if (type >= 0 && type <= sizeof(RecvMessage) / sizeof(RecvMessage[0]))
		RecvMessage[type](i);


SendToClient:

	// dont send the same message twice, 
	// or
	// To do: if client has not gotten prev message
	if (CtrClient[i].sendBuf.size == CtrClient[i].sendBufPrev.size)
		if (memcmp(&CtrClient[i].sendBuf, &CtrClient[i].sendBufPrev, CtrClient[i].sendBuf.size) == 0)
			return;

	// send a message to the client
	send(CtrClient[i].socket, (char*)&CtrClient[i].sendBuf, sizeof(Message), 0);

	// make a backup
	memcpy(&CtrClient[i].sendBufPrev, &CtrClient[i].sendBuf, sizeof(Message));

#if TEST_DEBUG

	type = CtrClient[i].sendBuf.type;
	size = CtrClient[i].sendBuf.size;

	if (type == 0)
	{
		// parse message
		char c1 = CtrClient[i].sendBuf.data[0];
		char c2 = CtrClient[i].sendBuf.data[1];

		printf("Send -- Tag: %d, size: %d, -- %d %d\n", type, size, c1, c2);
	}

	if (type == 3)
	{
		short s1 = *(short*)&CtrClient[i].sendBuf.data[0];

		printf("Send -- Tag: %d, size: %d, -- %04X\n", type, size, s1);
	}

	// type 4 will not come from server

	if (type == 5)
	{
		printf("Send -- Tag: %d, size: %d\n", type, size);
	}
#endif
}

void preparePositionMessage()
{
	Message* sendBuf;

	// Server sends to client
	// Client sends to server
	// 3 means Position Message

	for (int i = 0; i < clientCount; i++)
	{
		sendBuf = &CtrClient[i].sendBuf;

		sendBuf->type = 3;
		sendBuf->size = 2 + Type3_Size * (clientCount-1);

		int k = 0;

		for (int j = 0; j < clientCount; j++)
		{
			if (i == j)
				continue;

			// Why am I using this new "data" instead of recvBuf???
			// Investigate later
			memcpy(&sendBuf->data[Type3_Size * k], &CtrClient[j].recvBuf.data[0], Type3_Size);

			k++;
		}
	}
}

int main(int argc, char** argv)
{
	initialize();

	clock_t start = clock();
	clock_t end = clock();

	// Main loop...
	while (true)
	{
		// end of previous cycle
		end = clock();

		// If you finished in less than 16ms (1/60 second) 
		int ms = end - start;
		if (ms < 16) Sleep(16 - ms);

		// start of next cycle
		start = clock();

		// good for your CPU
		Sleep(5);

		if (!inGame)
		{
			CheckForNewClients();

			// send data to clients
			for (int i = 0; i < clientCount; i++)
			{
				HandleClient(i);
			}

			// if the waiting is over
			if (startLine.allHere())
			{
				// tell everyone to start
				for (int i = 0; i < clientCount; i++)
				{
					CtrClient[i].sendBuf.type = 5;
					CtrClient[i].sendBuf.size = 2;
				}

				inGame = true;

#if TEST_DEBUG
				// you are now racing
				printf("You are now racing\n");
#endif
			}
		}

		if (inGame)
		{
			if (!startLine.anyoneHere())
			{
				inGame = false;

#if TEST_DEBUG
				// you are now in menus
				printf("You are now in menus\n");
#endif
			}

			// check each client for message
			for (int i = 0; i < clientCount; i++)
			{
				HandleClient(i);
			}

			preparePositionMessage();
		}
	}

	return 0;
}