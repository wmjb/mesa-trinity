#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
int main(void){
  int fd=open("/dev/dri/card1",O_RDWR);
  drmModeRes *r=drmModeGetResources(fd);
  for(int i=0;i<r->count_connectors;i++){
    drmModeConnector *c=drmModeGetConnector(fd,r->connectors[i]);
    if(!c||c->connection!=DRM_MODE_CONNECTED){if(c)drmModeFreeConnector(c);continue;}
    printf("connector %u type=%u modes=%d\n",c->connector_id,c->connector_type,c->count_modes);
    for(int j=0;j<c->count_props;j++){
      drmModePropertyRes *p=drmModeGetProperty(fd,c->props[j]);
      if(!p) continue;
      printf("   %-24s = %llu",p->name,(unsigned long long)c->prop_values[j]);
      if(p->flags & DRM_MODE_PROP_ENUM)
        for(int k=0;k<p->count_enums;k++)
          if(p->enums[k].value==c->prop_values[j]) printf(" (%s)",p->enums[k].name);
      printf("\n");
      drmModeFreeProperty(p);
    }
    drmModeFreeConnector(c);
  }
  return 0;
}
