#include <cstdlib>
#include <ctime>
#include <thread>
#include <time.h>

#include <stdio.h>
#include <Windows.h>
#include <tlhelp32.h>
#include <windows.h>

#include <SDL_net.h>
#include "Scanner.h"

#define uint32_t DWORD

// Can be negative
long long int baseAddress;
HANDLE handle;

int PORT = 1234;
int sendLength = 0;
const unsigned short BUFFER_SIZE = 512;
unsigned short MAX_SOCKETS;
unsigned short MAX_CONNECTIONS;

bool pressingF9 = false;
bool pressingF10 = false;

SDLNet_SocketSet socketSet;
TCPsocket mySocket;
TCPsocket clientSocket[8];
bool      socketIsFree[8];

char sendBuf[BUFFER_SIZE];
char recvBuf[BUFFER_SIZE];
int receivedByteCount = 0;
int clientCount = 0;
bool shutdownServer = false;

// must be set to true after introAnim is 1
bool inGame = false;
unsigned int AddrP1 = 0;
int ePSXeModule = 0;

// needed for all hacks
DWORD GetProcId(const wchar_t* processName, DWORD desiredIndex)
{
	// create snapshot of PROCESS
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

	// check for valid
	if (hSnap != INVALID_HANDLE_VALUE)
	{
		// process entry
		PROCESSENTRY32 procEntry;
		procEntry.dwSize = sizeof(procEntry);

		// start looping through processes
		if (Process32First(hSnap, &procEntry))
		{
			int processIndex = 0;

			// check if this is the right process
			do
			{
				// if the name of this process is the name we are searching
				if (!_wcsicmp(procEntry.szExeFile, processName))
				{
					if (processIndex == desiredIndex)
					{
						// return this process ID
						return procEntry.th32ProcessID;
						break;
					}

					processIndex++;
				}

				// check the next one
			} while (Process32Next(hSnap, &procEntry));
		}
	}

	// if the procID is not found
	printf("Failed to inject emulator\n");
	system("pause");
	exit(0);

	// close the snap and return 0
	CloseHandle(hSnap);
	return 0;
}

// Needed for all ePSXe hacks, used for DLLs in other hacks
uintptr_t GetModuleBaseAddress(DWORD procId, const wchar_t* modName)
{
	// create snapshot of MODULE and MODULE32
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, procId);

	// check for valid
	if (hSnap != INVALID_HANDLE_VALUE)
	{
		// module entry
		MODULEENTRY32 modEntry;
		modEntry.dwSize = sizeof(modEntry);

		// start looping through modules
		if (Module32First(hSnap, &modEntry))
		{
			// check if this is the right module
			do
			{
				// if the name of this module is the name we are searching
				if (!_wcsicmp(modEntry.szModule, modName))
				{
					// return this module base address
					return (uintptr_t)modEntry.modBaseAddr;
					break;
				}

				// check the next one
			} while (Module32Next(hSnap, &modEntry));
		}
	}

	// close the snap and return 0
	CloseHandle(hSnap);
	return 0;
}

void WriteMem(unsigned int psxAddr, void* pcAddr, int size)
{
	WriteProcessMemory(handle, (PBYTE*)(baseAddress + psxAddr), pcAddr, size, 0);
}

void ReadMem(unsigned int psxAddr, void* pcAddr, int size)
{
	ReadProcessMemory(handle, (PBYTE*)(baseAddress + psxAddr), pcAddr, size, 0);
}

unsigned char gameStateCurr; // 0x161A871
unsigned char weaponPrev; // relative to posX
unsigned char weaponCurr; // relative to posX
unsigned char trackID; // 0x163671A
unsigned char trackVideoID; // 0x16379C8
unsigned char LevelOfDetail; // 0xB0F85C
unsigned long long menuState;

bool isServer = false;
bool isClient = false;
bool pauseUntilSync = false;

// used by server only
// This is hardcoded to wait for one client
// This needs to wait for 7 clients in the future
bool waitingForClient = false;
bool serverSynced = false;

/*
MessageID
0 = trackID
1 = lapRow message
2 = start loading
3 = position
4 = kart ID
5 = synced (ready to start loading, or ready to start race)
*/

// ID[0] is the server's character
short characterIDs[8];
short initNavData[8];

// copied from here https://stackoverflow.com/questions/5891811/generate-random-number-between-1-and-3-in-c
int roll(int min, int max)
{
	// x is in [0,1[
	double x = rand() / static_cast<double>(RAND_MAX + 1);

	// [0,1[ * (max - min) + min is in [min,max[
	int that = min + static_cast<int>(x * (max - min));

	return that;
}

clock_t timePosted = 0;

void SendMessageThread(char* message)
{
	// record time that this was posted
	timePosted = clock();

	// set controller mode to 0P mode, trigger error message
	char _0 = 0;
	WriteMem(0x800987C9, &_0, sizeof(_0));

	// change the error message
	WriteMem(0x800BC684, message, strlen(message) + 1);

	// leave it for one second
	Sleep(1000);

	// if no new messages have appeared
	if (clock() - timePosted > 900)
	{
		// remove message
		char _1 = 1;
		WriteMem(0x800987C9, &_1, sizeof(_1));
	}
}

