// Winsock tutorial
// https://docs.microsoft.com/en-us/windows/win32/winsock/finished-server-and-client-code

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

#include "Scanner.h"

// Can be negative
long long int baseAddress;
HANDLE handle;

bool pressingF9 = false;
bool pressingF10 = false;
bool isHost = false;

#define TEST_DEBUG 1

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
bool startLine_wait = true;
#define MAX_PLAYERS 4
bool trackSel_wait = true;
bool trackSel_waitForClients[MAX_PLAYERS];

unsigned int AddrP1 = 0;
unsigned char gameStateCurr; // 0x161A871
unsigned char weaponPrev; // relative to posX
unsigned char weaponCurr; // relative to posX
unsigned char trackID; // 0x163671A
unsigned char trackVideoID; // 0x16379C8
unsigned char LevelOfDetail; // 0xB0F85C
unsigned long long menuState;
unsigned char numPlayers = 0;
unsigned char myDriverIndex = 0; // will never change on server

int aiNavBackup[3] = { 0,0,0 };
uintptr_t ePSXeModule = 0;

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

// copied from here https://stackoverflow.com/questions/5891811/generate-random-number-between-1-and-3-in-c
int roll(int min, int max)
{
	// x is in [0,1[
	double x = rand() / static_cast<double>(RAND_MAX + 1);

	// [0,1[ * (max - min) + min is in [min,max[
	int that = min + static_cast<int>(x * (max - min));

	return that;
}

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

void
init_sockaddr(struct sockaddr_in* name,
	const char* hostname,
	unsigned short port)
{
	struct hostent* hostinfo;

	name->sin_family = AF_INET;
	name->sin_port = htons(port);
	
	hostinfo = gethostbyname(hostname);
	
	if (hostinfo == NULL)
	{
		printf("Unknown host\n");
	}

	name->sin_addr = *(struct in_addr*) hostinfo->h_addr;

	printf("URL converts to IP: %d.%d.%d.%d\n",
		name->sin_addr.S_un.S_un_b.s_b1,
		name->sin_addr.S_un.S_un_b.s_b2,
		name->sin_addr.S_un.S_un_b.s_b3,
		name->sin_addr.S_un.S_un_b.s_b4);
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
	srand((unsigned int)time(NULL));

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
	unsigned char ctrData[12] = { 0x71, 0xDC, 0x01, 0x0C, 0x00, 0x00, 0x00, 0x00, 0xD0, 0xF9, 0x00, 0x0C };

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

	// name of the server that you connect to
	char* serverName = nullptr;

	// set bool
	// set max variables
	// get server name
	system("cls");
	printf("Enter IP or URL: ");
	serverName = (char*)malloc(80);
	scanf("%79s", serverName);

	system("cls\n");

	WSADATA wsaData;
	int iResult;
	struct addrinfo* result = NULL,
		* ptr = NULL,
		hints;

	// Initialize Winsock
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf("WSAStartup failed with error: %d\n", iResult);
		system("pause");
		exit(0);
	}

	// sockAddr
	struct sockaddr_in socketIn;
	init_sockaddr(&socketIn, serverName, 1234);

	// Create a SOCKET for connecting to server
	CtrMain.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	// Setup the TCP listening socket
	int res = connect(CtrMain.socket, (struct sockaddr*) & socketIn, sizeof(socketIn));

	freeaddrinfo(result);

	if (CtrMain.socket == INVALID_SOCKET) {
		printf("Unable to connect to server!\n");
		WSACleanup();
		system("pause");
		exit(0);
	}

	// set socket to non-blocking
	unsigned long nonBlocking = 1;
	iResult = ioctlsocket(CtrMain.socket, FIONBIO, &nonBlocking);

	printf("Connected to server\n\n");

	printf("Choose a character (hit F10 for random)\n");
	printf("\n");

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

	for (int i = 0; i < MAX_PLAYERS; i++)
		trackSel_waitForClients[i] = true;
}

