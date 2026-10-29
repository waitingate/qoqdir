// The GUIDs the app names (FOLDERID_Profile, CLSID_ShellLink, IID_IDropTarget and the like), defined here rather than
// taken from libuuid: each of its members holds thousands of them, and the two the app pulled in made 53 KB of the exe.
// initguid.h makes each DEFINE_GUID in the headers below a definition, and the link keeps only the ones used.
#include <initguid.h>

#include <oleidl.h>
#include <shlguid.h>
#include <shobjidl.h>   // and with it knownfolders.h, which has no include guard

// MinGW's unknwn.h only declares this one.
DEFINE_GUID(IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