void SendMessage(char* message)
{
	std::thread t1(SendMessageThread, message);
	t1.detach();
}

int aiNavBackup[3] = { 0,0,0 };

void EnableAI()
{
	if (aiNavBackup[0] != 0)
	{
		// restore ASM so AI can take over
		WriteMem(0x80015538, &aiNavBackup[0], sizeof(int));
		WriteMem(0x80015560, &aiNavBackup[1], sizeof(int));
		WriteMem(0x80015594, &aiNavBackup[2], sizeof(int));

		aiNavBackup[0] = 0;
		aiNavBackup[1] = 0;
		aiNavBackup[2] = 0;
	}
}

void DisableAI()
{
	if (aiNavBackup[0] == 0)
	{
		// Mkae backups of the asm before overwriting
		ReadMem(0x80015538, &aiNavBackup[0], sizeof(int));
		ReadMem(0x80015560, &aiNavBackup[1], sizeof(int));
		ReadMem(0x80015594, &aiNavBackup[2], sizeof(int));

		// Stop AI system from writing position data
		int zero = 0;
		WriteMem(0x80015538, &zero, sizeof(int));
		WriteMem(0x80015560, &zero, sizeof(int));
		WriteMem(0x80015594, &zero, sizeof(int));
	}
}

void initialize()
{
	int choice = 0;
	HWND console = GetConsoleWindow();
	RECT r;
	GetWindowRect(console, &r); //stores the console's current dimensions

	// 300 + height of bar (25)
	MoveWindow(console, r.left, r.top, 400, 325, TRUE);

	// Initialize random number generator
	srand(time(NULL));

	printf("\n");
	printf("Step 1: Open any ps1 emulator\n");
	printf("Step 2: Open Crash Team Racing SCUS_94426\n");
	printf("Step 3: Go to character selection\n");
	printf("Step 4: Save a state (F1), then load it (F3)\n");
	printf("\n");
	printf("Step 5: Enter emulator PID from 'Details'\n");
	printf("           tab of Windows Task Manager\n");
	printf("Enter: ");

	DWORD procID = 0;
	scanf("%d", &procID);

	printf("\n");
	printf("Searching for CTR 94426 in emulator ram...\n");

	// get the base address, relative to the module
	ePSXeModule = GetModuleBaseAddress(procID, L"ePSXe.exe");

	// open the process with procID, and store it in the 'handle'
	handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, procID);

	// if handle fails to open
	if (!handle)
	{
		printf("Failed to open process\n");
		system("pause");
		exit(0);
	}

	// This idea to scan memory for 11 bytes to automatically
	// find that CTR is running, and to find the base address
	// of any emulator universally, was EuroAli's idea in the
	// CTR-Tools discord server. Thank you EuroAli

	// Shows at PSX address 0x8003C62D, only in CTR 94426
	char ctrData[12] = { 0x71, 0xDC, 0x01, 0x0C, 0x00, 0x00, 0x00, 0x00, 0xD0, 0xF9, 0x00, 0x0C };

	// can't be nullptr by default or it crashes,
	// it will become 1 when the loop starts
	baseAddress = 0;

	// Modified from https://guidedhacking.com/threads/hyperscan-fast-vast-memory-scanner.9659/
	std::vector<UINT_PTR> AddressHolder = Hyperscan::HYPERSCAN_SCANNER::Scan(procID, ctrData, 12, Hyperscan::HyperscanAllignment4Bytes,
		Hyperscan::HyperscanTypeExact);

	// Copy the result, need to add 1 for some reason
	baseAddress = AddressHolder[0] + 1;

	// Remove 0x8003C62D address of PSX memory,
	// to find the relative address where PSX memory
	// is located in RAM. It is ok for baseAddress
	// to be a negative number
	baseAddress -= 0x8003C62D;
	
	system("cls");
	printf("\nStep 6: Configure Network\n");
	printf("1: Server\n");
	printf("2: Client\n");
	printf("Enter: ");
	scanf("%d", &choice);

	printf("\n");

	// name of the server that you connect to
	char* serverName = nullptr;

	// if you want to be server
	if (choice == 1)
	{
		// set bool
		// set max variables
		// leave name as nullptr
		isServer = true;
		//SendMessage((char*)"Server");
		MAX_SOCKETS = 9;
		MAX_CONNECTIONS = MAX_SOCKETS - 1;
	}

	// if you dont hit 1, you're a client
	else
	{
		// set bool
		// set max variables
		// get server name
		isClient = true;
		//SendMessage((char*)"Client");
		MAX_SOCKETS = 1;
		MAX_CONNECTIONS = 1;
		printf("Enter IP or URL: ");
		serverName = (char*)malloc(80);
		scanf("%79s", serverName);

		char text[100];
		sprintf(text, "IP: %s", serverName);
		//SendMessage(text);
	}

	// get the port
	printf("Enter Port: ");
	scanf("%d", &PORT);

	char text[100];
	sprintf(text, "Port: %d", PORT);
	//SendMessage(text);

	// initialize SDL_Net
	SDLNet_Init();

	// initialize socket set
	socketSet = SDLNet_AllocSocketSet(MAX_SOCKETS);

	// set all connections to NULL
	for (int loop = 0; loop < MAX_CONNECTIONS; loop++)
	{
		clientSocket[loop] = NULL;
		socketIsFree[loop] = true; // Set all our sockets to be free (i.e. available for use for new client connections)
	}

	// the IP of the server
	// this is not the same as serverName
	// serverName goes through DNS (could be IP or URL) 
	// serverIP is the IP that the DNS returns
	IPaddress serverIP;

	// Get the IP from the DNS, given the serverName
	int hostResolved = SDLNet_ResolveHost(&serverIP, serverName, PORT);

	// if you do not want to go through DNS, you
	// can set the IP and Port by copying bytes
	// into serverIP, but I do it this way so that
	// I can use URLs

	// if you are server, this opens a server socket on your PC
	// if you are client, this opens a connection socket to server
	mySocket = SDLNet_TCP_Open(&serverIP);
	SDLNet_TCP_AddSocket(socketSet, mySocket);

	// wait so that the PORT message appears
	Sleep(1000);

	system("cls\n");

	// check for any problem when trying to connect
	if (isClient)
	{
		if(!mySocket)
			printf("Failed to connect\n");
		else

			printf("Connected to server\n");
	}

	if (isServer)
	{
		if (!mySocket)
			printf("Failed to host\n");
		else

			printf("Host ready\n");
	}

	if (isServer)
	{
		printf("Choose a character (hit F10 for random),\n");
		printf("wait for all players to enter the\n");
		printf("track selection menu before starting\n");
		printf("the race. Ask everyone if they are ready\n");
		printf("\n");
		printf("Press F9, then Up, for Battle Maps\n");
		printf("Press F10 for random track\n");
	}

	if (isClient)
	{
		printf("Choose a character (hit F10 for random)\n");
		printf("sit still, your menu syncs with the server\n");
	}

	// Unlock all cars and tracks immediately
	unsigned long long value = 0xFFFFFFFFFFFFFFFF;
	WriteMem(0x8008E6EC, &value, sizeof(value));

	// Ja ra, return asm, 
	// disable weapons for players and enemies
	int jaRa = 0x3e00008;
	WriteMem(0x8006540C, &jaRa, sizeof(int));

	// Patch the first if-statement of FUN_8003282c
	// Allow 4 characters to load in high LOD
	int zero = 0;
	WriteMem(0x80032840, &zero, sizeof(int));

	short HighMpk = 0x00F2;
	WriteMem(0x80032888, &HighMpk, sizeof(short));
	WriteMem(0x800328A4, &HighMpk, sizeof(short));
	WriteMem(0x800328C0, &HighMpk, sizeof(short));
}

