
#include <iostream>
#include <sys/uio.h>
#include <vector>
#include "pmparser.h"

pid_t pid = 1711;
ulong baseAddress = 0;

void WriteMem(ulong psxAddr, ulong linuxAddr, int size)
{
	struct iovec local[1];
    struct iovec remote[1];
	
    local[0].iov_base = (void *)linuxAddr;
    local[0].iov_len = size;
    remote[0].iov_base = (void *)(baseAddress + psxAddr);
    remote[0].iov_len = size;
	
    process_vm_writev(pid, local,1,remote,1,0);
    
    if (errno == EPERM) 
		printf("Try again with sudo\n");
}

void ReadMem(ulong psxAddr, ulong linuxAddr, int size)
{
    struct iovec local[1];
    struct iovec remote[1];	
	
    local[0].iov_base = (void *)linuxAddr;
    local[0].iov_len = size;
    remote[0].iov_base = (void *)(baseAddress + psxAddr);
    remote[0].iov_len = size;
	
    process_vm_readv(pid, local,1,remote,1,0);
    
    if (errno == EPERM) 
		printf("Try again with sudo\n");
}

void FindBaseAddr()
{
	// Shows at PSX address 0x8003C62C, only in CTR 94426
	unsigned char ctrData[12] = { 0x71, 0xDC, 0x01, 0x0C, 0x00, 0x00, 0x00, 0x00, 0xD0, 0xF9, 0x00, 0x0C };
	
	procmaps_iterator* maps = pmparser_parse(pid);
	
	std::vector<ulong> startAddr;
	std::vector<ulong> endAddr;

	//iterate over areas
	procmaps_struct* maps_tmp=NULL;
	
	while( (maps_tmp = pmparser_next(maps)) != NULL)
	{
		// if this module is too small to hold 2mb psx RAM,
		// skip this module and go to the next
		if(maps_tmp->length < 2048000) continue;
		
		startAddr.push_back((ulong)maps_tmp->addr_start);
		endAddr.push_back((ulong)maps_tmp->addr_end - 1024000);
		
		//pmparser_print(maps_tmp,0);
	}
	
	//mandatory: should free the list
	pmparser_free(maps);
	
	ulong searchBase = 0;
	
	while(true)
	{
		// search all modules at the same time
		for(int i = 0; i < startAddr.size(); i++)
		{
			
			ulong currSearch = startAddr[i] + searchBase;
		
			if(currSearch > endAddr[i])
			{
				startAddr.erase(startAddr.begin() + i);
				endAddr.erase(endAddr.begin() + i);
				i--;
				
				if(startAddr.size() == 0)
				{
					printf("Failed to find CTR BaseAddr\n");
					exit(0);
				}
				
				continue;
			}
		
			unsigned char readData[12];
			ReadMem(currSearch, (ulong)readData, 12);
			
			if(memcmp((void*)readData, (void*)ctrData, 12) == 0)
			{
				baseAddress = currSearch;
				goto FinishBaseAddressSearch;
			}
			
			if((searchBase % 1024000) == 0)
			{
				printf("Progress: %p\n", (void*)searchBase);
			}
			
			searchBase++;
		}
	}
	
FinishBaseAddressSearch:
if(baseAddress != 0)
printf("BaseAddress = %p\n",(void*)baseAddress);
}

int main() 
{
	baseAddress = 0;
	
// This wont work
#if 0	
	FindBaseAddr();
	
// This works but kinda annoying
#else
	printf("Use GameConqueror to open your emulator\n");
	printf("And search for the following byte array:\n");
	printf("71 DC 01 0C 00 00 00 00 D0 F9 00 0C\n\n");

	std::string str;
	std::cout << "Enter address (include 0x before hex): ";
	std::getline (std::cin,str);
	unsigned long ul = std::stoul (str,nullptr,0);
	baseAddress = ul;
#endif

	baseAddress -= 0x8003C62C;
	
	short characterID;
	ReadMem(0x80086e84, (ulong)&characterID, 2);
	
	// if your cursor is on n gin
	if(characterID == 0x4)
	{
		// change cursor to penta
		short penta = 13;
		WriteMem(0x80086e84, (ulong)&penta, 2);
	}
}



