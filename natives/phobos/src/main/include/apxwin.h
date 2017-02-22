/*
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
 
#ifndef _APXWIN_H_INCLUDED_
#define _APXWIN_H_INCLUDED_

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <zmouse.h>
#include <richedit.h>
#include <lm.h>
#include <stdint.h>

#define APXMACRO_BEGIN                  do {
#define APXMACRO_END                    } while(0)

#ifdef  __cplusplus
#define __APXBEGIN_DECLS    extern "C" {
#define __APXEND_DECLS  }
#else
#define __APXBEGIN_DECLS
#define __APXEND_DECLS
#endif

#define SET_BIT_FLAG(x, b) ((x) |= (1 << b))
#define CLR_BIT_FLAG(x, b) ((x) &= ~(1 << b))
#define TST_BIT_FLAG(x, b) ((x) & (1 << b))

#define IS_INVALID_HANDLE(h) ((((void *)h) == (void *)NULL || ((void *)h) == INVALID_HANDLE_VALUE))
#define IS_VALID_STRING(s)   ((s) != NULL && *(s) != 0)
#define IS_EMPTY_STRING(s)   ((s) == NULL || *(s) == 0)

#define DYNOLAD_TYPE_DECLARE(fnName, callconv, retType)             \
    typedef retType (callconv *PFN_##fnName)                        \

#define DYNLOAD_FPTR_DECLARE(fnName)                                \
    PFN_##fnName FP_##fnName

#define DYNLOAD_FPTR(fnName)  FP_##fnName

#define DYNLOAD_FPTR_ADDRESS(fnName, dllName)                       \
    FP_##fnName = (PFN_##fnName)GetProcAddress(                     \
                                GetModuleHandle(TEXT(#dllName)),    \
                                #fnName)    

#define DYNLOAD_FPTR_LOAD(fnName, dllHandle)                        \
    FP_##fnName = (PFN_##fnName)GetProcAddress(                     \
                                dllHandle,                          \
                                #fnName)    

#define DYNLOAD_CALL(fnName)    (*FP_##fnName)

#ifndef ABS
#define ABS(x)       (((x) > 0) ? (x) : (x) * (-1))
#endif

#define SIZ_RESLEN         256
#define SIZ_RESMAX         (SIZ_RESLEN -1)
#define SIZ_BUFLEN         512
#define SIZ_BUFMAX         (SIZ_BUFLEN -1)
#define SIZ_DESLEN         1024
#define SIZ_DESMAX         (SIZ_DESLEN -1)
#define SIZ_HUGLEN         8192
#define SIZ_HUGMAX         (SIZ_HUGLEN -1)
#define SIZ_PATHLEN        4096
#define SIZ_PATHMAX        4095

#include "handles.h"
#include "log.h"
#include "cmdline.h"
#include "console.h"
#include "rprocess.h"
#include "registry.h"
#include "service.h"
#include "javajni.h"

__APXBEGIN_DECLS

typedef enum {
        APX_WINVER_UNK =       0,
        APX_WINVER_UNSUP =     1,
        APX_WINVER_95 =       10,
        APX_WINVER_95_B =     11,
        APX_WINVER_95_OSR2 =  12,
        APX_WINVER_98 =       14,
        APX_WINVER_98_SE =    16,
        APX_WINVER_ME =       18,

        APX_WINVER_UNICODE =  20, /* Prior versions support only narrow chars */

        APX_WINVER_CE_3 =     23, /* CE is an odd beast, not supporting */
                               /* some pre-NT features, such as the    */
        APX_WINVER_NT =       30, /* narrow charset APIs (fooA fns), while  */
        APX_WINVER_NT_3_5 =   35, /* not supporting some NT-family features.  */
        APX_WINVER_NT_3_51 =  36,

        APX_WINVER_NT_4 =     40,
        APX_WINVER_NT_4_SP2 = 42,
        APX_WINVER_NT_4_SP3 = 43,
        APX_WINVER_NT_4_SP4 = 44,
        APX_WINVER_NT_4_SP5 = 45,
        APX_WINVER_NT_4_SP6 = 46,

        APX_WINVER_2000 =     50,
        APX_WINVER_2000_SP1 = 51,
        APX_WINVER_2000_SP2 = 52,
        APX_WINVER_XP =       60
} APX_OSLEVEL;

