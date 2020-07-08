
#include <iostream>
#include <sys/uio.h>

pid_t pid = 3774;

void WriteMem(ulong psxAddr, ulong linuxAddr, int size)
{
	struct iovec local[1];
    struct iovec remote[1];
	
    local[0].iov_base = (void *)linuxAddr;
    local[0].iov_len = size;
    remote[0].iov_base = (void *)psxAddr;
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
    remote[0].iov_base = (void *)psxAddr;
    remote[0].iov_len = size;
	
    process_vm_readv(pid, local,1,remote,1,0);
    
    if (errno == EPERM) 
		printf("Try again with sudo\n");
}

int main() 
{	
	short characterID;
	ReadMem(0x7f64afe66e84, (ulong)&characterID, 2);
	
	// if your cursor is on n gin
	if(characterID == 0x4)
	{
		// change cursor to penta
		short penta = 13;
		WriteMem(0x7f64afe66e84, (ulong)&penta, 2);
	}
}