void disableAI_RenameThis(int aiNumber)
{
	unsigned int AddrAI = AddrP1;
	AddrAI -= 0x670 * aiNumber;

	// This stops the AI from proceding with the NAV system,
	// but with this, the AI's values still get set to the 
	// values that the NAV system wants the AI to have at the starting line
	int _30 = 30;
	WriteMem(AddrAI + 0x604, &_30, sizeof(int));
}

void drawAI(int aiNumber, int* netPos)
{
	unsigned int AddrAI = AddrP1;
	AddrAI -= 0x670 * aiNumber;

	/*WriteMem(AddrAI + 0x2d4, &netPos[0], sizeof(int));
	WriteMem(AddrAI + 0x2d8, &netPos[1], sizeof(int));
	WriteMem(AddrAI + 0x2dC, &netPos[2], sizeof(int));*/

	WriteMem(AddrAI + 0x5f0, &netPos[0], sizeof(int));
	WriteMem(AddrAI + 0x5f4, &netPos[1], sizeof(int));
	WriteMem(AddrAI + 0x5f8, &netPos[2], sizeof(int));
}

void updateNetwork()
{
		unsigned char type = 0xFF;
		unsigned char size = 0xFF;

		// Get a message
		memset(&CtrMain.recvBuf, 0xFF, sizeof(Message));
		receivedByteCount = recv(CtrMain.socket, (char*)&CtrMain.recvBuf, sizeof(Message), 0);

		if (receivedByteCount == -1)
			goto SendToServer;
		//printf("Error %d\n", WSAGetLastError());

		if (receivedByteCount == 0)
		{
			printf("Disconnected\n");
			system("pause");
			exit(0);
		}

		if (receivedByteCount < CtrMain.recvBuf.size)
		{
			//printf("Bug! -- Tag: %d, recvBuf.size: %d, recvCount: %d\n",
			//	recvBuf.type, recvBuf.size, receivedByteCount);

			goto SendToServer;
		}

		// We can confirm we have a valid message

		// dont parse same message twice
		if (CtrMain.recvBuf.size == CtrMain.recvBufPrev.size)
			if (memcmp(&CtrMain.recvBuf, &CtrMain.recvBufPrev, CtrMain.recvBuf.size) == 0)
				goto SendToServer;

		// make a backup
		memcpy(&CtrMain.recvBufPrev, &CtrMain.recvBuf, sizeof(Message));

		type = CtrMain.recvBuf.type;
		size = CtrMain.recvBuf.size;

		// 0 means track ID, or telling you that you're the host
		if (type == 0)
		{
			if (size == 2)
			{
				isHost = true;
				printf("You are host, so you pick track\n");
				printf("Press F9, then Up, for Battle Maps\n");
				printf("Press F10 for random track\n");
				goto SendToServer;
			}

			// track, driver index, num characters, characters

			// parse message
			char trackByte = CtrMain.recvBuf.data[0];
			myDriverIndex =  CtrMain.recvBuf.data[1];
			numPlayers =     CtrMain.recvBuf.data[2];

#if TEST_DEBUG
			printf("Recv -- Tag: %d, size: %d, -- %d %d %d", type, size, trackByte, myDriverIndex, numPlayers);
			for (int i = 0; i < numPlayers-1; i++)
			{
				printf(" %d", CtrMain.recvBuf.data[3 + i]);
			}
			printf("\n");
#endif

			char zero = 0;
			char ogTrackByte = 0;

			// Get characterID for this player
			// for characters 0 - 7:
			// CharacterID[i] : 0x1608EA4 + 2 * i
			for (int i = 0; i < numPlayers; i++)
			{
				// This character is in track selection
				trackSel_waitForClients[i] = false;

				characterIDs[i + 1] = CtrMain.recvBuf.data[3 + i];
			}

			// temporarily set this to true
			trackSel_wait = false;

			// check if they're all done
			for (int i = 0; i < numPlayers; i++)
				if (trackSel_waitForClients[i])
					trackSel_wait = true;

			// skip menu sync if you are host
			if (isHost) goto SendToServer;

			// close the lapRowSelector
			WriteMem(0x800B59AC, &zero, sizeof(char));

			// Get original track byte
			ReadMem(0x800B46FA, &ogTrackByte, sizeof(char));

			// set Text+Map address 
			WriteMem(0x800B46FA, &trackByte, sizeof(char));

			// set Video Address
			WriteMem(0x800B59A8, &trackByte, sizeof(char));

			// Set two variables to refresh the video
			short s_One = 1;
			WriteMem(0x800B59B8, &s_One, sizeof(short));
			WriteMem(0x800B59BA, &s_One, sizeof(short));
		}

		// 1 means lap row, NOT DONE
		else if (type == 1)
		{
			// open the lapRowSelector menu, 
			char one = 1;
			WriteMem(0x800B59AC, &one, sizeof(char));

			// convert to one byte
			char lapByte = CtrMain.recvBuf.data[0];
			WriteMem(0x8008D920, &lapByte, sizeof(lapByte));

#if TEST_DEBUG
			printf("Recv -- Tag: %d, size: %d, -- %d\n", type, size, lapByte);
#endif

			// change the spawn order

			// Client 0:       0 1 2 3 4 5 6 7
			// Client 1: 1 0       2 3 4 5 6 7
			// Client 2: 2 0 1       3 4 5 6 7
			// Client 3: 3 0 1 2       4 5 6 7

			// used to set your own spawn
			char zero = 0;

			// loop through all drivers prior to your index
			for (char i = 1; i <= myDriverIndex; i++)
			{
				char spawnValue = i - 1;

				WriteMem(0x80080F28 + i, &spawnValue, sizeof(char));
			}

			// set your own index
			WriteMem(0x80080F28, &myDriverIndex, sizeof(char));
		}

		// 2 means start loading, NOT DONE
		else if (type == 2)
		{
#if TEST_DEBUG
			printf("Recv -- Tag: %d, size: %d\n", type, size);
#endif

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
		else if (type == 3)
		{
#if TEST_DEBUG
			printf("Recv -- Tag: %d, size: %d, -- %d %d %d\n", type, size,
				*(int*)&CtrMain.recvBuf.data[0],
				*(int*)&CtrMain.recvBuf.data[4],
				*(int*)&CtrMain.recvBuf.data[8]);
#endif

			// This NEEDS to move somewhere else

			// If race is ready to start
			if (!startLine_wait)
			{
				// draw all AIs
				for (int i = 0; i < numPlayers - 1; i++)
				{
					drawAI(i + 1, (int*)&CtrMain.recvBuf.data[12 * i]);
				}
			}
		}

		// message 4 will not come from server

		// 5 means start race at traffic lights
		else if (type == 5)
		{
#if TEST_DEBUG
			printf("Recv -- Tag: %d, size: %d\n", type, size);
#endif

			startLine_wait = false;

			// set controller mode to 1P, remove error message
			char _1 = 1;
			WriteMem(0x800987C9, &_1, sizeof(_1));
		}

	SendToServer:

		if (CtrMain.sendBuf.type == 0xFF)
			return;

		// dont send the same message twice, 
		// or
		// To do: if server has not gotten prev message
		if (CtrMain.sendBuf.size == CtrMain.sendBufPrev.size)
			if (memcmp(&CtrMain.sendBuf, &CtrMain.sendBufPrev, CtrMain.sendBuf.size) == 0)
				return;

		// send a message to the client
		send(CtrMain.socket, (char*)&CtrMain.sendBuf, sizeof(Message), 0);

		// make a backup
		memcpy(&CtrMain.sendBufPrev, &CtrMain.sendBuf, sizeof(Message));

#if TEST_DEBUG

		type = CtrMain.sendBuf.type;
		size = CtrMain.sendBuf.size;

		if (type == 3)
		{
			int i1 = *(int*)&CtrMain.sendBuf.data[0];
			int i2 = *(int*)&CtrMain.sendBuf.data[4];
			int i3 = *(int*)&CtrMain.sendBuf.data[8];

			printf("Send -- Tag: %d, size: %d -- %d %d %d\n", type, size, i1, i2, i3);
		}

		if (type == 4)
		{
			// parse message
			char c1 = CtrMain.sendBuf.data[0];

			printf("Send -- Tag: %d, size: %d, -- %d\n", type, size, c1);
		}

		if (type == 5)
		{
			printf("Send -- Tag: %d, size: %d\n", type, size);
		}
#endif
}

void SendOnlinePlayersToRAM()
{
	// set number of players, prevent extra AIs from spawning
	WriteMem(0x8003B83C, &numPlayers, sizeof(numPlayers));

	// set number of icons (on the left of the screen)
	if(numPlayers < 4)
		WriteMem(0x800525A8, &numPlayers, sizeof(numPlayers));

	// put network characters into RAM
	for (unsigned char i = 1; i < numPlayers; i++)
	{
		char oneByte = (char)characterIDs[(int)i];
		WriteMem(0x80086E84 + 2 * i, &oneByte, sizeof(char)); // 4, for 2 shorts
	}
}

void HostShortcutKeys()
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

		// set Text+Map address 
		WriteMem(0x800B46FA, &trackByte, sizeof(char));

		// set Video Address
		WriteMem(0x800B59A8, &trackByte, sizeof(char));

		// Set two variables to refresh the video
		short s_One = 1;
		WriteMem(0x800B59B8, &s_One, sizeof(short));
		WriteMem(0x800B59BA, &s_One, sizeof(short));

		// Not sure if I want the "random track" button to automatically open
		// the lapRowSelector or not, but if we ever want it, here is the code

		// open the lapRowSelector
		//char one = 1;
		//WriteMem(0x800B59AC, &one, sizeof(char));

	}
}

