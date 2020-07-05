
#include <pspsdk.h>
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspsyscon.h>
#include <pspdisplay.h>

#include <stdio.h>
#include <string.h> // memcpy

// networking
#include <psputility.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>

PSP_MODULE_INFO("NikoTest", 0x1000, 0, 1);
PSP_MAIN_THREAD_ATTR(0);

SceCtrlData pad; // Structure which represents the PSP controls, we'll use it to interact with the PSP

// PSP
// base: 0x8800000
// size: 0x1800000
// tail: 0xA000000

// PS1
// base: 0x9800000
// size: 0x0200000
// tail: 0x9A00000

const int pspBaseAddr = 0x8800000;
const int ps1BaseAddr = 0x9800000;


void WriteMem(unsigned int psxAddr, void* pspAddr, int size)
{
	// remove 0x80 prefix
	psxAddr = psxAddr & 0xFFFFFF;
	
	// add ps1BaseAddr
	psxAddr = psxAddr + ps1BaseAddr;
	
	// copy
	memcpy((void*)psxAddr,(void*)pspAddr,size);
}

void ReadMem(unsigned int psxAddr, void* pspAddr, int size)
{
	// remove 0x80 prefix
	psxAddr = psxAddr & 0xFFFFFF;
	
	// add ps1BaseAddr
	psxAddr = psxAddr + ps1BaseAddr;
	
	// copy
	memcpy((void*)pspAddr,(void*)psxAddr,size);
}

// ripper roo and oxide
char characterIDs[8] = {0, 10, 15, 0, 0, 0, 0, 0};

void SendOnlinePlayersToRAM()
{
	unsigned char numPlayers = 3;
	
	// set number of players, prevent extra AIs from spawning
	WriteMem(0x8003B83C, &numPlayers, sizeof(numPlayers));

	// set number of icons (on the left of the screen)
	if(numPlayers < 4)
		WriteMem(0x800525A8, &numPlayers, sizeof(numPlayers));

	// put network characters into RAM
	for (unsigned char i = 1; i < /*numPlayers*/8; i++)
	{
		char oneByte = (char)characterIDs[(int)i];
		WriteMem(0x80086E84 + 2 * i, &oneByte, sizeof(char)); // 4, for 2 shorts
	}
}

void DisableAI()
{
	// Stop AI system from writing position data
	int zero = 0;
	WriteMem(0x80015538, &zero, sizeof(int));
	WriteMem(0x80015560, &zero, sizeof(int));
	WriteMem(0x80015594, &zero, sizeof(int));
}

void disableAI_RenameThis(int aiNumber)
{
	unsigned int AddrP1 = 0;
	ReadMem(0x8009900C, &AddrP1, sizeof(AddrP1));
	
	unsigned int AddrAI = AddrP1;
	AddrAI -= 0x670 * aiNumber;

	// This stops the AI from proceding with the NAV system,
	// but with this, the AI's values still get set to the 
	// values that the NAV system wants the AI to have at the starting line
	int _30 = 30;
	WriteMem(AddrAI + 0x604, &_30, sizeof(int));
}

int sock = -1;
int test[100];
int* currTest;
	
void
init_sockaddr(struct sockaddr_in* name,
	const char* hostname,
	unsigned short port)
{

	name->sin_family = PF_INET;
	name->sin_port = htons(port);
	
	int rid = -1;
	char buf[1024];
	
	*currTest++ = sceNetResolverCreate(&rid, buf, sizeof(buf));
	*currTest++ = sceNetResolverStartNtoA(rid, hostname, &name->sin_addr, 2, 3);
}

