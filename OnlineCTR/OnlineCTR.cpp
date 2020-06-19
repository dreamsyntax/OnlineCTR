#include <iostream>
#include <cstdlib>
#include <string>
#include <ctime>
#include <thread>
#include <time.h>

#include <stdio.h>
#include <Windows.h>
#include <tlhelp32.h>

#include <SDL_net.h>

#define uint32_t DWORD

DWORD baseAddress;
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
	WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB1A7E9), &_0, sizeof(_0), NULL);

	// change the error message
	WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB3E6A4), message, strlen(message) + 1, NULL);

	// leave it for one second
	Sleep(1000);

	// if no new messages have appeared
	if (clock() - timePosted > 900)
	{
		// remove message
		char _1 = 1;
		WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB1A7E9), &_1, sizeof(_1), NULL);
	}
}

void SendMessage(char* message)
{
	std::thread t1(SendMessageThread, message);
	t1.detach();
}

void UnlockPlayersAndTracks()
{
	// Unlock all cars and tracks immediately
	unsigned long long value = 18446744073709551615;
	WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB1070C), &value, sizeof(value), 0);

	// This writes 0b11111111...
	// which enables all flags in 8 bytes.

	// These flags determine what is unlocked,
	// so by setting all bits to 1, it unlocks everything
}

void initialize()
{
	HWND console = GetConsoleWindow();
	RECT r;
	GetWindowRect(console, &r); //stores the console's current dimensions

	// 300 + height of bar (25)
	MoveWindow(console, r.left, r.top, 400, 325, TRUE);

	// Initialize random number generator
	srand(time(NULL));

	printf("Step 1: Open ePSXe.exe\n");
	printf("Step 2: Open Crash Team Racing SCUS_94426\n");
	printf("Step 3: Go to track selection\n");
	printf("\n");
	printf("Step 4: Enter ProcessID below\n");
	printf("For auto-detection, enter 0\n\n");
	printf("Enter: ");

	DWORD procID = 0;
	scanf("%d", &procID);

	// auto detect the procID if the user enters number less than 5
	// This will act as instance index (first instance, second, etc)
	if(procID < 10) procID = GetProcId(L"ePSXe.exe", procID);

	// get the base address, relative to the module
	baseAddress = GetModuleBaseAddress(procID, L"ePSXe.exe");

	// if the procID is not found
	if (!procID)
	{
		printf("Failed to find ePSXe.exe\n");
		printf("open ePSXe.exe\n");
		printf("and try again\n");

		system("pause");
		exit(0);
	}

	// open the process with procID, and store it in the 'handle'
	handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, procID);

	// if handle fails to open
	if (!handle)
	{
		printf("Failed to open process\n");
		system("pause");
		exit(0);
	}

	// welcome the player
	SendMessage((char*)"OnlineCTR");

	// Unlock all cars and tracks immediately
	UnlockPlayersAndTracks();
	
	printf("\nStep 5: Configure Network\n");

	int choice = 0;
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
		SendMessage((char*)"Server");
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
		SendMessage((char*)"Client");
		MAX_SOCKETS = 1;
		MAX_CONNECTIONS = 1;
		printf("Enter IP or URL: ");
		serverName = (char*)malloc(80);
		scanf("%79s", serverName);

		char text[100];
		sprintf(text, "IP: %s", serverName);
		SendMessage(text);
	}

	// get the port
	printf("Enter Port: ");
	scanf("%d", &PORT);

	char text[100];
	sprintf(text, "Port: %d", PORT);
	SendMessage(text);

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

	// check for any problem when trying to connect
	if (isClient)
	{
		if(!mySocket)
			SendMessage((char*)"Failed to connect");
		else

			SendMessage((char*)"Connected to server");
	}

	if (isServer)
	{
		if (!mySocket)
			SendMessage((char*)"Failed to host");
		else

			SendMessage((char*)"Host ready");
	}

	system("cls\n");

	if (isServer)
	{
		printf("Press F9, then Up, for Battle Maps\n");
		printf("Press F10 for random track\n");
	}

	if (isClient)
		printf("Sit still, your menu will sync with the server\n");

	// disable parts of AI in the EXE (in RAM)
	int zero = 0;
	WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xA97558), &zero, sizeof(int), NULL);
	WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xA97580), &zero, sizeof(int), NULL);
	WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xA975B4), &zero, sizeof(int), NULL);

	// Ja ra, return asm, 
	// disable weapons for players and enemies
	int jaRa = 0x3e00008;
	WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xA82020 + 0x6540C), &jaRa, sizeof(int), NULL);
}