APX_OSLEVEL apxGetOsLevel();
LPWSTR      AsciiToWide(LPCSTR s, LPWSTR ws);
LPSTR       WideToAscii(LPCWSTR ws, LPSTR s);
LPSTR       WideToANSI(LPCWSTR ws);
LPSTR       MzWideToANSI(LPCWSTR ws);

typedef struct APXMULTISZ APXMULTISZ;
typedef APXMULTISZ*       LPAPXMULTISZ;

DWORD   apxMultiSzToArrayA(APXHANDLE hPool, LPCSTR lpString, LPSTR **lppArray);
DWORD   apxMultiSzToArrayW(APXHANDLE hPool, LPCWSTR lpString, LPWSTR **lppArray);
LPWSTR  apxMultiSzCombine(APXHANDLE hPool, LPCWSTR lpStrA, LPCWSTR lpStrB,
                          LPDWORD lpdwLength);

LPAPXMULTISZ    apxMultiSzStrdup(LPCTSTR szSrc);
LPTSTR          apxMultiSzStrcat(LPAPXMULTISZ lpmSz, LPCTSTR szSrc);
LPCTSTR         apxMultiSzGet(LPAPXMULTISZ lpmSz);
/** Remove the specified character from the string
 * Replacement is done in-place.
 */
LPTSTR          apxStrCharRemove(LPTSTR szString, TCHAR chSkip);
DWORD           apxStrUnQuoteInplaceA(LPSTR szString);
DWORD           apxStrUnQuoteInplaceW(LPWSTR szString);

BOOL            apxUltohex(ULONG n, LPTSTR lpBuff, DWORD dwBuffLength);
BOOL            apxUptohex(ULONG_PTR n, LPTSTR lpBuff, DWORD dwBuffLength);
ULONG           apxStrToul(LPCTSTR szNum);
ULONG           apxStrToulW(LPCWSTR szNum);
ULONG           apxAtoulW(LPCWSTR szNum);

BOOL            apxMakeResourceName(LPCTSTR szPrefix, LPTSTR lpBuff,
                                    DWORD dwBuffLength);

INT             apxStrMatchA(LPCSTR szString, LPCSTR szPattern, BOOL bIgnoreCase);
INT             apxStrMatchW(LPCWSTR szString, LPCWSTR szPattern, BOOL bIgnoreCase);
INT             apxMultiStrMatchW(LPCWSTR szString, LPCWSTR szPattern,
                                  WCHAR chSeparator, BOOL bIgnoreCase);
LPSTR           apxArrayToMultiSzA(APXHANDLE hPool, DWORD nArgs, LPCSTR *lpArgs);
void            apxStrQuoteInplaceW(LPWSTR szString);
LPWSTR          apxMszToCRLFW(APXHANDLE hPool, LPCWSTR szStr);
LPWSTR          apxCRLFToMszW(APXHANDLE hPool, LPCWSTR szStr, LPDWORD lpdwBytes);
LPSTR           apxExpandStrA(APXHANDLE hPool, LPCSTR szString);
LPWSTR          apxExpandStrW(APXHANDLE hPool, LPCWSTR szString);
void            apxStrCharReplaceA(LPSTR szString, CHAR chReplace, CHAR chReplaceWith);
void            apxStrCharReplaceW(LPWSTR szString, WCHAR chReplace, WCHAR chReplaceWith);
BOOL            apxAddToPathW(APXHANDLE hPool, LPCWSTR szAdd);

/*
 * Find the first occurrence of lpFind in lpMem.
 * dwLen:   The length of lpFind
 * dwSize:  The length of lpMem
 */
LPBYTE  ApcMemSearch(LPCVOID lpMem, LPCVOID lpFind, SIZE_T dwLen, SIZE_T dwSize);
LPSTR   AplRindexA(LPCSTR lpStr, int ch);

#define AplMoveMemory   memcpy

PSECURITY_ATTRIBUTES GetNullACL();
void CleanNullACL(void *sa);

__APXEND_DECLS  

#endif /* _APXWIN_H_INCLUDED_ */