void GetHostMenuState()
{
	// check if lapRowSelector is open
	bool lapRowSelectorOpen = false;
	ReadMem(0x800B59AC, &lapRowSelectorOpen, sizeof(bool));

	// if lap selector is closed
	if (!lapRowSelectorOpen)
	{
		// F9 and F10 keys
		HostShortcutKeys();

		// Get Track ID, send it to clients
		ReadMem(0x800B46FA, &trackID, sizeof(trackID));

		// track, driver index, num characters, characters

		CtrMain.sendBuf.type = 0;
		CtrMain.sendBuf.size = 4;

		CtrMain.sendBuf.data[0] = trackID;
		CtrMain.sendBuf.data[1] = (unsigned char)characterIDs[0];
		
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
			// wait for all at starting line
			startLine_wait = true;

			// Reset game frame counter to zero
			int zero = 0;
			WriteMem(0x80096B20 + 0x1cec, &zero, sizeof(int));

			inGame = false;

			// 2 means Start Loading
			CtrMain.sendBuf.type = 2;
			CtrMain.sendBuf.size = 2;
		}

		// if lap is being chosen
		else
		{
			// 0 -> 3 laps
			// 1 -> 5 laps
			// 2 -> 7 laps
			unsigned char lapRowSelected = 0;
			ReadMem(0x8008D920, &lapRowSelected, sizeof(lapRowSelected));

			// send info to all players
			CtrMain.sendBuf.type = 1;
			CtrMain.sendBuf.size = 3;
			CtrMain.sendBuf.data[0] = lapRowSelected;
		}
	}
}

