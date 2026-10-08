#include <stdio.h>
int guest_loader_dtb_node_has_prop(const unsigned char*, unsigned, const char*, const char*);
int main(int c, char**v){ static unsigned char b[16384]; FILE*f=fopen(v[1],"rb");
 size_t n=fread(b,1,sizeof(b),f); fclose(f);
 printf("  不存在的节点       -> %d\n", guest_loader_dtb_node_has_prop(b,(unsigned)n,"nope@0","no-map"));
 printf("  存在的节点+错属性  -> %d\n", guest_loader_dtb_node_has_prop(b,(unsigned)n,"npu@fdab0000","no-map"));
 printf("  非 FDT             -> %d\n", guest_loader_dtb_node_has_prop((unsigned char*)"hello",5,"npu@fdab0000","dma-coherent"));
 return 0;}
