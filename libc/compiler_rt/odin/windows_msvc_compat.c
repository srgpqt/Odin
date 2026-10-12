// Odin generates code for the MSVC flavour of the Windows ABI, which expects a few symbols that
// the C runtime of Visual Studio defines and that mingw-w64 has no need for.

// Referenced by every object which uses floating point
int _fltused = 0x9875;