void SendCharacterID()
{
	CtrMain.sendBuf.type = 4;
	CtrMain.sendBuf.size = 3;
	CtrMain.sendBuf.data[0] = (char)characterIDs[0];
}

void preparePositionMessage()
{
	Message* sendBuf;

	// Server sends to client
	// Client sends to server
	// 3 means Position Message
	
	// client sending to server

	sendBuf = &CtrMain.sendBuf;

	sendBuf->type = 3;
	sendBuf->size = 14;

	// Get Player 1 position
	// All players have 12-byte positions (4x3). 
	// Changing those coordinates will not move
	// the players. 
	ReadMem(AddrP1 + 0x2D4, (int*)&sendBuf->data[0], sizeof(int)*3);
}

void updateRace()
{
	for (int i = 1; i < numPlayers; i++)
		disableAI_RenameThis(i);

	// If not all racers are ready to start
	if (startLine_wait)
	{
		// Set the traffic lights to be above the screen
		// They are set to 3840 by default without modding
		short wait = 4500;
		WriteMem(0x8009882C, &wait, sizeof(short));

		// set controller mode to 0P mode, trigger error message
		char _0 = 0;
		WriteMem(0x800987C9, &_0, sizeof(_0));

		// change the error message
		WriteMem(0x800BC684, (char*)"waiting for players...", 23);

		// client "wants" to start
		CtrMain.sendBuf.type = 5;
		CtrMain.sendBuf.size = 2;
	}

	// if everyone is ready to start
	else
	{
		// set controller mode to 1P mode, disable error message
		// The message gets enabled lower in the code
		char _1 = 1;
		WriteMem(0x800987C9, &_1, sizeof(_1));

		// Send your player's position to the server (or client)
		// If you are the client, this sends to server, DONE
		// If you are the server, this sends to one client, NOT DONE
		preparePositionMessage();
	}
}