void drawAI(int aiNumber, unsigned int netPos[3])
{
	unsigned int AddrAI = AddrP1;
	AddrAI -= 0x670 * (aiNumber + 1);

	// This stops the AI from proceding with the NAV system,
	// but with this, the AI's values still get set to the 
	// values that the NAV system wants the AI to have at the starting line
	int _30 = 30;
	WriteMem(AddrAI + 0x604, &_30, sizeof(int));

	WriteMem(AddrAI + 0x2d4, &netPos[0], sizeof(int));
	WriteMem(AddrAI + 0x2d8, &netPos[1], sizeof(int));
	WriteMem(AddrAI + 0x2dC, &netPos[2], sizeof(int));

	WriteMem(AddrAI + 0x5f0, &netPos[0], sizeof(int));
	WriteMem(AddrAI + 0x5f4, &netPos[1], sizeof(int));
	WriteMem(AddrAI + 0x5f8, &netPos[2], sizeof(int));
}

void updateNetwork()
{
	// If you are server
	if (isServer)
	{
		// check all sockets in the set,
		// including mySocket, which (for the server)
		// is only for temporary connections
		SDLNet_CheckSockets(socketSet, 0);

		// if we see activity on the server socket,
		// which is NOT a client socket, it means
		// somebody is trying to connect
		if (SDLNet_SocketReady(mySocket) != 0)
		{
			// only accept a connection if there is room left on the server
			if (clientCount < MAX_CONNECTIONS)
			{
				printf("Found connection\n");

				int freeSpot = -99;
				for (int loop = 0; loop < MAX_CONNECTIONS; loop++)
				{
					if (socketIsFree[loop] == true)
					{
						//cout << "Found a free spot at element: " << loop << endl;
						socketIsFree[loop] = false; // Set the socket to be taken
						freeSpot = loop;            // Keep the location to add our connection at that index in the array of client sockets
						break;                      // Break out of the loop straight away
					}
				}

				// add socket to the clientSocket array
				// add socket to the socketSet
				// incrememnt clientCount
				clientSocket[freeSpot] = SDLNet_TCP_Accept(mySocket);
				SDLNet_TCP_AddSocket(socketSet, clientSocket[freeSpot]);
				clientCount++;

				// send a message to the client
				// This message does not have a tag, so client will throw it out
				// I will fix this later, when we actually need to use it
				int len = sprintf(sendBuf, "You are client %d", clientCount);
				SDLNet_TCP_Send(clientSocket[freeSpot], (void *)sendBuf, len + 1);
			}
		}

		// check each client for message
		for (int clientNumber = 0; clientNumber < MAX_CONNECTIONS; clientNumber++)
		{
			// if we see activity on the client sockets,
			// it means somebody sent us a message
			if (SDLNet_SocketReady(clientSocket[clientNumber]) != 0)
			{
				// clear before Recv
				memset(recvBuf, 0, BUFFER_SIZE);
				receivedByteCount = SDLNet_TCP_Recv(clientSocket[clientNumber], recvBuf, BUFFER_SIZE);

				//printf("Recv: %s\n", recvBuf);

				// check for an error
				if (receivedByteCount == -1)
					printf("Error: %s", SDLNet_GetError());

				// Get Character ID from Client
				int messageID = 0;
				int kartID = 0;
				if (sscanf(recvBuf, "%d", &messageID) == 1)
				{
					// message 0, 1, 2 will not come from client

					// 3 means Position Message (same in server and client)
					if (messageID == 3)
					{
						// This holds the position you get from network
						unsigned int netPos[3];

						// Get online player's position from the network message
						if (sscanf(recvBuf, "%d %u %u %u", &messageID, &netPos[0], &netPos[1], &netPos[2]) == 4)
						{
							// draw the first AI (index = 0)
							// at the position that we get
							drawAI(0, netPos);
						}
					}

					// 4 means Kart ID
					if (messageID == 4)
					{
						if (sscanf(recvBuf, "%d %d", &messageID, &kartID) == 2)
						{
							// Get characterID for this player
							// for characters 0 - 7:
							// CharacterID[i] : 0x1608EA4 + 2 * i
							short kartID_short = (short)kartID;
							characterIDs[1] = kartID_short;
						}
					}
				}

				// 5 means start race at traffic lights
				if (messageID == 5)
				{
					// if the server is ready to start
					if (serverSynced)
					{
						// this is hard-coded for one client
						// needs to work with 7 clients

						// if all clients send a 5 message,
						// then stop waiting and start race
						waitingForClient = false;

						memset(sendBuf, 0, BUFFER_SIZE);
						sendLength = sprintf(sendBuf, "5");
					}
				}

				// send a message to teh client
				int x = SDLNet_TCP_Send(clientSocket[clientNumber], sendBuf, sendLength + 1);

				//printf("Send: %s\n", sendBuf);

				// check for an error
				if (x == -1)
					printf("Error: %s", SDLNet_GetError());

				// Disconnect client
				// Put this somewhere

				/*SDLNet_TCP_DelSocket(socketSet, clientSocket[clientNumber]);
				SDLNet_TCP_Close(clientSocket[clientNumber]);
				clientSocket[clientNumber] = NULL;
				socketIsFree[clientNumber] = true;
				clientCount--;*/
			}
		}
	}

	if (isClient)
	{
		// check the socket, there should only be
		// one socket in this case, for the server
		SDLNet_CheckSockets(socketSet, 0);

		// The server uses mySocket as a temporary connection
		// The client uses mySocket for all server communication
		// This is because client will only be connected to
		// one socket, which is the server

		// if we have activity in our socket (assuming its from Server),
		// it means the server is trying to send us something
		if (SDLNet_SocketReady(mySocket) != 0)
		{
			// clear before Recv
			memset(recvBuf, 0, BUFFER_SIZE);
			receivedByteCount = SDLNet_TCP_Recv(mySocket, recvBuf, BUFFER_SIZE);

			//printf("Recv: %s\n", recvBuf);

			// check for an error
			if (receivedByteCount == -1)
				printf("Error: %s", SDLNet_GetError());

			int messageID = 0;
			if (sscanf(recvBuf, "%d", &messageID) == 1)
			{
				// 0 means track ID
				if (messageID == 0)
				{
					int trackIdFromBuf = 0;
					int kartID = 0;
					if (sscanf(recvBuf, "%d %d %d", &messageID, &trackIdFromBuf, &kartID) == 3)
					{
						// convert to one byte
						char trackByte = (char)trackIdFromBuf;

						char zero = 0;
						char ogTrackByte = 0;
						char OneNineOne = 191;
						char TwoFiveFive = 255;

						// Get characterID for this player
						// for characters 0 - 7:
						// CharacterID[i] : 0x1608EA4 + 2 * i
						short kartID_short = (short)kartID;
						characterIDs[1] = kartID_short;

						// close the lapRowSelector
						WriteMem(0x800B59AC, &zero, sizeof(char));

						// Get original track byte
						ReadMem(0x800B46FA, &ogTrackByte, sizeof(char));

						// set Text+Map address 
						WriteMem(0x800B46FA, &trackByte, sizeof(char));

						// set Video Address
						WriteMem(0x800B59A8, &trackByte, sizeof(char));

						// Spam the down button to update video, after selected-track changes
						if (ogTrackByte != trackByte)
						{
							// progress of video in menu
							char videoProgress[3] = { 1, 1, 1 };

							// keep hitting "down" until video refreshes and sets to zero
							while (videoProgress[0] != 0 || videoProgress[1] != 0 || videoProgress[2] != 0)
							{
								// read to see the new memory, 12 bytes, 3 ints
								ReadMem(0x8009EC28, &videoProgress[0], sizeof(char)); // first int
								ReadMem(0x8009EC2C, &videoProgress[1], sizeof(char)); // next int
								ReadMem(0x8009EC30, &videoProgress[2], sizeof(char)); // next int

								// Hit the 'Down' button on controller
								// 8008d2b0 -> 80096804
								// + 0x14
								char two = 2;
								WriteMem(0x80096804 + 0x14, &two, sizeof(char));
								
							}
						}
					}
				}

				// 1 means lap row, NOT DONE
				if (messageID == 1)
				{
					int lapIdFromBuf = 0;
					if (sscanf(recvBuf, "%d %d", &messageID, &lapIdFromBuf) == 2)
					{
						// open the lapRowSelector menu, 
						char one = 1;
						WriteMem(0x800B59AC, &one, sizeof(char));

						// convert to one byte
						char lapByte = (char)lapIdFromBuf;
						WriteMem(0x8008D920, &lapByte, sizeof(lapByte));

						// change the spawn order

						// Server:   0 1 2 3 4 5 6 7
						// Client 1: 1 0 2 3 4 5 6 7
						// Client 2: 1 2 0 3 4 5 6 7
						// Client 3: 1 2 3 0 4 5 6 7

						// this will change when we have more than 2 players
						char zero = 0;

						// Change the spawn order (look at comments above)
						// With only two players, this should be fine for now
						WriteMem(0x80080F28 + 0, &one, sizeof(char));
						WriteMem(0x80080F28 + 1, &zero, sizeof(char));
					}
				}

				// 2 means start loading, NOT DONE
				if (messageID == 2)
				{
					// let the client know that we are trying to load a race
					pauseUntilSync = true;

					char one = 1;
					char two = 2;

					// set menuA to 2 and menuB to 1,
					WriteMem(0x800B59AE, &two, sizeof(char));
					WriteMem(0x800B59B0, &one, sizeof(char));

					// Reset game frame counter to zero
					int zero = 0;
					WriteMem(0x80096B20 + 0x1cec, &zero, sizeof(int));

					inGame = false;

					// This message will include number of players
					// and array of characterIDs, save it for later
					// Stop looking for messages until later
				}
			
				// 3 means Position Message (same in server and client)
				if (messageID == 3)
				{
					// This holds the position you get from network
					unsigned int netPos[3];

					// Get online player's position from the network message
					if (sscanf(recvBuf, "%d %u %u %u", &messageID, &netPos[0], &netPos[1], &netPos[2]) == 4)
					{
						// draw the first AI (index = 0)
						// at the position that we get
						drawAI(0, netPos);
					}
				}

				// message 4 will not come from server

				// 5 means start race at traffic lights
				if (messageID == 5)
				{
					pauseUntilSync = false;

					// set controller mode to 1P, remove error message
					char _1 = 1;
					WriteMem(0x800987C9, &_1, sizeof(_1));
				}
			}
			// still need to handle disconnection
			// I will work on that later

			// send a message to the server
			int x = SDLNet_TCP_Send(mySocket, sendBuf, sendLength + 1);

			//printf("Send: %s\n", sendBuf);

			// check for an error
			if (x == -1)
				printf("Error: %s", SDLNet_GetError());
		}
	}
}

