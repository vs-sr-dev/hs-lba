#include "DEFINES.H"

extern UBYTE *Log;
extern UBYTE *Phys;

/*
 * `Log` is one buffer with two readings, and MCGA mode picks the second.
 *
 * The F12 zoom leaves the game rendering into Log as a 640x480 page and
 * windows 320x200 out of it (GAMEMENU.C's CopyBlockPhysMCGA). The FLA player
 * instead overwrites the front of the same memory with a plain contiguous
 * 320x200 image: ADFLI_A.C's DrawFrame() and UpdateFrame() — the two codecs
 * that carry every frame — advance by their `deltax` argument, which PLAYFLA.C
 * always passes as 320. So the flat 64000-byte copy below is right, and the
 * 320x200 window the zoom uses is a different question answered elsewhere.
 *
 * (Rewriting this to a strided copy made the FLA arrive as its top half over a
 * black lower half, each output row an image row skipped — the visible shape
 * of reading a 320-stride image with a 640 stride.)
 */
void Mcga_Cls(void)
{
	memset(Log, 0, 64000);
}

void Mcga_Flip(void)
{
	memcpy(Phys, Log, 64000);

#if defined(PORT_HS)
	{
		/* On DOS Phys was the VGA aperture, so the copy above *was* the
		 * display. Here it has to be handed over. This is the only present in
		 * the FLA player's loop: it never reaches Vsync() or Flip(). */
		void PORT_PresentPhys(void); /* src/platform_gfx.c */
		PORT_PresentPhys();
	}
#endif
}
