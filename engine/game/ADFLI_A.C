#include "C_EXTERN.H"

/*
 * PORT: the FLA image is a contiguous 320x200 block at the front of Log, not
 * rows of a 640-wide page. DrawFrame() and UpdateFrame() below — the two
 * codecs every frame actually goes through — step by the `deltax` PLAYFLA.C
 * hands them, which is always 320, and Mcga_Flip() copies 64000 bytes flat.
 * These two were translated with a doubled step, so a FLA_BLACK or FLA_COPY
 * pass wrote every other row and spilled past the end of the image.
 */
void BlackFrame(void)
{
	int i = 0;
	UBYTE *dst = Log;
	for (i = 0; i < 200; i++)
	{
		memset(dst, 0, 320);
		dst += 320;
	}
}

void CopyFrame(UBYTE *ptsrc)
{
	int i = 0;
	UBYTE *dst = Log;
	UBYTE *src = ptsrc;
	for (i = 0; i < 200; i++)
	{
		memcpy(dst, src, 320);
		src += 320;
		dst += 320;
	}
}

void DrawFrame(UBYTE *ptframe, LONG deltax, LONG deltay)
{
	BYTE count;
	UBYTE nBlocks, data;
	UBYTE *src = ptframe;
	UBYTE *dst = Log;
	UBYTE *tmp = dst;

	while (deltay != 0)
	{
		nBlocks = *(src++);
		while (nBlocks != 0)
		{
			count = (BYTE)(*(src++));
			if (count < 0)
			{
				count = -count;
				memcpy(dst, src, count);
				dst += count;
				src += count;
			}
			else
			{
				data = (*src++);
				memset(dst, data, count);
				dst += count;
			}
			nBlocks--;
		}
		dst = tmp + deltax;
		tmp = dst;
		deltay--;
	}
}

void UpdateFrame(UBYTE *ptframe, LONG deltax)
{
	UBYTE nBlocks, xPos, data;
	BYTE count;
	WORD yline, cptline;
	UBYTE *src = ptframe;
	UBYTE *dst = Log;
	UBYTE *dst2;

	yline = *(src++);
	yline |= *(src++) << 8;
	dst += yline * deltax;
	dst2 = dst;

	cptline = *(src++);
	cptline |= *(src++) << 8;

	while (cptline != 0)
	{
		nBlocks = *(src++);
		while (nBlocks != 0)
		{
			xPos = *(src++);
			dst += xPos;
			count = *(src++);
			if (count < 0)
			{
				count = -count;
				data = *(src++);
				memset(dst, data, count);
				dst += count;
			}
			else
			{
				memcpy(dst, src, count);
				dst += count;
				src += count;
			}
			nBlocks--;
		}
		dst = dst2 + deltax;
		dst2 = dst;
		cptline--;
	}
}