void SendOnlinePlayersToRAM()
{
	int numOnlinePlayers = 1;

	int i = 1;

	// put network characters into RAM
	for (i; i < numOnlinePlayers + 1; i++)
	{
		char oneByte = (char)characterIDs[i];
		WriteMem(0x80086E84 + 2 * i, &oneByte, sizeof(char)); // 4, for 2 shorts
	}

	// If you have less than 4 human drivers
	if (i < 4)
	{
		// load random other characters,
		// so that icons aren't confused with humans
		for (i; i < 4; i++)
		{
			char oneByte = 0;

			// find the first ID that is not loaded yet
			for (int j = 0; j < 8; j++)
			{
				bool found = true;

				// compare each j to each already-loaded characterID
				for (int k = 0; k < i; k++)
				{
					// if the character is already loaded
					if (characterIDs[k] == j)
					{
						found = false;
						k = 8;
					}
				}

				// if the character is not loaded
				if (found)
				{
					oneByte = j;
					j = 8;
				}
			}

			WriteMem(0x80086E84 + 2 * i, &oneByte, sizeof(char)); // 4, for 2 shorts
		}
	}

	// set the rest of the characters to the ID of P1
	for (i; i < 8; i++)
	{
		char oneByte = (char)characterIDs[0];
		WriteMem(0x80086E84 + 2 * i, &oneByte, sizeof(char)); // 4, for 2 shorts
	}
}

