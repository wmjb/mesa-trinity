/* Export a gbm bo as a dmabuf and import it back: the exact round trip DRI3
 * asks the driver to do when Xwayland makes a pixmap from a client buffer. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#include <xf86drm.h>
int main(void){
  int fd=open("/dev/dri/renderD128",O_RDWR);
  if(fd<0){perror("open");return 1;}
  struct gbm_device *g=gbm_create_device(fd);
  if(!g){printf("gbm_create_device failed\n");return 1;}
  struct gbm_bo *bo=gbm_bo_create(g,256,256,GBM_FORMAT_ARGB8888,GBM_BO_USE_RENDERING);
  if(!bo){printf("gbm_bo_create failed\n");return 1;}
  int stride=gbm_bo_get_stride(bo);
  uint64_t mod=gbm_bo_get_modifier(bo);
  int dfd=gbm_bo_get_fd(bo);
  printf("exported: stride=%d modifier=0x%llx fd=%d\n",stride,(unsigned long long)mod,dfd);
  if(dfd<0){printf("export FAILED\n");return 1;}

  /* raw prime import, the step grate_bo_import does first */
  uint32_t h=0;
  int err=drmPrimeFDToHandle(fd,dfd,&h);
  printf("drmPrimeFDToHandle: err=%d errno=%d (%s) handle=%u\n",err,errno,strerror(errno),h);
  off_t sz=lseek(dfd,0,SEEK_END);
  printf("fd size via lseek: %lld (errno %d %s)\n",(long long)sz,errno,strerror(errno));

  struct gbm_import_fd_modifier_data d={.width=256,.height=256,.format=GBM_FORMAT_ARGB8888,
     .num_fds=1,.fds={dfd},.strides={stride},.offsets={0},.modifier=mod};
  struct gbm_bo *ib=gbm_bo_import(g,GBM_BO_IMPORT_FD_MODIFIER,&d,GBM_BO_USE_RENDERING);
  printf("gbm_bo_import(FD_MODIFIER): %s\n", ib?"OK":"FAILED");
  return 0;
}
