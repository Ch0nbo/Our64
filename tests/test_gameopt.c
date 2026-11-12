#include "gameopt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
	gameopt_parse("Super Mario 64 (USA).z64\t720p\nbad line\nX.z64\t480p\r\nStar Fox 64 (USA) (Rev 1).z64\t720p\r\n");
	assert(gameopt_lines("Super Mario 64 (USA).z64")==720);
	assert(gameopt_lines("Star Fox 64 (USA) (Rev 1).z64")==720);
	assert(gameopt_lines("X.z64")==480 && gameopt_lines("nope")==480 && gameopt_lines(NULL)==480);
	assert(gameopt_set_lines("New.z64",720)==0 && gameopt_lines("New.z64")==720);
	assert(gameopt_set_lines("Super Mario 64 (USA).z64",480)==0 && gameopt_lines("Super Mario 64 (USA).z64")==480);
	assert(gameopt_set_lines("a\tb.z64",720)==-1);
	char t[512]; int n=gameopt_text(t,sizeof t); assert(n==(int)strlen(t));
	assert(!strcmp(t,"Star Fox 64 (USA) (Rev 1).z64\t720p\nNew.z64\t720p\n"));
	assert(gameopt_text(NULL,0)==n);
	gameopt_parse(t); assert(gameopt_lines("New.z64")==720);
	puts("gameopt ok"); return 0; }