void SyncPlayersInMenus()
{
	// Get characterID for this player
	// for characters 0 - 7:
	// CharacterID[i] : 0x1608EA4 + 2 * i
	ReadMem(0x80086E84, &characterIDs[0], sizeof(short));

	// check if lapRowSelector is open
	bool lapRowSelectorOpen = false;
	ReadMem(0x800B59AC, &lapRowSelectorOpen, sizeof(bool));

	// Dont get stuck with Waiting for players...
	if (!lapRowSelectorOpen)
	{
		// If you are not waiting any more, then you must be synced,
		// wow I need to rename these
		pauseUntilSync = false;
		waitingForClient = false;
		serverSynced = false;

		char _1 = 1;
		WriteMem(0x800987C9, &_1, sizeof(_1));
	}

	// if you are the server
	if (isServer)
	{
		// if lap selector is closed
		if (!lapRowSelectorOpen)
		{
			// There are better ways to do input,
			// but it doesn't need to be perfect, it just needs to work
			if (!GetAsyncKeyState(VK_F9)) pressingF9 = false;

			// Enable Battle Tracks in Arcade
			if (GetAsyncKeyState(VK_F9) && !pressingF9)
			{
				// this disables key-repeat
				pressingF9 = true;

				// Write 25 to the track selection menu, which brings us to battle tracks
				char _25 = 25;
				WriteMem(0x800B46FA, &_25, sizeof(_25));
			}

			// There are better ways to do input,
			// but it doesn't need to be perfect, it just needs to work
			if (!GetAsyncKeyState(VK_F10)) pressingF10 = false;

			// Choose Random Track if you can't decide
			if (GetAsyncKeyState(VK_F10) && !pressingF10)
			{
				// this disables key-repeat
				pressingF10 = true;

				// get random track
				char trackByte = (char)roll(0, 17);

				// variable to hit the Down button
				char OneNineOne = 191;

				// set Text+Map address 
				WriteMem(0x800B46FA, &trackByte, sizeof(char));

				// set Video Address
				WriteMem(0x800B59A8, &trackByte, sizeof(char));

				// progress of video in menu
				char videoProgress[3] = { 1, 1, 1 };

				// keep hitting "down" until video refreshes and sets to zero
				while (videoProgress[0] != 0 || videoProgress[1] != 0 || videoProgress[2] != 0)
				{
					// read to see the new memory, 12 bytes, 3 ints
					ReadMem(0x8009EC28, &videoProgress[0], sizeof(char)); // first int
					ReadMem(0x8009EC2C, &videoProgress[1], sizeof(char)); // next int
					ReadMem(0x8009EC30, &videoProgress[2], sizeof(char)); // next int

					// Hit the 'Down' button on controller
					// 8008d2b0 -> 80096804
					// + 0x14
					char two = 2;
					WriteMem(0x80096804 + 0x14, &two, sizeof(char));
				}

				// Not sure if I want the "random track" button to automatically open
				// the lapRowSelector or not, but if we ever want it, here is the code

				// open the lapRowSelector
				//char one = 1;
				//WriteMem(0x800B59AC, &one, sizeof(char));

			}

			// Get Track ID, send it to clients
			ReadMem(0x800B46FA, &trackID, sizeof(trackID));

			// 0 means Track Message
			memset(sendBuf, 0, BUFFER_SIZE);
			sendLength = sprintf(sendBuf, "0 %d %d ", trackID, (int)characterIDs[0]);
		}

		// if lap selector is open
		else
		{
			// These determine if the loading screen has triggered yet
			unsigned char menuA = 0;
			unsigned char menuB = 0;
			ReadMem(0x800B59AE, &menuA, sizeof(menuA));
			ReadMem(0x800B59B0, &menuB, sizeof(menuB));

			// if race is starting
			if (menuA == 2 && menuB == 1)
			{
				// do not start the race
				pauseUntilSync = true;

				// wait for clients to be ready
				waitingForClient = true;

				// server is not ready to race
				serverSynced = false;

				// Reset game frame counter to zero
				int zero = 0;
				WriteMem(0x80096B20 + 0x1cec, &zero, sizeof(int));

				inGame = false;

				//printf("Sending to clients: Start Race with X amount of players and Array of characters\n");

				// In the future, rather than sending a message to start with no info,
				// we will send a message to start, with the number of players, and 
				// which character each player selected

				// 2 means Start Message
				memset(sendBuf, 0, BUFFER_SIZE);
				sendLength = sprintf(sendBuf, "2 ");
			}

			// if lap is being chosen
			else
			{
				// 0 -> 3 laps
				// 1 -> 5 laps
				// 2 -> 7 laps
				unsigned char lapRowSelected = 0;
				ReadMem(0x8008D920, &lapRowSelected, sizeof(lapRowSelected));

				// 1 means Lap Message
				memset(sendBuf, 0, BUFFER_SIZE);
				sendLength = sprintf(sendBuf, "1 %d", lapRowSelected);
			}
		}
	}

	// if you are the client
	if (isClient)
	{
		// 0 means Track Message
		memset(sendBuf, 0, BUFFER_SIZE);
		sendLength = sprintf(sendBuf, "4 %d ", (int)characterIDs[0]);

		// Get the new Track ID
		ReadMem(0x800B46FA, &trackID, sizeof(trackID));
	}
}

