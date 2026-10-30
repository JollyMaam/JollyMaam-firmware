#include "transport_scsi.h"
#include "transport_jieli.h"
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <setupapi.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Buffered SCSI pass-through on a caller-owned Windows disk/interface handle.
 * Return zero for completed I/O; inspect SCSI status and actual length separately.
 * Return -1 with GetLastError on invalid arguments or OS failure. No retry or
 * handle close occurs here. Buffered I/O avoids buffer-alignment requirements. */
int jm_scsi_win32_transfer(void *handle, const uint8_t *cdb, size_t cdb_size,
                            enum jm_scsi_direction direction, uint8_t *data,
                            size_t size, unsigned timeout_ms, struct jm_scsi_result *result) {
  if (!cdb || !result || !cdb_size || cdb_size>16 || size>0x7fffffff ||
      !timeout_ms || (size && !data) || direction<JM_SCSI_NONE || direction>JM_SCSI_OUT ||
      (size && direction==JM_SCSI_NONE)) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
  size_t offset=sizeof(SCSI_PASS_THROUGH)+32, total=offset+size;
  uint8_t *packet=calloc(1,total);
  if (!packet) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return -1; }
  SCSI_PASS_THROUGH *h=(SCSI_PASS_THROUGH *)packet;
  h->Length=sizeof *h; h->CdbLength=(UCHAR)cdb_size;
  h->SenseInfoLength=32; h->SenseInfoOffset=sizeof *h;
  h->DataIn=!size?SCSI_IOCTL_DATA_UNSPECIFIED:direction==JM_SCSI_IN?SCSI_IOCTL_DATA_IN:SCSI_IOCTL_DATA_OUT;
  h->DataTransferLength=(ULONG)size; h->DataBufferOffset=offset;
  h->TimeOutValue=timeout_ms/1000+(timeout_ms%1000!=0);
  memcpy(h->Cdb,cdb,cdb_size);
  if (size && direction==JM_SCSI_OUT) memcpy(packet+offset,data,size);
  DWORD got=0;
  int ok=DeviceIoControl(handle,IOCTL_SCSI_PASS_THROUGH,packet,(DWORD)total,packet,(DWORD)total,&got,NULL);
  DWORD error=GetLastError();
  if (ok && (got<sizeof *h || h->DataTransferLength>size ||
      (direction==JM_SCSI_IN && size && (got<offset || h->DataTransferLength>got-offset)))) {
    ok=0; error=ERROR_INVALID_DATA;
  }
  if (ok) {
    struct jm_scsi_result r; memset(&r,0,sizeof r);
    r.transferred=h->DataTransferLength; r.status=h->ScsiStatus;
    if (got>=sizeof *h+32) { memcpy(r.sense,packet+sizeof *h,32); r.sense_size=32; }
    if (direction==JM_SCSI_IN && r.transferred) memcpy(data,packet+offset,r.transferred);
    *result=r;
  }
  free(packet);
  if (!ok) { SetLastError(error); return -1; }
  return 0;
}

/* Enumerate disk interfaces using registry metadata only, without opening
 * disks or sending SCSI requests. Accept known Jieli UBOOT hardware IDs.
 * Callback paths are borrowed during the visit. Product identity still needs
 * protocol probing. Return candidate count or -1 on enumeration failure. */
int jm_jieli_candidates(jm_jieli_candidate_visitor visit, void *user) {
  static const GUID disk={0x53f56307,0xb6bf,0x11d0,{0x94,0xf2,0x00,0xa0,0xc9,0x1e,0xfb,0x8b}};
  if (!visit) return -1;
  HDEVINFO set=SetupDiGetClassDevsA(&disk,NULL,NULL,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
  if (set==INVALID_HANDLE_VALUE) return -1;
  int count=0;
  for (DWORD index=0;;++index) {
    SP_DEVICE_INTERFACE_DATA item; memset(&item,0,sizeof item); item.cbSize=sizeof item;
    if (!SetupDiEnumDeviceInterfaces(set,NULL,&disk,index,&item)) {
      if (GetLastError()!=ERROR_NO_MORE_ITEMS) count=-1;
      break;
    }
    DWORD size=0;
    SetupDiGetDeviceInterfaceDetailA(set,&item,NULL,0,&size,NULL);
    if (size<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) || size>65536) continue;
    SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail=calloc(1,size);
    if (!detail) { count=-1; break; }
    detail->cbSize=sizeof *detail;
    SP_DEVINFO_DATA info; memset(&info,0,sizeof info); info.cbSize=sizeof info;
    char ids[4096]={0};
    int ok=SetupDiGetDeviceInterfaceDetailA(set,&item,detail,size,NULL,&info) &&
      SetupDiGetDeviceRegistryPropertyA(set,&info,SPDRP_HARDWAREID,NULL,(BYTE *)ids,sizeof ids-1,NULL);
    enum jm_jieli_chip chip=JM_JIELI_BR23;
    if (ok) {
      for (size_t i=0;i<sizeof ids;++i) ids[i]=(char)toupper((unsigned char)ids[i]);
      ok=strstr(ids,"PROD_UBOOT1.00")!=NULL;
      if (strstr(ids,"VEN_BR23")) chip=JM_JIELI_BR23;
      else if (strstr(ids,"VEN_BD19")) chip=JM_JIELI_BD19;
      else ok=0;
    }
    int stop=0;
    if (ok) { ++count; stop=visit(user,detail->DevicePath,chip); }
    free(detail);
    if (stop) break;
  }
  SetupDiDestroyDeviceInfoList(set); return count;
}
