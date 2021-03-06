#include "gl_typedef.h"
#include <omnetpp.h>
using namespace omnetpp;


#define BUFLEN 1024

void simDbgPrint(const char *fmt, ...)
{
    char* staticbuf=(char*)malloc(BUFLEN) ;

    va_list va;

    va_start (va, fmt) ;

    vsnprintf(staticbuf, BUFLEN, fmt, va) ;
    staticbuf[BUFLEN-1] = '\0' ;

    va_end (va) ;

    EV << staticbuf;

    free(staticbuf) ;
}