void throwExtrasToVoid()
{
	// In a 2-player game, there is one player over network
	int numberOfNetworkPlayers = 1;

	// Move unwanted players into oblivion
	for (int i = numberOfNetworkPlayers; i < 7; i++)
	{
		int Gone = 99999999;

		unsigned int AddrAI = AddrP1;
		AddrAI -= 0x670 * (i + 1);

		int one = 1;
		WriteMem(AddrAI + 0x604, &one, sizeof(int));

		// Teleport them all under the track.
		// You can try changing X and Z, but they'll just warp back to spawn.
		// Only height goes into effect, which is all we need.
		WriteMem(AddrAI + 0x2d8, &Gone, sizeof(int));
		WriteMem(AddrAI + 0x5f4, &Gone, sizeof(int));
	}
}



void preparePositionMessage()
{
	// Get Player 1 position
	// All players have 12-byte positions (4x3). 
	// Changing those coordinates will not move
	// the players. 
	unsigned int rawPosition[3];
	ReadMem(AddrP1 + 0x2D4, &rawPosition[0], sizeof(int));
	ReadMem(AddrP1 + 0x2D8, &rawPosition[1], sizeof(int));
	ReadMem(AddrP1 + 0x2DC, &rawPosition[2], sizeof(int));

	// Server sends to client
	// Client sends to server
	// 3 means Position Message
	memset(sendBuf, 0, BUFFER_SIZE);
	sendLength = sprintf(sendBuf, "3 %u %u %u", rawPosition[0], rawPosition[1], rawPosition[2]);
}

