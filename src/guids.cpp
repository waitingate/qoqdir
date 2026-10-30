// The GUIDs the app names (FOLDERID_Profile, CLSID_ShellLink, IID_IDropTarget and the like), defined here so the link
// needs no libuuid; the exe is the same size either way, as the link keeps only the GUIDs used. initguid.h makes each
// DEFINE_GUID in the headers below a definition.
#include <initguid.h>

#include <oleidl.h>
#include <shlguid.h>
#include <shobjidl.h>   // and with it knownfolders.h, which has no include guard

// MinGW's unknwn.h only declares this one.
DEFINE_GUID(IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
