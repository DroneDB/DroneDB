#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include <nan.h>

// Top-level functions

NAN_METHOD(getVersion);
NAN_METHOD(typeToHuman);
NAN_METHOD(parseFiles);
NAN_METHOD(_thumbs_getFromUserCache);


#endif
