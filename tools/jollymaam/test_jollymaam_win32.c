#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <assert.h>
#include <string.h>
static unsigned mode, calls;
static BOOL WINAPI fake_ioctl(HANDLE handle, DWORD code, LPVOID input, DWORD input_size,
                               LPVOID output, DWORD output_size, LPDWORD got, LPOVERLAPPED overlap) {
  (void)handle; (void)input_size; (void)output_size; (void)overlap;
  assert(code==IOCTL_SCSI_PASS_THROUGH && input==output); ++calls;
  SCSI_PASS_THROUGH *h=input;
  assert(h->CdbLength==6 && h->Cdb[0]==0x12 && h->TimeOutValue==2);
  if(mode==1) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
  if(mode==2) h->DataTransferLength+=1;
  if(mode==3) h->DataTransferLength-=1;
  if(mode==4) h->ScsiStatus=2;
  if(h->DataIn==SCSI_IOCTL_DATA_IN && mode!=2)
    memset((BYTE *)output+h->DataBufferOffset,0xa5,h->DataTransferLength);
  *got=(DWORD)h->DataBufferOffset+h->DataTransferLength;
  if(mode==5) *got=0;
  return TRUE;
}
#define DeviceIoControl fake_ioctl
#include "../../src/lib/transport_win32.c"
#undef DeviceIoControl
int main(void) {
  uint8_t cdb[16]={0x12}, data[36]; struct jm_scsi_result result;
  assert(jm_scsi_win32_transfer(NULL,cdb,17,JM_SCSI_IN,data,36,1501,&result)==-1);
  assert(GetLastError()==ERROR_INVALID_PARAMETER && !calls);
  for(mode=0;mode<=5;++mode) {
    memset(&result,0x55,sizeof result);
    int r=jm_scsi_win32_transfer(NULL,cdb,6,JM_SCSI_IN,data,36,1501,&result);
    if(mode==1) assert(r==-1 && GetLastError()==ERROR_ACCESS_DENIED);
    else if(mode==2 || mode==5) assert(r==-1 && GetLastError()==ERROR_INVALID_DATA);
    else {
      assert(!r && result.transferred==(mode==3?35:36));
      assert(result.status==(mode==4?2:0) && data[0]==0xa5);
    }
  }
  mode=0;
  assert(!jm_scsi_win32_transfer(NULL,cdb,6,JM_SCSI_OUT,data,36,1501,&result));
  return 0;
}
