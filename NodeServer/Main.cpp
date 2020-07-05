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
	// 3 for position
	//		posX, posY, posZ

	// [client -> server]
	// 4 for kart IDs
	//		client character

	// [bidirectional]
	// 5 for start-line sync
	//		[null]

	unsigned char type;
	unsigned char size;
	char data[2 + 12 * 4];
};

struct SocketCtr
{
	SOCKET socket;
	Message sendBuf;
	Message sendBufPrev;
	Message recvBuf;
	Message recvBufPrev;
	int pos[3];
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

void initialize()
{
	// set all connections to INVALID_SOCKET
	for (int i = 0; i < MAX_CLIENTS; i++)
		memset(&CtrClient[0], 0xFF, sizeof(CtrClient[0]) * MAX_CLIENTS);

	int choice = 0;
	HWND console = GetConsoleWindow();
	RECT r;
	GetWindowRect(console, &r); //stores the console's current dimensions

	// 300 + height of bar (25)
	MoveWindow(console, r.left, r.top, 400, 325, TRUE);

	WSADATA wsaData;
	int iResult;

	// Initialize Winsock
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf("WSAStartup failed with error: %d\n", iResult);
		system("pause");
	}

	BuildListeningSocket();

	startLine.reset();
	trackSel.reset();

	printf("Host ready on port 1234\n\n");

	// set LISTENING socket to non-blocking
	unsigned long nonBlocking = 1;
	iResult = ioctlsocket(CtrMain.socket, FIONBIO, &nonBlocking);

	FD_ZERO(&master);
	FD_SET(CtrMain.socket, &master);
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

void HandleClient(int i)
{
	unsigned char type = 0xFF;
	unsigned char size = 0xFF;

	// Get a message
	memset(&CtrClient[i].recvBuf, 0xFF, sizeof(Message));
	receivedByteCount = recv(CtrClient[i].socket, (char*)&CtrClient[i].recvBuf, sizeof(Message), 0);

	if (receivedByteCount == -1)
		goto SendToClient;
	//printf("Error %d\n", WSAGetLastError());

	if (receivedByteCount == 0)
	{
		// disconnect

		printf("Someone disconnected\n");

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
	}

	if (receivedByteCount < CtrClient[i].recvBuf.size)
	{
		//printf("Bug! -- Tag: %d, recvBuf.size: %d, recvCount: %d\n",
			//recvBuf.type, recvBuf.size, receivedByteCount);

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

	// 3 means Position Message (same in server and client)
	if (type == 3)
	{
		// store a backup
		memcpy(&CtrClient[i].pos[0], &CtrClient[i].recvBuf.data[0], 12);

#if TEST_DEBUG
		printf("Recv -- Tag: %d, size: %d, -- %d %d %d\n", type, size,
			*(int*)&CtrClient[i].recvBuf.data[0],
			*(int*)&CtrClient[i].recvBuf.data[4],
			*(int*)&CtrClient[i].recvBuf.data[8]);
#endif
	}

	// 4 means Kart ID
	if (type == 4)
	{
		// Get characterID for this player
		// for characters 0 - 7:
		// CharacterID[i] : 0x1608EA4 + 2 * i
		characterIDs[i] = (short)CtrClient[i].recvBuf.data[0];

#if TEST_DEBUG
		printf("Recv -- Tag: %d, size: %d, -- %d\n", type, size,
			CtrClient[i].recvBuf.data[0]);
#endif
	}

	// if the client "wants" to start the race
	if (type == 5)
	{
		// if all clients send a 5 message,
		// then stop waiting and start race
		startLine.clientsHere[i] = true;

#if TEST_DEBUG
		printf("Recv -- Tag: %d, size: %d\n", type, size);
#endif
	}

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

	if (type == 1)
	{
		// parse message
		char c1 = CtrClient[i].sendBuf.data[0];

		printf("Send -- Tag: %d, size: %d, -- %d\n", type, size, c1);
	}

	if (type == 2)
	{
		printf("Send -- Tag: %d, size: %d\n", type, size);
	}

	if (type == 3)
	{
		int i1 = *(int*)&CtrClient[i].sendBuf.data[0];
		int i2 = *(int*)&CtrClient[i].sendBuf.data[4];
		int i3 = *(int*)&CtrClient[i].sendBuf.data[8];

		printf("Send -- Tag: %d, size: %d -- %d %d %d\n", type, size, i1, i2, i3);
	}

	// type 4 will not come from server

	if (type == 5)
	{
		printf("Send -- Tag: %d, size: %d\n", type, size);
	}
#endif
}

void SyncPlayersInMenus()
{
	// warning, if you dont get message 0 from host,
	// then non-hosts wont get characters of nonhosts

	// if the last message you got from host was '0' message
	if (CtrClient[0].recvBuf.type == 0)
	{
		unsigned char trackID = CtrClient[0].recvBuf.data[0];
		characterIDs[0] = CtrClient[0].recvBuf.data[1];

		// send to all clients, since it includes characters
		for (int i = 0; i < clientCount; i++)
		{
			// track, driver index, num characters, characters

			CtrClient[i].sendBuf.type = 0;
			CtrClient[i].sendBuf.size = 5 + clientCount;

			CtrClient[i].sendBuf.data[0] = trackID;
			CtrClient[i].sendBuf.data[1] = i;
			CtrClient[i].sendBuf.data[2] = clientCount;

			int j = 0;
			int k = 0;
			for (; j < clientCount; j++)
			{
				if (i == j)
					continue;

				CtrClient[i].sendBuf.data[3 + k] = (char)characterIDs[j];
				k++;
			}
		}
	}

	// if last message was a '1' message
	else if (CtrClient[0].recvBuf.type == 1)
	{
		// send info to all "other" players
		for (int i = 1; i < clientCount; i++)
		{
			memcpy(&CtrClient[i].sendBuf, &CtrClient[0].recvBuf, 3);
		}
	}

	// if you get a '2' message from host
	else if (CtrClient[0].recvBuf.type == 2)
	{
		// wait for everyone at starting line
		startLine.reset();

		// send to all "other" clients
		for (int i = 1; i < clientCount; i++)
		{
			memcpy(&CtrClient[i].sendBuf, &CtrClient[0].recvBuf, 2);
		}
	}
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
		sendBuf->size = 2 + 12 * clientCount;

		int k = 0;

		for (int j = 0; j < clientCount; j++)
		{
			if (i == j)
				continue;

			memcpy(&sendBuf->data[12 * k], CtrClient[j].pos, 12);

			k++;
		}
	}
}

int main(int argc, char** argv)
{
	initialize();

	// Main loop...
	while (true)
	{
		// good for your CPU
		Sleep(1);

		if (!inGame)
		{
			CheckForNewClients();

			// first client is the host
			HandleClient(0);

			// use the host's messages
			SyncPlayersInMenus();

			// send data to clients
			for (int i = 1; i < clientCount; i++)
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

				// erase data for next race
				startLine.reset();

				// you are now racing
				inGame = true;
			}
		}

		if (inGame)
		{
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