void drawAI(int aiNumber, unsigned int netPos[3])
{
	unsigned int AddrAI = AddrP1;
	AddrAI -= 0x670 * (aiNumber + 1);

	// This stops the AI from proceding with the NAV system,
	// but with this, the AI's values still get set to the 
	// values that the NAV system wants the AI to have at the starting line
	int _30 = 30;
	WriteProcessMemory(handle, (PBYTE*)(AddrAI + 0x604), &_30, sizeof(int), NULL);

	WriteProcessMemory(handle, (PBYTE*)(AddrAI + 0x2d4), &netPos[0], sizeof(int), NULL);
	WriteProcessMemory(handle, (PBYTE*)(AddrAI + 0x2d8), &netPos[1], sizeof(int), NULL);
	WriteProcessMemory(handle, (PBYTE*)(AddrAI + 0x2dC), &netPos[2], sizeof(int), NULL);

	WriteProcessMemory(handle, (PBYTE*)(AddrAI + 0x5f0), &netPos[0], sizeof(int), NULL);
	WriteProcessMemory(handle, (PBYTE*)(AddrAI + 0x5f4), &netPos[1], sizeof(int), NULL);
	WriteProcessMemory(handle, (PBYTE*)(AddrAI + 0x5f8), &netPos[2], sizeof(int), NULL);
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
				SendMessage((char*)"Found connection");

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
						WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB379CC), &zero, sizeof(char), 0);

						// Get original track byte
						ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB3671A), &ogTrackByte, sizeof(char), 0);

						// set Text+Map address 
						WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB3671A), &trackByte, sizeof(char), 0);

						// set Video Address
						WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB379C8), &trackByte, sizeof(char), 0);

						// Spam the down button to update video, after selected-track changes
						if (ogTrackByte != trackByte)
						{
							// progress of video in menu
							char videoProgress[3] = { 1, 1, 1 };

							// keep hitting "down" until video refreshes and sets to zero
							while (videoProgress[0] != 0 || videoProgress[1] != 0 || videoProgress[2] != 0)
							{
								// read to see the new memory, 12 bytes, 3 ints
								ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB20C48), &videoProgress[0], sizeof(char), 0); // first int
								ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB20C4C), &videoProgress[1], sizeof(char), 0); // next int
								ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB20C50), &videoProgress[2], sizeof(char), 0); // next int

								// Hit the 'Down' button on controller
								WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0x20603D), &OneNineOne, sizeof(char), 0);
								WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0x22C58D), &OneNineOne, sizeof(char), 0);
								WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0x99DDA8), &OneNineOne, sizeof(char), 0);
								WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB18AF8), &OneNineOne, sizeof(char), 0);
								WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB21630), &OneNineOne, sizeof(char), 0);
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
						WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB379CC), &one, sizeof(char), 0);

						// convert to one byte
						char lapByte = (char)lapIdFromBuf;
						WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB0F940), &lapByte, sizeof(lapByte), 0);

						// change the spawn order

						// Server:   0 1 2 3 4 5 6 7
						// Client 1: 1 0 2 3 4 5 6 7
						// Client 2: 1 2 0 3 4 5 6 7
						// Client 3: 1 2 3 0 4 5 6 7

						// this will change when we have more than 2 players
						char zero = 0;

						// Change the spawn order (look at comments above)
						// With only two players, this should be fine for now
						WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB02F48 + 0), &one, sizeof(char), 0);
						WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB02F48 + 1), &zero, sizeof(char), 0);
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
					WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB379CE), &two, sizeof(char), 0);
					WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB379D0), &one, sizeof(char), 0);

					// Reset game frame counter to zero
					int zero = 0;
					WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xA82020 + 0x96B20 + 0x1cec), &zero, sizeof(int), 0);

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
					WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB1A7E9), &_1, sizeof(_1), NULL);
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