int main(int argc, char** argv)
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
	while (true)
	{
		// good for your CPU
		Sleep(5);

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
				unsigned char _80 = 0x80;
				unsigned char _C0 = 0xC0;

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
				unsigned char fakeCrashData[8] = { 0x00, 0x01, 0xAE, 0x00, 0x06, 0x0E, 0x0D, 0x0F };
				WriteMem(0x800B4F28, &fakeCrashData[0], 8);

				// Change 3P's Crash Icon to 1P's Oxide Icon
				unsigned char oxideData[10] = { 0x40, 0x01, 0xAE, 0x00, 0x07, 0x0F, 0x0E, 0x0B, 0x0F, 0x00 };
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

			// Get characterID for this player
			ReadMem(0x80086E84, &characterIDs[0], sizeof(short));

			// wait for all players before continuing
			if (isHost)
			{
				// copy host menu state to clients
				GetHostMenuState();

				//if (trackSel_wait)
				if(0)
				{
					// set controller mode to 0P mode, trigger error message
					char _0 = 0;
					WriteMem(0x800987C9, &_0, sizeof(_0));

					// change the error message
					WriteMem(0x800BC684, (char*)"waiting for players...", 23);
				}

				else
				{
					// set controller mode to 1P mode, remove error message
					char _1 = 1;
					WriteMem(0x800987C9, &_1, sizeof(_1));
				}
			}

			else
			{
				SendCharacterID();
			}

			// Restart the loop, don't proceed
			continue;
		}

		// GameState
		// 2 = loading screen
		// 10 = some menus, intro of race (including traffic lights)
		// 11 = racing

		// Read gameStateCurr
		ReadMem(0x80098851, &gameStateCurr, sizeof(gameStateCurr));

		// when you're in the loading screen
		if (gameStateCurr == 2)
		{
			// constantly write these values,
			// to make sure the right characters are loaded
			SendOnlinePlayersToRAM();

			// restart the loop
			continue;
		}


		// Read Game Timer
		int timer = 0;
		ReadMem(0x80096B20 + 0x1cec, &timer, sizeof(int));

		if (!inGame)
		{
			if (
				timer > 30 &&
				!inTrackSelection &&
				inCharSelection != 18100 &&
				(gameStateCurr == 11 || gameStateCurr == 10)
				)
			{
				// see if the intro cutscene is playing
				// becomes 0 when traffic lights should show
				char introAnimState;
				ReadMem(0x801FFDDE, &introAnimState, sizeof(char));

				// if the intro animation is done
				if (introAnimState == 0)
				{
					inGame = true;

					// used by host
					trackSel_wait = true;

					for (int i = 0; i < numPlayers; i++)
						trackSel_waitForClients[i] = true;
				}
			}
		}

		if(inGame)
		{
			// This commented-out code wont work in ePSXe
			// due to how ASM is cached, but it works in 
			// Bizhawk, should it be used at all?

			/*int playerFlags = 0;
			ReadMem(AddrP1 + 0x2c8, &playerFlags, sizeof(int));
			if ((playerFlags & 0x2000000) != 0)
			{
				// Enable AIs properly to take over
				EnableAI();
			}*/

			updateRace();
		}
	}

	return 0;
}