void updateRace()
{	
	// teleport extra characters under the world
	throwExtrasToVoid();

	// set controller mode to 1P mode, disable error message
	// The message gets enabled lower in the code
	char _1 = 1;
	WriteMem(0x800987C9, &_1, sizeof(_1));

	// If not all racers are ready to start
	if (pauseUntilSync)
	{
		// Set the traffic lights to be above the screen
		// They are set to 3840 by default without modding
		short wait = 4500;
		WriteMem(0x8009882C, &wait, sizeof(short));

		// see if the intro cutscene is playing
		// becomes 0 when traffic lights should show
		char introAnimState;
		ReadMem(0x801FFDDE, &introAnimState, sizeof(char));

		// if the intro animation is done
		if (introAnimState == 0)
		{
			// set controller mode to 0P mode, trigger error message
			char _0 = 0;
			WriteMem(0x800987C9, &_0, sizeof(_0));

			// change the error message
			WriteMem(0x800BC684, (char*)"waiting for players...", 23);

			if (isClient)
			{
				// let the server know you are ready
				sendLength = sprintf(sendBuf, "5");
			}

			if (isServer)
			{
				serverSynced = true;

				// if the waiting is over
				if (!waitingForClient)
				{
					// tell everyone to start
					sendLength = sprintf(sendBuf, "5");

					// start the race
					pauseUntilSync = false;

					// set controller mode to 1P, remove error message
					char _1 = 1;
					WriteMem(0x800987C9, &_1, sizeof(_1));
				}
			}

			// send a message to the server that you are ready to start

			// all clients send message to server
			// when all clients and server are ready
			// server sends start message
		}

		// skip the rest
		return;
	}

	// Send your player's position to the server (or client)
	// If you are the client, this sends to server, DONE
	// If you are the server, this sends to one client, NOT DONE
	preparePositionMessage();

}