void CalculateLOD()
{
	// Set LOD to 1 by default
	LevelOfDetail = 1;

	// if network player is using someone
	// who is an unlockable character,
	// not part of original 8
	if (characterIDs[1] > 7)
	{
		// if both the server and client are playing
		// as the same unlocked character, then LOD
		// does not need to change
		if (characterIDs[0] != characterIDs[1])
		{
			// Drop LOD to 3
			LevelOfDetail = 3;
		}
	}

	// If the net player is an original player, 
	// then choose if LOD should drop depending on Oxide
	else
	{
		// if you are playing as Oxide
		if (characterIDs[0] == 15)
		{
			// if you're choosing a track where LOD needs to drop
			if (
				trackID == 4 ||	// Mystery Caves
				trackID == 10 ||// Polar Pass
				trackID == 11 ||// Cortex Castle
				trackID == 13 ||// Hot Air Skyway
				trackID == 14 ||// N Gin Labs
				trackID == 15	// Oxide Station
				)
			{
				// Set LOD to 2
				LevelOfDetail = 2;
			}
		}
	}

	// Set the Level of Detail
	WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB0F85C), &LevelOfDetail, sizeof(LevelOfDetail), 0);
}

void SendOnlinePlayersToRAM()
{
	// put network characters into RAM
	for (int i = 1; i < 2; i++)
	{
		char oneByte = (char)characterIDs[i];
		WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB08EA4 + 2 * i), &oneByte, 1, 0); // 4, for 2 shorts
	}
}

void SyncPlayersInMenus()
{
	// Get characterID for this player
	// for characters 0 - 7:
	// CharacterID[i] : 0x1608EA4 + 2 * i
	ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB08EA4), &characterIDs[0], sizeof(short), 0);

	// if you are the server
	if (isServer)
	{
		// check if lapRowSelector is open
		bool lapRowSelectorOpen = false;
		ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB379CC), &lapRowSelectorOpen, sizeof(lapRowSelectorOpen), 0);

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
				WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB3671A), &_25, sizeof(_25), 0);
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
				WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB3671A), &trackByte, sizeof(char), 0);

				// set Video Address
				WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB379C8), &trackByte, sizeof(char), 0);

				// progress of video in menu
				char videoProgress[3] = { 1, 1, 1 };

				// keep hitting "down" until video refreshes and sets to zero
				while (videoProgress[0] != 0 || videoProgress[1] != 0 || videoProgress[2] != 0)
				{
					// read to see the new memory, 12 bytes, 3 ints
					ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB20C48), &videoProgress[0], sizeof(char), 0); // first int
					ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB20C4C), &videoProgress[1], sizeof(char), 0); // next int
					ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB20C50), &videoProgress[2], sizeof(char), 0); // next int

					// Hit the 'Down' button on controller
					WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0x20603D), &OneNineOne, sizeof(char), 0);
					WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0x22C58D), &OneNineOne, sizeof(char), 0);
					WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0x99DDA8), &OneNineOne, sizeof(char), 0);
					WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB18AF8), &OneNineOne, sizeof(char), 0);
					WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB21630), &OneNineOne, sizeof(char), 0);
				}

				// Not sure if I want the "random track" button to automatically open
				// the lapRowSelector or not, but if we ever want it, here is the code

				// open the lapRowSelector
				//char one = 1;
				//WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB379CC), &one, sizeof(char), 0);

			}

			// Get Track ID, send it to clients
			ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB3671A), &trackID, sizeof(trackID), 0);

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
			ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB379CE), &menuA, sizeof(menuA), 0);
			ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB379D0), &menuB, sizeof(menuB), 0);

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
				WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xA82020 + 0x96B20 + 0x1cec), &zero, sizeof(int), 0);

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
				ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB0F940), &lapRowSelected, sizeof(lapRowSelected), 0);

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
		ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB3671A), &trackID, sizeof(trackID), 0);
	}
}

void updateTrackSelection()
{
	// Play as Oxide if you press F11
	/*if (GetAsyncKeyState(VK_F11))
	{
		// set character ID to 15
		char _15 = 15;
		WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB08EA4), &_15, sizeof(_15), 0);
	}*/

	// copy server menu state to client, and exchange character info
	SyncPlayersInMenus();

	// determine LOD to prevent crashes
	CalculateLOD();

	pauseUntilSync = false;
	waitingForClient = false;
	serverSynced = false;
}