/* Connect to an access point */
int connect_to_apctl(int config)
{
	int err;

	/* Connect using the first profile */
	err = sceNetApctlConnect(config);
	if (err != 0)
	{
		return 0;
	}

	while (1)
	{
		int state;
		err = sceNetApctlGetState(&state);
		if (err != 0)
		{
			break;
		}
		if (state == 4)
			break;  // connected with static IP

		// wait a little before polling again
		sceKernelDelayThread(50*1000); // 50ms
	}

	if(err != 0)
	{
		return 0;
	}

	return 1;
}

	
int main_thread(SceSize args, void* argp)
{
	pspDebugScreenInit();
	
	bool loading = false;
	bool inGame = false;
	
	while(1)
	{
		sceCtrlPeekBufferPositive(&pad, 1);
		if (pad.Buttons != 0)
		{
			if (pad.Buttons & PSP_CTRL_SELECT)
			{
				if(sock < 0)
				{	
					currTest = &test[0];
			
					// http://www.darkhaven3.com/psp-dev/wiki/index.php/Enabling_Networking
					//*currTest++ = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
					//*currTest++ = sceUtilityLoadNetModule(PSP_NET_MODULE_INET); 
			
					*currTest++ = _PSP_FW_VERSION;
			
					//#if _PSP_FW_VERSION >= 200  
					//	*currTest++ = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);  
					//	*currTest++ = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);  
					//#else  
						*currTest++ = pspSdkLoadInetModules();  
					//#endif  
			
					// Create a pool of 128 kilobytes
					*currTest++ = sceNetInit(128 * 1024, 42, 0, 42, 0);
					// Initialize INet
					*currTest++ = pspSdkInetInit();
					// Initialize Access Point Control
					*currTest++ = sceNetApctlInit(0x10000, 48);
			
					
					
					*currTest++ = connect_to_apctl(1);
			
					struct sockaddr_in socketIn;
					init_sockaddr(&socketIn, "192.168.1.138", 1234);
					
					sock = socket(PF_INET, SOCK_STREAM, 0);
					*currTest++ = sock;
					
					*currTest++ = connect(sock, (struct sockaddr*) &socketIn, sizeof(socketIn));
				}
			}
			
			if (pad.Buttons & PSP_CTRL_LTRIGGER)
			{
				// Disable AIs so that humans can be injected
				DisableAI();

				unsigned char zero = 0;
				
				// close the lapRowSelector
				WriteMem(0x800B59AC, &zero, sizeof(char));
	
				// oxide station
				char trackByte = 15;
	
				// set Text+Map address 
				WriteMem(0x800B46FA, &trackByte, sizeof(char));
	
				// set Video Address
				WriteMem(0x800B59A8, &trackByte, sizeof(char));
	
				// Set two variables to refresh the video
				short s_One = 1;
				WriteMem(0x800B59B8, &s_One, sizeof(short));
				WriteMem(0x800B59BA, &s_One, sizeof(short));
				
				// Ja ra, return asm, 
				// disable weapons for players and enemies
				int jaRa = 0x3e00008;
				WriteMem(0x8006540C, &jaRa, sizeof(int));
			
				// Patch the first if-statement of FUN_8003282c
				// Allow 4 characters to load in high LOD
				int zero2 = 0;
				WriteMem(0x80032840, &zero2, sizeof(int));
			
				short HighMpk = 0x00F2;
				WriteMem(0x80032888, &HighMpk, sizeof(short));
				WriteMem(0x800328A4, &HighMpk, sizeof(short));
				WriteMem(0x800328C0, &HighMpk, sizeof(short));
			}
			
			if(pad.Buttons & PSP_CTRL_RTRIGGER)
			{
				// open the lapRowSelector menu, 
				char one = 1;
				WriteMem(0x800B59AC, &one, sizeof(char));
			}
			
			if(pad.Buttons & PSP_CTRL_SELECT)
			{
				char one = 1;
				char two = 2;
	
				// set menuA to 2 and menuB to 1,
				WriteMem(0x800B59AE, &two, sizeof(char));
				WriteMem(0x800B59B0, &one, sizeof(char));

			
				loading = true;
			}
		}
		
		if(loading)
		{
			SendOnlinePlayersToRAM();
						
			// temporary
			#if 1
			
			// see if the intro cutscene is playing
			// becomes 0 when traffic lights should show
			char introAnimState;
			ReadMem(0x801FFDDE, &introAnimState, sizeof(char));

			// if the intro animation is done
			if (introAnimState == 0)
			{
				loading = false;
				inGame = true;
			}
			
			#endif
		}
		
		if(inGame)
		{
			unsigned char numPlayers = 3;
			
			for (int i = 1; i < numPlayers; i++)
				disableAI_RenameThis(i);
		}	
	
		unsigned char numKarts = 0;
		ReadMem(0x8003B83C, &numKarts, sizeof(numKarts));

		unsigned char numIcons = 0;
		ReadMem(0x800525A8, &numIcons, sizeof(numIcons));
	
		pspDebugScreenSetXY(0, 0);
		pspDebugScreenPrintf("karts: %02X\n", numKarts);
		pspDebugScreenPrintf("icons: %02X\n", numIcons);
		for(int* t = &test[0]; t < currTest; t++)
		{
			pspDebugScreenPrintf("%08X\n", *t);
		}
		sceDisplayWaitVblankStart();
		
		if(pad.Buttons & PSP_CTRL_HOME)
			break;
	}
	
	sceKernelExitDeleteThread(0);
    return 0;
}

// start a thread for the program
extern "C"{
int module_start(SceSize args, void *argp)
{
	int thid1 = sceKernelCreateThread("NikoTest", main_thread, 10, 0x2000, 0, NULL);
	if(thid1 >= 0)
		sceKernelStartThread(thid1, args, argp);
	return 0;
}
}

// terminate the plugin
extern "C"{
int module_stop (void) {
	return 0;
}
}
