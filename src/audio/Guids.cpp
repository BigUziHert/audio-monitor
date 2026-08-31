//
// The single translation unit that emits definitions for the PROPERTYKEYs we
// reference. <initguid.h> must be included before the headers that declare
// them; everywhere else they resolve as ordinary extern declarations.
//
#include <initguid.h>

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