int main(int argc, char **argv)
{
	/*union
	{
		short x[3];
		unsigned char y[6];
	} x;

	// -3005, 677, -4693
	// 67 244 165 2 171 237

	x.x[0] = -3005;
	x.x[1] = 677;
	x.x[2] = -4693;

	printf("%hu, %hu, %hu\n", x.x[0], x.x[1], x.x[2]);
	for(int i = 0; i < 6; i++)printf("%d ", x.y[i]);

	printf("\n");*/

	//=======================================================================

	initialize();

	// Main loop...
	while(true)
	{
		ReadMem(0x8009900C, &AddrP1, sizeof(AddrP1));

		// handle all message reading and writing
		updateNetwork();

		short inCharSelection = 0;
		ReadMem(0x8008D908, &inCharSelection, sizeof(inCharSelection));

		// if you are in character selection menu
		if (inCharSelection == 18100)
		{
			// if ePSXe
			if (ePSXeModule != 0)
			{
				char f = 0xF;
				char _80 = 0x80;
				char _C0 = 0xC0;

				// Write array of icon ids
				WriteMem(0x800b50d2, &f, sizeof(f));

				// Change Pura Nav to go "down" to Oxide
				WriteMem(0x800B4ED9, &f, sizeof(f));

				// Change Papu Nav to go "down" to Oxide
				WriteMem(0x800B4F09, &f, sizeof(f));

				// Move Komodo Joe
				WriteMem(0x800B4F10, &_80, sizeof(_80));

				// Move Penta Penguin
				WriteMem(0x800B4F1C, &_C0, sizeof(_C0));

				// Move Fake Crash, change nav to point to oxide
				char fakeCrashData[8] = { 0x00, 0x01, 0xAE, 0x00, 0x06, 0x0E, 0x0D, 0x0F };
				WriteMem(0x800B4F28, &fakeCrashData[0], 8);

				// Change 3P's Crash Icon to 1P's Oxide Icon
				char oxideData[10] = { 0x40, 0x01, 0xAE, 0x00, 0x07, 0x0F, 0x0E, 0x0B, 0x0F, 0x00 };
				WriteMem(0x800B4F34, &oxideData[0], 10);

				char _10 = 0x10;
				WriteMem(0x800AE524, &_10, sizeof(char));
				WriteMem(0x800AF398, &_10, sizeof(char));
				WriteMem(0x800AF7C4, &_10, sizeof(char));

				char a;
				char b;
				char c;
				char d;
				ReadMem(0x80086E84, &a, sizeof(char));
				ReadMem(0x800B59F0, &b, sizeof(char));
				ReadMem(0x800B59F8, &c, sizeof(char));
				ReadMem(0x801FFEA8, &d, sizeof(char));

				if (a == 15 || b == 15 || c == 15 || d == 15)
				{
					WriteMem(0x800B4D45, &_10, 1);
				}

				else
				{
					char zero = 0;
					WriteMem(0x800B4D45, &zero, sizeof(char));
				}
			}

			// There are better ways to do input,
			// but it doesn't need to be perfect, it just needs to work
			if (!GetAsyncKeyState(VK_F10)) pressingF10 = false;

			// Choose Random Track if you can't decide
			if (GetAsyncKeyState(VK_F10) && !pressingF10)
			{
				// this disables key-repeat
				pressingF10 = true;

				// Get random kart
				char kartByte = (char)roll(0, 0xF);
				characterIDs[0] = kartByte;
				WriteMem(0x80086E84, &kartByte, sizeof(char));
			}

			Sleep(1);
			continue;
		}

		// Check to see if you are in the track selection menu
		bool inTrackSelection = false;
		ReadMem(0x8008D88C, &inTrackSelection, sizeof(inTrackSelection));

		// if you're in the track selection menu
		if (inTrackSelection)
		{
			// Disable AIs so that humans can be injected
			DisableAI();

			// copy server menu state to client, and exchange character info
			SyncPlayersInMenus();

			// 1000 ms per second
			// 1/1000 second
			// prevent network spam
			// without missing a message
			Sleep(1);

			// Restart the loop, don't proceed
			continue;
		}

		// If you aren't in track selection,
		// spam less, to reduce chance of network lag,
		// messages are larger in gameplay
		Sleep(20);

		// GameState
		// 2 = loading screen
		// 10 = some menus, intro of race (including traffic lights)
		// 11 = racing

		// Read gameStateCurr
		ReadMem(0x80098851, &gameStateCurr, sizeof(gameStateCurr));

		// when you're in the loading screen
		if (gameStateCurr == 2)
		{
			// reset messages
			memset(recvBuf, 0, BUFFER_SIZE);
			memset(sendBuf, 0, BUFFER_SIZE);

			// constantly write these values,
			// to make sure the right characters are loaded
			SendOnlinePlayersToRAM();

			// restart the loop
			continue;
		}



		// Read Game Timer
		int timer = 0;
		ReadMem(0x80096B20 + 0x1cec, &timer, sizeof(int));

		if (
			timer > 10 &&

			(gameStateCurr == 11 || gameStateCurr == 10)
			)
		{
			inGame = true;
		}

		if (inGame)
		{
			int playerFlags = 0;
			ReadMem(AddrP1 + 0x2c8, &playerFlags, sizeof(int));
			if((playerFlags & 0x2000000) != 0) 
			{
				// Enable AIs properly to take over
				EnableAI();
			}

			updateRace();
		}
	}

	// delete everything we initialized
	SDLNet_FreeSocketSet(socketSet);
	SDLNet_TCP_Close(mySocket);
	SDLNet_Quit();
	return 0;
}