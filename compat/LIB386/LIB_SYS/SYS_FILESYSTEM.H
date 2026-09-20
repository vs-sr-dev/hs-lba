/* Forwarder. The engine asks for "../LIB386/LIB_SYS/SYS_FILESYSTEM.H", the
 * layout of the original DOS tree; the real header lives in engine/LIB_SYS.
 * The path has to be relative — a bare #include of the same name would find
 * this file again. */
#include "../../../engine/LIB_SYS/SYS_FILESYSTEM.H"