void updateLoadingScreen()
{
	// reset messages
	memset(recvBuf, 0, BUFFER_SIZE);
	memset(sendBuf, 0, BUFFER_SIZE);

	// constantly write these values,
	// to make sure the right characters are loaded
	SendOnlinePlayersToRAM();

	// dont call CalculateLOD becasue there
	// is no way it can change during loading screen,
	// characters and tracks are already chosen

	// Set the Level of Detail
	WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB0F85C), &LevelOfDetail, sizeof(LevelOfDetail), 0);
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
		WriteProcessMemory(handle, (PBYTE*)(AddrAI + 0x604), &one, sizeof(int), NULL);

		// Teleport them all under the track.
		// You can try changing X and Z, but they'll just warp back to spawn.
		// Only height goes into effect, which is all we need.
		WriteProcessMemory(handle, (PBYTE*)(AddrAI + 0x2d8), &Gone, sizeof(int), 0);
		WriteProcessMemory(handle, (PBYTE*)(AddrAI + 0x5f4), &Gone, sizeof(int), 0);
	}
}



void preparePositionMessage()
{
	// Get Player 1 position
	// All players have 12-byte positions (4x3). 
	// Changing those coordinates will not move
	// the players. 
	unsigned int rawPosition[3];
	ReadProcessMemory(handle, (PBYTE*)(AddrP1 + 0x2D4), &rawPosition[0], sizeof(int), 0);
	ReadProcessMemory(handle, (PBYTE*)(AddrP1 + 0x2D8), &rawPosition[1], sizeof(int), 0);
	ReadProcessMemory(handle, (PBYTE*)(AddrP1 + 0x2DC), &rawPosition[2], sizeof(int), 0);

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
	WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB1A7E9), &_1, sizeof(_1), NULL);

	// If not all racers are ready to start
	if (pauseUntilSync)
	{
		// Set the traffic lights to be above the screen
		// They are set to 3840 by default without modding
		short wait = 4500;
		WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB1A84C), &wait, sizeof(short), NULL);

		// see if the intro cutscene is playing
		// becomes 0 when traffic lights should show
		char introAnimState;
		ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xC81DFE), &introAnimState, sizeof(char), NULL);

		// if the intro animation is done
		if (introAnimState == 0)
		{
			// set controller mode to 0P mode, trigger error message
			char _0 = 0;
			WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB1A7E9), &_0, sizeof(_0), NULL);

			// change the error message
			WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB3E6A4), (char*)"waiting for players...", 23, NULL);

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
					WriteProcessMemory(handle, (PBYTE*)(baseAddress + 0xB1A7E9), &_1, sizeof(_1), NULL);
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
		ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xA82020 + 0x9900C), &AddrP1, sizeof(AddrP1), NULL);
		AddrP1 -= 0x80000000;
		AddrP1 += baseAddress + 0xA82020;

		// handle all message reading and writing
		updateNetwork();

		// Check to see if you are in the track selection menu
		bool inTrackSelection = false;
		ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB0F8AC), &inTrackSelection, sizeof(inTrackSelection), 0);

		// if you're in the track selection menu
		if (inTrackSelection)
		{
			updateTrackSelection();

			// Restart the loop, don't proceed
			continue;
		}

		// GameState
		// 2 = loading screen
		// 10 = some menus, intro of race (including traffic lights)
		// 11 = racing

		// Read gameStateCurr
		ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xB1A871), &gameStateCurr, sizeof(gameStateCurr), 0);

		// when you're in the loading screen
		if (gameStateCurr == 2)
		{
			// handle characters and LODs
			updateLoadingScreen();

			// restart the loop
			continue;
		}



		// Read Game Timer
		int timer = 0;
		ReadProcessMemory(handle, (PBYTE*)(baseAddress + 0xA82020 + 0x96B20 + 0x1cec), &timer, sizeof(int), 0);

		if (
			timer > 10 &&

			(gameStateCurr == 11 || gameStateCurr == 10)
			)
		{
			inGame = true;
		}

		if (inGame)
		{
			updateRace();
		}
	}

	// delete everything we initialized
	SDLNet_FreeSocketSet(socketSet);
	SDLNet_TCP_Close(mySocket);
	SDLNet_Quit();
	return 0;
}