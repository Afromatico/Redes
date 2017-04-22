#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv){
	FILE *out;
	int n = atoi(argv[1]);
	int m = 0;
	out = fopen("a.txt","w");
	int i;
	char buf[100];
	size_t escritos;
	for (i = 0; i < n + 1; ++i){
		memset(buf,0,100);
		sprintf(buf,"%d-",i);
		m = strlen(buf);
		escritos = fwrite(buf,m,1,out);
	}
	fclose(out);

}
