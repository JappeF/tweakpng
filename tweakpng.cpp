// tweakpng.cpp
//
//
/*
	Copyright (C) 1999-2008 Jason Summers

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	See the file tweakpng-src.txt for more information.
*/

// Try to use a less ancient version of comctl32.dll than the default.
// This belongs in the .manifest file(s), but I can't figure out how to
// get it to work.
#ifdef _WIN64
#pragma comment(linker, \
	"\"/manifestdependency:type='Win32' "\
	"name='Microsoft.Windows.Common-Controls' "\
	"version='6.0.0.0' "\
	"processorArchitecture='amd64' "\
	"publicKeyToken='6595b64144ccf1df' "\
	"language='*'\"")
#else
#pragma comment(linker, \
	"\"/manifestdependency:type='Win32' "\
	"name='Microsoft.Windows.Common-Controls' "\
	"version='6.0.0.0' "\
	"processorArchitecture='x86' "\
	"publicKeyToken='6595b64144ccf1df' "\
	"language='*'\"")
#endif

#include "twpng-config.h"

#include <windows.h>
#include <tchar.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdarg.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "shlwapi.lib")

#include "resource.h"
#include "tweakpng.h"
#ifdef TWPNG_HAVE_ZLIB
#include <zlib.h>
#else
#define Z_DEFAULT_COMPRESSION  (-1)
#endif
#include <strsafe.h>

#define UPDATE_DELAY 400  // milliseconds
#define TWPNG_TOP_MARGIN 8

// Undocumented messages Windows sends when it wants the app to owner-draw the menu bar in dark mode.
// Struct layout must match the (undocumented) UAH menu structures exactly; see
// adzm/win32-custom-menubar-aero-theme. The DRAWITEMSTRUCT is the FIRST member of the
// item struct, not the last.
#define WM_UAHDRAWMENU     0x0091
#define WM_UAHDRAWMENUITEM 0x0092

typedef union tagUAHMENUITEMMETRICS {
	struct { DWORD cx; DWORD cy; } rgsizeBar[2];
	struct { DWORD cx; DWORD cy; } rgsizePopup[4];
} UAHMENUITEMMETRICS;

typedef struct tagUAHMENUPOPUPMETRICS {
	DWORD rgcx[4];
	DWORD fUpdateMaxWidths : 2;
} UAHMENUPOPUPMETRICS;

typedef struct tagUAHMENU {
	HMENU hmenu;
	HDC hdc;
	DWORD dwFlags;
} UAHMENU;

typedef struct tagUAHMENUITEM {
	int iPosition;
	UAHMENUITEMMETRICS umim;
	UAHMENUPOPUPMETRICS umpm;
} UAHMENUITEM;

typedef struct tagUAHDRAWMENUITEM {
	DRAWITEMSTRUCT dis;
	UAHMENU um;
	UAHMENUITEM umi;
} UAHDRAWMENUITEM;

static Png *png;
Viewer *g_viewer;

struct globals_struct globals;

// Modeless "AI Generation Parameters" viewer (one instance at a time).
static HWND g_hwndAIParams = NULL;
// Modeless "Folder Overview" window (one instance at a time).
static HWND g_hwndAIFolder = NULL;
static void twpng_ShowFolderOverview(const TCHAR *dir, BOOL focusSearch);
static LRESULT CALLBACK MainListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
	LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
static void twpng_ViewAIParams(HWND owner);
static void twpng_CloseAIParamsView();
static int  twpng_ShowAIParams(const TCHAR *text, const TCHAR *extra, const TCHAR *path, BOOL isDoc);
static int  twpng_ReadMp4VideoInfo(const TCHAR *fn, TCHAR *out, int cap);
static int  OkToClosePNG();
static BOOL twpng_AIParamsKey(const MSG *m);
#define WM_APP_SIBLINGS (WM_APP+21)
#define WM_APP_THUMB    (WM_APP+22)
static int  cf_skip_ws(const TCHAR *s, int i, int len);
static int  twpng_ReadJpegAIParams(const TCHAR *fn, TCHAR **outText);
static int  twpng_ReadWebpAIParams(const TCHAR *fn, TCHAR **outText);
static int  twpng_SniffForeignKind(const unsigned char *hdr, DWORD got);
static int  twpng_ReadForeignAIParams(int kind, const TCHAR *fn, TCHAR **outText);
static BOOL twpng_LooksLikeComfyJson(const TCHAR *s);
struct aiparams_ctx;
static BOOL twpng_ParseComfyJson(const TCHAR *full, struct aiparams_ctx *ctx);

// When a JPEG/WebP/MP4/MOV is opened, png stays NULL but we remember the filename so the title
// and status bar can show its info. Empty when no foreign file is loaded.
static TCHAR g_foreignFn[MAX_PATH] = _T("");
static int   g_foreignKind = 0;   // 1=JPEG, 2=WebP, 3=MP4, 4=MOV
// Generator badge for the currently-loaded foreign file, or NULL. Cached on open so the
// status bar doesn't re-read EXIF on every redraw.
static const TCHAR *g_foreignGen = NULL;

static LRESULT CALLBACK WndProcMain(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK DlgProcAbout(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK DlgProcPrefs(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK DlgProcSplitIDAT(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK DlgProcSetSig(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK DlgProcTools(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static int OkToClosePNG();
static void SetTitle(Png *p);
static void ImportChunkByFilename(const TCHAR *fn, int pos);
static int GetLVFocus(HWND hwnd);

/* make the table for a fast crc */
void make_crc_table()
{
	int n;

	for (n = 0; n < 256; n++) {
		DWORD c;
		int k;

		c = (DWORD)n;
		for (k = 0; k < 8; k++)
		c = c & 1 ? 0xedb88320L ^ (c >> 1):c >> 1;

		globals.crc_table[n] = c;
	}
}


DWORD update_crc(DWORD crc, unsigned char *buf, int len)
{
	DWORD c = crc;
	unsigned char *p = buf;
	int n = len;

	if (n > 0) do {
		c = globals.crc_table[(c ^ (*p++)) & 0xff] ^ (c >> 8);
	} while (--n);
	return c;
}

// ---- Dark-themed MessageBox ---------------------------------------------------------
// The standard MessageBox paints a light footer band that can't be themed, so we use a
// small custom dialog (DLG_MSGBOX) themed like every other dialog.

struct msgbox_ctx {
	const TCHAR *text;
	const TCHAR *caption;
	UINT type;
};

static INT_PTR CALLBACK DlgProcMsgBox(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	WORD id = LOWORD(wParam);

	if(msg == WM_INITDIALOG) {
		struct msgbox_ctx *c = (struct msgbox_ctx*)lParam;
		if(!c) { EndDialog(hwnd, IDCANCEL); return TRUE; }
		SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)c->type);

		SetWindowText(hwnd, (c->caption && c->caption[0]) ? c->caption : _T("TweakPNG"));
		SetDlgItemText(hwnd, IDC_MSGBOX_TEXT, c->text ? c->text : _T(""));

		LPCTSTR icoName = NULL;
		switch(c->type & MB_ICONMASK) {
		case MB_ICONERROR:       icoName = IDI_ERROR; break;
		case MB_ICONQUESTION:    icoName = IDI_QUESTION; break;
		case MB_ICONWARNING:     icoName = IDI_WARNING; break;
		case MB_ICONINFORMATION: icoName = IDI_INFORMATION; break;
		}
		HWND hIcon = GetDlgItem(hwnd, IDC_MSGBOX_ICON);
		if(icoName) SendMessage(hIcon, STM_SETICON, (WPARAM)LoadIcon(NULL, icoName), 0);
		else        ShowWindow(hIcon, SW_HIDE);

		HDC hdc = GetDC(hwnd);
		int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
		HFONT hOldFont = globals.hUiFont ? (HFONT)SelectObject(hdc, globals.hUiFont) : NULL;
		int pad     = MulDiv(14, dpi, 96);
		int gap     = MulDiv(14, dpi, 96);
		int iconSz  = icoName ? GetSystemMetrics(SM_CXICON) : 0;
		int maxTxtW = MulDiv(340, dpi, 96);
		RECT rcTxt = {0,0,maxTxtW,0};
		DrawText(hdc, c->text ? c->text : _T(""), -1, &rcTxt,
			DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
		if(hOldFont) SelectObject(hdc, hOldFont);
		ReleaseDC(hwnd, hdc);
		int txtW = rcTxt.right, txtH = rcTxt.bottom;

		int contentLeft = pad + (iconSz ? iconSz + gap : 0);
		int contentH = (txtH > iconSz) ? txtH : iconSz;

		int btnW = MulDiv(80, dpi, 96), btnH = MulDiv(23, dpi, 96), btnGap = MulDiv(8, dpi, 96);
		int btns[3], nbtn = 0, defbtn = IDOK;
		switch(c->type & MB_TYPEMASK) {
		case MB_OKCANCEL:    btns[nbtn++]=IDOK;  btns[nbtn++]=IDCANCEL; defbtn=IDOK;  break;
		case MB_YESNO:       btns[nbtn++]=IDYES; btns[nbtn++]=IDNO;     defbtn=IDYES; break;
		case MB_YESNOCANCEL: btns[nbtn++]=IDYES; btns[nbtn++]=IDNO; btns[nbtn++]=IDCANCEL; defbtn=IDYES; break;
		default:             btns[nbtn++]=IDOK;  defbtn=IDOK;          break;
		}
		const int allBtns[4] = {IDOK, IDYES, IDNO, IDCANCEL};
		for(int i=0;i<4;i++) ShowWindow(GetDlgItem(hwnd, allBtns[i]), SW_HIDE);

		int btnRowW = nbtn*btnW + (nbtn-1)*btnGap;
		int clientW = contentLeft + txtW + pad;
		int minW = pad + btnRowW + pad;
		if(clientW < minW) clientW = minW;
		int btnY = pad + contentH + MulDiv(16, dpi, 96);
		int clientH = btnY + btnH + pad;

		RECT wr = {0,0,clientW,clientH};
		AdjustWindowRectEx(&wr, GetWindowLong(hwnd,GWL_STYLE), FALSE, GetWindowLong(hwnd,GWL_EXSTYLE));
		int winW = wr.right-wr.left, winH = wr.bottom-wr.top;

		RECT orc; HWND owner = GetWindow(hwnd, GW_OWNER);
		if(!(owner && GetWindowRect(owner, &orc)))
			SystemParametersInfo(SPI_GETWORKAREA, 0, &orc, 0);
		int x = orc.left + ((orc.right-orc.left)-winW)/2;
		int y = orc.top  + ((orc.bottom-orc.top)-winH)/2;
		SetWindowPos(hwnd, NULL, x, y, winW, winH, SWP_NOZORDER);

		if(iconSz) SetWindowPos(hIcon, NULL, pad, pad, iconSz, iconSz, SWP_NOZORDER);
		SetWindowPos(GetDlgItem(hwnd, IDC_MSGBOX_TEXT), NULL,
			contentLeft, pad + (contentH-txtH)/2, txtW, txtH, SWP_NOZORDER);
		int bx = clientW - pad - btnRowW;
		for(int i=0;i<nbtn;i++) {
			HWND hb = GetDlgItem(hwnd, btns[i]);
			SetWindowPos(hb, NULL, bx, btnY, btnW, btnH, SWP_NOZORDER);
			ShowWindow(hb, SW_SHOW);
			bx += btnW + btnGap;
		}
		SendMessage(hwnd, DM_SETDEFID, defbtn, 0);

		twpng_InitDarkDialog(hwnd);
		MessageBeep(c->type & MB_ICONMASK);
		SetFocus(GetDlgItem(hwnd, defbtn));
		return FALSE;
	}

	switch(msg) {
	case WM_COMMAND:
		switch(id) {
		case IDOK: case IDYES: case IDNO:
			EndDialog(hwnd, id);
			return TRUE;
		case IDCANCEL:
			// For an OK-only box, Esc/close should map to OK like the real MessageBox.
			EndDialog(hwnd, ((GetWindowLongPtr(hwnd,DWLP_USER) & MB_TYPEMASK)==MB_OK) ? IDOK : IDCANCEL);
			return TRUE;
		}
		break;
	}
	{
		INT_PTR darkResult = 0;
		if(twpng_HandleDlgDarkMsg(msg, wParam, lParam, &darkResult)) return darkResult;
	}
	return FALSE;
}

int twpng_MessageBox(HWND hwnd, const TCHAR *text, const TCHAR *caption, UINT type)
{
	struct msgbox_ctx c;
	c.text = text;
	c.caption = caption;
	c.type = type;
	INT_PTR r = DialogBoxParam(globals.hInst, _T("DLG_MSGBOX"),
		hwnd ? hwnd : globals.hwndMain, DlgProcMsgBox, (LPARAM)&c);
	if(r <= 0) {  // creation failed or closed without a button
		switch(type & MB_TYPEMASK) {
		case MB_OKCANCEL: case MB_YESNOCANCEL: return IDCANCEL;
		case MB_YESNO: return IDNO;
		default: return IDOK;
		}
	}
	return (int)r;
}

void mesg(int severity, const TCHAR *fmt, ...)
{
	va_list ap;
	TCHAR buf[2048];
	const TCHAR *t;
	int flags;

	va_start(ap, fmt);
	StringCbVPrintf(buf,sizeof(buf),fmt,ap);
	va_end(ap);

	switch(severity) {
	case MSG_S: t=_T("Error");   flags=MB_ICONERROR;        break;
	case MSG_W: t=_T("Warning"); flags=MB_ICONWARNING;      break;
	case MSG_I: t=_T("Notice");  flags=MB_ICONINFORMATION;  break;
	default:    t=_T("Error");   flags=MB_ICONWARNING;      break;  // (MSG_E)
	}
	twpng_MessageBox(globals.hwndMain,buf,t,MB_OK|flags);
}

// Some functions for reading/writing big-endian integers.

DWORD read_int32(unsigned char *x)
{
//	return x[0]*256*256*256 +
//	       x[1]*256*256 +
//	       x[2]*256 +
//	       x[3];
	return (((DWORD)x[0])<<24) +
	       (((DWORD)x[1])<<16) +
	       (((DWORD)x[2])<<8) +
	       ((DWORD)x[3]);
}

void write_int32(unsigned char *buf, DWORD x)
{
	buf[0]= (unsigned char) ((x & 0xff000000)>>24);
	buf[1]= (unsigned char) ((x & 0x00ff0000)>>16);
	buf[2]= (unsigned char) ((x & 0x0000ff00)>> 8);
	buf[3]= (unsigned char) ( x & 0x000000ff     );
}


int read_int16(unsigned char *x)
{
	return x[0]*256 +
	       x[1];
}

void write_int16(unsigned char *buf, int x)
{
	buf[0]= (unsigned char) ((x & 0x0000ff00)>> 8);
	buf[1]= (unsigned char) ( x & 0x000000ff     );
}


// return pointer to first chunk of requested type.
// return NULL if none exist
// returns the index of the chunk if index != NULL
Chunk* Png::find_first_chunk(int id, int *index)
{
	int i;
	for(i=0;i<m_num_chunks;i++) {
		if(chunk[i]->m_chunktype_id==id) {
			if(index) (*index)=i;
			return chunk[i];
		}
	}
	if(index) (*index)= -1;
	return NULL;
}

// fill in a single row (chunk) of the listview window
void Png::update_row(HWND hwnd,int i)
{
	TCHAR buf[4096];
	LV_ITEM lvi;
	int rv;

	ZeroMemory((void*)&lvi,sizeof(LV_ITEM));
	lvi.mask= LVIF_TEXT    /* LVIF_DI_SETITEM */;

	lvi.iItem=i;
	lvi.iSubItem=0;
	lvi.pszText=chunk[i]->m_chunktype_tchar;
	rv=ListView_SetItem(hwnd,&lvi);

	StringCchPrintf(buf,4096,_T("%u"),chunk[i]->length);       // length
	lvi.pszText=buf;
	lvi.iSubItem=1;
	rv=ListView_SetItem(hwnd,&lvi);

	StringCchPrintf(buf,4096,_T("%08x"),chunk[i]->m_crc);          // crc
	lvi.iSubItem=2;
	rv=ListView_SetItem(hwnd,&lvi);

	chunk[i]->get_text_descr_generic(buf,4096);        // attributes
	lvi.iSubItem=3;
	rv=ListView_SetItem(hwnd,&lvi);

	chunk[i]->get_text_descr(buf,4096);     // contents
	lvi.iSubItem=4;
	rv=ListView_SetItem(hwnd,&lvi);
}

// fill listview control, one line per PNG chunk
void Png::fill_listbox(HWND hwnd)
{
	int i;
	int rv;
	TCHAR textbuf[50];

	LV_ITEM lvi;

	ListView_DeleteAllItems(hwnd);

	ZeroMemory((void*)&lvi,sizeof(LV_ITEM));
	lvi.mask= LVIF_TEXT    /* | LVIF_DI_SETITEM ? */;

	StringCbCopy(textbuf, sizeof(textbuf), _T(""));
	for(i=0;i<m_num_chunks;i++) {
		lvi.iItem=i;
		lvi.iSubItem=0;
		lvi.pszText=textbuf;  // start with blank; update_row() will set everything
		rv=ListView_InsertItem(hwnd,&lvi);

		update_row(hwnd,i);
	}
}

DWORD Png::get_file_size()
{
	int i;
	DWORD s=0;

	// calculate new file size
	s=8;   // the file signature
	for(i=0;i<m_num_chunks;i++) {
		s+= 12+chunk[i]->length;
	}
	return s;
}

static void update_viewer_filename()
{
#ifdef TWPNG_SUPPORT_VIEWER
	if(!g_viewer) return;
	if(png && png->m_named)
		g_viewer->SetCurrentFileName(png->m_filename);
	else
		g_viewer->SetCurrentFileName(NULL);
#endif
}

// Identify the AI image generator from the text chunks, or NULL if none recognized.
static const TCHAR *twpng_DetectGenerator(Png *p)
{
	int a1111=0, comfy=0, hasSoftware=0, hasComment=0;
	if(!p) return NULL;
	for(int i=0;i<p->m_num_chunks;i++) {
		Chunk *c = p->chunk[i];
		if(!c) continue;
		if(c->m_chunktype_id!=CHUNK_tEXt && c->m_chunktype_id!=CHUNK_zTXt &&
		   c->m_chunktype_id!=CHUNK_iTXt) continue;
		struct keyword_info_struct kw;
		if(!c->get_keyword_info(&kw)) continue;
		if(!lstrcmp(kw.keyword,_T("parameters")))      a1111=1;
		else if(!lstrcmp(kw.keyword,_T("workflow")) ||
		        !lstrcmp(kw.keyword,_T("prompt")))     comfy=1;
		else if(!lstrcmp(kw.keyword,_T("Software")))   hasSoftware=1;
		else if(!lstrcmp(kw.keyword,_T("Comment")))    hasComment=1;
	}
	if(a1111) return _T("Stable Diffusion ") SYM_MIDDOT _T(" A1111");
	if(comfy) return _T("ComfyUI");
	if(hasSoftware && hasComment) return _T("NovelAI");
	return NULL;
}

// Same detection but against raw EXIF text (JPEG/WebP). The text we get from the readers
// is usually A1111-formatted (Steps:/Negative prompt:) — even ComfyUI workflows are
// converted to A1111 shape by twpng_ParseComfyJson before being shown. NovelAI's
// "Comment" is JSON with "prompt" + "uc" keys.
static const TCHAR *twpng_DetectGeneratorFromText(const TCHAR *t)
{
	if(!t) return NULL;
	if(_tcsstr(t,_T("\"class_type\"")))               return _T("ComfyUI");
	if(_tcsstr(t,_T("\"uc\"")) && _tcsstr(t,_T("\"prompt\""))) return _T("NovelAI");
	if(_tcsstr(t,_T("Steps:")))                        return _T("Stable Diffusion ") SYM_MIDDOT _T(" A1111");
	return NULL;
}

static void update_status_bar_and_viewer()
{
	TCHAR buf[200];
	DWORD s;
	const TCHAR *type;
	const TCHAR *gen;

	if(!globals.hwndStBar) { goto done; }
	if(!png) {
		if(g_foreignFn[0]) {
			HANDLE fh = CreateFile(g_foreignFn, GENERIC_READ, FILE_SHARE_READ, NULL,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			ULONGLONG fs = 0;
			LARGE_INTEGER li;
			if(fh != INVALID_HANDLE_VALUE) { if(GetFileSizeEx(fh, &li)) fs = (ULONGLONG)li.QuadPart; CloseHandle(fh); }
			const TCHAR *label = (g_foreignKind==1) ? _T("JPEG") :
			                     (g_foreignKind==2) ? _T("WebP") :
			                     (g_foreignKind==3) ? _T("MP4") :
			                     (g_foreignKind==4) ? _T("MOV") : _T("File");
			TCHAR fbuf[40];
			if(fs >= 1073741824UL)   StringCchPrintf(fbuf,40,_T(" (%.2f GB)"),(double)fs/1073741824.0);
			else if(fs >= 1048576UL) StringCchPrintf(fbuf,40,_T(" (%.2f MB)"),(double)fs/1048576.0);
			else if(fs >= 1024UL)    StringCchPrintf(fbuf,40,_T(" (%.2f KB)"),(double)fs/1024.0);
			else                     fbuf[0]=0;
			TCHAR sbuf[200];
			StringCchPrintf(sbuf,200,_T("%s file size: %I64u bytes%s"),label,fs,fbuf);
			if(g_foreignGen) {
				TCHAR genbuf[80];
				StringCchPrintf(genbuf,80,_T("     %s  %s"),SYM_MIDDOT,g_foreignGen);
				StringCchCat(sbuf,200,genbuf);
			}
			SetWindowText(globals.hwndStBar,sbuf);
		} else {
			SetWindowText(globals.hwndStBar,_T("No file loaded"));
		}
		goto done;
	}

	switch(png->m_imgtype) {
	case IMG_PNG: type=_T("PNG"); break;
	case IMG_MNG: type=_T("MNG"); break;
	case IMG_JNG: type=_T("JNG"); break;
	default:      type=_T("???"); break;
	}

	s=png->get_file_size();

	TCHAR humanbuf[40];
	if(s >= 1073741824UL)   StringCchPrintf(humanbuf,40,_T(" (%.2f GB)"),(double)s/1073741824.0);
	else if(s >= 1048576UL) StringCchPrintf(humanbuf,40,_T(" (%.2f MB)"),(double)s/1048576.0);
	else if(s >= 1024UL)    StringCchPrintf(humanbuf,40,_T(" (%.2f KB)"),(double)s/1024.0);
	else                    humanbuf[0]=_T('\0');

	StringCchPrintf(buf,200,_T("%s file size: %u bytes%s"),type,(unsigned int)s,humanbuf);

	gen = twpng_DetectGenerator(png);
	if(gen) {
		TCHAR genbuf[80];
		StringCchPrintf(genbuf,80,_T("     %s  %s"),SYM_MIDDOT,gen);
		StringCchCat(buf,200,genbuf);
	}
	SetWindowText(globals.hwndStBar,buf);

done:
	if(g_viewer) {
		SetTimer(globals.hwndMain,1,UPDATE_DELAY,NULL);
		globals.timer_set=1;
	}
}

void Png::modified()
{
	if(!m_dirty) {
		m_dirty=1;
		SetTitle(this);
	}
	update_status_bar_and_viewer();
}

int Png::write_to_mem(unsigned char **pmem, int *plen)
{
	int s;
	int i;
	int pos;

	unsigned char *m;

	// create an image of the file in memory
	s=get_file_size();

	m=(unsigned char*)malloc(s);
	if(!m) return 0;

	CopyMemory(m,signature,8);

	pos=8;
	for(i=0;i<m_num_chunks;i++) {
		chunk[i]->copy_to_memory(&m[pos]);
		pos+= 12+chunk[i]->length;
	}

	*pmem = m;
	*plen = s; // should also = pos
	return 1;
}

// Call before a sequence of stream_file_read() calls.
void Png::stream_file_start()
{
	m_stream_phase = 0;
	m_stream_curchunk = 0; // -1 means we're reading the signature
	m_stream_curpos_in_curchunk = 0;
}

// Starting at position m_stream_curpos_in_curchunk in chunk m_stream_curchunk,
// copy nbytes bytes of the PNG file into buf (unless end of file is reached).
// This may require reading from the following chunk(s).
// Returns the number of bytes copied.
DWORD Png::stream_file_read(unsigned char *buf, DWORD bytes_requested)
{
	DWORD total_bytes_copied=0; // bytes copied in this call to stream_file_read
	DWORD nread;
	DWORD bytes_to_copy;

	// Read the file signature, if necessary.
	if(m_stream_phase==0) {
		bytes_to_copy = 8-m_stream_curpos_in_curchunk;
		if(bytes_to_copy>bytes_requested) bytes_to_copy=bytes_requested;

		CopyMemory(buf,&signature[m_stream_curpos_in_curchunk],bytes_to_copy);
		m_stream_curpos_in_curchunk+=bytes_to_copy;
		total_bytes_copied+=bytes_to_copy;
		if(total_bytes_copied>=8) {
			// Done reading signature. Now switch to reading chunks.
			m_stream_phase=1;
			m_stream_curchunk=0;
			m_stream_curpos_in_curchunk=0;
		}
		else {
			goto done;
		}
	}

	// Read the chunk(s).
	while(1) {
		if(total_bytes_copied>=bytes_requested) goto done;

		if(m_stream_curchunk>=m_num_chunks) goto done;

		if(m_stream_curpos_in_curchunk >= chunk[m_stream_curchunk]->length+12) {
			// advance to next chunk
			m_stream_curchunk++;
			m_stream_curpos_in_curchunk=0;
			if(m_stream_curchunk>=m_num_chunks) goto done;
		}

		nread = chunk[m_stream_curchunk]->copy_segment_to_memory(&buf[total_bytes_copied],
			m_stream_curpos_in_curchunk, bytes_requested-total_bytes_copied);
		m_stream_curpos_in_curchunk+=nread;
		total_bytes_copied+=nread;
	}

done:
	return total_bytes_copied;
}

// opens up space for new chunks and optionally creates them.
// if init is 0, opens up space for new chunks but doesn't create them
void Png::insert_chunks(int pos, int num, int init)
{
	int i;

	ensure_chunks_alloc(m_num_chunks+num);

	for(i=m_num_chunks+num-1;i>=(pos+num);i--) {
		chunk[i] = chunk[i-num];
	}

	if(init) {
		for(i=pos;i<pos+num;i++) {
			chunk[i] = new Chunk;
			chunk[i]->m_parentpng=this;
		}
	}

	m_num_chunks+=num;
}

// insert a new chunk of the specified type
void Png::new_chunk(int newid)
{
	int pos,ok,i;
	SYSTEMTIME st;
	Chunk *c;
	char newname[5];
	int plte_pos;

	pos=1;
	ok=1;

	c = new Chunk();
	c->m_parentpng=png;

	if(!get_name_from_id(newname,newid)) {
		mesg(MSG_S,_T("internal: chunk name not found for %d"),newid);
		delete c;
		return;
	}
	StringCchCopyA(c->m_chunktype_ascii,5,newname);
	c->set_chunktype_tchar_from_ascii();

	// plte_pos will be -1 if no PLTE chunk exists
	find_first_chunk(CHUNK_PLTE, &plte_pos);

	switch(newid) {
	case CHUNK_tEXt:
		pos=m_num_chunks-1;
		c->length=8;
		c->data = (unsigned char*)malloc(c->length);  // fixme, check for failure
		StringCchCopyA((char*)c->data,8,"Comment");
		break;

	case CHUNK_gAMA:
		c->length=4;
		c->data = (unsigned char*)malloc(c->length);
		write_int32(c->data,45455);
		break;

	case CHUNK_oFFs:
		c->length=9;
		c->data = (unsigned char*)malloc(c->length);
		write_int32(&c->data[0],0);
		write_int32(&c->data[4],0);
		c->data[8]=0;
		break;

	case CHUNK_pHYs:
		c->length=9;
		c->data = (unsigned char*)malloc(c->length);
		write_int32(&c->data[0],1);
		write_int32(&c->data[4],1);
		c->data[8]=0;
		break;

	case CHUNK_sRGB:
		c->length=1;
		c->data = (unsigned char*)malloc(c->length);
		c->data[0]=0;
		break;

	case CHUNK_sTER:
		c->length=1;
		c->data = (unsigned char*)malloc(c->length);
		c->data[0]=0;
		break;

	case CHUNK_cHRM:
		c->length=32;
		c->data = (unsigned char*)malloc(c->length);
		write_int32(&c->data[ 0],31270);
		write_int32(&c->data[ 4],32900);
		write_int32(&c->data[ 8],64000);
		write_int32(&c->data[12],33000);
		write_int32(&c->data[16],30000);
		write_int32(&c->data[20],60000);
		write_int32(&c->data[24],15000);
		write_int32(&c->data[28], 6000);
		break;

	case CHUNK_bKGD:
		if(plte_pos>=0) pos=plte_pos+1;
		switch(m_colortype) {
		case 3: c->length=1; break;
		case 0: case 4: c->length=2; break;
		case 2: case 6: c->length=6; break;
		default: return;
		}
		c->data = (unsigned char*)calloc(c->length,1);
		break;

	case CHUNK_sBIT:
		switch(m_colortype) {
		case 0: c->length=1; break;
		case 2: case 3: c->length=3; break;
		case 4: c->length=2; break;
		case 6: c->length=4; break;
		default: return;
		}
		c->data = (unsigned char*)calloc(c->length,1);
		for(i=0;i<(int)c->length;i++) {
			c->data[i]= (m_colortype==3)?8:m_bitdepth;
		}
		break;

	case CHUNK_PLTE:
		if(m_colortype!=2 && m_colortype!=3 && m_colortype!=6) {
			mesg(MSG_E,_T("Palette is not allowed for grayscale images"));
			return;
		}
		c->length=3;
		c->data = (unsigned char*)calloc(c->length,1);
		break;

	case CHUNK_tRNS:
		if(plte_pos>=0) pos=plte_pos+1;
		switch(m_colortype) {
		case 3: c->length=1; break;
		case 0: c->length=2; break;
		case 2: c->length=6; break;
		case 4: case 6:
			mesg(MSG_E,_T("Transparency chunk is not allowed for this image, since ")
				_T("it already has a full alpha channel"));
			return;
		default: return;
		}
		c->data = (unsigned char*)calloc(c->length,1);
		if(m_colortype==3) c->data[0]=255;  // default to opaque
		break;

	case CHUNK_IHDR:
		pos=0;
		c->length=13;
		c->data = (unsigned char*)calloc(c->length,1);
		write_int32(&c->data[ 0],1); // width
		write_int32(&c->data[ 4],1); // height
		c->data[8]=1; // bit depth
		break;

	case CHUNK_IEND:
		pos=m_num_chunks;
		c->length=0;
		c->data = NULL;
		break;

	case CHUNK_tIME:
		pos=m_num_chunks-1;
		c->length=7;
		c->data = (unsigned char*)malloc(c->length);

		GetSystemTime(&st);
		write_int16(&c->data[0],(int)st.wYear);
		c->data[2]= (unsigned char)st.wMonth;
		c->data[3]= (unsigned char)st.wDay;
		c->data[4]= (unsigned char)st.wHour;
		c->data[5]= (unsigned char)st.wMinute;
		c->data[6]= (unsigned char)st.wSecond;
		break;

	case CHUNK_sCAL:
		c->length=8;
		c->data = (unsigned char*)malloc(c->length);
		c->data[0]= 1;
		c->data[1]= '1'; c->data[2]= '.'; c->data[3]= '0';
		c->data[4]= 0;
		c->data[5]= '1'; c->data[6]= '.'; c->data[7]= '0';
		break;

	case CHUNK_acTL:
		c->length=8;
		c->data = (unsigned char*)calloc(c->length,1);
		write_int32(&c->data[0],1);
		write_int32(&c->data[4],0);
		break;

	case CHUNK_fcTL:
		c->length=26;
		c->data = (unsigned char*)calloc(c->length,1);
		write_int32(&c->data[4],m_width);
		write_int32(&c->data[8],m_height);
		write_int16(&c->data[20],100); // delay numerator
		write_int16(&c->data[22],100); // delay denominator
		break;

	case CHUNK_vpAg:
		c->length=9;
		c->data = (unsigned char*)malloc(c->length);
		write_int32(&c->data[0],m_width);
		write_int32(&c->data[4],m_height);
		c->data[8]=0;
		break;

	default:
		ok=0;
	}

	if(!ok) {
		mesg(MSG_W,_T("Not implemented"));
		return;
	}

	if(pos<0) pos=0;
	if(pos>m_num_chunks) pos=m_num_chunks;

	insert_chunks(pos,1,1);

	chunk[pos]=c;
	c->after_init();
	c->chunkmodified();

	fill_listbox(globals.hwndMainList);
	twpng_SetLVSelection(globals.hwndMainList,pos,1);
	modified();
}


void Png::edit_chunk(int n)
{
	int r,i;

	if(n<0 || n>=m_num_chunks) return;
	r= chunk[n]->edit();

	if(r>0) {
		if(r==2) {
			// update all
			for(i=0;i<m_num_chunks;i++) {
				update_row(globals.hwndMainList,i);
			}
		}
		else if(r==1) {
			update_row(globals.hwndMainList,n);
		}
		else {
			mesg(MSG_S,_T("internal error: change code %d"),r);
		}
		modified();
	}
}

// save to disk
// returns 1 on success, 0 on failure
int Png::write_file(const TCHAR *fn)
{
	int i;
	HCURSOR hcur;
	DWORD written;
	HANDLE fh;

	fh=CreateFile(fn,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,NULL);
	if(fh==INVALID_HANDLE_VALUE) {
		mesg(MSG_E,_T("Can") SYM_RSQUO _T("t write file (%s)"),fn);
		return 0;
	}

	hcur=SetCursor(LoadCursor(NULL,IDC_WAIT));

	WriteFile(fh,(LPVOID)signature,8,&written,NULL);

	for(i=0;i<m_num_chunks;i++) {
		chunk[i]->write_to_file(fh,0);
	}
	CloseHandle(fh);
	SetCursor(hcur);
	return 1;
}

#define TWPNG_MAX_CHUNKS 1024000

static void on_oom(void)
{
	throw "Out of memory";
}

// make sure we have room for have n chunks in the chunk[] array
void Png::ensure_chunks_alloc(int n)
{
	Chunk **new_chunk_arr;
	size_t new_numalloc;

	if(n<=m_chunks_alloc) return;

	// oops -- overran our chunks array. try to alloc a bigger one
	if(m_chunks_alloc<200) {
		new_numalloc = 200;
	}
	else {
		new_numalloc = (size_t)m_chunks_alloc*2;
	}

	if(new_numalloc > TWPNG_MAX_CHUNKS) {
		on_oom();
		return;
	}

	new_chunk_arr = (Chunk**)realloc((void*)chunk, new_numalloc*sizeof(Chunk*));
	if(!new_chunk_arr) {
		mesg(MSG_S,_T("can") SYM_RSQUO _T("t alloc memory for chunks array"));
		m_num_chunks=0;
		on_oom();
		return;
	}

	chunk = new_chunk_arr;
	m_chunks_alloc = (int)new_numalloc;
}

void Png::delete_chunk(int n)
{
	int i;
	if(n<0 || n>=m_num_chunks) return;

	twpng_CloseEditorForChunk(chunk[n]);
	delete chunk[n];
	for(i=n+1;i<m_num_chunks;i++) {
		chunk[i-1]=chunk[i];
	}
	m_num_chunks--;
}

void Png::move_chunk(int n, int delta)  // move chunk n by delta
{
	Chunk *tmpchunk;
	int moveto;
	int i;

	if(n<0 || n>=m_num_chunks) return;
	moveto=n+delta;

	if(moveto<0) moveto=0;
	if(moveto>=m_num_chunks) moveto=m_num_chunks-1;

	if(moveto==n) return;

	if(moveto>n) {        // move down
		for(i=0;i<(moveto-n);i++) {
			tmpchunk=chunk[n+i];       //swap(n+i,n+i+1)
			chunk[n+i]=chunk[n+i+1];
			chunk[n+i+1]=tmpchunk;
		}
	}
	else {        // move up      (moveto < n)
		for(i=(n-moveto-1);i>=0;i--) {
			tmpchunk=chunk[moveto+i];   //swap(moveto+i+1,moveto+i)
			chunk[moveto+i]=chunk[moveto+i+1];
			chunk[moveto+i+1]=tmpchunk;
		}
	}
}


static const unsigned char sig_png[] = {137,80,78,71,13,10,26,10};
static const unsigned char sig_mng[] = {138,77,78,71,13,10,26,10};
static const unsigned char sig_jng[] = {139,74,78,71,13,10,26,10};

void Png::set_signature()
{
	switch(m_imgtype) {
	case IMG_PNG: memcpy(signature,sig_png,8); break;
	case IMG_MNG: memcpy(signature,sig_mng,8); break;
	case IMG_JNG: memcpy(signature,sig_jng,8); break;
	}
}

int Png::read_signature(HANDLE fh)
{
	DWORD n;
	int r;

	r=ReadFile(fh,(LPVOID)signature,8,&n,NULL);
	if(!r || n<8) return 0;
	if(!memcmp(signature,sig_png,8)) return IMG_PNG;
	if(!memcmp(signature,sig_mng,8)) return IMG_MNG;
	if(!memcmp(signature,sig_jng,8)) return IMG_JNG;
	return IMG_UNKNOWN;
}

int Png::read_next_chunk(HANDLE fh, DWORD *filepos)
{
	DWORD n;
	Chunk *c;
	unsigned char fbuf[8];
	DWORD ccrc;
	int r;
	int i;

	ZeroMemory(fbuf, sizeof(fbuf));
	// first 4 bytes are the chunk data length,
	// next 4 bytes are the chunk type
	r=ReadFile(fh,(LPVOID)fbuf,8,&n,NULL);
	if(!r || n==0) {  // this is normal; we've reached the end of file
		return 0;
	}

	if(n!=8) {
		mesg(MSG_W,_T("Garbage found at end of file"));
		return 0;
	}

	// allocate a Chunk structure for this new chunk
	c = new Chunk();

	c->m_parentpng = this;  // chunks sometimes depend other chunks, ...

	c->length= read_int32(&fbuf[0]);

	memcpy(c->m_chunktype_ascii,&fbuf[4],4);
	c->m_chunktype_ascii[4]='\0';  // null-terminate for convenience
	for(i=0;i<4;i++) {
		if( !((c->m_chunktype_ascii[i]>='a' && c->m_chunktype_ascii[i]<='z') ||
			(c->m_chunktype_ascii[i]>='A' && c->m_chunktype_ascii[i]<='Z')))
		{
			mesg(MSG_W,_T("Invalid chunk type found at file position %u. ")
				_T("This may indicate garbage at the end of the file."),*filepos);
			delete c;
			return 0;
		}
	}
	c->set_chunktype_tchar_from_ascii();

	// now read the data
	if(c->length>0) {

		// A sanity test for the chunk length.
		if(c->length > m_pngfilesize - *filepos - 12) {
			mesg(MSG_W,_T("Bogus chunk length found. This may indicate garbage at the end of the file."));
			delete c;
			return 0;
		}

		c->data = (unsigned char*)malloc(c->length);
		if(!c->data) {
			mesg(MSG_S,_T("Can") SYM_RSQUO _T("t allocate memory for chunk"));
			delete c;
			return 0;
		}
	}

	r=ReadFile(fh,(LPVOID)c->data,c->length,&n,NULL);
	if(!r || n!=c->length) {
		mesg(MSG_W,_T("Garbage found at end of file"));
		delete c;
		return 0;
	}

	// now read the CRC
	r=ReadFile(fh,(LPVOID)fbuf,4,&n,NULL);
	if(!r || n!=4) {
		mesg(MSG_W,_T("Garbage found at end of file"));
		delete c;
		return 0;
	}

	c->m_crc = read_int32(&fbuf[0]);

	// check the crc
	ccrc=c->calc_crc();

	if(c->m_crc != ccrc) {
		mesg(MSG_W,_T("Incorrect crc for %s chunk (is %08x, should be %08x)"),
			c->m_chunktype_tchar, c->m_crc, ccrc);
		c->m_crc=ccrc;  // correct it
	}

	ensure_chunks_alloc(m_num_chunks+1);  // make sure chunks array is large enough
	chunk[m_num_chunks++]=c;

	c->after_init();

	*filepos += c->length + 12;
	return 1;
}

void Png::initialize()
{
	m_valid = 0;
	m_num_chunks=0;
	chunk=NULL;
	m_chunks_alloc=0;
	StringCchCopy(m_filename,MAX_PATH,_T("untitled"));
	m_named=0;
	m_dirty=0;
	m_scratch=0;
	m_imgtype=IMG_PNG;
	m_pngfilesize = 0;
	m_width=1;
	m_height=1;
	m_bitdepth=1;
	m_colortype=0;
	m_stream_phase = 0;
	m_stream_curchunk = 0;
	m_stream_curpos_in_curchunk = 0;
	memcpy(signature,sig_png,8);

	ensure_chunks_alloc(200);
}

Png::Png()
{
	initialize();
	m_valid=1;
}

// The reason for two filename params is to support the plan to add
// support for running "filter" utilities. A filter will accept a
// (temporary) PNG file, and write a new (temporary) PNG file, which
// we will read as a replacement for the original file.
Png::Png(const TCHAR *load_fn, const TCHAR *save_fn)
{
	int okay;
	HANDLE fh;
	DWORD filepos;

	initialize();
	m_valid=0;

	StringCchCopy(m_filename,MAX_PATH,save_fn);
	m_named=1;
	m_dirty=0;

	m_colortype=255;  // random invalid value

	fh=CreateFile(load_fn,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,NULL);
	if(fh==INVALID_HANDLE_VALUE) {
		mesg(MSG_E,_T("Can") SYM_RSQUO _T("t open file (%s)"),load_fn);
		return;
	}

	m_pngfilesize=GetFileSize(fh,NULL);  // note, this will fail for files over 4 GB

	m_imgtype=read_signature(fh);
	okay=(m_imgtype>=1);

	if(!okay) {
		CloseHandle(fh);
		SetForegroundWindow(globals.hwndMain);
		mesg(MSG_E,_T("Unrecognized file format\n\nThis is not a valid PNG file."));
		return;
	}

	filepos = 8;

	while(okay) {
		okay=read_next_chunk(fh,&filepos);
	}

	CloseHandle(fh);
	m_valid=1;
}


Png::~Png()
{
	int i;

	// This document is going away; close the modeless editor and parameters viewer.
	if(!m_scratch) {
		twpng_CloseModelessEditors();
		twpng_CloseAIParamsView();
	}

	if(chunk) {
		// free individual chunks
		for(i=0;i<m_num_chunks;i++) {
			if(chunk[i]) delete chunk[i];
		}

		// free chunk list
		free(chunk);
	}
}


///////////////////////////////////////////////////////////////////

static int RegisterClasses()
{
	WNDCLASS  wc;

	ZeroMemory((void*)&wc, sizeof(WNDCLASS));
	wc.style = CS_DBLCLKS|CS_HREDRAW;
	wc.lpfnWndProc = (WNDPROC)WndProcMain;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = globals.hInst;
	wc.hIcon = LoadIcon(globals.hInst,_T("ICONMAIN"));
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
	wc.lpszMenuName =  _T("MENUMAIN");
	wc.lpszClassName = _T("TWEAKPNGMAIN");
	if(!RegisterClass(&wc)) {
		return 0;
	}

	wc.style= CS_HREDRAW|CS_VREDRAW;
	wc.lpfnWndProc = (WNDPROC)WndProcEditPal;
	wc.hIcon = NULL;
	wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
	wc.lpszMenuName =  NULL;
	wc.lpszClassName = _T("TWEAKPNGEDITPAL");
	if(!RegisterClass(&wc)) {
		return 0;
	}

	return 1;
}


int twpng_StoreWindowPos(HWND hwnd, struct windowpos_struct *q)
{
	WINDOWPLACEMENT wp;

	if(!IsWindow(hwnd)) return 0;
	ZeroMemory(&wp,sizeof(wp));
	wp.length=sizeof(wp);
	if(!GetWindowPlacement(hwnd,&wp)) return 0;
	q->x= wp.rcNormalPosition.left;
	q->y= wp.rcNormalPosition.top;
	q->w= wp.rcNormalPosition.right - q->x;
	q->h= wp.rcNormalPosition.bottom - q->y;
	q->max= (wp.showCmd == SW_SHOWMAXIMIZED);  // this isn't used
	return 1;
}

void twpng_SetWindowPos(HWND hwnd, const struct windowpos_struct *q)
{
	WINDOWPLACEMENT wp;

	// We don't save all the information that SetWindowPlacement requires.
	// I hope it's okay to call GetWindowPlacement to get the current
	// placement information, modify only the position, then turn around
	// and call SetWindowPlacement.

	ZeroMemory(&wp,sizeof(wp));
	wp.length=sizeof(wp);
	GetWindowPlacement(hwnd,&wp);
	wp.rcNormalPosition.left = q->x;
	wp.rcNormalPosition.top = q->y;
	wp.rcNormalPosition.right = q->x + q->w;
	wp.rcNormalPosition.bottom = q->y + q->h;
	SetWindowPlacement(hwnd,&wp);
}

static int SaveSettings()
{
	HKEY key;
	LONG r;
	DWORD disp, tmpd;
	TCHAR v[24];
	int i;

	r=RegCreateKeyEx(HKEY_CURRENT_USER,globals.twpng_reg_key,0,NULL,
		REG_OPTION_NON_VOLATILE,KEY_ALL_ACCESS,NULL,&key,&disp);
	if(r!=ERROR_SUCCESS) {
		mesg(MSG_E,_T("Cannot save settings to registry"));
		return 0;
	}

	for(i=0;i<TWPNG_NUMTOOLS;i++) {
		StringCchPrintf(v,24,_T("tool%dn"),i);
		r=RegSetValueEx(key,v,0,REG_SZ,(LPBYTE)globals.tools[i].name  ,(DWORD)sizeof(TCHAR)*(1+lstrlen(globals.tools[i].name)));

		StringCchPrintf(v,24,_T("tool%dc"),i);
		r=RegSetValueEx(key,v,0,REG_SZ,(LPBYTE)globals.tools[i].cmd   ,(DWORD)sizeof(TCHAR)*(1+lstrlen(globals.tools[i].cmd)));

		StringCchPrintf(v,24,_T("tool%dp"),i);
		r=RegSetValueEx(key,v,0,REG_SZ,(LPBYTE)globals.tools[i].params,(DWORD)sizeof(TCHAR)*(1+lstrlen(globals.tools[i].params)));
	}

	r=RegSetValueEx(key,_T("deflate_lvl"),0,REG_DWORD,(LPBYTE)&globals.compression_level,sizeof(globals.compression_level));
	r=RegSetValueEx(key,_T("show_viewer"),0,REG_DWORD,(LPBYTE)&globals.autoopen_viewer,sizeof(DWORD));
	r=RegSetValueEx(key,_T("use_gamma"),0,REG_DWORD,(LPBYTE)&globals.use_gamma,sizeof(DWORD));
	r=RegSetValueEx(key,_T("use_custombg"),0,REG_DWORD,(LPBYTE)&globals.use_custombg,sizeof(DWORD));
	r=RegSetValueEx(key,_T("use_imagebg"),0,REG_DWORD,(LPBYTE)&globals.use_imagebg,sizeof(DWORD));
	r=RegSetValueEx(key,_T("windowbg"),0,REG_DWORD,(LPBYTE)&globals.window_bgcolor,sizeof(DWORD));
	r=RegSetValueEx(key,_T("zoom"),0,REG_DWORD,(LPBYTE)&globals.vsize,sizeof(DWORD));
	r=RegSetValueEx(key,_T("open_params"),0,REG_DWORD,(LPBYTE)&globals.open_params_on_load,sizeof(DWORD));
	r=RegSetValueEx(key,_T("theme"),0,REG_DWORD,(LPBYTE)&globals.theme_pref,sizeof(DWORD));

	if(IsWindow(globals.hwndMainList)) {
		for(i=0;i<5;i++) {
			globals.window_prefs.column_width[i]=ListView_GetColumnWidth(globals.hwndMainList,i);
		}
	}

	twpng_StoreWindowPos(globals.hwndMain,&globals.window_prefs.main);

	RegSetValueEx(key,_T("windowpos"),0,REG_BINARY,(LPBYTE)(&globals.window_prefs),sizeof(globals.window_prefs));

	tmpd = (DWORD)globals.custombgcolor;
	RegSetValueEx(key,_T("bgcolor"),0,REG_DWORD,(LPBYTE)&tmpd,sizeof(DWORD));
	RegSetValueEx(key,_T("custcolors"),0,REG_BINARY,(LPBYTE)&globals.custcolors,16*sizeof(COLORREF));

	RegCloseKey(key);

	return 1;
}

static void ReadSettings()
{
	HKEY key;
	LONG r;
	DWORD datasize,tmpd;
	int i;
	TCHAR v[24];
	//prefs_t tmp_prefs;

	globals.compression_level=Z_DEFAULT_COMPRESSION;
	globals.use_gamma=1;
	globals.custombgcolor = RGB(128,128,128);
	globals.use_custombg = 1;
	globals.use_imagebg = 1;
	for(i=0;i<16;i++) globals.custcolors[i] = RGB(0,0,0);
	globals.autoopen_viewer=0;
	globals.window_bgcolor=TWPNG_WBG_SAMEASIMAGE;
	globals.theme_pref=TWPNG_THEME_DARK;

	for(i=0;i<TWPNG_NUMTOOLS;i++) {
		StringCchCopy(globals.tools[i].name,MAX_TOOL_NAME,_T(""));
		StringCchCopy(globals.tools[i].cmd,MAX_TOOL_CMD,_T(""));
		StringCchCopy(globals.tools[i].params,MAX_TOOL_PARAMS,_T(""));
	}
	StringCchCopy(globals.tools[0].name,MAX_TOOL_NAME,_T("Default viewer"));
	globals.window_prefs.version = 1;
	globals.window_prefs.column_width[0]=50;
	globals.window_prefs.column_width[1]=60;
	globals.window_prefs.column_width[2]=60;
	globals.window_prefs.column_width[3]=130;
	globals.window_prefs.column_width[4]=600;

	globals.window_prefs.main.x=0;
	globals.window_prefs.main.y=0;
	globals.window_prefs.main.w=600;
	globals.window_prefs.main.h=300;
	globals.window_prefs.main.max=0;

	globals.window_prefs.text.x=0;
	globals.window_prefs.text.y=0;
	globals.window_prefs.text.w=500;
	globals.window_prefs.text.h=300;
	globals.window_prefs.text.max=0;

	globals.window_prefs.plte.x=0;
	globals.window_prefs.plte.y=0;
	globals.window_prefs.plte.w=490;
	globals.window_prefs.plte.h=440;
	globals.window_prefs.plte.max=0;

	globals.window_prefs.viewer.x=0;
	globals.window_prefs.viewer.y=100;
	globals.window_prefs.viewer.w=350;
	globals.window_prefs.viewer.h=250;
	globals.window_prefs.viewer.max=0;

	globals.window_prefs.aiparams.x=0;
	globals.window_prefs.aiparams.y=0;
	globals.window_prefs.aiparams.w=0;   // 0 => use the dialog's default size/centering
	globals.window_prefs.aiparams.h=0;
	globals.window_prefs.aiparams.max=0;

	globals.window_prefs.aifolder.x=0;
	globals.window_prefs.aifolder.y=0;
	globals.window_prefs.aifolder.w=0;   // 0 => use the dialog's default size/centering
	globals.window_prefs.aifolder.h=0;
	globals.window_prefs.aifolder.max=0;

	if(RegOpenKeyEx(HKEY_CURRENT_USER,globals.twpng_reg_key,0,KEY_READ,&key)
		!= ERROR_SUCCESS) return;

	for(i=0;i<TWPNG_NUMTOOLS;i++) {
		StringCchPrintf(v,24,_T("tool%dn"),i); datasize= sizeof(TCHAR)*(MAX_TOOL_NAME-1);
		r=RegQueryValueEx(key,v,NULL,NULL,(LPBYTE)globals.tools[i].name,&datasize);
		globals.tools[i].name[MAX_TOOL_NAME-1]='\0';

		StringCchPrintf(v,24,_T("tool%dc"),i); datasize= sizeof(TCHAR)*(MAX_TOOL_CMD-1);
		r=RegQueryValueEx(key,v,NULL,NULL,(LPBYTE)globals.tools[i].cmd,&datasize);
		globals.tools[i].cmd[MAX_TOOL_CMD-1]='\0';

		StringCchPrintf(v,24,_T("tool%dp"),i); datasize= sizeof(TCHAR)*(MAX_TOOL_PARAMS-1);
		r=RegQueryValueEx(key,v,NULL,NULL,(LPBYTE)globals.tools[i].params,&datasize);
		globals.tools[i].params[MAX_TOOL_PARAMS-1]='\0';
	}

	datasize=sizeof(globals.compression_level);
	r=RegQueryValueEx(key,_T("deflate_lvl"),NULL,NULL,(LPBYTE)&globals.compression_level,&datasize);

	datasize=sizeof(globals.window_prefs);
	r=RegQueryValueEx(key,_T("windowpos"),NULL,NULL,(LPBYTE)(&globals.window_prefs),&datasize);

	datasize=sizeof(DWORD);
	r=RegQueryValueEx(key,_T("show_viewer"),NULL,NULL,(LPBYTE)(&globals.autoopen_viewer),&datasize);

	datasize=sizeof(DWORD);
	r=RegQueryValueEx(key,_T("use_gamma"),NULL,NULL,(LPBYTE)(&globals.use_gamma),&datasize);
	datasize=sizeof(DWORD);
	r=RegQueryValueEx(key,_T("use_custombg"),NULL,NULL,(LPBYTE)(&globals.use_custombg),&datasize);
	datasize=sizeof(DWORD);
	r=RegQueryValueEx(key,_T("use_imagebg"),NULL,NULL,(LPBYTE)(&globals.use_imagebg),&datasize);
	datasize=sizeof(DWORD);
	r=RegQueryValueEx(key,_T("windowbg"),NULL,NULL,(LPBYTE)(&globals.window_bgcolor),&datasize);
	datasize=sizeof(DWORD);
	r=RegQueryValueEx(key,_T("zoom"),NULL,NULL,(LPBYTE)(&globals.vsize),&datasize);
	datasize=sizeof(DWORD);
	r=RegQueryValueEx(key,_T("open_params"),NULL,NULL,(LPBYTE)(&globals.open_params_on_load),&datasize);
	datasize=sizeof(DWORD);
	r=RegQueryValueEx(key,_T("theme"),NULL,NULL,(LPBYTE)(&globals.theme_pref),&datasize);
	if(globals.theme_pref<TWPNG_THEME_SYSTEM || globals.theme_pref>TWPNG_THEME_DARK)
		globals.theme_pref=TWPNG_THEME_DARK;

	datasize=sizeof(DWORD);
	tmpd = 0;
	r=RegQueryValueEx(key,_T("bgcolor"),NULL,NULL,(LPBYTE)&tmpd,&datasize);
	if(r==ERROR_SUCCESS) {
		globals.custombgcolor = (COLORREF)tmpd;
	}

	datasize = 16*sizeof(COLORREF);
	r=RegQueryValueEx(key,_T("custcolors"),NULL,NULL,(LPBYTE)(&globals.custcolors),&datasize);
	RegCloseKey(key);
}

// Sets globals.file_from_cmdline.
static void get_filename_from_cmdline(const TCHAR *lpCmdLine)
{
	TCHAR buf[MAX_PATH];
	int len;
	DWORD ret;

	// "/aiparams <file>" (the Explorer menu entry): show only the AI parameters viewer.
	if(!_tcsnicmp(lpCmdLine, _T("/aiparams"), 9) &&
	   (lpCmdLine[9]==_T(' ') || lpCmdLine[9]==0)) {
		globals.aiparams_only = 1;
		lpCmdLine += 9;
		while(*lpCmdLine==_T(' ')) lpCmdLine++;
	}

	if(lpCmdLine[0]=='"') { // if quoted, strip quotes
		StringCbCopy(buf,sizeof(buf),&lpCmdLine[1]);
		len = lstrlen(buf);
		if(len>0 && buf[len-1]=='"') buf[len-1]='\0';
	}
	else {
		StringCbCopy(buf,sizeof(buf),lpCmdLine);
	}

	// Figure out the full filename.
	ret = GetFullPathName(buf,MAX_PATH,globals.file_from_cmdline,NULL);
	if(!ret) {
		StringCchCopy(globals.file_from_cmdline,MAX_PATH,_T(""));
	}
}

#ifndef _In_
#define _In_
#define _In_opt_
#endif

int WINAPI _tWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance,
	_In_ LPTSTR lpCmdLine, _In_ int nShowCmd)
{
	MSG msg;
	HACCEL hAccTable;
	int p;

	ZeroMemory(&globals,sizeof(struct globals_struct));

	globals.hInst=hInstance;
	globals.dlgs_open = 0;
	globals.twpng_homepage = TWEAKPNG_HOMEPAGE;
	globals.twpng_reg_key = _T("SOFTWARE\\Generic\\TweakPNG");

#ifdef UNICODE
	globals.unicode_supported=1;
#else
	globals.unicode_supported=0;
#endif

#ifdef TWPNG_HAVE_ZLIB
	globals.zlib_available=1;
#else
	globals.zlib_available=0;
#endif

	globals.stbar_height=0;
	globals.hwndMain=NULL;
	globals.hwndMainList=NULL;
	globals.hwndStBar=NULL;
	png=NULL;
	g_viewer=NULL;
	globals.timer_set=0;
	globals.vborder=4;
	globals.vsize=TWPNG_VS_FIT;
	globals.viewer_correct_nonsquare=1;
	globals.open_params_on_load=1;

	INITCOMMONCONTROLSEX icc;
	ZeroMemory(&icc,sizeof(icc));
	icc.dwSize=sizeof(icc);
	icc.dwICC=ICC_LISTVIEW_CLASSES|ICC_BAR_CLASSES|ICC_WIN95_CLASSES|ICC_USEREX_CLASSES|ICC_LINK_CLASS;
	InitCommonControlsEx(&icc);

	globals.pngchunk_cf = RegisterClipboardFormat(_T("pngchunks"));

	if(!RegisterClasses()) return 0;
	BufferedPaintInit();
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
#ifdef TWPNG_SUPPORT_VIEWER
	Viewer::GlobalInit();
#endif
	globals.hcurDrag2 = LoadCursor(globals.hInst,_T("DRAGCURSOR2"));

	StringCchCopy(globals.last_open_dir,MAX_PATH,_T(""));

	get_filename_from_cmdline(lpCmdLine);

	make_crc_table();

	StringCchCopy(globals.orig_dir,MAX_PATH,_T(""));
	StringCchCopy(globals.home_dir,MAX_PATH,_T(""));

	GetCurrentDirectory(MAX_PATH,globals.orig_dir);

	// record the directory we were loaded from
	if(GetModuleFileName(globals.hInst,globals.home_dir,MAX_PATH)) {
		// strip filename, leaving directory path
		p=lstrlen(globals.home_dir)-1;
		while(p>=0) {
			if(globals.home_dir[p]=='\\') {
				globals.home_dir[p]='\0';
				break;
			}
			p--;
		}
	}

	ReadSettings();
	// The app-wide dark/light mode must be set before any window is created.
	twpng_ResolveTheme();
	twpng_SetAppMode();

	globals.hwndMain = CreateWindow(
		_T("TWEAKPNGMAIN"),_T("TweakPNG"),
		(globals.aiparams_only ? 0 : WS_VISIBLE)|WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
		CW_USEDEFAULT,CW_USEDEFAULT,
		globals.window_prefs.main.w,globals.window_prefs.main.h,
		NULL,NULL,globals.hInst,NULL);
	if (!globals.hwndMain) return 0;

	DragAcceptFiles(globals.hwndMain,TRUE);
	hAccTable=LoadAccelerators(globals.hInst,_T("ACCELTABLE"));

	while(GetMessage(&msg,NULL,0,0)){
		if(twpng_IsModelessEditorMessage(&msg)) continue;
		if(g_hwndAIParams && twpng_AIParamsKey(&msg)) continue;
		if(g_hwndAIParams && IsDialogMessage(g_hwndAIParams,&msg)) continue;
		if(g_hwndAIFolder && IsDialogMessage(g_hwndAIFolder,&msg)) continue;
		if (!TranslateAccelerator(globals.hwndMain, hAccTable, &msg)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

#ifdef TWPNG_SUPPORT_VIEWER
	Viewer::GlobalDestroy();
#endif
	BufferedPaintUnInit();
	CoUninitialize();
	return (int)msg.wParam;
}

struct filename_path_struct {
	const TCHAR *full_fn;
	const TCHAR *base_fn; // pointer into .full_fn
	TCHAR path[MAX_PATH];
};

// Caller sets fnp->full_fn; we calculate ->path and ->base_fn.
static int parse_filename_path(struct filename_path_struct *fnp)
{
	const TCHAR *sep;
	size_t sep_pos;

	sep = _tcsrchr(fnp->full_fn,'\\');
	if(!sep) {
		// Filename does not include a path
		fnp->base_fn = fnp->full_fn;
		fnp->path[0] = '\0';
		return 0;
	}

	sep_pos = sep - fnp->full_fn; // Pointer arithmetic

	// Make a copy of the full path...
	StringCchCopy(fnp->path,MAX_PATH,fnp->full_fn);
	// and truncate it after the last backslash.
	fnp->path[sep_pos+1] = '\0';

	fnp->base_fn = &fnp->full_fn[sep_pos+1];
	return 1;
}

// set the titlebar text for the main window
static void SetTitle(Png *p)
{
	TCHAR buf[1024];
	int buf_valid = 0;
	const TCHAR *basefn = NULL;
	int ret;

	if(!p) {
		SetWindowText(globals.hwndMain,_T("TweakPNG"));
		return;
	}

	if(p->m_named) {
		struct filename_path_struct fnp;

		// TODO: The filename is parsed more often than necessary.
		ZeroMemory(&fnp, sizeof(struct filename_path_struct));
		fnp.full_fn = p->m_filename;
		ret = parse_filename_path(&fnp);

		if(ret) {
			// Special format for filenames with paths.
			StringCbPrintf(buf,sizeof(buf),_T("%s%s (%s) - TweakPNG"),fnp.base_fn,
				p->m_dirty?_T("*"):_T(""),fnp.path);
			buf_valid = 1;
		}
	}

	if(!buf_valid) {
		StringCbPrintf(buf,sizeof(buf),_T("%s%s - TweakPNG"),p->m_filename,
			p->m_dirty?_T("*"):_T(""));
	}

	SetWindowText(globals.hwndMain,buf);
}

// Unconditionally close the current document, and update the UI.
static void ClosePngDocument()
{
	if(png) {
		// ~Png() also closes the modeless editor and AI params viewer.
		delete png;
		png=NULL;
	}
	else if(g_foreignFn[0]) {
		// No Png to destruct, but the AI params viewer may still be up showing this
		// foreign file's text — close it for consistency with the PNG case.
		twpng_CloseAIParamsView();
	}
	if(globals.hwndMainList) {
		ListView_DeleteAllItems(globals.hwndMainList);
	}
	// Also clear any foreign (JPEG/WebP) state so the title, status bar, and the
	// View AI Parameters menu item all return to the "no document" state together.
	g_foreignFn[0] = 0;
	g_foreignKind = 0;
	g_foreignGen = NULL;
	SetTitle(NULL);
	update_viewer_filename();
	update_status_bar_and_viewer();
}

// Unconditionally close the current document, and create a new empty document.
static void NewPng()
{
	if(png) {
		delete png;
		png=NULL;
	}
	if(globals.hwndMainList) {
		ListView_DeleteAllItems(globals.hwndMainList);
	}
	png=new Png();
	SetTitle(png);
	update_viewer_filename();
	update_status_bar_and_viewer();
}

// If enabled, find the first tEXt chunk whose keyword is "parameters" (the keyword used
// by AI image generators) and open its editor automatically.
static void twpng_AutoOpenParamsText()
{
	if(!globals.open_params_on_load || !png) return;
	// First match wins: A1111 "parameters", then ComfyUI "prompt".
	static const TCHAR *keys[] = { _T("parameters"), _T("prompt"), NULL };
	for(int ki=0; keys[ki]; ki++) {
		for(int i=0; i<png->m_num_chunks; i++) {
			Chunk *c = png->chunk[i];
			if(c && (c->m_chunktype_id==CHUNK_tEXt ||
			         c->m_chunktype_id==CHUNK_zTXt ||
			         c->m_chunktype_id==CHUNK_iTXt)) {
				struct keyword_info_struct kw;
				if(c->get_keyword_info(&kw) && !lstrcmp(kw.keyword,keys[ki])) {
					twpng_SetLVSelection(globals.hwndMainList,i,1);
					twpng_ViewAIParams(globals.hwndMain);
					return;
				}
			}
		}
	}
}

// handles loading a new png file
static int OpenPngByName(const TCHAR *fn)
{
	// Sniff the file format first so we can route JPEG/WebP through a metadata-only
	// path that leaves the chunk table empty.
	HANDLE fh = CreateFile(fn, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if(fh==INVALID_HANDLE_VALUE) {
		mesg(MSG_E,_T("Can") SYM_RSQUO _T("t open file (%s)"),fn);
		return 0;
	}
	unsigned char hdr[16] = {0};
	DWORD got = 0;
	ReadFile(fh, hdr, sizeof(hdr), &got, NULL);
	CloseHandle(fh);
	int foreignKind = twpng_SniffForeignKind(hdr, got);

	if(png) { delete png; png=NULL; }
	else if(g_foreignFn[0]) {
		// Replacing a foreign file with another file — ~Png() didn't run, so close any
		// stale AI viewer left over from the previous foreign file.
		twpng_CloseAIParamsView();
	}
	ListView_DeleteAllItems(globals.hwndMainList);

	if(foreignKind) {
		// JPEG / WebP: no PNG document, just show the filename and try to extract AI
		// metadata from EXIF UserComment.
		StringCbCopy(g_foreignFn, sizeof(g_foreignFn), fn);
		g_foreignKind = foreignKind;
		TCHAR titleBuf[1024];
		struct filename_path_struct fnp;
		ZeroMemory(&fnp, sizeof(fnp));
		fnp.full_fn = (TCHAR*)fn;
		if(parse_filename_path(&fnp))
			StringCbPrintf(titleBuf, sizeof(titleBuf), _T("%s (%s) - TweakPNG"), fnp.base_fn, fnp.path);
		else
			StringCbPrintf(titleBuf, sizeof(titleBuf), _T("%s - TweakPNG"), fn);
		SetWindowText(globals.hwndMain, titleBuf);
		update_viewer_filename();
		update_status_bar_and_viewer();

		TCHAR *aiText = NULL, extra[200] = _T("");
		twpng_ReadForeignAIParams(foreignKind, fn, &aiText);
		g_foreignGen = twpng_DetectGeneratorFromText(aiText);
		if(aiText) {
			if(globals.open_params_on_load) {
				if(foreignKind==3 || foreignKind==4) twpng_ReadMp4VideoInfo(fn, extra, 200);
				twpng_ShowAIParams(aiText, extra, fn, TRUE);
			}
			free(aiText);
		}
		update_status_bar_and_viewer();   // refresh now that g_foreignGen is set
		return 1;
	}

	// PNG / MNG / JNG path.
	g_foreignFn[0] = 0;
	g_foreignKind = 0;
	g_foreignGen = NULL;
	png = new Png(fn, fn);

	if(!png->m_valid) {
		delete png;
		png=NULL;
		SetTitle(NULL);
		update_viewer_filename();
		update_status_bar_and_viewer();
		return 0;
	}

	png->fill_listbox(globals.hwndMainList);
	SetTitle(png);
	update_viewer_filename();
	update_status_bar_and_viewer();

	twpng_AutoOpenParamsText();

	return 1;
}

static int OpenPngFromMenu(HWND hwnd)
{
	TCHAR fn[MAX_PATH];
	OPENFILENAME ofn;
	BOOL bRet;

	StringCchCopy(fn,MAX_PATH,_T(""));

	ZeroMemory(&ofn,sizeof(OPENFILENAME));

	ofn.lStructSize=sizeof(OPENFILENAME);
	ofn.hwndOwner=hwnd;
	ofn.hInstance=NULL;
	ofn.lpstrFilter=_T("Images and videos\0*.png;*.mng;*.jng;*.jpg;*.jpeg;*.webp;*.mp4;*.mov;*.m4v\0PNG, MNG, JNG\0*.png;*.mng;*.jng\0JPEG\0*.jpg;*.jpeg\0WebP\0*.webp\0MP4, MOV\0*.mp4;*.mov;*.m4v\0All files\0*.*\0\0");
	ofn.nFilterIndex=1;
	ofn.lpstrTitle=_T("Open image file");
	if(lstrlen(globals.last_open_dir))
		ofn.lpstrInitialDir=globals.last_open_dir;  // else NULL ==> current dir
	ofn.lpstrFile=fn;
	ofn.nMaxFile=MAX_PATH;
	ofn.Flags=OFN_FILEMUSTEXIST|OFN_HIDEREADONLY|OFN_NOCHANGEDIR;

	globals.dlgs_open++;
	bRet=GetOpenFileName(&ofn);
	globals.dlgs_open--;
	if(!bRet) {
		return 0;  // user canceled
	}

	StringCchCopy(globals.last_open_dir,MAX_PATH,fn);
	globals.last_open_dir[ofn.nFileOffset]='\0'; // chop off filename; save the path for next time

	return OpenPngByName(fn);
}

static void ReopenPngDocument()
{
	TCHAR fn[MAX_PATH];
	int x;

	if(!png) return;
	if(!png->m_named) return;

	if(png->m_dirty) {
		x=twpng_MessageBox(globals.hwndMain,_T("Reload and lose changes?"),
			_T("TweakPNG"),MB_OKCANCEL|MB_ICONWARNING|MB_DEFBUTTON1);
		if(x!=IDOK) return;
	}

	// Use a copy of the filename, because OpenPngByName will
	// delete the png object.
	StringCbCopy(fn,sizeof(fn),png->m_filename);
	OpenPngByName(fn);
}

void DroppedFiles(HDROP hDrop)
{
	UINT num_files;
	UINT rv;
	DWORD attr;
	TCHAR fn[MAX_PATH];
	int fnlen;

	// Ask how many files were dropped.
	num_files = DragQueryFile(hDrop,0xFFFFFFFF,NULL,0);
	if(num_files!=1) goto done;

	// Look up the filename of the dropped file.
	rv=DragQueryFile(hDrop,0,fn,MAX_PATH);
	if(!rv) goto done;

	// Don't try to open directories.
	attr = GetFileAttributes(fn);
	if(attr&FILE_ATTRIBUTE_DIRECTORY) goto done;

	// Look at the file extension, to guess what type of file was dropped.
	// We have special handling of .chunk and .icc files.
	fnlen = lstrlen(fn);
	if(fnlen>=7 && !_tcsicmp(&fn[fnlen-6],_T(".chunk"))) {
		// TODO: How to decide where to insert the new chunk?
		int pos=GetLVFocus(globals.hwndMainList);
		ImportChunkByFilename(fn,pos);
	}
	else if(fnlen>=5 && !_tcsicmp(&fn[fnlen-4],_T(".icc"))) {
		ImportICCProfileByFilename(png,fn);
	}
	else {
		// Assume this is a PNG/MNG/JNG file to be opened.
		if(OkToClosePNG()) {
			OpenPngByName(fn);
		}
	}

done:
	DragFinish(hDrop);
}

// Do a few validity checks on the number and ordering of chunks
// sets pointer to string describing the problem. Setting msg to NULL
// makes this function display a dialog box about any errors (or even
// if all is okay)
// This function only checks the overall structure, not parameter values,
// etc.
//
// msgmode==0: prints results (good or bad), returns 1 if okay, 0 if errors found
// msgmode==1: if errors found, asks if you want to save, returns 0 if user says no, otherwise 1
int Png::check_validity(int msgmode)
{
	const TCHAR *m;
	int i;
	int e=0;   // error count
	int t;
	int prev_chunk;
	TCHAR buf[500];

	int idat_seen=0, plte_seen=0, chrm_seen=0, gama_seen=0;
	int iccp_seen=0, sbit_seen=0, srgb_seen=0, bkgd_seen=0;
	int hist_seen=0, trns_seen=0, phys_seen=0, time_seen=0;
	int ster_seen=0, offs_seen=0, pcal_seen=0, scal_seen=0;
	int dsig_pending=0;
	int dsig_nesting_level=0;

	m=_T("No problems found.");

	if(m_imgtype!=IMG_PNG) {
		if(msgmode==0) {
			e++; m=_T("Can only check PNG files");
		}
		goto done;
	}

	i=0;
	prev_chunk=CHUNK_UNKNOWN;

	while(i<m_num_chunks && e==0) {
		t=chunk[i]->m_chunktype_id;   // to save typing

		if(chunk[i]->is_critical() && t!=CHUNK_IHDR && t!=CHUNK_PLTE
			&& t!=CHUNK_IDAT && t!=CHUNK_IEND)
		{
			e++; m=_T("Unrecognized critical chunk");
		}

		if(dsig_pending && t!=CHUNK_dSIG) {
			if(t!=CHUNK_IEND) {
				e++; m=_T("Misplaced dSIG chunk");
			}
			dsig_pending=0;
		}

		if(i==0 && t!=CHUNK_IHDR) {
			e++; m=_T("First chunk must be IHDR");
		}
		else if((i==m_num_chunks-1) && t!=CHUNK_IEND) {
			e++; m=_T("Last chunk must be IEND");
		}
		else {
			switch(t) {
			case CHUNK_IHDR:
				if(i!=0) {
					e++; m=_T("Misplaced or extra IHDR");
				}
				break;
			case CHUNK_IEND:
				if(i!=m_num_chunks-1) {
					e++; m=_T("Misplaced or extra IEND");
				}
				break;
			case CHUNK_IDAT:
				if(idat_seen && prev_chunk!=CHUNK_IDAT) {
					e++; m=_T("IDAT chunks must be consecutive");
				}
				idat_seen++;
				break;
			case CHUNK_PLTE:
				if(plte_seen) { e++; m=_T("Multiple PLTE chunks not allowed"); }
				else if(m_colortype==0 || m_colortype==4) {
					e++; m=_T("PLTE chunk not allowed in grayscale image");
				}
				else if(idat_seen) { e++; m=_T("PLTE must appear before IDAT"); }
				else if(bkgd_seen) { e++; m=_T("bKGD must appear after PLTE"); }
				else if(hist_seen) { e++; m=_T("hIST must appear after PLTE"); }
				else if(trns_seen) { e++; m=_T("tRNS must appear after PLTE"); }
				plte_seen++;
				break;
			case CHUNK_tIME:
				if(time_seen) { e++; m=_T("Multiple tIME chunks not allowed"); }
				time_seen++;
				break;
			case CHUNK_cHRM:
				if(chrm_seen) { e++; m=_T("Multiple cHRM chunks not allowed"); }
				else if(plte_seen) { e++; m=_T("cHRM must appear before PLTE"); }
				else if(idat_seen) { e++; m=_T("cHRM must appear before IDAT"); }
				chrm_seen++;
				break;
			case CHUNK_gAMA:
				if(gama_seen) { e++; m=_T("Multiple gAMA chunks not allowed"); }
				else if(plte_seen) { e++; m=_T("gAMA must appear before PLTE"); }
				else if(idat_seen) { e++; m=_T("gAMA must appear before IDAT"); }
				gama_seen++;
				break;
			case CHUNK_iCCP:
				if(iccp_seen) { e++; m=_T("Multiple iCCP chunks not allowed"); }
				else if(plte_seen) { e++; m=_T("iCCP must appear before PLTE"); }
				else if(idat_seen) { e++; m=_T("iCCP must appear before IDAT"); }
				iccp_seen++;
				break;
			case CHUNK_sBIT:
				if(sbit_seen) { e++; m=_T("Multiple sBIT chunks not allowed"); }
				else if(plte_seen) { e++; m=_T("sBIT must appear before PLTE"); }
				else if(idat_seen) { e++; m=_T("sBIT must appear before IDAT"); }
				sbit_seen++;
				break;
			case CHUNK_sRGB:
				if(srgb_seen) { e++; m=_T("Multiple sRGB chunks not allowed"); }
				else if(plte_seen) { e++; m=_T("sRGB must appear before PLTE"); }
				else if(idat_seen) { e++; m=_T("sRGB must appear before IDAT"); }
				srgb_seen++;
				break;
			case CHUNK_sTER:
				if(ster_seen) { e++; m=_T("Multiple sTER chunks not allowed"); }
				else if(idat_seen) { e++; m=_T("sTER must appear before IDAT"); }
				ster_seen++;
				break;
			case CHUNK_bKGD:
				if(bkgd_seen) { e++; m=_T("Multiple bKGD chunks not allowed"); }
				else if(idat_seen) { e++; m=_T("bKGD must appear before IDAT"); }
				bkgd_seen++;
				break;
			case CHUNK_hIST:
				if(hist_seen) { e++; m=_T("Multiple hIST chunks not allowed"); }
				else if(idat_seen) { e++; m=_T("hIST must appear before IDAT"); }
				hist_seen++;
				break;
			case CHUNK_tRNS:
				if(trns_seen) { e++; m=_T("Multiple tRNS chunks not allowed"); }
				else if(idat_seen) { e++; m=_T("tRNS must appear before IDAT"); }
				trns_seen++;
				break;
			case CHUNK_pHYs:
				if(phys_seen) { e++; m=_T("Multiple pHYs chunks not allowed"); }
				else if(idat_seen) { e++; m=_T("pHYs must appear before IDAT"); }
				phys_seen++;
				break;
			case CHUNK_sPLT:
				if(idat_seen) { e++; m=_T("sPLT must appear before IDAT"); }
				break;
			case CHUNK_dSIG:
				if(prev_chunk!=CHUNK_IHDR && prev_chunk!=CHUNK_dSIG) {
					dsig_pending=1; // The next non-dSIG chunk must be IEND
				}
				if(idat_seen) dsig_nesting_level--;
				else dsig_nesting_level++;
				break;
			case CHUNK_oFFs:
				if(offs_seen) { e++; m=_T("Multiple oFFs chunks not allowed"); }
				else if(idat_seen) { e++; m=_T("oFFs must appear before IDAT"); }
				offs_seen++;
				break;
			case CHUNK_pCAL:
				if(pcal_seen) { e++; m=_T("Multiple pCAL chunks not allowed"); }
				else if(idat_seen) { e++; m=_T("pCAL must appear before IDAT"); }
				pcal_seen++;
				break;
			case CHUNK_sCAL:
				if(scal_seen) { e++; m=_T("Multiple sCAL chunks not allowed"); }
				else if(idat_seen) { e++; m=_T("sCAL must appear before IDAT"); }
				scal_seen++;
				break;
			}
		}

		prev_chunk=t;
		i++;
	}

	if(e==0) {  // if no errors yet, we'll test some more things
		if(m_num_chunks<1) {
			e++; m=_T("No chunks. Not valid.");
		}
		else if(!idat_seen) {
			e++; m=_T("Required IDAT chunk not found");
		}
		else if(m_colortype==3 && !plte_seen) {
			e++; m=_T("Required PLTE chunk not found");
		}
		else if(dsig_nesting_level!=0) {
			e++; m=_T("Mismatched dSIG chunks");
		}
	}

done:
	if(msgmode==0) {
		twpng_MessageBox(globals.hwndMain,m,_T("Validity check"),MB_OK|(e?MB_ICONWARNING:MB_ICONINFORMATION));
		return (e==0);
	}
	//else

	if(e) {
		StringCchPrintf(buf,500,_T("A problem was detected with the current file:\n\n%s\n\n")
			_T("Do you want to save it anyway?"),m);
		if(twpng_MessageBox(globals.hwndMain,buf,_T("Validity check"),MB_OKCANCEL|MB_ICONWARNING)!=IDOK)
		{
			return 0;
		}
	}
	return 1;

}

// returns 1 if saved, else 0
static int SavePngAs(HWND hwnd)
{
	OPENFILENAME ofn;
	TCHAR fn[MAX_PATH];
	BOOL bRet;

	if(!png->check_validity(1)) return 0;

	StringCchCopy(fn,MAX_PATH,png->m_filename);

	ZeroMemory(&ofn,sizeof(OPENFILENAME));
	ofn.lStructSize=sizeof(OPENFILENAME);
	ofn.hwndOwner=hwnd;

	if(png->m_imgtype==IMG_MNG) {
		ofn.lpstrFilter=_T("MNG (*.mng)\0*.mng\0\0");
		ofn.lpstrDefExt=_T("mng");
	}
	else if(png->m_imgtype==IMG_JNG) {
		ofn.lpstrFilter=_T("JNG (*.jng)\0*.jng\0\0");
		ofn.lpstrDefExt=_T("jng");
	}
	else {
		ofn.lpstrFilter=_T("PNG (*.png)\0*.png\0\0");
		ofn.lpstrDefExt=_T("png");
	}

	ofn.nFilterIndex=1;
	ofn.lpstrTitle=_T("Save Image As...");
	ofn.lpstrFile=fn;
	ofn.nMaxFile=MAX_PATH;
	ofn.Flags=OFN_PATHMUSTEXIST|OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR;

	globals.dlgs_open++;
	bRet = GetSaveFileName(&ofn);
	globals.dlgs_open--;
	if(bRet) {
		if(png->write_file(ofn.lpstrFile)) {
			StringCchCopy(png->m_filename,MAX_PATH,ofn.lpstrFile);
			png->m_named=1;
			png->m_dirty=0;
			SetTitle(png);
			return 1;
		}
	}
	return 0;
}

// returns 1 if saved, else 0
static int SavePng(HWND hwnd)
{
	if(png->m_named) {
		if(!png->check_validity(1)) return 0;
		if(png->write_file(png->m_filename)) {
			png->m_dirty=0;
			SetTitle(png);
			return 1;
		}
		return 0;
	}
	else {
		return SavePngAs(hwnd);
	}
}

static void DeleteChunks()          // delete all selected items
{
	int i;
	int numdeleted=0;
	int firstdeleted;

	firstdeleted= png->m_num_chunks;

	for(i=png->m_num_chunks-1;i>=0;i--) {
		if(ListView_GetItemState(globals.hwndMainList,i,LVIS_SELECTED) & LVIS_SELECTED) {
			png->delete_chunk(i);
			numdeleted++;
			firstdeleted=i;
		}
	}

	if(numdeleted>0) {
		png->fill_listbox(globals.hwndMainList);
	}

	if(firstdeleted<0) firstdeleted=0;
	if(firstdeleted>(png->m_num_chunks-1)) firstdeleted=png->m_num_chunks-1;
	twpng_SetLVSelection(globals.hwndMainList,firstdeleted,1);
	png->modified();
}

// Returns 1 if the chunk holds AI-generation metadata (prompts, workflow, EXIF, etc.).
static int twpng_IsAIMetadataChunk(Chunk *c)
{
	if(!c) return 0;
	if(c->m_chunktype_id==CHUNK_eXIf) return 1;
	if(c->m_chunktype_id==CHUNK_tEXt || c->m_chunktype_id==CHUNK_zTXt ||
	   c->m_chunktype_id==CHUNK_iTXt) {
		static const TCHAR *aiKeys[] = {
			_T("parameters"), _T("workflow"), _T("prompt"), _T("Comment"),
			_T("Software"), _T("Source"), _T("Title"), _T("Description"),
			_T("Generation time"), _T("Dream"), _T("sd-metadata"),
			_T("negative_prompt"), NULL };
		struct keyword_info_struct kw;
		if(c->get_keyword_info(&kw)) {
			for(int k=0; aiKeys[k]; k++)
				if(!lstrcmp(kw.keyword, aiKeys[k])) return 1;
		}
	}
	return 0;
}

// Remove all AI-generation metadata chunks (so an image can be shared without leaking
// prompts, seed, workflow, etc.).
static void twpng_StripForeignMetadata();

static void StripAIMetadata()
{
	int i, count=0;
	TCHAR buf[200];

	if(!png && g_foreignFn[0]) { twpng_StripForeignMetadata(); return; }
	if(!png) return;
	for(i=0;i<png->m_num_chunks;i++)
		if(twpng_IsAIMetadataChunk(png->chunk[i])) count++;

	if(count<1) {
		mesg(MSG_I,_T("No AI metadata chunks were found in this file."));
		return;
	}

	StringCchPrintf(buf,200,
		_T("Remove %d AI metadata chunk%s (prompts, settings, workflow, etc.)?\r\n\r\n")
		_T("This cannot be undone."),count,(count==1)?_T(""):_T("s"));
	if(twpng_MessageBox(globals.hwndMain,buf,_T("Strip AI Metadata"),
		MB_OKCANCEL|MB_ICONWARNING)!=IDOK) return;

	for(i=png->m_num_chunks-1;i>=0;i--)
		if(twpng_IsAIMetadataChunk(png->chunk[i]))
			png->delete_chunk(i);

	png->fill_listbox(globals.hwndMainList);
	png->modified();
}

// ---- A1111 "parameters" parsed viewer -----------------------------------------------

static void twpng_SetClipboardText(HWND owner, const TCHAR *text)
{
	if(!text || !OpenClipboard(owner)) return;
	EmptyClipboard();
	SIZE_T n = (SIZE_T)(lstrlen(text)+1)*sizeof(TCHAR);
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
	if(h) {
		void *p = GlobalLock(h);
		if(p) {
			memcpy(p, text, n);
			GlobalUnlock(h);
#ifdef UNICODE
			SetClipboardData(CF_UNICODETEXT, h);
#else
			SetClipboardData(CF_TEXT, h);
#endif
		}
	}
	CloseClipboard();
}

static int twpng_FindLineStart(const TCHAR *s, int from, int limit, const TCHAR *marker)
{
	int mlen = lstrlen(marker);
	for(int i=from; i+mlen<=limit; i++)
		if((i==0 || s[i-1]==_T('\n')) && !_tcsncmp(&s[i], marker, mlen))
			return i;
	return -1;
}

// Allocate a NUL-terminated copy of s[a,b), trimming trailing whitespace/newlines.
static TCHAR *twpng_DupTrim(const TCHAR *s, int a, int b)
{
	while(b>a && (s[b-1]==_T('\n')||s[b-1]==_T('\r')||s[b-1]==_T(' ')||s[b-1]==_T('\t'))) b--;
	int n = (b>a)?(b-a):0;
	TCHAR *out = (TCHAR*)malloc(((size_t)n+1)*sizeof(TCHAR));
	if(!out) return NULL;
	if(n>0) memcpy(out, &s[a], (size_t)n*sizeof(TCHAR));
	out[n]=0;
	return out;
}

// Convert lone \n to \r\n so edit controls / the clipboard show line breaks.
static TCHAR *twpng_LfToCrlf(const TCHAR *s)
{
	if(!s) return NULL;
	int len=lstrlen(s), extra=0, i;
	for(i=0;i<len;i++) if(s[i]==_T('\n') && (i==0 || s[i-1]!=_T('\r'))) extra++;
	TCHAR *out=(TCHAR*)malloc(((size_t)len+extra+1)*sizeof(TCHAR));
	if(!out) return NULL;
	int o=0;
	for(i=0;i<len;i++){
		if(s[i]==_T('\n') && (i==0 || s[i-1]!=_T('\r'))) out[o++]=_T('\r');
		out[o++]=s[i];
	}
	out[o]=0;
	return out;
}

// Reformat "k: v, k: v" settings into one pair per line (respecting quoted values).
static TCHAR *twpng_SettingsToLines(const TCHAR *s)
{
	if(!s) return NULL;
	int len=lstrlen(s), i;
	TCHAR *out=(TCHAR*)malloc(((size_t)len*2+2)*sizeof(TCHAR));
	if(!out) return NULL;
	int o=0, inq=0;
	for(i=0;i<len;i++){
		TCHAR ch=s[i];
		if(ch==_T('"')) inq=!inq;
		if(!inq && ch==_T(',') && s[i+1]==_T(' ')){ out[o++]=_T('\r'); out[o++]=_T('\n'); i++; continue; }
		out[o++]=ch;
	}
	out[o]=0;
	return out;
}

struct aiparams_ctx {
	TCHAR *full;    // full original parameters text (NULL if the file has none)
	TCHAR *pos;     // positive prompt
	TCHAR *neg;     // negative prompt (NULL if none)
	TCHAR *setRaw;  // settings line, original single line (NULL if none)
	TCHAR path[MAX_PATH];  // file the parameters came from
	BOOL isDoc;     // path is the open document, so browsing opens the next file as one
	HBITMAP thumb;  // Explorer thumbnail, arrives later from a background thread
	int thumbW, thumbH;
	BOOL thumbAlpha;
	LONG thumbGen;
};

static void twpng_ParseA1111(const TCHAR *full, struct aiparams_ctx *c)
{
	c->pos=c->neg=c->setRaw=NULL;
	if(!full) return;
	int len=lstrlen(full);
	int setStart = twpng_FindLineStart(full,0,len,_T("Steps:"));
	int region = (setStart>=0)? setStart : len;
	int negPos = twpng_FindLineStart(full,0,region,_T("Negative prompt:"));
	if(negPos>=0){
		c->pos = twpng_DupTrim(full,0,negPos);
		int na = negPos + lstrlen(_T("Negative prompt:"));
		while(na<region && (full[na]==_T(' ')||full[na]==_T('\t'))) na++;
		c->neg = twpng_DupTrim(full,na,region);
	} else {
		c->pos = twpng_DupTrim(full,0,region);
	}
	if(setStart>=0) c->setRaw = twpng_DupTrim(full,setStart,len);
}

static struct aiparams_ctx *g_aiParamsCtx = NULL;
// While set, replacing the document keeps the viewer open (folder browsing updates it).
static BOOL g_keepAIView = FALSE;

static void twpng_FreeAIParamsCtx(struct aiparams_ctx *c)
{
	if(!c) return;
	free(c->full); free(c->pos); free(c->neg); free(c->setRaw);
	if(c->thumb) DeleteObject(c->thumb);
	free(c);
}

static void twpng_ParseAIText(const TCHAR *full, struct aiparams_ctx *c)
{
	if(twpng_LooksLikeComfyJson(full)) twpng_ParseComfyJson(full, c);
	else twpng_ParseA1111(full, c);
}

// ---- Folder browsing -----------------------------------------------------------------

static BOOL twpng_IsBrowsableFile(const TCHAR *name)
{
	static const TCHAR *exts[] = { _T("png"), _T("jpg"), _T("jpeg"), _T("webp"),
		_T("mp4"), _T("mov"), _T("m4v"), NULL };
	const TCHAR *ext = _tcsrchr(name, _T('.'));
	if(!ext) return FALSE;
	for(int i=0; exts[i]; i++) if(!lstrcmpi(ext+1, exts[i])) return TRUE;
	return FALSE;
}

static int twpng_CmpNames(const void *a, const void *b)
{
#ifdef UNICODE
	return StrCmpLogicalW(*(const TCHAR* const*)a, *(const TCHAR* const*)b);
#else
	return lstrcmpi(*(const TCHAR* const*)a, *(const TCHAR* const*)b);
#endif
}

static void twpng_FreeList(TCHAR **list, int n)
{
	for(int i=0; i<n; i++) free(list[i]);
	free(list);
}

// Supported files in `dir` (ending in a backslash), in Explorer order.
static int twpng_ListDir(const TCHAR *dir, TCHAR ***outList)
{
	*outList = NULL;
	TCHAR pat[MAX_PATH];
	StringCchPrintf(pat, MAX_PATH, _T("%s*"), dir);
	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFileEx(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	if(h==INVALID_HANDLE_VALUE) return 0;
	TCHAR **list = NULL;
	int n = 0, cap = 0;
	do {
		if((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !twpng_IsBrowsableFile(fd.cFileName))
			continue;
		if(n==cap) {
			cap = cap ? cap*2 : 256;
			TCHAR **nl = (TCHAR**)realloc(list, (size_t)cap*sizeof(TCHAR*));
			if(!nl) break;
			list = nl;
		}
		TCHAR full[MAX_PATH];
		StringCchPrintf(full, MAX_PATH, _T("%s%s"), dir, fd.cFileName);
		if((list[n] = _tcsdup(full)) != NULL) n++;
	} while(FindNextFile(h, &fd));
	FindClose(h);
	if(n) qsort(list, n, sizeof(TCHAR*), twpng_CmpNames);
	*outList = list;
	return n;
}

// The folder listing is cached and rebuilt on a background thread: a folder can hold tens
// of thousands of files, on a network drive that takes seconds to list.
struct twpng_sibjob { TCHAR dir[MAX_PATH]; TCHAR **list; int n; FILETIME stamp; };
static struct {
	TCHAR dir[MAX_PATH];
	TCHAR **list;
	int n;
	FILETIME stamp;       // the folder's last-write time when it was listed
	ULONGLONG tick;       // when it was listed
	BOOL loading;
} g_sib;

static BOOL twpng_DirStamp(const TCHAR *dir, FILETIME *ft)
{
	WIN32_FILE_ATTRIBUTE_DATA fa;
	if(!GetFileAttributesEx(dir, GetFileExInfoStandard, &fa)) return FALSE;
	*ft = fa.ftLastWriteTime;
	return TRUE;
}

static DWORD WINAPI twpng_SibThread(LPVOID p)
{
	struct twpng_sibjob *j = (struct twpng_sibjob*)p;
	twpng_DirStamp(j->dir, &j->stamp);   // before listing, so a change meanwhile shows up next time
	j->n = twpng_ListDir(j->dir, &j->list);
	if(!PostMessage(globals.hwndMain, WM_APP_SIBLINGS, 0, (LPARAM)j)) {
		twpng_FreeList(j->list, j->n);
		free(j);
	}
	return 0;
}

static void twpng_StartSibListing(const TCHAR *dir)
{
	if(g_sib.loading) return;   // one at a time; the result handler re-checks the viewer
	struct twpng_sibjob *j = (struct twpng_sibjob*)calloc(1, sizeof(struct twpng_sibjob));
	if(!j) return;
	StringCchCopy(j->dir, MAX_PATH, dir);
	HANDLE th = CreateThread(NULL, 0, twpng_SibThread, j, 0, NULL);
	if(th) { CloseHandle(th); g_sib.loading = TRUE; }
	else free(j);
}

// Called on the main thread when a listing is done.
static void twpng_SibListingDone(struct twpng_sibjob *j)
{
	twpng_FreeList(g_sib.list, g_sib.n);
	StringCchCopy(g_sib.dir, MAX_PATH, j->dir);
	g_sib.list = j->list;
	g_sib.n = j->n;
	g_sib.stamp = j->stamp;
	g_sib.tick = GetTickCount64();
	g_sib.loading = FALSE;
	free(j);
}

// Cached list of the supported files next to `fn` (owned by the cache, valid until the
// next listing arrives). Starts a background re-list when the folder is new, changed or
// the list is old; returns 0 while the first listing of a folder is still running.
static int twpng_GetSiblings(const TCHAR *fn, TCHAR ***outList, int *outIdx, BOOL force)
{
	*outList = NULL; *outIdx = -1;
	TCHAR dir[MAX_PATH];
	StringCchCopy(dir, MAX_PATH, fn);
	TCHAR *sl = _tcsrchr(dir, _T('\\'));
	if(!sl) return 0;
	sl[1] = 0;
	BOOL same = g_sib.list && !lstrcmpi(dir, g_sib.dir);
	int idx = -1;
	if(same) for(int i=0; i<g_sib.n; i++) if(!lstrcmpi(g_sib.list[i], fn)) { idx = i; break; }
	ULONGLONG age = GetTickCount64() - g_sib.tick;
	BOOL stale = force || !same || age > 5*60*1000 || (idx<0 && age > 3000);
	if(!stale) {
		FILETIME ft;
		if(twpng_DirStamp(dir, &ft) && CompareFileTime(&ft, &g_sib.stamp)) stale = TRUE;
	}
	if(stale) twpng_StartSibListing(dir);
	if(!same) return 0;
	*outList = g_sib.list;
	*outIdx = idx;
	return g_sib.n;
}

// Title shows the file and its position in the folder; Prev/Next enable at the ends.
static void twpng_UpdateAINav(HWND hwnd, struct aiparams_ctx *c)
{
	TCHAR title[MAX_PATH+80];
	TCHAR **list = NULL;
	int idx = -1, n = 0;
	if(c->path[0]) n = twpng_GetSiblings(c->path, &list, &idx, FALSE);
	const TCHAR *base = _tcsrchr(c->path, _T('\\'));
	base = base ? base+1 : c->path;
	if(base[0] && idx>=0)
		StringCchPrintf(title, MAX_PATH+80, _T("AI Generation Parameters - %s (%d/%d)"), base, idx+1, n);
	else if(base[0] && g_sib.loading)
		StringCchPrintf(title, MAX_PATH+80, _T("AI Generation Parameters - %s (listing folder...)"), base);
	else if(base[0])
		StringCchPrintf(title, MAX_PATH+80, _T("AI Generation Parameters - %s"), base);
	else
		StringCchCopy(title, MAX_PATH+80, _T("AI Generation Parameters"));
	SetWindowText(hwnd, title);
	EnableWindow(GetDlgItem(hwnd,IDC_AIP_PREV), idx>0);
	EnableWindow(GetDlgItem(hwnd,IDC_AIP_NEXT), idx>=0 && idx<n-1);
}

// ---- Thumbnail ------------------------------------------------------------------------

static volatile LONG g_thumbGen = 0;
struct twpng_thumbjob { TCHAR path[MAX_PATH]; LONG gen; HBITMAP bmp; };

static DWORD WINAPI twpng_ThumbThread(LPVOID p)
{
	struct twpng_thumbjob *j = (struct twpng_thumbjob*)p;
	if(j->gen == g_thumbGen) {   // skip it if another file was shown meanwhile
		HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		IShellItemImageFactory *f = NULL;
		if(SUCCEEDED(SHCreateItemFromParsingName(j->path, NULL, IID_PPV_ARGS(&f)))) {
			SIZE sz = { 512, 512 };
			if(FAILED(f->GetImage(sz, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &j->bmp))) j->bmp = NULL;
			f->Release();
		}
		if(SUCCEEDED(hr)) CoUninitialize();
	}
	if(!j->bmp || !PostMessage(globals.hwndMain, WM_APP_THUMB, 0, (LPARAM)j)) {
		if(j->bmp) DeleteObject(j->bmp);
		free(j);
	}
	return 0;
}

// Ask the shell (Explorer's thumbnail cache) for the file's thumbnail in the background.
static void twpng_RequestThumb(struct aiparams_ctx *c)
{
	c->thumbGen = InterlockedIncrement(&g_thumbGen);
	if(!c->path[0]) return;
	struct twpng_thumbjob *j = (struct twpng_thumbjob*)calloc(1, sizeof(struct twpng_thumbjob));
	if(!j) return;
	StringCchCopy(j->path, MAX_PATH, c->path);
	j->gen = c->thumbGen;
	HANDLE th = CreateThread(NULL, 0, twpng_ThumbThread, j, 0, NULL);
	if(th) CloseHandle(th); else free(j);
}

static void twpng_LayoutAIParams(HWND hwnd);

// Called on the main thread when a thumbnail is ready.
static void twpng_ThumbDone(struct twpng_thumbjob *j)
{
	struct aiparams_ctx *c = g_aiParamsCtx;
	if(g_hwndAIParams && c && c->thumbGen==j->gen && !c->thumb) {
		DIBSECTION ds;
		ZeroMemory(&ds, sizeof(ds));
		if(GetObject(j->bmp, sizeof(ds), &ds) >= (int)sizeof(BITMAP) && ds.dsBm.bmWidth>0) {
			c->thumb = j->bmp;
			j->bmp = NULL;
			c->thumbW = ds.dsBm.bmWidth;
			c->thumbH = ds.dsBm.bmHeight<0 ? -ds.dsBm.bmHeight : ds.dsBm.bmHeight;
			// Only blend when the alpha channel is really used; many thumbnails leave it 0.
			if(ds.dsBm.bmBitsPixel==32 && ds.dsBm.bmBits) {
				const BYTE *px = (const BYTE*)ds.dsBm.bmBits;
				size_t cnt = (size_t)c->thumbW * c->thumbH;
				BOOL some = FALSE, partial = FALSE;
				for(size_t i=0; i<cnt; i++) {
					BYTE a = px[i*4+3];
					if(a) some = TRUE;
					if(a!=255) partial = TRUE;
				}
				c->thumbAlpha = some && partial;
			}
			twpng_LayoutAIParams(g_hwndAIParams);
			InvalidateRect(GetDlgItem(g_hwndAIParams, IDC_AIP_THUMB), NULL, TRUE);
		}
	}
	if(j->bmp) DeleteObject(j->bmp);
	free(j);
}

static void twpng_DrawThumb(const DRAWITEMSTRUCT *d, const struct aiparams_ctx *c)
{
	RECT r = d->rcItem;
	FillRect(d->hDC, &r, globals.hUiBgBrush ? globals.hUiBgBrush : GetSysColorBrush(COLOR_BTNFACE));
	if(!c || !c->thumb || c->thumbW<=0 || c->thumbH<=0) return;
	int w = r.right-r.left, h = r.bottom-r.top;
	int dw = w, dh = MulDiv(w, c->thumbH, c->thumbW);
	if(dh > h) { dh = h; dw = MulDiv(h, c->thumbW, c->thumbH); }
	int x = r.right - dw, y = r.top;   // top-right, next to the settings text
	HDC mdc = CreateCompatibleDC(d->hDC);
	HGDIOBJ old = SelectObject(mdc, c->thumb);
	BOOL done = FALSE;
	if(c->thumbAlpha) {
		BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
		done = AlphaBlend(d->hDC, x, y, dw, dh, mdc, 0, 0, c->thumbW, c->thumbH, bf);
	}
	if(!done) {
		SetStretchBltMode(d->hDC, HALFTONE);
		SetBrushOrgEx(d->hDC, 0, 0, NULL);
		StretchBlt(d->hDC, x, y, dw, dh, mdc, 0, 0, c->thumbW, c->thumbH, SRCCOPY);
	}
	SelectObject(mdc, old);
	DeleteDC(mdc);
}

static void twpng_FillAIParams(HWND hwnd, struct aiparams_ctx *c)
{
	TCHAR *t;
	SetDlgItemText(hwnd,IDC_AIP_PROMPT,_T(""));
	SetDlgItemText(hwnd,IDC_AIP_NEG,_T(""));
	SetDlgItemText(hwnd,IDC_AIP_SETTINGS,_T(""));
	if(!c->full) SetDlgItemText(hwnd,IDC_AIP_PROMPT,_T("(No AI parameters found in this file.)"));
	t=twpng_LfToCrlf(c->pos);            if(t){ SetDlgItemText(hwnd,IDC_AIP_PROMPT,t);   free(t); }
	t=twpng_LfToCrlf(c->neg);            if(t){ SetDlgItemText(hwnd,IDC_AIP_NEG,t);      free(t); }
	t=twpng_SettingsToLines(c->setRaw);  if(t){ SetDlgItemText(hwnd,IDC_AIP_SETTINGS,t); free(t); }
	EnableWindow(GetDlgItem(hwnd,IDC_AIP_COPYPROMPT), c->pos!=NULL);
	EnableWindow(GetDlgItem(hwnd,IDC_AIP_COPYNEG), c->neg!=NULL);
	EnableWindow(GetDlgItem(hwnd,IDC_AIP_COPYSET), c->setRaw!=NULL);
	EnableWindow(GetDlgItem(hwnd,IDC_AIP_COPYALL), c->full!=NULL);
	BOOL json = FALSE;
	if(c->full) {
		int len = lstrlen(c->full), i = cf_skip_ws(c->full, 0, len);
		json = (i<len && c->full[i]==_T('{'));
	}
	EnableWindow(GetDlgItem(hwnd,IDC_AIP_SAVEJSON), json);
	twpng_UpdateAINav(hwnd, c);
}

// Lay out the AI params controls to fill the (resizable) client area.
static void twpng_LayoutAIParams(HWND hwnd)
{
	RECT rc;
	GetClientRect(hwnd,&rc);
	int W=rc.right, H=rc.bottom;
	int m=10, gap=8, labelH=16, copyW=52, copyH=24, btnH=24;
	int editX=m, editW=W-2*m-copyW-gap;
	if(editW<80) editW=80;
	int copyX=editX+editW+gap;

	int bottomY=H-m-btnH;
	int top=m;
	int avail=bottomY-gap-top-3*labelH-2*gap;   // vertical space shared by 3 edits
	if(avail<90) avail=90;
	int promptH=(int)(avail*0.50);              // prompt is the largest box
	int negH=(int)(avail*0.22);
	int setH=avail-promptH-negH;

	int y=top;
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_LBLPROMPT),NULL,editX,y,editW,labelH,SWP_NOZORDER);
	y+=labelH;
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_PROMPT),NULL,editX,y,editW,promptH,SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_COPYPROMPT),NULL,copyX,y,copyW,copyH,SWP_NOZORDER);
	y+=promptH+gap;
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_LBLNEG),NULL,editX,y,editW,labelH,SWP_NOZORDER);
	y+=labelH;
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_NEG),NULL,editX,y,editW,negH,SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_COPYNEG),NULL,copyX,y,copyW,copyH,SWP_NOZORDER);
	y+=negH+gap;
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_LBLSET),NULL,editX,y,editW,labelH,SWP_NOZORDER);
	y+=labelH;
	// The thumbnail sits right of the settings, as tall as the box and at most half as wide.
	struct aiparams_ctx *c = (struct aiparams_ctx*)GetWindowLongPtr(hwnd,DWLP_USER);
	int thumbW = 0;
	if(c && c->thumb && c->thumbH>0) {
		thumbW = MulDiv(setH, c->thumbW, c->thumbH);
		if(thumbW > editW/2) thumbW = editW/2;
	}
	int setW = thumbW ? editW-thumbW-gap : editW;
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_SETTINGS),NULL,editX,y,setW,setH,SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_THUMB),NULL,editX+editW-thumbW,y,thumbW?thumbW:1,setH,SWP_NOZORDER);
	ShowWindow(GetDlgItem(hwnd,IDC_AIP_THUMB), thumbW ? SW_SHOW : SW_HIDE);
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_COPYSET),NULL,copyX,y,copyW,copyH,SWP_NOZORDER);

	int closeX=W-m-copyW, copyAllX=closeX-gap-58, saveX=copyAllX-gap-84, navW=64;
	SetWindowPos(GetDlgItem(hwnd,IDOK),NULL,closeX,bottomY,copyW,btnH,SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_COPYALL),NULL,copyAllX,bottomY,58,btnH,SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_SAVEJSON),NULL,saveX,bottomY,84,btnH,SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_PREV),NULL,m,bottomY,navW,btnH,SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AIP_NEXT),NULL,m+navW+gap,bottomY,navW,btnH,SWP_NOZORDER);
}

static void twpng_SaveAIJson(HWND hwnd, struct aiparams_ctx *c);
static void twpng_AIParamsNavigate(int dir);

static INT_PTR CALLBACK DlgProcAIParams(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	WORD id = LOWORD(wParam);
	struct aiparams_ctx *c;

	if(msg==WM_INITDIALOG){
		c = (struct aiparams_ctx*)lParam;
		SetWindowLongPtr(hwnd,DWLP_USER,lParam);
		SendMessage(hwnd,WM_SETICON,ICON_BIG,(LPARAM)LoadIcon(globals.hInst,_T("ICONMAIN")));
		if(c) twpng_FillAIParams(hwnd,c);
		twpng_LayoutAIParams(hwnd);
		twpng_InitDarkDialog(hwnd);
		return 1;
	}
	c = (struct aiparams_ctx*)GetWindowLongPtr(hwnd,DWLP_USER);

	switch(msg){
	case WM_SIZE:
		twpng_LayoutAIParams(hwnd);
		return 0;

	case WM_GETMINMAXINFO:
		{
			LPMINMAXINFO mm=(LPMINMAXINFO)lParam;
			mm->ptMinTrackSize.x=360;
			mm->ptMinTrackSize.y=300;
			return 0;
		}

	case WM_DRAWITEM:
		if(wParam==IDC_AIP_THUMB) {
			twpng_DrawThumb((const DRAWITEMSTRUCT*)lParam, c);
			SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
			return TRUE;
		}
		break;

	case WM_EXITSIZEMOVE:
	case WM_DESTROY:
		// Remember the window position/size (kept current so it survives app exit too).
		twpng_StoreWindowPos(hwnd,&globals.window_prefs.aiparams);
		break;

	case WM_COMMAND:
		switch(id){
		case IDC_AIP_COPYPROMPT: if(c){TCHAR*t=twpng_LfToCrlf(c->pos);twpng_SetClipboardText(hwnd,t);free(t);} return 1;
		case IDC_AIP_COPYNEG:    if(c){TCHAR*t=twpng_LfToCrlf(c->neg);twpng_SetClipboardText(hwnd,t);free(t);} return 1;
		case IDC_AIP_COPYSET:    if(c) twpng_SetClipboardText(hwnd,c->setRaw); return 1;
		case IDC_AIP_COPYALL:    if(c){TCHAR*t=twpng_LfToCrlf(c->full);twpng_SetClipboardText(hwnd,t);free(t);} return 1;
		case IDC_AIP_SAVEJSON:   twpng_SaveAIJson(hwnd,c); return 1;
		case IDC_AIP_PREV:       twpng_AIParamsNavigate(-1); return 1;
		case IDC_AIP_NEXT:       twpng_AIParamsNavigate(1); return 1;
		case IDOK: case IDCANCEL: DestroyWindow(hwnd); return 1;
		}
		break;

	case WM_NCDESTROY:
		if(g_hwndAIParams==hwnd){
			g_hwndAIParams=NULL;
			twpng_FreeAIParamsCtx(g_aiParamsCtx);
			g_aiParamsCtx=NULL;
			// Started from the Explorer menu: the viewer is the whole app.
			if(globals.aiparams_only && globals.hwndMain && !IsWindowVisible(globals.hwndMain))
				PostMessage(globals.hwndMain,WM_CLOSE,0,0);
		}
		SetWindowLongPtr(hwnd,DWLP_USER,0);
		return 0;
	}
	{
		INT_PTR dr=0;
		if(twpng_HandleDlgDarkMsg(msg,wParam,lParam,&dr)) return dr;
	}
	return 0;
}

// Force-close the parameters viewer (e.g. when the document changes).
static void twpng_CloseAIParamsView()
{
	if(g_hwndAIParams && !g_keepAIView) DestroyWindow(g_hwndAIParams);
}

// Parsed parameters for `text` (may be NULL), with `extra` appended to the settings.
static struct aiparams_ctx *twpng_BuildAICtx(const TCHAR *text, const TCHAR *extra)
{
	struct aiparams_ctx *c = (struct aiparams_ctx*)calloc(1,sizeof(struct aiparams_ctx));
	if(!c) return NULL;
	if(text && text[0]) {
		c->full = _tcsdup(text);
		if(!c->full) { free(c); return NULL; }
		twpng_ParseAIText(c->full, c);
	}
	if(extra && extra[0]) {
		int n = (c->setRaw ? lstrlen(c->setRaw) : 0) + lstrlen(extra) + 3;
		TCHAR *ns = (TCHAR*)malloc((size_t)n*sizeof(TCHAR));
		if(ns) {
			ns[0] = 0;
			if(c->setRaw) { StringCchCopy(ns, n, c->setRaw); StringCchCat(ns, n, _T(", ")); }
			StringCchCat(ns, n, extra);
			free(c->setRaw);
			c->setRaw = ns;
		}
	}
	return c;
}

// Show the parsed viewer, or update it in place if it is already open. `text` may be NULL
// for a file without AI parameters; `extra` is appended to the settings (container info
// such as the video size). Returns 1 on success.
static int twpng_ShowAIParams(const TCHAR *text, const TCHAR *extra, const TCHAR *path, BOOL isDoc)
{
	struct aiparams_ctx *c = twpng_BuildAICtx(text, extra);
	if(!c) return 0;
	if(path) StringCchCopy(c->path, MAX_PATH, path);
	c->isDoc = isDoc;
	twpng_RequestThumb(c);

	if(g_hwndAIParams) {
		struct aiparams_ctx *old = g_aiParamsCtx;
		g_aiParamsCtx = c;
		SetWindowLongPtr(g_hwndAIParams, DWLP_USER, (LONG_PTR)c);
		twpng_FillAIParams(g_hwndAIParams, c);
		twpng_FreeAIParamsCtx(old);
		if(IsIconic(g_hwndAIParams)) ShowWindow(g_hwndAIParams, SW_RESTORE);
		SetForegroundWindow(g_hwndAIParams);
		return 1;
	}

	g_aiParamsCtx = c;
	// No owner window, so it minimizes independently and gets its own taskbar button.
	g_hwndAIParams = CreateDialogParam(globals.hInst,_T("DLG_AIPARAMS"),
		NULL, DlgProcAIParams, (LPARAM)c);
	if(!g_hwndAIParams) {
		twpng_FreeAIParamsCtx(c);
		g_aiParamsCtx = NULL;
		return 0;
	}
	if(globals.window_prefs.aiparams.w>0 && globals.window_prefs.aiparams.h>0)
		twpng_SetWindowPos(g_hwndAIParams,&globals.window_prefs.aiparams);
	ShowWindow(g_hwndAIParams, SW_SHOW);
	SetForegroundWindow(g_hwndAIParams);
	return 1;
}

// Copy of the "parameters" (A1111) or "prompt" (ComfyUI) text chunk, or NULL.
static TCHAR *twpng_PngAIText(Png *p)
{
	static const TCHAR *keys[] = { _T("parameters"), _T("prompt"), NULL };
	for(int ki=0; keys[ki]; ki++) {
		for(int i=0; i<p->m_num_chunks; i++) {
			Chunk *c = p->chunk[i];
			if(!c || (c->m_chunktype_id!=CHUNK_tEXt && c->m_chunktype_id!=CHUNK_zTXt &&
			          c->m_chunktype_id!=CHUNK_iTXt)) continue;
			struct keyword_info_struct kw;
			if(!c->get_keyword_info(&kw) || lstrcmp(kw.keyword,keys[ki])) continue;
			if(!c->get_text_info() || !c->m_text_info.text) return NULL;
			int n = c->m_text_info.text_size_in_tchars;
			if(n<0) n = 0;
			TCHAR *out = (TCHAR*)malloc(((size_t)n+1)*sizeof(TCHAR));
			if(!out) return NULL;
			memcpy(out, c->m_text_info.text, (size_t)n*sizeof(TCHAR));
			out[n] = 0;
			return out;
		}
	}
	return NULL;
}

static const TCHAR *twpng_DocPath()
{
	if(png) return png->m_named ? png->m_filename : _T("");
	return g_foreignFn;
}

// AI text of the open document. Caller frees *outText; `extra` gets container info.
static int twpng_GetDocAIText(TCHAR **outText, TCHAR *extra, int extraCap)
{
	*outText = NULL;
	extra[0] = 0;
	if(png) {
		*outText = twpng_PngAIText(png);
	}
	else if(g_foreignFn[0]) {
		twpng_ReadForeignAIParams(g_foreignKind, g_foreignFn, outText);
		if(g_foreignKind==3 || g_foreignKind==4) twpng_ReadMp4VideoInfo(g_foreignFn, extra, extraCap);
	}
	return *outText!=NULL;
}

// AI text of any supported file on disk, without opening it as the document.
static int twpng_ReadAnyAIParams(const TCHAR *fn, TCHAR **outText, TCHAR *extra, int extraCap)
{
	*outText = NULL;
	extra[0] = 0;
	HANDLE fh = CreateFile(fn, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if(fh==INVALID_HANDLE_VALUE) return 0;
	unsigned char hdr[16]; DWORD got = 0;
	ReadFile(fh, hdr, sizeof(hdr), &got, NULL);
	CloseHandle(fh);

	int kind = twpng_SniffForeignKind(hdr, got);
	if(kind) {
		twpng_ReadForeignAIParams(kind, fn, outText);
		if(kind==3 || kind==4) twpng_ReadMp4VideoInfo(fn, extra, extraCap);
	}
	else if(got>=8 && !memcmp(hdr,"\x89PNG\r\n\x1a\n",8)) {
		Png *tmp = new Png(fn, fn);
		tmp->m_scratch = 1;
		if(tmp->m_valid) *outText = twpng_PngAIText(tmp);
		delete tmp;
	}
	return *outText!=NULL;
}

// Show the parsed viewer for the current document.
static void twpng_ViewAIParams(HWND owner)
{
	TCHAR *text = NULL, extra[200];
	(void)owner;
	if(!twpng_GetDocAIText(&text, extra, 200)) {
		if(png || g_foreignFn[0]) mesg(MSG_I, _T("No AI parameters were found in this file."));
		return;
	}
	twpng_ShowAIParams(text, extra, twpng_DocPath(), TRUE);
	free(text);
}

static void twpng_OpenInViewer(const TCHAR *path);

// Prev/Next in the viewer: step through the supported files in the same folder.
static void twpng_AIParamsNavigate(int dir)
{
	struct aiparams_ctx *c = g_aiParamsCtx;
	if(!c || !c->path[0]) return;
	TCHAR **list;
	int idx, n = twpng_GetSiblings(c->path, &list, &idx, FALSE);
	if(idx<0 || idx+dir<0 || idx+dir>=n) return;
	TCHAR next[MAX_PATH];
	StringCchCopy(next, MAX_PATH, list[idx+dir]);

	if(c->isDoc) {
		twpng_OpenInViewer(next);
		return;
	}
	TCHAR *text = NULL, extra[200] = _T("");
	twpng_ReadAnyAIParams(next, &text, extra, 200);
	twpng_ShowAIParams(text, extra, next, FALSE);
	free(text);
}

// Open `path` as the document and show it in the viewer, keeping the viewer window.
static void twpng_OpenInViewer(const TCHAR *path)
{
	TCHAR p[MAX_PATH];
	StringCchCopy(p, MAX_PATH, path);
	if(!OkToClosePNG()) return;
	g_keepAIView = TRUE;
	OpenPngByName(p);
	g_keepAIView = FALSE;
	TCHAR *text = NULL, extra[200] = _T("");
	twpng_GetDocAIText(&text, extra, 200);
	twpng_ShowAIParams(text, extra, p, TRUE);
	free(text);
}

// Viewer shortcuts: Alt+Left/Right and Ctrl+PageUp/PageDown browse, F5 re-reads the folder,
// Ctrl+F opens the folder overview with its search box.
static BOOL twpng_AIParamsKey(const MSG *m)
{
	if(m->message!=WM_KEYDOWN && m->message!=WM_SYSKEYDOWN) return FALSE;
	if(m->hwnd!=g_hwndAIParams && !IsChild(g_hwndAIParams, m->hwnd)) return FALSE;
	BOOL alt = (GetKeyState(VK_MENU) & 0x8000)!=0, ctrl = (GetKeyState(VK_CONTROL) & 0x8000)!=0;
	int dir = 0;
	if(alt && !ctrl && m->wParam==VK_LEFT) dir = -1;
	else if(alt && !ctrl && m->wParam==VK_RIGHT) dir = 1;
	else if(ctrl && !alt && m->wParam==VK_PRIOR) dir = -1;
	else if(ctrl && !alt && m->wParam==VK_NEXT) dir = 1;
	if(dir) { twpng_AIParamsNavigate(dir); return TRUE; }
	if(ctrl && !alt && m->wParam=='F' && g_aiParamsCtx && g_aiParamsCtx->path[0]) {
		TCHAR d[MAX_PATH];
		StringCchCopy(d, MAX_PATH, g_aiParamsCtx->path);
		TCHAR *sl = _tcsrchr(d, _T('\\'));
		if(sl) { sl[1] = 0; twpng_ShowFolderOverview(d, TRUE); }
		return TRUE;
	}
	if(m->wParam==VK_F5 && !alt && !ctrl && g_aiParamsCtx && g_aiParamsCtx->path[0]) {
		TCHAR **list; int idx;
		twpng_GetSiblings(g_aiParamsCtx->path, &list, &idx, TRUE);
		twpng_UpdateAINav(g_hwndAIParams, g_aiParamsCtx);
		return TRUE;
	}
	return FALSE;
}

// Save the ComfyUI graph JSON (UTF-8) so it can be loaded back into ComfyUI.
static void twpng_SaveAIJson(HWND hwnd, struct aiparams_ctx *c)
{
	if(!c || !c->full) return;
	TCHAR fn[MAX_PATH], dir[MAX_PATH] = _T("");
	const TCHAR *base = _tcsrchr(c->path, _T('\\'));
	// Full path as the default, so the dialog opens next to the source file.
	StringCchCopy(fn, MAX_PATH, c->path[0] ? c->path : _T("workflow"));
	TCHAR *dot = _tcsrchr(fn, _T('.'));
	if(dot && (!base || dot > fn + (base - c->path))) *dot = 0;
	StringCchCat(fn, MAX_PATH, _T(".json"));
	if(base) {
		StringCchCopy(dir, MAX_PATH, c->path);
		dir[base - c->path] = 0;
	}

	OPENFILENAME ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hwnd;
	ofn.lpstrFilter = _T("JSON files (*.json)\0*.json\0All files (*.*)\0*.*\0\0");
	ofn.lpstrFile = fn;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrInitialDir = dir[0] ? dir : NULL;
	ofn.lpstrDefExt = _T("json");
	ofn.lpstrTitle = _T("Save ComfyUI JSON");
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
	if(!GetSaveFileName(&ofn)) return;

#ifdef UNICODE
	int n = WideCharToMultiByte(CP_UTF8, 0, c->full, -1, NULL, 0, NULL, NULL);
	char *data = (n>0) ? (char*)malloc(n) : NULL;
	if(!data) return;
	WideCharToMultiByte(CP_UTF8, 0, c->full, -1, data, n, NULL, NULL);
	DWORD size = (DWORD)n-1;
#else
	const char *data = c->full;
	DWORD size = (DWORD)lstrlen(c->full);
#endif
	HANDLE fh = CreateFile(fn, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD written = 0;
	BOOL ok = fh!=INVALID_HANDLE_VALUE && WriteFile(fh, data, size, &written, NULL) && written==size;
	if(fh!=INVALID_HANDLE_VALUE) CloseHandle(fh);
#ifdef UNICODE
	free(data);
#endif
	if(!ok) mesg(MSG_E, _T("Could not write %s"), fn);
}

// ---- Minimal ComfyUI prompt-JSON extractor -------------------------------------------
// ComfyUI stores its graph as { "<nodeId>": { "inputs": {...}, "class_type": "..." }, ... }.
// We don't write a full JSON parser; we do brace-aware substring scans for the few keys
// we care about and synthesize an A1111-style "Prompt\nNegative prompt: ...\nSteps: ..."
// string that the existing twpng_ParseA1111 / DLG_AIPARAMS viewer can consume.

// Skip ASCII whitespace.
static int cf_skip_ws(const TCHAR *s, int i, int len) {
	while(i<len && (s[i]==_T(' ')||s[i]==_T('\t')||s[i]==_T('\n')||s[i]==_T('\r'))) i++;
	return i;
}

// Given the index of a '{' (or '[') in s, return the index just past its matching close.
// Returns -1 on unbalanced input.
static int cf_match_brace(const TCHAR *s, int start, int len) {
	if(start>=len) return -1;
	TCHAR open = s[start];
	TCHAR close = (open==_T('{')) ? _T('}') : (open==_T('[')) ? _T(']') : 0;
	if(!close) return -1;
	int depth = 0; BOOL inStr=FALSE, esc=FALSE;
	for(int i=start;i<len;i++){
		TCHAR c = s[i];
		if(inStr){
			if(esc) esc=FALSE;
			else if(c==_T('\\')) esc=TRUE;
			else if(c==_T('"')) inStr=FALSE;
			continue;
		}
		if(c==_T('"')) { inStr=TRUE; continue; }
		if(c==open) depth++;
		else if(c==close) { depth--; if(depth==0) return i+1; }
	}
	return -1;
}

// Parse a JSON string starting at s[i]=='"'. Returns allocated TCHAR (caller frees) and
// sets *outAfter to the index just past the closing quote. NULL on failure.
static TCHAR *cf_parse_string(const TCHAR *s, int i, int len, int *outAfter) {
	if(i>=len || s[i]!=_T('"')) return NULL;
	i++;
	int j=i, n=0;
	while(j<len) {
		TCHAR c = s[j];
		if(c==_T('\\')) { if(j+1>=len) return NULL; j+=2; n++; }
		else if(c==_T('"')) break;
		else { j++; n++; }
	}
	if(j>=len) return NULL;
	TCHAR *out = (TCHAR*)malloc(((size_t)n+1)*sizeof(TCHAR));
	if(!out) return NULL;
	int k=0, p=i;
	while(p<j) {
		TCHAR c = s[p];
		if(c==_T('\\') && p+1<j) {
			TCHAR e = s[p+1];
			switch(e){
			case _T('n'): out[k++]=_T('\n'); p+=2; break;
			case _T('r'): out[k++]=_T('\r'); p+=2; break;
			case _T('t'): out[k++]=_T('\t'); p+=2; break;
			case _T('"'): out[k++]=_T('"'); p+=2; break;
			case _T('\\'): out[k++]=_T('\\'); p+=2; break;
			case _T('/'): out[k++]=_T('/'); p+=2; break;
			case _T('u'): {
				if(p+5>=j){ p+=2; break; }
				unsigned u=0; BOOL ok=TRUE;
				for(int h=0; h<4; h++){
					TCHAR d = s[p+2+h]; u <<= 4;
					if(d>=_T('0')&&d<=_T('9')) u |= d-_T('0');
					else if(d>=_T('a')&&d<=_T('f')) u |= d-_T('a')+10;
					else if(d>=_T('A')&&d<=_T('F')) u |= d-_T('A')+10;
					else { ok=FALSE; break; }
				}
				if(ok) out[k++] = (TCHAR)u;
				p += 6;
				break;
			}
			default: out[k++]=e; p+=2; break;
			}
		}
		else { out[k++]=c; p++; }
	}
	out[k]=0;
	*outAfter = j+1;
	return out;
}

// Within object body [start, end), find "key" and return TRUE with *valueStart pointing
// to the first non-whitespace character of the value (just past the colon).
static BOOL cf_find_key(const TCHAR *s, int start, int end, const TCHAR *key, int *valueStart) {
	int klen = lstrlen(key);
	int depth = 0; BOOL inStr=FALSE, esc=FALSE;
	for(int i=start; i<end; i++) {
		TCHAR c = s[i];
		if(inStr) {
			if(esc) esc=FALSE;
			else if(c==_T('\\')) esc=TRUE;
			else if(c==_T('"')) {
				inStr=FALSE;
				// Only check at top level of this object: depth==0.
				if(depth==0 && i-start >= klen) {
					int kstart = i-klen;
					if(kstart>0 && s[kstart-1]==_T('"') && !_tcsncmp(&s[kstart],key,klen)) {
						// after the closing quote, expect optional ws then ':'
						int p = cf_skip_ws(s, i+1, end);
						if(p<end && s[p]==_T(':')) {
							p = cf_skip_ws(s, p+1, end);
							*valueStart = p;
							return TRUE;
						}
					}
				}
			}
			continue;
		}
		if(c==_T('"')) inStr=TRUE;
		else if(c==_T('{') || c==_T('[')) depth++;
		else if(c==_T('}') || c==_T(']')) depth--;
	}
	return FALSE;
}

static TCHAR *cf_get_string(const TCHAR *s, int start, int end, const TCHAR *key) {
	int v;
	if(!cf_find_key(s,start,end,key,&v)) return NULL;
	int after;
	return cf_parse_string(s, v, end, &after);
}

// "key": ["nodeId", n] → returns "nodeId".
static TCHAR *cf_get_ref(const TCHAR *s, int start, int end, const TCHAR *key) {
	int v;
	if(!cf_find_key(s,start,end,key,&v)) return NULL;
	if(v>=end || s[v]!=_T('[')) return NULL;
	int p = cf_skip_ws(s, v+1, end);
	int after;
	return cf_parse_string(s, p, end, &after);
}

// "key": <number>. Reads the raw digits/'.'/'-'/etc into out (NUL-term, max outCap).
// Returns TRUE if found.
static BOOL cf_get_number_str(const TCHAR *s, int start, int end, const TCHAR *key, TCHAR *out, int outCap) {
	int v;
	if(!cf_find_key(s,start,end,key,&v)) return FALSE;
	int p=v, q=v;
	while(q<end) {
		TCHAR c = s[q];
		if((c>=_T('0')&&c<=_T('9'))||c==_T('.')||c==_T('-')||c==_T('+')||c==_T('e')||c==_T('E')) q++;
		else break;
	}
	int n = q-p;
	if(n<=0 || n>=outCap) return FALSE;
	memcpy(out, &s[p], (size_t)n*sizeof(TCHAR));
	out[n]=0;
	return TRUE;
}

// Find the value range (start..end) of a top-level node by id, i.e. "<id>": { ... }.
// Returns TRUE; on success sets *nodeStart/*nodeEnd to the inside of the braces.
static BOOL cf_find_node_by_id(const TCHAR *s, int len, const TCHAR *id, int *nodeStart, int *nodeEnd) {
	int idlen = lstrlen(id);
	int depth = 0; BOOL inStr=FALSE, esc=FALSE;
	for(int i=0; i<len; i++) {
		TCHAR c = s[i];
		if(inStr) {
			if(esc) esc=FALSE;
			else if(c==_T('\\')) esc=TRUE;
			else if(c==_T('"')) {
				inStr=FALSE;
				if(depth==1 && i-idlen-1 >= 0) {
					int kstart = i-idlen;
					if(s[kstart-1]==_T('"') && !_tcsncmp(&s[kstart],id,idlen) && (i-kstart)==idlen) {
						int p = cf_skip_ws(s,i+1,len);
						if(p<len && s[p]==_T(':')) {
							p = cf_skip_ws(s,p+1,len);
							if(p<len && s[p]==_T('{')) {
								int after = cf_match_brace(s, p, len);
								if(after>0) {
									*nodeStart = p+1;
									*nodeEnd = after-1;
									return TRUE;
								}
							}
						}
					}
				}
			}
			continue;
		}
		if(c==_T('"')) inStr=TRUE;
		else if(c==_T('{') || c==_T('[')) depth++;
		else if(c==_T('}') || c==_T(']')) depth--;
	}
	return FALSE;
}

// True if a node's class_type matches `wanted`.
static BOOL cf_class_is(const TCHAR *s, int nodeStart, int nodeEnd, const TCHAR *wanted) {
	TCHAR *ct = cf_get_string(s, nodeStart, nodeEnd, _T("class_type"));
	if(!ct) return FALSE;
	BOOL eq = (!lstrcmp(ct, wanted));
	free(ct);
	return eq;
}

// Detect ComfyUI format by content (first non-ws char is '{' and the text contains
// "class_type"). Returns TRUE if it looks like a ComfyUI prompt graph.
static BOOL twpng_LooksLikeComfyJson(const TCHAR *s) {
	if(!s) return FALSE;
	int len = lstrlen(s);
	int i = cf_skip_ws(s, 0, len);
	if(i>=len || s[i]!=_T('{')) return FALSE;
	return _tcsstr(s, _T("\"class_type\"")) != NULL;
}

// Inside of a node body [ns, ne) to inside of its "inputs" object.
static BOOL cf_inputs_range(const TCHAR *s, int len, int ns, int ne, int *is, int *ie) {
	int v;
	if(!cf_find_key(s,ns,ne,_T("inputs"),&v) || v>=ne || s[v]!=_T('{')) return FALSE;
	int after = cf_match_brace(s, v, len);
	if(after<0) return FALSE;
	*is = v+1; *ie = after-1;
	return TRUE;
}

static BOOL cf_node_inputs_by_id(const TCHAR *s, int len, const TCHAR *id, int *is, int *ie) {
	int ns, ne;
	return id && cf_find_node_by_id(s,len,id,&ns,&ne) && cf_inputs_range(s,len,ns,ne,is,ie);
}

// String input `key`, or the string behind it when the input is wired to a primitive or
// string node (PrimitiveStringMultiline, String Literal...).
static TCHAR *cf_get_text(const TCHAR *s, int len, int is, int ie, const TCHAR *key) {
	TCHAR *t = cf_get_string(s, is, ie, key);
	if(t) return t;
	static const TCHAR *valueKeys[] = { _T("value"), _T("text"), _T("string"), _T("prompt"), NULL };
	TCHAR *ref = cf_get_ref(s, is, ie, key);
	for(int hop=0; ref && hop<4 && !t; hop++) {
		int ns, ne;
		if(!cf_node_inputs_by_id(s, len, ref, &ns, &ne)) break;
		for(int k=0; valueKeys[k] && !t; k++) t = cf_get_string(s, ns, ne, valueKeys[k]);
		TCHAR *next = NULL;
		for(int k=0; valueKeys[k] && !t && !next; k++) next = cf_get_ref(s, ns, ne, valueKeys[k]);
		free(ref);
		ref = next;
	}
	free(ref);
	return t;
}

// Inputs of the first node whose class_type is `cls`.
static BOOL cf_find_class_inputs(const TCHAR *s, int len, const TCHAR *cls, int *is, int *ie) {
	TCHAR needle[96];
	StringCchPrintf(needle,96,_T("\"class_type\": \"%s\""),cls);
	const TCHAR *hit = _tcsstr(s, needle);
	if(!hit) { StringCchPrintf(needle,96,_T("\"class_type\":\"%s\""),cls); hit=_tcsstr(s,needle); }
	if(!hit) return FALSE;
	// Walk back to the node's '{'. Strings aren't tracked here, but ComfyUI node bodies
	// don't have unbalanced braces inside strings ahead of class_type in practice.
	int back = (int)(hit-s), depth = 0, ns = -1;
	while(back>0) {
		back--;
		TCHAR c = s[back];
		if(c==_T('}')||c==_T(']')) depth++;
		else if(c==_T('{')||c==_T('[')) { if(depth==0) { ns=back+1; break; } depth--; }
	}
	if(ns<0) return FALSE;
	int after = cf_match_brace(s, ns-1, len);
	if(after<0) return FALSE;
	return cf_inputs_range(s, len, ns, after-1, is, ie);
}

struct cf_settings {
	TCHAR seed[64], steps[32], cfg[32], denoise[32];
	TCHAR *sampler, *scheduler;
};

// Fill whichever settings are still missing from one node's inputs.
static void cf_read_settings(const TCHAR *s, int is, int ie, struct cf_settings *st) {
	if(!st->seed[0])    cf_get_number_str(s,is,ie,_T("seed"),st->seed,64);
	if(!st->seed[0])    cf_get_number_str(s,is,ie,_T("noise_seed"),st->seed,64);
	if(!st->steps[0])   cf_get_number_str(s,is,ie,_T("steps"),st->steps,32);
	if(!st->cfg[0])     cf_get_number_str(s,is,ie,_T("cfg"),st->cfg,32);
	if(!st->denoise[0]) cf_get_number_str(s,is,ie,_T("denoise"),st->denoise,32);
	if(!st->sampler)    st->sampler   = cf_get_string(s,is,ie,_T("sampler_name"));
	if(!st->scheduler)  st->scheduler = cf_get_string(s,is,ie,_T("scheduler"));
}

// Next member of an object body [*pos, ie). Returns the allocated key and sets the value
// range [*vs, *ve), or NULL when there are no more members.
static TCHAR *cf_next_member(const TCHAR *s, int *pos, int ie, int *vs, int *ve) {
	int p = cf_skip_ws(s, *pos, ie);
	if(p<ie && s[p]==_T(',')) p = cf_skip_ws(s, p+1, ie);
	if(p>=ie || s[p]!=_T('"')) return NULL;
	int after;
	TCHAR *key = cf_parse_string(s, p, ie, &after);
	if(!key) return NULL;
	p = cf_skip_ws(s, after, ie);
	if(p>=ie || s[p]!=_T(':')) { free(key); return NULL; }
	p = cf_skip_ws(s, p+1, ie);
	int e = -1;
	if(p<ie && (s[p]==_T('{') || s[p]==_T('['))) e = cf_match_brace(s, p, ie);
	else if(p<ie && s[p]==_T('"')) { TCHAR *t = cf_parse_string(s, p, ie, &e); if(t) free(t); else e = -1; }
	else { e = p; while(e<ie && s[e]!=_T(',')) e++; }
	if(e<0) { free(key); return NULL; }
	*vs = p; *ve = e; *pos = e;
	return key;
}

// Generation mode from a *ToVideo node's class_type, or NULL if it isn't one.
static const TCHAR *cf_video_mode(const TCHAR *s, int len, const TCHAR *id, int is, int ie) {
	int ns, ne, v;
	if(!cf_find_node_by_id(s, len, id, &ns, &ne)) return NULL;
	TCHAR *ct = cf_get_string(s, ns, ne, _T("class_type"));
	if(!ct) return NULL;
	const TCHAR *mode = NULL;
	if(_tcsstr(ct,_T("ReferenceToVideo")) || _tcsstr(ct,_T("Ref2V")))
		mode = _T("Reference to video");
	else if(_tcsstr(ct,_T("FirstLastFrame")))
		mode = _T("First-last frame to video");
	else if(_tcsstr(ct,_T("ImageToVideo")) || _tcsstr(ct,_T("I2V")))
		mode = (cf_find_key(s,is,ie,_T("last_frame"),&v) || cf_find_key(s,is,ie,_T("end_image"),&v))
			? _T("First-last frame to video") : _T("Image to video");
	else if(_tcsstr(ct,_T("TextToVideo")) || _tcsstr(ct,_T("T2V")))
		mode = _T("Text to video");
	free(ct);
	return mode;
}

// Follow an image ref (through resize/crop nodes) back to a LoadImage-style node and
// return its file name. Takes ownership of `ref`.
static TCHAR *cf_resolve_image(const TCHAR *s, int len, TCHAR *ref) {
	for(int hop=0; ref && hop<4; hop++) {
		int is, ie;
		if(!cf_node_inputs_by_id(s, len, ref, &is, &ie)) break;
		TCHAR *img = cf_get_string(s, is, ie, _T("image"));
		if(img) { free(ref); return img; }
		TCHAR *next = cf_get_ref(s, is, ie, _T("image"));
		if(!next) next = cf_get_ref(s, is, ie, _T("images"));
		if(!next) next = cf_get_ref(s, is, ie, _T("pixels"));
		free(ref);
		ref = next;
	}
	free(ref);
	return NULL;
}

// Collect the input image file names of a video node, sorted by role.
static void cf_collect_images(const TCHAR *s, int len, int is, int ie,
	TCHAR *refs, TCHAR *first, TCHAR *last, int cap)
{
	int pos = is, vs, ve;
	TCHAR *key;
	while((key = cf_next_member(s, &pos, ie, &vs, &ve)) != NULL) {
		TCHAR *list = NULL;
		if(!_tcsncmp(key,_T("ref_image"),9) || _tcsstr(key,_T("reference")))
			list = refs;
		else if(!lstrcmp(key,_T("first_frame")) || !lstrcmp(key,_T("start_image")) ||
		        !lstrcmp(key,_T("start_frame")) || !lstrcmp(key,_T("image")))
			list = first;
		else if(!lstrcmp(key,_T("last_frame")) || !lstrcmp(key,_T("end_image")) ||
		        !lstrcmp(key,_T("end_frame")))
			list = last;
		free(key);
		if(!list || s[vs]!=_T('[')) continue;
		int after;
		TCHAR *ref = cf_parse_string(s, cf_skip_ws(s, vs+1, ve), ve, &after);
		TCHAR *name = cf_resolve_image(s, len, ref);
		if(!name) continue;
		if(list[0]) StringCchCat(list, cap, _T(", "));
		StringCchCat(list, cap, name);
		free(name);
	}
}

static void cf_add_lora(TCHAR *list, int cap, const TCHAR *name, const TCHAR *strength) {
	if(list[0]) StringCchCat(list, cap, _T(", "));
	StringCchCat(list, cap, name);
	if(strength && strength[0]) { StringCchCat(list, cap, _T(":")); StringCchCat(list, cap, strength); }
}

// LoRAs of one loader node: LoraLoader-style (lora_name + strength_model) or rgthree's
// Power Lora Loader ("lora_1": {"on": true, "lora": "...", "strength": 0.8}, ...).
static void cf_collect_loras(const TCHAR *s, int is, int ie, TCHAR *list, int cap) {
	TCHAR *name = cf_get_string(s, is, ie, _T("lora_name"));
	if(name) {
		TCHAR st[32] = _T("");
		if(!cf_get_number_str(s, is, ie, _T("strength_model"), st, 32))
			cf_get_number_str(s, is, ie, _T("strength"), st, 32);
		if(lstrcmp(name, _T("None"))) cf_add_lora(list, cap, name, st);
		free(name);
		return;
	}
	int pos = is, vs, ve;
	TCHAR *key;
	while((key = cf_next_member(s, &pos, ie, &vs, &ve)) != NULL) {
		free(key);
		if(s[vs]!=_T('{')) continue;
		TCHAR *lora = cf_get_string(s, vs+1, ve-1, _T("lora"));
		if(!lora) continue;
		int v;
		BOOL on = !cf_find_key(s, vs+1, ve-1, _T("on"), &v) || !_tcsncmp(&s[v], _T("true"), 4);
		TCHAR st[32] = _T("");
		cf_get_number_str(s, vs+1, ve-1, _T("strength"), st, 32);
		if(on && lstrcmp(lora, _T("None"))) cf_add_lora(list, cap, lora, st);
		free(lora);
	}
}

// Parse a ComfyUI prompt JSON and fill aiparams_ctx (pos, neg, setRaw). Returns TRUE
// on success. The caller still owns ctx->full.
static BOOL twpng_ParseComfyJson(const TCHAR *full, struct aiparams_ctx *ctx) {
	int len = lstrlen(full);

	static const TCHAR *samplerTypes[] = {
		_T("KSampler"), _T("KSamplerAdvanced"), _T("KSampler (Efficient)"),
		_T("SamplerCustom"), _T("SamplerCustomAdvanced"), _T("WanVideoSampler"), NULL };
	int iStart=-1, iEnd=-1;
	for(int t=0; samplerTypes[t] && iStart<0; t++)
		cf_find_class_inputs(full, len, samplerTypes[t], &iStart, &iEnd);
	if(iStart<0) return FALSE;

	struct cf_settings st;
	ZeroMemory(&st, sizeof(st));
	cf_read_settings(full, iStart, iEnd, &st);
	// SamplerCustom(Advanced) keeps its settings in separate noise/sampler/sigmas/guider
	// nodes (RandomNoise, KSamplerSelect, BasicScheduler, CFGGuider...).
	static const TCHAR *settingRefs[] = { _T("noise"), _T("sampler"), _T("sigmas"), _T("guider"), NULL };
	for(int k=0; settingRefs[k]; k++) {
		TCHAR *ref = cf_get_ref(full, iStart, iEnd, settingRefs[k]);
		int ns, ne;
		if(cf_node_inputs_by_id(full, len, ref, &ns, &ne)) cf_read_settings(full, ns, ne, &st);
		free(ref);
	}

	TCHAR *posRef = cf_get_ref(full, iStart, iEnd, _T("positive"));
	TCHAR *negRef = cf_get_ref(full, iStart, iEnd, _T("negative"));
	if(!posRef && !negRef) {
		TCHAR *g = cf_get_ref(full, iStart, iEnd, _T("guider"));
		int gs, ge;
		if(cf_node_inputs_by_id(full, len, g, &gs, &ge)) {
			posRef = cf_get_ref(full, gs, ge, _T("positive"));                 // CFGGuider
			if(!posRef) posRef = cf_get_ref(full, gs, ge, _T("conditioning")); // BasicGuider
			if(!posRef) posRef = cf_get_ref(full, gs, ge, _T("cond1"));        // DualCFGGuider
			negRef = cf_get_ref(full, gs, ge, _T("negative"));
		}
		free(g);
	}
	if(!posRef && !negRef) {
		// WanVideoSampler: one text_embeds node carries both prompts.
		posRef = cf_get_ref(full, iStart, iEnd, _T("text_embeds"));
		if(posRef) negRef = _tcsdup(posRef);
	}

	// Resolve positive/negative to the prompt text by walking the graph until we reach a
	// node with a string text input (skipping ConditioningSet*, *ToVideo pass-throughs...).
	// The first *ToVideo node on the positive path also gives the generation mode and
	// its input images.
	TCHAR *positive = NULL, *negative = NULL;
	const TCHAR *mode = NULL;
	int vStart = -1, vEnd = -1;
	for(int side=0; side<2; side++) {
		TCHAR *ref = (side==0) ? posRef : negRef;
		TCHAR **outp = (side==0) ? &positive : &negative;
		const TCHAR *textKeys[] = { _T("text"), _T("prompt"),
			(side==0) ? _T("positive_prompt") : _T("negative_prompt"), _T("text_g"), NULL };
		const TCHAR *follow[] = { (side==0) ? _T("positive") : _T("negative"),
			_T("conditioning"), _T("conditioning_1"), _T("conditioning_to"), _T("clip"), NULL };
		for(int hop=0; ref && hop<8 && !*outp; hop++) {
			int ns, ne;
			if(!cf_node_inputs_by_id(full, len, ref, &ns, &ne)) break;
			if(side==0 && !mode && (mode = cf_video_mode(full, len, ref, ns, ne)) != NULL) {
				vStart = ns; vEnd = ne;
			}
			for(int k=0; textKeys[k] && !*outp; k++) *outp = cf_get_text(full, len, ns, ne, textKeys[k]);
			if(*outp) break;
			TCHAR *next = NULL;
			for(int k=0; follow[k] && !next; k++) next = cf_get_ref(full, ns, ne, follow[k]);
			free(ref);
			ref = next;
		}
		free(ref);
	}

	// Size / frame count from the first latent-producing node.
	TCHAR width[16]=_T(""), height[16]=_T(""), frames[16]=_T(""), fps[32]=_T("");
	static const TCHAR *latentTypes[] = {
		_T("EmptyLatentImage"), _T("EmptySD3LatentImage"),
		_T("EmptyHunyuanLatentVideo"), _T("EmptyMochiLatentVideo"),
		_T("EmptyLTXVLatentVideo"), _T("EmptyCosmosLatentVideo"),
		_T("WanImageToVideo"), _T("WanVaceToVideo"), _T("HunyuanImageToVideo"), NULL };
	for(int t=0; latentTypes[t] && !width[0]; t++) {
		int is, ie;
		if(!cf_find_class_inputs(full, len, latentTypes[t], &is, &ie)) continue;
		cf_get_number_str(full, is, ie, _T("width"), width, 16);
		cf_get_number_str(full, is, ie, _T("height"), height, 16);
		if(!frames[0]) cf_get_number_str(full, is, ie, _T("length"), frames, 16);
	}
	static const struct { const TCHAR *cls; const TCHAR *key; } fpsNodes[] = {
		{ _T("CreateVideo"),      _T("fps") },
		{ _T("VHS_VideoCombine"), _T("frame_rate") },
		{ _T("SaveAnimatedWEBP"), _T("fps") },
		{ NULL, NULL } };
	for(int t=0; fpsNodes[t].cls && !fps[0]; t++) {
		int is, ie;
		if(cf_find_class_inputs(full, len, fpsNodes[t].cls, &is, &ie))
			cf_get_number_str(full, is, ie, fpsNodes[t].key, fps, 32);
	}

	// Find a model name from common loader class types.
	TCHAR *model = NULL;
	static const struct { const TCHAR *cls; const TCHAR *key; } loaders[] = {
		{ _T("UNETLoader"),             _T("unet_name") },
		{ _T("UnetLoaderGGUF"),         _T("unet_name") },
		{ _T("CheckpointLoaderSimple"), _T("ckpt_name") },
		{ _T("CheckpointLoader"),       _T("ckpt_name") },
		{ _T("WanVideoModelLoader"),    _T("model") },
		{ NULL, NULL } };
	for(int t=0; loaders[t].cls && !model; t++) {
		int is, ie;
		if(cf_find_class_inputs(full, len, loaders[t].cls, &is, &ie))
			model = cf_get_string(full, is, ie, loaders[t].key);
	}

	// LoRAs from every loader node (the API prompt only holds nodes that actually ran).
	TCHAR loras[600] = _T("");
	{
		int pos = cf_skip_ws(full, 0, len), vs, ve;
		if(pos<len && full[pos]==_T('{')) pos++;
		TCHAR *key;
		while((key = cf_next_member(full, &pos, len, &vs, &ve)) != NULL) {
			free(key);
			if(full[vs]!=_T('{')) continue;
			TCHAR *ct = cf_get_string(full, vs+1, ve-1, _T("class_type"));
			BOOL isLora = ct && _tcsstr(ct, _T("Lora")) != NULL;
			free(ct);
			int is, ie;
			if(isLora && cf_inputs_range(full, len, vs+1, ve-1, &is, &ie))
				cf_collect_loras(full, is, ie, loras, 600);
		}
	}

	// Build settings line.
	TCHAR setbuf[2000] = _T("");
	if(st.steps[0])    { StringCchCat(setbuf,2000,_T("Steps: "));         StringCchCat(setbuf,2000,st.steps); }
	if(st.sampler)     { StringCchCat(setbuf,2000,_T(", Sampler: "));     StringCchCat(setbuf,2000,st.sampler); }
	if(st.scheduler)   { StringCchCat(setbuf,2000,_T(", Schedule type: ")); StringCchCat(setbuf,2000,st.scheduler); }
	if(st.cfg[0])      { StringCchCat(setbuf,2000,_T(", CFG scale: "));   StringCchCat(setbuf,2000,st.cfg); }
	if(st.seed[0])     { StringCchCat(setbuf,2000,_T(", Seed: "));        StringCchCat(setbuf,2000,st.seed); }
	if(width[0] && height[0]) {
		StringCchCat(setbuf,2000,_T(", Size: "));
		StringCchCat(setbuf,2000,width); StringCchCat(setbuf,2000,_T("x")); StringCchCat(setbuf,2000,height);
	}
	if(frames[0])      { StringCchCat(setbuf,2000,_T(", Frames: "));      StringCchCat(setbuf,2000,frames); }
	if(fps[0])         { StringCchCat(setbuf,2000,_T(", FPS: "));         StringCchCat(setbuf,2000,fps); }
	if(mode)           { StringCchCat(setbuf,2000,_T(", Mode: "));        StringCchCat(setbuf,2000,mode); }
	if(vStart>=0) {
		TCHAR refs[600]=_T(""), first[600]=_T(""), last[600]=_T("");
		cf_collect_images(full, len, vStart, vEnd, refs, first, last, 600);
		// A list is quoted so the settings splitter keeps it on one line.
		if(refs[0]) {
			BOOL list = _tcschr(refs, _T(',')) != NULL;
			StringCchCat(setbuf,2000, list ? _T(", References: \"") : _T(", References: "));
			StringCchCat(setbuf,2000,refs);
			if(list) StringCchCat(setbuf,2000,_T("\""));
		}
		if(first[0]) { StringCchCat(setbuf,2000,_T(", First frame: "));  StringCchCat(setbuf,2000,first); }
		if(last[0])  { StringCchCat(setbuf,2000,_T(", Last frame: "));   StringCchCat(setbuf,2000,last); }
	}
	if(model)          { StringCchCat(setbuf,2000,_T(", Model: "));       StringCchCat(setbuf,2000,model); }
	if(loras[0]) {
		BOOL list = _tcschr(loras, _T(',')) != NULL;
		StringCchCat(setbuf,2000, list ? _T(", LoRAs: \"") : _T(", LoRA: "));
		StringCchCat(setbuf,2000,loras);
		if(list) StringCchCat(setbuf,2000,_T("\""));
	}
	if(st.denoise[0])  { StringCchCat(setbuf,2000,_T(", Denoising strength: ")); StringCchCat(setbuf,2000,st.denoise); }
	// "Steps:" must come first in the settings line for the A1111 parser to find it.
	if(setbuf[0]==_T(',')) memmove(setbuf,setbuf+2,(lstrlen(setbuf+2)+1)*sizeof(TCHAR));

	free(st.sampler);
	free(st.scheduler);
	free(model);

	ctx->pos = positive;
	ctx->neg = negative;  // may be NULL
	ctx->setRaw = NULL;
	if(setbuf[0]) {
		int sl = lstrlen(setbuf);
		ctx->setRaw = (TCHAR*)malloc(((size_t)sl+1)*sizeof(TCHAR));
		if(ctx->setRaw) memcpy(ctx->setRaw, setbuf, ((size_t)sl+1)*sizeof(TCHAR));
	}
	return (ctx->pos || ctx->neg || ctx->setRaw);
}

// ---- EXIF UserComment extraction from JPEG / WebP files ------------------------------

struct tiff_ctx { const unsigned char *base; DWORD len; BOOL be; };

static WORD tiff_read16(const struct tiff_ctx *t, DWORD off)
{
	if(off+2 > t->len) return 0;
	return t->be ? (WORD)((t->base[off]<<8) | t->base[off+1])
	             : (WORD)(t->base[off] | (t->base[off+1]<<8));
}

static DWORD tiff_read32(const struct tiff_ctx *t, DWORD off)
{
	if(off+4 > t->len) return 0;
	if(t->be) return ((DWORD)t->base[off]<<24)   | ((DWORD)t->base[off+1]<<16) |
	                 ((DWORD)t->base[off+2]<<8)  |  (DWORD)t->base[off+3];
	else      return ((DWORD)t->base[off+3]<<24) | ((DWORD)t->base[off+2]<<16) |
	                 ((DWORD)t->base[off+1]<<8)  |  (DWORD)t->base[off];
}

// In the IFD at ifdOff find `tag`; on success fills outType/outCount and
// outValOff (offset of the 4-byte value-or-offset field within the TIFF).
static BOOL tiff_find_tag(const struct tiff_ctx *t, DWORD ifdOff, WORD tag,
	WORD *outType, DWORD *outCount, DWORD *outValOff)
{
	if(ifdOff+2 > t->len) return FALSE;
	WORD n = tiff_read16(t, ifdOff);
	if(ifdOff+2+(DWORD)n*12 > t->len) return FALSE;
	for(WORD i=0;i<n;i++) {
		DWORD eo = ifdOff + 2 + (DWORD)i*12;
		if(tiff_read16(t, eo) == tag) {
			*outType   = tiff_read16(t, eo+2);
			*outCount  = tiff_read32(t, eo+4);
			*outValOff = eo+8;
			return TRUE;
		}
	}
	return FALSE;
}

// Convert the EXIF UserComment byte buffer (raw, including the 8-byte charset prefix)
// into an allocated TCHAR string. `tiff_be` indicates the TIFF's byte order; UNICODE
// data follows that byte order (UTF-16BE for "MM", UTF-16LE for "II").
static TCHAR *twpng_DecodeUserComment(const unsigned char *data, DWORD len, BOOL tiff_be)
{
	if(!data || len < 1) return NULL;
	const unsigned char *body = data;
	DWORD bodyLen = len;
	int charset = 0;   // 0=ASCII/Latin-1, 1=UTF-16, 2=UTF-8 (heuristic)
	if(len >= 8) {
		if(!memcmp(data,"UNICODE\0",8))         { charset=1; body+=8; bodyLen-=8; }
		else if(!memcmp(data,"ASCII\0\0\0",8))  { charset=0; body+=8; bodyLen-=8; }
		else if(!memcmp(data,"\0\0\0\0\0\0\0\0",8)) { charset=0; body+=8; bodyLen-=8; }
	}
	while(bodyLen>0 && body[bodyLen-1]==0) bodyLen--;
	if(bodyLen==0) return NULL;

	// "UNICODE" can be UTF-16 in either byte order (writers are inconsistent: the
	// Civitai/A1111 JPEG exporter writes UTF-16BE even though the TIFF is LE) or
	// occasionally UTF-8 with a mistagged marker. Detect from the data itself: count
	// zeros at even vs odd positions in the first ~32 bytes — LE has zero at the high
	// byte (odd position for ASCII), BE at the high byte (even position for ASCII).
	BOOL be_for_decode = tiff_be;
	if(charset==1) {
		int probe = (bodyLen<32) ? (int)bodyLen : 32;
		int zEven=0, zOdd=0;
		for(int i=0; i<probe; i++) {
			if(body[i]==0) { if(i & 1) zOdd++; else zEven++; }
		}
		if(zEven > zOdd*2)       be_for_decode = TRUE;
		else if(zOdd > zEven*2)  be_for_decode = FALSE;
		// Almost no zeros: definitely not UTF-16 ASCII text → treat as UTF-8.
		if(probe >= 8 && (zEven + zOdd) * 4 < probe) charset = 2;
	}

#ifdef UNICODE
	if(charset==1) {
		if(bodyLen%2) bodyLen--;
		int nchars = (int)(bodyLen/2);
		TCHAR *out = (TCHAR*)malloc(((size_t)nchars+1)*sizeof(TCHAR));
		if(!out) return NULL;
		if(be_for_decode) {
			for(int i=0; i<nchars; i++)
				out[i] = (TCHAR)(((unsigned)body[i*2]<<8) | (unsigned)body[i*2+1]);
		} else {
			memcpy(out, body, (size_t)nchars*2);
		}
		out[nchars]=0;
		return out;
	}
	// ASCII/UTF-8: try UTF-8, fall back to Windows-1252.
	UINT cp = (charset==2) ? CP_UTF8 : CP_UTF8;
	int wlen = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, (const char*)body, (int)bodyLen, NULL, 0);
	if(wlen<=0) {
		cp = 1252;
		wlen = MultiByteToWideChar(cp, 0, (const char*)body, (int)bodyLen, NULL, 0);
		if(wlen<=0) return NULL;
	}
	TCHAR *out = (TCHAR*)malloc(((size_t)wlen+1)*sizeof(TCHAR));
	if(!out) return NULL;
	MultiByteToWideChar(cp, 0, (const char*)body, (int)bodyLen, out, wlen);
	out[wlen]=0;
	return out;
#else
	(void)charset;
	TCHAR *out = (TCHAR*)malloc(((size_t)bodyLen+1)*sizeof(TCHAR));
	if(!out) return NULL;
	memcpy(out, body, bodyLen);
	out[bodyLen]=0;
	return out;
#endif
}

// Given a TIFF stream (the bytes immediately after "Exif\0\0" in a JPEG APP1, or the
// payload of a WebP EXIF chunk), extract the EXIF UserComment as a TCHAR string.
static int twpng_ExtractFromExifTiff(const unsigned char *tiff, DWORD tiffLen, TCHAR **outText)
{
	*outText = NULL;
	if(tiffLen < 8) return 0;
	BOOL be;
	if(tiff[0]=='I' && tiff[1]=='I')      be=FALSE;
	else if(tiff[0]=='M' && tiff[1]=='M') be=TRUE;
	else return 0;

	struct tiff_ctx ctx = { tiff, tiffLen, be };
	if(tiff_read16(&ctx, 2) != 0x002A) return 0;
	DWORD ifd0 = tiff_read32(&ctx, 4);
	if(ifd0+2 > tiffLen) return 0;

	WORD type; DWORD count, valOff;
	if(!tiff_find_tag(&ctx, ifd0, 0x8769, &type, &count, &valOff)) return 0; // ExifIFDPointer
	DWORD exifIfdOff = tiff_read32(&ctx, valOff);
	if(exifIfdOff+2 > tiffLen) return 0;

	if(!tiff_find_tag(&ctx, exifIfdOff, 0x9286, &type, &count, &valOff)) return 0; // UserComment
	if(type != 7 || count < 1) return 0; // UNDEFINED

	DWORD dataOff = (count<=4) ? valOff : tiff_read32(&ctx, valOff);
	if(dataOff+count > tiffLen) return 0;

	*outText = twpng_DecodeUserComment(tiff+dataOff, count, be);
	return *outText ? 1 : 0;
}

// Read a whole file into memory (capped). Caller frees with free().
static int twpng_SlurpFile(const TCHAR *fn, DWORD cap, unsigned char **outBuf, DWORD *outSize)
{
	*outBuf=NULL; *outSize=0;
	HANDLE fh = CreateFile(fn, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if(fh==INVALID_HANDLE_VALUE) return 0;
	DWORD fsz = GetFileSize(fh, NULL);
	if(fsz==INVALID_FILE_SIZE) { CloseHandle(fh); return 0; }
	if(fsz > cap) fsz = cap;
	if(fsz < 4) { CloseHandle(fh); return 0; }
	unsigned char *buf = (unsigned char*)malloc(fsz);
	if(!buf) { CloseHandle(fh); return 0; }
	DWORD got = 0;
	if(!ReadFile(fh, buf, fsz, &got, NULL) || got<4) { free(buf); CloseHandle(fh); return 0; }
	CloseHandle(fh);
	*outBuf = buf; *outSize = got;
	return 1;
}

// Locate the EXIF TIFF payload in a JPEG (APP1 marker, "Exif\0\0" prefix).
// We deliberately report the TIFF length as "rest of file from the TIFF start" rather
// than the APP1 segment's declared length, because some AI image writers (Civitai etc.)
// write a small APP1 header whose declared length doesn't cover the full UserComment —
// the TIFF offsets reference data beyond the segment boundary. The TIFF parser will
// still bounds-check against the value we return.
static int twpng_FindJpegExif(const unsigned char *buf, DWORD len,
	const unsigned char **outTiff, DWORD *outTiffLen)
{
	if(len<4 || buf[0]!=0xFF || buf[1]!=0xD8) return 0;
	DWORD p = 2;
	while(p+4 <= len) {
		if(buf[p] != 0xFF) return 0;
		while(p<len && buf[p]==0xFF) p++;
		if(p>=len) return 0;
		BYTE marker = buf[p++];
		if(marker==0x00 || marker==0xFF) continue;
		if(marker==0xD8 || marker==0xD9 || marker==0x01 ||
		   (marker>=0xD0 && marker<=0xD7)) continue;          // standalone markers
		if(marker==0xDA) return 0;                            // SOS: scan data follows
		if(p+2 > len) return 0;
		WORD seglen = (WORD)((buf[p]<<8) | buf[p+1]);
		if(seglen<2) return 0;
		// Don't bail out if seglen would exceed len — some writers lie about seglen.
		// We only need enough bytes inside the segment to recognize the EXIF prefix.
		DWORD declaredEnd = p + seglen;
		DWORD segDataAvail = (declaredEnd > len ? len : declaredEnd) - p - 2;
		const unsigned char *seg = &buf[p+2];
		if(marker==0xE1 && segDataAvail>=6 && !memcmp(seg,"Exif\0\0",6)) {
			DWORD tiffOff = p + 2 + 6;
			*outTiff = &buf[tiffOff];
			*outTiffLen = len - tiffOff;   // up to end of file, not end of segment
			return 1;
		}
		if(p + seglen > len) return 0;
		p += seglen;
	}
	return 0;
}

static int twpng_ReadJpegAIParams(const TCHAR *fn, TCHAR **outText)
{
	*outText = NULL;
	unsigned char *buf; DWORD size;
	if(!twpng_SlurpFile(fn, 32UL*1024*1024, &buf, &size)) return 0;
	const unsigned char *tiff; DWORD tiffLen;
	int ok = twpng_FindJpegExif(buf, size, &tiff, &tiffLen) &&
	         twpng_ExtractFromExifTiff(tiff, tiffLen, outText);
	free(buf);
	return ok;
}

static int twpng_ReadWebpAIParams(const TCHAR *fn, TCHAR **outText)
{
	*outText = NULL;
	unsigned char *buf; DWORD size;
	if(!twpng_SlurpFile(fn, 128UL*1024*1024, &buf, &size)) return 0;
	int ok = 0;
	if(size>=12 && !memcmp(buf,"RIFF",4) && !memcmp(buf+8,"WEBP",4)) {
		DWORD p = 12;
		while(p+8 <= size) {
			DWORD chunkSize = (DWORD)buf[p+4] | ((DWORD)buf[p+5]<<8) |
			                  ((DWORD)buf[p+6]<<16) | ((DWORD)buf[p+7]<<24);
			DWORD bodyAt = p+8;
			if(bodyAt+chunkSize > size) break;
			if(!memcmp(buf+p,"EXIF",4)) {
				ok = twpng_ExtractFromExifTiff(buf+bodyAt, chunkSize, outText);
				break;
			}
			p = bodyAt + chunkSize + (chunkSize & 1);   // chunks are padded to even
		}
	}
	free(buf);
	return ok;
}

// ---- ComfyUI metadata from MP4 / MOV ---------------------------------------------------
// ComfyUI's SaveVideo writes the prompt graph as a QuickTime "mdta" key `prompt` (moov/
// udta/meta/keys+ilst). VideoHelperSuite writes `comment` (iTunes 0xA9 "cmt") holding
// {"prompt": "<graph json>", "workflow": {...}}.

static DWORD mp4_be32(const unsigned char *p)
{
	return ((DWORD)p[0]<<24) | ((DWORD)p[1]<<16) | ((DWORD)p[2]<<8) | (DWORD)p[3];
}

// Find child box `type` in [p, end). Returns its payload and sets *outLen.
static const unsigned char *mp4_find_box(const unsigned char *p, const unsigned char *end,
	const char *type, DWORD *outLen)
{
	while(end-p >= 8) {
		unsigned __int64 sz = mp4_be32(p);
		DWORD hdr = 8;
		if(sz==1) {
			if(end-p < 16) return NULL;
			sz = ((unsigned __int64)mp4_be32(p+8)<<32) | mp4_be32(p+12);
			hdr = 16;
		}
		else if(sz==0) sz = (unsigned __int64)(end-p);
		if(sz < hdr || sz > (unsigned __int64)(end-p)) return NULL;
		if(!memcmp(p+4, type, 4)) { *outLen = (DWORD)sz - hdr; return p+hdr; }
		p += (size_t)sz;
	}
	return NULL;
}

static TCHAR *mp4_decode_text(const unsigned char *s, DWORD len, BOOL utf16be)
{
	while(len>0 && s[len-1]==0) len--;
	if(len==0) return NULL;
#ifdef UNICODE
	if(utf16be) {
		int n = (int)(len/2);
		TCHAR *out = (TCHAR*)malloc(((size_t)n+1)*sizeof(TCHAR));
		if(!out) return NULL;
		for(int i=0; i<n; i++) out[i] = (TCHAR)(((unsigned)s[i*2]<<8) | s[i*2+1]);
		out[n]=0;
		return out;
	}
	int wlen = MultiByteToWideChar(CP_UTF8, 0, (const char*)s, (int)len, NULL, 0);
	if(wlen<=0) return NULL;
	TCHAR *out = (TCHAR*)malloc(((size_t)wlen+1)*sizeof(TCHAR));
	if(!out) return NULL;
	MultiByteToWideChar(CP_UTF8, 0, (const char*)s, (int)len, out, wlen);
	out[wlen]=0;
	return out;
#else
	(void)utf16be;
	TCHAR *out = (TCHAR*)malloc((size_t)len+1);
	if(!out) return NULL;
	memcpy(out, s, len);
	out[len]=0;
	return out;
#endif
}

// Metadata slots, in order of preference.
enum { MP4_PROMPT, MP4_PARAMETERS, MP4_COMMENT, MP4_DESCRIPTION, MP4_NSLOTS };

static int mp4_slot_for_name(const unsigned char *name, DWORD len)
{
	static const struct { const char *name; int slot; } names[] = {
		{ "prompt", MP4_PROMPT }, { "parameters", MP4_PARAMETERS },
		{ "comment", MP4_COMMENT }, { "com.apple.quicktime.comment", MP4_COMMENT },
		{ "description", MP4_DESCRIPTION }, { "com.apple.quicktime.description", MP4_DESCRIPTION },
		{ "\xa9" "cmt", MP4_COMMENT }, { "\xa9" "des", MP4_DESCRIPTION }, { "desc", MP4_DESCRIPTION },
		{ NULL, 0 } };
	for(int i=0; names[i].name; i++) {
		if(strlen(names[i].name)==len && !memcmp(names[i].name, name, len)) return names[i].slot;
	}
	return -1;
}

// Parse a meta box payload (keys + ilst) into the slots.
static void mp4_parse_meta(const unsigned char *p, DWORD len, TCHAR **slots)
{
	// ISO meta is a FullBox (4 bytes version/flags); QuickTime's isn't. A child box can't
	// start with a zero size, so a leading zero word means version/flags.
	if(len>=4 && mp4_be32(p)==0) { p+=4; len-=4; }
	const unsigned char *end = p+len;

	const unsigned char *keyName[256];
	DWORD keyLen[256];
	DWORD nkeys = 0;
	DWORD klen;
	const unsigned char *keys = mp4_find_box(p, end, "keys", &klen);
	if(keys && klen>=8) {
		DWORD count = mp4_be32(keys+4);
		const unsigned char *k = keys+8, *kend = keys+klen;
		for(DWORD i=0; i<count && nkeys<256 && kend-k>=8; i++) {
			DWORD ksz = mp4_be32(k);
			if(ksz<8 || ksz>(DWORD)(kend-k)) break;
			keyName[nkeys] = k+8;          // skip size + namespace ("mdta")
			keyLen[nkeys]  = ksz-8;
			nkeys++;
			k += ksz;
		}
	}

	DWORD ilen;
	const unsigned char *ilst = mp4_find_box(p, end, "ilst", &ilen);
	if(!ilst) return;
	const unsigned char *it = ilst, *iend = ilst+ilen;
	while(iend-it >= 8) {
		DWORD isz = mp4_be32(it);
		if(isz<8 || isz>(DWORD)(iend-it)) break;
		// With a keys table the item type is a 1-based key index, otherwise a fourcc.
		DWORD idx = mp4_be32(it+4);
		int slot = (nkeys && idx>=1 && idx<=nkeys) ? mp4_slot_for_name(keyName[idx-1], keyLen[idx-1])
		                                           : mp4_slot_for_name(it+4, 4);
		DWORD dlen;
		const unsigned char *data = mp4_find_box(it+8, it+isz, "data", &dlen);
		if(slot>=0 && !slots[slot] && data && dlen>8) {
			DWORD dtype = mp4_be32(data) & 0xFFFFFF;   // 1=UTF-8, 2=UTF-16BE
			slots[slot] = mp4_decode_text(data+8, dlen-8, dtype==2);
		}
		it += isz;
	}
}

// Read the moov box (it may sit before or after mdat) without loading the media data.
static unsigned char *mp4_read_moov(const TCHAR *fn, DWORD *outLen, unsigned __int64 *outOff)
{
	HANDLE fh = CreateFile(fn, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if(fh==INVALID_HANDLE_VALUE) return NULL;
	LARGE_INTEGER fsz;
	if(!GetFileSizeEx(fh, &fsz)) { CloseHandle(fh); return NULL; }
	unsigned char *buf = NULL;
	unsigned __int64 off = 0, total = (unsigned __int64)fsz.QuadPart;
	while(off+8 <= total) {
		LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
		unsigned char h[16]; DWORD got = 0;
		if(!SetFilePointerEx(fh, li, NULL, FILE_BEGIN) || !ReadFile(fh, h, 16, &got, NULL) || got<8) break;
		unsigned __int64 sz = mp4_be32(h);
		DWORD hdr = 8;
		if(sz==1) {
			if(got<16) break;
			sz = ((unsigned __int64)mp4_be32(h+8)<<32) | mp4_be32(h+12);
			hdr = 16;
		}
		else if(sz==0) sz = total-off;
		if(sz<hdr || sz>total-off) break;
		if(!memcmp(h+4, "moov", 4)) {
			if(sz-hdr > 64UL*1024*1024) break;
			DWORD n = (DWORD)(sz-hdr);
			buf = (unsigned char*)malloc(n ? n : 1);
			li.QuadPart = (LONGLONG)(off+hdr);
			if(!buf || !SetFilePointerEx(fh, li, NULL, FILE_BEGIN) ||
			   !ReadFile(fh, buf, n, &got, NULL) || got!=n) {
				free(buf); buf = NULL;
			}
			else {
				*outLen = n;
				if(outOff) *outOff = off+hdr;
			}
			break;
		}
		off += sz;
	}
	CloseHandle(fh);
	return buf;
}

static int twpng_ReadMp4AIParams(const TCHAR *fn, TCHAR **outText)
{
	*outText = NULL;
	DWORD mlen = 0;
	unsigned char *moov = mp4_read_moov(fn, &mlen, NULL);
	if(!moov) return 0;
	const unsigned char *mend = moov+mlen;

	TCHAR *slots[MP4_NSLOTS] = { NULL };
	DWORD len;
	const unsigned char *udta = mp4_find_box(moov, mend, "udta", &len);
	if(udta) {
		DWORD mlen2;
		const unsigned char *meta = mp4_find_box(udta, udta+len, "meta", &mlen2);
		if(meta) mp4_parse_meta(meta, mlen2, slots);
		// Classic QuickTime user data: 0xA9 "cmt" = 16-bit length, 16-bit language, text.
		DWORD clen;
		const unsigned char *cmt = mp4_find_box(udta, udta+len, "\xa9" "cmt", &clen);
		if(cmt && !slots[MP4_COMMENT] && clen>4) {
			DWORD tl = ((DWORD)cmt[0]<<8) | cmt[1];
			if(tl > clen-4) tl = clen-4;
			slots[MP4_COMMENT] = mp4_decode_text(cmt+4, tl, FALSE);
		}
	}
	const unsigned char *meta = mp4_find_box(moov, mend, "meta", &len);
	if(meta) mp4_parse_meta(meta, len, slots);
	free(moov);

	for(int i=0; i<MP4_NSLOTS; i++) {
		if(slots[i] && !*outText) *outText = slots[i];
		else free(slots[i]);
	}
	if(!*outText) return 0;

	// VideoHelperSuite wraps the graph: {"prompt": "<json string>" | {...}, "workflow": ...}.
	TCHAR *s = *outText;
	int slen = lstrlen(s);
	int i = cf_skip_ws(s, 0, slen);
	int v;
	if(i<slen && s[i]==_T('{') && cf_find_key(s, i+1, slen, _T("prompt"), &v)) {
		TCHAR *inner = NULL;
		if(s[v]==_T('"')) {
			int after;
			inner = cf_parse_string(s, v, slen, &after);
		}
		else if(s[v]==_T('{')) {
			int after = cf_match_brace(s, v, slen);
			if(after>0) {
				inner = (TCHAR*)malloc(((size_t)(after-v)+1)*sizeof(TCHAR));
				if(inner) { memcpy(inner, s+v, (size_t)(after-v)*sizeof(TCHAR)); inner[after-v]=0; }
			}
		}
		if(inner && twpng_LooksLikeComfyJson(inner)) { free(s); *outText = inner; }
		else free(inner);
	}
	return 1;
}

// Size, frame count and duration of the first video track, as settings entries
// ("Video size: 832x480, Frames: 121, Duration: 5.04 s"). Returns 1 if found.
static int twpng_ReadMp4VideoInfo(const TCHAR *fn, TCHAR *out, int cap)
{
	out[0] = 0;
	DWORD mlen = 0;
	unsigned char *moov = mp4_read_moov(fn, &mlen, NULL);
	if(!moov) return 0;
	const unsigned char *p = moov, *end = moov+mlen;
	while(end-p >= 8 && !out[0]) {
		DWORD sz = mp4_be32(p);
		if(sz<8 || sz>(DWORD)(end-p)) break;
		const unsigned char *t = p+8, *tend = p+sz;
		DWORD len, hl;
		const unsigned char *mdia = memcmp(p+4,"trak",4) ? NULL : mp4_find_box(t, tend, "mdia", &len);
		const unsigned char *hdlr = mdia ? mp4_find_box(mdia, mdia+len, "hdlr", &hl) : NULL;
		if(hdlr && hl>=12 && !memcmp(hdlr+8, "vide", 4)) {
			DWORD w = 0, h = 0, frames = 0, tl, ml, nl, bl, zl;
			const unsigned char *tkhd = mp4_find_box(t, tend, "tkhd", &tl);
			if(tkhd && tl>=8) { w = mp4_be32(tkhd+tl-8)>>16; h = mp4_be32(tkhd+tl-4)>>16; }
			double secs = 0;
			const unsigned char *mdhd = mp4_find_box(mdia, mdia+len, "mdhd", &ml);
			if(mdhd && ml>=20) {
				DWORD scale;
				unsigned __int64 dur;
				if(mdhd[0]==1 && ml>=32) {
					scale = mp4_be32(mdhd+20);
					dur = ((unsigned __int64)mp4_be32(mdhd+24)<<32) | mp4_be32(mdhd+28);
				} else {
					scale = mp4_be32(mdhd+12);
					dur = mp4_be32(mdhd+16);
				}
				if(scale) secs = (double)dur/scale;
			}
			const unsigned char *minf = mp4_find_box(mdia, mdia+len, "minf", &nl);
			const unsigned char *stbl = minf ? mp4_find_box(minf, minf+nl, "stbl", &bl) : NULL;
			const unsigned char *stsz = stbl ? mp4_find_box(stbl, stbl+bl, "stsz", &zl) : NULL;
			if(!stsz && stbl) stsz = mp4_find_box(stbl, stbl+bl, "stz2", &zl);
			if(stsz && zl>=12) frames = mp4_be32(stsz+8);
			TCHAR item[64];
			if(w && h) { StringCchPrintf(item, 64, _T("Video size: %ux%u"), w, h); StringCchCat(out, cap, item); }
			if(frames) {
				StringCchPrintf(item, 64, _T("%sFrames: %u"), out[0] ? _T(", ") : _T(""), frames);
				StringCchCat(out, cap, item);
			}
			if(secs>0) {
				StringCchPrintf(item, 64, _T("%sDuration: %.2f s"), out[0] ? _T(", ") : _T(""), secs);
				StringCchCat(out, cap, item);
			}
			if(!out[0]) StringCchCopy(out, cap, _T(" "));   // video track found, nothing usable
		}
		p += sz;
	}
	free(moov);
	if(out[0]==_T(' ')) out[0] = 0;
	return out[0]!=0;
}

// Sniff a file header: 1=JPEG, 2=WebP, 3=MP4, 4=MOV, 0=other.
static int twpng_SniffForeignKind(const unsigned char *hdr, DWORD got)
{
	if(got>=2 && hdr[0]==0xFF && hdr[1]==0xD8) return 1;
	if(got>=12 && !memcmp(hdr,"RIFF",4) && !memcmp(hdr+8,"WEBP",4)) return 2;
	if(got>=12 && !memcmp(hdr+4,"ftyp",4)) return memcmp(hdr+8,"qt  ",4) ? 3 : 4;
	if(got>=8 && (!memcmp(hdr+4,"moov",4) || !memcmp(hdr+4,"wide",4) ||
	              !memcmp(hdr+4,"mdat",4) || !memcmp(hdr+4,"free",4))) return 4;
	return 0;
}

static int twpng_ReadForeignAIParams(int kind, const TCHAR *fn, TCHAR **outText)
{
	*outText = NULL;
	if(kind==1) return twpng_ReadJpegAIParams(fn, outText);
	if(kind==2) return twpng_ReadWebpAIParams(fn, outText);
	if(kind==3 || kind==4) return twpng_ReadMp4AIParams(fn, outText);
	return 0;
}

// ---- Strip metadata from JPEG / WebP / MP4 --------------------------------------------

// JPEG without APP1 (EXIF/XMP), APP13 (IPTC) and COM segments. NULL if the file doesn't
// parse up to its image data, so we never write a broken copy.
static unsigned char *twpng_StripJpeg(const unsigned char *buf, DWORD len, DWORD *outLen, int *removed)
{
	*removed = 0;
	if(len<4 || buf[0]!=0xFF || buf[1]!=0xD8) return NULL;
	unsigned char *out = (unsigned char*)malloc(len);
	if(!out) return NULL;
	DWORD o = 0, p = 2;
	out[o++] = 0xFF; out[o++] = 0xD8;
	while(p+2 <= len) {
		if(buf[p]!=0xFF) { p++; continue; }   // junk after a too-short APP1 (Civitai)
		while(p<len && buf[p]==0xFF) p++;
		if(p>=len) break;
		BYTE marker = buf[p++];
		if(marker<0xC0) continue;               // 0xFF inside junk, not a marker
		if(marker==0xD8 || (marker>=0xD0 && marker<=0xD7)) { out[o++]=0xFF; out[o++]=marker; continue; }
		if(marker==0xDA || marker==0xD9) {      // image data: copy the rest verbatim
			out[o++] = 0xFF;
			memcpy(out+o, buf+p-1, len-(p-1));
			o += len-(p-1);
			*outLen = o;
			return out;
		}
		if(p+2 > len) break;
		DWORD seglen = ((DWORD)buf[p]<<8) | buf[p+1];
		if(seglen<2 || p+seglen>len) break;
		if(marker==0xE1 || marker==0xED || marker==0xFE) {
			(*removed)++;
		} else {
			out[o++] = 0xFF; out[o++] = marker;
			memcpy(out+o, buf+p, seglen);
			o += seglen;
		}
		p += seglen;
	}
	free(out);
	return NULL;
}

static DWORD le32(const unsigned char *p)
{
	return (DWORD)p[0] | ((DWORD)p[1]<<8) | ((DWORD)p[2]<<16) | ((DWORD)p[3]<<24);
}

// WebP without EXIF and XMP chunks; the VP8X flags and RIFF size are updated to match.
static unsigned char *twpng_StripWebp(const unsigned char *buf, DWORD len, DWORD *outLen, int *removed)
{
	*removed = 0;
	if(len<12 || memcmp(buf,"RIFF",4) || memcmp(buf+8,"WEBP",4)) return NULL;
	unsigned char *out = (unsigned char*)malloc(len);
	if(!out) return NULL;
	memcpy(out, buf, 12);
	DWORD o = 12, p = 12, vp8x = 0;
	while(p+8 <= len) {
		DWORD cs = le32(buf+p+4);
		if(cs > len-p-8) break;
		DWORD total = 8 + cs + (cs & 1);
		if(total > len-p) total = len-p;
		if(!memcmp(buf+p,"EXIF",4) || !memcmp(buf+p,"XMP ",4)) {
			(*removed)++;
		} else {
			if(!memcmp(buf+p,"VP8X",4)) vp8x = o;
			memcpy(out+o, buf+p, total);
			o += total;
		}
		p += total;
	}
	if(vp8x && o >= vp8x+9) out[vp8x+8] &= (unsigned char)~(0x08|0x04);   // EXIF, XMP flags
	DWORD riff = o-8;
	out[4] = (unsigned char)riff; out[5] = (unsigned char)(riff>>8);
	out[6] = (unsigned char)(riff>>16); out[7] = (unsigned char)(riff>>24);
	*outLen = o;
	return out;
}

// Turn moov/udta and moov/meta into "free" boxes in place. Sizes and offsets don't change,
// so the media data is untouched. With dryRun only counts them.
static int twpng_StripMp4File(const TCHAR *fn, BOOL dryRun, int *removed)
{
	*removed = 0;
	DWORD mlen = 0;
	unsigned __int64 moff = 0;
	unsigned char *moov = mp4_read_moov(fn, &mlen, &moff);
	if(!moov) return 0;
	HANDLE fh = INVALID_HANDLE_VALUE;
	if(!dryRun) {
		fh = CreateFile(fn, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if(fh==INVALID_HANDLE_VALUE) { free(moov); return 0; }
	}
	int ok = 1;
	const unsigned char *p = moov, *end = moov+mlen;
	while(end-p >= 8) {
		unsigned __int64 sz = mp4_be32(p);
		if(sz==1 && end-p>=16) sz = ((unsigned __int64)mp4_be32(p+8)<<32) | mp4_be32(p+12);
		else if(sz==0) sz = (unsigned __int64)(end-p);
		if(sz<8 || sz>(unsigned __int64)(end-p)) break;
		if(!memcmp(p+4,"udta",4) || !memcmp(p+4,"meta",4)) {
			(*removed)++;
			if(!dryRun) {
				LARGE_INTEGER li;
				li.QuadPart = (LONGLONG)(moff + (unsigned __int64)(p-moov) + 4);
				DWORD w = 0;
				if(!SetFilePointerEx(fh, li, NULL, FILE_BEGIN) || !WriteFile(fh, "free", 4, &w, NULL) || w!=4)
					ok = 0;
				// Zero the old contents too, so the prompt isn't left readable in the file.
				DWORD hdr = (mp4_be32(p)==1) ? 16 : 8;
				DWORD n = (DWORD)sz - hdr;
				unsigned char *zero = (unsigned char*)calloc(n ? n : 1, 1);
				li.QuadPart = (LONGLONG)(moff + (unsigned __int64)(p-moov) + hdr);
				if(!zero || !SetFilePointerEx(fh, li, NULL, FILE_BEGIN) || !WriteFile(fh, zero, n, &w, NULL) || w!=n)
					ok = 0;
				free(zero);
			}
		}
		p += (size_t)sz;
	}
	if(fh!=INVALID_HANDLE_VALUE) CloseHandle(fh);
	free(moov);
	return ok;
}

static BOOL twpng_WriteWholeFile(const TCHAR *fn, const unsigned char *data, DWORD len)
{
	HANDLE fh = CreateFile(fn, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if(fh==INVALID_HANDLE_VALUE) return FALSE;
	DWORD w = 0;
	BOOL ok = WriteFile(fh, data, len, &w, NULL) && w==len;
	CloseHandle(fh);
	return ok;
}

// Strip AI Metadata for a loaded JPEG/WebP/MP4/MOV: writes a cleaned copy (or replaces
// the file if the user picks the same name).
static void twpng_StripForeignMetadata()
{
	const TCHAR *src = g_foreignFn;
	int kind = g_foreignKind, removed = 0;
	unsigned char *buf = NULL, *out = NULL;
	DWORD size = 0, outLen = 0;

	if(kind==3 || kind==4) {
		twpng_StripMp4File(src, TRUE, &removed);
	} else {
		if(!twpng_SlurpFile(src, 512UL*1024*1024, &buf, &size)) { mesg(MSG_E,_T("Can") SYM_RSQUO _T("t read %s"), src); return; }
		out = (kind==1) ? twpng_StripJpeg(buf, size, &outLen, &removed)
		                : twpng_StripWebp(buf, size, &outLen, &removed);
		free(buf);
		if(!out) { mesg(MSG_E,_T("The file structure could not be parsed, nothing was changed.")); return; }
	}
	if(removed<1) {
		free(out);
		mesg(MSG_I,_T("No metadata was found in this file."));
		return;
	}

	TCHAR fn[MAX_PATH], dir[MAX_PATH];
	const TCHAR *base = _tcsrchr(src, _T('\\'));
	StringCchCopy(dir, MAX_PATH, src);
	if(base) dir[base-src+1] = 0; else dir[0] = 0;
	StringCchCopy(fn, MAX_PATH, src);   // full path, so the dialog opens in the source folder
	TCHAR ext[16] = _T("");
	TCHAR *dot = _tcsrchr(fn, _T('.'));
	if(dot) { StringCchCopy(ext, 16, dot); *dot = 0; }
	StringCchCat(fn, MAX_PATH, _T("_clean"));
	StringCchCat(fn, MAX_PATH, ext);

	OPENFILENAME ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = globals.hwndMain;
	ofn.lpstrFilter = _T("Same type as the original\0*.*\0\0");
	ofn.lpstrFile = fn;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrInitialDir = dir[0] ? dir : NULL;
	ofn.lpstrTitle = _T("Save a copy without AI metadata");
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
	if(!GetSaveFileName(&ofn)) { free(out); return; }

	BOOL same = !lstrcmpi(fn, src);
	BOOL ok;
	if(kind==3 || kind==4) {
		ok = same || CopyFile(src, fn, FALSE);
		if(ok) ok = twpng_StripMp4File(fn, FALSE, &removed);
	} else {
		ok = twpng_WriteWholeFile(fn, out, outLen);
		free(out);
	}
	if(!ok) { mesg(MSG_E,_T("Could not write %s"), fn); return; }
	if(same) OpenPngByName(fn);   // refresh the badge and viewer for the cleaned file
	mesg(MSG_I,_T("Removed %d metadata block%s.\r\n\r\nSaved: %s"), removed, removed==1?_T(""):_T("s"), fn);
}

// ---- Compare AI parameters of two files ----------------------------------------------

struct twpng_tbuf { TCHAR *s; size_t len, cap; };

static void tb_add(struct twpng_tbuf *b, const TCHAR *t, int n)
{
	if(!t) return;
	if(n<0) n = lstrlen(t);
	if(b->len + n + 1 > b->cap) {
		size_t ncap = (b->cap ? b->cap*2 : 4096);
		while(ncap < b->len + n + 1) ncap *= 2;
		TCHAR *ns = (TCHAR*)realloc(b->s, ncap*sizeof(TCHAR));
		if(!ns) return;
		b->s = ns; b->cap = ncap;
	}
	memcpy(b->s + b->len, t, (size_t)n*sizeof(TCHAR));
	b->len += n;
	b->s[b->len] = 0;
}

struct twpng_kv { TCHAR key[80]; TCHAR *val; };

// Settings line into key/value pairs (the same split the viewer uses for display).
static int twpng_SplitSettings(const TCHAR *set, struct twpng_kv *kv, int max)
{
	TCHAR *lines = twpng_SettingsToLines(set);
	if(!lines) return 0;
	int n = 0;
	TCHAR *line = lines;
	while(line && *line && n<max) {
		TCHAR *nl = _tcsstr(line, _T("\r\n"));
		if(nl) *nl = 0;
		TCHAR *colon = _tcsstr(line, _T(": "));
		if(colon) {
			*colon = 0;
			StringCchCopy(kv[n].key, 80, line);
			kv[n].val = _tcsdup(colon+2);
		} else {
			StringCchCopy(kv[n].key, 80, line);
			kv[n].val = _tcsdup(_T(""));
		}
		n++;
		line = nl ? nl+2 : NULL;
	}
	free(lines);
	return n;
}

// Words of a text; a whitespace run holding a line break becomes a "\n" token.
static int twpng_Tokenize(const TCHAR *s, const TCHAR ***outTok, int **outLen)
{
	int cap = 256, n = 0;
	const TCHAR **tok = (const TCHAR**)malloc(cap*sizeof(TCHAR*));
	int *tl = (int*)malloc(cap*sizeof(int));
	if(!tok || !tl) { free(tok); free(tl); *outTok = NULL; *outLen = NULL; return 0; }
	const TCHAR *p = s ? s : _T("");
	while(*p) {
		const TCHAR *ws = p;
		BOOL brk = FALSE;
		while(*p==_T(' ') || *p==_T('\t') || *p==_T('\r') || *p==_T('\n')) { if(*p==_T('\n')) brk = TRUE; p++; }
		if(n+1 >= cap) {
			cap *= 2;
			const TCHAR **nt = (const TCHAR**)realloc((void*)tok, cap*sizeof(TCHAR*));
			int *nl = (int*)realloc(tl, cap*sizeof(int));
			if(!nt || !nl) { if(nt) tok = nt; if(nl) tl = nl; break; }
			tok = nt; tl = nl;
		}
		if(brk && n>0) { tok[n] = _T("\n"); tl[n] = 1; n++; }
		(void)ws;
		if(!*p) break;
		const TCHAR *w = p;
		while(*p && *p!=_T(' ') && *p!=_T('\t') && *p!=_T('\r') && *p!=_T('\n')) p++;
		tok[n] = w; tl[n] = (int)(p-w); n++;
	}
	*outTok = tok; *outLen = tl;
	return n;
}

static void tb_add_tokens(struct twpng_tbuf *b, const TCHAR **tok, const int *tl, int from, int to,
	const TCHAR *open, const TCHAR *close)
{
	if(from>=to) return;
	tb_add(b, open, -1);
	for(int i=from; i<to; i++) {
		if(tl[i]==1 && tok[i][0]==_T('\n')) { tb_add(b, _T("\r\n"), 2); continue; }
		if(i>from && !(tl[i-1]==1 && tok[i-1][0]==_T('\n'))) tb_add(b, _T(" "), 1);
		tb_add(b, tok[i], tl[i]);
	}
	tb_add(b, close, -1);
}

// Word diff: unchanged words as-is, removed words as [-...-], added words as {+...+}.
static void twpng_DiffWords(struct twpng_tbuf *b, const TCHAR *x, const TCHAR *y)
{
	const TCHAR **ta, **tb2;
	int *la, *lb;
	int na = twpng_Tokenize(x, &ta, &la), nb = twpng_Tokenize(y, &tb2, &lb);
	unsigned short *dp = NULL;
	if(na<=3000 && nb<=3000)
		dp = (unsigned short*)calloc((size_t)(na+1)*(nb+1), sizeof(unsigned short));
	if(!dp) {
		tb_add(b, _T("A:\r\n"), -1); tb_add_tokens(b, ta, la, 0, na, _T(""), _T(""));
		tb_add(b, _T("\r\n\r\nB:\r\n"), -1); tb_add_tokens(b, tb2, lb, 0, nb, _T(""), _T(""));
	} else {
		#define DP(i,j) dp[(size_t)(i)*(nb+1)+(j)]
		for(int i=na-1; i>=0; i--)
			for(int j=nb-1; j>=0; j--)
				DP(i,j) = (la[i]==lb[j] && !_tcsncmp(ta[i], tb2[j], la[i]))
					? DP(i+1,j+1)+1 : (DP(i+1,j)>=DP(i,j+1) ? DP(i+1,j) : DP(i,j+1));
		#define EQ(i,j) (la[i]==lb[j] && !_tcsncmp(ta[i], tb2[j], la[i]))
		int i = 0, j = 0;
		while(i<na || j<nb) {
			if(i<na && j<nb && EQ(i,j)) {
				if(la[i]==1 && ta[i][0]==_T('\n')) tb_add(b, _T("\r\n"), 2);
				else { if(b->len && b->s[b->len-1]!=_T('\n')) tb_add(b, _T(" "), 1); tb_add(b, ta[i], la[i]); }
				i++; j++;
				continue;
			}
			// A run of edits up to the next common word.
			int di = i, dj = j;
			while((di<na || dj<nb) && !(di<na && dj<nb && EQ(di,dj))) {
				if(dj>=nb || (di<na && DP(di+1,dj)>=DP(di,dj+1))) di++; else dj++;
			}
			if(b->len && b->s[b->len-1]!=_T('\n')) tb_add(b, _T(" "), 1);
			tb_add_tokens(b, ta, la, i, di, _T("[-"), _T("-]"));
			if(di>i && dj>j) tb_add(b, _T(" "), 1);
			tb_add_tokens(b, tb2, lb, j, dj, _T("{+"), _T("+}"));
			i = di; j = dj;
		}
		#undef EQ
		#undef DP
		free(dp);
	}
	free((void*)ta); free(la); free((void*)tb2); free(lb);
}

static void twpng_CompareSection(struct twpng_tbuf *b, const TCHAR *title, const TCHAR *x, const TCHAR *y)
{
	tb_add(b, title, -1);
	if(!x && !y) { tb_add(b, _T(": (none in either file)\r\n\r\n"), -1); return; }
	if(x && y && !lstrcmp(x, y)) { tb_add(b, _T(": identical\r\n"), -1); }
	else {
		tb_add(b, _T(": differs  ([-only in A-]  {+only in B+})\r\n"), -1);
		twpng_DiffWords(b, x ? x : _T(""), y ? y : _T(""));
		tb_add(b, _T("\r\n"), -1);
	}
	tb_add(b, _T("\r\n"), -1);
}

static TCHAR *twpng_BuildCompareReport(const TCHAR *fa, const struct aiparams_ctx *a,
	const TCHAR *fb, const struct aiparams_ctx *c)
{
	struct twpng_tbuf b = { NULL, 0, 0 };
	tb_add(&b, _T("A: "), -1); tb_add(&b, fa, -1); tb_add(&b, a->full ? _T("") : _T("  (no AI parameters)"), -1);
	tb_add(&b, _T("\r\nB: "), -1); tb_add(&b, fb, -1); tb_add(&b, c->full ? _T("") : _T("  (no AI parameters)"), -1);
	tb_add(&b, _T("\r\n\r\nSETTINGS\r\n"), -1);

	struct twpng_kv *ka = (struct twpng_kv*)calloc(200, sizeof(struct twpng_kv));
	struct twpng_kv *kb = (struct twpng_kv*)calloc(200, sizeof(struct twpng_kv));
	int na = ka ? twpng_SplitSettings(a->setRaw, ka, 200) : 0;
	int nb = kb ? twpng_SplitSettings(c->setRaw, kb, 200) : 0;
	struct twpng_tbuf same = { NULL, 0, 0 }, onlyA = { NULL, 0, 0 }, onlyB = { NULL, 0, 0 };
	BOOL anyChange = FALSE;
	for(int i=0; i<na; i++) {
		int j;
		for(j=0; j<nb && lstrcmp(ka[i].key, kb[j].key); j++) {}
		if(j==nb) {
			tb_add(&onlyA, _T("    "), -1); tb_add(&onlyA, ka[i].key, -1); tb_add(&onlyA, _T(": "), -1);
			tb_add(&onlyA, ka[i].val, -1); tb_add(&onlyA, _T("\r\n"), -1);
		} else if(!lstrcmp(ka[i].val, kb[j].val)) {
			if(same.len) tb_add(&same, _T(", "), -1);
			tb_add(&same, ka[i].key, -1);
		} else {
			if(!anyChange) tb_add(&b, _T("  Changed:\r\n"), -1);
			anyChange = TRUE;
			tb_add(&b, _T("    "), -1); tb_add(&b, ka[i].key, -1); tb_add(&b, _T(": "), -1);
			tb_add(&b, ka[i].val, -1); tb_add(&b, _T("  ->  "), -1); tb_add(&b, kb[j].val, -1);
			tb_add(&b, _T("\r\n"), -1);
		}
	}
	for(int j=0; j<nb; j++) {
		int i;
		for(i=0; i<na && lstrcmp(ka[i].key, kb[j].key); i++) {}
		if(i==na) {
			tb_add(&onlyB, _T("    "), -1); tb_add(&onlyB, kb[j].key, -1); tb_add(&onlyB, _T(": "), -1);
			tb_add(&onlyB, kb[j].val, -1); tb_add(&onlyB, _T("\r\n"), -1);
		}
	}
	if(onlyA.len) { tb_add(&b, _T("  Only in A:\r\n"), -1); tb_add(&b, onlyA.s, -1); }
	if(onlyB.len) { tb_add(&b, _T("  Only in B:\r\n"), -1); tb_add(&b, onlyB.s, -1); }
	if(same.len)  { tb_add(&b, _T("  Same: "), -1); tb_add(&b, same.s, -1); tb_add(&b, _T("\r\n"), -1); }
	if(!na && !nb) tb_add(&b, _T("  (none)\r\n"), -1);
	tb_add(&b, _T("\r\n"), -1);
	for(int i=0; i<na; i++) free(ka[i].val);
	for(int j=0; j<nb; j++) free(kb[j].val);
	free(ka); free(kb); free(same.s); free(onlyA.s); free(onlyB.s);

	twpng_CompareSection(&b, _T("PROMPT"), a->pos, c->pos);
	twpng_CompareSection(&b, _T("NEGATIVE PROMPT"), a->neg, c->neg);
	return b.s;
}

static void twpng_LayoutCompare(HWND hwnd)
{
	RECT rc;
	GetClientRect(hwnd, &rc);
	int m = 10, btnW = 72, btnH = 24, gap = 8;
	int by = rc.bottom - m - btnH;
	SetWindowPos(GetDlgItem(hwnd,IDC_CMP_TEXT), NULL, m, m, rc.right-2*m, by-gap-m, SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDOK), NULL, rc.right-m-btnW, by, btnW, btnH, SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_CMP_COPY), NULL, rc.right-m-2*btnW-gap, by, btnW, btnH, SWP_NOZORDER);
}

static INT_PTR CALLBACK DlgProcCompare(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch(msg) {
	case WM_INITDIALOG:
		SetDlgItemText(hwnd, IDC_CMP_TEXT, (const TCHAR*)lParam);
		twpng_LayoutCompare(hwnd);
		twpng_InitDarkDialog(hwnd);
		return TRUE;
	case WM_SIZE:
		twpng_LayoutCompare(hwnd);
		return TRUE;
	case WM_GETMINMAXINFO:
		((LPMINMAXINFO)lParam)->ptMinTrackSize.x = 360;
		((LPMINMAXINFO)lParam)->ptMinTrackSize.y = 240;
		return TRUE;
	case WM_COMMAND:
		switch(LOWORD(wParam)) {
		case IDC_CMP_COPY:
			{
				int n = GetWindowTextLength(GetDlgItem(hwnd,IDC_CMP_TEXT));
				TCHAR *t = (TCHAR*)malloc(((size_t)n+1)*sizeof(TCHAR));
				if(t) { GetDlgItemText(hwnd, IDC_CMP_TEXT, t, n+1); twpng_SetClipboardText(hwnd, t); free(t); }
			}
			return TRUE;
		case IDOK: case IDCANCEL:
			EndDialog(hwnd, 0);
			return TRUE;
		}
		break;
	}
	INT_PTR dr = 0;
	if(twpng_HandleDlgDarkMsg(msg, wParam, lParam, &dr)) return dr;
	return FALSE;
}

static BOOL twpng_PickAIFile(HWND owner, const TCHAR *title, TCHAR *fn, BOOL multi, DWORD *outOffset)
{
	OPENFILENAME ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = owner;
	ofn.lpstrFilter = _T("Images and videos\0*.png;*.jpg;*.jpeg;*.webp;*.mp4;*.mov;*.m4v\0All files (*.*)\0*.*\0\0");
	ofn.lpstrFile = fn;
	ofn.nMaxFile = 4*MAX_PATH;
	ofn.lpstrTitle = title;
	ofn.lpstrInitialDir = globals.last_open_dir[0] ? globals.last_open_dir : NULL;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER |
		(multi ? OFN_ALLOWMULTISELECT : 0);
	if(!GetOpenFileName(&ofn)) return FALSE;
	if(outOffset) *outOffset = ofn.nFileOffset;
	return TRUE;
}

// Tools > Compare AI Parameters: pick two files (or one, to compare with the open document).
static void twpng_CompareAIParams(HWND owner)
{
	TCHAR sel[4*MAX_PATH] = _T(""), fa[MAX_PATH] = _T(""), fb[MAX_PATH] = _T("");
	DWORD off = 0;
	const TCHAR *doc = twpng_DocPath();
	if(!twpng_PickAIFile(owner, doc[0] ? _T("Compare with the open file (or select two files)")
	                                     : _T("Select two files to compare"), sel, TRUE, &off)) return;
	if(off>0 && sel[off-1]==0) {
		// Multi-select: "dir\0name1\0name2\0\0".
		const TCHAR *n1 = sel+off, *n2 = n1 + lstrlen(n1) + 1;
		StringCchPrintf(fa, MAX_PATH, _T("%s\\%s"), sel, n1);
		if(*n2) StringCchPrintf(fb, MAX_PATH, _T("%s\\%s"), sel, n2);
	} else {
		StringCchCopy(fa, MAX_PATH, sel);
	}
	if(!fb[0]) {
		if(doc[0] && lstrcmpi(doc, fa)) {
			StringCchCopy(fb, MAX_PATH, fa);
			StringCchCopy(fa, MAX_PATH, doc);
		} else {
			TCHAR sel2[4*MAX_PATH] = _T("");
			if(!twpng_PickAIFile(owner, _T("Select the second file"), sel2, FALSE, NULL)) return;
			StringCchCopy(fb, MAX_PATH, sel2);
		}
	}

	struct aiparams_ctx *ca = NULL, *cb = NULL;
	TCHAR *ta = NULL, *tb = NULL, ea[200], eb[200];
	twpng_ReadAnyAIParams(fa, &ta, ea, 200);
	twpng_ReadAnyAIParams(fb, &tb, eb, 200);
	ca = twpng_BuildAICtx(ta, ea);
	cb = twpng_BuildAICtx(tb, eb);
	free(ta); free(tb);
	if(ca && cb) {
		TCHAR *report = twpng_BuildCompareReport(fa, ca, fb, cb);
		if(report) {
			DialogBoxParam(globals.hInst, _T("DLG_AICOMPARE"), owner, DlgProcCompare, (LPARAM)report);
			free(report);
		}
	}
	twpng_FreeAIParamsCtx(ca);
	twpng_FreeAIParamsCtx(cb);
}

// ---- Folder overview: search and CSV export over a whole folder ----------------------
// A background thread reads only the metadata bytes of every file (folders can hold
// 20k+ files on network drives) and keeps an index in %LOCALAPPDATA%\TweakPNG\index, so
// later scans only read new or changed files.

static TCHAR *twpng_BytesToTchar(const char *s, int len, UINT cp)
{
	if(len<=0) return NULL;
#ifdef UNICODE
	int n = MultiByteToWideChar(cp, 0, s, len, NULL, 0);
	if(n<=0) return NULL;
	TCHAR *out = (TCHAR*)malloc(((size_t)n+1)*sizeof(TCHAR));
	if(!out) return NULL;
	MultiByteToWideChar(cp, 0, s, len, out, n);
	out[n] = 0;
	return out;
#else
	(void)cp;
	TCHAR *out = (TCHAR*)malloc((size_t)len+1);
	if(!out) return NULL;
	memcpy(out, s, len);
	out[len] = 0;
	return out;
#endif
}

#ifdef TWPNG_HAVE_ZLIB
static char *twpng_Inflate(const unsigned char *in, DWORD inLen, DWORD *outLen)
{
	z_stream z;
	ZeroMemory(&z, sizeof(z));
	if(inflateInit(&z)!=Z_OK) return NULL;
	DWORD cap = inLen*4 + 1024;
	char *out = (char*)malloc(cap);
	z.next_in = (Bytef*)in;
	z.avail_in = inLen;
	int err = Z_OK;
	while(out && err==Z_OK) {
		if(cap - z.total_out < 4096) {
			if(cap > 64UL*1024*1024) break;
			char *no = (char*)realloc(out, (size_t)cap*2);
			if(!no) { free(out); out = NULL; break; }
			out = no; cap *= 2;
		}
		z.next_out = (Bytef*)out + z.total_out;
		z.avail_out = cap - (DWORD)z.total_out;
		err = inflate(&z, Z_NO_FLUSH);
	}
	DWORD n = (DWORD)z.total_out;
	inflateEnd(&z);
	if(!out || err!=Z_STREAM_END) { free(out); return NULL; }
	*outLen = n;
	return out;
}
#endif

// "parameters" (preferred) or "prompt" text of a PNG; the image data is skipped by seeking.
static int twpng_ScanPngAIText(HANDLE fh, TCHAR **outText)
{
	unsigned char sig[8];
	DWORD got;
	*outText = NULL;
	if(!ReadFile(fh, sig, 8, &got, NULL) || got!=8 || memcmp(sig, "\x89PNG\r\n\x1a\n", 8)) return 0;
	TCHAR *prompt = NULL;
	for(;;) {
		unsigned char ch[8];
		if(!ReadFile(fh, ch, 8, &got, NULL) || got!=8) break;
		DWORD len = mp4_be32(ch);
		if(!memcmp(ch+4, "IEND", 4)) break;
		BOOL isT = !memcmp(ch+4,"tEXt",4), isI = !memcmp(ch+4,"iTXt",4), isZ = !memcmp(ch+4,"zTXt",4);
		if(!(isT || isI || isZ) || len > 32UL*1024*1024) {
			LARGE_INTEGER li;
			li.QuadPart = (LONGLONG)len + 4;   // data + CRC
			if(!SetFilePointerEx(fh, li, NULL, FILE_CURRENT)) break;
			continue;
		}
		unsigned char *d = (unsigned char*)malloc((size_t)len+4);
		if(!d) break;
		if(!ReadFile(fh, d, len+4, &got, NULL) || got!=len+4) { free(d); break; }
		DWORD klen = 0;
		while(klen<len && d[klen]) klen++;
		BOOL isParams = (klen==10 && !memcmp(d,"parameters",10));
		BOOL isPrompt = (klen==6 && !memcmp(d,"prompt",6));
		TCHAR *t = NULL;
		if((isParams || isPrompt) && klen<len) {
			const unsigned char *body = d+klen+1;
			DWORD blen = len-klen-1;
			if(isT) {
				t = twpng_BytesToTchar((const char*)body, (int)blen, 28591);   // Latin-1
			}
#ifdef TWPNG_HAVE_ZLIB
			else if(isZ) {
				DWORD ol;
				char *u = blen>1 ? twpng_Inflate(body+1, blen-1, &ol) : NULL;
				if(u) { t = twpng_BytesToTchar(u, (int)ol, 28591); free(u); }
			}
#endif
			else if(isI && blen>=2) {
				// compression flag, method, language tag\0, translated keyword\0, text
				BYTE comp = body[0];
				DWORD p = 2;
				while(p<blen && body[p]) p++;
				p++;
				while(p<blen && body[p]) p++;
				p++;
				if(p<=blen) {
					if(!comp) t = twpng_BytesToTchar((const char*)body+p, (int)(blen-p), CP_UTF8);
#ifdef TWPNG_HAVE_ZLIB
					else {
						DWORD ol;
						char *u = twpng_Inflate(body+p, blen-p, &ol);
						if(u) { t = twpng_BytesToTchar(u, (int)ol, CP_UTF8); free(u); }
					}
#endif
				}
			}
		}
		free(d);
		if(t && isParams) { free(prompt); *outText = t; return 1; }
		if(t && isPrompt && !prompt) prompt = t;
		else free(t);
	}
	*outText = prompt;
	return prompt!=NULL;
}

// AI text of one file for the folder scan. No UI, so it is safe on the worker thread.
static int twpng_ScanFileAI(const TCHAR *fn, TCHAR **outText, TCHAR *extra, int cap)
{
	*outText = NULL;
	extra[0] = 0;
	HANDLE fh = CreateFile(fn, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if(fh==INVALID_HANDLE_VALUE) return 0;
	unsigned char hdr[16];
	DWORD got = 0;
	ReadFile(fh, hdr, sizeof(hdr), &got, NULL);
	int kind = twpng_SniffForeignKind(hdr, got);
	LARGE_INTEGER zero;
	zero.QuadPart = 0;
	SetFilePointerEx(fh, zero, NULL, FILE_BEGIN);
	if(got>=8 && !memcmp(hdr, "\x89PNG\r\n\x1a\n", 8)) {
		twpng_ScanPngAIText(fh, outText);
	}
	else if(kind==1) {
		// The EXIF segment sits at the start of a JPEG.
		LARGE_INTEGER sz;
		DWORD n = 0;
		if(GetFileSizeEx(fh, &sz)) n = sz.QuadPart > 2*1024*1024 ? 2*1024*1024 : (DWORD)sz.QuadPart;
		unsigned char *buf = (unsigned char*)malloc(n ? n : 1);
		if(buf && ReadFile(fh, buf, n, &got, NULL)) {
			const unsigned char *tiff;
			DWORD tl;
			if(twpng_FindJpegExif(buf, got, &tiff, &tl)) twpng_ExtractFromExifTiff(tiff, tl, outText);
		}
		free(buf);
	}
	else if(kind==2) {
		// WebP keeps EXIF after the image data: walk the chunk headers.
		LARGE_INTEGER li;
		li.QuadPart = 12;
		if(SetFilePointerEx(fh, li, NULL, FILE_BEGIN)) for(;;) {
			unsigned char ch[8];
			if(!ReadFile(fh, ch, 8, &got, NULL) || got!=8) break;
			DWORD cs = le32(ch+4);
			if(!memcmp(ch, "EXIF", 4)) {
				if(cs > 16UL*1024*1024) break;
				unsigned char *b = (unsigned char*)malloc(cs ? cs : 1);
				if(b && ReadFile(fh, b, cs, &got, NULL) && got==cs) twpng_ExtractFromExifTiff(b, cs, outText);
				free(b);
				break;
			}
			li.QuadPart = (LONGLONG)cs + (cs & 1);
			if(!SetFilePointerEx(fh, li, NULL, FILE_CURRENT)) break;
		}
	}
	CloseHandle(fh);
	if(kind==3 || kind==4) {
		twpng_ReadMp4AIParams(fn, outText);
		twpng_ReadMp4VideoInfo(fn, extra, cap);
	}
	return *outText!=NULL;
}

struct twpng_airec {
	TCHAR *name;              // file name without the folder
	ULONGLONG size, mtime;
	BOOL hasAI;
	TCHAR *pos, *neg, *set;   // parsed like the viewer does (set includes video info)
};

static void twpng_FreeRec(struct twpng_airec *r)
{
	free(r->name); free(r->pos); free(r->neg); free(r->set);
	ZeroMemory(r, sizeof(*r));
}

// ---- index file ----

struct twpng_bbuf { unsigned char *p; size_t len, cap; BOOL bad; };

static void bb_add(struct twpng_bbuf *b, const void *d, size_t n)
{
	if(b->bad) return;
	if(b->len + n > b->cap) {
		size_t nc = b->cap ? b->cap*2 : 65536;
		while(nc < b->len + n) nc *= 2;
		unsigned char *np = (unsigned char*)realloc(b->p, nc);
		if(!np) { b->bad = TRUE; return; }
		b->p = np; b->cap = nc;
	}
	memcpy(b->p + b->len, d, n);
	b->len += n;
}

static void bb_u32(struct twpng_bbuf *b, DWORD v) { bb_add(b, &v, 4); }
static void bb_u64(struct twpng_bbuf *b, ULONGLONG v) { bb_add(b, &v, 8); }

// UTF-8 string with a length prefix; 0xFFFFFFFF stands for NULL.
static void bb_str(struct twpng_bbuf *b, const TCHAR *s)
{
	if(!s) { bb_u32(b, 0xFFFFFFFF); return; }
#ifdef UNICODE
	int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
	if(n<=0) { bb_u32(b, 0); return; }
	char *u = (char*)malloc(n);
	if(!u) { b->bad = TRUE; return; }
	WideCharToMultiByte(CP_UTF8, 0, s, -1, u, n, NULL, NULL);
	bb_u32(b, (DWORD)n-1);
	bb_add(b, u, (size_t)n-1);
	free(u);
#else
	DWORD n = (DWORD)lstrlen(s);
	bb_u32(b, n);
	bb_add(b, s, n);
#endif
}

struct twpng_rd { const unsigned char *p, *end; BOOL bad; };

static DWORD rd_u32(struct twpng_rd *r)
{
	if(r->end - r->p < 4) { r->bad = TRUE; return 0; }
	DWORD v; memcpy(&v, r->p, 4); r->p += 4;
	return v;
}

static ULONGLONG rd_u64(struct twpng_rd *r)
{
	if(r->end - r->p < 8) { r->bad = TRUE; return 0; }
	ULONGLONG v; memcpy(&v, r->p, 8); r->p += 8;
	return v;
}

static TCHAR *rd_str(struct twpng_rd *r)
{
	DWORD n = rd_u32(r);
	if(r->bad || n==0xFFFFFFFF) return NULL;
	if((DWORD)(r->end - r->p) < n) { r->bad = TRUE; return NULL; }
	TCHAR *s = n ? twpng_BytesToTchar((const char*)r->p, (int)n, CP_UTF8) : _tcsdup(_T(""));
	r->p += n;
	return s;
}

static BOOL twpng_IndexPath(const TCHAR *dir, TCHAR *out, int cap, BOOL create)
{
	TCHAR base[MAX_PATH];
	if(FAILED(SHGetFolderPath(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) return FALSE;
	TCHAR folder[MAX_PATH];
	StringCchPrintf(folder, MAX_PATH, _T("%s\\TweakPNG\\index"), base);
	if(create) SHCreateDirectoryEx(NULL, folder, NULL);
	// FNV-1a of the lower-cased folder path names the index file.
	ULONGLONG h = 1469598103934665603ULL;
	for(const TCHAR *p=dir; *p; p++) {
		TCHAR c = *p;
		if(c>=_T('A') && c<=_T('Z')) c = (TCHAR)(c - _T('A') + _T('a'));
		h ^= (ULONGLONG)c;
		h *= 1099511628211ULL;
	}
	StringCchPrintf(out, cap, _T("%s\\%016I64x.idx"), folder, h);
	return TRUE;
}

#define TWPNG_INDEX_MAGIC 0x49415754   // "TWAI"
#define TWPNG_INDEX_VER   1

static int twpng_CmpRecName(const void *a, const void *b)
{
	return lstrcmpi(((const struct twpng_airec*)a)->name, ((const struct twpng_airec*)b)->name);
}

// Old index records for `dir`, sorted by name for lookup. Returns the count.
static int twpng_LoadIndex(const TCHAR *dir, struct twpng_airec **out)
{
	*out = NULL;
	TCHAR fn[MAX_PATH];
	unsigned char *buf;
	DWORD size;
	if(!twpng_IndexPath(dir, fn, MAX_PATH, FALSE) || !twpng_SlurpFile(fn, 1024UL*1024*1024, &buf, &size)) return 0;
	struct twpng_rd r = { buf, buf+size, FALSE };
	int n = 0;
	struct twpng_airec *recs = NULL;
	if(rd_u32(&r)==TWPNG_INDEX_MAGIC && rd_u32(&r)==TWPNG_INDEX_VER) {
		TCHAR *d = rd_str(&r);
		BOOL ok = d && !lstrcmpi(d, dir);
		free(d);
		DWORD cnt = ok ? rd_u32(&r) : 0;
		if(ok && !r.bad && cnt < 10000000) recs = (struct twpng_airec*)calloc(cnt ? cnt : 1, sizeof(struct twpng_airec));
		for(DWORD i=0; recs && i<cnt && !r.bad; i++) {
			struct twpng_airec *x = &recs[n];
			x->name = rd_str(&r);
			x->size = rd_u64(&r);
			x->mtime = rd_u64(&r);
			x->hasAI = rd_u32(&r)!=0;
			x->pos = rd_str(&r);
			x->neg = rd_str(&r);
			x->set = rd_str(&r);
			if(r.bad || !x->name) { twpng_FreeRec(x); break; }
			n++;
		}
	}
	free(buf);
	if(n) qsort(recs, n, sizeof(struct twpng_airec), twpng_CmpRecName);
	*out = recs;
	return n;
}

static void twpng_SaveIndex(const TCHAR *dir, const struct twpng_airec *recs, int n)
{
	TCHAR fn[MAX_PATH], tmp[MAX_PATH];
	if(!twpng_IndexPath(dir, fn, MAX_PATH, TRUE)) return;
	struct twpng_bbuf b = { NULL, 0, 0, FALSE };
	bb_u32(&b, TWPNG_INDEX_MAGIC);
	bb_u32(&b, TWPNG_INDEX_VER);
	bb_str(&b, dir);
	int cnt = 0;
	for(int i=0; i<n; i++) if(recs[i].name) cnt++;
	bb_u32(&b, (DWORD)cnt);
	for(int i=0; i<n; i++) {
		if(!recs[i].name) continue;
		bb_str(&b, recs[i].name);
		bb_u64(&b, recs[i].size);
		bb_u64(&b, recs[i].mtime);
		bb_u32(&b, recs[i].hasAI ? 1 : 0);
		bb_str(&b, recs[i].pos);
		bb_str(&b, recs[i].neg);
		bb_str(&b, recs[i].set);
	}
	if(!b.bad && b.len < 0xFFFFFFFF) {
		StringCchPrintf(tmp, MAX_PATH, _T("%s.tmp"), fn);
		if(twpng_WriteWholeFile(tmp, b.p, (DWORD)b.len) && !MoveFileEx(tmp, fn, MOVEFILE_REPLACE_EXISTING))
			DeleteFile(tmp);
	}
	free(b.p);
}

// ---- scan worker ----

#define WM_APP_SCAN (WM_APP+23)   // wParam 0: progress, 1: finished

struct twpng_scan {
	volatile LONG refs;
	volatile LONG cancel;
	HWND notify;
	TCHAR dir[MAX_PATH];           // ends in a backslash
	struct twpng_airec *recs;      // `total` entries, allocated before scanning starts
	volatile LONG total;           // -1 while the folder is being listed
	volatile LONG done;            // recs[0..done) are final and read-only
	volatile LONG read;            // files actually read this run (not from the index)
	ULONGLONG start;
};

static void twpng_ScanRelease(struct twpng_scan *s)
{
	if(InterlockedDecrement(&s->refs)) return;
	for(LONG i=0; i<s->total; i++) twpng_FreeRec(&s->recs[i]);
	free(s->recs);
	free(s);
}

struct twpng_dirent { TCHAR *name; ULONGLONG size, mtime; };

static int twpng_CmpDirent(const void *a, const void *b)
{
#ifdef UNICODE
	return StrCmpLogicalW(((const struct twpng_dirent*)a)->name, ((const struct twpng_dirent*)b)->name);
#else
	return lstrcmpi(((const struct twpng_dirent*)a)->name, ((const struct twpng_dirent*)b)->name);
#endif
}

static DWORD WINAPI twpng_ScanThread(LPVOID p)
{
	struct twpng_scan *s = (struct twpng_scan*)p;
	TCHAR pat[MAX_PATH];
	StringCchPrintf(pat, MAX_PATH, _T("%s*"), s->dir);

	struct twpng_dirent *ents = NULL;
	int n = 0, cap = 0;
	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFileEx(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	if(h!=INVALID_HANDLE_VALUE) {
		do {
			if((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !twpng_IsBrowsableFile(fd.cFileName)) continue;
			if(n==cap) {
				cap = cap ? cap*2 : 1024;
				struct twpng_dirent *ne = (struct twpng_dirent*)realloc(ents, (size_t)cap*sizeof(*ents));
				if(!ne) break;
				ents = ne;
			}
			ents[n].name = _tcsdup(fd.cFileName);
			ents[n].size = ((ULONGLONG)fd.nFileSizeHigh<<32) | fd.nFileSizeLow;
			ents[n].mtime = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime<<32) | fd.ftLastWriteTime.dwLowDateTime;
			if(ents[n].name) n++;
		} while(!s->cancel && FindNextFile(h, &fd));
		FindClose(h);
	}
	if(n) qsort(ents, n, sizeof(*ents), twpng_CmpDirent);

	struct twpng_airec *old = NULL;
	int nold = twpng_LoadIndex(s->dir, &old);

	s->recs = (struct twpng_airec*)calloc(n ? n : 1, sizeof(struct twpng_airec));
	if(!s->recs) n = 0;
	InterlockedExchange(&s->total, n);
	PostMessage(s->notify, WM_APP_SCAN, 0, (LPARAM)s);

	ULONGLONG lastPost = GetTickCount64();
	for(int i=0; i<n && !s->cancel; i++) {
		struct twpng_airec *r = &s->recs[i];
		r->name = ents[i].name;
		ents[i].name = NULL;
		r->size = ents[i].size;
		r->mtime = ents[i].mtime;
		struct twpng_airec key;
		key.name = r->name;
		struct twpng_airec *o = nold ? (struct twpng_airec*)bsearch(&key, old, nold, sizeof(*old), twpng_CmpRecName) : NULL;
		if(o && o->size==r->size && o->mtime==r->mtime) {
			r->hasAI = o->hasAI;
			r->pos = o->pos; o->pos = NULL;
			r->neg = o->neg; o->neg = NULL;
			r->set = o->set; o->set = NULL;
		} else {
			TCHAR full[MAX_PATH], *text = NULL, extra[200];
			StringCchPrintf(full, MAX_PATH, _T("%s%s"), s->dir, r->name);
			twpng_ScanFileAI(full, &text, extra, 200);
			r->hasAI = text!=NULL;
			struct aiparams_ctx *c = twpng_BuildAICtx(text, extra);
			if(c) {
				r->pos = c->pos; c->pos = NULL;
				r->neg = c->neg; c->neg = NULL;
				r->set = c->setRaw; c->setRaw = NULL;
				twpng_FreeAIParamsCtx(c);
			}
			free(text);
			InterlockedIncrement(&s->read);
		}
		InterlockedExchange(&s->done, i+1);
		ULONGLONG now = GetTickCount64();
		if(now - lastPost >= 100) {
			lastPost = now;
			PostMessage(s->notify, WM_APP_SCAN, 0, (LPARAM)s);
		}
	}

	// Keep what is known: scanned records, plus still-valid old ones for files not reached.
	int done = s->done;
	struct twpng_airec *save = (struct twpng_airec*)calloc(n ? n : 1, sizeof(struct twpng_airec));
	if(save) {
		int k = 0;
		for(int i=0; i<n; i++) {
			if(i<done) { save[k++] = s->recs[i]; continue; }
			struct twpng_airec key;
			key.name = ents[i].name;
			struct twpng_airec *o = (nold && key.name) ? (struct twpng_airec*)bsearch(&key, old, nold, sizeof(*old), twpng_CmpRecName) : NULL;
			if(o && o->size==ents[i].size && o->mtime==ents[i].mtime && o->name) save[k++] = *o;
		}
		if(k || !nold) twpng_SaveIndex(s->dir, save, k);
		free(save);   // shallow copies only
	}
	for(int i=0; i<nold; i++) twpng_FreeRec(&old[i]);
	free(old);
	for(int i=0; i<n; i++) free(ents[i].name);
	free(ents);

	PostMessage(s->notify, WM_APP_SCAN, 1, (LPARAM)s);
	twpng_ScanRelease(s);
	return 0;
}

// ---- dialog ----

struct twpng_folderdlg {
	struct twpng_scan *scan;
	TCHAR dir[MAX_PATH];
	int *rows;          // indexes into scan->recs that match the search
	int nrows, caprows;
	int filtered;       // recs[0..filtered) have been checked against the search
	TCHAR search[512];
	BOOL finished, stopped;
};

// Value of `key` in a settings line ("k: v, k: v"), outer quotes removed.
static BOOL twpng_FindSetting(const TCHAR *set, const TCHAR *key, TCHAR *out, int cap)
{
	out[0] = 0;
	if(!set) return FALSE;
	int klen = lstrlen(key);
	BOOL inq = FALSE;
	for(const TCHAR *p=set; *p; p++) {
		BOOL start = (p==set) || (!inq && p>=set+2 && p[-2]==_T(',') && p[-1]==_T(' '));
		if(start && !_tcsncmp(p, key, klen) && p[klen]==_T(':') && p[klen+1]==_T(' ')) {
			const TCHAR *v = p+klen+2, *e = v;
			BOOL q = FALSE;
			while(*e && (q || !(e[0]==_T(',') && e[1]==_T(' ')))) { if(*e==_T('"')) q = !q; e++; }
			if(e-v>=2 && *v==_T('"') && e[-1]==_T('"')) { v++; e--; }
			int n = (int)(e-v);
			if(n >= cap) n = cap-1;
			memcpy(out, v, (size_t)n*sizeof(TCHAR));
			out[n] = 0;
			return TRUE;
		}
		if(*p==_T('"')) inq = !inq;
	}
	return FALSE;
}

// Every space-separated word of the search must appear in the file name, prompts or settings.
static BOOL twpng_RecMatches(const struct twpng_airec *r, const TCHAR *search)
{
	TCHAR word[256];
	const TCHAR *p = search;
	for(;;) {
		while(*p==_T(' ')) p++;
		if(!*p) return TRUE;
		int n = 0;
		while(*p && *p!=_T(' ') && n<255) word[n++] = *p++;
		word[n] = 0;
		if(!(StrStrI(r->name, word) || (r->pos && StrStrI(r->pos, word)) ||
		     (r->neg && StrStrI(r->neg, word)) || (r->set && StrStrI(r->set, word))))
			return FALSE;
	}
}

static void twpng_FolderFilterMore(HWND hwnd, struct twpng_folderdlg *f)
{
	if(!f->scan || f->scan->total<=0) return;
	LONG done = f->scan->done;
	int before = f->nrows;
	for(; f->filtered<done; f->filtered++) {
		if(!twpng_RecMatches(&f->scan->recs[f->filtered], f->search)) continue;
		if(f->nrows==f->caprows) {
			int nc = f->caprows ? f->caprows*2 : 1024;
			int *nr = (int*)realloc(f->rows, (size_t)nc*sizeof(int));
			if(!nr) break;
			f->rows = nr; f->caprows = nc;
		}
		f->rows[f->nrows++] = f->filtered;
	}
	if(f->nrows!=before)
		ListView_SetItemCountEx(GetDlgItem(hwnd,IDC_AF_LIST), f->nrows, LVSICF_NOSCROLL|LVSICF_NOINVALIDATEALL);
}

static void twpng_FolderRefilter(HWND hwnd, struct twpng_folderdlg *f)
{
	GetDlgItemText(hwnd, IDC_AF_SEARCH, f->search, 512);
	f->nrows = 0;
	f->filtered = 0;
	ListView_SetItemCountEx(GetDlgItem(hwnd,IDC_AF_LIST), 0, 0);
	twpng_FolderFilterMore(hwnd, f);
	InvalidateRect(GetDlgItem(hwnd,IDC_AF_LIST), NULL, TRUE);
}

// A marquee while the folder is listed (the total isn't known yet), a normal bar after.
static void twpng_SetMarquee(HWND pb, BOOL on)
{
	LONG st = GetWindowLong(pb, GWL_STYLE);
	if(on != ((st & PBS_MARQUEE)!=0)) SetWindowLong(pb, GWL_STYLE, on ? (st|PBS_MARQUEE) : (st & ~PBS_MARQUEE));
	SendMessage(pb, PBM_SETMARQUEE, on, 30);
}

static void twpng_FolderStatus(HWND hwnd, struct twpng_folderdlg *f)
{
	TCHAR buf[300];
	struct twpng_scan *s = f->scan;
	LONG total = s ? s->total : 0, done = s ? s->done : 0;
	HWND pb = GetDlgItem(hwnd, IDC_AF_PROGRESS);
	if(!s || total<0) {
		StringCchCopy(buf, 300, _T("Listing the folder..."));
		twpng_SetMarquee(pb, TRUE);
	} else {
		twpng_SetMarquee(pb, FALSE);
		SendMessage(pb, PBM_SETRANGE32, 0, total ? total : 1);
		SendMessage(pb, PBM_SETPOS, f->finished ? (total ? total : 1) : done, 0);
		if(f->finished && f->stopped)
			StringCchPrintf(buf, 300, _T("Stopped at %d of %d files."), done, total);
		else if(f->finished)
			StringCchPrintf(buf, 300, _T("%d files, %d read and %d from the index."), total, s->read, done - s->read);
		else {
			StringCchPrintf(buf, 300, _T("Scanning %d of %d files"), done, total);
			ULONGLONG el = GetTickCount64() - s->start;
			if(done>20 && el>1500) {
				double left = (double)el/done*(total-done)/1000.0;
				TCHAR t[64];
				if(left < 90) StringCchPrintf(t, 64, _T(", about %d s left"), (int)left+1);
				else StringCchPrintf(t, 64, _T(", about %d min left"), (int)(left/60)+1);
				StringCchCat(buf, 300, t);
			}
			StringCchCat(buf, 300, _T("..."));
		}
	}
	SetDlgItemText(hwnd, IDC_AF_STATUS, buf);
	StringCchPrintf(buf, 300, f->search[0] ? _T("%d matches") : _T("%d files"), f->nrows);
	SetDlgItemText(hwnd, IDC_AF_COUNT, buf);
	ShowWindow(GetDlgItem(hwnd, IDC_AF_STOP), f->finished ? SW_HIDE : SW_SHOW);
}

static void twpng_FolderStartScan(HWND hwnd, struct twpng_folderdlg *f)
{
	if(f->scan) {
		InterlockedExchange(&f->scan->cancel, 1);
		twpng_ScanRelease(f->scan);
		f->scan = NULL;
	}
	f->nrows = 0;
	f->filtered = 0;
	f->finished = f->stopped = FALSE;
	ListView_SetItemCountEx(GetDlgItem(hwnd,IDC_AF_LIST), 0, 0);
	struct twpng_scan *s = (struct twpng_scan*)calloc(1, sizeof(struct twpng_scan));
	if(!s) return;
	s->refs = 2;   // dialog + worker
	s->notify = hwnd;
	s->total = -1;
	s->start = GetTickCount64();
	StringCchCopy(s->dir, MAX_PATH, f->dir);
	HANDLE th = CreateThread(NULL, 0, twpng_ScanThread, s, 0, NULL);
	if(!th) { free(s); return; }
	CloseHandle(th);
	f->scan = s;
	TCHAR title[MAX_PATH+40];
	StringCchPrintf(title, MAX_PATH+40, _T("Folder Overview - %s"), f->dir);
	SetWindowText(hwnd, title);
	SetDlgItemText(hwnd, IDC_AF_PATH, f->dir);
	twpng_FolderStatus(hwnd, f);
}

static void twpng_LayoutFolder(HWND hwnd)
{
	RECT rc;
	GetClientRect(hwnd, &rc);
	int W = rc.right, H = rc.bottom, m = 10, gap = 8, rowH = 24, lblW = 50, btnH = 24;
	int y = m;
	SetWindowPos(GetDlgItem(hwnd,IDC_AF_LBLFOLDER), NULL, m, y+4, lblW, rowH-4, SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AF_RESCAN), NULL, W-m-70, y, 70, btnH, SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AF_CHANGE), NULL, W-m-70-gap-80, y, 80, btnH, SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AF_PATH), NULL, m+lblW, y+4, W-2*m-lblW-70-80-3*gap, rowH-4, SWP_NOZORDER);
	y += rowH+gap;
	SetWindowPos(GetDlgItem(hwnd,IDC_AF_LBLSEARCH), NULL, m, y+4, lblW, rowH-4, SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AF_COUNT), NULL, W-m-130, y+4, 130, rowH-4, SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AF_SEARCH), NULL, m+lblW, y, W-2*m-lblW-130-gap, rowH, SWP_NOZORDER);
	y += rowH+gap;
	int stopW = 60, statW = (W-2*m)/2;
	SetWindowPos(GetDlgItem(hwnd,IDC_AF_PROGRESS), NULL, m, y+4, W-2*m-statW-stopW-2*gap, 16, SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AF_STATUS), NULL, W-m-stopW-gap-statW, y+4, statW, rowH-4, SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AF_STOP), NULL, W-m-stopW, y, stopW, btnH, SWP_NOZORDER);
	y += rowH+gap;
	int bottomY = H-m-btnH;
	SetWindowPos(GetDlgItem(hwnd,IDC_AF_LIST), NULL, m, y, W-2*m, bottomY-gap-y, SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDC_AF_EXPORT), NULL, m, bottomY, 100, btnH, SWP_NOZORDER);
	SetWindowPos(GetDlgItem(hwnd,IDCANCEL), NULL, W-m-70, bottomY, 70, btnH, SWP_NOZORDER);
}

static void twpng_CsvField(struct twpng_tbuf *b, const TCHAR *s, BOOL last)
{
	tb_add(b, _T("\""), 1);
	for(const TCHAR *p = s ? s : _T(""); *p; p++) {
		if(*p==_T('"')) tb_add(b, _T("\"\""), 2);
		else tb_add(b, p, 1);
	}
	tb_add(b, last ? _T("\"\r\n") : _T("\","), -1);
}

// CSV of the rows currently shown (UTF-8 with BOM, so Excel opens it correctly).
static void twpng_FolderExportCsv(HWND hwnd, struct twpng_folderdlg *f)
{
	if(!f->scan || f->nrows<=0) { mesg(MSG_I, _T("There are no rows to export.")); return; }
	TCHAR fn[MAX_PATH];
	StringCchPrintf(fn, MAX_PATH, _T("%sai_parameters.csv"), f->dir);
	OPENFILENAME ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hwnd;
	ofn.lpstrFilter = _T("CSV files (*.csv)\0*.csv\0All files (*.*)\0*.*\0\0");
	ofn.lpstrFile = fn;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrDefExt = _T("csv");
	ofn.lpstrTitle = _T("Export AI parameters to CSV");
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
	if(!GetSaveFileName(&ofn)) return;

	static const TCHAR *cols[] = { _T("Seed"), _T("Steps"), _T("Sampler"), _T("Schedule type"),
		_T("CFG scale"), _T("Model"), _T("LoRAs"), _T("Mode"), _T("References"), _T("First frame"),
		_T("Last frame"), _T("Size"), _T("Video size"), _T("Frames"), _T("Duration"), _T("FPS"),
		_T("Denoising strength"), NULL };
	struct twpng_tbuf b = { NULL, 0, 0 };
	twpng_CsvField(&b, _T("File"), FALSE);
	twpng_CsvField(&b, _T("Prompt"), FALSE);
	twpng_CsvField(&b, _T("Negative prompt"), FALSE);
	for(int k=0; cols[k]; k++) twpng_CsvField(&b, cols[k], FALSE);
	twpng_CsvField(&b, _T("Other settings"), TRUE);
	struct twpng_kv *kv = (struct twpng_kv*)calloc(200, sizeof(struct twpng_kv));
	for(int i=0; i<f->nrows && kv; i++) {
		const struct twpng_airec *r = &f->scan->recs[f->rows[i]];
		twpng_CsvField(&b, r->name, FALSE);
		twpng_CsvField(&b, r->pos, FALSE);
		twpng_CsvField(&b, r->neg, FALSE);
		int n = twpng_SplitSettings(r->set, kv, 200);
		BOOL *used = (BOOL*)calloc(n ? n : 1, sizeof(BOOL));
		for(int k=0; cols[k]; k++) {
			const TCHAR *val = NULL;
			for(int j=0; j<n && !val; j++) {
				if(!lstrcmp(kv[j].key, cols[k]) || (k==6 && !lstrcmp(kv[j].key, _T("LoRA")))) {
					val = kv[j].val;
					if(used) used[j] = TRUE;
				}
			}
			// Lists are quoted in the settings line; the CSV cell doesn't need that.
			TCHAR tmp[2048] = _T("");
			if(val) {
				StringCchCopy(tmp, 2048, val);
				int l = lstrlen(tmp);
				if(l>=2 && tmp[0]==_T('"') && tmp[l-1]==_T('"')) { memmove(tmp, tmp+1, (l-2)*sizeof(TCHAR)); tmp[l-2] = 0; }
			}
			twpng_CsvField(&b, tmp, FALSE);
		}
		struct twpng_tbuf other = { NULL, 0, 0 };
		for(int j=0; j<n; j++) {
			if(used && used[j]) continue;
			if(other.len) tb_add(&other, _T("; "), 2);
			tb_add(&other, kv[j].key, -1);
			tb_add(&other, _T(": "), 2);
			tb_add(&other, kv[j].val, -1);
		}
		twpng_CsvField(&b, other.s, TRUE);
		free(other.s);
		free(used);
		for(int j=0; j<n; j++) free(kv[j].val);
	}
	free(kv);

	BOOL ok = FALSE;
#ifdef UNICODE
	int n = b.s ? WideCharToMultiByte(CP_UTF8, 0, b.s, (int)b.len, NULL, 0, NULL, NULL) : 0;
	unsigned char *u = (unsigned char*)malloc((size_t)n+3);
	if(u) {
		u[0] = 0xEF; u[1] = 0xBB; u[2] = 0xBF;
		WideCharToMultiByte(CP_UTF8, 0, b.s, (int)b.len, (char*)u+3, n, NULL, NULL);
		ok = twpng_WriteWholeFile(fn, u, (DWORD)n+3);
		free(u);
	}
#else
	ok = twpng_WriteWholeFile(fn, (const unsigned char*)b.s, (DWORD)b.len);
#endif
	free(b.s);
	if(!ok) mesg(MSG_E, _T("Could not write %s"), fn);
	else mesg(MSG_I, _T("Exported %d rows to\r\n%s"), f->nrows, fn);
}

static BOOL twpng_PickFolder(HWND owner, TCHAR *dir)
{
	IFileOpenDialog *d = NULL;
	BOOL ok = FALSE;
	if(FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return FALSE;
	DWORD opt = 0;
	d->GetOptions(&opt);
	d->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
	d->SetTitle(L"Choose a folder");
	if(SUCCEEDED(d->Show(owner))) {
		IShellItem *it = NULL;
		if(SUCCEEDED(d->GetResult(&it))) {
			PWSTR path = NULL;
			if(SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
				StringCchCopy(dir, MAX_PATH, path);
				int l = lstrlen(dir);
				if(l && dir[l-1]!=_T('\\')) StringCchCat(dir, MAX_PATH, _T("\\"));
				CoTaskMemFree(path);
				ok = TRUE;
			}
			it->Release();
		}
	}
	d->Release();
	return ok;
}

static void twpng_OpenInViewer(const TCHAR *path);

static INT_PTR CALLBACK DlgProcAIFolder(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	struct twpng_folderdlg *f = (struct twpng_folderdlg*)GetWindowLongPtr(hwnd, DWLP_USER);
	switch(msg) {
	case WM_INITDIALOG:
		{
			f = (struct twpng_folderdlg*)lParam;
			SetWindowLongPtr(hwnd, DWLP_USER, lParam);
			SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadIcon(globals.hInst,_T("ICONMAIN")));
			HWND lv = GetDlgItem(hwnd, IDC_AF_LIST);
			ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_HEADERDRAGDROP);
			static const struct { const TCHAR *name; int w; } cols[] = {
				{ _T("File"), 220 }, { _T("Seed"), 100 }, { _T("Model"), 220 }, { _T("Mode"), 130 },
				{ _T("Duration"), 70 }, { _T("Prompt"), 700 }, { NULL, 0 } };
			for(int i=0; cols[i].name; i++) {
				LVCOLUMN c;
				ZeroMemory(&c, sizeof(c));
				c.mask = LVCF_TEXT|LVCF_WIDTH;
				c.pszText = (LPTSTR)cols[i].name;
				c.cx = cols[i].w;
				ListView_InsertColumn(lv, i, &c);
			}
			SetWindowSubclass(lv, MainListSubclassProc, 1, 0);
			twpng_InitDarkDialog(hwnd);
			if(globals.window_prefs.aifolder.w>0 && globals.window_prefs.aifolder.h>0)
				twpng_SetWindowPos(hwnd, &globals.window_prefs.aifolder);
			twpng_LayoutFolder(hwnd);
			twpng_FolderStartScan(hwnd, f);
			SetFocus(GetDlgItem(hwnd, IDC_AF_SEARCH));
			return FALSE;
		}
	case WM_SIZE:
		twpng_LayoutFolder(hwnd);
		return TRUE;
	case WM_GETMINMAXINFO:
		((LPMINMAXINFO)lParam)->ptMinTrackSize.x = 520;
		((LPMINMAXINFO)lParam)->ptMinTrackSize.y = 300;
		return TRUE;
	case WM_EXITSIZEMOVE:
	case WM_DESTROY:
		twpng_StoreWindowPos(hwnd, &globals.window_prefs.aifolder);
		break;
	case WM_APP_SCAN:
		if(f && (struct twpng_scan*)lParam==f->scan) {
			if(wParam==1) { f->finished = TRUE; f->stopped = f->scan->cancel!=0; }
			twpng_FolderFilterMore(hwnd, f);
			twpng_FolderStatus(hwnd, f);
		}
		return TRUE;
	case WM_TIMER:
		if(wParam==1 && f) {
			KillTimer(hwnd, 1);
			twpng_FolderRefilter(hwnd, f);
			twpng_FolderStatus(hwnd, f);
		}
		return TRUE;
	case WM_NOTIFY:
		{
			LPNMHDR nh = (LPNMHDR)lParam;
			if(!f || nh->idFrom!=IDC_AF_LIST) break;
			if(nh->code==LVN_GETDISPINFO) {
				NMLVDISPINFO *di = (NMLVDISPINFO*)lParam;
				if(!(di->item.mask & LVIF_TEXT) || di->item.iItem<0 || di->item.iItem>=f->nrows) return TRUE;
				const struct twpng_airec *r = &f->scan->recs[f->rows[di->item.iItem]];
				TCHAR *out = di->item.pszText;
				int cap = di->item.cchTextMax;
				out[0] = 0;
				switch(di->item.iSubItem) {
				case 0: StringCchCopy(out, cap, r->name); break;
				case 1: twpng_FindSetting(r->set, _T("Seed"), out, cap); break;
				case 2: twpng_FindSetting(r->set, _T("Model"), out, cap); break;
				case 3: twpng_FindSetting(r->set, _T("Mode"), out, cap); break;
				case 4: twpng_FindSetting(r->set, _T("Duration"), out, cap); break;
				case 5:
					if(!r->hasAI) StringCchCopy(out, cap, _T("(no AI parameters)"));
					else if(r->pos) {
						// First 300 characters on one line.
						int k = 0;
						for(const TCHAR *p=r->pos; *p && k<cap-1 && k<300; p++)
							out[k++] = (*p==_T('\r') || *p==_T('\n') || *p==_T('\t')) ? _T(' ') : *p;
						out[k] = 0;
					}
					break;
				}
				return TRUE;
			}
			if(nh->code==LVN_ITEMACTIVATE) {
				int i = ((LPNMITEMACTIVATE)lParam)->iItem;
				if(i>=0 && i<f->nrows) {
					TCHAR path[MAX_PATH];
					StringCchPrintf(path, MAX_PATH, _T("%s%s"), f->dir, f->scan->recs[f->rows[i]].name);
					twpng_OpenInViewer(path);
				}
				return TRUE;
			}
		}
		break;
	case WM_COMMAND:
		if(!f) break;
		switch(LOWORD(wParam)) {
		case IDC_AF_SEARCH:
			if(HIWORD(wParam)==EN_CHANGE) SetTimer(hwnd, 1, 250, NULL);
			return TRUE;
		case IDOK:   // Enter in the search box
			KillTimer(hwnd, 1);
			twpng_FolderRefilter(hwnd, f);
			twpng_FolderStatus(hwnd, f);
			return TRUE;
		case IDC_AF_STOP:
			if(f->scan) InterlockedExchange(&f->scan->cancel, 1);
			return TRUE;
		case IDC_AF_RESCAN:
			twpng_FolderStartScan(hwnd, f);
			return TRUE;
		case IDC_AF_CHANGE:
			{
				TCHAR dir[MAX_PATH];
				if(twpng_PickFolder(hwnd, dir)) {
					StringCchCopy(f->dir, MAX_PATH, dir);
					twpng_FolderStartScan(hwnd, f);
				}
			}
			return TRUE;
		case IDC_AF_EXPORT:
			twpng_FolderExportCsv(hwnd, f);
			return TRUE;
		case IDCANCEL:
			DestroyWindow(hwnd);
			return TRUE;
		}
		break;
	case WM_NCDESTROY:
		if(f) {
			if(f->scan) {
				InterlockedExchange(&f->scan->cancel, 1);
				twpng_ScanRelease(f->scan);
			}
			free(f->rows);
			free(f);
		}
		SetWindowLongPtr(hwnd, DWLP_USER, 0);
		if(g_hwndAIFolder==hwnd) g_hwndAIFolder = NULL;
		return TRUE;
	}
	INT_PTR dr = 0;
	if(twpng_HandleDlgDarkMsg(msg, wParam, lParam, &dr)) return dr;
	return FALSE;
}

// Tools > Folder Overview (also Ctrl+F in the viewer). `dir` ends in a backslash; NULL or
// empty asks for a folder.
static void twpng_ShowFolderOverview(const TCHAR *dir, BOOL focusSearch)
{
	TCHAR d[MAX_PATH] = _T("");
	if(dir && dir[0]) StringCchCopy(d, MAX_PATH, dir);
	else if(!twpng_PickFolder(globals.hwndMain, d)) return;
	if(g_hwndAIFolder) {
		struct twpng_folderdlg *f = (struct twpng_folderdlg*)GetWindowLongPtr(g_hwndAIFolder, DWLP_USER);
		if(f && lstrcmpi(f->dir, d)) {
			StringCchCopy(f->dir, MAX_PATH, d);
			twpng_FolderStartScan(g_hwndAIFolder, f);
		}
	} else {
		struct twpng_folderdlg *f = (struct twpng_folderdlg*)calloc(1, sizeof(struct twpng_folderdlg));
		if(!f) return;
		StringCchCopy(f->dir, MAX_PATH, d);
		g_hwndAIFolder = CreateDialogParam(globals.hInst, _T("DLG_AIFOLDER"), NULL, DlgProcAIFolder, (LPARAM)f);
		if(!g_hwndAIFolder) { free(f); return; }
		ShowWindow(g_hwndAIFolder, SW_SHOW);
	}
	if(IsIconic(g_hwndAIFolder)) ShowWindow(g_hwndAIFolder, SW_RESTORE);
	SetForegroundWindow(g_hwndAIFolder);
	if(focusSearch) {
		HWND e = GetDlgItem(g_hwndAIFolder, IDC_AF_SEARCH);
		SetFocus(e);
		SendMessage(e, EM_SETSEL, 0, -1);
	}
}

// Folder of the current document, ending in a backslash, or "".
static void twpng_DocFolder(TCHAR *out)
{
	out[0] = 0;
	const TCHAR *p = g_aiParamsCtx && g_aiParamsCtx->path[0] ? g_aiParamsCtx->path : twpng_DocPath();
	if(!p[0]) p = globals.last_open_dir;
	StringCchCopy(out, MAX_PATH, p);
	TCHAR *sl = _tcsrchr(out, _T('\\'));
	if(sl) sl[1] = 0; else out[0] = 0;
}

// File-open command: show the parsed viewer for a file without loading it as the document.
static void twpng_ReadAIFromFile(HWND owner)
{
	OPENFILENAME ofn;
	TCHAR fn[MAX_PATH];
	StringCchCopy(fn, MAX_PATH, _T(""));
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner   = owner;
	ofn.lpstrFilter = _T("Images and videos\0*.png;*.jpg;*.jpeg;*.webp;*.mp4;*.mov;*.m4v\0All files (*.*)\0*.*\0\0");
	ofn.nFilterIndex= 1;
	ofn.lpstrFile   = fn;
	ofn.nMaxFile    = MAX_PATH;
	ofn.lpstrTitle  = _T("Read AI parameters from file");
	ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
	if(!GetOpenFileName(&ofn)) return;

	TCHAR *text = NULL, extra[200];
	if(!twpng_ReadAnyAIParams(fn, &text, extra, 200)) {
		mesg(MSG_I,_T("No AI parameters were found in this file."));
		return;
	}
	twpng_ShowAIParams(text, extra, fn, FALSE);
	free(text);
}

// selects a range of items, and sets the focus to the first of that range
void twpng_SetLVSelection(HWND hwnd, int pos1, int num)
{
	int i;

	for(i=pos1;i<pos1+num;i++) {
		ListView_SetItemState(hwnd,i,
			LVIS_SELECTED| ((i==pos1)?LVIS_FOCUSED:0),
			LVIS_SELECTED| ((i==pos1)?LVIS_FOCUSED:0)  );
	}
	ListView_EnsureVisible(hwnd,pos1,FALSE);
}


static void LVUnselectAll(HWND hwnd)
{
	int i,c;

	c=ListView_GetItemCount(hwnd);
	for(i=0;i<c;i++) {
		ListView_SetItemState(hwnd,i,0,LVIS_SELECTED);
	}
}

static int GetLVFocus(HWND hwnd)
{
	int i;
	for(i=0;i<png->m_num_chunks;i++) {
		if(ListView_GetItemState(hwnd,i,LVIS_FOCUSED) & LVIS_FOCUSED) {
			return i;
		}
	}
	return 0;
}

// returns the selected item if exactly one item is selected,
// otherwise returns -1
static int GetLVSelection(HWND hwnd)
{
	int i;

	if(ListView_GetSelectedCount(hwnd)!=1) return(-1);

	for(i=0;i<png->m_num_chunks;i++) {
		if(ListView_GetItemState(hwnd,i,LVIS_SELECTED) & LVIS_SELECTED) {
			return i;
		}
	}
	return(-1);
}

static void PasteChunks()
{
	size_t msize_padded;
	size_t msize=0;
	HGLOBAL hClip = NULL;
	unsigned char* lpClip;
	size_t p;
	bool need_unlock = false;
	int r,inspos1,inspos,numnewchunks;

	inspos1=inspos=GetLVFocus(globals.hwndMainList);
	numnewchunks=0;

	if(!IsClipboardFormatAvailable(globals.pngchunk_cf)) return;

	OpenClipboard(NULL);
	hClip=GetClipboardData(globals.pngchunk_cf);
	if(!hClip) goto done;

	lpClip= (unsigned char*)GlobalLock(hClip);
	if(!lpClip) goto done;
	need_unlock = true;

	msize_padded = GlobalSize(hClip);
	if(msize_padded<4) goto done;
	msize = (size_t)read_int32(&lpClip[0]);
	if(msize>msize_padded) goto done;
	p=4;
	while(p<msize) {
		png->insert_chunks(inspos,1,1);  // this is wrong, I should make a new chunk, then insert it if things go okay
		r= png->chunk[inspos]->init_from_memory(&lpClip[p], (int)(msize-p));
		if(!r) break;  // error occurred
		inspos++;
		numnewchunks++;
		p+=r;
	}
	GlobalUnlock(hClip);
	need_unlock = false;
	png->fill_listbox(globals.hwndMainList);

	// reselect the new or changed chunks
	twpng_SetLVSelection(globals.hwndMainList,inspos1,numnewchunks);
	png->modified();

done:
	if(hClip) {
		if(need_unlock) GlobalUnlock(hClip);

	}
	CloseClipboard();
}

// returns 1 if any chunks were selected, and successfully copied
// otherwize 0
static int CopyChunks()
{
	int i;
	DWORD msize=0;
	HGLOBAL hClip;
	unsigned char* lpClip;
	int p;

	for(i=0;i<png->m_num_chunks;i++) {
		if(ListView_GetItemState(globals.hwndMainList,i,LVIS_SELECTED) & LVIS_SELECTED) {
			msize+= 12+png->chunk[i]->length;
		}
	}
	if(msize<1) return 0;  // nothing selected
	msize+=4;

	hClip=GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,msize);
	if(!hClip) {
		return 0;
	}

	lpClip= (unsigned char*)GlobalLock(hClip);
	if(!lpClip) {
		GlobalFree(hClip);
		return 0;
	}

	// I don't know how to figure out the size of the clipboard data
	// when retrieving it. Am I just stupid?
	// GlobalSize() often returns a larger than actual size ...
	// As a workaround, the first 4 bytes of the clipboard data
	// will contain the total length (including those 4 bytes)

	write_int32(&lpClip[0],msize);
	p=4;
	for(i=0;i<png->m_num_chunks;i++) {
		if(ListView_GetItemState(globals.hwndMainList,i,LVIS_SELECTED) & LVIS_SELECTED) {
			png->chunk[i]->copy_to_memory((unsigned char*)&lpClip[p]);
			p+= 12+png->chunk[i]->length;
		}
	}
	GlobalUnlock(hClip);

	OpenClipboard(NULL);
	EmptyClipboard();
	SetClipboardData(globals.pngchunk_cf,hClip);
	CloseClipboard();
	return 1;
}

static void CutChunks()
{
	if(CopyChunks()) {
		DeleteChunks();
	}
}

static void ImportChunkByFilename(const TCHAR *fn, int pos)
{
	HANDLE fh;
	Chunk *c;
	DWORD n;

	fh=CreateFile(fn,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,NULL);
	if(fh==INVALID_HANDLE_VALUE) {
		mesg(MSG_E,_T("Can") SYM_RSQUO _T("t open file (%s)"),fn);
		return;
	}

	c=new Chunk;
	c->length=GetFileSize(fh,NULL)-4;
	(void)ReadFile(fh,(LPVOID)c->m_chunktype_ascii,4,&n,NULL);
	c->m_chunktype_ascii[4]='\0';
	c->set_chunktype_tchar_from_ascii();
	c->data=(unsigned char*)malloc(c->length);
	(void)ReadFile(fh,(LPVOID)c->data,c->length,&n,NULL);
	CloseHandle(fh);

	c->m_parentpng=png;
	c->after_init();
	c->chunkmodified();


	png->insert_chunks(pos,1,0);
	png->chunk[pos]=c;

	png->fill_listbox(globals.hwndMainList);
	png->modified();
	twpng_SetLVSelection(globals.hwndMainList,pos,1);
}

static void ImportChunk()
{
	OPENFILENAME ofn;
	TCHAR fn[MAX_PATH];
	int pos;
	BOOL bRet;

	pos=GetLVFocus(globals.hwndMainList);

	StringCchCopy(fn,MAX_PATH,_T(""));
	ZeroMemory(&ofn,sizeof(OPENFILENAME));

	ofn.lStructSize=sizeof(OPENFILENAME);
	ofn.hwndOwner=globals.hwndMain;
	ofn.hInstance=NULL;
	ofn.lpstrFilter=_T("*.chunk\0*.chunk\0*.*\0*.*\0\0");
	ofn.nFilterIndex=1;
	ofn.lpstrTitle=_T("Import chunk...");
//	if(strlen(last_open_dir))
//		ofn.lpstrInitialDir=last_open_dir;  // else NULL ==> current dir
	ofn.lpstrFile=fn;
	ofn.nMaxFile=MAX_PATH;
	ofn.Flags=OFN_FILEMUSTEXIST|OFN_HIDEREADONLY|OFN_NOCHANGEDIR;

	globals.dlgs_open++;
	bRet = GetOpenFileName(&ofn);
	globals.dlgs_open--;
	if(!bRet) return;

	ImportChunkByFilename(fn, pos);
}

static void ExportChunk()
{
	OPENFILENAME ofn;
	TCHAR fn[MAX_PATH];
	HANDLE fh;
	int n;
	BOOL bRet;

	n=GetLVSelection(globals.hwndMainList);
	if(n<0) {
		mesg(MSG_I,_T("You must select a single chunk first"));
		return;
	}

	StringCchCopy(fn,MAX_PATH,_T(""));

	ZeroMemory(&ofn,sizeof(OPENFILENAME));
	ofn.lStructSize=sizeof(OPENFILENAME);
	ofn.hwndOwner=globals.hwndMain;
	ofn.lpstrFilter=_T("*.chunk\0*.chunk\0*.*\0*.*\0\0");
	ofn.nFilterIndex=1;
	ofn.lpstrTitle=_T("Export chunk to file...");
	ofn.lpstrFile=fn;
	ofn.nMaxFile=MAX_PATH;
	ofn.Flags=OFN_PATHMUSTEXIST|OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR;
	ofn.lpstrDefExt=_T("dat");

	globals.dlgs_open++;
	bRet = GetSaveFileName(&ofn);
	globals.dlgs_open--;
	if(bRet) {
		fh=CreateFile(ofn.lpstrFile,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,NULL);
		if(fh==INVALID_HANDLE_VALUE) {
			mesg(MSG_E,_T("Can") SYM_RSQUO _T("t create file"));
		}
		else {
			png->chunk[n]->write_to_file(fh,1);
			CloseHandle(fh);
		}
	}
}

static void MoveChunkUp()
{
	int i;

	if(ListView_GetSelectedCount(globals.hwndMainList)<1) return;

	// mark selected chunks so we can reselect them later
	for(i=0;i<png->m_num_chunks;i++) {
		png->chunk[i]->m_flag= (ListView_GetItemState(globals.hwndMainList,i,LVIS_SELECTED) & LVIS_SELECTED)?1:0;
	}

	// can't move up if first is selected
	if(png->chunk[0]->m_flag) return;

	for(i=1;i<png->m_num_chunks;i++) {
		if(png->chunk[i]->m_flag) {
			png->move_chunk(i,-1);
		}
	}

	png->fill_listbox(globals.hwndMainList);

	for(i=png->m_num_chunks-1;i>=0;i--) {   // reselect moved chunks
		if(png->chunk[i]->m_flag) twpng_SetLVSelection(globals.hwndMainList,i,1);
	}

	png->modified();
}


static void MoveChunkDown()
{
	int i;

	if(ListView_GetSelectedCount(globals.hwndMainList)<1) return;

	// mark selected chunks so we can reselect them later
	for(i=0;i<png->m_num_chunks;i++) {
		png->chunk[i]->m_flag= (ListView_GetItemState(globals.hwndMainList,i,LVIS_SELECTED) & LVIS_SELECTED)?1:0;
	}

	// can't move down if last is selected
	if(png->chunk[png->m_num_chunks-1]->m_flag) return;

	for(i=png->m_num_chunks-1;i>=0;i--) {
		if(png->chunk[i]->m_flag) {
			png->move_chunk(i,1);
		}
	}

	png->fill_listbox(globals.hwndMainList);

	for(i=0;i<png->m_num_chunks;i++) {   // reselect moved chunks
		if(png->chunk[i]->m_flag) twpng_SetLVSelection(globals.hwndMainList,i,1);
	}

	png->modified();
}


// split chunk n at size ssize (and repeat if repeat==1)
int Png::split_idat(int n, int ssize, int repeat)
{
	Chunk *c;
	int new_chunks;
	int i;
	int bytes_used;
	int thissize;
	TCHAR buf[200];

	c=chunk[n];

	// how many chunks will there be after the split
	if(!repeat) {
		new_chunks=2;
	}
	else {
		new_chunks= (c->length + ssize-1) / ssize;
	}

	if(new_chunks>100) {
		StringCchPrintf(buf,200,_T("This will create %d new chunks. Are you sure you want to continue?"),new_chunks);
		i=twpng_MessageBox(globals.hwndMain,buf,_T("Warning"),MB_ICONQUESTION|MB_YESNO|MB_DEFBUTTON2);
		if(i!=IDYES) return 0;
	}

	chunk[n]=new Chunk;

	insert_chunks(n,new_chunks-1,1);  // -1 because we start with one already
	for(i=n;i<n+new_chunks;i++) {
		chunk[i]->m_parentpng=png;
		StringCchCopyA(chunk[i]->m_chunktype_ascii,5,c->m_chunktype_ascii);
		StringCchCopy(chunk[i]->m_chunktype_tchar,5,c->m_chunktype_tchar);
	}

	bytes_used=0;

	for(i=n;i<n+new_chunks;i++) {
		if(i<n+new_chunks-1) {
			thissize=ssize;
		}
		else {
			thissize= c->length - bytes_used;  // == all remaining bytes
		}

		chunk[i]->length = thissize;
		if(thissize>0) {
			chunk[i]->data = (unsigned char*)malloc(thissize);
			memcpy((void*)chunk[i]->data,(void*)&c->data[bytes_used],thissize);
		}
		else {
			chunk[i]->data = NULL;
		}

		bytes_used += thissize;
	}

	delete c;

	for(i=n;i<n+new_chunks;i++) {
		chunk[i]->after_init();
		chunk[i]->chunkmodified();
	}
	modified();
	fill_listbox(globals.hwndMainList);

	// reselect the new or changed chunks
	twpng_SetLVSelection(globals.hwndMainList,n,new_chunks);

	return 1;
}

static void SplitIDAT()
{
	int n,id;

	n=GetLVSelection(globals.hwndMainList);
	if(n<0) {
		mesg(MSG_I,_T("Select a single IDAT chunk first"));
		return;
	}

	id=png->chunk[n]->m_chunktype_id;

	if(id!=CHUNK_IDAT && id!=CHUNK_JDAT) {
		mesg(MSG_I,_T("Select an IDAT chunk first"));
		return;
	}

	png->chunk[n]->m_index=n;
	globals.dlgs_open++;
	DialogBoxParam(globals.hInst,_T("DLG_SPLITIDAT"),globals.hwndMain,DlgProcSplitIDAT,
		(LPARAM)png->chunk[n]);
	globals.dlgs_open--;

}

static void CombineIDAT_range(int first, int last)
{
	DWORD len,pos;
	int i;
	unsigned char *newdata;
	Chunk *c;

	// calculate total length of data in new IDAT chunk
	len=0;
	for(i=first;i<=last;i++) {
		len+=png->chunk[i]->length;
	}

	newdata= (unsigned char*)malloc(len);
	if(!newdata) {
		mesg(MSG_S,_T("Can") SYM_RSQUO _T("t allocate memory"));
		return;
	}

	pos=0;
	for(i=first;i<=last;i++) {
		if(png->chunk[i]->length>0) {
			memcpy((void*)&newdata[pos],
				(void*)png->chunk[i]->data,png->chunk[i]->length);
			pos+=png->chunk[i]->length;
		}
	}

	c=new Chunk;
	c->m_parentpng = png;
	c->data=newdata;
	c->length=len;
	StringCchCopyA(c->m_chunktype_ascii,5,png->chunk[first]->m_chunktype_ascii);
	StringCchCopy(c->m_chunktype_tchar,5,png->chunk[first]->m_chunktype_tchar);
	c->after_init();

	c->chunkmodified();    // set crc

	for(i=first;i<=last;i++) {
		delete png->chunk[i];
	}

	png->chunk[first]=c;

	// we're deleting  (last-first) chunks
	for(i=first+1;i<=png->m_num_chunks-(last-first+1);i++) {
		png->chunk[i] = png->chunk[i+(last-first)];
	}

	png->m_num_chunks -= (last-first);
}

static void CombineIDAT_selected()
{
	int i, first, last;
	int idat_count, jdat_count;
	int chunktype;

	first= -1; last= -1;
	idat_count=0;
	jdat_count=0;

	for(i=0;i<png->m_num_chunks;i++) {

		if(ListView_GetItemState(globals.hwndMainList,i,LVIS_SELECTED) & LVIS_SELECTED) {
			chunktype = png->chunk[i]->m_chunktype_id;
			if(chunktype==CHUNK_IDAT) {
				idat_count++;
			}
			else if(chunktype==CHUNK_JDAT) {
				jdat_count++;
			}
			else {
				mesg(MSG_E,_T("Only IDAT chunks may be selected"));
				return;
			}
			if(first == -1) {
				first = i;
			}
			else if(last != -1) {
				mesg(MSG_E,_T("Selected chunks must be consecutive"));
				return;
			}
		}
		else {    // not selected
			if(first != -1) {
				if(last == -1) {
					last = i-1;
				}
			}
		}
	}

	if(first == -1) {
		mesg(MSG_E,_T("Must select IDAT chunks first"));
		return;
	}

	if(last == -1) last=png->m_num_chunks-1;

	if(idat_count>0 && jdat_count>0) {
		mesg(MSG_E,_T("Selected chunks must be the same type"));
		return;
	}

	if(last<=first) {
		mesg(MSG_E,_T("Must select more than one IDAT chunk"));
		return;
	}

	CombineIDAT_range(first,last);

	png->fill_listbox(globals.hwndMainList);
	twpng_SetLVSelection(globals.hwndMainList,first,1);

	png->modified();
}

// Locate the first run of 2 or more IDAT or JDAT chunks.
// Returns 0 if not found.
static int find_IDAT_range(int *pfirst, int *plast)
{
	int prevchunktype = CHUNK_UNKNOWN;
	int chunktype = CHUNK_UNKNOWN;
	int i;
	int in_run = 0;


	for(i=0;i<png->m_num_chunks;i++) {
		chunktype = png->chunk[i]->m_chunktype_id;
		if(in_run) {
			if(chunktype==prevchunktype) {
				// Countinuing a run
				*plast = i;
			}
			else {
				// End of a run
				*plast = i-1;
				return 1;
			}

		}
		else {
			if(chunktype==prevchunktype && (chunktype==CHUNK_IDAT || chunktype==CHUNK_JDAT)) {
				// Found the start of a run.
				in_run = 1;
				*pfirst = i-1;
			}
		}
		prevchunktype = chunktype;
	}

	if(in_run)
		return 1;
	return 0;
}

static void CombineIDAT_all()
{
	int first, last;
	int ret;
	int num_idat;
	int num_ranges;
	int i;
	int x;

	// Count number if IDAT/JDAT chunks, and complain if there aren't any.
	num_idat = 0;
	for(i=0;i<png->m_num_chunks;i++) {
		if(png->chunk[i]->m_chunktype_id==CHUNK_IDAT ||
			png->chunk[i]->m_chunktype_id==CHUNK_JDAT)
		{
			num_idat++;
		}
	}

	if(num_idat<1) {
		mesg(MSG_E,_T("No IDAT chunks present"));
		return;
	}

	// Find all ranges of 2 or more IDAT/JDAT chunks, and combine them.
	num_ranges = 0;
	while(1) {
		first= -1; last= -1;
		ret = find_IDAT_range(&first,&last);
		if(!ret) break;
		CombineIDAT_range(first,last);
		num_ranges++;
	}

	// If we made any changes, refresh the list.
	if(num_ranges>0) {
		png->fill_listbox(globals.hwndMainList);
		twpng_SetLVSelection(globals.hwndMainList,first,1);

		png->modified();
	}

	// Select all IDAT and JDAT chunks
	for(i=0;i<png->m_num_chunks;i++) {
		if(png->chunk[i]->m_chunktype_id==CHUNK_IDAT ||
			png->chunk[i]->m_chunktype_id==CHUNK_JDAT)
		{
			x = LVIS_SELECTED;
		}
		else {
			x = 0;
		}

		ListView_SetItemState(globals.hwndMainList,i,x,LVIS_SELECTED);
	}
}

static void DblClickOnList()
{
	int s;

	s=GetLVSelection(globals.hwndMainList);
	if(s>=0 && s<png->m_num_chunks) {
		png->edit_chunk(s);
	}
}

// ---- Theme (Options > Theme: System / Light / Dark) ------------------------------------

static BOOL twpng_SystemUsesDark()
{
	DWORD val = 1, sz = sizeof(val), type = 0;
	HKEY hKey;
	if(RegOpenKeyEx(HKEY_CURRENT_USER,
		_T("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
		0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		if(RegQueryValueEx(hKey, _T("AppsUseLightTheme"), NULL, &type, (LPBYTE)&val, &sz)!=ERROR_SUCCESS
		   || type!=REG_DWORD) val = 1;
		RegCloseKey(hKey);
	}
	return val==0;
}

void twpng_ResolveTheme()
{
	globals.dark = globals.theme_pref==TWPNG_THEME_DARK ||
		(globals.theme_pref==TWPNG_THEME_SYSTEM && twpng_SystemUsesDark());
}

// App-wide mode for menus, scrollbars and common controls (undocumented uxtheme ordinals).
void twpng_SetAppMode()
{
	HMODULE hUx = LoadLibrary(_T("uxtheme.dll"));
	if(!hUx) return;
	typedef int (WINAPI *SetPreferredAppModeProc)(int);
	typedef void (WINAPI *VoidProc)();
	SetPreferredAppModeProc setMode = (SetPreferredAppModeProc)GetProcAddress(hUx, MAKEINTRESOURCEA(135));
	VoidProc refreshPolicy = (VoidProc)GetProcAddress(hUx, MAKEINTRESOURCEA(104));
	VoidProc flushMenus = (VoidProc)GetProcAddress(hUx, MAKEINTRESOURCEA(136));
	if(setMode) setMode(globals.dark ? 2 : 3);   // ForceDark / ForceLight
	if(refreshPolicy) refreshPolicy();
	if(flushMenus) flushMenus();
	FreeLibrary(hUx);
}

static void twpng_AllowDark(HWND hwnd, BOOL dark)
{
	if(!hwnd) return;
	HMODULE hUx = LoadLibrary(_T("uxtheme.dll"));
	if(!hUx) return;
	typedef BOOL (WINAPI *AllowDarkModeForWindowProc)(HWND, BOOL);
	AllowDarkModeForWindowProc f = (AllowDarkModeForWindowProc)GetProcAddress(hUx, MAKEINTRESOURCEA(133));
	if(f) f(hwnd, dark);
	FreeLibrary(hUx);
}

// The dark brushes double as the "dark mode is on" switch for the WM_CTLCOLOR handlers.
static void twpng_UpdateThemeBrushes()
{
	if(globals.hUiBgBrush) { DeleteObject(globals.hUiBgBrush); globals.hUiBgBrush = NULL; }
	if(globals.hDarkEditBrush) { DeleteObject(globals.hDarkEditBrush); globals.hDarkEditBrush = NULL; }
	if(globals.dark) {
		globals.hUiBgBrush = CreateSolidBrush(RGB(32,32,32));
		globals.hDarkEditBrush = CreateSolidBrush(RGB(45,45,45));
	}
}

static void twpng_ThemeListView(HWND lv)
{
	BOOL d = globals.dark;
	HWND hdr = ListView_GetHeader(lv);
	twpng_AllowDark(lv, d);
	twpng_AllowDark(hdr, d);
	SetWindowTheme(lv, d ? _T("DarkMode_Explorer") : _T("Explorer"), NULL);
	COLORREF bg = d ? RGB(32,32,32) : GetSysColor(COLOR_WINDOW);
	ListView_SetBkColor(lv, bg);
	ListView_SetTextBkColor(lv, bg);
	ListView_SetTextColor(lv, d ? RGB(255,255,255) : GetSysColor(COLOR_WINDOWTEXT));
	if(hdr) SetWindowTheme(hdr, d ? _T("DarkMode_ItemsView") : _T("ItemsView"), NULL);
}

static void twpng_ThemeMainControls()
{
	BOOL d = globals.dark;
	HWND lv = globals.hwndMainList, hdr = lv ? ListView_GetHeader(lv) : NULL;
	twpng_AllowDark(globals.hwndStBar, d);
	twpng_AllowDark(lv, d);
	twpng_AllowDark(hdr, d);
	if(lv) twpng_ThemeListView(lv);
	(void)hdr;
}

static BOOL CALLBACK twpng_RethemeWindow(HWND h, LPARAM lParam)
{
	(void)lParam;
	if(h==globals.hwndMain || !IsWindowVisible(h)) return TRUE;
	TCHAR cls[32];
	GetClassName(h, cls, 32);
	if(!lstrcmp(cls, _T("#32770"))) {
		twpng_InitDarkDialog(h);
	} else if(!lstrcmp(cls, _T("TPNGVIEWER"))) {
		twpng_ApplyModernWindowStyle(h);
		SetWindowPos(h, NULL, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
		RedrawWindow(h, NULL, NULL, RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN);
	}
	return TRUE;
}

// Re-apply the theme to the running app (main window plus open modeless windows).
static void twpng_ApplyTheme()
{
	twpng_ResolveTheme();
	twpng_SetAppMode();
	twpng_UpdateThemeBrushes();
	twpng_ApplyModernWindowStyle(globals.hwndMain);
	twpng_ThemeMainControls();
	EnumThreadWindows(GetCurrentThreadId(), twpng_RethemeWindow, 0);
	SetWindowPos(globals.hwndMain, NULL, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
	RedrawWindow(globals.hwndMain, NULL, NULL, RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);
}

void twpng_ApplyModernWindowStyle(HWND hwnd)
{
	typedef HRESULT (WINAPI *DwmSetWindowAttributeProc)(HWND,DWORD,LPCVOID,DWORD);
	HMODULE hDwm;
	DwmSetWindowAttributeProc pDwmSetWindowAttribute;
	BOOL use_dark_title;
	int corner_pref;
	int backdrop_type;

	// Enable deep dark mode for main window
	HMODULE hUxTheme = LoadLibrary(_T("uxtheme.dll"));
	if(hUxTheme) {
		typedef BOOL (WINAPI *AllowDarkModeForWindowProc)(HWND, BOOL);
		typedef void (WINAPI *FlushMenuThemesProc)();
		AllowDarkModeForWindowProc AllowDarkModeForWindow = (AllowDarkModeForWindowProc)GetProcAddress(hUxTheme, MAKEINTRESOURCEA(133));
		FlushMenuThemesProc FlushMenuThemes = (FlushMenuThemesProc)GetProcAddress(hUxTheme, MAKEINTRESOURCEA(136));
		if(AllowDarkModeForWindow) {
			AllowDarkModeForWindow(hwnd, globals.dark);
		}
		if(FlushMenuThemes) {
			FlushMenuThemes();
		}
		// Set dark window theme class to force standard Win32 menu bar to go dark
		SetWindowTheme(hwnd, globals.dark ? _T("DarkMode_Explorer") : NULL, NULL);
		FreeLibrary(hUxTheme);
	}

	hDwm=LoadLibrary(_T("dwmapi.dll"));
	if(!hDwm) return;
	pDwmSetWindowAttribute=(DwmSetWindowAttributeProc)GetProcAddress(hDwm,"DwmSetWindowAttribute");
	if(pDwmSetWindowAttribute) {
		use_dark_title=globals.dark;
		pDwmSetWindowAttribute(hwnd,20,&use_dark_title,sizeof(use_dark_title));
		corner_pref=2;
		pDwmSetWindowAttribute(hwnd,33,&corner_pref,sizeof(corner_pref));
		backdrop_type=2;
		pDwmSetWindowAttribute(hwnd,38,&backdrop_type,sizeof(backdrop_type));
	}
	FreeLibrary(hDwm);

	// Force menu bar to redraw using the dark theme attributes
	DrawMenuBar(hwnd);
}

// Owner-draws a window's standard menu bar in dark mode via the undocumented UAH
// messages, and paints over the 1px light line Windows leaves under it. Shared by the
// main window and the image viewer. Returns TRUE if the message was handled.
BOOL twpng_HandleDarkMenuMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *result)
{
	if(!globals.dark) return FALSE;
	switch(msg) {
	case WM_UAHDRAWMENU:
		{
			UAHMENU *pUDM = (UAHMENU*)lParam;
			MENUBARINFO mbi = {};
			mbi.cbSize = sizeof(mbi);
			GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi);
			RECT rcWindow = {};
			GetWindowRect(hwnd, &rcWindow);
			RECT rc = mbi.rcBar;
			OffsetRect(&rc, -rcWindow.left, -rcWindow.top);
			if(globals.hUiBgBrush) FillRect(pUDM->hdc, &rc, globals.hUiBgBrush);
			*result = TRUE;
			return TRUE;
		}

	case WM_UAHDRAWMENUITEM:
		{
			UAHDRAWMENUITEM *pUDMI = (UAHDRAWMENUITEM*)lParam;
			HDC hdc = pUDMI->um.hdc;
			RECT rcItem = pUDMI->dis.rcItem;

			BOOL created = FALSE;
			HBRUSH hFill;
			if(pUDMI->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED)) {
				hFill = CreateSolidBrush(RGB(58, 58, 58));
				created = TRUE;
			} else {
				hFill = globals.hUiBgBrush;
			}
			FillRect(hdc, &rcItem, hFill ? hFill : (HBRUSH)GetStockObject(BLACK_BRUSH));
			if(created) DeleteObject(hFill);

			MENUITEMINFO mii = {};
			mii.cbSize = sizeof(mii);
			mii.fMask = MIIM_STRING;
			TCHAR szMenuText[256] = {};
			mii.dwTypeData = szMenuText;
			mii.cch = 255;
			GetMenuItemInfo(pUDMI->um.hmenu, pUDMI->umi.iPosition, TRUE, &mii);

			DWORD dtFlags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
			if(pUDMI->dis.itemState & ODS_NOACCEL) dtFlags |= DT_HIDEPREFIX;

			SetBkMode(hdc, TRANSPARENT);
			SetTextColor(hdc, (pUDMI->dis.itemState & ODS_GRAYED) ? RGB(120, 120, 120) : RGB(220, 220, 220));
			HGDIOBJ hOldFont2 = globals.hUiFont ? SelectObject(hdc, globals.hUiFont) : NULL;
			DrawText(hdc, szMenuText, -1, &rcItem, dtFlags);
			if(hOldFont2) SelectObject(hdc, hOldFont2);
			*result = TRUE;
			return TRUE;
		}

	case WM_NCACTIVATE:
	case WM_NCPAINT:
		{
			// Let Windows draw the title bar / frame normally, then paint over the
			// 1px light line Windows leaves at the bottom of the (dark) menu bar.
			LRESULT lr = DefWindowProc(hwnd, msg, wParam, lParam);
			if(globals.hUiBgBrush) {
				MENUBARINFO mbi = {};
				mbi.cbSize = sizeof(mbi);
				if(GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) {
					RECT rcWindow = {};
					GetWindowRect(hwnd, &rcWindow);
					RECT rcLine = mbi.rcBar;
					OffsetRect(&rcLine, -rcWindow.left, -rcWindow.top);
					rcLine.top = rcLine.bottom;
					rcLine.bottom = rcLine.top + 1;
					HDC hdc = GetWindowDC(hwnd);
					if(hdc) {
						FillRect(hdc, &rcLine, globals.hUiBgBrush);
						ReleaseDC(hwnd, hdc);
					}
				}
			}
			*result = lr;
			return TRUE;
		}
	}
	return FALSE;
}

static void twpng_CreateUiFont(UINT dpi = 0)
{
	NONCLIENTMETRICS ncm;
	BOOL ok = FALSE;

	ZeroMemory(&ncm,sizeof(ncm));
	ncm.cbSize=sizeof(ncm);

	if(dpi > 0) {
		typedef BOOL (WINAPI *SPI4DPI_t)(UINT, UINT, PVOID, UINT, UINT);
		SPI4DPI_t pSPI4DPI = (SPI4DPI_t)GetProcAddress(
			GetModuleHandle(_T("user32.dll")), "SystemParametersInfoForDpi");
		if(pSPI4DPI) ok = pSPI4DPI(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
	}
	if(!ok) ok = SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);

	if(ok) {
		if(globals.hUiFont) DeleteObject(globals.hUiFont);
		globals.hUiFont = CreateFontIndirect(&ncm.lfMessageFont);
	}
}

// Subtle 1px border color for edit controls and the chunk table.
static COLORREF twpng_GetEditBorderColor()
{
	return globals.dark ? RGB(90,90,90) : RGB(171,171,171);
}

// Paints a flat border over a control's non-client edge: outer 1px in the border
// color, inner 1px in the dark UI background (covering the light sunken client edge).
// Drawing only the perimeter leaves any scrollbar untouched.
static void twpng_PaintFlatBorder(HWND hwnd, COLORREF borderColor)
{
	HDC hdc = GetWindowDC(hwnd);
	if(!hdc) return;
	RECT rc;
	GetWindowRect(hwnd, &rc);
	OffsetRect(&rc, -rc.left, -rc.top);
	HBRUSH hbOuter = CreateSolidBrush(borderColor);
	FrameRect(hdc, &rc, hbOuter);
	DeleteObject(hbOuter);
	if(globals.hUiBgBrush) {
		InflateRect(&rc, -1, -1);
		FrameRect(hdc, &rc, globals.hUiBgBrush);
	}
	ReleaseDC(hwnd, hdc);
}

// Draws a flat border around an edit control (color passed via dwRefData) and keeps it
// visible across focus/hover repaints, which would otherwise restore the light edge.
static LRESULT CALLBACK EditBorderSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
	LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	switch(msg) {
	case WM_NCPAINT:
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		{
			LRESULT lr = DefSubclassProc(hwnd, msg, wParam, lParam);
			twpng_PaintFlatBorder(hwnd, (COLORREF)dwRefData);
			return lr;
		}
	case WM_NCDESTROY:
		RemoveWindowSubclass(hwnd, EditBorderSubclassProc, uIdSubclass);
		break;
	}
	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// Owner-draws a radio button so its label uses light text in dark mode while still
// drawing the modern themed (dark) radio glyph. The control's default proc still
// handles checking/unchecking; we only take over painting.
static LRESULT CALLBACK RadioSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
	LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	(void)dwRefData;
	switch(msg) {
	case WM_NCDESTROY:
		RemoveWindowSubclass(hwnd, RadioSubclassProc, uIdSubclass);
		break;
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		InvalidateRect(hwnd, NULL, FALSE);
		break;
	case WM_ERASEBKGND:
		return 1;  // painted in WM_PAINT
	case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			RECT rc;
			GetClientRect(hwnd, &rc);
			if(globals.hUiBgBrush) FillRect(hdc, &rc, globals.hUiBgBrush);

			BOOL checked = (SendMessage(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
			BOOL enabled = IsWindowEnabled(hwnd);
			int stateId = checked
				? (enabled ? RBS_CHECKEDNORMAL   : RBS_CHECKEDDISABLED)
				: (enabled ? RBS_UNCHECKEDNORMAL : RBS_UNCHECKEDDISABLED);

			HTHEME ht = OpenThemeData(hwnd, _T("Button"));
			SIZE gsz = {13,13};
			if(ht) GetThemePartSize(ht, hdc, BP_RADIOBUTTON, stateId, NULL, TS_DRAW, &gsz);
			int gy = (rc.bottom - gsz.cy)/2;
			RECT gr = {rc.left, gy, rc.left+gsz.cx, gy+gsz.cy};
			if(ht) DrawThemeBackground(ht, hdc, BP_RADIOBUTTON, stateId, &gr, NULL);
			if(ht) CloseThemeData(ht);

			RECT tr = rc;
			tr.left = gr.right + 4;
			TCHAR txt[256] = _T("");
			GetWindowText(hwnd, txt, 256);
			SetBkMode(hdc, TRANSPARENT);
			SetTextColor(hdc, enabled ? RGB(255,255,255) : RGB(140,140,140));
			HGDIOBJ oldFont = globals.hUiFont ? SelectObject(hdc, globals.hUiFont) : NULL;
			DrawText(hdc, txt, -1, &tr, DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);

			if(GetFocus()==hwnd && txt[0]) {
				RECT fr = tr;
				fr.right = fr.left;
				fr.top = 0; fr.bottom = 0;
				DrawText(hdc, txt, -1, &fr, DT_LEFT|DT_SINGLELINE|DT_NOPREFIX|DT_CALCRECT);
				RECT focus = tr;
				focus.right = focus.left + (fr.right - fr.left) + 2;
				InflateRect(&focus, 0, -1);
				DrawFocusRect(hdc, &focus);
			}
			if(oldFont) SelectObject(hdc, oldFont);

			EndPaint(hwnd, &ps);
			return 0;
		}
	}
	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static BOOL CALLBACK DarkModeEnumChildProc(HWND hChild, LPARAM lParam)
{
	TCHAR classname[64];
	GetClassName(hChild, classname, 64);

	if(!_tcsicmp(classname, _T("SysListView32"))) {
		twpng_ThemeListView(hChild);
		if(globals.hUiFont) SendMessage(hChild, WM_SETFONT, (WPARAM)globals.hUiFont, TRUE);
		return TRUE;
	}
	if(!globals.dark) {
		// Light theme: plain themed controls (also undoes dark styling on a live switch).
		if(!_tcsicmp(classname,_T("Edit")) || !_tcsicmp(classname,_T("Button")) ||
		   !_tcsicmp(classname,_T("ListBox")) || !_tcsicmp(classname,_T("ComboBox"))) {
			SetWindowTheme(hChild, NULL, NULL);
			RemoveWindowSubclass(hChild, EditBorderSubclassProc, 1);
			RemoveWindowSubclass(hChild, RadioSubclassProc, 1);
			SetWindowPos(hChild, NULL, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
		}
		twpng_AllowDark(hChild, FALSE);
		if(globals.hUiFont) SendMessage(hChild, WM_SETFONT, (WPARAM)globals.hUiFont, TRUE);
		return TRUE;
	}

	if(!_tcsicmp(classname, _T("Edit"))) {
		SetWindowTheme(hChild, _T("DarkMode_Explorer"), NULL);

		// Skip the edit embedded inside a combo box; it shouldn't get its own border.
		TCHAR parentClass[64] = _T("");
		HWND hParent = GetParent(hChild);
		if(hParent) GetClassName(hParent, parentClass, 64);

		if(_tcsicmp(parentClass, _T("ComboBox"))) {
			// Keep the sunken client edge (it preserves single-line text centering) but
			// paint over its bright pixels with a flat dark border via the subclass.
			SetWindowSubclass(hChild, EditBorderSubclassProc, 1,
				(DWORD_PTR)twpng_GetEditBorderColor());
			SetWindowPos(hChild, NULL, 0, 0, 0, 0,
				SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
		}
	}
	else if(!_tcsicmp(classname, _T("Button"))) {
		SetWindowTheme(hChild, _T("DarkMode_Explorer"), NULL);
		LONG bs = GetWindowLong(hChild, GWL_STYLE) & BS_TYPEMASK;
		if(bs==BS_RADIOBUTTON || bs==BS_AUTORADIOBUTTON) {
			// Themed radio buttons render their label black in dark mode, so owner-draw
			// them: keep the dark themed glyph but paint the label in light text.
			SetWindowSubclass(hChild, RadioSubclassProc, 1, 0);
		}
	}
	else if(!_tcsicmp(classname, _T("ListBox"))) {
		SetWindowTheme(hChild, _T("DarkMode_Explorer"), NULL);
	}
	else if(!_tcsicmp(classname, _T("ComboBox"))) {
		SetWindowTheme(hChild, _T("DarkMode_CFD"), NULL);
	}

	HMODULE hUxTheme = (HMODULE)lParam;
	if(hUxTheme) {
		typedef BOOL (WINAPI *AllowDarkModeForWindowProc)(HWND, BOOL);
		AllowDarkModeForWindowProc AllowDarkModeForWindow =
			(AllowDarkModeForWindowProc)GetProcAddress(hUxTheme, MAKEINTRESOURCEA(133));
		if(AllowDarkModeForWindow) AllowDarkModeForWindow(hChild, TRUE);
	}

	if(globals.hUiFont) {
		SendMessage(hChild, WM_SETFONT, (WPARAM)globals.hUiFont, TRUE);
	}
	return TRUE;
}

void twpng_InitDarkDialog(HWND hwnd)
{
	twpng_ApplyModernWindowStyle(hwnd);
	HMODULE hUxTheme = LoadLibrary(_T("uxtheme.dll"));
	EnumChildWindows(hwnd, DarkModeEnumChildProc, (LPARAM)hUxTheme);
	if(hUxTheme) FreeLibrary(hUxTheme);
	// Repaint the dialog AND every child control so freshly themed controls (radios,
	// checkboxes, etc.) drop their stale light-theme text instead of keeping it.
	RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW|RDW_FRAME);
}

// ---- Dark theming for system common dialogs (e.g. ChooseColor) ----------------------
// These can't host our dialog proc, so we briefly hook window activation and dark-theme
// the dialog chrome (title bar, labels, edits, buttons). Owner-drawn color swatches and
// the spectrum keep their own colors. Font is left alone so the layout isn't disturbed.

static HHOOK g_sysDlgCbtHook = NULL;

static BOOL CALLBACK SysDlgDarkChildProc(HWND hChild, LPARAM lParam)
{
	TCHAR cls[32];
	GetClassName(hChild, cls, 32);
	if(!_tcsicmp(cls,_T("Edit")) || !_tcsicmp(cls,_T("Button")) || !_tcsicmp(cls,_T("ListBox")))
		SetWindowTheme(hChild, _T("DarkMode_Explorer"), NULL);
	else if(!_tcsicmp(cls,_T("ComboBox")))
		SetWindowTheme(hChild, _T("DarkMode_CFD"), NULL);
	HMODULE hUx = (HMODULE)lParam;
	if(hUx) {
		typedef BOOL (WINAPI *AllowDarkModeForWindowProc)(HWND, BOOL);
		AllowDarkModeForWindowProc f =
			(AllowDarkModeForWindowProc)GetProcAddress(hUx, MAKEINTRESOURCEA(133));
		if(f) f(hChild, TRUE);
	}
	return TRUE;
}

static LRESULT CALLBACK SysDlgDarkSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
	LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	(void)dwRefData;
	switch(msg) {
	case WM_CTLCOLORDLG:
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
		if(globals.hUiBgBrush) {
			SetBkColor((HDC)wParam, RGB(32,32,32));
			SetTextColor((HDC)wParam, RGB(255,255,255));
			return (LRESULT)globals.hUiBgBrush;
		}
		break;
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
		if(globals.hDarkEditBrush) {
			SetBkColor((HDC)wParam, RGB(45,45,45));
			SetTextColor((HDC)wParam, RGB(220,220,220));
			return (LRESULT)globals.hDarkEditBrush;
		}
		break;
	case WM_NCDESTROY:
		RemoveWindowSubclass(hwnd, SysDlgDarkSubclassProc, uIdSubclass);
		break;
	}
	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK SysDlgDarkCbtProc(int code, WPARAM wParam, LPARAM lParam)
{
	if(code == HCBT_ACTIVATE) {
		HWND hwnd = (HWND)wParam;
		TCHAR cls[16];
		if(GetClassName(hwnd, cls, 16) && !_tcscmp(cls, _T("#32770"))) {
			twpng_ApplyModernWindowStyle(hwnd);
			HMODULE hUx = LoadLibrary(_T("uxtheme.dll"));
			EnumChildWindows(hwnd, SysDlgDarkChildProc, (LPARAM)hUx);
			if(hUx) FreeLibrary(hUx);
			SetWindowSubclass(hwnd, SysDlgDarkSubclassProc, 1, 0);
			RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW|RDW_FRAME);
		}
	}
	return CallNextHookEx(g_sysDlgCbtHook, code, wParam, lParam);
}

BOOL twpng_ChooseColorDark(LPCHOOSECOLOR cc)
{
	if(!globals.dark) return ChooseColor(cc);
	g_sysDlgCbtHook = SetWindowsHookEx(WH_CBT, SysDlgDarkCbtProc, NULL, GetCurrentThreadId());
	BOOL r = ChooseColor(cc);
	if(g_sysDlgCbtHook) {
		UnhookWindowsHookEx(g_sysDlgCbtHook);
		g_sysDlgCbtHook = NULL;
	}
	return r;
}

BOOL twpng_HandleDlgDarkMsg(UINT msg, WPARAM wParam, LPARAM lParam, INT_PTR *result)
{
	(void)lParam;
	if(!globals.dark) return FALSE;
	switch(msg) {
	case WM_CTLCOLORDLG:
	case WM_CTLCOLORSTATIC:
		if(globals.hUiBgBrush) {
			SetBkColor((HDC)wParam, RGB(32, 32, 32));
			SetTextColor((HDC)wParam, RGB(255, 255, 255));
			*result = (INT_PTR)globals.hUiBgBrush;
			return TRUE;
		}
		break;
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
		if(globals.hDarkEditBrush) {
			SetBkColor((HDC)wParam, RGB(45, 45, 45));
			SetTextColor((HDC)wParam, RGB(220, 220, 220));
			*result = (INT_PTR)globals.hDarkEditBrush;
			return TRUE;
		}
		break;
	case WM_CTLCOLORBTN:
		if(globals.hUiBgBrush) {
			SetBkColor((HDC)wParam, RGB(32, 32, 32));
			SetTextColor((HDC)wParam, RGB(255, 255, 255));
			*result = (INT_PTR)globals.hUiBgBrush;
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static void LayoutMainWindows(HWND hwnd)
{
	RECT r;

	GetClientRect(hwnd,&r);
	if(globals.hwndStBar) {
		// Inset the status text by 12px so it lines up with the table's left margin.
		SetWindowPos(globals.hwndStBar,NULL,
			r.left+12,r.bottom-globals.stbar_height,r.right-r.left-12,globals.stbar_height,
			SWP_NOZORDER);
	}
	if(globals.hwndMainList) {
		SetWindowPos(globals.hwndMainList,NULL,
			r.left+12,r.top+TWPNG_TOP_MARGIN,r.right-r.left-24,
			r.bottom-r.top-globals.stbar_height-TWPNG_TOP_MARGIN-12,
			SWP_NOZORDER);
	}
}

// Subclass on the main listview so we can intercept the header control's
// NM_CUSTOMDRAW (the header notifies its parent, the listview, not the main window).
// DarkMode_ItemsView does not reliably set the header text color on Windows 11, so we
// owner-draw each header item with a dark background and light text.
static LRESULT CALLBACK MainListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
	LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	(void)uIdSubclass; (void)dwRefData;
	if(msg == WM_NOTIFY && globals.dark) {
		LPNMHDR pnmh = (LPNMHDR)lParam;
		HWND hwndHdr = ListView_GetHeader(hwnd);
		if(pnmh->code == NM_CUSTOMDRAW && pnmh->hwndFrom == hwndHdr) {
			LPNMCUSTOMDRAW pcd = (LPNMCUSTOMDRAW)lParam;
			switch(pcd->dwDrawStage) {
			case CDDS_PREPAINT:
				return CDRF_NOTIFYITEMDRAW;
			case CDDS_ITEMPREPAINT:
				{
					HBRUSH hHdrBg = CreateSolidBrush(RGB(40, 40, 40));
					FillRect(pcd->hdc, &pcd->rc, hHdrBg);
					DeleteObject(hHdrBg);

					TCHAR szHdrText[256] = {};
					HDITEM hdi = {};
					hdi.mask = HDI_TEXT;
					hdi.pszText = szHdrText;
					hdi.cchTextMax = 255;
					Header_GetItem(hwndHdr, (int)pcd->dwItemSpec, &hdi);

					SetBkMode(pcd->hdc, TRANSPARENT);
					SetTextColor(pcd->hdc, RGB(200, 200, 200));
					HGDIOBJ hOldFont = globals.hUiFont ? SelectObject(pcd->hdc, globals.hUiFont) : NULL;
					RECT rcTxt = pcd->rc;
					InflateRect(&rcTxt, -6, 0);
					DrawText(pcd->hdc, szHdrText, -1, &rcTxt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
					if(hOldFont) SelectObject(pcd->hdc, hOldFont);

					HPEN hSepPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
					HGDIOBJ hOldPen = SelectObject(pcd->hdc, hSepPen);
					MoveToEx(pcd->hdc, pcd->rc.right - 1, pcd->rc.top + 4, NULL);
					LineTo(pcd->hdc, pcd->rc.right - 1, pcd->rc.bottom - 4);
					SelectObject(pcd->hdc, hOldPen);
					DeleteObject(hSepPen);

					return CDRF_SKIPDEFAULT;
				}
			}
		}
	}
	else if(msg == WM_NCPAINT) {
		// Draw a flat border around the chunk table, matching the edit borders.
		LRESULT lr = DefSubclassProc(hwnd, msg, wParam, lParam);
		HDC hdc = GetWindowDC(hwnd);
		if(hdc) {
			RECT rc;
			GetWindowRect(hwnd, &rc);
			OffsetRect(&rc, -rc.left, -rc.top);
			HBRUSH hbr = CreateSolidBrush(twpng_GetEditBorderColor());
			FrameRect(hdc, &rc, hbr);
			DeleteObject(hbr);
			ReleaseDC(hwnd, hdc);
		}
		return lr;
	}
	else if(msg == WM_NCDESTROY) {
		RemoveWindowSubclass(hwnd, MainListSubclassProc, 1);
	}
	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static int CreateMainWindows(HWND hwnd)
{
	LV_COLUMN lvc;
	RECT r;
	DWORD style;
	TCHAR textbuf[50];

	// Create a modern flat dark status bar using a STATIC text control
	globals.stbar_height=24;
	twpng_CreateUiFont();

	globals.hwndStBar=CreateWindowEx(0,_T("STATIC"),_T("No file loaded"),
		WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
		0,0,10,24,hwnd,(HMENU)ID_STBAR,globals.hInst,NULL);
	if(!globals.hwndStBar) {
		mesg(MSG_S,_T("Cannot create status bar"));
		return 0;
	}
	SendMessage(globals.hwndStBar, WM_SETFONT, (WPARAM)globals.hUiFont, TRUE);
	twpng_UpdateThemeBrushes();

	GetClientRect(hwnd,&r);

	globals.hwndMainList=CreateWindowEx(0,
		WC_LISTVIEW, _T("main listview"),
		WS_VISIBLE|WS_CHILD|WS_BORDER|LVS_REPORT|LVS_SHOWSELALWAYS|LVS_NOSORTHEADER,
		r.left+12, r.top+TWPNG_TOP_MARGIN, r.right-r.left-24,
		r.bottom-r.top-globals.stbar_height-TWPNG_TOP_MARGIN-12,
		hwnd,NULL,globals.hInst,NULL);
	if(!globals.hwndMainList) {
		mesg(MSG_S,_T("Cannot create listview control"));
		return 0;
	}

	style=ListView_GetExtendedListViewStyle(globals.hwndMainList);
	ListView_SetExtendedListViewStyle(globals.hwndMainList,style|LVS_EX_FULLROWSELECT|0x00010000|LVS_EX_HEADERDRAGDROP);
	SendMessage(globals.hwndMainList,WM_SETFONT,(WPARAM)globals.hUiFont,TRUE);
	SetWindowSubclass(globals.hwndMainList, MainListSubclassProc, 1, 0);
	twpng_ThemeMainControls();

	globals.hListRowImageList=ImageList_Create(1,28,ILC_COLOR32,1,1);
	if(globals.hListRowImageList) {
		ListView_SetImageList(globals.hwndMainList,(HIMAGELIST)globals.hListRowImageList,LVSIL_SMALL);
	}

	ZeroMemory((void*)&lvc, sizeof(LV_COLUMN));
	lvc.mask= LVCF_FMT | LVCF_TEXT | LVCF_WIDTH;
	lvc.cchTextMax=0;
	lvc.iSubItem=0;

	StringCbCopy(textbuf, sizeof(textbuf), _T("Chunk"));
	lvc.pszText=textbuf;
	lvc.cx= globals.window_prefs.column_width[0];
	lvc.fmt= LVCFMT_LEFT;
	ListView_InsertColumn(globals.hwndMainList,0,&lvc);

	StringCbCopy(textbuf, sizeof(textbuf), _T("Length"));
	lvc.pszText=textbuf;
	lvc.cx= globals.window_prefs.column_width[1];
	lvc.fmt= LVCFMT_RIGHT;
	ListView_InsertColumn(globals.hwndMainList,1,&lvc);

	StringCbCopy(textbuf, sizeof(textbuf), _T("CRC"));
	lvc.pszText=textbuf;
	lvc.cx= globals.window_prefs.column_width[2];
	lvc.fmt= LVCFMT_LEFT;
	ListView_InsertColumn(globals.hwndMainList,2,&lvc);

	StringCbCopy(textbuf, sizeof(textbuf), _T("Attributes"));
	lvc.pszText=textbuf;
	lvc.cx= globals.window_prefs.column_width[3];
	lvc.fmt= LVCFMT_LEFT;
	ListView_InsertColumn(globals.hwndMainList,3,&lvc);

	StringCbCopy(textbuf, sizeof(textbuf), _T("Contents"));
	lvc.pszText=textbuf;
	lvc.cx= globals.window_prefs.column_width[4];
	lvc.fmt= LVCFMT_LEFT;
	ListView_InsertColumn(globals.hwndMainList,4,&lvc);

	LayoutMainWindows(hwnd);
	return 1;
}

#define TWPNG_TPARAMS_EMPTY               0
#define TWPNG_TPARAMS_VIEWER_NOREPLACE    1
#define TWPNG_TPARAMS_VIEWER_REPLACE      2
#define TWPNG_TPARAMS_FILTER              3

// Evaluates a tool's "param" string.
// Returns the first of these which applies:
//   TWPNG_TPARAMS_FILTER           if s contains %2
//   TWPNG_TPARAMS_VIEWER_REPLACE   if s contains %1
//   TWPNG_TPARAMS_VIEWER_NOREPLACE if s is nonempty
//   TWPNG_TPARAMS_EMPTY            if s is empty
static int check_tool_param_string(const TCHAR *s)
{
	int i;
	int found_p1=0;
	int found_p2=0;
	int nonempty=0;
	TCHAR prevchar = ' ';

	for(i=0;s[i];i++) {
		if(s[i]=='1') {
			if(prevchar=='%') found_p1=1;
		}
		else if(s[i]=='2') {
			if(prevchar=='%') found_p2=1;
		}
		if(s[i]!=' ') nonempty=1;
		prevchar=s[i];
	}
	if(!nonempty) return TWPNG_TPARAMS_EMPTY;
	if(found_p2) return TWPNG_TPARAMS_FILTER;
	if(found_p1) return TWPNG_TPARAMS_VIEWER_REPLACE;
	return TWPNG_TPARAMS_VIEWER_NOREPLACE;
}

static void twpng_ReplaceParams(const TCHAR *src,
	TCHAR *dst, int dstlen, const TCHAR *param1, const TCHAR *param2)
{
	int srcpos,dstpos;
	int param1len, param2len;
	TCHAR prevchar=' ';

	param1len = lstrlen(param1);
	param2len = lstrlen(param2);

	dstpos=0;
	for(srcpos=0;src[srcpos];srcpos++) {
		if(dstpos>=dstlen-10) goto done;
		if(src[srcpos]=='1' && prevchar=='%') {
			dstpos--; // overwrite the '%'
			if(dstpos+param1len<dstlen-10) {
				StringCchCopy(&dst[dstpos],dstlen,param1);
				dstpos+=param1len;
			}
		}
		else if(src[srcpos]=='2' && prevchar=='%') {
			dstpos--;
			if(dstpos+param2len<dstlen-10) {
				StringCchCopy(&dst[dstpos],dstlen,param2);
				dstpos+=param2len;
			}
		}
		else {
			dst[dstpos++]=src[srcpos];
		}
		prevchar=src[srcpos];
	}
done:
	dst[dstpos]='\0';

}

static int twpng_FileExists(const TCHAR *fn)
{
	DWORD ret = GetFileAttributes(fn);
	return !(ret==INVALID_FILE_ATTRIBUTES);
}

// Returns 0 on failure, nonzero on apparent success.
static int twpng_RunFilter(struct tools_t_struct *t, const TCHAR *infn, const TCHAR *outfn)
{
	TCHAR parambuf1[1000];
	TCHAR parambuf2[1500];
	const TCHAR *app;
	BOOL b;
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	DWORD ret;
	int retval=0;

	if(lstrlen(t->cmd)) {
		app=t->cmd;
		twpng_ReplaceParams(t->params,parambuf1,1000,infn,outfn);
		StringCbPrintf(parambuf2,sizeof(parambuf2),_T("\"%s\" %s"),app,parambuf1);
	}
	else {
		app=NULL;
		twpng_ReplaceParams(t->params,parambuf2,1000,infn,outfn);
	}

	ZeroMemory(&si,sizeof(STARTUPINFO));
	ZeroMemory(&pi,sizeof(PROCESS_INFORMATION));
	si.cb = sizeof(STARTUPINFO);
	b = CreateProcess(app,parambuf2,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi);
	if(b) {
		ret = WaitForSingleObject(pi.hProcess,INFINITE);
		retval=1;
	}

	// Are we supposed to close these even if CreateProcess failed?
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return retval;
}

// n is the tool number (0-5)
static void RunTool(int n)
{
	TCHAR tmpdirname[MAX_PATH+2];
	TCHAR tmpname[MAX_PATH+2];
	TCHAR filt_infn[MAX_PATH+2];
	TCHAR filt_outfn[MAX_PATH+2];
	TCHAR parambuf[1000];
	HINSTANCE hInst;
	unsigned __int64 h;
	int ret;
	int paramstype;
	const TCHAR *ext;

	if(n<0 || n>=TWPNG_NUMTOOLS) return;
	if(!lstrlen(globals.tools[n].name)) {
		mesg(MSG_E,_T("Tool %d not defined"),n+1);
		return;
	}

	// Figure out what kind of tool this is, etc.
	paramstype=check_tool_param_string(globals.tools[n].params);

	SetCurrentDirectory(globals.home_dir);

	if(!GetTempPath(MAX_PATH,tmpdirname)) {
		//mesg(MSG_S,"Can't find a temp directory");
		//return;
		StringCchCopy(tmpdirname,MAX_PATH+2,_T(""));
	}

	// make sure pathname ends in a backslash or is empty
	if(lstrlen(tmpdirname)) {
		if(tmpdirname[lstrlen(tmpdirname)-1] != '\\') StringCchCat(tmpdirname,MAX_PATH+2,_T("\\"));
	}

	if(png->m_imgtype==IMG_MNG) { ext = _T("mng"); }
	else if(png->m_imgtype==IMG_JNG) { ext = _T("jng"); }
	else { ext = _T("png"); }

	if(paramstype==TWPNG_TPARAMS_FILTER) {
		Png *newpng;

		StringCchPrintf(filt_infn,MAX_PATH+2,_T("%stwptemp1.%s"),tmpdirname,ext);
		StringCchPrintf(filt_outfn,MAX_PATH+2,_T("%stwptemp2.%s"),tmpdirname,ext);

		if(!png->write_file(filt_infn)) {
			goto done;
		}

		// Make sure we don't get confused by a preexisting output file.
		DeleteFile(filt_outfn);

		ret=twpng_RunFilter(&globals.tools[n],filt_infn,filt_outfn);
		DeleteFile(filt_infn);
		if(!ret) {
			mesg(MSG_E,_T("Cannot execute filter tool"));
			DeleteFile(filt_outfn);
			goto done;
		}

		if(!twpng_FileExists(filt_outfn)){
			mesg(MSG_E,_T("Filter failed to produce an output file"));
			goto done;
		}

		newpng = new Png(filt_outfn,png->m_filename);
		if(newpng->m_valid) {
			// Replace current Png object with new one.
			if(png) delete png;
			png = newpng;

			png->fill_listbox(globals.hwndMainList);
			png->modified();
		}
		else {
			mesg(MSG_E,_T("Filter produced an invalid output file"));
			delete newpng;
		}

		DeleteFile(filt_outfn);
	}
	else { // Tool is a viewer
		StringCchPrintf(tmpname  ,MAX_PATH+2,_T("%stemp$$$$.%s"),tmpdirname,ext);
		if(!png->write_file(tmpname)) {
			goto done;
		}

		if(lstrlen(globals.tools[n].cmd)) {
			if(paramstype==TWPNG_TPARAMS_VIEWER_REPLACE) {
				// Replace "%1" with tmpname
				twpng_ReplaceParams(globals.tools[n].params,parambuf,1000,tmpname,_T(""));
			}
			else if(paramstype==TWPNG_TPARAMS_VIEWER_NOREPLACE) {
				// params doesn't contain '%1': Append tmpname to it.
				StringCbPrintf(parambuf,sizeof(parambuf),_T("%s %s"),globals.tools[n].params,tmpname);
			}
			else { // paramstype==TWPNG_TPARAMS_EMPTY
				// params is empty: Use tmpname as the only param.
				StringCbCopy(parambuf,sizeof(parambuf),tmpname);
			}

			hInst = ShellExecute(globals.hwndMain,_T("open"),globals.tools[n].cmd,parambuf,NULL,SW_SHOWNORMAL);
			h = (unsigned __int64)hInst;
			if(h <= 32) {
				mesg(MSG_E,_T("Cannot execute tool (error %u)"), (unsigned int)h);
			}
		}
		else {
			// If no application name was given, use the default PNG opener.
			hInst = ShellExecute(globals.hwndMain,_T("open"),tmpname,NULL,NULL,SW_SHOWNORMAL);
			h = (unsigned __int64)hInst;
			if(h <= 32) {
				mesg(MSG_E,_T("Cannot execute tool (error %u)"), (unsigned int)h);
			}
		}
	}

done:
	SetCurrentDirectory(globals.orig_dir);
}

// hwnd is the window to attach the menu to, x,y is the mouse position
static void ContextMenu(HWND hwnd, int x, int y)
{
	int sel;
	int cmd;
	HMENU menu;
	int selected_chunk_index=0;
	int can_edit_selected_chunk=0;

	if(!png) return;

	sel=ListView_GetSelectedCount(globals.hwndMainList);
	if(sel<1) return;

	if(sel==1) {
		// Exactly one chunk is currently selected.
		selected_chunk_index=GetLVSelection(globals.hwndMainList);
		if(selected_chunk_index>=0 && selected_chunk_index<png->m_num_chunks) {
			if(png->chunk[selected_chunk_index]->can_edit()) {
				can_edit_selected_chunk=1;
			}
		}
	}

	if(x == -1 && y == -1) {
		// Special case; most likely the user pressed the Menu key.
		POINT tmppt;

		ZeroMemory(&tmppt, sizeof(POINT));
		tmppt.x = 4;
		tmppt.y = 4;
		ClientToScreen(globals.hwndMainList,&tmppt);
		x = tmppt.x;
		y = tmppt.y;
	}

	menu=CreatePopupMenu();

	// FIXME: Don't enable "Edit" for chunks that aren't editable.
	AppendMenu(menu,can_edit_selected_chunk?MF_ENABLED:MF_GRAYED,ID_EDITCHUNK,_T("&Edit Chunk..."));
	AppendMenu(menu,MF_ENABLED,ID_DELCHUNK,_T("&Delete"));
	AppendMenu(menu,MF_ENABLED,ID_MOVEUP,_T("Move &Up"));
	AppendMenu(menu,MF_ENABLED,ID_MOVEDOWN,_T("Mo&ve Down"));
	AppendMenu(menu,MF_SEPARATOR,0,NULL);
	AppendMenu(menu,MF_ENABLED,ID_CUT,_T("Cu&t"));
	AppendMenu(menu,MF_ENABLED,ID_COPY,_T("&Copy"));
	AppendMenu(menu,MF_ENABLED,ID_PASTE,_T("&Paste"));

	cmd=TrackPopupMenuEx(menu, TPM_LEFTALIGN|TPM_TOPALIGN|TPM_NONOTIFY|TPM_RETURNCMD|
		TPM_RIGHTBUTTON,x,y,hwnd,NULL);
	DestroyMenu(menu);

	switch(cmd) {
	case ID_EDITCHUNK:    png->edit_chunk(selected_chunk_index);  return;
	case ID_DELCHUNK:     DeleteChunks();    return;
	case ID_MOVEUP:       MoveChunkUp();     return;
	case ID_MOVEDOWN:     MoveChunkDown();   return;
	case ID_COPY:   CopyChunks();    return;
	case ID_CUT:    CutChunks();     return;
	case ID_PASTE:  PasteChunks();   return;
	}
}

static int OkToClosePNG()
{
	TCHAR buf[500];
	int x,ret;

	if(!png) return 1;

	if(png->m_dirty) {
		SetForegroundWindow(globals.hwndMain);
		StringCchPrintf(buf,500,_T("Save changes to %s?"),png->m_filename);
		x=twpng_MessageBox(globals.hwndMain,buf,_T("TweakPNG"),MB_YESNOCANCEL|MB_ICONWARNING|MB_DEFBUTTON3);
		if(x==IDYES) {
			ret=SavePng(globals.hwndMain);
			return ret?1:0;
		}
		else if(x==IDNO) {
			return 1;
		}
		else if(x==IDCANCEL) {
			return 0;
		}
		return 0;
	}
	else {
		return 1; // not modified
	}
}

void update_viewer()
{
#ifdef TWPNG_SUPPORT_VIEWER
	if(!g_viewer) return;

	if(!png) {
		g_viewer->Update(NULL);
		return;
	}

	if(png->m_num_chunks < 1) {
		g_viewer->Update(NULL);
		return;
	}

	g_viewer->Update(png);
#endif
}

struct prefs_dlg_ctx {
	int remember_menusetting;
	int remember_aimenu;
};

static LRESULT CALLBACK WndProcMain(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	WORD id;
	RECT r;
	int i;
	TCHAR buf[150];

	id=LOWORD(wParam);

	switch(msg) {
	case WM_UAHDRAWMENU:
	case WM_UAHDRAWMENUITEM:
	case WM_NCACTIVATE:
	case WM_NCPAINT:
		{
			LRESULT dmResult = 0;
			if(twpng_HandleDarkMenuMsg(hwnd, msg, wParam, lParam, &dmResult)) return dmResult;
		}
		break;

	case WM_ERASEBKGND:
		GetClientRect(hwnd,&r);
		FillRect((HDC)wParam,&r,globals.hUiBgBrush ? globals.hUiBgBrush : GetSysColorBrush(COLOR_WINDOW));
		return 1;

	case WM_CREATE:
		globals.hwndMain=hwnd;
		twpng_ApplyModernWindowStyle(hwnd);

		// create the listview control, statusbar, etc.
		if(!CreateMainWindows(hwnd)) return -1;  // abort program

		if(lstrlen(globals.file_from_cmdline)) {
			OpenPngByName(globals.file_from_cmdline);
		}
		if(globals.aiparams_only) {
			// The viewer is the whole app; closing it closes the hidden main window.
			TCHAR *text = NULL, extra[200] = _T("");
			if(png || g_foreignFn[0]) twpng_GetDocAIText(&text, extra, 200);
			if(!(png || g_foreignFn[0]) || !twpng_ShowAIParams(text, extra, globals.file_from_cmdline, TRUE))
				PostMessage(hwnd, WM_CLOSE, 0, 0);
			free(text);
			return 0;
		}
		if(globals.autoopen_viewer) {
			PostMessage(hwnd,WM_COMMAND,ID_IMGVIEWER,(LPARAM)0);
		}
		return 0;

	case WM_CLOSE:
		// close requested e.g. by user clicking on 'x' icon.
		if(OkToClosePNG()) break;
		return 0; // handle this message --> will not be closed

	case WM_DESTROY:
		if(globals.timer_set) {
			KillTimer(hwnd,1);
			globals.timer_set=0;
		}
		SaveSettings();
		if(png) delete png;
		png=NULL;
		if(globals.hListRowImageList) {
			ImageList_Destroy((HIMAGELIST)globals.hListRowImageList);
			globals.hListRowImageList=NULL;
		}
		if(globals.hUiFont) {
			DeleteObject(globals.hUiFont);
			globals.hUiFont=NULL;
		}
		if(globals.hUiBgBrush) {
			DeleteObject(globals.hUiBgBrush);
			globals.hUiBgBrush=NULL;
		}
		if(globals.hDarkEditBrush) {
			DeleteObject(globals.hDarkEditBrush);
			globals.hDarkEditBrush=NULL;
		}
		PostQuitMessage(0);
		return 0;

	case WM_SETFOCUS:
		if(globals.hwndMainList) {
			SetFocus(globals.hwndMainList);
			return 0;
		}
		break;

	case WM_CONTEXTMENU:
		ContextMenu(hwnd,((int)(short)LOWORD(lParam)),((int)(short)HIWORD(lParam)));
		return 0;

	case WM_SIZE:
		// this is probably not the right way to do this...
		LayoutMainWindows(hwnd);
		return 0;

	case WM_CTLCOLORSTATIC:
		if((HWND)lParam==globals.hwndStBar) {
			if(globals.hUiBgBrush) {
				SetBkColor((HDC)wParam,RGB(32,32,32));
				SetTextColor((HDC)wParam,RGB(255,255,255));
				return (LRESULT)globals.hUiBgBrush;
			}
			SetBkColor((HDC)wParam,GetSysColor(COLOR_WINDOW));
			SetTextColor((HDC)wParam,GetSysColor(COLOR_WINDOWTEXT));
			return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
		}
		return 0;

	case WM_APP_SIBLINGS:
		twpng_SibListingDone((struct twpng_sibjob*)lParam);
		if(g_hwndAIParams && g_aiParamsCtx) twpng_UpdateAINav(g_hwndAIParams, g_aiParamsCtx);
		return 0;

	case WM_APP_THUMB:
		twpng_ThumbDone((struct twpng_thumbjob*)lParam);
		return 0;

	case WM_SETTINGCHANGE:
		// Follow the Windows light/dark switch when the theme is "System default".
		if(globals.theme_pref==TWPNG_THEME_SYSTEM && lParam &&
		   !lstrcmpi((LPCTSTR)lParam, _T("ImmersiveColorSet"))) {
			int was = globals.dark;
			twpng_ResolveTheme();
			if(was!=globals.dark) twpng_ApplyTheme();
		}
		break;

	case WM_DPICHANGED:
		{
			UINT newDpi = HIWORD(wParam);
			RECT *prc = (RECT*)lParam;
			SetWindowPos(hwnd, NULL, prc->left, prc->top,
				prc->right - prc->left, prc->bottom - prc->top,
				SWP_NOZORDER | SWP_NOACTIVATE);
			twpng_CreateUiFont(newDpi);
			if(globals.hwndMainList) SendMessage(globals.hwndMainList, WM_SETFONT, (WPARAM)globals.hUiFont, TRUE);
			if(globals.hwndStBar) SendMessage(globals.hwndStBar, WM_SETFONT, (WPARAM)globals.hUiFont, TRUE);
			HWND hChild = GetWindow(hwnd, GW_CHILD);
			while(hChild) {
				if(hChild != globals.hwndMainList && hChild != globals.hwndStBar) {
					SendMessage(hChild, WM_SETFONT, (WPARAM)globals.hUiFont, TRUE);
					InvalidateRect(hChild, NULL, TRUE);
				}
				hChild = GetWindow(hChild, GW_HWNDNEXT);
			}
			LayoutMainWindows(hwnd);
			InvalidateRect(hwnd, NULL, TRUE);
		}
		return 0;

	case WM_DROPFILES:
		DroppedFiles((HDROP)wParam);
		return 0;

	case WM_TIMER:
		if(globals.timer_set) {
			KillTimer(hwnd,1);
			globals.timer_set=0;
		}
		update_viewer();
		return 0;

	case WM_INITMENU:
		{
			HMENU m, mtools;
			UINT x;
			int sel;

			// commands requiring an open file
			static const UINT cmdlist1[] = {ID_SAVE,ID_SAVEAS,
				ID_NEWTEXT,ID_NEWBKGD,ID_NEWGAMA,ID_NEWIEND,ID_NEWPHYS,
				ID_NEWIHDR,
				ID_NEWSRGB,ID_NEWTIME,ID_NEWCHRM,ID_NEWTRNS,ID_NEWSBIT,
				ID_NEWPLTE,ID_NEWSTER,ID_NEWACTL,ID_NEWFCTL,ID_NEWOFFS,
				ID_NEWSCAL,ID_NEWVPAG,
				ID_COMBINEALLIDAT,
				ID_IMPORTCHUNK,ID_IMPORTICCPROF,ID_SIGNATURE,ID_CHECKPNG,
				ID_STRIPAIMETA,ID_VIEWAIPARAMS,
				ID_TOOL_1,ID_TOOL_2,ID_TOOL_3,ID_TOOL_4,ID_TOOL_5,ID_TOOL_6,
				0};
			// commands requiring exactly 1 selected chunk
			static const UINT cmdlist2[] = {ID_EDITCHUNK,
				ID_SPLITIDAT,ID_EXPORTCHUNK,
				0};
			// requiring 1 or more selected chunks
			static const UINT cmdlist3[] = {ID_DELCHUNK,ID_COPY,ID_CUT,ID_MOVEUP,ID_MOVEDOWN,
				0};

			static const UINT toolsi[] = {ID_TOOL_1,ID_TOOL_2,ID_TOOL_3,ID_TOOL_4,
				ID_TOOL_5,ID_TOOL_6};

			m=(HMENU)wParam;

			mtools=CreatePopupMenu();
#ifdef TWPNG_SUPPORT_VIEWER
			AppendMenu(mtools,MF_STRING|MF_ENABLED,ID_IMGVIEWER,_T("&Image Viewer\tF7"));
			AppendMenu(mtools,MF_SEPARATOR,0,NULL);
#endif
			AppendMenu(mtools,MF_STRING|MF_ENABLED,ID_VIEWAIPARAMS,_T("&View AI Parameters..."));
			AppendMenu(mtools,MF_STRING|MF_ENABLED,ID_READAIFROMFILE,_T("Read AI parameters from &File..."));
			AppendMenu(mtools,MF_STRING|MF_ENABLED,ID_COMPAREAI,_T("&Compare AI Parameters..."));
			AppendMenu(mtools,MF_STRING|MF_ENABLED,ID_AIFOLDER,_T("Folder &Overview..."));
			AppendMenu(mtools,MF_SEPARATOR,0,NULL);
			for(i=0;i<TWPNG_NUMTOOLS;i++) {
				if(lstrlen(globals.tools[i].name)) {
					StringCchPrintf(buf,150,_T("&%d  %s\tCtrl+%d"),i+1,globals.tools[i].name,i+1);
					AppendMenu(mtools,MF_STRING|MF_ENABLED,toolsi[i],buf);
				}
			}

			// the Tools menu has an index of 5
			ModifyMenu(m,4,MF_POPUP|MF_BYPOSITION|MF_ENABLED,(UINT_PTR)mtools,_T("&Tools"));

			sel=0;
			if(png) {
				if(globals.hwndMainList) {
					sel=ListView_GetSelectedCount(globals.hwndMainList);
				}
			}

			x= MF_BYCOMMAND | (png?MF_ENABLED:MF_GRAYED);
			for(i=0;cmdlist1[i];i++) {
				EnableMenuItem(m,cmdlist1[i],x);
			}

			// "View AI Parameters" also works for a loaded JPEG/WebP (no PNG document).
			if(!png && g_foreignFn[0]) {
				EnableMenuItem(m,ID_VIEWAIPARAMS,MF_BYCOMMAND|MF_ENABLED);
				EnableMenuItem(m,ID_STRIPAIMETA,MF_BYCOMMAND|MF_ENABLED);
			}

			CheckMenuRadioItem(m, ID_THEME_SYSTEM, ID_THEME_DARK,
				ID_THEME_SYSTEM + globals.theme_pref, MF_BYCOMMAND);

			// Close Document is enabled for either a PNG or a foreign (JPEG/WebP) file.
			EnableMenuItem(m,ID_CLOSEDOCUMENT,MF_BYCOMMAND |
				((png || g_foreignFn[0]) ? MF_ENABLED : MF_GRAYED));

			x= MF_BYCOMMAND | ( (sel==1)?MF_ENABLED:MF_GRAYED );
			for(i=0;cmdlist2[i];i++) {
				EnableMenuItem(m,cmdlist2[i],x);
			}

			x= MF_BYCOMMAND | ( (sel>=1)?MF_ENABLED:MF_GRAYED );
			for(i=0;cmdlist3[i];i++) {
				EnableMenuItem(m,cmdlist3[i],x);
			}

			// commands with unusual rules for enabling
			EnableMenuItem(m,ID_REOPEN, MF_BYCOMMAND |
				((png && png->m_named)?MF_ENABLED:MF_GRAYED) );

			EnableMenuItem(m,ID_COMBINEIDAT, MF_BYCOMMAND |
				((sel>1)?MF_ENABLED:MF_GRAYED) );

			x= MF_BYCOMMAND;
			if((png) && IsClipboardFormatAvailable(globals.pngchunk_cf)) {
				x |= MF_ENABLED;
			}
			else {
				x |= MF_GRAYED;
			}
			EnableMenuItem(m,ID_PASTE,x);

			CheckMenuItem(m,ID_IMGVIEWER,MF_BYCOMMAND|
				(g_viewer?MF_CHECKED:MF_UNCHECKED));
			return 0;
		}

	case WM_NOTIFY:
		{
			LPNMHDR  lpnmh = (LPNMHDR) lParam;
			if(!png) break;
			switch(lpnmh->code) {
			case NM_DBLCLK:
			case NM_RETURN:
				if(lpnmh->hwndFrom==globals.hwndMainList) {
					DblClickOnList();
					return 0;
				}
			}
			break;
		}

	case WM_COMMAND:
		// commands always available
		switch(id) {
#ifdef TWPNG_SUPPORT_VIEWER
		case ID_IMGVIEWER:
			if(g_viewer) {
				g_viewer->Close();
				if(g_viewer) { delete g_viewer; g_viewer=NULL; }
				globals.autoopen_viewer=0;
			}
			else {
				const TCHAR *fn;
				if(png && png->m_named) fn=png->m_filename;
				else fn=NULL;
				g_viewer = new Viewer(globals.hwndMain,fn);
				update_viewer();
				globals.autoopen_viewer=1;
			}
			return 0;
		case ID_UPDATEVIEWER:
			update_viewer();
			return 0;
#endif
		case ID_EXIT:
			if(OkToClosePNG()) DestroyWindow(hwnd);
			return 0;
		case ID_OPEN:
			if(OkToClosePNG()) OpenPngFromMenu(hwnd);
			return 0;
		case ID_NEW:
			if(OkToClosePNG()) NewPng();
			return 0;

		case ID_REOPEN:
			ReopenPngDocument();
			return 0;

		case ID_CLOSEDOCUMENT:
			// TODO: There's too much redundant code here.
			if(png) {
				if(OkToClosePNG()) {
					ClosePngDocument();
				}
			}
			else if(g_foreignFn[0]) {
				// No PNG document, just a foreign (JPEG/WebP) file — nothing to prompt
				// about (we never modify the file), so close immediately.
				ClosePngDocument();
			}
			return 0;

		case ID_PREFS:
			globals.dlgs_open++;
			struct prefs_dlg_ctx prctx;
			ZeroMemory((void*)&prctx,sizeof(struct prefs_dlg_ctx));
			DialogBoxParam(globals.hInst,_T("DLG_PREFS"),hwnd,DlgProcPrefs,(LPARAM)&prctx);
			globals.dlgs_open--;
			return 0;

		case ID_EDITTOOLS:
			globals.dlgs_open++;
			DialogBox(globals.hInst,_T("DLG_TOOLS"),globals.hwndMain,DlgProcTools);
			globals.dlgs_open--;
			return 0;

		case ID_STRIPAIMETA:
			StripAIMetadata();
			return 0;
		case ID_THEME_SYSTEM:
		case ID_THEME_LIGHT:
		case ID_THEME_DARK:
			globals.theme_pref = id - ID_THEME_SYSTEM;
			SaveSettings();
			twpng_ApplyTheme();
			return 0;
		case ID_COMPAREAI:
			twpng_CompareAIParams(hwnd);
			return 0;
		case ID_AIFOLDER:
			{
				TCHAR d[MAX_PATH];
				twpng_DocFolder(d);
				twpng_ShowFolderOverview(d, FALSE);
			}
			return 0;
		case ID_READAIFROMFILE:
			twpng_ReadAIFromFile(hwnd);
			return 0;

		// View AI Parameters works both for an open PNG and for a loaded JPEG/WebP
		// (g_foreignFn). twpng_ViewAIParams handles both cases internally.
		case ID_VIEWAIPARAMS:
			twpng_ViewAIParams(hwnd);
			return 0;

		case ID_ABOUT:
			globals.dlgs_open++;
			DialogBox(globals.hInst,_T("DLG_ABOUT"),hwnd,DlgProcAbout);
			globals.dlgs_open--;
			return 0;

		case ID_HELPCONTENTS:
			SetCurrentDirectory(globals.home_dir);
			if((unsigned __int64)ShellExecute(globals.hwndMain,_T("open"),_T("..\\tweakpng.txt"),
				NULL,NULL,SW_SHOWNORMAL)<=32)
			{
				if((unsigned __int64)ShellExecute(globals.hwndMain,_T("open"),_T("tweakpng.txt"),
					NULL,NULL,SW_SHOWNORMAL)<=32)
				{
					mesg(MSG_E,_T("Cannot display help file"));
				}
			}
			SetCurrentDirectory(globals.orig_dir);
			return 0;
		case ID_SWITCHWINDOW:
			if(g_viewer) {
				if(GetFocus()==g_viewer->m_hwndViewer) {
					SetFocus(globals.hwndMain);
				}
				else {
					SetFocus(g_viewer->m_hwndViewer);
				}
			}
			return 0;
		}

		// commands only available if we have an image loaded
		if(png) switch(id) {
		case ID_SIGNATURE:
			globals.dlgs_open++;
			DialogBoxParam(globals.hInst,_T("DLG_SIGNATURE"),globals.hwndMain,DlgProcSetSig,
				(LPARAM)png);
			globals.dlgs_open--;
			return 0;

		case ID_SAVE:         SavePng(hwnd);     return 0;
		case ID_SAVEAS:       SavePngAs(hwnd);   return 0;
		case ID_CHECKPNG:     png->check_validity(0);  return 0;

		case ID_EDITCHUNK:    DblClickOnList();  return 0;
		case ID_DELCHUNK:     DeleteChunks();    return 0;
		case ID_MOVEUP:       MoveChunkUp();     return 0;
		case ID_MOVEDOWN:     MoveChunkDown();   return 0;

		case ID_NEWACTL: png->new_chunk(CHUNK_acTL); return 0;
		case ID_NEWBKGD: png->new_chunk(CHUNK_bKGD); return 0;
		case ID_NEWCHRM: png->new_chunk(CHUNK_cHRM); return 0;
		case ID_NEWFCTL: png->new_chunk(CHUNK_fcTL); return 0;
		case ID_NEWGAMA: png->new_chunk(CHUNK_gAMA); return 0;
		case ID_NEWIEND: png->new_chunk(CHUNK_IEND); return 0;
		case ID_NEWIHDR: png->new_chunk(CHUNK_IHDR); return 0;
		case ID_NEWOFFS: png->new_chunk(CHUNK_oFFs); return 0;
		case ID_NEWPHYS: png->new_chunk(CHUNK_pHYs); return 0;
		case ID_NEWPLTE: png->new_chunk(CHUNK_PLTE); return 0;
		case ID_NEWSBIT: png->new_chunk(CHUNK_sBIT); return 0;
		case ID_NEWSCAL: png->new_chunk(CHUNK_sCAL); return 0;
		case ID_NEWSRGB: png->new_chunk(CHUNK_sRGB); return 0;
		case ID_NEWSTER: png->new_chunk(CHUNK_sTER); return 0;
		case ID_NEWTEXT: png->new_chunk(CHUNK_tEXt); return 0;
		case ID_NEWTIME: png->new_chunk(CHUNK_tIME); return 0;
		case ID_NEWTRNS: png->new_chunk(CHUNK_tRNS); return 0;
		case ID_NEWVPAG: png->new_chunk(CHUNK_vpAg); return 0;

		case ID_CUT:    CutChunks();     return 0;
		case ID_COPY:   CopyChunks();    return 0;
		case ID_PASTE:  PasteChunks();   return 0;
		case ID_SELECTALL:
			twpng_SetLVSelection(globals.hwndMainList,0,png->m_num_chunks);
			return 0;

		case ID_SPLITIDAT:    SplitIDAT();       return 0;
		case ID_COMBINEIDAT:     CombineIDAT_selected(); return 0;
		case ID_COMBINEALLIDAT:  CombineIDAT_all();      return 0;

		case ID_IMPORTCHUNK:  ImportChunk();     return 0;
		case ID_EXPORTCHUNK:  ExportChunk();     return 0;
		case ID_IMPORTICCPROF: ImportICCProfile(png); return 0;

		case ID_TOOL_1: RunTool(0); return 0;
		case ID_TOOL_2: RunTool(1); return 0;
		case ID_TOOL_3: RunTool(2); return 0;
		case ID_TOOL_4: RunTool(3); return 0;
		case ID_TOOL_5: RunTool(4); return 0;
		case ID_TOOL_6: RunTool(5); return 0;

		}

	}
	return (DefWindowProc(hwnd, msg, wParam, lParam));
}

static void twpng_HandleAboutInitDialog(HWND hwnd)
{
	TCHAR buf[4000],buf1[1000];
	TCHAR buf2[200];

	StringCbPrintf(buf1,sizeof(buf1),_T("Version %s %s %s %s %s %s %d-bit"),TWEAKPNG_VER_STRING,
		SYM_MIDDOT,TWEAKPNG_FORK_VER_STRING,SYM_MIDDOT,_T(__DATE__),SYM_MIDDOT,(int)(sizeof(void*)*8));

	//StringCbPrintf(buf2,sizeof(buf2),_T(" %s %s"),SYM_MIDDOT,_T(__DATE__));
	//StringCbCat(buf1,sizeof(buf1),buf2);

#ifndef UNICODE
	StringCbPrintf(buf2,sizeof(buf2),_T(" %s non-Unicode"),SYM_MIDDOT);
	StringCbCat(buf1,sizeof(buf1),buf2);
#endif
#ifdef _DEBUG
	StringCbPrintf(buf2,sizeof(buf2),_T(" %s Debug build"),SYM_MIDDOT);
	StringCbCat(buf1,sizeof(buf1),buf2);
#endif

	StringCchPrintf(buf,4000,_T("TweakPNG %s A PNG image file manipulation utility\r\n\r\n%s\r\n")
		_T("Copyright ") SYM_COPYRIGHT _T(" %s by Jason Summers\r\n")
		_T("Website: <a href=\"%s\">%s</a>\r\n"),
		SYM_MIDDOT,buf1,
		TWEAKPNG_COPYRIGHT_DATE,globals.twpng_homepage,globals.twpng_homepage);

	StringCchCat(buf,4000,_T("\r\nWindows 11 Modern UI fork:\r\n")
		_T("Website: <a href=\"") TWEAKPNG_FORK_HOMEPAGE _T("\">") TWEAKPNG_FORK_HOMEPAGE _T("</a>\r\n"));

	StringCchCat(buf,4000,_T("\r\nThis program is distributed under the terms ")
		_T("of the GNU General Public License, version 3 or higher. Please read the file tweakpng.txt for more information.\r\n"));

#ifdef TWPNG_HAVE_ZLIB
	StringCchPrintf(buf1,1000,_T("\r\nUses zlib, version %s."),_T(ZLIB_VERSION));
	StringCchCat(buf,4000,buf1);
#else
	StringCchCat(buf,4000,_T("\r\nCompiled without compression/decompression support."));
#endif

#ifdef TWPNG_SUPPORT_VIEWER
	twpng_get_libpng_version(buf2,200);
	StringCchPrintf(buf1,1000,_T("\r\nUses libpng, version %s."),buf2);
	StringCchCat(buf,4000,buf1);
#else
	StringCchCat(buf,4000,_T("\r\nCompiled without image viewing support."));
#endif
	SetDlgItemText(hwnd,IDC_ABOUTTEXT,buf);
}

// Acrylic backdrop behind the whole window: DWM draws the blur wherever the client area
// is black (alpha 0), so the frame is extended over the full client area.
static void twpng_ApplyAcrylic(HWND hwnd)
{
	typedef HRESULT (WINAPI *DwmSetWindowAttributeProc)(HWND,DWORD,LPCVOID,DWORD);
	typedef HRESULT (WINAPI *DwmExtendFrameIntoClientAreaProc)(HWND,const MARGINS*);
	HMODULE hDwm = LoadLibrary(_T("dwmapi.dll"));
	if(!hDwm) return;
	DwmSetWindowAttributeProc pSet =
		(DwmSetWindowAttributeProc)GetProcAddress(hDwm,"DwmSetWindowAttribute");
	DwmExtendFrameIntoClientAreaProc pExtend =
		(DwmExtendFrameIntoClientAreaProc)GetProcAddress(hDwm,"DwmExtendFrameIntoClientArea");
	if(pSet && pExtend) {
		int backdrop_type = 3;   // DWMSBT_TRANSIENTWINDOW (acrylic)
		pSet(hwnd,38,&backdrop_type,sizeof(backdrop_type));
		MARGINS m = { -1, -1, -1, -1 };
		pExtend(hwnd,&m);
	}
	FreeLibrary(hDwm);
}

// Light acrylic: GDI leaves alpha at 0, so dark text vanishes on the see-through client.
// Paint the control onto white in a buffer, then turn the white into transparency
// (premultiplied alpha), which keeps text, links and the icon visible over the blur.
static LRESULT CALLBACK GlassChildSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
	LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	(void)dwRefData;
	switch(msg) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hwnd, &ps);
			RECT rc;
			GetClientRect(hwnd, &rc);
			HDC mdc;
			HPAINTBUFFER pb = BeginBufferedPaint(hdc, &rc, BPBF_TOPDOWNDIB, NULL, &mdc);
			if(pb) {
				FillRect(mdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
				DefSubclassProc(hwnd, WM_PRINTCLIENT, (WPARAM)mdc, PRF_CLIENT);
				RGBQUAD *bits;
				int row;
				if(SUCCEEDED(GetBufferedPaintBits(pb, &bits, &row))) {
					for(int y=0; y<rc.bottom; y++) {
						RGBQUAD *q = bits + (size_t)y*row;
						for(int x=0; x<rc.right; x++, q++) {
							// pixel = color*a + white*(1-a); for colors with a 0 channel
							// (black text, blue links) the smallest channel is white*(1-a).
							BYTE mn = q->rgbRed < q->rgbGreen ? q->rgbRed : q->rgbGreen;
							if(q->rgbBlue < mn) mn = q->rgbBlue;
							q->rgbRed -= mn; q->rgbGreen -= mn; q->rgbBlue -= mn;
							q->rgbReserved = (BYTE)(255 - mn);
						}
					}
				}
				EndBufferedPaint(pb, TRUE);
			}
			EndPaint(hwnd, &ps);
			return 0;
		}
	case WM_NCDESTROY:
		RemoveWindowSubclass(hwnd, GlassChildSubclassProc, uIdSubclass);
		break;
	}
	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static BOOL CALLBACK twpng_GlassChild(HWND hChild, LPARAM lParam)
{
	(void)lParam;
	SetWindowSubclass(hChild, GlassChildSubclassProc, 1, 0);
	return TRUE;
}

static INT_PTR CALLBACK DlgProcAbout(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	WORD id;
	id=LOWORD(wParam);

	switch (msg) {
	case WM_INITDIALOG:
		twpng_InitDarkDialog(hwnd);
		twpng_ApplyAcrylic(hwnd);
		if(!globals.dark) EnumChildWindows(hwnd, twpng_GlassChild, 0);
		twpng_HandleAboutInitDialog(hwnd);
		return 1;
	case WM_CTLCOLORDLG:
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
		if(!globals.dark && msg!=WM_CTLCOLORDLG) {
			// Children paint on white, which GlassChildSubclassProc turns transparent.
			SetBkMode((HDC)wParam, TRANSPARENT);
			SetBkColor((HDC)wParam, RGB(255,255,255));
			SetTextColor((HDC)wParam, RGB(0,0,0));
			return (INT_PTR)GetStockObject(WHITE_BRUSH);
		}
		// Black is "transparent" on the extended frame, letting the acrylic show through.
		SetBkMode((HDC)wParam, TRANSPARENT);
		SetBkColor((HDC)wParam, RGB(0,0,0));
		SetTextColor((HDC)wParam, RGB(255,255,255));
		return (INT_PTR)GetStockObject(BLACK_BRUSH);
	case WM_NOTIFY:
		{
			LPNMHDR nmh = (LPNMHDR)lParam;
			if(nmh->idFrom==IDC_ABOUTTEXT && (nmh->code==NM_CLICK || nmh->code==NM_RETURN)) {
				PNMLINK pnml = (PNMLINK)lParam;
				if(pnml->item.szUrl[0]) {
					ShellExecuteW(hwnd, L"open", pnml->item.szUrl, NULL, NULL, SW_SHOWNORMAL);
				}
				return TRUE;
			}
		}
		break;
	case WM_COMMAND:
		switch(id) {
		case IDOK:
		case IDCANCEL:
			EndDialog(hwnd, 0);
			return 1;
		}
	}
	{
		INT_PTR darkResult = 0;
		if(twpng_HandleDlgDarkMsg(msg, wParam, lParam, &darkResult)) return darkResult;
	}
	return 0;
}

// Returns the "name" of files with a given extension.
// format = ".png", ".jng", ".mng"
// returns zero if does not exist
// caller supplies buf[500]
static int get_reg_name_for_ext(const TCHAR *format, TCHAR *logicalname, int logicalname_len)
{
	HKEY key1;
	LONG rv;
	int retval;
	DWORD datatype,datasize;
	TCHAR buf1[500];

	retval=0;
	key1=NULL;
	StringCchCopy(logicalname,logicalname_len,_T(""));

	// First check FileExts key. If a UserChoice key refers us to a logical
	// name, we'll use that name.
	// This does not seem right at all, but it's the only thing I've figured out
	// that seems to work.
	// This probably won't work for everyone, and it will stop working whenever
	// you change your default PNG application.
	StringCbPrintf(buf1,sizeof(buf1),_T("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice"),format);
	rv=RegOpenKeyEx(HKEY_CURRENT_USER,buf1,0,KEY_READ,&key1);
	if(rv==ERROR_SUCCESS) {
		datasize=sizeof(TCHAR)*logicalname_len;
		rv=RegQueryValueEx(key1,_T("Progid"),NULL,&datatype,(BYTE*)logicalname,&datasize);
		if(rv==ERROR_SUCCESS && datatype==REG_SZ) {
			retval = 1;
			RegCloseKey(key1); key1=NULL;
			goto done;
		}
		RegCloseKey(key1); key1=NULL;
	}

	// If UserChoice doesn't exist, check Classes key directly.
	StringCbPrintf(buf1,sizeof(buf1),_T("Software\\Classes\\%s"),format);
	rv=RegOpenKeyEx(HKEY_CURRENT_USER,buf1,0,KEY_READ,&key1);
	if(rv!=ERROR_SUCCESS) goto done;

	datasize=sizeof(TCHAR)*logicalname_len;
	rv=RegQueryValueEx(key1,NULL,NULL,&datatype,(BYTE*)logicalname,&datasize);
	if(rv!=ERROR_SUCCESS) goto done;
	if(datatype!=REG_SZ) goto done;
	if(datasize<sizeof(TCHAR)*2) goto done;
	RegCloseKey(key1); key1=NULL;
	retval=1;

done:
	if(key1) RegCloseKey(key1);
	return retval;
}

static int is_explorer_menu_set()
{
	HKEY key1;
	LONG rv;
	int retval;
	TCHAR logicalname[500];
	TCHAR buf[500];
	int x;


	retval=0;
	key1=NULL;

	x=get_reg_name_for_ext(_T(".png"),logicalname,500);
	if(!x) goto abort;

	StringCchPrintf(buf,500,_T("Software\\Classes\\%s\\shell\\tweakpng"),logicalname);
	// we only want to know if this key exists
	rv=RegOpenKeyEx(HKEY_CURRENT_USER,buf,0,KEY_READ,&key1);

	if(rv!=ERROR_SUCCESS) goto abort;

	retval=1;

abort:
	if(key1) RegCloseKey(key1);
	return retval;
}

static const TCHAR *fmts[] = { _T(".png"),_T(".mng"),_T(".jng") };
static const TCHAR *dflt_fmt_name[] = { _T("png_auto_file"),_T("mng_auto_file"),_T("jng_auto_file") };

// Add to Windows Explorer right-click menu for .[pmj]ng files.
static void add_to_explorer_menu()
{
	int i;
	int x;
	TCHAR logicalname[500];
	HKEY key1;
	DWORD disp;
	LONG rv;
	TCHAR buf1[500];
	TCHAR buf[500];
	TCHAR cmd[500];

	for(i=0;i<=2;i++) {
		x=get_reg_name_for_ext(fmts[i],logicalname,500);
		if(!x) {
			StringCchCopy(logicalname,500,dflt_fmt_name[i]);
			StringCbPrintf(buf1,sizeof(buf1),_T("Software\\Classes\\%s"),fmts[i]);
			rv=RegCreateKeyEx(HKEY_CURRENT_USER,buf1,0,NULL,
				REG_OPTION_NON_VOLATILE,KEY_WRITE,NULL,&key1,&disp);
			if(rv==ERROR_SUCCESS) {
				RegSetValueEx(key1,_T(""),0,REG_SZ,(BYTE*)logicalname,(DWORD)sizeof(TCHAR)*(1+lstrlen(logicalname)));
				RegCloseKey(key1);
			}
		}

		StringCbPrintf(buf1,sizeof(buf1),_T("Software\\Classes\\%s"),logicalname);
		rv=RegCreateKeyEx(HKEY_CURRENT_USER,buf1,0,NULL,
			REG_OPTION_NON_VOLATILE,KEY_WRITE,NULL,&key1,&disp);
		if(rv==ERROR_SUCCESS) {
			RegCloseKey(key1);
		}

		StringCchPrintf(buf,500,_T("Software\\Classes\\%s\\shell"),logicalname);
		rv=RegCreateKeyEx(HKEY_CURRENT_USER,buf,0,NULL,
			REG_OPTION_NON_VOLATILE,KEY_WRITE,NULL,&key1,&disp);
		if(rv==ERROR_SUCCESS) {
			RegCloseKey(key1);
		}

		StringCchPrintf(buf,500,_T("Software\\Classes\\%s\\shell\\tweakpng"),logicalname);
		rv=RegCreateKeyEx(HKEY_CURRENT_USER,buf,0,NULL,
			REG_OPTION_NON_VOLATILE,KEY_WRITE,NULL,&key1,&disp);
		if(rv==ERROR_SUCCESS) {
			// The name on the menu:
			StringCchCopy(buf,500,_T("TweakPNG"));
			RegSetValueEx(key1,_T(""),0,REG_SZ,(BYTE*)buf,(DWORD)sizeof(TCHAR)*(1+lstrlen(buf)));
			RegCloseKey(key1);
		}

		StringCchPrintf(buf,500,_T("Software\\Classes\\%s\\shell\\tweakpng\\command"),logicalname);
		rv=RegCreateKeyEx(HKEY_CURRENT_USER,buf,0,NULL,
			REG_OPTION_NON_VOLATILE,KEY_WRITE,NULL,&key1,&disp);
		if(rv==ERROR_SUCCESS) {
			GetModuleFileName(globals.hInst,buf,500);
			StringCchPrintf(cmd,500,_T("\"%s\" \"%%1\""), buf);
			RegSetValueEx(key1,_T(""),0,REG_SZ,(BYTE*)cmd,(DWORD)sizeof(TCHAR*)*(1+lstrlen(cmd)));
			RegCloseKey(key1);
		}
	}
}

static void remove_from_explorer_menu()
{
	TCHAR logicalname[500];
	TCHAR buf[500];
	int i;
	int x;


	for(i=0;i<=2;i++) {
		x=get_reg_name_for_ext(fmts[i],logicalname,500);
		if(x) {
			StringCchPrintf(buf,500,_T("Software\\Classes\\%s\\shell\\tweakpng\\command"),logicalname);
			RegDeleteKey(HKEY_CURRENT_USER,buf);
			StringCchPrintf(buf,500,_T("Software\\Classes\\%s\\shell\\tweakpng"),logicalname);
			RegDeleteKey(HKEY_CURRENT_USER,buf);
		}
	}
	return;
}

static const TCHAR *ai_menu_exts[] = { _T(".png"), _T(".jpg"), _T(".jpeg"), _T(".webp"),
	_T(".mp4"), _T(".mov"), _T(".m4v"), NULL };
#define TWPNG_AIMENU_KEY _T("Software\\Classes\\SystemFileAssociations\\%s\\shell\\TweakPNG.AIParams")

static int is_ai_menu_set()
{
	TCHAR buf[300];
	HKEY key;
	StringCchPrintf(buf, 300, TWPNG_AIMENU_KEY, _T(".png"));
	if(RegOpenKeyEx(HKEY_CURRENT_USER, buf, 0, KEY_READ, &key)!=ERROR_SUCCESS) return 0;
	RegCloseKey(key);
	return 1;
}

static void set_ai_menu(int add)
{
	TCHAR exe[MAX_PATH], buf[300], val[MAX_PATH+40];
	GetModuleFileName(NULL, exe, MAX_PATH);
	for(int i=0; ai_menu_exts[i]; i++) {
		StringCchPrintf(buf, 300, TWPNG_AIMENU_KEY, ai_menu_exts[i]);
		if(!add) { RegDeleteTree(HKEY_CURRENT_USER, buf); RegDeleteKey(HKEY_CURRENT_USER, buf); continue; }
		HKEY key;
		if(RegCreateKeyEx(HKEY_CURRENT_USER, buf, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &key, NULL)==ERROR_SUCCESS) {
			StringCchCopy(val, MAX_PATH+40, _T("View AI parameters"));
			RegSetValueEx(key, NULL, 0, REG_SZ, (BYTE*)val, (DWORD)sizeof(TCHAR)*(1+lstrlen(val)));
			StringCchPrintf(val, MAX_PATH+40, _T("\"%s\",0"), exe);
			RegSetValueEx(key, _T("Icon"), 0, REG_SZ, (BYTE*)val, (DWORD)sizeof(TCHAR)*(1+lstrlen(val)));
			RegCloseKey(key);
		}
		StringCchCat(buf, 300, _T("\\command"));
		if(RegCreateKeyEx(HKEY_CURRENT_USER, buf, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &key, NULL)==ERROR_SUCCESS) {
			StringCchPrintf(val, MAX_PATH+40, _T("\"%s\" /aiparams \"%%1\""), exe);
			RegSetValueEx(key, NULL, 0, REG_SZ, (BYTE*)val, (DWORD)sizeof(TCHAR)*(1+lstrlen(val)));
			RegCloseKey(key);
		}
	}
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

static const TCHAR *cmprlevel_names[] = { _T("Default compression"),
	_T("0 = No compression"),
	_T("1 = Fastest"),_T("2"),_T("3"),_T("4"),_T("5"),_T("6"),_T("7"),_T("8"),
	_T("9 = Best compression"),NULL };

static INT_PTR CALLBACK DlgProcPrefs(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	int i;
	WORD id;
	struct prefs_dlg_ctx *prctx = NULL;

	id=LOWORD(wParam);

	if(msg==WM_INITDIALOG) {
		prctx = (struct prefs_dlg_ctx*)lParam;
		if(!prctx) return 1;
		SetWindowLongPtr(hwnd,DWLP_USER,lParam);

		for(i=0;cmprlevel_names[i];i++) {
			SendDlgItemMessage(hwnd,IDC_CMPRLEVEL,CB_ADDSTRING,0,
			(LPARAM)cmprlevel_names[i]);
		}

		if(globals.compression_level == Z_DEFAULT_COMPRESSION) {
			i=0;
		}
		else {
			i=globals.compression_level+1;
		}
		SendDlgItemMessage(hwnd,IDC_CMPRLEVEL,CB_SETCURSEL,(WPARAM)i,0);

		{
			int x;
			x=is_explorer_menu_set();
			CheckDlgButton(hwnd,IDC_ADDTOMENU,x?BST_CHECKED:BST_UNCHECKED);
			prctx->remember_menusetting=x;
		}

		CheckDlgButton(hwnd,IDC_OPENPARAMS,
			globals.open_params_on_load?BST_CHECKED:BST_UNCHECKED);
		prctx->remember_aimenu = is_ai_menu_set();
		CheckDlgButton(hwnd,IDC_ADDAIMENU,prctx->remember_aimenu?BST_CHECKED:BST_UNCHECKED);

		twpng_InitDarkDialog(hwnd);
		return 1;
	}
	else {
		prctx = (struct prefs_dlg_ctx*)GetWindowLongPtr(hwnd,DWLP_USER);
		if(!prctx) return 0;
	}

	switch (msg) {
	case WM_COMMAND:
		switch(id) {
		case IDOK:
			{
				int x;
				x= (IsDlgButtonChecked(hwnd,IDC_ADDTOMENU) == BST_CHECKED);

				if(x!=prctx->remember_menusetting) {
					if(x) {
						add_to_explorer_menu();
					}
					else {
						remove_from_explorer_menu();
					}
				}
			}

			globals.open_params_on_load= (IsDlgButtonChecked(hwnd,IDC_OPENPARAMS)==BST_CHECKED);
			{
				int x = (IsDlgButtonChecked(hwnd,IDC_ADDAIMENU)==BST_CHECKED);
				if(x!=prctx->remember_aimenu) set_ai_menu(x);
			}

			i=(int)SendDlgItemMessage(hwnd,IDC_CMPRLEVEL,CB_GETCURSEL,0,0);
			if(i==CB_ERR) i=0;
			if(i==0) globals.compression_level= Z_DEFAULT_COMPRESSION;
			else globals.compression_level= i-1;
			SaveSettings();
			EndDialog(hwnd, 0);
			return 1;

		case IDCANCEL:
			EndDialog(hwnd, 0);
			return 1;
		}
	}
	{
		INT_PTR darkResult = 0;
		if(twpng_HandleDlgDarkMsg(msg, wParam, lParam, &darkResult)) return darkResult;
	}
	return 0;
}

static INT_PTR CALLBACK DlgProcSplitIDAT(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	Chunk *ch=NULL;
	WORD id;
	TCHAR buf[500];

	id=LOWORD(wParam);

	if(msg==WM_INITDIALOG) {
		ch = (Chunk*)lParam;
		if(!ch) return 1;
		SetWindowLongPtr(hwnd,DWLP_USER,lParam);
		StringCchPrintf(buf,500,_T("Size in bytes of first section (0") SYM_ENDASH _T("%d)"),ch->length);
		SetDlgItemText(hwnd,IDC_SPLIT_TEXT,buf);
		twpng_InitDarkDialog(hwnd);
		return 1;
	}
	else {
		ch = (Chunk*)GetWindowLongPtr(hwnd,DWLP_USER);
	}

	switch (msg) {
	case WM_DESTROY:
		ch=NULL;
		return 1;

	case WM_COMMAND:
		if(!ch) return 1;
		switch(id) {
		case IDOK:
			{
				int n,rep;

				n=GetDlgItemInt(hwnd,IDC_SPLIT_SIZE,NULL,TRUE);
				if(n<0 || n>(int)ch->length) {
					EndDialog(hwnd, 0);
					return 1;
				}

				rep= (IsDlgButtonChecked(hwnd,IDC_SPLIT_REPEAT)==BST_CHECKED)?1:0;

				if(n==0 && rep) {
					mesg(MSG_E,_T("Can") SYM_RSQUO _T("t divide repeatedly into chunks of zero size."));
					return 1;
				}

				if(ch->m_parentpng->split_idat(ch->m_index,n,rep)) {
					EndDialog(hwnd, 0);
				}
				return 1;
			}
		case IDCANCEL:
			EndDialog(hwnd, 0);
			return 1;
		}
	}
	{
		INT_PTR darkResult = 0;
		if(twpng_HandleDlgDarkMsg(msg, wParam, lParam, &darkResult)) return darkResult;
	}
	return 0;
}

static INT_PTR CALLBACK DlgProcSetSig(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	Png *png1=NULL;
	WORD id;
	UINT x;

	id=LOWORD(wParam);

	if(msg==WM_INITDIALOG) {
		png1 = (Png*)lParam;
		if(!png1) return 1;
		SetWindowLongPtr(hwnd,DWLP_USER,lParam);

		switch(png1->m_imgtype) {
		case IMG_PNG: x=IDC_RADIO1; break;
		case IMG_MNG: x=IDC_RADIO2; break;
		case IMG_JNG: x=IDC_RADIO3; break;
		default: x=0;
		}
		if(x) CheckDlgButton(hwnd,x,BST_CHECKED);
		twpng_InitDarkDialog(hwnd);
		return 1;

	}
	else {
		png1 = (Png*)GetWindowLongPtr(hwnd,DWLP_USER);
		if(!png1) return 0;
	}

	switch (msg) {
	case WM_DESTROY:
		png1=NULL;
		return 1;

	case WM_COMMAND:
		if(!png1) return 1;
		switch(id) {
		case IDOK:
			{
				int oldtype=png1->m_imgtype;
				if(IsDlgButtonChecked(hwnd,IDC_RADIO1)==BST_CHECKED) png1->m_imgtype=IMG_PNG;
				if(IsDlgButtonChecked(hwnd,IDC_RADIO2)==BST_CHECKED) png1->m_imgtype=IMG_MNG;
				if(IsDlgButtonChecked(hwnd,IDC_RADIO3)==BST_CHECKED) png1->m_imgtype=IMG_JNG;
				if(png1->m_imgtype!=oldtype) {
					png1->set_signature();
					png1->modified();
				}
				EndDialog(hwnd, 0);
				return 1;
			}
		case IDCANCEL:
			EndDialog(hwnd, 0);
			return 1;
		}
	}
	{
		INT_PTR darkResult = 0;
		if(twpng_HandleDlgDarkMsg(msg, wParam, lParam, &darkResult)) return darkResult;
	}
	return 0;
}

static void twpng_BrowseForTool(HWND hwnd, int dlgitem)
{
	OPENFILENAME ofn;
	TCHAR fn[MAX_PATH];

	StringCchCopy(fn,MAX_PATH,_T(""));
	ZeroMemory(&ofn,sizeof(OPENFILENAME));
	ofn.lStructSize=sizeof(OPENFILENAME);
	ofn.hwndOwner=hwnd;
	ofn.hInstance=NULL;
	ofn.lpstrFilter=_T("Executable Files\0*.exe;*.bat\0All files (*.*)\0*.*\0\0");
	ofn.nFilterIndex=1;
	ofn.lpstrTitle=_T("Browse for tool");
	//ofn.lpstrInitialDir=NULL;
	ofn.lpstrFile=fn;
	ofn.nMaxFile=MAX_PATH;
	ofn.Flags=OFN_FILEMUSTEXIST|OFN_HIDEREADONLY|OFN_NOCHANGEDIR;

	if(!GetOpenFileName(&ofn)) {
		return;  // user canceled
	}

	SetDlgItemText(hwnd,dlgitem,fn);
}

static INT_PTR CALLBACK DlgProcTools(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	WORD id;
	int i;

	static const UINT toolsn[] = {IDC_EDIT1,IDC_EDIT2,IDC_EDIT3,IDC_EDIT4,
		IDC_EDIT5,IDC_EDIT6};
	static const UINT toolsc[] = {IDC_EDITC1,IDC_EDITC2,IDC_EDITC3,IDC_EDITC4,
		IDC_EDITC5,IDC_EDITC6};
	static const UINT toolsp[] = {IDC_EDITP1,IDC_EDITP2,IDC_EDITP3,IDC_EDITP4,
		IDC_EDITP5,IDC_EDITP6};


	id=LOWORD(wParam);

	switch (msg) {
	case WM_INITDIALOG:
		SetDlgItemText(hwnd,IDC_STATIC1,_T("Configure the tools to list on the Tools menu. ")
		  _T("The Name field is any name you choose. ")
		  _T("In the Parameters field, for a viewer, ") SYM_LDQUO _T("%1") SYM_RDQUO _T(" will be replaced by the PNG filename. ")
		  _T("For a filter, ") SYM_LDQUO _T("%1") SYM_RDQUO _T(" is the input file and ") SYM_LDQUO _T("%2") SYM_RDQUO _T(" is the output file."));
		for(i=0;i<TWPNG_NUMTOOLS;i++) {
			SetDlgItemText(hwnd,toolsn[i],globals.tools[i].name);
			SetDlgItemText(hwnd,toolsc[i],globals.tools[i].cmd);
			SetDlgItemText(hwnd,toolsp[i],globals.tools[i].params);
			SendDlgItemMessage(hwnd,toolsn[i],EM_LIMITTEXT,MAX_TOOL_NAME  -1,0);
			SendDlgItemMessage(hwnd,toolsc[i],EM_LIMITTEXT,MAX_TOOL_CMD   -1,0);
			SendDlgItemMessage(hwnd,toolsp[i],EM_LIMITTEXT,MAX_TOOL_PARAMS-1,0);
		}
		twpng_InitDarkDialog(hwnd);
		return 1;

//	case WM_DESTROY:
//		return 1;

	case WM_COMMAND:
		switch(id) {
		case IDC_TOOL1BROWSE: twpng_BrowseForTool(hwnd,IDC_EDITC1); return 1;
		case IDC_TOOL2BROWSE: twpng_BrowseForTool(hwnd,IDC_EDITC2); return 1;
		case IDC_TOOL3BROWSE: twpng_BrowseForTool(hwnd,IDC_EDITC3); return 1;
		case IDC_TOOL4BROWSE: twpng_BrowseForTool(hwnd,IDC_EDITC4); return 1;
		case IDC_TOOL5BROWSE: twpng_BrowseForTool(hwnd,IDC_EDITC5); return 1;
		case IDC_TOOL6BROWSE: twpng_BrowseForTool(hwnd,IDC_EDITC6); return 1;

		case IDOK:
			for(i=0;i<TWPNG_NUMTOOLS;i++) {
				GetDlgItemText(hwnd,toolsn[i],globals.tools[i].name,MAX_TOOL_NAME);
				globals.tools[i].name[MAX_TOOL_NAME-1]='\0';
				GetDlgItemText(hwnd,toolsc[i],globals.tools[i].cmd,MAX_TOOL_CMD);
				globals.tools[i].cmd[MAX_TOOL_CMD-1]='\0';
				GetDlgItemText(hwnd,toolsp[i],globals.tools[i].params,MAX_TOOL_PARAMS);
				globals.tools[i].params[MAX_TOOL_PARAMS-1]='\0';
				SaveSettings();
				EndDialog(hwnd, 0);
			}
			return 1;

		case IDCANCEL:
			EndDialog(hwnd, 0);
			return 1;
		}
	}
	{
		INT_PTR darkResult = 0;
		if(twpng_HandleDlgDarkMsg(msg, wParam, lParam, &darkResult)) return darkResult;
	}
	return 0;
}
